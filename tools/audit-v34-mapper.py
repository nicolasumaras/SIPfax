#!/usr/bin/env python3
"""Check actual mapper coordinates against V.34 9.6.1 clockwise rotations.

Figure5 quarter-superconstellation points have both coordinates congruent to1
modulo4. Rotating clockwise gives residues (1,1),(1,3),(3,3),(3,1) for Z=0..3.
Compare these independent cosets to emitted u before precoding; a transmitter
and decoder that share the wrong handedness cannot satisfy this check.
"""
import argparse
import os
import json
from pathlib import Path
import subprocess
import tempfile
p=argparse.ArgumentParser(description=__doc__);p.add_argument('binary',type=Path);a=p.parse_args();rows=[]
for taps in ('0,0,0,0,0,0','4415,2209,-3795,190,2700,-614'):
 with tempfile.TemporaryDirectory() as tmp:
  d=Path(tmp);env={k:v for k,v in os.environ.items() if not k.startswith('SIPFAX_')}
  env.update(SIPFAX_B1_REFERENCE=str(d/'symbols'),SIPFAX_B1_RATE='12000',SIPFAX_B1_TRELLIS='64',SIPFAX_SHAPE='0',SIPFAX_B1_H=taps,SIPFAX_ENCDUMP=str(d/'enc'),SIPFAX_PC_TXDBG=str(d/'pc'))
  subprocess.run([str(a.binary.resolve())],env=env,capture_output=True,check=True,timeout=10)
  enc=[list(map(int,l.split())) for l in (d/'enc').read_text().splitlines()];pc=[list(map(int,l.split())) for l in (d/'pc').read_text().splitlines()]
  assert len(enc)==60 and len(pc)==120
  rotations=[z for row in enc for z in row[:2]];assert set(rotations)=={0,1,2,3}
  cosets=((1,1),(1,3),(3,3),(3,1));bad=[]
  for n,(z,point) in enumerate(zip(rotations,pc)):
   got=(point[0]%4,point[1]%4)
   if got!=cosets[z]:bad.append({'symbol':n,'rotation':z,'actualResidues':got,'requiredResidues':cosets[z]})
  rows.append({'taps':taps,'symbols':len(pc),'violations':len(bad),'firstViolations':bad[:4]})
print(json.dumps({'basis':'ITU-T V.34 (02/98) Figure5 and 9.6.1 clockwise rotation; actual encoder u/Z trace','runs':rows},indent=2))
raise SystemExit(1 if any(r['violations'] for r in rows) else 0)
