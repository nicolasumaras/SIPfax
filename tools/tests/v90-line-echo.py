#!/usr/bin/env python3
"""Independent echo/double-talk signal preservation and block equivalence."""
from pathlib import Path
import numpy as np,subprocess,json,tempfile
root=Path(__file__).resolve().parents[2]
rng=np.random.default_rng(81293);n=480000
reference=np.rint(rng.normal(0,5500,n));wanted=np.rint(rng.normal(0,1200,n));assert abs(reference).max()<32768
results=[]
with tempfile.TemporaryDirectory() as td:
 d=Path(td);binary=d/'echo-test'
 subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'vendor/linmodem'),str(root/'tools/tests/v90-line-echo-file.c'),str(root/'vendor/linmodem/v90lineecho.c'),'-lm','-o',str(binary)],check=True)
 tx=d/'tx';reference.astype('<i2').tofile(tx)
 for echo in [False,True]:
  received=wanted.copy()
  if echo:
   received[1428:]+=.01*reference[:-1428];received[1435:]-=.004*reference[:-1435]
  received=np.rint(received);rx=d/'rx';received.astype('<i2').tofile(rx);out=d/'out'
  subprocess.run([str(binary),str(rx),str(tx),str(out),'160'],check=True)
  z=np.fromfile(out,dtype='<i2').astype(float);s=320000
  before=float(np.sqrt(np.mean((received[s:]-wanted[s:])**2)));after=float(np.sqrt(np.mean((z[s:]-wanted[s:])**2)))
  assert after<30,(echo,after)
  if echo:assert after<before*.5
  expected_output=out.read_bytes()
  for block in ['1','1024']:
   subprocess.run([str(binary),str(rx),str(tx),str(out),block],check=True)
   assert out.read_bytes()==expected_output, 'Audio block size changed echo cancellation'
  results.append({'echo_injected':echo,'input_error_rms':before,'output_error_rms':after,'evaluated_seconds':[40,60]})
print(json.dumps(results,indent=2))
