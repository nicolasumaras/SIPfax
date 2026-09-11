#!/usr/bin/env python3
"""Independent INFO waveforms: peer carrier masks, offers and startup propagation."""
import ctypes as C
import os
from pathlib import Path
import subprocess
import tempfile
import numpy as np
root=Path(__file__).resolve().parents[2]
def crc(bits):
    value=0xffff
    for bit in bits:
        top=(value>>15)^bit;value=(value<<1)&0xffff
        if top:value^=0x1021
    return [(value>>(15-i))&1 for i in range(16)]
def field(bits,offset,width,value):
    bits[offset:offset+width]=[(value>>i)&1 for i in range(width)]
def frame(n):return [1]*4+[0,1,1,1,0,0,1,0]+[0]*(n-12)
def wave(bits):
    t=np.arange(1200);sign=np.r_[1,(-1.)**np.cumsum(bits)]
    return np.rint(2500*sign[np.clip((t-3)*3//40,0,len(sign)-1)]*np.cos(2*np.pi*2400*t/8000+.73)).astype(np.int16)
def number(bits,start,width):return sum(bits[start+i]<<i for i in range(width))
with tempfile.TemporaryDirectory() as td:
    d=Path(td);w=d/'w.c'
    w.write_text('''#include <stdlib.h>
#include "v90startup.c"
#include "v90phase4.c"
void *create(void){V90Startup*s=malloc(sizeof(*s));v90_startup_init(s,0);return s;}
void destroy(void*s){free(s);}
void wire(V90Startup*s,const int16_t*p,int n,int info1){
 s->samples=0;if(info1){memset(s->rx,0,sizeof(s->rx));for(int j=0;j<14;++j)s->rx[j].clock=j*570;s->ranging_state=8;}
 for(int i=0;i<n;++i){receive(s,p[i]);++s->samples;}
}
void offer(V90Startup*s,unsigned char*b){memcpy(b,s->info1d,109);}
unsigned get(V90Startup*s,int k){switch(k){
 case 0:return s->info0_received;case 1:return s->training_active;
 case 2:return s->training.symbol_rate;case 3:return s->s_detector.symbol_rate;
 case 4:return s->upstream_data_rate;case 5:return s->upstream_high_carrier;
 case 6:return s->phase4.rx.symbol_rate;case 7:return s->phase4.upstream.symbol_rate;
 case 8:return s->phase4.upstream.rate;case 9:return s->phase4.upstream.high_carrier;
 case 10:return s->phase4_active;default:return 0;}}
void enter_phase4(V90Startup*s){
 int16_t in=0,out=0;s->training.found=1;s->uinfo=90;
 v90_startup_process(s,&out,&in,1);
 if(s->phase4.rx.symbol_rate!=s->upstream_symbol_rate)abort();
 s->training_tx.stage=2;v90_startup_process(s,&out,&in,1);
}
void mp(V90Startup*s,unsigned char*b){mp_build(&s->phase4);memcpy(b,s->phase4.mp,102);}
void retrain(V90Startup*s){begin_retrain(s,"profile test");}
''')
    files=['v90training','v90dil','v90cp','v90pcm','v90upstream','v90trellis','v90qam8','v90equalizer','v90shell','v90mapping','v90train_tx']
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror','-I'+str(root/'vendor/linmodem'),str(w),*[str(root/'vendor/linmodem'/f'{f}.c') for f in files],'-lm','-o',str(d/'test.so')],check=True)
    lib=C.CDLL(str(d/'test.so'));lib.create.restype=C.c_void_p
    for name in ['destroy','enter_phase4','retrain']:getattr(lib,name).argtypes=[C.c_void_p]
    lib.get.argtypes=[C.c_void_p,C.c_int]
    lib.offer.argtypes=lib.mp.argtypes=[C.c_void_p,C.POINTER(C.c_ubyte)]
    lib.wire.argtypes=[C.c_void_p,np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS'),C.c_int,C.c_int]
    count=0
    for forced in [None,'3000','3200','invalid']:
      for mask in range(16):
       for large in [0,1]:
        for code in [2,3,4,5]:
            os.environ['SIPFAX_V90_UPSTREAM_RATE']='31200'
            if forced is None:os.environ.pop('SIPFAX_V90_UPSTREAM_SYMBOL_RATE',None)
            else:os.environ['SIPFAX_V90_UPSTREAM_SYMBOL_RATE']=forced
            s=lib.create()
            try:
                b=frame(29);field(b,15,4,mask);b[25]=large;b+=crc(b[12:29])+[1]*4
                pcm=wave(b);lib.wire(s,pcm,len(pcm),0);assert lib.get(s,0)==1
                offer=(C.c_ubyte*109)();lib.offer(s,offer);offer=list(offer)
                assert offer[89:105]==crc(offer[12:89])
                rates=[]
                for i,baud in enumerate([3000,3200]):
                    carriers=(mask>>(2*i))&3;enabled=carriers and (forced not in ['3000','3200'] or int(forced)==baud)
                    expected=(12 if baud==3000 or not large else 13) if enabled else 0
                    offset=52+9*i;rates.append(expected)
                    assert number(offer,offset+5,4)==expected
                    assert offer[offset]==int(bool(enabled and carriers&2))
                    assert number(offer,offset+1,4)==0
                assert not any(offer[25:52]+offer[70:79])
                b=frame(50);field(b,25,7,90);field(b,34,3,code);field(b,37,3,6);b+=crc(b[12:50])+[1]*4
                damaged=b.copy();damaged[34]^=1
                pcm=wave(damaged);lib.wire(s,pcm,len(pcm),1)
                assert lib.get(s,1)==0,'CRC-damaged INFO1a enabled training'
                pcm=wave(b);lib.wire(s,pcm,len(pcm),1)
                accepted=code in [3,4] and rates[code-3]>0
                assert lib.get(s,1)==int(accepted),(forced,mask,large,code)
                if accepted:
                    baud=3000 if code==3 else 3200;rate=rates[code-3]*2400;high=offer[52+9*(code-3)]
                    assert [lib.get(s,k) for k in [2,3,4,5]]==[baud,baud,rate,high]
                    lib.enter_phase4(s)
                    assert [lib.get(s,k) for k in [6,7,8,9,10]]==[baud,baud,rate,high,1]
                    mp=(C.c_ubyte*102)();lib.mp(s,mp);mp=list(mp)
                    assert number(mp,24,4)==rate//2400
                    assert mp[36:50]==[int(i==rate//2400-2) for i in range(14)]
                os.environ['SIPFAX_V90_UPSTREAM_RATE']='4800';os.environ['SIPFAX_V90_UPSTREAM_SYMBOL_RATE']='3200'
                lib.retrain(s);after=(C.c_ubyte*109)();lib.offer(s,after);assert list(after)==offer
                count+=1
            finally:lib.destroy(s)
print(f'PASS: {count} wire negotiation cases, carrier offers, selected training/Phase4/MP profiles, CRC rejection, rejected modes and retrain preservation')
