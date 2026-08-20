#!/usr/bin/env python3
"""
V.34 MP sequence decoder - 4-point and 16-point.

Mirrors linmodem's `V34_mod_MP`:

    I1 = scramble(bit); I2 = scramble(bit)
    if 16-point: Q1 = scramble(bit); Q2 = scramble(bit); q = (Q2<<1)|Q1
    else:        q = 0
    z = (((I2<<1)|I1) + z) & 3                 # differential, accumulating
    symbol = constellation[q] rotated CLOCKWISE by z*90 deg

So each symbol carries 2 bits (4-point) or 4 bits (16-point), in the order I1,I2[,Q1,Q2].

Decoding: the 16 transmitted points are {base[q] rotated CW by z}, q,z in 0..3. A global
receiver phase rotation by k*90 deg maps (q,z) -> (q, z+k): it shifts z uniformly and
leaves q untouched. z is only ever used differentially, so the ambiguity cancels and q is
recovered absolutely. That makes the decode well defined without any phase reference.

Validated on synthetic MP at both sizes (run this file).
"""
import numpy as np, math

DEG = 23
GPA = 1 | (1 << 18)     # answer modem: 1 + x^-5  + x^-23   (V.34 clause 7)
GPC = 1 | (1 << 5)      # call   modem: 1 + x^-18 + x^-23

# the first four points of the V.34 data constellation = the MP base points
BASE = np.array([1+1j, -3+1j, 1-3j, -3-3j])


def descramble(bits, poly):
    """Self-synchronising descrambler: out = MSB(reg) ^ in, reg fed by the RECEIVED bit."""
    reg = 0
    out = np.empty(len(bits), int)
    for i, b in enumerate(bits):
        out[i] = ((reg >> (DEG-1)) & 1) ^ int(b)
        reg = (reg << 1) & ((1 << DEG) - 1)
        if b:
            reg ^= poly
    return out


def scramble(bits, poly):
    reg = 0
    out = []
    for b in bits:
        o = ((reg >> (DEG-1)) & 1) ^ int(b)
        reg = (reg << 1) & ((1 << DEG) - 1)
        if o:
            reg ^= poly
        out.append(o)
    return np.array(out, int)


def crc16(bits):
    c = 0xffff
    for bit in bits:
        x = (c & 1) ^ int(bit)
        c = (c >> 1) ^ ((x << 15) | (x << 10) | (x << 3))
    return sum(((c >> i) & 1) << (15-i) for i in range(16))


def mp_points(sixteen):
    """(points, q_of, z_of) for the MP signal set."""
    pts, qs, zs = [], [], []
    nq = 4 if sixteen else 1
    for q in range(nq):
        for z in range(4):
            pts.append(BASE[q]*((-1j)**z))     # rotate CLOCKWISE by z*90
            qs.append(q); zs.append(z)
    p = np.array(pts)
    return p/np.sqrt(np.mean(np.abs(p)**2)), np.array(qs), np.array(zs)


def symbols_to_bits(s, sixteen):
    """Equalised symbols -> the transmitted (still scrambled) bit stream."""
    pts, qs, zs = mp_points(sixteen)
    v = s/np.sqrt(np.mean(np.abs(s)**2))
    idx = np.argmin(np.abs(v[:, None] - pts[None, :]), axis=1)
    q = qs[idx]; z = zs[idx]
    dz = (z[1:] - z[:-1]) % 4                  # differential -> I1,I2 (phase-ambiguity free)
    I1 = dz & 1; I2 = dz >> 1
    if not sixteen:
        return np.column_stack([I1, I2]).ravel()
    Q = q[1:]                                   # q of the same symbol the dz ends on
    return np.column_stack([I1, I2, Q & 1, (Q >> 1) & 1]).ravel()


def trn_bits(s, sixteen, rot=0):
    """TRN -> transmitted (scrambled) bits.

    NOTE the rotation convention differs from MP. 10.1.3.6: TRN rotates point 2*Q2+Q1 of
    the quarter-superconstellation CLOCKWISE by In*90 with In = 2*I2n+I1n - an ABSOLUTE
    rotation. J and MP (10.1.3.3) instead ACCUMULATE: Zn = In + Zn-1. Decoding TRN with the
    differential convention yields ~50% ones and looks like "no lock", which is why the
    descramble-to-all-ones check was long believed unreliable. With the absolute convention
    a real caller TRN descrambles to 0.998 ones.

    `rot` applies one of the four global phase hypotheses (absolute decoding has no
    differential to cancel receiver phase, so the caller must try all four).
    """
    pts, qs, zs = mp_points(sixteen)
    v = s*((1j)**rot)
    v = v/np.sqrt(np.mean(np.abs(v)**2))
    idx = np.argmin(np.abs(v[:, None] - pts[None, :]), axis=1)
    q = qs[idx]; z = zs[idx]                 # z is In directly, not a difference
    I1 = z & 1; I2 = z >> 1
    if not sixteen:
        return np.column_stack([I1, I2]).ravel()
    return np.column_stack([I1, I2, q & 1, (q >> 1) & 1]).ravel()


