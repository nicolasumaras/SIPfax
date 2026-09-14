#!/usr/bin/env python3
"""Check real receiver warmup across independent and reused decoder states."""
import os,subprocess,sys
from pathlib import Path
env={k:v for k,v in os.environ.items() if not k.startswith('SIPFAX_')}
env.update(SIPFAX_TRACEBACK_RESET_TEST='1',SIPFAX_V0ALIGN='0',SIPFAX_V0FRAME='0',SIPFAX_FIG9='1')
r=subprocess.run([str(Path(sys.argv[1]).resolve())],env=env,capture_output=True,text=True,timeout=15)
if r.returncode:sys.stderr.write(r.stderr)
r.check_returncode()
assert r.stdout.strip()=='PASS: fresh receiver, second receiver, and in-place decoder reset wait 30 traceback pairs'
print(r.stdout.strip())
