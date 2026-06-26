# SIPfax SpanDSP Soft-Modem Worker

This worker is the LKMA-195 soft-modem scaffold for SIPfax. It is a standalone
process that keeps the existing SIPfax external modem process contract:

- stdin receives one G.711 RTP payload prefixed by a two-byte big-endian length.
- stdout writes outbound G.711 payloads with the same two-byte length prefix.
- fd 3 writes JSON-line control events. Override with `SIPFAX_MODEM_CONTROL_FD`.
- `SIPFAX_MODEM_CODEC`, `SIPFAX_MODEM_PAYLOAD_TYPE`, and
  `SIPFAX_MODEM_CLOCK_RATE` select the negotiated G.711 boundary format.
- data mode opens a pty pair and emits `pty-opened` with `ptySlavePath`.
  HDLC-decoded PPP bytes are written to the pty master. Bytes read from the pty
  master are HDLC-framed, byte-stuffed, and modulated back onto the selected
  outbound carrier.
- HDLC-decoded payload bytes are also mirrored to `SIPFAX_MODEM_DATA_OUT` when
  set, or to `./sipfax-softmodem-data.bin` by default, for diagnostics.

The worker advertises V.21 and V.22/V.22bis in the V.8 answer menu. When the
caller offers V.22/V.22bis, SIPfax selects V.22bis at 2400 bit/s; otherwise it
falls back to V.21 at 300 bit/s for non-V.8 or failed V.8 calls.

Set `SIPFAX_MODEM_START_MODE=v22bis` only for a controlled live diagnostic run
after normal V.8 negotiation reports `v8Status: "failed"` and
`lastEvent: "v8-failed-v21-fallback"`. This bypasses V.8 and starts the worker
directly in V.22bis answer mode so the lab can distinguish V.8 negotiation
failure from a lower RTP/analog carrier or PPP issue. Restore the default
`SIPFAX_MODEM_START_MODE=v8` after the fallback run.

## Debian Bookworm Build

Install the SIPfax runtime plus the native build dependencies:

```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libspandsp-dev python3
```

Build:

```bash
cd vendor/sipfax-softmodem
make
```

Install for SIPfax:

```bash
sudo make install
```

Configure SIPfax:

```bash
SIPFAX_MODEM_COMMAND=/usr/local/bin/sipfax-softmodem
```

The binary dynamically links to `libspandsp.so.2` from Debian's
`libspandsp2` package. SpanDSP is LGPL-2.1; the LGPL obligation applies to this
separate worker binary and its dynamic link to `libspandsp2`, not to the Node.js
SIPfax service process.

## V.21 Bench Replay

The replay harness consumes the LKMA-193a ground-truth WAV and asserts that the
worker enters V.21 data mode. PPP bytes now flow through the HDLC pty bridge, so
the replay check does not require raw diagnostic bytes in `SIPFAX_MODEM_DATA_OUT`.

```bash
cd vendor/sipfax-softmodem
python3 bench-v21.py \
  --worker ./sipfax-softmodem \
  --wav ../../artifacts/lkma-193a/groundtruth-v21.wav
```

The harness also accepts the Phase 1 directory layout:

```bash
python3 bench-v21.py \
  --worker ./sipfax-softmodem \
  --wav ../../artifacts/lkma-193a/ground-truth/v21/inbound.wav
```

Use `--allow-missing` for developer smoke checks on machines where the lab
capture has not been copied into the repository yet. Acceptance runs must not
use `--allow-missing`.

## V.22bis Bench Replay

The V.22bis replay harness consumes the Phase 1 V.22bis artifact and asserts
that the worker enters data mode with `modulation` reported as `V.22bis`:

```bash
cd vendor/sipfax-softmodem
python3 bench-v22bis.py \
  --worker ./sipfax-softmodem \
  --wav ../../artifacts/lkma-193a/ground-truth/v22bis/inbound.wav
```

The checked-in Phase 1 V.22bis artifact is synthetic. This bench therefore
forces the selected modulation while still replaying the artifact through the
same framed G.711 worker boundary. Replace the artifact with the real lab
recording before using the bench as a negotiated-signal quality check.

## HDLC Pty Bridge Bench

The pty bridge harness forces data mode, waits for `pty-opened`, writes a PPP
fixture to the reported slave path, feeds idle G.711 frames, and asserts that the
worker emits non-idle outbound modem audio:

```bash
cd vendor/sipfax-softmodem
make bench-hdlc-pty
```

The hardware acceptance run should attach the real Windows dial-up computer
through the ATA/FreePBX path, dial the SIPfax instance backed by this worker,
and verify that `pppd` starts on the `ptySlavePath` reported on fd 3.
