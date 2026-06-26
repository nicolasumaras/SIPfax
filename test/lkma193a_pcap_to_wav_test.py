#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = REPO_ROOT / "artifacts" / "lkma-193a" / "pcap_to_wav.py"


spec = importlib.util.spec_from_file_location("pcap_to_wav", MODULE_PATH)
assert spec is not None
pcap_to_wav = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = pcap_to_wav
assert spec.loader is not None
spec.loader.exec_module(pcap_to_wav)


def ipv4_udp_packet(src: str, dst: str, udp_payload: bytes) -> bytes:
    src_bytes = bytes(int(part) for part in src.split("."))
    dst_bytes = bytes(int(part) for part in dst.split("."))
    udp_len = 8 + len(udp_payload)
    total_len = 20 + udp_len
    ip_header = bytearray(20)
    ip_header[0] = 0x45
    ip_header[2:4] = total_len.to_bytes(2, "big")
    ip_header[8] = 64
    ip_header[9] = 17
    ip_header[12:16] = src_bytes
    ip_header[16:20] = dst_bytes
    udp_header = struct.pack("!HHHH", 40000, 40002, udp_len, 0)
    return bytes(ip_header) + udp_header + udp_payload


def rtp_packet(payload: bytes) -> bytes:
    return struct.pack("!BBHII", 0x80, 0x00, 1, 160, 0x12345678) + payload


def write_pcap(path: Path, link_type: int, frame: bytes) -> None:
    global_header = struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, link_type)
    packet_header = struct.pack("<IIII", 1, 0, len(frame), len(frame))
    path.write_bytes(global_header + packet_header + frame)


def ethernet_frame(ip_packet: bytes) -> bytes:
    return (b"\x00" * 12) + b"\x08\x00" + ip_packet


def linux_sll_frame(ip_packet: bytes) -> bytes:
    return struct.pack("!HHH8sH", 0, 1, 0, b"\x00" * 8, 0x0800) + ip_packet


class PcapToWavTest(unittest.TestCase):
    def test_parses_linux_sll_capture_from_tcpdump_any(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            pcap_path = tmp_path / "rtp-sll.pcap"
            ip_packet = ipv4_udp_packet("192.168.1.125", "192.168.1.31", rtp_packet(b"\xff" * 160))
            write_pcap(pcap_path, pcap_to_wav.LINKTYPE_LINUX_SLL, linux_sll_frame(ip_packet))

            result = subprocess.run(
                [
                    sys.executable,
                    str(MODULE_PATH),
                    "--pcap",
                    str(pcap_path),
                    "--local-ip",
                    "192.168.1.125",
                    "--out-dir",
                    str(tmp_path),
                ],
                check=True,
                text=True,
                capture_output=True,
            )

            report = json.loads(result.stdout)
            self.assertEqual(report["directions"]["outbound"]["packet_count"], 1)
            self.assertEqual(report["directions"]["inbound"]["packet_count"], 0)
            self.assertGreater((tmp_path / "outbound.wav").stat().st_size, 44)

    def test_keeps_ethernet_parser_compatible(self) -> None:
        ip_packet = ipv4_udp_packet("192.168.1.31", "192.168.1.125", b"payload")
        parsed = pcap_to_wav.parse_udp_ipv4(ethernet_frame(ip_packet), pcap_to_wav.LINKTYPE_ETHERNET)

        self.assertIsNotNone(parsed)
        assert parsed is not None
        self.assertEqual(parsed[:4], ("192.168.1.31", "192.168.1.125", 40000, 40002))
        self.assertEqual(parsed[4], b"payload")


if __name__ == "__main__":
    unittest.main()
