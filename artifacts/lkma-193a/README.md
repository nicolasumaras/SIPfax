# LKMA-193a Audio Capture Harness

This artifact captures the production ATA plus FreePBX RTP path without changing
SIPfax, FreePBX, or ATA configuration.

Run it on the SIPfax VM (`192.168.1.31`) during a live Windows dial-up attempt:

```bash
cd /opt/sipfax
LOCAL_IP=192.168.1.31 PEER_IP=192.168.1.29 DURATION_SECONDS=30 \
  artifacts/lkma-193a/capture.sh
```

Outputs are written under `artifacts/lkma-193a/captures/<timestamp>/`:

- `rtp.pcap` - raw packet capture for audit/debug replay
- `inbound.wav` - 8 kHz signed 16-bit linear PCM decoded from RTP toward SIPfax
- `outbound.wav` - 8 kHz signed 16-bit linear PCM decoded from SIPfax toward
  the FreePBX/ATA path
- `report.json` - packet count, expected packet count, gaps, loss fraction,
  out-of-order count, mean IPDV, and max IPDV for each direction
- `outbound-spectrogram.png` - SIPfax-to-ATA-path spectrogram when `ffmpeg` is
  installed

The parser accepts classic pcap files and decodes RTP payload types `0` (`PCMU`)
and `8` (`PCMA`) only. It uses Python's standard library so it can run on the VM
without adding a SIPfax runtime dependency.

## Environment

| Variable | Default | Purpose |
| --- | --- | --- |
| `LOCAL_IP` | first `hostname -I` address | SIPfax VM address used to classify direction |
| `PEER_IP` | unset | Optional FreePBX/ATA-side RTP peer filter |
| `DURATION_SECONDS` | `30` | Capture window |
| `RTP_PORT_RANGE` | `40000-40100` | UDP port range captured by `tcpdump` |
| `OUT_DIR` | timestamped capture directory | Output directory |

## Ground-Truth Capture

For the comparison set, run the same Windows dialer against a real Linux
soft-modem path and capture on the soft-modem host:

```bash
mkdir -p artifacts/lkma-193a/ground-truth
LOCAL_IP=<soft-modem-host-ip> PEER_IP=<windows-or-ata-side-ip> \
  OUT_DIR=artifacts/lkma-193a/ground-truth/v21 \
  DURATION_SECONDS=30 artifacts/lkma-193a/capture.sh

LOCAL_IP=<soft-modem-host-ip> PEER_IP=<windows-or-ata-side-ip> \
  OUT_DIR=artifacts/lkma-193a/ground-truth/v22bis \
  DURATION_SECONDS=30 artifacts/lkma-193a/capture.sh
```

Expected final ground-truth files:

- `artifacts/lkma-193a/ground-truth/v21/inbound.wav`
- `artifacts/lkma-193a/ground-truth/v21/outbound.wav`
- `artifacts/lkma-193a/ground-truth/v21/report.json`
- `artifacts/lkma-193a/ground-truth/v22bis/inbound.wav`
- `artifacts/lkma-193a/ground-truth/v22bis/outbound.wav`
- `artifacts/lkma-193a/ground-truth/v22bis/report.json`

The current repository change supplies the capture harness. The actual
ground-truth recordings require the lab Windows dialer plus the Linux soft-modem
and USB analog FXO path to be connected during capture.
