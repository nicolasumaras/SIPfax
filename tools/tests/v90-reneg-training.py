#!/usr/bin/env python3
"""Decode configured renegotiation TRN/MP after an independent S/Sbar signal."""
import ctypes as C
import os
from pathlib import Path
import subprocess
import tempfile
import numpy as np

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    wrapper = Path(tmp)/'wrapper.c'
    wrapper.write_text('''#include <stdlib.h>
#include "v90phase4.c"
static unsigned consumed;
static int bit(void *p){++consumed;return 1;}
void *create(void){
 V90Phase4 *s=malloc(sizeof(*s));v90_phase4_init(s,0,78);
 s->cpt=(V90Cp){0};s->cpt.drn=9;s->cpt.sr=1;s->cpt.lookahead=1;s->cpt.count=1;s->cpt.filter[0]=63;
 unsigned u[4]={53,78,88,96};for(unsigned i=0;i<4;++i)s->cpt.mask[0][0][u[i]]=1;
 s->cp=s->cpt;s->cp.type=1;for(unsigned i=1;i<100;++i)s->cp.mask[0][0][i]=1;
 s->get_data_bit=bit;
 if(v90_pcm_init(&s->encoder,&s->cp,data_bit,s))abort();
 s->stage=4;s->have_cpt=1;s->samples=600;s->data_start=0;consumed=0;return s;
}
unsigned run(V90Phase4 *s,const int16_t *in,int16_t *out,unsigned n){
 unsigned violations=0;
 for(unsigned i=0;i<n;++i){unsigned before=consumed;out[i]=v90_phase4_next(s,in[i]);if(s->stage!=4 && consumed!=before)++violations;}
 return violations;
}
unsigned get(V90Phase4 *s,unsigned field){return field==0?s->trn_start:field==1?s->trn_frames:field==2?s->reneg_start:field==3?s->stage:s->renegotiations;}
void destroy(void *s){free(s);}
''')
    src=root/'vendor/linmodem';so=Path(tmp)/'test.so'
    subprocess.run(['gcc','-O2','-Wall','-Werror','-shared','-fPIC','-I'+str(src),str(wrapper),*[str(src/f) for f in ['v90pcm.c','v90training.c','v90cp.c','v90dil.c','v90upstream.c','v90trellis.c','v90qam8.c','v90equalizer.c','v90shell.c']],'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.create.restype=C.c_void_p
    ptr=np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS')
    lib.run.argtypes=[C.c_void_p,ptr,ptr,C.c_uint];lib.get.argtypes=[C.c_void_p,C.c_uint];lib.destroy.argtypes=[C.c_void_p]
    n=np.arange(19000);t=n/8000
    x=(1300*np.cos(2*np.pi*1920*t+.8)+900*np.cos(2*np.pi*320*t+.2)+900*np.cos(2*np.pi*3520*t+1.4))*((n>=400)&(n<1240))
    x[n>=1200]*=-1;x=x.astype(np.int16)
    levels=[((u%16*8+132)<<(u//16))-132 for u in [96,88,78,53]]
    os.environ['SIPFAX_V90_INITIAL_TRN2D_MS']='1500'
    for setting,frames,rate in [(setting,frames,rate) for setting,frames in [(None,340),('0',0),('1',1),('255',340),('1500',2000),('2000',2666),('-1',340),('2001',340),('junk',340),('999999999999999999999',340)] for rate in [None,'7200','9600','12000','invalid']]:
        if rate is None:os.environ.pop('SIPFAX_V90_UPSTREAM_RATE',None)
        else:os.environ['SIPFAX_V90_UPSTREAM_RATE']=rate
        if setting is None:os.environ.pop('SIPFAX_V90_RENEG_TRN2D_MS',None)
        else:os.environ['SIPFAX_V90_RENEG_TRN2D_MS']=setting
        s=lib.create();out=np.zeros_like(x)
        try:
            assert lib.run(s,x,out,len(x))==0,'DTE consumed while clamped'
            start=lib.get(s,0)-600;rd=lib.get(s,2)-600
            assert lib.get(s,1)==frames and lib.get(s,3)==2 and lib.get(s,4)==1
            assert start-rd==408 and (rd+600)%6==0
            decoded=[];odd=q=prev=0
            for j in range(start,start+frames*6+72,6):
                frame=out[j:j+6];signs=sum(int(v>0)<<k for k,v in enumerate(frame))
                p=signs^prev;nextq=(p&1)^q;p^=[0,0x55,0xff,0xaa][(nextq<<1)|q];q=nextq;prev=signs
                sig=[]
                for k in range(1,6):
                    b=(p>>k)&1
                    if k&1:sig.append(b^odd);odd=b
                    else:sig.append(b)
                v=sum(levels.index(abs(int(v)))<<(2*k) for k,v in enumerate(frame))
                decoded.extend(sig+[(v>>k)&1 for k in range(12)])
            plain=[b^(decoded[j-18] if j>=18 else 0)^(decoded[j-23] if j>=23 else 0) for j,b in enumerate(decoded)]
            assert plain[:frames*17]==[1]*(frames*17)
            mp=plain[frames*17:frames*17+102]
            assert mp[:17]==[1]*17 and all(mp[k]==0 for k in [17,34,51,68,*range(85,102)])
            assert sum(mp[24+k]<<k for k in range(4))==(5 if rate=='12000' else 4 if rate=='9600' else 3 if rate=='7200' else 2)
            assert mp[36:50]==[int(k==(3 if rate=='12000' else 2 if rate=='9600' else 1 if rate=='7200' else 0)) for k in range(14)]
            crc=0xffff
            for k in range(18,69):
                if k%17==0:continue
                top=(crc>>15)^mp[k];crc=(crc<<1)&0xffff
                if top:crc^=0x1021
            assert mp[69:85]==[(crc>>(15-k))&1 for k in range(16)]
            assert plain[frames*17+102:frames*17+204]==mp
        finally:lib.destroy(s)
    print('PASS: S/Sbar, aligned Rd/Rbar, independent training/MP decode, zero/long/default/invalid durations, configured upstream rate/capability/CRC and DTE clamp')
