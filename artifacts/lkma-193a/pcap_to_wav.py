#!/usr/bin/env python3
"""Extract G.711 RTP from a pcap into directional 8 kHz linear PCM WAV files."""

from __future__ import annotations

import argparse
import ipaddress
import json
import struct
import sys
import wave
from dataclasses import dataclass, field
from pathlib import Path


PCAP_MAGIC = {
    b"\xd4\xc3\xb2\xa1": ("<", 1_000_000),
    b"\xa1\xb2\xc3\xd4": (">", 1_000_000),
    b"\x4d\x3c\xb2\xa1": ("<", 1_000_000_000),
    b"\xa1\xb2\x3c\x4d": (">", 1_000_000_000),
}
LINKTYPE_ETHERNET = 1
LINKTYPE_LINUX_SLL = 113

BIAS = 0x84
CLIP = 32635


def linear_to_ulaw(sample: int) -> int:
    sign = 0
    if sample < 0:
        sample = -sample
        sign = 0x80
    sample = min(sample, CLIP) + BIAS
    exponent = 7
    mask = 0x4000
    while exponent > 0 and not (sample & mask):
        mask >>= 1
        exponent -= 1
    mantissa = (sample >> (exponent + 3)) & 0x0F
    return (~(sign | (exponent << 4) | mantissa)) & 0xFF


def ulaw_to_linear(value: int) -> int:
    value = ~value & 0xFF
    sign = value & 0x80
    exponent = (value >> 4) & 0x07
    mantissa = value & 0x0F
    sample = ((mantissa << 3) + BIAS) << exponent
    sample -= BIAS
    return -sample if sign else sample


def alaw_to_linear(value: int) -> int:
    value ^= 0x55
    sign = value & 0x80
    exponent = (value & 0x70) >> 4
    mantissa = value & 0x0F
    sample = (mantissa << 4) + 8
    if exponent:
        sample += 0x100
        sample <<= exponent - 1
    return sample if sign else -sample


def samples_to_pcm(samples: list[int]) -> bytes:
    return b"".join(struct.pack("<h", max(-32768, min(32767, sample))) for sample in samples)


@dataclass
class RtpPacket:
    timestamp_s: float
    src: str
    dst: str
    payload_type: int
    sequence: int
    rtp_timestamp: int
    payload: bytes


@dataclass
class PcapCapture:
    link_type: int
    packets: list[tuple[float, bytes]]


@dataclass
class StreamStats:
    packet_count: int = 0
    expected_packets: int = 0
    gaps: list[dict[str, int]] = field(default_factory=list)
    out_of_order: int = 0
    ipdv_ms_values: list[float] = field(default_factory=list)
    first_sequence: int | None = None
    last_sequence: int | None = None
    first_capture_ts_s: float | None = None
    last_capture_ts_s: float | None = None
    first_rtp_ts: int | None = None
    last_rtp_ts: int | None = None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Decode PCMU/PCMA RTP from a pcap into inbound/outbound WAVs and report.json."
    )
    parser.add_argument("--pcap", required=True, type=Path)
    parser.add_argument("--local-ip", required=True, help="SIPfax VM IP address.")
    parser.add_argument(
        "--peer-ip",
        action="append",
        default=[],
        help="Optional RTP peer IP. May be repeated. When omitted, all UDP RTP peers are accepted.",
    )
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--inbound-wav", default="inbound.wav")
    parser.add_argument("--outbound-wav", default="outbound.wav")
    parser.add_argument("--report", default="report.json")
    return parser.parse_args()


def read_pcap(path: Path) -> PcapCapture:
    with path.open("rb") as handle:
        magic = handle.read(4)
        if magic not in PCAP_MAGIC:
            raise ValueError(f"{path} is not a classic pcap file with a supported byte order")
        endian, tick_rate = PCAP_MAGIC[magic]
        global_header = handle.read(20)
        if len(global_header) != 20:
            raise ValueError(f"{path} has a truncated pcap global header")
        _major, _minor, _thiszone, _sigfigs, _snaplen, link_type = struct.unpack(
            f"{endian}HHIIII", global_header
        )

        packets: list[tuple[float, bytes]] = []
        packet_header = struct.Struct(f"{endian}IIII")
        while True:
            header = handle.read(packet_header.size)
            if not header:
                break
            if len(header) != packet_header.size:
                raise ValueError(f"{path} has a truncated packet header")
            ts_sec, ts_frac, included_len, _original_len = packet_header.unpack(header)
            data = handle.read(included_len)
            if len(data) != included_len:
                raise ValueError(f"{path} has a truncated packet body")
            packets.append((ts_sec + ts_frac / tick_rate, data))
        return PcapCapture(link_type=link_type, packets=packets)


