#!/usr/bin/env python3
"""Recover a missed DIL-ending S/Sbar from CRC-valid CPt in private captures.
Usage: v90-dil-cpt.py RX10222.s16 RX10992.s16
"""
import ctypes as C
from pathlib import Path
import subprocess,tempfile,sys
import numpy as np
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
 w=Path(tmp)/'w.c';so=Path(tmp)/'test.so'
 w.write_text('''#include <stdlib.h>
#include "v90startup.h"
static long stop_at,phase4_at;
void *create(int law){
 V90Startup *s=malloc(sizeof(*s));v90_startup_init(s,law);
 s->ranging_state=9;s->training_tx_active=1;s->training.found=1;s->uinfo=78;
 v90_train_tx_init(&s->training_tx,law,78);s->training_tx.stage=1;s->training_tx.sample=3000;s->training_tx.jd_end=2544;
 V90Dil *d=&s->training_tx.dil;d->n=1;d->lsp=d->ltp=1;
 d->sp[0]=d->tp[0]=1;d->ucodes[0]=78;d->h[4]=20;
 s->training_tx.dil_position=7;
 v90_training_init(&s->phase4.rx);s->phase4.rx.cp_mode=1;
 stop_at=phase4_at=-1;return s;
}
void run(V90Startup *s,const int16_t *in,int16_t *out,unsigned n){
 for(unsigned i=0;i<n;++i){v90_startup_process(s,out+i,in+i,1);
 if(s->training_tx.stop_dil && stop_at<0)stop_at=s->samples-1;
 if(s->phase4_active && phase4_at<0)phase4_at=s->samples-1;}
}
long value(V90Startup *s,unsigned i){return i==0?s->phase4_active:i==1?s->s_transitions:i==2?stop_at:i==3?phase4_at:i==4?s->training_tx.dil_position:s->phase4.have_cpt;}
void destroy(void *s){free(s);}
''')
 sources=['v90startup.c','v90training.c','v90dil.c','v90cp.c','v90phase4.c','v90pcm.c','v90upstream.c','v90train_tx.c']
 subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror','-I'+str(root/'vendor/linmodem'),str(w),*[str(root/'vendor/linmodem'/n) for n in sources],'-lm','-o',str(so)],check=True)
 lib=C.CDLL(str(so));lib.create.argtypes=[C.c_int];lib.create.restype=C.c_void_p
 ptr=np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS')
 lib.run.argtypes=[C.c_void_p,ptr,ptr,C.c_uint];lib.value.argtypes=[C.c_void_p,C.c_uint];lib.value.restype=C.c_long;lib.destroy.argtypes=[C.c_void_p]
 # Start after the actual S/Sbar; a stronger detector now sees the transition
 # that the original 17.5s test prefix inadvertently included.
 for law,path,start,end in [(0,sys.argv[1],17.58,18.7),(1,sys.argv[2],16,18)]:
  for valid in [False,True]:
   x=np.fromfile(path,dtype='<i2')[int(start*8000):int(end*8000)] if valid else np.zeros(16000,dtype=np.int16)
   out=np.zeros_like(x);s=lib.create(law)
   lib.run(s,x,out,len(x))
   assert lib.value(s,1)==0,'test must not contain a detected S/Sbar'
   assert bool(lib.value(s,0))==valid
   if valid:
    stop=lib.value(s,2);finish=lib.value(s,3)
    assert 0<=finish-stop<126 and (finish+8)%126==0,'DIL ended inside a segment'
    assert lib.value(s,4)==0 and lib.value(s,5)==1
   lib.destroy(s)
 print('PASS: real mu-law/A-law CPt ends DIL without S/Sbar, at segment boundary; silence cannot advance')
