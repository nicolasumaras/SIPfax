#!/usr/bin/env python3
"""Independent 7200 transmitter through PCM, native acquisition and PPP FCS."""
import ctypes as C
import itertools
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import numpy as np
root=Path(__file__).resolve().parents[2]
converter=[[0,0,1,1,8,8,9,9],[3,2,2,3,11,10,10,11],
 [5,5,4,4,13,13,12,12],[6,7,7,6,14,15,15,14],
 [8,8,9,9,0,0,1,1],[11,10,10,11,3,2,2,3],
 [13,13,12,12,5,5,4,4],[14,15,15,14,6,7,7,6]]
rings=sorted(itertools.product(range(2),repeat=8),key=lambda r:
 (sum(r),sum(r[:4]),sum(r[4:6]),r[6],r[4],sum(r[:2]),r[2],r[0]))[:64]
def frame(payload):
    crc=0xffff
    for value in payload:
        crc^=value
        for _ in range(8):crc=(crc>>1)^(0x8408 if crc&1 else 0)
    return payload+bytes([(crc^0xffff)&255,(crc^0xffff)>>8])
expected=[frame(b'\xff\x03\xc0\x21'+bytes(range(n))) for n in [20,64,256]]
bad=bytearray(frame(b'\xff\x03\xc0\x21wrong CRC'));bad[-1]^=1
wire=bytearray(b'\x7e'*8)
for packet in [expected[0],bad,*expected[1:]]:
    for b in packet:
        wire.extend([0x7d,b^0x20] if b<32 or b in [0x7d,0x7e] else [b])
    wire.extend(b'\x7e'*8)
clock_drift='--clock-drift' in sys.argv
if clock_drift:
    wire*=8;expected*=8
plain=[1]*288+[1]*180
for b in wire:plain.extend([0]+[(b>>i)&1 for i in range(8)]+[1])
plain.extend([1]*360)
plain.extend([1]*((-len(plain))%18))
# GPA encoder, continuous from reset B1 into data.
register=0;bits=[]
for b in plain:
    out=b^((register>>22)&1);register=(register<<1)&0x7fffff
    if out:register^=1|(1<<18)
    bits.append(out)
