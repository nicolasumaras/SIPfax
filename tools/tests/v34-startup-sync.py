#!/usr/bin/env python3
"""Check clean decoding with a specification-derived superframe sequence.

Uses the existing diagnostic oracle input. This validates decoding *given*
alignment; it does not test automatic acquisition, B1 detection, or audio.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('binary', type=Path)
a = p.parse_args()
pattern = '0111011111111010'  # V.34 Table 12, J=8, leftmost bit first.
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    for shift in (0, 1):
        phase_file = root / f'sync-{shift}.txt'
        with phase_file.open('w') as f:
            for n in range(16000):
                phase = (n + shift) % 480
                v0 = int(pattern[phase//30]) if phase % 30 == 0 else 0
                f.write(f'0 0 0 {v0}\n')
        env = {k: v for k, v in os.environ.items() if not k.startswith('SIPFAX_')}
        env.update(SIPFAX_DATALOOP='1', SIPFAX_DATA_R='16800', SIPFAX_DL_SHAPE='1',
                   SIPFAX_FIG9='1', SIPFAX_HALF='2', SIPFAX_V0ORACLE=str(phase_file))
        subprocess.run([str(a.binary.resolve())], env=env, capture_output=True,
                       text=True, check=True, timeout=30)
        tx = Path('/tmp/dl_tx.txt').read_bytes()
        rx = Path('/tmp/dl_rx.txt').read_bytes()
        length = min(len(tx), len(rx))
        errors = sum(x != y for x, y in zip(tx, rx))
        assert length > 150000
        if shift == 0:
            assert errors == 0, errors
        else:
            assert errors > 0, 'wrong-phase control unexpectedly passed'
        print(f'phase shift {shift}: {errors} errors across {length} bits')
print('PASS: complete clean decode with correct Table 12 phase; shifted-phase control fails')
