#!/usr/bin/env python3
"""Summarize an offline SIPFAX_STREAM_STATE trace around received E.

Kurtosis describes amplitude distribution, not successful symbol decoding.
"""
import argparse
import csv
import json
from pathlib import Path
from statistics import mean

p = argparse.ArgumentParser()
p.add_argument('trace', type=Path)
p.add_argument('--capture-offset', type=float, default=0)
a = p.parse_args()
with a.trace.open() as f:
    rows = [{k: float(v) for k, v in r.items()} for r in csv.DictReader(f)]
e = next(r['sample24k'] / 24000 for r in rows if r['e_received'])
first_data = next((r['sample24k'] / 24000 for r in rows if r['data_symbols']), None)
windows = []
for start, end in ((-.2, 0), (0, .05), (.05, .15), (.15, .3), (.3, .6), (.6, 1), (1, 1.5), (1.5, 2)):
    selected = [r for r in rows if start <= r['sample24k']/24000 - e < end]
    if not selected:
        continue
    z = [complex(r['i'], r['q']) for r in selected]
    powers = [abs(v)**2 for v in z]
    fourth = mean(v*v for v in powers)
    windows.append({'fromESeconds': [start, end], 'symbols': len(z),
                    'powerKurtosis': fourth / mean(powers)**2 if mean(powers) else None,
                    'fourthPowerCoherence': abs(sum(v**4 for v in z)) / (len(z)*fourth) if fourth else None,
                    'maxAcqSymbols': max(r['acq_symbols'] for r in selected),
                    'maxDataSymbols': max(r['data_symbols'] for r in selected)})
print(json.dumps({'eSeconds': e + a.capture_offset,
                  'firstDataDecodeSecondsAfterE': first_data-e if first_data is not None else None,
                  'windows': windows}, indent=2))
