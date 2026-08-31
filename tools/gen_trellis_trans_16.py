#!/usr/bin/env python3
"""Generate linmodem's V.34 64-state branch table (trellis_trans_16).

WHY THIS EXISTS
    v34table.c is gitignored, and the table's semantics were nowhere written down. This
    script reconstructs it from first principles and is VALIDATED: fed the labelling the
    tree currently uses (--label y0) it reproduces the shipped trellis_trans_16 EXACTLY,
    32 of 32 blocks, all 8 tuples each, every one of the 256 coset tuples appearing once.

THE CONSTRUCTION
    A row is a coset 4-tuple (u0,v0,u1,v1), one 2-bit label per coordinate of the two 2D
    symbols, where u = ((Re+3)>>1)&3 and v = ((Im+3)>>1)&3.

    The table is 32 blocks of 8: block index = trans + 16*U0, with
        trans = (Y3<<3)|(Y4<<2)|(Y2<<1)|Y1   as v34.c packs it.

    The two U0 halves are NOT the same tuples. V.34 9.6.1 rotates u(2m+1) by
    [Z(m) + 2*I1 + U0(m)]*90 degrees, so U0 applies one extra 90-degree rotation to the
    SECOND 2D symbol only. On coset labels that is exact: Re = 2u-3, Im = 2v-3 and
    (Re,Im) -> (-Im,Re) give (u,v) -> (3-v, u). The shipped table obeys this for 16 of 16
    trans values (-90 and 180 match none), hence:

        half0(t) = { c : trans(c) == t and trans(rot(c)) == t }     -> exactly 8 tuples
        half1(t) = rot(half0(t))

    A builder that puts the SAME tuples in both halves - which is what the runtime builder
    in v34.c used to do - leaves the ACS with nothing to separate U0=0 from U0=1, and it
    scores at chance (4.1% state tracking).

USAGE
    python3 gen_trellis_trans_16.py --label y0 > table.c     # reproduces the shipped table
    python3 gen_trellis_trans_16.py --label y0 --verify v34table.c
    python3 gen_trellis_trans_16.py --label x0 > table.c     # Figure 9 variant, see NOTE

NOTE ON --label x0 AND --rot
    Figure 9/V.34 gives a middle subset bit that depends only on Re (x0); the tree uses y0,
    which disagrees at 8 of 16 points.

    The extra thing that must change with the labelling is --rot: which member of each
    rotation class is the half0 representative. Measured in loopback at 16800 with the x0
    labelling: --rot 0 and 2 give 84.8%, --rot 1 and 3 give 100.0%. With --rot 1 the x0
    labelling is fully self-consistent, 100.0% at 7200, 16800 and 33600.

    It is still NOT the live bug. Scored like-for-like on a real capture the sync-bit
    estimate moves only 24.9% -> 26.1%, where 2.5% would be correct, so both labellings
    extract the same poor structure from a real transmitter. Kept for the record.

    (An earlier report that this pair collapsed to chance was wrong: the table had been
    dropped from v34table.c while v34priv.h still declared it without extern, and -fcommon
    silently supplied a zero-filled array. Verify the table is really there.)
"""
import argparse, re, sys

def label(u, v, rule):
    x0 = u & 1; x1 = (u & 2) >> 1
    y0 = v & 1; y1 = (v & 2) >> 1
    s2 = x1 ^ y1 ^ y0 ^ x0
    s1 = x0 if rule == "x0" else y0
    s0 = y0 ^ x0
    return s2, s1, s0

def trans_of(c, rule):
    a = label(c[0], c[1], rule); b = label(c[2], c[3], rule)
    Y4 = a[0] ^ b[0]; Y3 = a[1]; Y2 = a[2]
    Y1 = (a[2] & (~b[2]) & 1) ^ a[1] ^ b[1]
    return (Y3 << 3) | (Y4 << 2) | (Y2 << 1) | Y1

rot = lambda c: (c[0], c[1], (3 - c[3]) & 3, c[2])   # 9.6.1: 90 deg on the 2nd 2D symbol

def rotn(c, n):
    for _ in range(n % 4):
        c = rot(c)
    return c

def build(rule, k=0):
    allc = [(a, b, c, d) for a in range(4) for b in range(4)
                          for c in range(4) for d in range(4)]
    blocks = {}
    for t in range(16):
        s = [c for c in allc if trans_of(c, rule) == t]
        base = sorted(c for c in s if trans_of(rot(c), rule) == t)
        h0 = [rotn(c, k) for c in base]
        blocks[t] = h0
        blocks[t + 16] = [rot(c) for c in h0]
    assert all(len(v) == 8 for v in blocks.values()), "blocks must hold 8 tuples"
    seen = set()
    for v in blocks.values():
        seen |= set(v)
    assert len(seen) == 256, "every coset tuple must appear exactly once"
    return blocks

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", choices=("y0", "x0"), default="y0")
    ap.add_argument("--rot", type=int, default=0, choices=(0, 1, 2, 3),
                    help="which member of each rotation class is the half0 representative")
    ap.add_argument("--verify", metavar="v34table.c")
    a = ap.parse_args()
    blocks = build(a.label, a.rot)
    if a.verify:
        rows = [tuple(int(x) for x in m) for m in re.findall(
            r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}",
            re.search(r"u8 trellis_trans_16\[256\]\[4\][^{]*\{.*?\n\};",
                      open(a.verify).read(), re.S).group(0))]
        ref = {b: set(rows[b*8:(b+1)*8]) for b in range(32)}
        same = sum(1 for b in range(32) if set(blocks[b]) == ref[b])
        print("%d of 32 blocks identical to %s" % (same, a.verify), file=sys.stderr)
        sys.exit(0 if same == 32 else 1)
    print("u8 trellis_trans_16[256][4] = {")
    for b in range(32):
        print(" /* trans=%d u0=%d */" % (b % 16, b // 16))
        for c in blocks[b]:
            print(" { %d, %d, %d, %d }," % c)
    print("};")

if __name__ == "__main__":
    main()