state=previous=0;symbols=[];pattern=[int(x) for x in '01110111111110']
for f in range(len(bits)//18):
    v=bits[18*f:18*f+18];shell=rings[sum(v[i]<<i for i in range(6))]
    for p in range(4):
        pair=4*f+p+384
        inv=pattern[(pair//32)%14] if pair%32==0 else 0
        a=(previous+v[7+3*p]+2*v[8+3*p])%4
        b=(a+2*v[6+3*p]+((state&1)^inv))%4;previous=a
        x=a+4*shell[2*p];y=b+4*shell[2*p+1]
        symbols.extend([([1+1j,-3+1j][q>>2])*(-1j)**(q&3) for q in [x,y]])
        t=converter[x][y];u=state&1
        state=(state>>1)^(t&1)^(((t>>1)&1)<<1)^((((t>>1)&1)^u)<<2)^(u<<3)
def pulse(t):
    beta=.1
    if abs(t)<1e-9:return 1-beta+4*beta/math.pi
    if abs(abs(t)-1/(4*beta))<1e-9:
        return beta/math.sqrt(2)*((1+2/math.pi)*math.sin(math.pi/(4*beta))+(1-2/math.pi)*math.cos(math.pi/(4*beta)))
    return (math.sin(math.pi*t*(1-beta))+4*beta*t*math.cos(math.pi*t*(1+beta)))/(math.pi*t*(1-16*beta*beta*t*t))
with tempfile.TemporaryDirectory() as td:
    d=Path(td);w=d/'wrapper.c';so=d/'receiver.so'
    w.write_text('''#include <stdlib.h>
#include "v90phase4.c"
void *create(void (*cb)(void*,const uint8_t*,unsigned)) {
 V90Upstream*s=malloc(sizeof(*s));v90_upstream_init(s);s->require_b1=1;s->receive_frame=cb;return s;
}
void run(V90Upstream*s,const int16_t*x,unsigned n){for(unsigned i=0;i<n;++i)v90_upstream_receive(s,x[i]);}
void destroy(void*s){free(s);}
unsigned rate(V90Upstream*s){return s->rate;}
unsigned acquired(V90Upstream*s){return s->b1_seen;}
void *phase4_create(void (*cb)(void*,const uint8_t*,unsigned)) {
 V90Phase4*s=malloc(sizeof(*s));v90_phase4_init(s,0,78);
 s->cpt.drn=9;s->cpt.sr=1;s->cpt.lookahead=1;s->cpt.count=1;s->cpt.filter[0]=63;
 unsigned u[4]={53,78,88,96};for(unsigned i=0;i<4;++i)s->cpt.mask[0][0][u[i]]=1;
 if(v90_pcm_init(&s->encoder,&s->cpt,training_bit,s))abort();
 s->stage=2;s->have_cpt=1;s->mp_length=102;s->upstream.receive_frame=cb;return s;
}
void phase4_run(V90Phase4*s,const int16_t*x,unsigned n) {
 for(unsigned i=0;i<n;++i){if(s->samples==140)s->rx.e_seen=1;v90_phase4_next(s,x[i]);}
}
unsigned phase4_acquired(V90Phase4*s){return s->upstream.b1_seen;}

''')
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC','-I'+str(root/'vendor/linmodem'),str(w),
        *[str(root/'vendor/linmodem'/f) for f in ['v90upstream.c','v90trellis.c','v90qam8.c','v90shell.c','v90training.c','v90pcm.c','v90cp.c','v90dil.c']],'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));cbtype=C.CFUNCTYPE(None,C.c_void_p,C.POINTER(C.c_uint8),C.c_uint)
    lib.create.argtypes=[cbtype];lib.create.restype=C.c_void_p
    lib.run.argtypes=[C.c_void_p,np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS'),C.c_uint]
    lib.phase4_create.argtypes=[cbtype];lib.phase4_create.restype=C.c_void_p
    lib.phase4_run.argtypes=lib.run.argtypes
    for name in ['rate','acquired','destroy','phase4_acquired']:getattr(lib,name).argtypes=[C.c_void_p]
    os.environ['SIPFAX_V90_UPSTREAM_RATE']='7200'
    rng=np.random.default_rng(9072)
    for fraction,ppm in ([(.25,-100),(.25,100)] if clock_drift else [(x,0) for x in [0,.25,.5,.75]]):
        base=np.zeros(int(len(symbols)*2.5)+250,dtype=complex)
        for i,z in enumerate(symbols):
            center=100+fraction+2.5*i*(1+ppm/1e6)
            for n in range(math.ceil(center-40),math.floor(center+40)+1):base[n]+=z*pulse((n-center)/2.5)
        samples=np.arange(len(base));carrier=np.exp(1j*(.61+2*np.pi*1920.3*samples/8000))
        pcm=np.rint(1800*(base*carrier).real+rng.normal(0,1,len(base))).astype(np.int16)
        received=[]
        cb=cbtype(lambda _,p,n:received.append(bytes(p[:n])))
        s=lib.create(cb)
        try:
            assert lib.rate(s)==7200
            # Irregular input chunks must not affect filter or decoder state.
            for start in range(0,len(pcm),137):
                chunk=pcm[start:start+137];lib.run(s,chunk,len(chunk))
            assert lib.acquired(s)
            assert received==expected,(fraction,[len(x) for x in received])
        finally:lib.destroy(s)
        received.clear();s=lib.phase4_create(cb)
        try:
            lib.phase4_run(s,pcm,len(pcm))
            assert lib.phase4_acquired(s), 'Delayed E reset discarded B1'
            assert received==expected, 'Pre-E replay changed PPP data'
        finally:lib.destroy(s)
print('PASS: clock drift '+str(clock_drift)+'; 7200 PCM to exact PPP frames, B1, fractional timing, carrier offset/noise, CRC rejection and duplicate filtering')
