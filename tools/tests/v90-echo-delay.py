#!/usr/bin/env python3
"""Independent delayed-echo acquisition and ambiguous/no-echo rejection."""
from pathlib import Path
import numpy as np,json,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
rng=np.random.default_rng(282719);n=96000
with tempfile.TemporaryDirectory() as td:
 d=Path(td);binary=d/'test'
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'vendor/linmodem'),str(root/'tools/tests/v90-echo-delay-file.c'),str(root/'vendor/linmodem/v90echodelay.c'),str(root/'vendor/linmodem/v90lineecho.c'),'-lm','-o',str(binary)],check=True)
 def run(rx,tx):
  assert np.max(abs(rx))<32768 and np.max(abs(tx))<32768
  np.rint(rx).astype('<i2').tofile(d/'rx');np.rint(tx).astype('<i2').tofile(d/'tx')
  result=json.loads(subprocess.check_output([str(binary),str(d/'rx'),str(d/'tx'),str(d/'out')],text=True))
  return result,np.fromfile(d/'out',dtype='<i2').astype(float)
 for delay in [240,1428,3217,6911]:
  tx=rng.normal(0,5500,n);wanted=rng.normal(0,2,n);wanted[:40000]+=rng.normal(0,1200,40000);rx=wanted.copy();rx[delay:]+=.008*tx[:-delay];rx[delay+7:]-=.002*tx[:-delay-7]
  r,z=run(rx,tx)
  assert r['locked'] and r['delay']==delay and r['lock_sample']<64000,(delay,r)
  assert np.mean((z[80000:]-wanted[80000:])**2)<np.mean((rx[80000:]-wanted[80000:])**2)
 for name in ['silence','unrelated-noise','tone']:
  tx=rng.normal(0,5500,n)
  if name=='silence':rx=np.zeros(n)
  elif name=='unrelated-noise':rx=rng.normal(0,20,n)
  else:
   tx=5500*np.sin(2*np.pi*2100*np.arange(n)/8000);rx=np.zeros(n);rx[1428:]=.008*tx[:-1428]
  r,z=run(rx,tx);assert not r['locked'],(name,r)
  assert np.array_equal(z,np.rint(rx)), 'Unqualified delay altered RX audio'
print('PASS: four independent delays, echo reduction, no false locks or RX changes in silence/noise/tone controls')
