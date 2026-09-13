#!/usr/bin/env python3
"""Check integrated S acquisition against independently measured PCM boundaries.

The same two-burst capture with MD=0 checks branching only, not a real MD=0 peer.
"""
import argparse
import os
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('binary')
p.add_argument('capture')
p.add_argument('first_sbar_end', type=float, help='seconds relative to capture start')
p.add_argument('second_sbar_end', type=float)
a = p.parse_args()
for md, expected in ((0, a.first_sbar_end), (700, a.second_sbar_end)):
    env = dict(os.environ, SIPFAX_V34_MATCHED_S='1',
               SIPFAX_DECODE_MD_MS=str(md),
               SIPFAX_DECODE_FILE=os.path.abspath(a.capture) + ':1')
    result = subprocess.run([os.path.abspath(a.binary)], env=env,
                            capture_output=True, text=True, check=True, timeout=30)
    log = result.stderr
    matches = re.findall(r'matched PP input=([\d.]+) filtered=([\d.]+) target=([\d.]+)', log)
    assert len(matches) == 1, matches
    raw, filtered, target = map(float, matches[0])
    assert abs(target / 8000 - expected) < .001, (md, target, expected)
    # First PP output follows the boundary by at most two 3x-baud outputs.
    assert 0 <= filtered - target <= 2 * 8000 / (3 * (24000 / 7)), (filtered, target)
    assert raw > filtered, (raw, filtered)
    transitions = [tuple(map(int, m)) for m in re.findall(
        r'demod state (-?\d+) -> (\d+) .* at (\d+) ms', log)]
    assert any(new == 5 for _, new, _ in transitions), transitions
    if md:
        entered = next(t for _, new, t in transitions if new == 18)
        exited = next(t for old, _, t in transitions if old == 18)
        assert abs(exited - entered - md) <= 1, transitions
        assert any(old == 19 and new == 3 for old, new, _ in transitions), transitions
    else:
        assert not any(new in (18, 19, 2, 3) for _, new, _ in transitions), transitions
    print(f'PASS MD={md}: PP PCM boundary {target / 8000:.6f}s; reference {expected:.6f}s')
