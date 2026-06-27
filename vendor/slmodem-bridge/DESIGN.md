# sipfax-slmodem-bridge — design & integration contract

Status: **design** (Phase 0/1 of the higher-speed modem plan). Build/run target is the Debian
test VM with 32-bit multilib — **not** the macOS dev box. See
`/Users/.../.claude/plans/i-want-to-implement-wobbly-simon.md` for the full plan.

## Why this exists

SIPfax's data path today uses a spandsp worker capped at V.22bis (2400). To reach V.32bis
(14.4k) and V.34 (33.6k) at no cost, we drive **SmartLink `slmodemd` + `dsplibs.o`** — the same
free datapump that **D-Modem** (`AonCyberLabs/D-Modem`, GPL-2.0) uses for "modem over SIP". This
bridge is a SIPfax-native replacement for D-Modem's `d-modem.c`: instead of running its own
PJSIP stack, it speaks SIPfax's existing worker protocol on stdin/stdout/fd-3, so the Node side
barely changes.

## The two contracts the bridge sits between

### A. SIPfax worker contract (toward Node) — must match `sipfax-softmodem` exactly
Source of truth: `src/media.js` (`ModemBackend`) and `vendor/sipfax-softmodem/sipfax-softmodem.c`.
- **stdin**: `[uint16 BE length][G.711 payload]`, 160 samples / 20 ms @ 8 kHz (µ-law or A-law).
- **stdout**: same framing — generated modem audio back to RTP.
- **fd 3**: newline-delimited JSON control events. Must emit `pty-opened` with `{slavePath}`
  when the data channel is ready — `src/server.js:155` routes it to `openPty` →
  `PppdSupervisor.start({slavePath})`, which attaches `pppd` to that path.
- **env in**: `SIPFAX_MODEM_CODEC` (PCMU/PCMA), `SIPFAX_MODEM_PAYLOAD_TYPE`,
  `SIPFAX_MODEM_CLOCK_RATE` (always 8000).

### B. slmodemd `-e` contract (toward the datapump) — reverse-engineered from D-Modem
Source of truth: `D-Modem/slmodemd/modem_main.c` (`socket_start`, main loop) + `modem.h`.
- slmodemd is launched as `slmodemd -e <bridge>`. On call setup it does:
  `socketpair(AF_UNIX, SOCK_STREAM)`, `fork()`, then in the child
  `execl(modem_exec, modem_exec, dial_string, "<fd>", NULL)`.
  → **The bridge is the `-e` program. It receives `argv[1]=dial_string`, `argv[2]=<socket fd>`
  (already open, inherited).** All audio flows over that one AF_UNIX stream socket.
- **Audio format on the socket: signed 16-bit little-endian, mono, `MODEM_RATE = 9600 Hz`**
  (`modem.h:85`). NOT 8 kHz. D-Modem hid this by declaring its pjmedia port at 9600 and letting
  PJSIP resample. We must resample ourselves (see below).
- **Framing/pacing**: slmodemd `select()`s on the socket, `device_read`s available int16
  samples, `modem_process(in,out,count)` (symmetric: RX `count` in → TX `count` out), then
  `device_write`s the same `count` back. `mdm_device_read/write` move `size*2` bytes = `size`
  samples. **The socket data rate IS the modem's sample clock** — feed it in real time.
  D-Modem uses 192-sample (384-byte) frames = 20 ms @ 9600; it primes the link by writing one
  384-byte silence frame at startup.
- **Control/data**: slmodemd creates a pty and symlinks `/dev/ttySLn` → `/dev/pts/N`. AT
  commands and the post-CONNECT data stream both ride that tty. `pppd` attaches there.

## Bridge data flow

```
            stdin (G.711 8k, 160/frame)            AF_UNIX socket (S16LE 9.6k, 192/frame)
 Node ───────────────────────────────► bridge ──────────────────────────────────► slmodemd
        decode G.711→S16  +  resample 8000→9600 (×6/5)                              (datapump RX)

 Node ◄─────────────────────────────── bridge ◄────────────────────────────────── slmodemd
        encode S16→G.711  +  resample 9600→8000 (×5/6)                              (datapump TX)

 fd 3:  bridge emits  pty-opened {slavePath:/dev/ttySLn}  after CONNECT  ─────────► pppd
```

### Key implementation points
1. **Resampler (the one real new DSP bit).** 8000↔9600 is ratio 6:5. Implement a small
   FIR-interpolating polyphase resampler (up 6 / down 5) with an anti-alias low-pass at ~3.6 kHz
   each direction. Reuse the G.711 tables/logic already in `vendor/sipfax-softmodem/`
   (`init_g711_tables`, `decode_g711`, `encode_g711`). A naive linear resampler is enough to
   prove the pipeline at V.22bis/V.32bis but will hurt V.34 SNR — budget a proper FIR for Phase 3.
2. **Pacing.** One inbound 20 ms G.711 frame (160 @ 8k) → 192 @ 9.6k → one socket write; read
   192 back per frame for stdout. This keeps slmodemd's clock fed without a separate timer.
   Prime with one 384-byte silence frame (like D-Modem) so the loop starts cleanly.
3. **Answer mode (SIPfax answers; D-Modem only dials).** After slmodemd creates `/dev/ttySLn`,
   the bridge opens it, sends `ATX3` (no dial tone), `AT+MS=<modulation>` (force/cap rate per
   `SIPFAX_MODEM_MODULATION`), then `ATA` (answer). On `CONNECT`, emit `pty-opened` and stop
   touching the tty so `pppd` owns it. (Alternative: a `pppd connect` chat script — but doing it
   in the bridge keeps the existing Node flow unchanged.)
