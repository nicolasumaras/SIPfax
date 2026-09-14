#!/usr/bin/env python3
"""Replay a capture containing failed MP folds followed by a CRC-valid frame."""
import argparse
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('binary', type=Path)
p.add_argument('capture', type=Path)
a = p.parse_args()
env = {k: v for k, v in os.environ.items() if not k.startswith('SIPFAX_')}
env['SIPFAX_STREAM_FILE'] = str(a.capture.resolve())
r = subprocess.run([str(a.binary.resolve())], env=env, text=True,
                   capture_output=True, check=True, timeout=30)
failed = 0
validated = False
saw_e = False
for line in r.stderr.splitlines():
    if '[p4] FOLD ' in line and 'crc=fail' in line:
        failed += 1
    if '[p4] FOLD ' in line and 'crc=OK' in line:
        validated = True
    if '[p4blk] LIVE MP READ:' in line:
        validated = True
    if 'MP READ (' in line or 'MP-prime READ (' in line:
        assert validated and '(CRC)' in line, line
    if '[p4] E received' in line:
        assert validated, line
        saw_e = True
assert failed >= 2 and validated and saw_e, (failed, validated, saw_e)
print(f'PASS: {failed} failed folds cannot authorize MP/E; validated frame permits E')
