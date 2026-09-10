#!/usr/bin/env python3
"""Independent V.34 Figure 5/9, Table 13 and shell-ordering test for 14400."""
import ctypes as C
from pathlib import Path
import random
import subprocess
import tempfile
from v90_shell_reference import ShellReference
root=Path(__file__).resolve().parents[2]
converter=[[0,0,1,1,8,8,9,9],[3,2,2,3,11,10,10,11],
 [5,5,4,4,13,13,12,12],[6,7,7,6,14,15,15,14],
 [8,8,9,9,0,0,1,1],[11,10,10,11,3,2,2,3],
 [13,13,12,12,5,5,4,4],[14,15,15,14,6,7,7,6]]
points=[z*(-1j)**q for z in [1+1j,-3+1j,1-3j,-3-3j,1+5j,5+1j,-3+5j,5-3j] for q in range(4)]
def subset(z):
    x=((int(z.real)+3)//2)&3;y=((int(z.imag)+3)//2)&3
    return ((x^y)&1)|((x&1)<<1)|((((x>>1)^(y>>1)^x^y)&1)<<2)
pattern=[int(x) for x in '01110111111110'];rng=random.Random(1440032)
oracle=ShellReference(8)
indices=sorted({0,2**24-1,*oracle.starts,*[x-1 for x in oracle.starts if x],*[rng.randrange(2**24) for _ in range(4000)]})
with tempfile.TemporaryDirectory() as td:
    d=Path(td);w=d/'w.c';so=d/'x.so'
    w.write_text('''#include <stdlib.h>
#include "v90qam8.h"
void*trellis(void){V90Trellis*s=malloc(sizeof(*s));v90_trellis_init(s);return s;}
void*frames(unsigned previous){V90Qam32Frames*s=malloc(sizeof(*s));v90_qam32_frames_init(s,previous);return s;}
void destroy(void*s){free(s);}
unsigned long long count(V90Trellis*s){return s->pairs;}
''')
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC','-I'+str(root/'vendor/linmodem'),str(w),*[str(root/'vendor/linmodem'/n) for n in ['v90trellis.c','v90qam8.c','v90equalizer.c','v90shell.c']],'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.trellis.restype=lib.frames.restype=C.c_void_p;lib.frames.argtypes=[C.c_uint];lib.destroy.argtypes=[C.c_void_p]
    lib.count.argtypes=[C.c_void_p];lib.count.restype=C.c_ulonglong
    lib.v90_qam32_frame.argtypes=lib.v90_qam20_frame.argtypes=[C.c_void_p,C.POINTER(C.c_uint8),C.POINTER(C.c_uint8)]
    lib.v90_trellis_qam32_pair.argtypes=[C.c_void_p,*([C.c_double]*4),C.c_uint,C.POINTER(C.c_uint),C.POINTER(C.c_uint)]
    f=lib.frames(0);previous=0;seen=set()
    for index in indices:
        shell=oracle[index];bits=[(index>>i)&1 for i in range(24)]+[rng.randrange(2) for _ in range(12)];labels=[]
        for p in range(4):
            a=(previous+bits[25+3*p]+2*bits[26+3*p])%4
            b=(a+2*bits[24+3*p]+rng.randrange(2))%4;previous=a
            labels.extend([a+4*shell[2*p],b+4*shell[2*p+1]])
        seen.update(labels);out=(C.c_uint8*36)()
        assert lib.v90_qam32_frame(f,(C.c_uint8*8)(*labels),out) and list(out)==bits,index
    assert seen==set(range(32))
    out=(C.c_uint8*36)(*([99]*36));assert not lib.v90_qam32_frame(f,(C.c_uint8*8)(32,0,0,0,0,0,0,0),out) and list(out)==[99]*36
    small=(C.c_uint8*30)(*([99]*30));assert not lib.v90_qam20_frame(f,(C.c_uint8*8)(),small) and list(small)==[99]*30
    lib.destroy(f)
    for initial in range(16):
        encoder=initial;pairs=[];invbits=[]
        for n in range(1600):
            inv=pattern[(n//32)%14] if n%32==0 else 0
            a=rng.randrange(4);b=(a+2*rng.randrange(2)+((encoder&1)^inv))%4
            a+=4*rng.randrange(8);b+=4*rng.randrange(8);pairs.append((a,b));invbits.append(inv)
            y=converter[subset(points[a])][subset(points[b])];u=encoder&1
            encoder=(encoder>>1)^(y&1)^(((y>>1)&1)<<1)^((((y>>1)&1)^u)<<2)^(u<<3)
        for noisy in [False,True]:
            s=lib.trellis();decoded=[]
            for (a,b),inv in zip(pairs,invbits):
                x,y=points[a],points[b]
                if noisy:x+=complex(rng.gauss(0,.08),rng.gauss(0,.08));y+=complex(rng.gauss(0,.08),rng.gauss(0,.08))
                aa=C.c_uint(99);bb=C.c_uint(99);count=lib.count(s)
                for bad in [float('nan'),float('inf'),1e200]:
                    assert lib.v90_trellis_qam32_pair(s,bad,0,0,0,inv,C.byref(aa),C.byref(bb))==-1 and lib.count(s)==count and aa.value==bb.value==99
                if lib.v90_trellis_qam32_pair(s,x.real,x.imag,y.real,y.imag,inv,C.byref(aa),C.byref(bb)):decoded.append((aa.value,bb.value))
            assert decoded[64:]==pairs[64:len(decoded)],(initial,noisy)
            lib.destroy(s)
print('PASS: 32 points, all trellis states, noise, 36-bit shell/differential mapping, all energy-bucket boundaries and rejection bounds')
