#!/usr/bin/env python3
"""Independent INFO0a wire input and per-call INFO1d/MP/rate-reset checks."""
import ctypes as C
import os
from pathlib import Path
import subprocess
import tempfile
import numpy as np
root=Path(__file__).resolve().parents[2]
def crc(bits):
    register=0xffff
    for bit in bits:
        top=(register>>15)^int(bit)
        register=(register<<1)&0xffff
        if top:register^=0x1021
    return [(register>>(15-i))&1 for i in range(16)]
with tempfile.TemporaryDirectory() as td:
    d=Path(td);w=d/'w.c';libpath=d/'test.so'
    w.write_text('''#include <stdlib.h>
#include <string.h>
#include "v90startup.c"
#include "v90phase4.c"
void *create(void){V90Startup*s=malloc(sizeof(*s));v90_startup_init(s,0);return s;}
void destroy(void*s){free(s);}
unsigned received(V90Startup*s){return s->info0_received;}
unsigned selected(V90Startup*s){return s->upstream_data_rate;}
void info(V90Startup*s,unsigned char*b){memcpy(b,s->info1d,109);}
void phase4(V90Startup*s){v90_phase4_init_rate(&s->phase4,0,78,s->upstream_data_rate);}
unsigned receiver(V90Startup*s){return s->phase4.upstream.rate;}
void mp(V90Startup*s,unsigned char*b){mp_build(&s->phase4);memcpy(b,s->phase4.mp,102);}
void receive_e(V90Startup*s){s->phase4.rx.e_seen=1;v90_phase4_next(&s->phase4,0);}
void retrain(V90Startup*s){begin_retrain(s,"rate selection test");}
unsigned explicit_rate(unsigned rate){V90Upstream*s=malloc(sizeof(*s));v90_upstream_init_rate(s,rate);unsigned got=s->rate;free(s);return got;}
''')
    files=['v90training.c','v90dil.c','v90cp.c','v90pcm.c','v90upstream.c','v90trellis.c','v90qam8.c','v90equalizer.c','v90shell.c','v90mapping.c','v90train_tx.c']
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'vendor/linmodem'),str(w),*[str(root/'vendor/linmodem'/p) for p in files],'-lm','-o',str(libpath)],check=True)
    lib=C.CDLL(str(libpath));lib.create.restype=C.c_void_p
    for name in ['destroy','received','selected','phase4','receiver','receive_e','retrain']:getattr(lib,name).argtypes=[C.c_void_p]
    lib.info.argtypes=lib.mp.argtypes=[C.c_void_p,C.POINTER(C.c_ubyte)]
    pcmtype=np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS')
    lib.v90_startup_process.argtypes=[C.c_void_p,pcmtype,pcmtype,C.c_int]
    lib.explicit_rate.argtypes=[C.c_uint]
    for invalid in [0,1,4799,5000,33600,0xffffffff]:assert lib.explicit_rate(invalid)==4800
    settings=[(None,4800),('invalid',4800)]+[(str(x),x) for x in range(4800,31201,2400)]
    for setting,maximum in settings:
        for large in [0,1]:
            for damaged in [False,True]:
                if setting is None:os.environ.pop('SIPFAX_V90_UPSTREAM_RATE',None)
                else:os.environ['SIPFAX_V90_UPSTREAM_RATE']=setting
                b=[1]*4+[0,1,1,1,0,0,1,0]+[1]*8+[0]*9
                b[25]=large;b+=crc(b[12:29])+[1]*4
                if damaged:b[25]^=1
                t=np.arange(1200);signs=np.r_[1,(-1.)**np.cumsum(b)]
                idx=np.clip((t-3)*3//40,0,len(signs)-1)
                pcm=np.rint(2500*signs[idx]*np.cos(2*np.pi*2400*t/8000+.73)).astype(np.int16)
                pcm[:3]=0;out=np.zeros_like(pcm);s=lib.create()
                try:
                    # Capture configuration once; subsequent resets must not reread it.
                    os.environ['SIPFAX_V90_UPSTREAM_RATE']='4800'
                    for start in range(0,len(pcm),37):
                        x=pcm[start:start+37];lib.v90_startup_process(s,out[start:start+37],x,len(x))
                    assert lib.received(s)==int(not damaged)
                    expected=maximum if large and not damaged else min(maximum,28800)
                    assert lib.selected(s)==expected
                    for repeat in range(2):
                        bits=(C.c_ubyte*109)();lib.info(s,bits);bits=list(bits)
                        assert bits[89:105]==crc(bits[12:89])
                        assert bits[61]==1 and sum(bits[66+k]<<k for k in range(4))==expected//2400
                        assert not any(bits[25:52]) and not any(bits[70:79])
                        assert sum(bits[57+k]<<k for k in range(4))==(min(expected,28800)//2400 if not damaged else 0)
                        lib.phase4(s);assert lib.receiver(s)==expected
                        mp=(C.c_ubyte*102)();lib.mp(s,mp);mp=list(mp)
                        assert sum(mp[24+k]<<k for k in range(4))==expected//2400
                        assert mp[36:50]==[int(i==expected//2400-2) for i in range(14)]
                        assert mp[69:85]==crc([mp[i] for i in range(18,69) if i%17])
                        lib.receive_e(s);assert lib.receiver(s)==expected
                        lib.retrain(s);assert lib.selected(s)==expected
                finally:lib.destroy(s)
print('PASS: CRC-protected peer capability, all configured rates, INFO1d/MP consistency, peer fallback, E/retrain preservation and invalid rates')
