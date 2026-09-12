#!/usr/bin/env python3
"""Replay an explicitly supplied caller training segment (beginning before S)."""
import argparse,os,re,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('binary',type=Path);p.add_argument('capture',type=Path);a=p.parse_args()
for md in (0,700):
 env=os.environ.copy();env['SIPFAX_DECODE_FILE']=str(a.capture.resolve())+':1';env['SIPFAX_DECODE_MD_MS']=str(md)
 with tempfile.TemporaryFile(mode='w+') as f:
  subprocess.run([str(a.binary.resolve())],env=env,stdout=f,stderr=f,check=True,timeout=30)
  f.seek(0);text=f.read()
 transitions=[tuple(map(int,m)) for m in re.findall(r'demod state (-?\d+) -> (\d+) .* at (\d+) ms',text)]
 if md:
  entered=next(t for old,new,t in transitions if new==18)
  exited=next(t for old,new,t in transitions if old==18)
  assert exited-entered==700,(entered,exited)
  assert any(old==19 and new==2 for old,new,t in transitions)
 else:
  assert not any(new in (18,19,2,3) for old,new,t in transitions)
 assert any(new==4 for old,new,t in transitions)
 assert any(new==5 for old,new,t in transitions)
print('PASS: zero-MD skips the second pair; negotiated 700 ms waits then reacquires S before PP/TRN')
