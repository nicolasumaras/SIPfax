# SIPfax SpanDSP Soft-Modem Worker

This worker is the LKMA-195 soft-modem scaffold for SIPfax. It is a standalone
process that keeps the existing SIPfax external modem process contract:

- stdin receives one G.711 RTP payload prefixed by a two-byte big-endian length.
- stdout writes outbound G.711 payloads with the same two-byte length prefix.
- fd 3 writes JSON-line control events. Override with `SIPFAX_MODEM_CONTROL_FD`.
- `SIPFAX_MODEM_CODEC`, `SIPFAX_MODEM_PAYLOAD_TYPE`, and
  `SIPFAX_MODEM_CLOCK_RATE` select the negotiated G.711 boundary format.
- decoded V.21 bytes are written to `SIPFAX_MODEM_DATA_OUT` when set, or to
  `./sipfax-softmodem-data.bin` by default.

Only V.21 is advertised and enabled in this milestone. HDLC framing, pty
bridging, PPP wiring, and faster modulation families are intentionally left for
later LKMA-193 follow-ups.

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
worker enters V.21 data mode and emits at least one decoded byte:

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
