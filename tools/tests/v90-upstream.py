#!/usr/bin/env python3
"""Recover CRC-valid PPP LCP from a real V.90 caller recording in native C."""
import ctypes as C
from pathlib import Path
import subprocess,tempfile,sys
import numpy as np
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    w=Path(tmp)/'wrap.c';so=Path(tmp)/'up.so'
    w.write_text('''#include <stdlib.h>
#include "v90upstream.h"
void *create(void){V90Upstream *s=malloc(sizeof(*s));v90_upstream_init(s);return s;}
void run(V90Upstream *s,const int16_t *x,unsigned n){for(unsigned i=0;i<n;++i)v90_upstream_receive(s,x[i]);}
unsigned frames(V90Upstream *s){return s->frames;}
unsigned lcp(V90Upstream *s){return s->last_length>=6 && s->last_frame[0]==255 && s->last_frame[1]==3 && s->last_frame[2]==0xc0 && s->last_frame[3]==0x21;}
''')
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror','-I'+str(root/'vendor/linmodem'),str(w),str(root/'vendor/linmodem/v90upstream.c'),'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.create.restype=C.c_void_p
    for name in ['frames','lcp']:getattr(lib,name).argtypes=[C.c_void_p]
    lib.run.argtypes=[C.c_void_p,np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS'),C.c_uint]
    x=np.fromfile(sys.argv[1],dtype='<i2')[21*8000:36*8000];s=lib.create()
    for i in range(0,len(x),160):a=x[i:i+160];lib.run(s,a,len(a))
    assert lib.frames(s)>0 and lib.lcp(s)
    print('PASS: native upstream receiver recovered',lib.frames(s),'CRC-valid LCP frames')
