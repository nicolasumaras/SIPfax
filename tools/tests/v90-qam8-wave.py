#!/usr/bin/env python3
"""Independent rate-specific transmitter through PCM, native acquisition and PPP FCS."""
import argparse
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
parser=argparse.ArgumentParser(add_help=False)
parser.add_argument('--symbol-rate',type=int,choices=[3000,3200],default=3200)
parser.add_argument('--low-carrier',action='store_true')
parser.add_argument('--seed',type=int,default=None)
parser.add_argument('--keep-going',action='store_true',help='Report every frame mismatch in the timing matrix, then fail')
options,_=parser.parse_known_args()
baud=options.symbol_rate
failures=[]
def verify_frames(received, fraction, ppm, mode):
    if received==expected:return
    result={'phase':fraction,'ppm':ppm,'mode':mode,'frames':len(received),
            'expected':len(expected),'first_mismatch':next((i for i,(a,b) in
            enumerate(zip(received,expected)) if a!=b),None)}
    if not options.keep_going:raise AssertionError(result)
    failures.append(result)

converter=[[0,0,1,1,8,8,9,9],[3,2,2,3,11,10,10,11],
 [5,5,4,4,13,13,12,12],[6,7,7,6,14,15,15,14],
 [8,8,9,9,0,0,1,1],[11,10,10,11,3,2,2,3],
 [13,13,12,12,5,5,4,4],[14,15,15,14,6,7,7,6]]
rate=4800 if "--4800" in sys.argv else 31200 if "--31200" in sys.argv else 28800 if "--28800" in sys.argv else 26400 if "--26400" in sys.argv else 24000 if '--24000' in sys.argv else 21600 if '--21600' in sys.argv else 19200 if '--19200' in sys.argv else 16800 if '--16800' in sys.argv else 14400 if '--14400' in sys.argv else 12000 if '--12000' in sys.argv else 9600 if '--9600' in sys.argv else 7200
k,m={4800:(0,1),7200:(6,2),9600:(12,3),12000:(18,5),14400:(24,8),16800:(30,14),19200:(28,12),21600:(26,10),24000:(24,8),26400:(30,14),28800:(28,12),31200:(26,10)}[rate]
q_bits=5 if rate==31200 else 4 if rate==28800 else 3 if rate>=24000 else 2 if rate==21600 else 1 if rate==19200 else 0
if baud==3000:
    if rate>28800:parser.error('3000 symbols/s supports at most 28800 bit/s')
    k,m,q_bits={4800:(1,2,0),7200:(8,2,0),9600:(14,4,0),12000:(20,6,0),14400:(27,11,0),16800:(25,9,1),19200:(24,8,2),21600:(30,14,2),24000:(28,12,3),26400:(27,11,4),28800:(25,9,5)}[rate]