def ipv4_offset(frame: bytes, link_type: int) -> int | None:
    if link_type == LINKTYPE_ETHERNET:
        if len(frame) < 14:
            return None
        ether_type = int.from_bytes(frame[12:14], "big")
        offset = 14
        if ether_type == 0x8100 and len(frame) >= 18:
            ether_type = int.from_bytes(frame[16:18], "big")
            offset = 18
        if ether_type != 0x0800:
            return None
        return offset

    if link_type == LINKTYPE_LINUX_SLL:
        if len(frame) < 16:
            return None
        protocol_type = int.from_bytes(frame[14:16], "big")
        if protocol_type != 0x0800:
            return None
        return 16

    return None


def parse_udp_ipv4(frame: bytes, link_type: int = LINKTYPE_ETHERNET) -> tuple[str, str, int, int, bytes] | None:
    offset = ipv4_offset(frame, link_type)
    if offset is None:
        return None

    if len(frame) < offset + 20:
        return None

    ip_header_start = offset
    version_ihl = frame[ip_header_start]
    version = version_ihl >> 4
    ihl = (version_ihl & 0x0F) * 4
    if version != 4 or ihl < 20 or len(frame) < ip_header_start + ihl + 8:
        return None
    if frame[ip_header_start + 9] != 17:
        return None

    total_len = int.from_bytes(frame[ip_header_start + 2 : ip_header_start + 4], "big")
    src = str(ipaddress.ip_address(frame[ip_header_start + 12 : ip_header_start + 16]))
    dst = str(ipaddress.ip_address(frame[ip_header_start + 16 : ip_header_start + 20]))
    udp_start = ip_header_start + ihl
    src_port = int.from_bytes(frame[udp_start : udp_start + 2], "big")
    dst_port = int.from_bytes(frame[udp_start + 2 : udp_start + 4], "big")
    udp_len = int.from_bytes(frame[udp_start + 4 : udp_start + 6], "big")
    ip_payload_end = ip_header_start + total_len if total_len else len(frame)
    udp_payload_end = min(udp_start + udp_len, ip_payload_end, len(frame))
    return src, dst, src_port, dst_port, frame[udp_start + 8 : udp_payload_end]


def parse_rtp(timestamp_s: float, src: str, dst: str, payload: bytes) -> RtpPacket | None:
    if len(payload) < 12:
        return None
    first = payload[0]
    version = first >> 6
    if version != 2:
        return None

    padding = bool(first & 0x20)
    extension = bool(first & 0x10)
    csrc_count = first & 0x0F
    marker_pt = payload[1]
    payload_type = marker_pt & 0x7F
    if payload_type not in (0, 8):
        return None

    header_len = 12 + csrc_count * 4
    if len(payload) < header_len:
        return None
    if extension:
        if len(payload) < header_len + 4:
            return None
        extension_len_words = int.from_bytes(payload[header_len + 2 : header_len + 4], "big")
        header_len += 4 + extension_len_words * 4
        if len(payload) < header_len:
            return None

    rtp_payload = payload[header_len:]
    if padding:
        pad_len = rtp_payload[-1] if rtp_payload else 0
        if pad_len == 0 or pad_len > len(rtp_payload):
            return None
        rtp_payload = rtp_payload[:-pad_len]
    if not rtp_payload:
        return None

    return RtpPacket(
        timestamp_s=timestamp_s,
        src=src,
        dst=dst,
        payload_type=payload_type,
        sequence=int.from_bytes(payload[2:4], "big"),
        rtp_timestamp=int.from_bytes(payload[4:8], "big"),
        payload=rtp_payload,
    )


def update_stats(stats: StreamStats, packet: RtpPacket) -> None:
    stats.packet_count += 1
    if stats.first_sequence is None:
        stats.first_sequence = packet.sequence
        stats.last_sequence = packet.sequence
        stats.first_capture_ts_s = packet.timestamp_s
        stats.last_capture_ts_s = packet.timestamp_s
        stats.first_rtp_ts = packet.rtp_timestamp
        stats.last_rtp_ts = packet.rtp_timestamp
        stats.expected_packets = 1
        return

    assert stats.last_sequence is not None
    assert stats.last_capture_ts_s is not None
    assert stats.last_rtp_ts is not None

    expected_sequence = (stats.last_sequence + 1) & 0xFFFF
    if packet.sequence != expected_sequence:
        delta = (packet.sequence - expected_sequence) & 0xFFFF
        if delta < 0x8000:
            missing = delta
            if missing:
                stats.gaps.append(
                    {
                        "after_sequence": stats.last_sequence,
                        "next_sequence": packet.sequence,
                        "missing_packets": missing,
                    }
                )
                stats.expected_packets += missing
        else:
            stats.out_of_order += 1

    capture_delta_ms = (packet.timestamp_s - stats.last_capture_ts_s) * 1000
    rtp_delta_samples = (packet.rtp_timestamp - stats.last_rtp_ts) & 0xFFFFFFFF
    if rtp_delta_samples < 0x80000000:
        expected_delta_ms = rtp_delta_samples / 8
        stats.ipdv_ms_values.append(abs(capture_delta_ms - expected_delta_ms))

    stats.expected_packets += 1
    stats.last_sequence = packet.sequence
    stats.last_capture_ts_s = packet.timestamp_s
    stats.last_rtp_ts = packet.rtp_timestamp


