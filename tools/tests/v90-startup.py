#!/usr/bin/env python3
"""Independent wire-format checks for the V.90 capability exchange."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import sys
import numpy as np

root = Path(__file__).resolve().parents[2]
def crc(bits):
    # Polynomial long division, deliberately separate from the C LSB implementation.
    register = 0xffff
    for bit in bits:
        top = (register >> 15) ^ int(bit)
        register = (register << 1) & 0xffff
        if top: register ^= 0x1021
    return [(register >> (15-i)) & 1 for i in range(16)]

def info0a():
    bits = [1]*4 + [0,1,1,1,0,0,1,0] + [1]*8 + [0]*9
    return bits + crc(bits[12:29]) + [1]*4

with tempfile.TemporaryDirectory() as tmp:
    wrapper = Path(tmp)/'wrapper.c'
    wrapper.write_text('''#include <stdlib.h>
#include "v90startup.h"
void *create(int law) { V90Startup *s=malloc(sizeof(*s)); v90_startup_init(s,law); return s; }
int received(V90Startup *s) { return s->info0_received; }
void destroy(void *s) { free(s); }
''')
    libpath = Path(tmp)/'test.so'
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror',
                    '-I'+str(root/'vendor/linmodem'), str(wrapper),
                    str(root/'vendor/linmodem/v90startup.c'),'-lm','-o',str(libpath)],check=True)
    lib=C.CDLL(str(libpath)); lib.create.argtypes=[C.c_int];lib.create.restype=C.c_void_p
    lib.destroy.argtypes=[C.c_void_p];lib.received.argtypes=[C.c_void_p]
    ptr=np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS')
    lib.v90_startup_process.argtypes=[C.c_void_p,ptr,ptr,C.c_int]
    lib.v90_startup_history.argtypes=[C.c_void_p,ptr,C.c_int]
    lib.v90_info0d.argtypes=[C.POINTER(C.c_ubyte),C.c_int]
    for law in [0,1]:
        b=(C.c_ubyte*62)();lib.v90_info0d(b,law);b=list(b)
        assert b[:12]==[1]*4+[0,1,1,1,0,0,1,0]
        assert b[39]==law and b[42:58]==crc(b[12:42]) and b[58:]==[1]*4
        s=lib.create(law); out=np.zeros(1600,dtype=np.int16);sil=out.copy()
        lib.v90_startup_process(s,out,sil,len(out));lib.destroy(s)
        # Decode each emitted symbol with a coherent 1200Hz correlator.
        z=out*np.exp(-2j*np.pi*1200*np.arange(len(out))/8000)
        sums=np.array([z[int(np.ceil(k*40/3)):int(np.ceil((k+1)*40/3))].sum() for k in range(64)])
        wire=(np.real(sums[1:]*sums[:-1].conj())<0).astype(int).tolist()
        assert wire[:62]==b, 'transmitted DPSK differs from the message'
    rng=np.random.default_rng(902)
    for offset in range(14):
        for damaged in [False,True]:
            b=info0a()
            if damaged:b[18]^=1
            signs=np.r_[1,(-1.)**np.cumsum(b)]
            t=np.arange(1200); idx=np.clip(((t-offset)*3//40),0,len(signs)-1)
            pcm=2500*signs[idx]*np.cos(2*np.pi*2400*t/8000+0.73)
            pcm+=1000*np.cos(2*np.pi*1800*t/8000)+rng.normal(0,15,len(t))
            pcm[:offset]=0;pcm=pcm.astype(np.int16);out=np.zeros_like(pcm)
            s=lib.create(0)
            history = 640 if offset % 2 else 0
            if history: lib.v90_startup_history(s,pcm[:history],history)
            for start in range(history,len(pcm),37):
                a=pcm[start:start+37];o=out[start:start+37]
                lib.v90_startup_process(s,o,a,len(a))
            assert bool(lib.received(s)) != damaged, (offset,damaged)
            lib.destroy(s)
    print('PASS: both PCM laws, wire DPSK/CRC, 14 receive phases with guard/noise, invalid CRC rejected')

    if len(sys.argv) > 1:
        pcm=np.fromfile(sys.argv[1],dtype='<i2')
        s=lib.create(0);out=np.zeros_like(pcm)
        for start in range(0,len(pcm),160):
            a=pcm[start:start+160];o=out[start:start+160]
            lib.v90_startup_process(s,o,a,len(a))
        assert lib.received(s), 'no valid INFO0a in hardware recording'
        lib.destroy(s)
        print('PASS: CRC-valid INFO0a from hardware recording')
