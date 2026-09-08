#!/usr/bin/env python3
"""Validate captured CPt, reject corrupt/truncated messages, replay live C receiver."""
import ctypes as C
from pathlib import Path
import subprocess,tempfile,sys
import numpy as np
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    wrapper=Path(tmp)/'wrap.c';so=Path(tmp)/'cp.so'
    wrapper.write_text('''#include <stdlib.h>
#include "v90training.h"
int parse(const unsigned char *b,unsigned n) {
 V90Cp cp;unsigned used;int r=v90_cp_parse(&cp,b,n,&used);
 if(r!=1)return r;
 return used==428 && cp.type==0 && cp.drn==9 && cp.sr==1 && cp.lookahead==1 && cp.gain==8180 && cp.filter[0]==63 && cp.count==1 && cp.codec_masks==1 && cp.mask[0][0][53] && cp.mask[0][0][78] && cp.mask[0][0][88] && cp.mask[0][0][96]?1:-2;
}
void *create(void){V90Training *s=malloc(sizeof(*s));v90_training_init(s);s->cp_mode=1;return s;}
int drn(V90Training *s){return s->cp.drn;}
''')
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror','-I'+str(root/'vendor/linmodem'),str(wrapper),*[str(root/'vendor/linmodem'/n) for n in ['v90cp.c','v90dil.c','v90training.c']],'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.parse.argtypes=[C.c_char_p,C.c_uint]
    b=(root/'test/fixtures/v90-cpt-6417.bits').read_bytes();assert lib.parse(b,len(b))==1
    for i in range(len(b)):assert lib.parse(b,i)!=1
    for i in range(len(b)):
        bad=bytearray(b);bad[i]^=1;assert lib.parse(bytes(bad),len(bad))!=1,i
    if len(sys.argv)>1:
        lib.create.restype=C.c_void_p;lib.drn.argtypes=[C.c_void_p]
        lib.v90_training_receive.argtypes=[C.c_void_p,np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS'),C.c_int]
        x=np.fromfile(sys.argv[1],dtype='<i2')[17*8000:20*8000];s=lib.create()
        for i in range(0,len(x),160):
            frame=x[i:i+160];found=lib.v90_training_receive(s,frame,len(frame))
        assert found>=10 and lib.drn(s)==9,found
        print('Live receiver CRC-valid CPt count:',found)
    print('PASS: hardware CPt fields, all truncations and single-bit corruptions')
