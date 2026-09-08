#!/usr/bin/env python3
"""Recover CRC-valid PPP frames from a real V.90 caller recording in native C."""
import ctypes as C
from pathlib import Path
import subprocess,tempfile,argparse
import numpy as np
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('recording')
ap.add_argument('--start',type=float,default=21)
ap.add_argument('--end',type=float,default=36)
ap.add_argument('--min-frames',type=int,default=3)
ap.add_argument('--min-long-frames',type=int,default=0)
args=ap.parse_args()
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    w=Path(tmp)/'wrap.c';so=Path(tmp)/'up.so'
    w.write_text('''#include <stdlib.h>
#include "v90upstream.h"
static unsigned long_frames;
static void receive(void *opaque,const uint8_t *frame,unsigned n){if(n>=1500)++long_frames;}
unsigned long_count(void){return long_frames;}
void *create(void){V90Upstream *s=malloc(sizeof(*s));v90_upstream_init(s);s->receive_frame=receive;return s;}
void run(V90Upstream *s,const int16_t *x,unsigned n){for(unsigned i=0;i<n;++i)v90_upstream_receive(s,x[i]);}
unsigned frames(V90Upstream *s){return s->frames;}
unsigned lcp(V90Upstream *s){return s->last_length>=6 && s->last_frame[0]==255 && s->last_frame[1]==3 && s->last_frame[2]==0xc0 && s->last_frame[3]==0x21;}
''')
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror','-I'+str(root/'vendor/linmodem'),str(w),str(root/'vendor/linmodem/v90upstream.c'),'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.create.restype=C.c_void_p
    for name in ['frames','lcp']:getattr(lib,name).argtypes=[C.c_void_p]
    lib.run.argtypes=[C.c_void_p,np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS'),C.c_uint]
    x=np.fromfile(args.recording,dtype='<i2')[int(args.start*8000):int(args.end*8000)];s=lib.create()
    minimum=args.min_frames;minimum_long=args.min_long_frames
    for i in range(0,len(x),160):chunk=x[i:i+160];lib.run(s,chunk,len(chunk))
    assert lib.frames(s)>=minimum,(lib.frames(s),minimum)
    assert lib.long_count()>=minimum_long,(lib.long_count(),minimum_long)
    if not minimum_long:assert lib.lcp(s)
    print('PASS: native upstream receiver recovered',lib.frames(s),'CRC-valid PPP frames, including',lib.long_count(),'full-size frames')
