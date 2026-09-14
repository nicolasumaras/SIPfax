#!/usr/bin/env python3
"""V.34 alternating-point S/Sbar through RRC PCM, noise and mu-law."""
import ctypes as C
import math
from pathlib import Path
import subprocess
import tempfile
import numpy as np
root=Path(__file__).resolve().parents[2]

def pulse(t):
    beta=.1;out=np.empty_like(t);zero=abs(t)<1e-9;edge=abs(abs(t)-1/(4*beta))<1e-9;other=~(zero|edge)
    out[zero]=1-beta+4*beta/np.pi
    out[edge]=beta/np.sqrt(2)*((1+2/np.pi)*np.sin(np.pi/(4*beta))+(1-2/np.pi)*np.cos(np.pi/(4*beta)))
    u=t[other];out[other]=(np.sin(np.pi*u*(1-beta))+4*beta*u*np.cos(np.pi*u*(1+beta)))/(np.pi*u*(1-16*beta*beta*u*u))
    return out

def quantize(sample):
    value=int(sample)>>2;mask=0x7f if value<0 else 0xff
    value=abs(value)+33;segment=max(0,value.bit_length()-6)
    code=((0x7f if segment>=8 else (segment<<4)|((value>>(segment+1))&15))^mask)^255
    magnitude=(((code&15)<<3)+132)<<((code>>4)&7)
    return 132-magnitude if code&128 else magnitude-132

with tempfile.TemporaryDirectory() as td:
    d=Path(td);w=d/'w.c';so=d/'detector.so'
    w.write_text('''#include <stdlib.h>
#include "v90training.h"
void *create(unsigned baud,unsigned high){V90SDetect*s=malloc(sizeof(*s));if(!s)return NULL;if(!v90_s_detect_init_profile(s,baud,high)){free(s);return NULL;}return s;}
void destroy(void*s){free(s);}
''')
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC','-I'+str(root/'vendor/linmodem'),str(w),*[str(root/'vendor/linmodem'/f) for f in ['v90training.c','v90cp.c','v90dil.c']],'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.create.argtypes=[C.c_uint,C.c_uint];lib.create.restype=C.c_void_p
    lib.v90_s_detect.argtypes=[C.c_void_p,C.c_int16];lib.v90_s_detect_reset.argtypes=[C.c_void_p];lib.destroy.argtypes=[C.c_void_p]
    rng=np.random.default_rng(9053000);cases=0
    for baud in [3000,3200]:
      for high in [0,1]:
        carrier=(2000 if high else 1800) if baud==3000 else (1920 if high else 12800/7)
        s=lib.create(baud,high);assert s
        def events(pcm):
            lib.v90_s_detect_reset(s);out=[]
            for n,x in enumerate(pcm):
                event=lib.v90_s_detect(s,int(x))
                if event:out.append((n,event))
            return out
        try:
          for start in range(400,500,7):
            for ppm in [-100,100]:
              sps=8000/baud*(1+ppm/1e6);base=np.zeros(1500,complex)
              for n in range(144):
                z=complex(1 if n%2==0 else -1,1)*(1 if n<128 else -1)
                center=start+n*sps;idx=np.arange(math.ceil(center-35),math.floor(center+35)+1)
                base[idx]+=z*pulse((idx-center)/sps)
              t=np.arange(len(base))/8000
              raw=np.rint(1300*np.real(base*np.exp(1j*(.81+2*np.pi*(carrier+.3)*t)))+rng.normal(0,10,len(base))).astype(np.int16)
              for pcmu in [False,True]:
                pcm=np.array([quantize(x) for x in raw],dtype=np.int16) if pcmu else raw
                out=events(pcm);assert [e for _,e in out]==[1,2],(baud,high,start,ppm,pcmu,out)
                assert start+128*sps-16<=out[1][0]<=start+144*sps+25,(baud,high,out)
                cases+=1
          t=np.arange(5000)/8000
          for f in [carrier-baud/2,carrier,carrier+baud/2,1200,2400]:
            assert not events(2500*np.cos(2*np.pi*f*t)+rng.normal(0,10,len(t))),(baud,high,f)
          assert not events(rng.normal(0,1500,len(t)))
        finally:lib.destroy(s)
    print('PASS:',cases,'shaped S/Sbar cases across profiles, offsets, drift and mu-law; tones/noise rejected')
