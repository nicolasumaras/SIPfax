#!/usr/bin/env python3
"""Independent V.34 B1 scrambler/framing checks for 16800 bit/s at 3429 baud.

Uses clauses 7, 8, 9.6.3 and 10.1.3.1. It does not validate shell mapping,
constellation coordinates, precoding, pulse shaping, or the complete waveform.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('binary', type=Path)
a = p.parse_args()
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    env = {k: v for k, v in os.environ.items() if not k.startswith('SIPFAX_')}
    env.update(SIPFAX_B1_REFERENCE=str(root/'symbols.txt'),
               SIPFAX_MFDUMP_TX=str(root/'bits.txt'), SIPFAX_ENCDUMP=str(root/'sync.txt'))
    subprocess.run([str(a.binary.resolve())], env=env, capture_output=True,
                   text=True, timeout=10, check=True)
    frames = (root/'bits.txt').read_text().splitlines()
    # One 35 ms data frame: 16800 * .035 = 588 bits, 15 mapping frames.
    expected_lengths = [39 + (k in (4, 9, 14)) for k in range(15)]
    assert list(map(len, frames)) == expected_lengths
    # Call scrambler divides all-ones input by 1 + D^18 + D^23, zero history.
    expected = []
    for n in range(588):
        expected.append(1 ^ (expected[n-18] if n >= 18 else 0)
                          ^ (expected[n-23] if n >= 23 else 0))
    assert ''.join(frames) == ''.join(map(str, expected))
    sync = [tuple(map(int, line.split())) for line in (root/'sync.txt').read_text().splitlines()]
    assert len(sync) == 60  # Four 4D intervals per mapping frame.
    # Last data frame of J=8 superframe uses Table 12's final pair "10".
    for n, row in enumerate(sync):
        v0, half_position, half_index = row[3:6]
        assert (half_position, half_index) == (n % 30, 14 + n//30)
        assert v0 == (1 if n == 0 else 0)
print('PASS: 588 GPC-scrambled B1 bits; mapping-frame schedule; final-superframe sync pattern')
