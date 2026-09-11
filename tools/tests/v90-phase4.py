#!/usr/bin/env python3
"""Replay hardware CPt and independently decode emitted TRN2d/MP."""
import ctypes as C
import os
from pathlib import Path
import subprocess,tempfile,sys
import numpy as np
root=Path(__file__).resolve().parents[2]
training_frames=340
if '--long-training' in sys.argv:
    os.environ['SIPFAX_V90_INITIAL_TRN2D_MS']='1500'
    training_frames=2000
with tempfile.TemporaryDirectory() as tmp:
    wrapper=Path(tmp)/'wrap.c';so=Path(tmp)/'p4.so'
    wrapper.write_text('''#include <stdlib.h>
#include "v90phase4.h"
void *create(void){V90Phase4 *s=malloc(sizeof(*s));v90_phase4_init(s,0,78);return s;}
unsigned configured(const char *value){if(value)setenv("SIPFAX_V90_INITIAL_TRN2D_MS",value,1);else unsetenv("SIPFAX_V90_INITIAL_TRN2D_MS");V90Phase4 s;v90_phase4_init(&s,0,78);return s.trn_frames;}
unsigned start(V90Phase4 *s){return s->trn_start;}
unsigned length(V90Phase4 *s){return s->mp_length;}
unsigned data_start(V90Phase4 *s){return s->data_start;}
unsigned ed(V90Phase4 *s){return s->ed_frame;}
unsigned level(V90Phase4 *s,unsigned frame,unsigned index){return v90_pcm_level(s->cp.alaw,s->encoder.map[frame][index]);}
unsigned size(V90Phase4 *s,unsigned frame){return s->encoder.m[frame];}
unsigned k(V90Phase4 *s){return s->encoder.k;}
''')
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror','-I'+str(root/'vendor/linmodem'),str(wrapper),*[str(root/'vendor/linmodem'/n) for n in ['v90upstream.c','v90trellis.c','v90qam8.c','v90equalizer.c','v90shell.c','v90phase4.c','v90pcm.c','v90cp.c','v90dil.c','v90training.c']],'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.create.restype=C.c_void_p
    lib.configured.argtypes=[C.c_char_p]
    for value,expected in [(None,340),(b'255',340),(b'1500',2000),(b'2000',2666),(b'254',340),(b'2001',340),(b'-1',340),(b'junk',340),(b'999999999999999999999999999',340)]:
        assert lib.configured(value)==expected
    lib.configured(b'1500' if '--long-training' in sys.argv else None)
    for name in ['start','length','data_start','ed','k']:getattr(lib,name).argtypes=[C.c_void_p]
    lib.v90_phase4_next.argtypes=[C.c_void_p,C.c_int16];lib.v90_phase4_next.restype=C.c_int16
    complete='--complete' in sys.argv;pcm=np.fromfile(sys.argv[1],dtype='<i2')[(16 if complete else 17)*8000:(24 if complete else 19)*8000];s=lib.create()
    x=[lib.v90_phase4_next(s,int(v)) for v in pcm];start=lib.start(s);length=lib.length(s)
    assert start>=216 and start%6==0 and length==102
    ri=[3772]*3+[-3772]*3
    assert x[:start-24]==ri*((start-24)//6)
    assert x[start-24:start]==[-v for v in ri]*4
    levels=[((u%16*8+132)<<(u//16))-132 for u in [96,88,78,53]]
    decoded=[];odd=q=prev=0
    finish=lib.data_start(s) if complete else len(x)
    for j in range(start,finish-5,6):
        frame=x[j:j+6];t=sum((v>0)<<k for k,v in enumerate(frame))
        p=t^prev;nextq=(p&1)^q;p^=[0,0x55,0xff,0xaa][(nextq<<1)|q];q=nextq;prev=t
        sig=[]
        for k in range(1,6):
            b=(p>>k)&1
            if k&1:sig.append(b^odd);odd=b
            else:sig.append(b)
        v=sum(levels.index(abs(v))<<(2*k) for k,v in enumerate(frame))
        decoded.extend(sig+[(v>>k)&1 for k in range(12)])
    plain=[b^(decoded[j-18] if j>=18 else 0)^(decoded[j-23] if j>=23 else 0) for j,b in enumerate(decoded)]
    assert plain[:training_frames*17]==[1]*(training_frames*17)
    mp=plain[training_frames*17:training_frames*17+102]
    assert mp[:17]==[1]*17 and all(mp[k]==0 for k in [17,34,51,68,*range(85,102)])
    rate={'7200':3,'9600':4,'12000':5,'14400':6,'16800':7,'19200':8,'21600':9,'24000':10,'26400':11,'28800':12,'31200':13}.get(os.environ.get('SIPFAX_V90_UPSTREAM_RATE'),2)
    assert sum(mp[24+k]<<k for k in range(4))==rate and mp[34+rate]==1 and mp[33]==0
    crc=0xffff
    for k in range(18,69):
        if k%17==0:continue
        top=(crc>>15)^mp[k];crc=(crc<<1)&0xffff
        if top:crc^=0x1021
    assert mp[69:85]==[(crc>>(15-k))&1 for k in range(16)]
    assert plain[training_frames*17+102:training_frames*17+204]==mp
    if complete:
        data_start=lib.data_start(s);ed=lib.ed(s);assert data_start and ed
        assert data_start==start+(ed+2)*6
        assert plain[ed*17:(ed+2)*17]==[0]*34
        lastmp=plain[ed*17-102:ed*17];assert lastmp[33]==1
        lib.level.argtypes=[C.c_void_p,C.c_uint,C.c_uint];lib.size.argtypes=[C.c_void_p,C.c_uint]
        maps=[[lib.level(s,j,u) for u in range(lib.size(s,j))] for j in range(6)];K=lib.k(s);assert K in [35,37]
        bits=[];odd=q=prev=0
        for j in range(data_start,min(data_start+288,len(x)),6):
            frame=x[j:j+6];t=sum((v>0)<<k for k,v in enumerate(frame));p=t^prev
            nextq=(p&1)^q;p^=[0,0x55,0xff,0xaa][(nextq<<1)|q];q=nextq;prev=t
            sig=[]
            for k in range(1,6):
                b=(p>>k)&1
                if k&1:sig.append(b^odd);odd=b
                else:sig.append(b)
            value=0
            for k in range(5,-1,-1):value=value*len(maps[k])+maps[k].index(abs(frame[k]))
            bits.extend(sig+[(value>>k)&1 for k in range(K)])
        b1=[b^(bits[j-18] if j>=18 else 0)^(bits[j-23] if j>=23 else 0) for j,b in enumerate(bits)]
        assert b1==[1]*(48*(K+5))
        print('PASS: complete MP-prime, two Ed frames, and 48 B1d frames using actual data-mode CP constellations')
    print('PASS: live CPt configures Ri/Ri-bar, configured TRN2d symbols and repeated independently decoded MP/CRC')
