#!/usr/bin/env python3
"""Check the public carrier-readiness gate through V34 startup and retraining."""
import os,subprocess,sys
from pathlib import Path
binary=Path(sys.argv[1]).resolve();env={k:v for k,v in os.environ.items() if not k.startswith('SIPFAX_')};env['SIPFAX_READINESS_TEST']='1'
r=subprocess.run([str(binary)],env=env,capture_output=True,text=True,check=True,timeout=10)
rows=r.stdout.splitlines();assert len(rows)==71
for mask,line in enumerate(rows[:64]):assert line==f'stage {mask} {int(mask==63)}',line
assert rows[64:]==['lapm-uninitialized 0','lapm-negotiating 0','lapm-connected 1','lapm-reacquiring 0','unset 0','retrain 0','idle 1']
print('PASS: 64 startup readiness combinations, unset configuration, retraining, and idle state')
