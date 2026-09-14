#!/usr/bin/env python3
"""Real ramped S/Sbar across packet phases, with data/noise rejection.
Usage: v90-s-ramp.py RX10057.s16 RX10222.s16 RX10992.s16
"""
import ctypes as C
from pathlib import Path
import subprocess,tempfile,sys
import numpy as np
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
 w=Path(tmp)/'w.c';so=Path(tmp)/'test.so'
 w.write_text('''#include <string.h>
#include "v90training.h"
unsigned run(const int16_t *x,unsigned n,unsigned offset){V90SDetect s;memset(&s,0,sizeof(s));unsigned found=0;
 for(unsigned i=0;i<offset;++i)v90_s_detect(&s,0);
 for(unsigned i=0;i<n;++i){int e=v90_s_detect(&s,x[i]);if(e==1)found+=1;if(e==2)found+=1000;}
 return found;}
''')
 subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror','-I'+str(root/'vendor/linmodem'),str(w),*[str(root/'vendor/linmodem'/n) for n in ['v90training.c','v90cp.c','v90dil.c']],'-lm','-o',str(so)],check=True)
 lib=C.CDLL(str(so));ptr=np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS');lib.run.argtypes=[ptr,C.c_uint,C.c_uint]
 recordings=[np.fromfile(p,dtype='<i2') for p in sys.argv[1:4]]
 for recording,(start,end) in zip(recordings,[(15.85,16.025),(17.4,17.57),(15.85,16.01)]):
  x=recording[round(start*8000):round(end*8000)]
  for offset in range(100):assert lib.run(x,len(x),offset)==1001,(start,offset)
 # Genuine data before its later rate request; includes quiet periods/PPP bursts.
 x=recordings[0][30*8000:440*8000]
 assert lib.run(x,len(x),0)==0,'false S in long hardware data recording'
 # CPt itself must not look like a rate request.
 x=recordings[2][round(16.1*8000):round(18.6*8000)]
 assert lib.run(x,len(x),0)==0,'false S in CPt'
 rng=np.random.default_rng(902);t=np.arange(20*8000)
 for f in [0,320,1800,1920,2400,3520]:
  x=(rng.normal(0,3000,len(t)) if not f else 3000*np.cos(2*np.pi*f*t/8000)+rng.normal(0,20,len(t))).astype(np.int16)
  assert lib.run(x,len(x),0)==0,('noise/tone false S',f)
 print('PASS: all 300 real S/Sbar timing alignments; 410s hardware data, CPt, noise and single-tone rejection')
