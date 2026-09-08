#!/usr/bin/env python3
"""Decode native PCM output independently across shaping modes and lookahead."""
import ctypes as C
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    wrapper=Path(tmp)/'wrap.c';so=Path(tmp)/'pcm.so'
    wrapper.write_text('''#include <stdlib.h>
#include "v90pcm.h"
void *create(unsigned sr,unsigned ld,unsigned law){
 V90Pcm *s=malloc(sizeof(*s));V90Cp cp={0};
 cp.sr=sr;cp.lookahead=ld;cp.alaw=law;cp.drn=12+6-sr-8;
 cp.filter[0]=63;cp.count=1;
 for(unsigned u=0;u<4;++u)cp.mask[0][0][(unsigned[]){53,78,88,96}[u]]=1;
 if(v90_pcm_init(s,&cp,0,0)){free(s);return 0;}return s;
}
''')
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror','-I'+str(root/'vendor/linmodem'),str(wrapper),str(root/'vendor/linmodem/v90pcm.c'),'-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.create.restype=C.c_void_p;lib.create.argtypes=[C.c_uint]*3
    lib.v90_pcm_frame.argtypes=[C.c_void_p,C.POINTER(C.c_int16)]
    for sr in range(4):
      for ld in range(4):
       for law in range(2):
        s=lib.create(sr,ld,law);assert s
        def level(u):
            seg,p=divmod(u,16)
            return ((p*8+132)<<seg)-132 if not law else ((p*16+264)<<(seg-1)) if seg else p*16+8
        levels=[level(u) for u in [96,88,78,53]];out=(C.c_int16*6)();bits=[];last=0;odd=0;q=0;tprev=0
        first=None
        for frame in range(400):
            lib.v90_pcm_frame(s,out);x=list(out)
            if first is None:first=[abs(v) for v in x]
            signs=[int(v>0) for v in x];sig=[]
            if sr==0:
                for v in signs:sig.append(v^last);last=v
            else:
                width=6//sr
                for j in range(sr):
                    t=sum(signs[j*width+k]<<k for k in range(width));p=t^tprev
                    nextq=(p&1)^q;p^=[0,0x55,0xff,0xaa][(nextq<<1)|q];q=nextq;tprev=t
                    assert p&1==0
                    for k in range(1,width):
                        b=(p>>k)&1
                        if k&1:sig.append(b^odd);odd=b
                        else:sig.append(b)
            value=sum(levels.index(abs(v))<<(2*j) for j,v in enumerate(x))
            bits.extend(sig+[(value>>k)&1 for k in range(12)])
        decoded=[b^(bits[i-18] if i>=18 else 0)^(bits[i-23] if i>=23 else 0) for i,b in enumerate(bits)]
        assert decoded==[1]*len(decoded),(sr,ld,law,next(i for i,b in enumerate(decoded) if b!=1))
        assert first==[levels[3]]*6,first
    print('PASS: 32 law/redundancy/lookahead combinations recover every scrambled training bit; no lost first frame')
