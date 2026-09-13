#!/usr/bin/env python3
"""Half-symbol alignment, independent channel recovery, and atomic rejection."""
import sys
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import numpy as np
symbols=120 if "--120" in sys.argv else 128
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as td:
    d=Path(td);w=d/'wrapper.c';so=d/'eq.so'
    w.write_text('''#include <stdlib.h>
#include <string.h>
#include "v90equalizer.h"
void*create(void){V90Equalizer*s=malloc(sizeof(*s));v90_equalizer_init_taps(s,V90_EQ_HALF_TAPS);return s;}
void*clone(void*s){void*t=malloc(sizeof(V90Equalizer));memcpy(t,s,sizeof(V90Equalizer));return t;}
int same(void*a,void*b){return !memcmp(a,b,sizeof(V90Equalizer));}
void destroy(void*s){free(s);}
''')
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC','-I'+str(root/'vendor/linmodem'),str(w),str(root/'vendor/linmodem/v90equalizer.c'),'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.create.restype=C.c_void_p
    array=np.ctypeslib.ndpointer(dtype=np.float64,flags='C_CONTIGUOUS')
    for name in ['v90_equalizer_train','v90_equalizer_train_half']:getattr(lib,name).argtypes=[C.c_void_p,array,array,array,array]
    lib.v90_equalizer_train_symbols.argtypes=[C.c_void_p,array,array,array,array,C.c_uint,C.c_uint]
    lib.v90_equalizer_symbol.argtypes=[C.c_void_p,C.c_double,C.c_double,C.POINTER(C.c_double),C.POINTER(C.c_double)]
    lib.v90_equalizer_adapt.argtypes=[C.c_void_p,C.c_double,C.c_double,C.c_double]
    lib.v90_equalizer_init_taps.argtypes=[C.c_void_p,C.c_uint]
    lib.clone.argtypes=[C.c_void_p];lib.clone.restype=C.c_void_p
    lib.same.argtypes=[C.c_void_p,C.c_void_p];lib.destroy.argtypes=[C.c_void_p]
    def train(s,x,y,half=True):
        arrays=[np.ascontiguousarray(z) for z in [x.real,x.imag,y.real,y.imag]]
        if symbols==120:return lib.v90_equalizer_train_symbols(s,*arrays,symbols,2 if half else 1)
        clone=lib.clone(s)
        old=getattr(lib,'v90_equalizer_train_half' if half else 'v90_equalizer_train')(s,*arrays)
        new=lib.v90_equalizer_train_symbols(clone,*arrays,128,2 if half else 1)
        assert old==new and lib.same(s,clone)
        lib.destroy(clone);return old
    def run(s,x):
        out=[]
        for n,z in enumerate(x):
            r=C.c_double(999);i=C.c_double(999)
            ready=lib.v90_equalizer_symbol(s,z.real,z.imag,C.byref(r),C.byref(i))
            assert ready==(n>=14)
            if ready and n%2:out.append(r.value+1j*i.value)
            if not ready:assert r.value==i.value==999
        return np.array(out)
    rng=np.random.default_rng(2640029)
    desired=rng.choice([-7,-5,-3,-1,1,3,5,7],4096)+1j*rng.choice([-7,-5,-3,-1,1,3,5,7],4096)
    samples=np.empty(2*len(desired),complex);samples[1::2]=desired
    samples[0::2]=.5*(np.r_[0,desired[:-1]]+desired)
    s=lib.create();assert np.array_equal(run(s,samples),desired[:-7]);lib.destroy(s)
    for channel in [np.array([.2-.08j,1,-.15j]),np.array([.12j,.1,1,-.14-.05j,.08])]:
        distorted=np.convolve(samples,channel,'same')
        s=lib.create();assert train(s,distorted[:2*symbols],desired[:symbols])
        corrected=run(s,distorted)
        before=np.mean(abs(distorted[1::2][symbols:-7]-desired[symbols:-7])**2)
        after=np.mean(abs(corrected[symbols:]-desired[symbols:-7])**2)
        assert after<.02*before,(before,after)
        old=lib.clone(s)
        bad=desired[:symbols].copy();bad[87:]*=-1
        assert not train(s,distorted[:2*symbols],bad) and lib.same(s,old)
        assert not train(s,distorted[:symbols],desired[:symbols],False) and lib.same(s,old)
        for invalid in [float('nan'),float('inf'),1e100]:
            broken=distorted[:2*symbols].copy();broken[2*symbols-1]=invalid
            assert not train(s,broken,desired[:symbols]) and lib.same(s,old)
            r=C.c_double(999);i=C.c_double(999)
            assert lib.v90_equalizer_symbol(s,invalid,0,C.byref(r),C.byref(i))==-1
            assert r.value==i.value==999 and lib.same(s,old)
        for step in [0,-1,.2001,float('nan'),float('inf')]:
            assert not lib.v90_equalizer_adapt(s,1,1,step) and lib.same(s,old)
        assert lib.v90_equalizer_adapt(s,1,1,.2)
        lib.destroy(old);lib.destroy(s)
    # Half training must reject a baud-spaced state before indexing 256 inputs.
    s=lib.create();assert lib.v90_equalizer_init_taps(s,15);old=lib.clone(s)
    assert not train(s,samples[:symbols],desired[:symbols]) and lib.same(s,old)
    lib.destroy(old);lib.destroy(s)
print('PASS:',symbols,'symbols; half-symbol delay, held-out channel recovery, mode checks and unchanged state on rejected input/updates')
