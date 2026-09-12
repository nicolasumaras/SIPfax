#!/usr/bin/env python3
"""Generate V.34 branch tuples from subset labels and modulation geometry.

Each base constellation point has both coordinates congruent to 1 modulo 4.
A 90-degree rotation flips one coordinate's coset parity. Therefore Z parity
is u XOR v for coset labels u=((Re+3)//2)&3, v=((Im+3)//2)&3.
Clause 9.6.1 gives Z1=Z0+2*I0+U0 (mod 4), so the branch half is
U0=(u0 XOR v0 XOR u1 XOR v1)&1. Combine that half with the Figure 9/Table 13
transition index. Every one of 256 tuples belongs to exactly one 8-row block.

--label y0 --verify vendor/linmodem/v34table.c reproduces the legacy table.
--label x0 generates the Figure 9 table. The former rotation heuristic failed
for x0: it assigned about half of clean encoder tuples to the wrong half.
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

def build(rule):
    blocks = {k: [] for k in range(32)}
    for a in range(4):
        for b in range(4):
            for c in range(4):
                for d in range(4):
                    row = (a, b, c, d)
                    half = (a ^ b ^ c ^ d) & 1
                    blocks[trans_of(row, rule) + 16*half].append(row)
    assert all(len(v) == 8 for v in blocks.values())
    assert len({row for rows in blocks.values() for row in rows}) == 256
    return blocks

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", choices=("y0", "x0"), default="y0")
    ap.add_argument("--verify", metavar="v34table.c")
    a = ap.parse_args()
    blocks = build(a.label)
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
