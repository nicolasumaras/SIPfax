#!/usr/bin/env python3
"""Independent distorted constellation recovery and held-out training checks."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import sys
import numpy as np
taps=15 if "--long" in sys.argv else 7
delay=(taps-1)//2
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as td:
    d=Path(td);w=d/'w.c';so=d/'eq.so'
    w.write_text('''#include <stdlib.h>
#include <string.h>
#include "v90equalizer.h"
void*create(void){V90Equalizer*s=malloc(sizeof(*s));v90_equalizer_init(s);return s;}
void*clone(void*s){void*t=malloc(sizeof(V90Equalizer));memcpy(t,s,sizeof(V90Equalizer));return t;}
int same(void*a,void*b){return !memcmp(a,b,sizeof(V90Equalizer));}
void destroy(void*s){free(s);}
unsigned long long count(V90Equalizer*s){return s->samples;}
''')
    w.write_text(w.read_text().replace('v90_equalizer_init(s);','v90_equalizer_init_taps(s,%d);'%taps))
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC','-I'+str(root/'vendor/linmodem'),str(w),str(root/'vendor/linmodem/v90equalizer.c'),'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.create.restype=C.c_void_p;lib.destroy.argtypes=[C.c_void_p]
    array=np.ctypeslib.ndpointer(dtype=np.float64,flags='C_CONTIGUOUS')
    lib.v90_equalizer_train.argtypes=[C.c_void_p,array,array,array,array]
    lib.v90_equalizer_symbol.argtypes=[C.c_void_p,C.c_double,C.c_double,C.POINTER(C.c_double),C.POINTER(C.c_double)]
    lib.v90_equalizer_adapt.argtypes=[C.c_void_p,C.c_double,C.c_double,C.c_double]
    lib.v90_equalizer_init_taps.argtypes=[C.c_void_p,C.c_uint]
    lib.clone.argtypes=[C.c_void_p];lib.clone.restype=C.c_void_p
    lib.same.argtypes=[C.c_void_p,C.c_void_p]
    lib.count.argtypes=[C.c_void_p];lib.count.restype=C.c_ulonglong
    def train(s,x,y):
        return lib.v90_equalizer_train(s,*[np.ascontiguousarray(z) for z in [x.real,x.imag,y.real,y.imag]])
    def run(s,x):
        values=[]
        for v in x:
            r=C.c_double(999);i=C.c_double(999)
            ready=lib.v90_equalizer_symbol(s,v.real,v.imag,C.byref(r),C.byref(i))
            assert ready>=0
            if ready:values.append(r.value+1j*i.value)
            else:assert r.value==i.value==999
        return np.array(values)
    rng=np.random.default_rng(120007)
    points=np.array([z*(-1j)**q for z in [1+1j,-3+1j,1-3j,-3-3j,1+5j] for q in range(4)])
    desired=rng.choice(points,4096)
    s=lib.create();identity=run(s,desired)
    assert np.array_equal(identity,desired[:-delay])
    old=lib.clone(s)
    for invalid in [0,1,6,8,14,16,2**32-1]:
        assert not lib.v90_equalizer_init_taps(s,invalid) and lib.same(s,old)
    lib.destroy(old)
    count=lib.count(s)
    for v in [float('nan'),float('inf'),1e100]:
        r=C.c_double(999);i=C.c_double(999)
        assert lib.v90_equalizer_symbol(s,v,0,C.byref(r),C.byref(i))==-1
        assert lib.count(s)==count and r.value==i.value==999
    lib.destroy(s)
    for channel in [np.array([.13-.08j,1,-.12-.04j]),np.array([.06+.07j,-.13j,1,.1-.05j,-.04j])]:
        noisy=np.convolve(desired,channel,'same')+rng.normal(0,.015,len(desired))+1j*rng.normal(0,.015,len(desired))
        s=lib.create();assert train(s,noisy[:128],desired[:128])
        corrected=run(s,noisy)
        before=np.mean(abs(noisy[128:-delay]-desired[128:-delay])**2)
        after=np.mean(abs(corrected[128:]-desired[128:-delay])**2)
        assert after<.05*before,(before,after)
        nearest=np.argmin(abs(corrected[128:,None]-points[None,:]),axis=1)
        assert np.array_equal(points[nearest],desired[128:-delay])
        # Reject a fit that sees correct training but contradictory held-out data.
        # The rejection must preserve coefficients AND streaming history.
        count=lib.count(s);bad=desired[:128].copy();bad[80+delay:]*=-1
        assert not train(s,noisy[:128],bad) and lib.count(s)==count
        lib.destroy(s)
    # A changing complex channel: train once, then adapt only confident
    # nearest-point decisions. Unseen late symbols must improve over fixed FIR.
    variation=np.linspace(0,1,len(desired))
    varying=desired+(.13-.08j+.12*variation)*np.roll(desired,1)+(-.12-.04j-.08*variation)*np.roll(desired,-1)
    results=[]
    for adaptive in [False,True]:
        s=lib.create();assert train(s,varying[:128],desired[:128]);output=[]
        for z in varying:
            r=C.c_double();i=C.c_double()
            if lib.v90_equalizer_symbol(s,z.real,z.imag,C.byref(r),C.byref(i)):
                value=r.value+1j*i.value;output.append(value)
                target=points[np.argmin(abs(points-value))]
                if adaptive and abs(target-value)**2<.25:
                    assert lib.v90_equalizer_adapt(s,target.real,target.imag,.1 if taps==15 else .01)
        results.append(np.mean(abs(np.array(output)[-1000:]-desired[:-delay][-1000:])**2))
        old=lib.clone(s)
        for step in [0,-1,.1001,float('nan'),float('inf')]:
            assert not lib.v90_equalizer_adapt(s,1,1,step) and lib.same(s,old)
        for bad in [float('nan'),float('inf'),1e100]:
            assert not lib.v90_equalizer_adapt(s,bad,1,.01) and lib.same(s,old)
        lib.destroy(old);lib.destroy(s)
    assert results[1]<.1*results[0],results
    print('PASS: guarded normalized LMS tracks a changing channel; invalid updates preserve state')
    s=lib.create();zeros=np.zeros(128,dtype=complex)
    assert not train(s,zeros,desired[:128])
    assert not train(s,desired[:128],desired[:128])
    for bad in [float('nan'),float('inf'),1e100]:
        broken=desired[:128].copy();broken[40]=bad
        assert not train(s,broken,desired[:128])
    assert np.array_equal(run(s,desired),desired[:-delay]);lib.destroy(s)
print('PASS: held-out complex FIR training, unseen distorted symbol recovery, delay, identity and rejection state preservation')
