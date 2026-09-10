import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'audit_rtp', Path(__file__).resolve().parents[2] / 'research/v90/audit_rtp.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class AuditTests(unittest.TestCase):
    def test_red_primary_and_bounds(self):
        primary = b'p' * 160
        redundant = b'r' * 160
        self.assertEqual(audit.red_primary(b'\0' + primary), primary)
        self.assertEqual(audit.red_primary(b'\x80\x02\x80\xa0\0' + redundant + primary), primary)
        for value in [b'', b'\x80', b'\x80\x02\x80\xa0',
                      b'\x80\x02\x80\xa0\0' + redundant, b'\x60' + primary]:
            self.assertIsNone(audit.red_primary(value))

    def test_cooked_and_ethernet_red_selection(self):
        payload = b'\x80\x02\x80\xa0\0' + b'r' * 160 + b'p' * 160
        rtp = struct.pack('!BBHII', 128, 96, 42, 160, 999) + payload
        udp = struct.pack('!HHHH', 10000, 35268, len(rtp) + 8, 0) + rtp
        ip = bytearray(20)
        ip[0] = 69
        ip[9] = 17
        struct.pack_into('!H', ip, 2, len(udp) + 20)
        ip[12:20] = bytes([192, 168, 1, 29, 192, 168, 1, 235])
        packets = {1: bytes(12) + b'\x08\0', 113: bytes(14) + b'\x08\0',
                   276: b'\x08\0' + bytes(18)}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.pcap'
            expected = None
            for kind, prefix in packets.items():
                raw = prefix + bytes(ip) + udp
                header = struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, kind)
                path.write_bytes(header + struct.pack('<IIII', 100, 200, len(raw), len(raw)) + raw)
                self.assertFalse(audit.read_flows(path))
                flows = audit.read_flows(path, red_payload_type=96)
                self.assertEqual(len(flows), 1)
                record = next(iter(flows.values()))[0]
                self.assertEqual(record, (100.0002, 42, 160, b'p' * 160))
                if expected is not None:
                    self.assertEqual(flows, expected)
                expected = flows


if __name__ == '__main__':
    unittest.main()
