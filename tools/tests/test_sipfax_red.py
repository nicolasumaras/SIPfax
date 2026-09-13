import importlib.util
from pathlib import Path
import socket
import struct
import unittest
from unittest import mock
import tempfile
import types
import io
from contextlib import redirect_stdout

spec = importlib.util.spec_from_file_location(
    'sipfax_red', Path(__file__).resolve().parents[2] / 'deploy/sipfax_red.py')
red = importlib.util.module_from_spec(spec)
spec.loader.exec_module(red)


def packet(seq, stamp, value, port=35270, ssrc=1234):
    ip = bytearray(20)
    ip[0] = 0x45
    ip[8] = 64
    ip[9] = 17
    ip[12:20] = socket.inet_aton('192.168.1.29') + socket.inet_aton('192.168.1.235')
    struct.pack_into('!H', ip, 2, 200)
    return (bytes(ip) + struct.pack('!HHHH', 12300, port, 180, 0) +
            struct.pack('!BBHII', 128, 0, seq, stamp, ssrc) + bytes([value]) * 160)


class RedTests(unittest.TestCase):
    def setUp(self):
        self.encoder = red.RedEncoder('192.168.1.29', '192.168.1.235')
        self.encoder.register(('1.2', 35270))

    def test_wire_blocks_checksums_wrap_and_missing_packet(self):
        first = packet(65535, 0xffffffa0, 17)
        second = packet(0, 64, 29)
        a = self.encoder.packet(first)
        self.encoder.packet(second)  # Simulate this packet being lost on the wire.
        b = self.encoder.packet(packet(1, 224, 31))
        self.assertEqual(len(a), 201)
        self.assertEqual(b[40:45], b'\x80\x02\x80\xa0\0')
        self.assertEqual(b[45:205], second[40:])
        self.assertEqual(b[205:], bytes([31]) * 160)
        self.assertEqual(red.checksum(b[:20]), 0)
        pseudo = b[12:20] + b'\0\x11' + struct.pack('!H', len(b) - 20)
        self.assertEqual(red.checksum(pseudo + b[20:]), 0)
        self.assertEqual(self.encoder.packet(b), b)

    def test_hangup_and_reused_port_clear_history(self):
        self.encoder.packet(packet(1, 160, 1))
        self.encoder.register(None)
        raw = packet(2, 320, 2)
        self.assertEqual(self.encoder.packet(raw), raw)
        self.encoder.register(('2.3', 35270))
        self.assertEqual(len(self.encoder.packet(raw)), 201)
        self.assertEqual(len(self.encoder.packet(packet(3, 480, 3))), 365)

    def test_unrelated_destination_preserves_authorized_stream(self):
        self.encoder.packet(packet(1, 160, 1))
        raw = packet(2, 320, 9, port=35268)
        self.assertEqual(self.encoder.packet(raw), raw)
        self.assertEqual(len(self.encoder.packet(packet(2, 320, 2))), 365)

    def test_gap_or_ssrc_change_has_no_false_redundancy(self):
        self.encoder.packet(packet(1, 160, 1))
        self.assertEqual(len(self.encoder.packet(packet(3, 480, 2))), 201)
        self.assertEqual(len(self.encoder.packet(packet(4, 640, 3, ssrc=5678))), 201)

    def test_startup_timestamp_rewind_preserves_clock_and_resets_redundancy(self):
        # Hardware capture: early media ended at seq 8831 / timestamp 2400;
        # bridged modem audio began at seq 8832 / timestamp 0, same SSRC.
        # Exercise both activation at the handoff and an already active bridge.
        for active_during_early_media in (False, True):
            with self.subTest(active=active_during_early_media):
                encoder = red.RedEncoder('192.168.1.29', '192.168.1.235')
                if active_during_early_media:
                    encoder.register(('handoff', 35270))
                encoder.packet(packet(8831, 2400, 17))
                encoder.register(('handoff', 35270))
                first = bytearray(packet(8832, 0, 29))
                first[29] |= 128  # The RTP marker must survive encapsulation.
                first = bytes(first)
                encoded = encoder.packet(first)
                self.assertEqual(encoded[28], first[28])
                self.assertEqual(encoded[29], 128 | 96)
                self.assertEqual(encoded[30:40], first[30:40])
                self.assertEqual(encoded[40:], b'\0' + first[40:])
                following = encoder.packet(packet(8833, 160, 31))
                self.assertEqual(following[40:45], b'\x80\x02\x80\xa0\0')
                self.assertEqual(following[45:205], first[40:])
                self.assertEqual(following[205:], bytes([31]) * 160)

    def test_malformed_and_non_audio_are_unchanged(self):
        raw = packet(1, 160, 1)
        for value in [b'', raw[:20], raw[:39], raw + b'x',
                      raw[:28] + b'\x90' + raw[29:]]:
            self.assertEqual(self.encoder.packet(value), value)

    def test_registration_is_strict_and_bound_to_ata(self):
        self.assertEqual(red.parse_destination('Value: 123.4|192.168.1.235:35270',
                                              '192.168.1.235'), ('123.4', 35270))
        for value in ['Value: 123.4|192.168.1.234:35270',
                      'Value: 123.4|192.168.1.235:5062',
                      'Value: $(id)|192.168.1.235:35270',
                      'Database entry not found.']:
            self.assertIsNone(red.parse_destination(value, '192.168.1.235'))


class ControlTests(unittest.TestCase):
    def test_registration_cleanup_and_require_active(self):
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(red.os, 'chown'), \
                mock.patch.object(red.grp, 'getgrnam', return_value=types.SimpleNamespace(gr_gid=0)), \
                mock.patch.object(red.subprocess, 'check_output') as query, \
                redirect_stdout(io.StringIO()):
            path = directory + '/control'
            control = red.Control(path, '192.168.1.235')
            control.start()
            try:
                query.return_value = b'Value: 123.4|192.168.1.235:35270\n'
                self.assertEqual(red.sync(path, require_active=True), 0)
                self.assertEqual(control.registration, ('123.4', 35270))
                query.return_value = b'Database entry not found.\n'
                self.assertEqual(red.sync(path, require_active=True), 1)
                self.assertIsNone(control.registration)
                self.assertEqual(red.sync(path), 0)
                query.side_effect = red.subprocess.CalledProcessError(124, 'timeout')
                self.assertEqual(red.sync(path), 1)
                self.assertIsNone(control.registration)
            finally:
                control.alive = False
                control.sock.close()
                control.join(3)
            self.assertFalse(control.is_alive())

    def test_absent_service_rejects_setup(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(red.sync(directory + '/missing', require_active=True), 1)


if __name__ == '__main__':
    unittest.main()
