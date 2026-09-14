#!/usr/bin/env python3
"""Check real V34 front-end position/history accounting across silent bursts."""
import os,subprocess,sys
from pathlib import Path
env={k:v for k,v in os.environ.items() if not k.startswith('SIPFAX_')}
env['SIPFAX_DATA_TIMELINE_TEST']='1'
r=subprocess.run([str(Path(sys.argv[1]).resolve())],env=env,capture_output=True,text=True,timeout=15)
if r.returncode:sys.stderr.write(r.stderr)
r.check_returncode()
assert r.stdout.strip()=='PASS: data positions and history survive silence; startup squelch remains'
print(r.stdout.strip())