mapping_period=15 if baud==3000 else 16
carrier_hz=(1800 if options.low_carrier else 2000) if baud==3000 else (12800/7 if options.low_carrier else 1920)
sps=8000/baud
frame_bits=k+12+8*q_bits
high_count=rate//25-(frame_bits-1)*mapping_period
schedule=[frame_bits-1+int((i+1)*high_count//mapping_period!=i*high_count//mapping_period) for i in range(mapping_period)]
from v90_shell_reference import ShellReference
if m>=5:rings=ShellReference(m)
else:
    rings=sorted(itertools.product(range(m),repeat=8),key=lambda r:
     (sum(r),sum(r[:4]),sum(r[4:6]),r[6],r[4],sum(r[:2]),r[2],r[0]))[:1<<k]
def frame(payload):
    crc=0xffff
    for value in payload:
        crc^=value
        for _ in range(8):crc=(crc>>1)^(0x8408 if crc&1 else 0)
    return payload+bytes([(crc^0xffff)&255,(crc^0xffff)>>8])
expected=[frame(b'\xff\x03\xc0\x21'+(bytes(np.random.default_rng((24090 if options.seed is None else options.seed)+n).integers(0,256,n,dtype=np.uint8)) if '--random-payloads' in sys.argv else bytes(range(n)))) for n in [20,64,256]]
bad=bytearray(frame(b'\xff\x03\xc0\x21wrong CRC'));bad[-1]^=1
wire=bytearray(b'\x7e'*8)
for packet in [expected[0],bad,*expected[1:]]:
    for b in packet:
        wire.extend([0x7d,b^0x20] if b<32 or b in [0x7d,0x7e] else [b])
    wire.extend(b'\x7e'*8)
clock_drift='--clock-drift' in sys.argv or '--long-clock' in sys.argv
if clock_drift:
    repetitions=64 if '--long-clock' in sys.argv else 8
    wire*=repetitions;expected*=repetitions
plain=[1]*(rate//25)+[1]*180
for b in wire:plain.extend([0]+[(b>>i)&1 for i in range(8)]+[1])
# Flush the rate-dependent trellis lookahead with real idle symbols.
plain.extend([1]*(20*frame_bits))
frame_sizes=[];total=0
while total<len(plain):
    size=schedule[len(frame_sizes)%mapping_period];frame_sizes.append(size);total+=size
plain.extend([1]*(total-len(plain)))
# GPA encoder, continuous from reset B1 into data.
register=0;bits=[]
for b in plain:
    out=b^((register>>22)&1);register=(register<<1)&0x7fffff
    if out:register^=1|(1<<18)
    bits.append(out)
quarter=[complex(x,y) for x in range(-63,66,4) for y in range(-63,66,4)];quarter.sort(key=lambda z:(abs(z)**2,-z.imag));
state=previous=0;symbols=[];pattern=[int(x) for x in '01110111111110']
def subset(z):
    x=((int(z.real)+3)//2)&3;y=((int(z.imag)+3)//2)&3
    return ((x^y)&1)|((x&1)<<1)|((((x>>1)^(y>>1)^x^y)&1)<<2)
cursor=0
for f,size in enumerate(frame_sizes):
    v=bits[cursor:cursor+size];cursor+=size;frame_k=k-int(size<frame_bits)
    shell=rings[sum(v[i]<<i for i in range(frame_k))]
    for p in range(4):
        pair=4*f+p+24*mapping_period
        half=2*mapping_period
        inv=pattern[(pair//half)%14] if pair%half==0 else 0
        g=frame_k+(3+2*q_bits)*p
        a=(previous+v[g+1]+2*v[g+2])%4
        b=(a+2*v[g]+((state&1)^inv))%4;previous=a
        qa=sum(v[g+3+j]<<j for j in range(q_bits));qb=sum(v[g+3+q_bits+j]<<j for j in range(q_bits))
        x=a+4*((shell[2*p]<<q_bits)|qa);y=b+4*((shell[2*p+1]<<q_bits)|qb)
        points=[(quarter[q>>2])*(-1j)**(q&3) for q in [x,y]]
        symbols.extend(points)
        t=converter[subset(points[0])][subset(points[1])];u=state&1
        state=(state>>1)^(t&1)^(((t>>1)&1)<<1)^((((t>>1)&1)^u)<<2)^(u<<3)
# A complex symbol-spaced channel models precursor/postcursor interference.
# The expected PPP bytes remain independent of this received-signal distortion.
if '--isi' in sys.argv:
    symbols=np.convolve(symbols,np.array([.2-.09j,1,-.17-.05j]),'same')
def pulse(t):
    beta=.1
    if abs(t)<1e-9:return 1-beta+4*beta/math.pi
    if abs(abs(t)-1/(4*beta))<1e-9:
        return beta/math.sqrt(2)*((1+2/math.pi)*math.sin(math.pi/(4*beta))+(1-2/math.pi)*math.cos(math.pi/(4*beta)))
    return (math.sin(math.pi*t*(1-beta))+4*beta*t*math.cos(math.pi*t*(1+beta)))/(math.pi*t*(1-16*beta*beta*t*t))
# Independent G.711 mu-law quantization, using the 14-bit segmented mapping.
# Reference (Sun/Sox codec):
# https://github.com/python/cpython/blob/3.12/Modules/audioop.c
def ulaw_encode(sample):
    value=sample>>2;mask=0x7f if value<0 else 0xff
    value=abs(value)+33;segment=max(0,value.bit_length()-6)
    return (0x7f if segment>=8 else (segment<<4)|((value>>(segment+1))&15))^mask
def ulaw_decode(code):
    value=code^255;magnitude=(((value&15)<<3)+132)<<((value>>4)&7)
    return 132-magnitude if value&128 else magnitude-132
if '--pcmu' in sys.argv:
    assert [ulaw_decode(x) for x in [0,128,127,255]]==[-32124,32124,0,0]
    assert all(ulaw_encode(ulaw_decode(x))==(255 if x==127 else x) for x in range(256))
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
    if baud==3000 or options.low_carrier:
        w.write_text(w.read_text().replace('v90_upstream_init(s);','if(!v90_upstream_init_profile(s,%d,%d,%d))abort();'%(rate,baud,not options.low_carrier)).replace('v90_phase4_init(s,0,78);','if(!v90_phase4_init_profile(s,0,78,%d,%d,%d))abort();'%(rate,baud,not options.low_carrier)))
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC','-I'+str(root/'vendor/linmodem'),str(w),
        *[str(root/'vendor/linmodem'/f) for f in ['v90upstream.c','v90trellis.c','v90qam8.c','v90equalizer.c','v90shell.c','v90mapping.c','v90training.c','v90pcm.c','v90cp.c','v90dil.c']],'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));cbtype=C.CFUNCTYPE(None,C.c_void_p,C.POINTER(C.c_uint8),C.c_uint)
    lib.create.argtypes=[cbtype];lib.create.restype=C.c_void_p
    lib.run.argtypes=[C.c_void_p,np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS'),C.c_uint]
    lib.phase4_create.argtypes=[cbtype];lib.phase4_create.restype=C.c_void_p
    lib.phase4_run.argtypes=lib.run.argtypes
    for name in ['rate','acquired','destroy','phase4_acquired']:getattr(lib,name).argtypes=[C.c_void_p]
    os.environ['SIPFAX_V90_UPSTREAM_RATE']=str(rate)
    rng=np.random.default_rng(9072 if options.seed is None else options.seed)
    cases=[(f,p) for f in [0,.25,.5,.75] for p in [-100,100]] if "--timing-sweep" in sys.argv else [( .25,-100),(.25,100)] if clock_drift else [(x,0) for x in [0,.25,.5,.75]]
    for fraction,ppm in cases:
        base=np.zeros(int(len(symbols)*sps)+250,dtype=complex)
        for i,z in enumerate(symbols):
            center=100+fraction+sps*i*(1+ppm/1e6)
            for n in range(math.ceil(center-40),math.floor(center+40)+1):base[n]+=z*pulse((n-center)/sps)
        samples=np.arange(len(base));carrier=np.exp(1j*(.61+2*np.pi*(carrier_hz+.3)*samples/8000))
        wave=np.rint((400 if rate==31200 else 500 if rate==28800 else 650 if rate==26400 else 900 if rate>=16800 else 1800)*(base*carrier).real+rng.normal(0,1,len(base)))
        assert np.max(abs(wave))<32768,'synthetic PCM clipping'
        pcm=wave.astype(np.int16)
        if '--pcmu' in sys.argv:
            pcm=np.fromiter((ulaw_decode(ulaw_encode(int(x))) for x in pcm),dtype=np.int16)
        received=[]
        cb=cbtype(lambda _,p,n:received.append(bytes(p[:n])))
        s=lib.create(cb)
        try:
            assert lib.rate(s)==rate
            # Irregular input chunks must not affect filter or decoder state.
            for start in range(0,len(pcm),137):
                chunk=pcm[start:start+137];lib.run(s,chunk,len(chunk))
            assert lib.acquired(s)
            verify_frames(received,fraction,ppm,'direct')
        finally:lib.destroy(s)
        received.clear();s=lib.phase4_create(cb)
        try:
            lib.phase4_run(s,pcm,len(pcm))
            assert lib.phase4_acquired(s), 'Delayed E reset discarded B1'
            verify_frames(received,fraction,ppm,'delayed-E')
        finally:lib.destroy(s)
if failures:
    import json
    print('FAIL: '+json.dumps(failures),file=sys.stderr)
    sys.exit(1)
print('PASS: PCMU '+str('--pcmu' in sys.argv)+'; clock drift '+str(clock_drift)+'; '+str(rate)+' bit/s at '+str(baud)+' symbols/s PCM to exact PPP frames, B1, fractional timing, carrier offset/noise, CRC rejection and duplicate filtering')
