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
  master are HDLC-framed, byte-stuffed, and modulated back onto outbound V.21.
- HDLC-decoded payload bytes are also mirrored to `SIPFAX_MODEM_DATA_OUT` when
  set, or to `./sipfax-softmodem-data.bin` by default, for diagnostics.

Only V.21 is advertised and enabled in this milestone. PPP supervision, IPCP,
egress wiring, and faster modulation families are intentionally left for later
LKMA-193 follow-ups.

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
worker enters V.21 data mode:

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

## HDLC Pty Bridge Bench

The pty bridge harness forces data mode, waits for `pty-opened`, writes a PPP
fixture to the reported slave path, feeds idle G.711 frames, and asserts that the
worker emits non-idle outbound modem audio:

```bash
cd vendor/sipfax-softmodem
make bench-hdlc-pty
```

The hardware acceptance run should attach a Linux client modem over USB FXO,
dial the SIPfax instance backed by this worker, open the `ptySlavePath` reported
on fd 3, and compare HDLC traffic in both directions with a fixture stream.
