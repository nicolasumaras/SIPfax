#!/usr/bin/env python3
"""Check caller-J recognition on supplied PCM, with a same-length silence control.

Pass a Phase-3 caller capture containing TRN and J, at 3429 baud. This checks
receive negotiation only; it does not prove a complete handshake or PPP.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('binary', type=Path)
p.add_argument('capture', type=Path)
a = p.parse_args()

def replay(path):
    env = os.environ.copy()
    for name in tuple(env):
        if name.startswith('SIPFAX_'):
            del env[name]
    env['SIPFAX_STREAM_FILE'] = str(path.resolve())
    r = subprocess.run([str(a.binary.resolve())], env=env, capture_output=True,
                       text=True, timeout=30, check=True)
    end = re.search(r'\[stream\] END: J_received=(\d+)', r.stderr)
    assert end, r.stderr[-1000:]
    return int(end[1]), r.stderr

found, log = replay(a.capture)
assert found == 1, log[-1000:]
assert 'caller J detected' in log
assert re.search(r'J variant vote: .* -> J(?:4|16)POINTS', log)
with tempfile.TemporaryDirectory() as tmp:
    silence = Path(tmp) / 'silence.s16'
    with silence.open('wb') as f:
        f.truncate(a.capture.stat().st_size)
    found, log = replay(silence)
    assert not found and 'caller J detected' not in log
print('PASS: caller J and constellation vote recognized; equal-duration silence rejected')
