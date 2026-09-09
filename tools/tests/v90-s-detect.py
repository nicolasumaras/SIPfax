#!/usr/bin/env python3
"""Check S/Sbar detection, tone rejection, and optional real hardware capture."""
import ctypes as C
from pathlib import Path
import subprocess,tempfile,sys
import numpy as np
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    wrapper=Path(tmp)/'wrapper.c'
    wrapper.write_text('''#include <stdlib.h>
#include "v90training.h"
void *create(void) { return calloc(1, sizeof(V90SDetect)); }
void destroy(void *state) { free(state); }
''')
    so=Path(tmp)/'s.so'
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror','-I'+str(root/'vendor/linmodem'),str(wrapper),str(root/'vendor/linmodem/v90training.c'),str(root/'vendor/linmodem/v90dil.c'),str(root/'vendor/linmodem/v90cp.c'),'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.v90_s_detect.argtypes=[C.c_void_p,C.c_int16]
    lib.create.restype=C.c_void_p;lib.destroy.argtypes=[C.c_void_p]
    def events(x):
        state=lib.create();out=[]
        assert state, 'detector allocation failed'
        try:
            for i,v in enumerate(x):
                event=lib.v90_s_detect(state,int(v))
                if event:out.append((i/8000,event))
            return out
        finally:
            lib.destroy(state)
    rng=np.random.default_rng(3)
    n=np.arange(5000);t=n/8000
    for start in range(400,500,7):
        active=(n>=start)&(n<start+840)
        x=(1300*np.cos(2*np.pi*1920*t+.8)+900*np.cos(2*np.pi*320*t+.2)+900*np.cos(2*np.pi*3520*t+1.4))*active
        x[n>=start+800]*=-1
        found=events(x+rng.normal(0,10,len(x)))
        assert [e for _,e in found]==[1,2],found
        assert (start+800)/8000<=found[1][0]<=(start+840)/8000
    for f in [320,1200,1800,1920,2400,3520]:
        assert not events(2500*np.cos(2*np.pi*f*t)+rng.normal(0,10,len(t)))
    assert not events(rng.normal(0,1500,5000))
    if len(sys.argv)>1:
        x=np.fromfile(sys.argv[1],dtype='<i2');found=events(x[11*8000:16*8000]);print('Hardware events:',[(11+t,e) for t,e in found]);assert any(e==1 and 14.58<11+t<14.65 for t,e in found)
    print('PASS: S/Sbar across offsets with noise; pure tones/noise rejected')
