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
    lib.v90_trellis_sync.argtypes=[C.c_void_p,C.c_uint,C.POINTER(C.c_uint),C.POINTER(C.c_uint)]
    lib.v90_trellis_inversion.argtypes=[C.c_uint,C.c_uint]
    def acquire(labels):
        packed=np.ascontiguousarray(labels[:,0]|(labels[:,1]<<2),dtype=np.uint8)
        offset=C.c_uint();errors=C.c_uint()
        ok=lib.v90_trellis_sync(packed.ctypes.data,len(packed),C.byref(offset),C.byref(errors))
        return ok,offset.value,errors.value
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
            ok,offset,errors=acquire(hard)
            assert ok and offset==0,(initial,disturb,ok,offset,errors)
            if not disturb:assert errors==0

            s=lib.create();out=[]
            for i,(a,b) in enumerate(samples):
                aa=C.c_uint();bb=C.c_uint()
                ready=lib.v90_trellis_pair(s,a.real,a.imag,b.real,b.imag,inv[i],C.byref(aa),C.byref(bb))
                assert bool(ready)==(i>=63)
                if ready:out.append([aa.value,bb.value])
            lib.destroy(s);out=np.array(out)
            assert np.array_equal(out[64:],labels[64:len(out)]),(initial,disturb)
    # Cropping and rotating symbols must retain the actual superframe phase.
    for crop in [0,1,31,32,127,389,447]:
        for rotation in range(4):
            ok,offset,errors=acquire((labels[crop:]+rotation)%4)
            assert ok and offset==(-crop)%448 and errors==0,(crop,rotation,offset,errors)
            for i in range(448):
                expected=pattern[((i+crop)//32)%14] if (i+crop)%32==0 else 0
                assert lib.v90_trellis_inversion(i,offset)==expected
    for bad in [np.zeros((2200,2),dtype=int),rng.integers(0,4,(2200,2)),labels[:895]]:
        assert not acquire(bad)[0],'false superframe acquisition'
    assert total_hard>0
    print('PASS: all16 initial states, superframe inversions,64-pair ring wrap, clean data and',total_hard,'controlled hard-decision errors corrected; automatic superframe sync and rejection guards')