def trn_score(s, sixteen, poly):
    """Best (ones-fraction, rot) over the four phase hypotheses. ~1.0 => TRN decoded."""
    best = (0.0, None)
    for rot in range(4):
        o = float(descramble(trn_bits(s, sixteen, rot), poly).mean())
        sc = max(o, 1-o)
        if sc > best[0]:
            best = (sc, rot)
    return best


def find_frames(bits, poly):
    """Descramble and return every CRC-valid MP frame as a dict."""
    db = descramble(bits, poly)
    out = []
    i = 0
    while i < len(db)-190:
        if db[i:i+17].all():
            for typ, L, co in ((1, 188, 171), (0, 88, 69)):
                if i+L > len(db):
                    continue
                f = db[i:i+L]
                if int(f[18]) != typ:
                    continue
                cov = [f[bp] for bp in range(17, co)
                       if not ((bp == 17 or bp == 34) or
                               ((bp >= 51 and (bp-51) % 17 == 0) if typ else (bp in (51, 68))))]
                if crc16(cov) == sum(int(f[co+k]) << (15-k) for k in range(16)):
                    out.append(dict(
                        at=i, type=typ,
                        ca=int((f[20] << 3)|(f[21] << 2)|(f[22] << 1)|f[23])*2400,
                        ac=int((f[24] << 3)|(f[25] << 2)|(f[26] << 1)|f[27])*2400,
                        aux=int(f[28]), trellis=int((f[29] << 1)|f[30]),
                        nonlin=int(f[31]), shape=int(f[32]), ack=int(f[33]),
                        mask=sum(int(f[35+k]) << k for k in range(15)),
                        asym=int(f[50]) if L > 50 else None))
                    i += L-1
                    break
        i += 1
    return out


def decode(s, poly, sixteen=None):
    """Try 4-point and/or 16-point; return (frames, which)."""
    tries = [sixteen] if sixteen is not None else [False, True]
    best = ([], None)
    for six in tries:
        fr = find_frames(symbols_to_bits(s, six), poly)
        if len(fr) > len(best[0]):
            best = (fr, '16-point' if six else '4-point')
    return best


def build_mp(type_=1, ack=0, ca=12, ac=12, trel=0, mask=0x0fff):
    """Build an MP frame exactly as V34_send_MP does (for validation)."""
    b = []
    def put(n, v):
        for i in range(n-1, -1, -1):
            b.append((v >> i) & 1)
    put(17, 0x1ffff); put(1, 0); put(1, type_); put(1, 0)
    put(4, ca); put(4, ac); put(1, 0); put(2, trel); put(1, 0); put(1, 0); put(1, ack)
    put(1, 0)
    for i in range(15):
        b.append((mask >> i) & 1)
    put(1, 1)
    if type_ == 1:
        for _ in range(3):
            for _ in range(2):
                put(1, 0); put(16, 0)
    put(1, 0); put(16, 0); put(1, 0)
    cov = [b[bp] for bp in range(17, len(b))
           if not ((bp == 17 or bp == 34) or
                   ((bp >= 51 and (bp-51) % 17 == 0) if type_ else (bp in (51, 68))))]
    put(16, crc16(cov)); put(1, 0)
    return np.array(b, int)


def modulate(frame_bits, poly, sixteen, reps=30):
    """Bits -> MP symbols, exactly as V34_mod_MP (for validation)."""
    sc = scramble(np.tile(frame_bits, reps), poly)
    step = 4 if sixteen else 2
    z = 0; out = []
    for i in range(0, len(sc)-step+1, step):
        I1, I2 = int(sc[i]), int(sc[i+1])
        q = ((int(sc[i+3]) << 1) | int(sc[i+2])) if sixteen else 0
        z = (((I2 << 1) | I1) + z) & 3
        out.append(BASE[q]*((-1j)**z))
    return np.array(out)


if __name__ == '__main__':
    print("MP decoder self-test (synthetic, no channel)\n")
    for sixteen in (False, True):
        nm = '16-point' if sixteen else '4-point'
        f = build_mp(ack=1, ca=7, ac=4, trel=2)
        sym = modulate(f, GPA, sixteen)
        for rot in range(4):                      # phase ambiguity must not matter
            fr, which = decode(sym*((1j)**rot), GPA)
            ok = len(fr) > 0 and which == nm
            if rot == 0:
                print("  %-9s: %d frames, detected as %s" % (nm, len(fr), which))
                if fr:
                    d = fr[0]
                    print("             ca=%d ac=%d trellis=%d ack=%d mask=0x%04x"
                          % (d['ca'], d['ac'], d['trellis'], d['ack'], d['mask']))
            if not ok:
                print("             rotation %d: FAILED" % rot)
                break
        else:
            print("             rotation-invariant across all 4 phases: OK")
