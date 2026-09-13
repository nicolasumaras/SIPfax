#!/usr/bin/env python3
"""Replay a private 48k call's initial training and late rate request.
Usage: v90-renegotiation.py RX10057.s16
Captures contain private traffic and must remain outside the repository.
"""
import ctypes as C
from pathlib import Path
import subprocess,tempfile,sys
import numpy as np
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
 w=Path(tmp)/'w.c';so=Path(tmp)/'r.so'
 w.write_text('''#include <stdlib.h>
#include "v90phase4.h"
static unsigned consumed,violations,reneg_k;
static int bit(void *p){++consumed;return 1;}
void *create(void){V90Phase4 *s=malloc(sizeof(*s));v90_phase4_init(s,0,78);s->get_data_bit=bit;return s;}
void run(V90Phase4 *s,const int16_t *in,int16_t *out,unsigned n){
 for(unsigned i=0;i<n;++i){unsigned before=consumed,stage=s->stage;out[i]=v90_phase4_next(s,in[i]);
 if(stage==6 && s->stage==2)reneg_k=s->encoder.k;
 if(s->stage!=4 && consumed!=before)++violations;}}
unsigned value(V90Phase4 *s,unsigned i){return i==0?s->stage:i==1?s->renegotiations:i==2?s->reneg_start:i==3?s->trn_start:i==4?s->encoder.k:i==5?violations:i==6?consumed:reneg_k;}
int peak(V90Phase4 *s,unsigned i){for(int u=127;u>=0;--u)if(s->preceding_cp.mask[0][s->preceding_cp.indices[i]][u])return v90_pcm_level(0,u);return 0;}
void destroy(void *p){free(p);}
''')
 sources=['v90phase4.c','v90pcm.c','v90cp.c','v90dil.c','v90training.c','v90upstream.c','v90trellis.c','v90qam8.c','v90equalizer.c','v90shell.c','v90mapping.c','v42detect.c','v90odp.c']
 subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror','-I'+str(root/'vendor/linmodem'),str(w),*[str(root/'vendor/linmodem'/n) for n in sources],'-lm','-o',str(so)],check=True)
 lib=C.CDLL(str(so));lib.create.restype=C.c_void_p
 ptr=np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS')
 lib.run.argtypes=[C.c_void_p,ptr,ptr,C.c_uint];lib.value.argtypes=[C.c_void_p,C.c_uint];lib.peak.argtypes=[C.c_void_p,C.c_uint];lib.destroy.argtypes=[C.c_void_p]
 x=np.fromfile(sys.argv[1],dtype='<i2')[16*8000:474*8000];out=np.zeros_like(x);s=lib.create()
 try:
  lib.run(s,x,out,len(x))
  assert lib.value(s,1)==1,'expected one real rate request'
  start=lib.value(s,2);trn=lib.value(s,3)
  assert trn-start==408 and start%6==0,(start,trn)
  peaks=[lib.peak(s,i) for i in range(6)]
  rd=[peaks[i]*(1 if i<3 else -1) for i in range(6)]
  assert out[start:start+384].tolist()==rd*64
  assert out[start+384:trn].tolist()==[-v for v in rd]*4
  assert lib.value(s,7)==12,'renegotiation must retain CPt K'
  assert lib.value(s,5)==0,'DTE consumed while clamped'
  assert lib.value(s,6)>0,'test never entered data mode'
  assert set(np.abs(out[trn:trn+2040]).tolist())=={1244,3772,6140,8316},'TRN2d must use CPt Ucodes, not data Ucodes'
  print('PASS: real late S/Sbar triggers one aligned384+24-symbol Rd response and clamps DTE')
 finally:lib.destroy(s)
