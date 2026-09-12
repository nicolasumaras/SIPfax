#!/usr/bin/env python3
"""Check B1 reference length, deterministic resets, and parameter handling.

These checks do not establish that the encoder matches a hardware modem.
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
    out = Path(tmp) / 'b1.txt'
    def generate(**options):
        env = {k: v for k, v in os.environ.items() if not k.startswith('SIPFAX_')}
        env.update(SIPFAX_B1_REFERENCE=str(out), **options)
        r = subprocess.run([str(a.binary.resolve())], env=env, capture_output=True,
                           text=True, timeout=10)
        assert r.returncode == 0, r.stderr
        pairs = [tuple(map(int, line.split())) for line in out.read_text().splitlines()]
        assert len(pairs) == 120 and all(len(v) == 2 for v in pairs)
        return pairs
    reference = generate()
    assert generate() == reference
    assert generate(SIPFAX_B1_H='0,0,0,0,0,0') == reference
    assert generate(SIPFAX_B1_H='4476,1768,-3722,329,2710,-924') != reference
    for trellis in ('16', '32', '64'):
        for shape in ('0', '1'):
            opts = dict(SIPFAX_B1_TRELLIS=trellis, SIPFAX_SHAPE=shape)
            assert generate(**opts) == generate(**opts)
    for invalid in ('1,2,3', '32768,0,0,0,0,0', '1,2,3,4,5,6extra'):
        env = {k: v for k, v in os.environ.items() if not k.startswith('SIPFAX_')}
        env.update(SIPFAX_B1_REFERENCE=str(out), SIPFAX_B1_H=invalid)
        assert subprocess.run([str(a.binary.resolve())], env=env, capture_output=True,
                              timeout=10).returncode != 0
print('PASS: 120-symbol B1 references, deterministic resets, shaping/trellis/tap controls')
