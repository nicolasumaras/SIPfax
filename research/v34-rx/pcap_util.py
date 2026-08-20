"""Extract per-direction G.711 audio from an RTP pcap (Linux SLL2 / SLL / Ethernet)."""
import struct, numpy as np

def _ulaw(v):
    v = ~v & 0xff
    t = ((v & 0x0f) << 3) + 0x84
    t <<= (v & 0x70) >> 4
    return (0x84 - t) if (v & 0x80) else (t - 0x84)

_LUT = np.array([_ulaw(b) for b in range(256)], dtype=np.int16)


def flows(path):
    """{(src_ip, dst_port): [(timestamp, payload), ...]}"""
    d = open(path, 'rb').read()
    dlt = struct.unpack('<I', d[20:24])[0]
    off = 24
    fl = {}
    while off + 16 <= len(d):
        ts, tu, cl, ol = struct.unpack('<IIII', d[off:off+16]); off += 16
        pkt = d[off:off+cl]; off += cl
        if dlt == 276:   l2, eth = 20, pkt[0:2]
        elif dlt == 113: l2, eth = 16, pkt[14:16]
        else:            l2, eth = 14, pkt[12:14]
        if eth != b'\x08\x00':
            continue
        ip = pkt[l2:]
        if len(ip) < 20 or ip[9] != 17:
            continue
        u = ip[(ip[0] & 0x0f)*4:]
        if len(u) < 12:
            continue
        pl = u[8:]
        if len(pl) < 12:
            continue
        key = ('.'.join(map(str, ip[12:16])), struct.unpack('>H', u[2:4])[0])
        fl.setdefault(key, []).append((ts + tu*1e-6, pl[12:]))
    return fl


def audio(fl, src_ip):
    """Largest flow from src_ip -> (samples, first_timestamp)."""
    ks = [k for k in fl if k[0] == src_ip]
    k = max(ks, key=lambda kk: len(fl[kk]))
    p = sorted(fl[k], key=lambda z: z[0])
    x = np.concatenate([_LUT[np.frombuffer(z[1], np.uint8)] for z in p]).astype(float)
    return x, p[0][0]