def decode_payload(packet: RtpPacket) -> bytes:
    if packet.payload_type == 0:
        return samples_to_pcm([ulaw_to_linear(byte) for byte in packet.payload])
    if packet.payload_type == 8:
        return samples_to_pcm([alaw_to_linear(byte) for byte in packet.payload])
    raise ValueError(f"unsupported payload type {packet.payload_type}")


def write_wav(path: Path, pcm_chunks: list[bytes]) -> None:
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(8000)
        wav.writeframes(b"".join(pcm_chunks))


def stats_to_json(stats: StreamStats) -> dict[str, object]:
    loss = max(stats.expected_packets - stats.packet_count, 0)
    ipdv = stats.ipdv_ms_values
    return {
        "packet_count": stats.packet_count,
        "expected_packets": stats.expected_packets,
        "lost_packets": loss,
        "loss_fraction": loss / stats.expected_packets if stats.expected_packets else 0,
        "gaps": stats.gaps,
        "out_of_order": stats.out_of_order,
        "mean_ipdv_ms": sum(ipdv) / len(ipdv) if ipdv else 0,
        "max_ipdv_ms": max(ipdv) if ipdv else 0,
        "first_sequence": stats.first_sequence,
        "last_sequence": stats.last_sequence,
        "first_capture_ts_s": stats.first_capture_ts_s,
        "last_capture_ts_s": stats.last_capture_ts_s,
        "first_rtp_ts": stats.first_rtp_ts,
        "last_rtp_ts": stats.last_rtp_ts,
    }


def main() -> int:
    args = parse_args()
    local_ip = args.local_ip
    peer_ips = set(args.peer_ip)
    inbound_pcm: list[bytes] = []
    outbound_pcm: list[bytes] = []
    inbound_stats = StreamStats()
    outbound_stats = StreamStats()
    payload_types: dict[str, int] = {}

    capture = read_pcap(args.pcap)
    for timestamp_s, frame in capture.packets:
        udp = parse_udp_ipv4(frame, capture.link_type)
        if udp is None:
            continue
        src, dst, _src_port, _dst_port, udp_payload = udp
        if local_ip not in (src, dst):
            continue
        peer = dst if src == local_ip else src
        if peer_ips and peer not in peer_ips:
            continue
        packet = parse_rtp(timestamp_s, src, dst, udp_payload)
        if packet is None:
            continue

        direction = "outbound" if packet.src == local_ip else "inbound"
        payload_types[direction] = packet.payload_type
        if direction == "outbound":
            update_stats(outbound_stats, packet)
            outbound_pcm.append(decode_payload(packet))
        else:
            update_stats(inbound_stats, packet)
            inbound_pcm.append(decode_payload(packet))

    args.out_dir.mkdir(parents=True, exist_ok=True)
    inbound_path = args.out_dir / args.inbound_wav
    outbound_path = args.out_dir / args.outbound_wav
    report_path = args.out_dir / args.report
    write_wav(inbound_path, inbound_pcm)
    write_wav(outbound_path, outbound_pcm)

    report = {
        "pcap": str(args.pcap),
        "local_ip": local_ip,
        "peer_ips": sorted(peer_ips),
        "sample_rate_hz": 8000,
        "sample_format": "signed 16-bit little-endian PCM",
        "directions": {
            "inbound": {
                "description": "RTP from peer toward SIPfax",
                "wav": str(inbound_path),
                "payload_type": payload_types.get("inbound"),
                **stats_to_json(inbound_stats),
            },
            "outbound": {
                "description": "RTP from SIPfax toward peer/ATA path",
                "wav": str(outbound_path),
                "payload_type": payload_types.get("outbound"),
                **stats_to_json(outbound_stats),
            },
        },
    }
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"pcap_to_wav.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
