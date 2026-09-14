"""Read receiver-reported RTP loss from Ethernet/IPv4 captures (metadata only).

RTCP SR/RR report blocks follow RFC3550 section6.4. A receiver's counters
are observations, not proof of where a packet was lost or discarded.
"""
import json
import struct
import sys
from pathlib import Path


def decode_reports(payload):
    """Return report blocks, rejecting truncated or malformed compounds."""
    reports = []
    offset = 0
    while offset < len(payload):
        if offset + 4 > len(payload):
            return []
        flags, kind, words = struct.unpack_from('!BBH', payload, offset)
        end = offset + 4 * (words + 1)
        if flags >> 6 != 2 or not 192 <= kind <= 223 or end > len(payload):
            return []
        packet = payload[offset:end]
        if flags & 32:
            padding = packet[-1]
            if end != len(payload) or not padding or padding > len(packet) - 4:
                return []
            packet = packet[:-padding]
        if kind in (200, 201):
            start = 28 if kind == 200 else 8
            count = flags & 31
            if len(packet) < start + 24 * count:
                return []
            reporter = int.from_bytes(packet[4:8], 'big')
            for i in range(count):
                block = packet[start + i * 24:start + (i + 1) * 24]
                reports.append({
                    'reporter_ssrc': reporter,
                    'source_ssrc': int.from_bytes(block[:4], 'big'),
                    'fraction_lost_256': block[4],
                    'cumulative_lost': int.from_bytes(block[5:8], 'big', signed=True),
                    'highest_extended_sequence': int.from_bytes(block[8:12], 'big'),
                    'jitter_timestamp_units': int.from_bytes(block[12:16], 'big'),
                })
        offset = end
    return reports


def read_reports(path):
    data = Path(path).read_bytes()
    if len(data) < 24:
        raise ValueError('Truncated pcap header')
    endian = {b'\xd4\xc3\xb2\xa1': '<', b'\xa1\xb2\xc3\xd4': '>'}.get(data[:4])
    if not endian or struct.unpack_from(endian + 'I', data, 20)[0] != 1:
        raise ValueError('Microsecond Ethernet pcap required')
    pos = 24
    while pos + 16 <= len(data):
        sec, usec, size, _ = struct.unpack_from(endian + 'IIII', data, pos)
        pos += 16
        packet = data[pos:pos + size]
        pos += size
        if len(packet) != size:
            break  # A live capture copy can end partway through a record.
        if len(packet) < 34 or packet[12:14] != b'\x08\x00':
            continue
        ip = packet[14:]
        header = (ip[0] & 15) * 4
        total = int.from_bytes(ip[2:4], 'big')
        if ip[0] >> 4 != 4 or header < 20 or total > len(ip) or total < header + 8:
            continue
        if ip[9] != 17 or int.from_bytes(ip[6:8], 'big') & 0x3fff:
            continue
        udp = ip[header:total]
        length = int.from_bytes(udp[4:6], 'big')
        if length < 8 or length > len(udp):
            continue
        for report in decode_reports(udp[8:length]):
            yield {'epoch': sec + usec / 1e6,
                   'reporter_ip': '.'.join(map(str, ip[12:16])), **report}


if __name__ == '__main__':
    for row in read_reports(sys.argv[1]):
        print(json.dumps(row))