4. **Lifecycle.** Bridge spawns/owns slmodemd (or is spawned by it via `-e`; decide in Phase 1 —
   simplest is: Node spawns the bridge, the bridge spawns `slmodemd -e /proc/self/exe`-style, or
   Node spawns `slmodemd` directly and the bridge is the `-e` child. Leaning toward: **Node spawns
   a thin launcher that runs `slmodemd -e <bridge>`**, and the bridge handles audio+AT+fd3.)
   On hangup/socket reset slmodemd calls `modem_hangup`; bridge should exit cleanly so Node's
   `backend-exit` fires.

## AT+MS modulation selector — CONFIRMED on the Debian VM (Phase 0)

`AT+MS=<dp_id>,<automode 0|1>,<min_rate>,<max_rate>` (rates 300–56000). `dp_id` values are the
`enum DP_ID` codes (`modem_defs.h`). Live `AT+MS=?` from this exact `dsplibs.o` (slmodem 2.9.11)
returned the supported set:

```
(21,22,23,122,32,132,34,103,212,90,92),(0,1),(300-56000),(300-56000)
```

| Modulation | dp_id | Example |
| --- | --- | --- |
| V.22bis | **122** | `AT+MS=122,0,1200,2400` |
| V.32 | 32 | `AT+MS=32,0,4800,9600` |
| V.32bis | **132** | `AT+MS=132,0,4800,14400` |
| V.34 | **34** | `AT+MS=34,0,2400,33600` (accepted `OK`) |
| V.90 / V.92 | 90 / 92 | *client side only — not usable as the SIPfax answerer; see plan Track B* |

Map `SIPFAX_MODEM_MODULATION` → dp_id. The datapump `create` takes a `caller` flag
(`modem.c`: `m->caller,m->srate`), so **answer-side (caller=0) V.34 is supported** — this is the
Track A ceiling, now confirmed present in the blob (symbols `datapumpv34`, `dp_v32`, `dp_vpcm`).

## What stays unchanged on the Node side
- `src/media.js` `ModemBackend` — no change; it just spawns `command` and speaks the same
  stdio/fd-3/framing.
- `src/pppd-supervisor.js` — no change; attaches `pppd` to the `pty-opened` slavePath.
- Engine selection is a one-line `command`/`args` switch in `src/index.js` keyed on
  `SIPFAX_MODEM_ENGINE` (default `spandsp`, so nothing regresses).

## Phase 0 results (Debian VM 192.168.1.31, 2026-06-26) — CONFIRMED
- ✅ 32-bit slmodemd builds clean (`gcc-multilib`); `dsplibs.o` sha256 verified.
- ✅ Datapumps V.22bis/V.32/V.32bis/V.34 present and selectable; V.90/V.92 present (client side).
- ✅ Headless AT control over `/dev/ttySL0` works (`ATE0`, `AT+MS=?`, `AT+MS=34,...` → `OK`).
- ✅ Answer mode: `ATA` → `modem_answer()` (`modem_at.c:945`); auto-answer via S0
  (`SREG_RINGS_TO_AUTO_ANSWER`, `modem.c:752`).

## Phase 1 — IMPLEMENTED & VALIDATED (Debian VM, 2026-06-26)

**Two V.22bis modems trained, connected at 2400 bps, and exchanged data bidirectionally through
the bridge** (loopback: one answer `ATA`, one originate `ATD`, cross-connected G.711). Result:
`both connected: True`, `CONNECT 2400`, `DATA-PASS: True` (PING/PONG delivered each way).

Resolved decisions from bring-up:
- **Process topology = patched slmodemd direct-connect (option C).** The fork-based `-e` audio
  transport is unreliable on *answer* (the child exec races / EFAULTs; D-Modem only ever dials).
  We patch `slmodemd`'s `socket_start` to **connect to the bridge's AF_UNIX socket** (path via
  `SIPFAX_AUDIO_SOCK`) instead of fork+exec. No shim. See
  `vendor/slmodem/0001-sipfax-socket-transport-and-ttylink.patch` (applied by `fetch.sh`). The
  bridge's shim-mode code remains as dead fallback only.
- **slmodemd is launched `-n` (regular priority).** SCHED_FIFO 99 destabilized the VM; our pacing
  is RTP-frame-driven so RT priority is unnecessary.
- **The bridge drives AT** (`ATE0`,`ATX3`,`AT+MS=<mod>`, then `ATA` answer / `ATD` originate),
  watches the tty for `CONNECT`, emits `pty-opened`, then **closes its tty fd so pppd owns it**.
- **`SIPFAX_TTY_LINK`** env (slmodemd patch) overrides the `/dev/ttySLn` link name so multiple
  instances coexist (loopback test needs two).
- **Linear resampling (8000↔9600, 6:5) is sufficient for V.22bis.** Re-evaluate / add FIR for V.34.
- The `dev read: Operation not permitted` seen at shutdown is a **teardown artifact** (socket
  closed under slmodemd), not a runtime fault — the link ran cleanly for 28 s in tests.

### Bridge env interface (final)
`SIPFAX_SLMODEMD` (path), `SIPFAX_MODEM_CODEC` (PCMU/PCMA), `SIPFAX_MODEM_MODULATION`
(v22bis|v32|v32bis|v34), `SIPFAX_MODEM_TTY` (default /dev/ttySL0), `SIPFAX_MODEM_DIAL` (originate;
unset = answer), `SIPFAX_SLMODEM_DEV` / `SIPFAX_TTY_LINK` (multi-instance).

## Still open for later phases
- Full SIPfax pipeline test (Node engine switch + real inbound call → pppd) — Phase 1 finish.
- V.32bis / V.34 training over a real (lossy) RTP path; FIR resampler for V.34.
- systemd hardening drop-in (PrivateDevices/CAP_SYS_NICE) for the engine under the service unit.
