#!/usr/bin/env python3
"""Feed synthetic caller CPs/CPs-prime/SCR/CP/CP-prime audio through Phase4."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import numpy as np
root=Path(__file__).resolve().parents[2]

def cp(silence,ack,corrupt=False):
    b=bytearray((root/'test/fixtures/v90-cpt-6417.bits').read_bytes())
    b[19]=1;b[30]=silence;b[33]=ack
    for side in range(2):
        for u in range(128):b[137+side*136+(u//16)*17+u%16]=int(1<=u<100)
    register=0xffff
    for j in range(18,409):
        if j%17==0:continue
        top=(register>>15)^b[j];register=(register<<1)&0xffff
        if top:register^=0x1021
    b[409:425]=bytes((register>>(15-j))&1 for j in range(16))
    if corrupt:b[52]^=1
    return list(b)

def audio(bits,phase,offset):
    # Independent GPA recurrence and differential four-point transmitter.
    y=[]
    for n,b in enumerate(bits):y.append(b^(y[n-5] if n>=5 else 0)^(y[n-23] if n>=23 else 0))
    angle=phase;symbols=[]
    for i in range(0,len(y)-1,2):
        angle-=np.pi/2*(y[i]+2*y[i+1]);symbols.append(np.exp(1j*angle))
    impulses=np.zeros(len(symbols)*10,dtype=complex);impulses[::10]=symbols
    t=np.arange(-160,161)/10;beta=.1
    taps=np.empty(len(t))
    for i,v in enumerate(t):
        if abs(v)<1e-9:taps[i]=1-beta+4*beta/np.pi
        elif abs(abs(v)-1/(4*beta))<1e-9:
            taps[i]=beta/np.sqrt(2)*((1+2/np.pi)*np.sin(np.pi/(4*beta))+(1-2/np.pi)*np.cos(np.pi/(4*beta)))
        else:taps[i]=(np.sin(np.pi*v*(1-beta))+4*beta*v*np.cos(np.pi*v*(1+beta)))/(np.pi*v*(1-16*beta*beta*v*v))
    z=np.convolve(impulses,taps)
    n=np.arange(len(z));wave=2200*np.real(z*np.exp(2j*np.pi*1920*n/32000))
    return np.ascontiguousarray(wave[offset::4].astype(np.int16))

with tempfile.TemporaryDirectory() as tmp:
    w=Path(tmp)/'test.c';so=Path(tmp)/'test.so'
    w.write_text(r'''
#include <stdlib.h>
#include "v90phase4.c"
int run(const int16_t *pcm,unsigned n,const uint8_t *bits,unsigned nb,int valid){
 V90Phase4 s;v90_phase4_init(&s,0,78);
 if(v90_cp_parse(&s.cpt,bits,nb,0)!=1)return 1;
 s.preceding_cp=s.cpt;s.preceding_cp.type=1;
 for(unsigned u=1;u<100;++u)s.preceding_cp.mask[0][0][u]=1;
 if(v90_pcm_renegotiate(&s.encoder,&s.preceding_cp,&s.cpt,training_bit,&s))return 2;
 s.stage=2;s.have_cpt=1;s.renegotiations=1;
 s.mp_length=((86+s.encoder.k+s.encoder.s-1)/(s.encoder.k+s.encoder.s))*(s.encoder.k+s.encoder.s);
 mp_build(&s);
 unsigned silent=0,rt=0,seen=0;
 for(unsigned i=0;i<n;++i){
  unsigned stage=s.stage;
  int out=v90_phase4_next(&s,pcm[i]);
  if(s.stage==7){++silent;seen|=1;if(out!=0)return 3;}
  if(stage==8 && s.rt_start && i>=s.rt_start)++rt;
  if(s.stage==4)seen|=2;
 }
 if(!valid)return seen || s.have_cp?4:0;
 return seen==3 && silent>=800 && rt==408 && s.stage==4?0:5;
}
''')
    sources=['v90training.c','v90pcm.c','v90cp.c','v90dil.c','v90upstream.c','v90trellis.c','v90qam8.c','v90equalizer.c','v90shell.c','v90mapping.c','v42detect.c']
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror','-I'+str(root/'vendor/linmodem'),str(w),*[str(root/'vendor/linmodem'/s) for s in sources],'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.run.argtypes=[np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS'),C.c_uint,C.c_char_p,C.c_uint,C.c_int]
    fixture=(root/'test/fixtures/v90-cpt-6417.bits').read_bytes()
    for valid in [False,True]:
        bits=[1]*300+cp(1,0,not valid)*8+cp(1,1,not valid)*8+[1]*1600+cp(0,0,not valid)*8+cp(0,1,not valid)*8
        for offset in range(4):
            x=audio(bits,.71,offset)
            result=lib.run(x,len(x),fixture,len(fixture),valid)
            assert result==0,(valid,offset,result)
    print('PASS: caller audio drives CPs silence/Rt/data; corrupt CRC cannot start handshake; four fractional sample phases')
