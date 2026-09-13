#!/usr/bin/env python3
"""Independent Ja/CP/E waveforms at each upstream symbol rate and carrier."""
import ctypes as C
import math
import sys
from pathlib import Path
import subprocess
import tempfile
import numpy as np
root=Path(__file__).resolve().parents[2]

def crc_cp(bits):
    register=0xffff
    for j in range(18,409):
        if j%17==0:continue
        top=(register>>15)^bits[j];register=(register<<1)&0xffff
        if top:register^=0x1021
    bits[409:425]=bytes((register>>(15-j))&1 for j in range(16))

ja=(root/'test/fixtures/v90-ja-5983.bits').read_bytes()
cp=bytearray((root/'test/fixtures/v90-cpt-6417.bits').read_bytes());cp[19]=1;cp[30]=0;crc_cp(cp)
cpt=(root/'test/fixtures/v90-cpt-6417.bits').read_bytes()

def pulse(t):
    beta=.1;out=np.empty_like(t);zero=abs(t)<1e-9;edge=abs(abs(t)-1/(4*beta))<1e-9;other=~(zero|edge)
    out[zero]=1-beta+4*beta/np.pi
    out[edge]=beta/np.sqrt(2)*((1+2/np.pi)*np.sin(np.pi/(4*beta))+(1-2/np.pi)*np.cos(np.pi/(4*beta)))
    u=t[other];out[other]=(np.sin(np.pi*u*(1-beta))+4*beta*u*np.cos(np.pi*u*(1+beta)))/(np.pi*u*(1-16*beta*beta*u*u))
    return out

def audio(message,baud,carrier,phase,ppm,corrupt=False):
    message=bytearray(message)
    if corrupt:message[52]^=1
    plain=[1,0]*50+list(message)+[1]*60+[0,1]*40
    scrambled=[]
    for n,b in enumerate(plain):scrambled.append(b^(scrambled[n-5] if n>=5 else 0)^(scrambled[n-23] if n>=23 else 0))
    angle=.47;symbols=[]
    for n in range(0,len(scrambled)-1,2):
        angle-=np.pi/2*(scrambled[n]+2*scrambled[n+1]);symbols.append(np.exp(1j*angle))
    sps=8000/baud;out=np.zeros(math.ceil(len(symbols)*sps*(1+ppm/1e6))+200,complex)
    for n,z in enumerate(symbols):
        center=80+phase+n*sps*(1+ppm/1e6);indices=np.arange(math.ceil(center-35),math.floor(center+35)+1)
        out[indices]+=z*pulse((indices-center)/sps)
    n=np.arange(len(out));wave=2100*np.real(out*np.exp(2j*np.pi*(carrier+.3)*n/8000))
    wave+=np.random.default_rng(903000).normal(0,1,len(wave))
    pcm=np.rint(wave).astype(np.int16)
    if '--pcmu' in sys.argv:
        def quantize(sample):
            value=int(sample)>>2;mask=0x7f if value<0 else 0xff
            value=abs(value)+33;segment=max(0,value.bit_length()-6)
            code=((0x7f if segment>=8 else (segment<<4)|((value>>(segment+1))&15))^mask)^255
            magnitude=(((code&15)<<3)+132)<<((code>>4)&7)
            return 132-magnitude if code&128 else magnitude-132
        pcm=np.array([quantize(x) for x in pcm],dtype=np.int16)
    return np.ascontiguousarray(pcm)

with tempfile.TemporaryDirectory() as td:
    d=Path(td);w=d/'w.c';so=d/'receiver.so'
    w.write_text('''#include <stdlib.h>
#include <string.h>
#include "v90training.h"
typedef struct {V90Training rx;V90Dil dil;V90Cp cp;} Test;
void*create(unsigned baud,unsigned high,unsigned cp,const uint8_t*b,unsigned n){
 Test*s=calloc(1,sizeof(*s));if(!s)return NULL;
 if(!v90_training_init_profile(&s->rx,baud,high) || (cp?v90_cp_parse(&s->cp,b,n,NULL):v90_dil_parse(&s->dil,b,n,NULL))!=1){free(s);return NULL;}
 s->rx.cp_mode=cp;return s;
}
void run(Test*s,const int16_t*x,unsigned n){v90_training_receive(&s->rx,x,n);}
int matched(Test*s){return s->rx.found && !(s->rx.cp_mode?memcmp(&s->rx.cp,&s->cp,sizeof(s->cp)):memcmp(&s->rx.dil,&s->dil,sizeof(s->dil)));}
int found(Test*s){return s->rx.found;}
int e_seen(Test*s){return s->rx.e_seen;}
void destroy(void*s){free(s);}
''')
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC','-I'+str(root/'vendor/linmodem'),str(w),*[str(root/'vendor/linmodem'/f) for f in ['v90training.c','v90cp.c','v90dil.c']],'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.create.argtypes=[C.c_uint,C.c_uint,C.c_uint,C.c_char_p,C.c_uint];lib.create.restype=C.c_void_p
    lib.run.argtypes=[C.c_void_p,np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS'),C.c_uint]
    for name in ['matched','found','e_seen','destroy']:getattr(lib,name).argtypes=[C.c_void_p]
    cases=0;failures=[]
    for baud in [3000,3200]:
      for high in [0,1]:
        carrier=(2000 if high else 1800) if baud==3000 else (1920 if high else 12800/7)
        for phase in [0,.25,.5,.75]:
          for ppm in [-100,100]:
            for name,msg,mode,e,corrupt in [('ja',ja,0,0,False),('bad-ja',ja,0,0,True),('cp-e',cp,1,1,False),('bad-cp',cp,1,0,True),('cpt-no-e',cpt,1,0,False)]:
                pcm=audio(msg,baud,carrier,phase,ppm,corrupt);s=lib.create(baud,high,mode,bytes(msg),len(msg));assert s
                try:
                    for start in range(0,len(pcm),137):
                        chunk=pcm[start:start+137];lib.run(s,chunk,len(chunk))
                    valid=bool(lib.matched(s))==(not corrupt) and bool(lib.found(s))==(not corrupt) and lib.e_seen(s)==e
                    if not valid:
                        detail=(baud,high,phase,ppm,name,lib.found(s),lib.matched(s),lib.e_seen(s))
                        if '--keep-going' not in sys.argv:raise AssertionError(detail)
                        failures.append(detail)
                    cases+=1
                finally:lib.destroy(s)
    if failures:raise AssertionError(failures)
    print('PASS:',cases,'PCMU' if '--pcmu' in sys.argv else 'linear PCM','Ja/CP/E cases; symbol rates/carriers, timing, clock offset, CRC and pre-data E rejection')
