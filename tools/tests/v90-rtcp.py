#!/usr/bin/env python3
"""Independent RTCP wire fixtures, malformed datagrams and pcap boundaries."""
import importlib.util
import struct
import tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('audit_rtcp', root/'research/v90/audit_rtcp.py')
m = importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
# One receiver report: SSRC1 reports SSRC2, fraction3/256, loss -2,
# extended sequence65537, jitter9, zero LSR/DLSR.
rr = bytes.fromhex('81c90007000000010000000203fffffe00010001000000090000000000000000')
r = m.decode_reports(rr)
assert r == [{'reporter_ssrc':1,'source_ssrc':2,'fraction_lost_256':3,'cumulative_lost':-2,'highest_extended_sequence':65537,'jitter_timestamp_units':9}]
sr = bytes.fromhex('81c8000c00000001') + bytes(20) + rr[8:]
assert m.decode_reports(sr) == r
assert m.decode_reports(rr + rr) == r + r
padded = bytes.fromhex('a1c90008') + rr[4:] + b'\0\0\0\4'
assert m.decode_reports(padded) == r
for bad in [rr[:-1], rr + b'\0', bytes.fromhex('82c90007') + rr[4:], b'\x41' + rr[1:], padded[:-1]+b'\0', padded+rr]:
    assert m.decode_reports(bad) == []
assert m.decode_reports(bytes.fromhex('80c9000100000001')) == []
with tempfile.TemporaryDirectory() as td:
    ip = bytearray(20);ip[0]=0x45;ip[9]=17;ip[12:16]=bytes([192,0,2,1]);ip[16:20]=bytes([192,0,2,2])
    udp = struct.pack('!HHHH',5001,5003,8+len(rr),0)+rr
    ip[2:4]=(20+len(udp)).to_bytes(2,'big')
    ethernet=bytes(12)+b'\x08\x00'
    for endian in ['<','>']:
        header=struct.pack(endian+'IHHIIII',0xa1b2c3d4,2,4,0,0,65535,1)
        packet=ethernet+ip+udp
        record=struct.pack(endian+'IIII',123,250000,len(packet),len(packet))+packet
        path=Path(td)/'test.pcap';path.write_bytes(header+record+record[:-1])
        assert list(m.read_reports(path)) == [{'epoch':123.25,'reporter_ip':'192.0.2.1',**r[0]}]
        ip[6:8]=b'\x20\0'  # Never parse an unreassembled IP fragment as RTCP.
        path.write_bytes(header+struct.pack(endian+'IIII',123,0,len(packet),len(packet))+ethernet+ip+udp)
        assert list(m.read_reports(path)) == []
        ip[6:8]=bytes(2)
print('PASS: SR/RR, compound reports, signed loss, padding, malformed input and fragmented/truncated pcaps')
