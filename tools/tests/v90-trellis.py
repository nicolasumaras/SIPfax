#!/usr/bin/env python3
"""Independent Table13 transmitter tests for the experimental trellis kernel."""
import ctypes as C
import subprocess,tempfile
from pathlib import Path
import numpy as np
root=Path(__file__).resolve().parents[2]
# Low two bits transcribed from the four-point subset of ITU V.34 Table13.
converter=[[0,0,1,1],[3,2,2,3],[1,1,0,0],[2,3,3,2]]
pattern=[int(c) for c in '01110111111110']
rng=np.random.default_rng(90534)
with tempfile.TemporaryDirectory() as directory:
    d=Path(directory);w=d/'wrap.c';so=d/'test.so'
    w.write_text('''#include <stdlib.h>
#include "v90trellis.h"
void *create(void){V90Trellis *s=malloc(sizeof(*s));v90_trellis_init(s);return s;}
void destroy(void *s){free(s);}
''')
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror','-I'+str(root/'vendor/linmodem'),str(w),str(root/'vendor/linmodem/v90trellis.c'),'-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.create.restype=C.c_void_p;lib.destroy.argtypes=[C.c_void_p]
    lib.v90_trellis_pair.argtypes=[C.c_void_p,*([C.c_double]*4),C.c_uint,C.POINTER(C.c_uint),C.POINTER(C.c_uint)]
    total_hard=0
    for initial in range(16):
        n=2200;labels=[];inv=[];state=initial
        for i in range(n):
            inversion=pattern[(i//32)%14] if i%32==0 else 0
            a=int(rng.integers(4));b=(a+2*int(rng.integers(2))+((state&1)^inversion))%4
            labels.append([a,b]);inv.append(inversion)
            bits=converter[a][b];u=state&1
            state=(state>>1)^(bits&1)^((bits>>1)<<1)^(((bits>>1)^u)<<2)^(u<<3)
        labels=np.array(labels);clean=np.exp(-1j*labels*np.pi/2)
        for disturb in [False,True]:
            samples=clean.copy()
            if disturb:
                for i in range(150,n-150,101):samples[i,i%2]*=np.exp(1j*np.deg2rad(55))
            hard=(-np.rint(np.angle(samples)/(np.pi/2)).astype(int))%4
            total_hard+=int(np.count_nonzero(hard!=labels))
            s=lib.create();out=[]
            for i,(a,b) in enumerate(samples):
                aa=C.c_uint();bb=C.c_uint()
                ready=lib.v90_trellis_pair(s,a.real,a.imag,b.real,b.imag,inv[i],C.byref(aa),C.byref(bb))
                assert bool(ready)==(i>=63)
                if ready:out.append([aa.value,bb.value])
            lib.destroy(s);out=np.array(out)
            assert np.array_equal(out[64:],labels[64:len(out)]),(initial,disturb)
    assert total_hard>0
    print('PASS: all16 initial states, superframe inversions,64-pair ring wrap, clean data and',total_hard,'controlled hard-decision errors corrected')
