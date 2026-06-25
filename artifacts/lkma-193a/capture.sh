#!/usr/bin/env bash
set -euo pipefail

DURATION_SECONDS="${DURATION_SECONDS:-30}"
RTP_PORT_RANGE="${RTP_PORT_RANGE:-40000-40100}"
LOCAL_IP="${LOCAL_IP:-}"
PEER_IP="${PEER_IP:-}"
OUT_DIR="${OUT_DIR:-$(pwd)/artifacts/lkma-193a/captures/$(date -u +%Y%m%dT%H%M%SZ)}"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

if [[ -z "$LOCAL_IP" ]]; then
  LOCAL_IP="$(hostname -I | awk '{print $1}')"
fi

if [[ -z "$LOCAL_IP" ]]; then
  echo "LOCAL_IP could not be inferred; rerun with LOCAL_IP=192.168.1.31" >&2
  exit 2
fi

if ! command -v tcpdump >/dev/null 2>&1; then
  echo "tcpdump is required on the SIPfax VM" >&2
  exit 2
fi

mkdir -p "$OUT_DIR"
PCAP="$OUT_DIR/rtp.pcap"

FILTER="udp and portrange ${RTP_PORT_RANGE}"
if [[ -n "$PEER_IP" ]]; then
  FILTER="${FILTER} and host ${PEER_IP}"
fi

echo "Capturing ${DURATION_SECONDS}s of RTP on ${LOCAL_IP}; output: ${OUT_DIR}" >&2
echo "tcpdump filter: ${FILTER}" >&2

sudo timeout "$DURATION_SECONDS" tcpdump -i any -s 0 -w "$PCAP" "$FILTER" || true

PY_ARGS=(
  --pcap "$PCAP"
  --local-ip "$LOCAL_IP"
  --out-dir "$OUT_DIR"
)
if [[ -n "$PEER_IP" ]]; then
  PY_ARGS+=(--peer-ip "$PEER_IP")
fi

python3 "$SCRIPT_DIR/pcap_to_wav.py" "${PY_ARGS[@]}"

if command -v ffmpeg >/dev/null 2>&1; then
  ffmpeg -hide_banner -loglevel error -y \
    -i "$OUT_DIR/outbound.wav" \
    -lavfi showspectrumpic=s=1280x720:legend=1 \
    "$OUT_DIR/outbound-spectrogram.png"
  echo "Wrote $OUT_DIR/outbound-spectrogram.png" >&2
else
  echo "ffmpeg not found; skipping outbound spectrogram generation" >&2
fi

echo "Wrote $OUT_DIR/inbound.wav, $OUT_DIR/outbound.wav, and $OUT_DIR/report.json" >&2
