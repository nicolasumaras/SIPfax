"""Audit Ethernet IPv4 G.711 RTP continuity and PBX payload preservation.

Read-only; emits transport metadata, never audio or authentication payloads.
"""
import collections
import hashlib
import json
import struct
import sys


def read_flows(path):
    data = open(path, 'rb').read()
    endian = {'d4c3b2a1': '<', 'a1b2c3d4': '>'}[data[:4].hex()]
    if struct.unpack_from(endian + 'I', data, 20)[0] != 1:
        raise ValueError('Ethernet pcap required')
    flows = collections.defaultdict(list)
    pos = 24
    while pos + 16 <= len(data):
        sec, usec, size, _ = struct.unpack_from(endian + 'IIII', data, pos)
        pos += 16
        if pos + size > len(data):
            break  # A capture copied while running may end mid-record.
        packet = data[pos:pos + size]
        pos += size
        if len(packet) < 34 or packet[12:14] != b'\x08\x00':
            continue
        ip = packet[14:]
        if ip[9] != 17 or int.from_bytes(ip[6:8], 'big') & 0x3fff:
            continue
        udp = ip[(ip[0] & 15) * 4:int.from_bytes(ip[2:4], 'big')]
        if len(udp) < 20:
            continue
        sport, dport, length = struct.unpack_from('>HHH', udp)
        rtp = udp[8:length]
        if rtp[0] >> 6 != 2 or rtp[1] & 127 not in (0, 8):
            continue
        start = 12 + 4 * (rtp[0] & 15)
        if rtp[0] & 16:
            if len(rtp) < start + 4:
                continue
            start += 4 + 4 * int.from_bytes(rtp[start + 2:start + 4], 'big')
        end = len(rtp)
        if rtp[0] & 32:
            if not rtp[-1] or rtp[-1] > end - start:
                continue
            end -= rtp[-1]
        if start > end:
            continue
        seq, stamp, ssrc = struct.unpack_from('>HII', rtp, 2)
        key = ('.'.join(map(str, ip[12:16])), sport,
               '.'.join(map(str, ip[16:20])), dport, ssrc, rtp[1] & 127)
        flows[key].append((sec + usec / 1e6, seq, stamp, rtp[start:end]))
    return flows


def report(flows):
    for key, packets in flows.items():
        gaps, clock, intervals, wraps, timed_intervals = [], [], [], [], []
        for a, b in zip(packets, packets[1:]):
            step = (b[1] - a[1]) & 65535
            if step != 1:
                gaps.append([round(b[0] - packets[0][0], 6), step])
            elif b[1] < a[1]:
                wraps.append({'epoch': b[0],
                    'relative_seconds': round(b[0] - packets[0][0], 6)})
            ts_step = (b[2] - a[2]) & 0xffffffff
            if ts_step != len(a[3]):
                clock.append([round(b[0] - packets[0][0], 6), ts_step])
            intervals.append((b[0] - a[0]) * 1000)
            timed_intervals.append((intervals[-1], b[0]))
        ordered = sorted(intervals)
        print(json.dumps({'flow': key, 'packets': len(packets),
            'lengths': dict(collections.Counter(len(p[3]) for p in packets)),
            'sequence_anomalies': gaps, 'timestamp_anomalies': clock,
            'first_epoch': packets[0][0], 'last_epoch': packets[-1][0],
            'first_sequence': packets[0][1], 'sequence_wraps': wraps,
            'largest_intervals': [{'milliseconds': ms, 'epoch': epoch,
                'relative_seconds': round(epoch - packets[0][0], 6)}
                for ms, epoch in sorted(timed_intervals, reverse=True)[:5]],
            'interval_ms_min_median_max': [ordered[0], ordered[len(ordered)//2], ordered[-1]] if ordered else [],
            'payload_sha256': hashlib.sha256(b''.join(p[3] for p in packets)).hexdigest()}))
    # Packet-level exact comparison excludes duplicated silence hashes.
    keys = list(flows)
    for source, dest in [('192.168.1.25', '192.168.1.235'),
                         ('192.168.1.235', '192.168.1.25')]:
        incoming = [k for k in keys if k[0] == source]
        outgoing = [k for k in keys if k[2] == dest]
        for a in incoming:
            for b in outgoing:
                # Do not compare separate calls just because their endpoints
                # match: silence and repeated training share payload hashes.
                if min(flows[a][-1][0], flows[b][-1][0]) < max(flows[a][0][0], flows[b][0][0]):
                    continue
                left = collections.Counter(p[3] for p in flows[a])
                right = collections.Counter(p[3] for p in flows[b])
                mismatch = next((i for i, (x, y) in enumerate(zip(flows[a], flows[b]))
                    if x[3] != y[3]), None)
                if mismatch is None and len(flows[a]) != len(flows[b]):
                    mismatch = min(len(flows[a]), len(flows[b]))
                print(json.dumps({'path': [source, dest],
                    'identical_packets': sum((left & right).values()),
                    'ordered_payloads_identical': mismatch is None,
                    'first_ordered_mismatch_packet': mismatch,
                    'input_packets': len(flows[a]), 'output_packets': len(flows[b])}))


if __name__ == '__main__':
    report(read_flows(sys.argv[1]))
