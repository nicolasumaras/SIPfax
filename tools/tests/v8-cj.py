#!/usr/bin/env python3
"""Exercise coherent CJ acquisition on waveforms and a missed hardware CJ."""
import ctypes as C
import pathlib, subprocess, tempfile, sys
import numpy as np
root=pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as temp:
    p=pathlib.Path(temp);(p/'wrap.c').write_text('''#include <stdlib.h>
#include "v8cj.h"
long scan(const int16_t *x,unsigned n){V8Cj s;v8_cj_init(&s);
for(unsigned i=0;i<n;i+=160)v8_cj_receive(&s,x+i,n-i<160?n-i:160);
return s.found_at;}
''')
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror',
        '-I'+str(root/'vendor/linmodem'),str(p/'wrap.c'),
        str(root/'vendor/linmodem/v8cj.c'),'-lm','-o',str(p/'cj.so')],check=True)
    lib=C.CDLL(str(p/'cj.so'));lib.scan.restype=C.c_long
    lib.scan.argtypes=[np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS'),C.c_uint]
    def scan(x):
        x=np.asarray(x,dtype=np.int16);return lib.scan(x,len(x))
    rng=np.random.default_rng(18)
    def signal(bits,offset=0,frequency=0):
        t=np.arange(int(len(bits)*8000/300))/8000
        index=np.minimum((t*300).astype(int),len(bits)-1)
        f=np.where(np.asarray(bits)[index],980,1180)+frequency
        y=2400*np.cos(np.cumsum(2*np.pi*f/8000))
        return np.r_[np.zeros(offset),y+rng.normal(0,50,len(y))]
    for offset in range(27):
        for frequency in [-5,0,5]:
            x=signal([1]*12+([0]*9+[1])*3,offset,frequency)
            detected=scan(x)
            assert len(x)-35<=detected<=len(x),(offset,frequency,detected)
    assert scan(signal([1]*12+([0]*9+[1])*2+[1]*15))==-1
    for frequency in [980,1180,1800,2100,2400]:
        assert scan(2500*np.cos(2*np.pi*frequency*np.arange(8000)/8000))==-1
    assert scan(rng.normal(0,1000,8000))==-1
    source=sys.argv[1] if len(sys.argv)>1 else root/'test/fixtures/v8-cj-7716.s16'
    offset=0 if len(sys.argv)>1 else 4.4
    x=np.fromfile(source,dtype='<i2')[:6*8000]
    detected=scan(x)
    assert (4.734-offset)*8000<detected<(4.740-offset)*8000,detected
    assert scan(x[:int((4.70-offset)*8000)])==-1
    print('Hardware CJ detected at',detected/8000+offset,'s (legacy fallback:5.42s)')
    print('PASS:81 phase/frequency cases; two-octet, tone and noise rejection')
