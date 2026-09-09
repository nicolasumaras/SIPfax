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
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror','-I'+str(root/'vendor/linmodem'),str(w),str(root/'vendor/linmodem/v90trellis.c'),'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.create.restype=C.c_void_p;lib.destroy.argtypes=[C.c_void_p]
    lib.v90_trellis_pair.argtypes=[C.c_void_p,*([C.c_double]*4),C.c_uint,C.POINTER(C.c_uint),C.POINTER(C.c_uint)]
    lib.v90_trellis_sync.argtypes=[C.c_void_p,C.c_uint,C.POINTER(C.c_uint),C.POINTER(C.c_uint)]
    lib.v90_trellis_inversion.argtypes=[C.c_uint,C.c_uint]
    def acquire(labels):
        packed=np.ascontiguousarray(labels[:,0]|(labels[:,1]<<2),dtype=np.uint8)
        offset=C.c_uint();errors=C.c_uint()
        ok=lib.v90_trellis_sync(packed.ctypes.data,len(packed),C.byref(offset),C.byref(errors))
        return ok,offset.value,errors.value
    class Acquisition(C.Structure):
        _fields_=[('phase',C.c_double),('gain',C.c_double),('coherence',C.c_double),('pair_alignment',C.c_uint),('offset',C.c_uint),('errors',C.c_uint),('pairs',C.c_uint)]
    lib.v90_trellis_acquire.argtypes=[C.c_void_p,C.c_void_p,C.c_uint,C.POINTER(Acquisition)]
    def acquire_samples(samples):
        re=np.ascontiguousarray(samples.real,dtype=np.float64);im=np.ascontiguousarray(samples.imag,dtype=np.float64);a=Acquisition()
        ok=lib.v90_trellis_acquire(re.ctypes.data,im.ctypes.data,len(re),C.byref(a))
        return ok,a
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
    for angle in [-2.4,-.7,0,.4,1.8]:
        for gain in [.01,1,5000]:
            for prefix in [0,1]:
                signal=np.exp(-1j*labels.flatten()*np.pi/2)*gain*np.exp(1j*angle)
                if prefix:signal=np.r_[signal[0],signal]
                ok,acq=acquire_samples(signal)
                assert ok and acq.pair_alignment==prefix and acq.offset==0
                assert abs(acq.gain/gain-1)<1e-12 and acq.coherence>.999
                assert abs(np.sin(4*(acq.phase-angle)))<1e-12
    damaged=np.exp(-1j*labels.flatten()*np.pi/2)*np.exp(.37j)*12
    damaged[300:-300:202]*=np.exp(1j*np.deg2rad(55))
    ok,acq=acquire_samples(damaged)
    assert ok and acq.offset==0 and acq.pair_alignment==0
    for bad in [np.zeros(4000,dtype=complex),np.full(4000,np.nan,dtype=complex),np.ones(4000,dtype=complex),rng.normal(size=4000)+1j*rng.normal(size=4000),np.ones(1792,dtype=complex)]:
        assert not acquire_samples(bad)[0],'invalid or ambiguous carrier acquired'
    assert total_hard>0
    print('PASS: all16 initial states, superframe inversions,64-pair ring wrap, clean data and',total_hard,'controlled hard-decision errors corrected; automatic superframe sync and rejection guards')
