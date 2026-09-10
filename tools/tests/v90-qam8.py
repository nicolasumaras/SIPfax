#!/usr/bin/env python3
"""Independent 8-QAM transmitter -> C trellis -> 7200-bit/s frame bits."""
import cmath
import ctypes as C
import itertools
from pathlib import Path
import random
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
# Full Table 13, transcribed independently; the 16-state encoder uses Y2/Y1.
converter=[
 [0,0,1,1,8,8,9,9],[3,2,2,3,11,10,10,11],
 [5,5,4,4,13,13,12,12],[6,7,7,6,14,15,15,14],
 [8,8,9,9,0,0,1,1],[11,10,10,11,3,2,2,3],
 [13,13,12,12,5,5,4,4],[14,15,15,14,6,7,7,6]]
rings=sorted(itertools.product(range(2),repeat=8),key=lambda r:
 (sum(r),sum(r[:4]),sum(r[4:6]),r[6],r[4],sum(r[:2]),r[2],r[0]))[:64]
pattern=[int(x) for x in '01110111111110']
rng=random.Random(907200)

def transmit(frames,state,previous):
    labels=[];inversions=[];source=[]
    for frame in range(frames):
        bits=[rng.randrange(2) for _ in range(18)];source.append(bits)
        shell=rings[sum(bits[i]<<i for i in range(6))]
        for j in range(4):
            i=4*frame+j
            inversion=pattern[(i//32)%14] if i%32==0 else 0
            a=(previous+bits[7+3*j]+2*bits[8+3*j])%4
            b=(a+2*bits[6+3*j]+((state&1)^inversion))%4
            previous=a
            x=a+4*shell[2*j];y=b+4*shell[2*j+1]
            labels.append((x,y));inversions.append(inversion)
            v=converter[x][y];u=state&1
            state=(state>>1)^(v&1)^(((v>>1)&1)<<1)^((((v>>1)&1)^u)<<2)^(u<<3)
    return labels,inversions,source

def point(label):return [1+1j,-3+1j][label>>2]*(-1j)**(label&3)

with tempfile.TemporaryDirectory() as directory:
    d=Path(directory);w=d/'wrapper.c';so=d/'qam8.so'
    w.write_text('''#include <stdlib.h>
#include "v90trellis.h"
#include "v90qam8.h"
void *trellis(void){V90Trellis*s=malloc(sizeof(*s));v90_trellis_init(s);return s;}
void *frames(unsigned previous){V90Qam8Frames*s=malloc(sizeof(*s));v90_qam8_frames_init(s,previous);return s;}
void destroy(void*s){free(s);}
uint64_t count(V90Trellis*s){return s->pairs;}
''')
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',
        '-I'+str(root/'vendor/linmodem'),str(w),
        *[str(root/'vendor/linmodem'/x) for x in ['v90trellis.c','v90qam8.c','v90shell.c']],
        '-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.trellis.restype=lib.frames.restype=C.c_void_p
    lib.frames.argtypes=[C.c_uint];lib.destroy.argtypes=[C.c_void_p]
    lib.count.argtypes=[C.c_void_p];lib.count.restype=C.c_uint64
    lib.v90_trellis_qam8_pair.argtypes=[C.c_void_p,*([C.c_double]*4),C.c_uint,C.POINTER(C.c_uint),C.POINTER(C.c_uint)]
    lib.v90_qam8_frame.argtypes=[C.c_void_p,C.POINTER(C.c_uint8),C.POINTER(C.c_uint8)]
    hard_errors=0
    for initial in range(16):
        previous=initial%4
        labels,inversions,source=transmit(180,initial,previous)
        for disturb in [False,True]:
            s=lib.trellis();decoded=[]
            for i,((a,b),inversion) in enumerate(zip(labels,inversions)):
                values=[point(a),point(b)]
                if disturb:
                    values=[v+complex(rng.gauss(0,.04),rng.gauss(0,.04)) for v in values]
                    if 100<i<len(labels)-100 and i%101==0 and a<4:
                        values[0]*=cmath.exp(1j*55*cmath.pi/180)
                    hard_errors+=sum(min(range(8),key=lambda q:abs(v-point(q)))!=x for v,x in zip(values,[a,b]))
                aa=C.c_uint(99);bb=C.c_uint(99)
                if i==150:
                    old=lib.count(s)
                    for bad in [float('nan'),float('inf'),1e200]:
                        assert lib.v90_trellis_qam8_pair(s,bad,0,0,0,0,C.byref(aa),C.byref(bb))==-1
                        assert lib.count(s)==old and aa.value==bb.value==99
                x,y=values
                result=lib.v90_trellis_qam8_pair(s,x.real,x.imag,y.real,y.imag,inversion,C.byref(aa),C.byref(bb))
                assert bool(result)==(i>=63)
                if result:decoded.append((aa.value,bb.value))
            assert decoded==labels[:len(decoded)],(initial,disturb,next((i for i,(x,y) in enumerate(zip(decoded,labels)) if x!=y),None))
            f=lib.frames(previous)
            flat=[v for pair in decoded for v in pair]
            for i in range(len(flat)//8):
                out=(C.c_uint8*18)();packet=(C.c_uint8*8)(*flat[8*i:8*i+8])
                assert lib.v90_qam8_frame(f,packet,out)
                assert list(out)==source[i],(initial,disturb,i)
            lib.destroy(f);lib.destroy(s)
    assert hard_errors>0
    # Reject an unused shell but retain differential history for the next frame.
    f=lib.frames(0);out=(C.c_uint8*18)(*([99]*18))
    bad=(C.c_uint8*8)(*([7]*8));assert not lib.v90_qam8_frame(f,bad,out)
    assert list(out)==[99]*18
    labels,_,source=transmit(1,0,3)
    good=(C.c_uint8*8)(*[x for p in labels for x in p])
    assert lib.v90_qam8_frame(f,good,out) and list(out)==source[0]
    invalid=(C.c_uint8*8)(255,0,0,0,0,0,0,0);out=(C.c_uint8*18)(*([99]*18))
    assert not lib.v90_qam8_frame(f,invalid,out) and list(out)==[99]*18
    lib.destroy(f)
print('PASS: 8-QAM soft pairs, 18-bit frames, all 16 states, noise/corrected decisions, rejection recovery')
