#!/usr/bin/env python3
"""Compare captured live RX frame parameters with initialized offline replay.

Input should be post-echo Phase-3/4 PCM. Matching frame parameters alone does
not establish bit-exact parity, data lock, or PPP success.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('binary', type=Path)
p.add_argument('capture', type=Path)
p.add_argument('live_log', type=Path)
a = p.parse_args()
pattern = r'\[data\] RX params: (.+)'
live = re.findall(pattern, a.live_log.read_text())
assert live, 'live log lacks negotiated RX frame parameters'
env = {k: v for k, v in os.environ.items() if not k.startswith('SIPFAX_')}
env.update(SIPFAX_STREAM_LIVE_INIT='1', SIPFAX_STREAM_FILE=str(a.capture.resolve()))
r = subprocess.run([str(a.binary.resolve())], env=env, capture_output=True,
                   text=True, check=True, timeout=30)
assert 'live RX initialization: peer calling=1 shape=1 R=19200 S=3429' in r.stderr
replayed = re.findall(pattern, r.stderr)
assert replayed and replayed[-1] == live[-1], (replayed, live)
print('PASS: live and replay negotiated RX parameters match: ' + replayed[-1])
for line in r.stderr.splitlines():
    if '[data] acquired:' in line:
        print(line)
