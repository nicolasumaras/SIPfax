#!/usr/bin/env python3
"""V.34 table oracles, legacy shell encoder and independent inverse-map inputs."""
import ctypes as C
import random
import subprocess
import tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[2]
native = root / 'vendor/linmodem'
# Published V.34 Tables 8 and 10, no auxiliary channel, minimum shaping.
profiles = {
 3000: [(13,0x3def,1,2,0),(20,0x421,8,2,0),(26,0x2d6b,14,4,0),
        (32,0x7fff,20,6,0),(39,0x14a5,27,11,0),(45,0x3def,25,9,1),
        (52,0x421,24,8,2),(58,0x2d6b,30,14,2),(64,0x7fff,28,12,3),
        (71,0x14a5,27,11,4),(77,0x3def,25,9,5)],
 3200: [(12,0xffff,0,1,0),(18,0xffff,6,2,0),(24,0xffff,12,3,0),
        (30,0xffff,18,5,0),(36,0xffff,24,8,0),(42,0xffff,30,14,0),
        (48,0xffff,28,12,1),(54,0xffff,26,10,2),(60,0xffff,24,8,3),
        (66,0xffff,30,14,3),(72,0xffff,28,12,4),(78,0xffff,26,10,5)]}
legacy = (native/'v34.c.orig').read_text(encoding='latin1')
legacy = legacy[legacy.index('static void index_to_rings('):legacy.index('/* return the K bit index')]
with tempfile.TemporaryDirectory() as td:
 d=Path(td);w=d/'w.c';so=d/'mapping.so'
 w.write_text('''#include <stdlib.h>
#include "v90mapping.h"
#include "v90qam8.h"
void *create(unsigned rate,unsigned baud){V90Mapping*s=malloc(sizeof(*s));if(!s)return NULL;if(!v90_mapping_init(s,rate,baud)){free(s);return NULL;}return s;}
void destroy(void*s){free(s);}
void *detector(unsigned rate,unsigned baud){V90Qam8B1*s=malloc(sizeof(*s));if(!s)return NULL;if(!v90_qam_b1_init_profile(s,rate,baud)){free(s);return NULL;}return s;}
unsigned detector_length(V90Qam8B1*s){return s->length;}
unsigned detector_count(V90Qam8B1*s){return s->count;}
typedef struct {V90Qam8Stream stream;void(*cb)(const uint8_t*,unsigned);} TestStream;
static void delivered(void*opaque,const uint8_t*bits){TestStream*s=opaque;s->cb(bits,s->stream.frame_bits);}
void*stream_create(unsigned rate,unsigned baud,void(*cb)(const uint8_t*,unsigned)){TestStream*s=calloc(1,sizeof(*s));if(!s)return NULL;if(!v90_qam_stream_init_profile(&s->stream,rate,baud)){free(s);return NULL;}s->cb=cb;s->stream.opaque=s;s->stream.receive_bits=delivered;return s;}
unsigned long long stream_source(TestStream*s){return s->stream.output_symbol;}
int stream_symbol(TestStream*s,double r,double i){return v90_qam8_stream_symbol(&s->stream,r,i);}
void stream_mid(TestStream*s,double r,double i){s->stream.have_mid=1;s->stream.mid_re=r;s->stream.mid_im=i;}


void params(V90Mapping*s,unsigned*out){out[0]=s->b;out[1]=s->k;out[2]=s->m;out[3]=s->q;out[4]=s->p;out[5]=s->j;}
double carrier(V90Mapping*s,unsigned high){return high?s->high_carrier:s->low_carrier;}
typedef struct {int M,g2_tab[137],g4_tab[137],z8_tab[138];} V34DSPState;
'''+legacy+'''
void legacy_encode(V34DSPState*s,int index,int*rings){index_to_rings(s,(int(*)[2])rings,index);}
''')
 subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror','-I'+str(native),str(w),str(native/'v90mapping.c'),str(native/'v90shell.c'),str(native/'v90qam8.c'),str(native/'v90trellis.c'),str(native/'v90equalizer.c'),'-lm','-o',str(so)],check=True)
 lib=C.CDLL(str(so));lib.create.argtypes=[C.c_uint,C.c_uint];lib.create.restype=C.c_void_p
 callback=C.CFUNCTYPE(None,C.POINTER(C.c_uint8),C.c_uint)
 lib.stream_create.argtypes=[C.c_uint,C.c_uint,callback];lib.stream_create.restype=C.c_void_p
 lib.stream_source.argtypes=[C.c_void_p];lib.stream_source.restype=C.c_ulonglong
 lib.stream_symbol.argtypes=[C.c_void_p,C.c_double,C.c_double]
 lib.stream_mid.argtypes=[C.c_void_p,C.c_double,C.c_double]
 lib.detector.argtypes=[C.c_uint,C.c_uint];lib.detector.restype=C.c_void_p
 lib.detector_length.argtypes=[C.c_void_p];lib.detector_count.argtypes=[C.c_void_p]
 lib.v90_qam8_b1_symbol.argtypes=[C.c_void_p,C.c_double,C.c_double,C.POINTER(C.c_double),C.POINTER(C.c_double),C.POINTER(C.c_double)]
 lib.destroy.argtypes=[C.c_void_p];lib.params.argtypes=[C.c_void_p,C.POINTER(C.c_uint)]
 lib.carrier.argtypes=[C.c_void_p,C.c_uint];lib.carrier.restype=C.c_double
 lib.v90_mapping_frame_bits.argtypes=[C.c_void_p,C.c_ulonglong]
 lib.v90_mapping_inversion.argtypes=[C.c_void_p,C.c_ulonglong]
 lib.v90_mapping_b1.argtypes=[C.c_void_p,C.POINTER(C.c_uint16),C.c_uint]
 lib.v90_mapping_decode.argtypes=[C.c_void_p,C.c_ulonglong,C.c_uint,C.POINTER(C.c_uint16),C.POINTER(C.c_uint8),C.c_uint,C.POINTER(C.c_uint)]
 class Legacy(C.Structure):
  _fields_=[('M',C.c_int),('g2_tab',C.c_int*137),('g4_tab',C.c_int*137),('z8_tab',C.c_int*138)]
 lib.legacy_encode.argtypes=[C.POINTER(Legacy),C.c_int,C.POINTER(C.c_int)]
 rng=random.Random(9103000);cases=0
 for baud,rows in profiles.items():
  p=15 if baud==3000 else 16
  for j,(b,swp,k,m,q) in enumerate(rows):
   rate=4800+2400*j;s=lib.create(rate,baud);assert s
   try:
    params=(C.c_uint*6)();lib.params(s,params);assert list(params)==[b,k,m,q,p,7]
    assert lib.carrier(s,0)==(1800 if baud==3000 else 12800/7)
    assert lib.carrier(s,1)==(2000 if baud==3000 else 1920)
    schedule=[lib.v90_mapping_frame_bits(s,i) for i in range(p)]
    assert schedule==[b-1+((swp>>(p-1-i))&1) for i in range(p)]
    assert sum(schedule)*25==rate
    for i in [p,7*p,2**64-1]:assert lib.v90_mapping_frame_bits(s,i)==schedule[i%p]
    oracle=Legacy();oracle.M=m;counts=[1]
    for width in range(1,9):
     nxt=[0]*(len(counts)+m-1)
     for a,v in enumerate(counts):
      for z in range(m):nxt[a+z]+=v
     counts=nxt
     if width in [2,4]:
      field=oracle.g2_tab if width==2 else oracle.g4_tab
      for a,v in enumerate(counts):field[a]=v
    for a,v in enumerate(counts):oracle.z8_tab[a+1]=oracle.z8_tab[a]+v
    pattern=[int(x) for x in '01110111111110']
    for pair in list(range(28*p*3))+[2**64-1]:
     pos=(pair+24*p)%(28*p)
     assert lib.v90_mapping_inversion(s,pair)==(pattern[pos//(2*p)] if pos%(2*p)==0 else 0)
    # Independent GPA recurrence, legacy shell encoder, geometric subset table.
    scrambled=[]
    for i in range(rate//25):scrambled.append(1^(scrambled[i-5] if i>=5 else 0)^(scrambled[i-23] if i>=23 else 0))
    quarter=[complex(x,y) for x in range(-63,66,4) for y in range(-63,66,4)]
    quarter.sort(key=lambda z:(int(z.real)**2+int(z.imag)**2,-z.imag))
    converter=[[0,0,1,1,8,8,9,9],[3,2,2,3,11,10,10,11],
     [5,5,4,4,13,13,12,12],[6,7,7,6,14,15,15,14],
     [8,8,9,9,0,0,1,1],[11,10,10,11,3,2,2,3],
     [13,13,12,12,5,5,4,4],[14,15,15,14,6,7,7,6]]
    def subset(label):
     z=quarter[label>>2]*(-1j)**(label&3)
     x=((int(z.real)+3)//2)&3;y=((int(z.imag)+3)//2)&3
     return ((x^y)&1)|((x&1)<<1)|((((x>>1)^(y>>1)^x^y)&1)<<2)
    reference=[];cursor=state=prev=0
    for f in range(p):
     actual_k=k-(schedule[f]<b);v=scrambled[cursor:cursor+schedule[f]];cursor+=schedule[f]
     rings=(C.c_int*8)();lib.legacy_encode(C.byref(oracle),sum(v[z]<<z for z in range(actual_k)),rings)
     for pair in range(4):
      at=actual_k+(3+2*q)*pair;a=(prev+v[at+1]+2*v[at+2])%4
      bb=(a+2*v[at]+((state&1)^int(f==0 and pair==0)))%4
      qa=sum(v[at+3+z]<<z for z in range(q));qb=sum(v[at+3+q+z]<<z for z in range(q))
      x=a+4*((rings[2*pair]<<q)|qa);y=bb+4*((rings[2*pair+1]<<q)|qb)
      reference.extend([x,y]);t=converter[subset(x)][subset(y)];u=state&1
      state=(state>>1)^(t&1)^(((t>>1)&1)<<1)^((((t>>1)&1)^u)<<2)^(u<<3);prev=a
    generated=(C.c_uint16*130)(*([65535]*130))
    assert lib.v90_mapping_b1(s,generated,8*p)==8*p
    assert list(generated)[:8*p]==reference and list(generated)[8*p:]==[65535]*(130-8*p)
    detector=lib.detector(rate,baud);assert detector and lib.detector_length(detector)==8*p
    try:
     gain=C.c_double(-99);phase=C.c_double(-99);score=C.c_double(-99)
     def feed(z):return lib.v90_qam8_b1_symbol(detector,z.real,z.imag,C.byref(gain),C.byref(phase),C.byref(score))
     for _ in range(17):assert not feed(0j)
     import cmath
     rotation=1.7*cmath.exp(.31j)
     for index,label in enumerate(reference):
      z=quarter[label>>2]*(-1j)**(label&3)*rotation
      match=feed(z)
      assert bool(match)==(index==len(reference)-1),(baud,rate,index,match)
     assert abs(gain.value-1.7)<1e-10 and abs(phase.value-.31)<1e-10 and abs(score.value-1)<1e-10
     assert not feed(complex(float('nan'),0)) and lib.detector_count(detector)==0
     assert not feed(complex(1e101,0)) and lib.detector_count(detector)==0
     # Reset forces acquisition to wait for a complete fresh B1.
     for index,label in enumerate(reference):
      assert bool(feed(quarter[label>>2]*(-1j)**(label&3)))==(index==len(reference)-1)
     assert not feed(complex(float('nan'),0))
     for index,label in enumerate(reference):
      z=quarter[label>>2]*(-1j)**(label&3)*rotation+complex(rng.gauss(0,.01),rng.gauss(0,.01))
      assert bool(feed(z))==(index==len(reference)-1)
     assert abs(gain.value-1.7)<.02 and abs(phase.value-.31)<.01 and score.value>.99
    finally:lib.destroy(detector)
    generated[:]=[65535]*130
    assert not lib.v90_mapping_b1(s,generated,8*p-1) and list(generated)==[65535]*130
    # Continue from the B1 encoder state through data-frame/superframe wraps.
    expected=[];at=0
    for n in schedule:expected.append(scrambled[at:at+n]);at+=n
    transmitted=list(reference)
    for f in range(p,4*p+20):
     n=schedule[f%p];actual_k=k-(n<b);v=[rng.randrange(2) for _ in range(n)];expected.append(v)
     rings=(C.c_int*8)();lib.legacy_encode(C.byref(oracle),sum(v[z]<<z for z in range(actual_k)),rings)
     for pair in range(4):
      at=actual_k+(3+2*q)*pair;a=(prev+v[at+1]+2*v[at+2])%4
      pos=(4*f+pair+24*p)%(28*p);inv=pattern[pos//(2*p)] if pos%(2*p)==0 else 0
      bb=(a+2*v[at]+((state&1)^inv))%4
      qa=sum(v[at+3+z]<<z for z in range(q));qb=sum(v[at+3+q+z]<<z for z in range(q))
      x=a+4*((rings[2*pair]<<q)|qa);y=bb+4*((rings[2*pair+1]<<q)|qb);transmitted.extend([x,y])
      t=converter[subset(x)][subset(y)];u=state&1
      state=(state>>1)^(t&1)^(((t>>1)&1)<<1)^((((t>>1)&1)^u)<<2)^(u<<3);prev=a
    for impaired,half in [(False,False),(True,False),(False,True),(True,True)]:
     received=[];sources=[]
     def deliver(ptr,n):
      received.append(list(ptr[:n]) if ptr else None);sources.append(lib.stream_source(stream))
     cb=callback(deliver)
     stream=lib.stream_create(rate,baud,cb);assert stream
     try:
      previous_point=0j
      for index,label in enumerate(transmitted):
       z=quarter[label>>2]*(-1j)**(label&3)
       midpoint=.5*(previous_point+z);previous_point=z
       if impaired:
        z=z*1.7*cmath.exp(1j*(.31+.0002*index))+complex(rng.gauss(0,.01),rng.gauss(0,.01))
        midpoint=midpoint*1.7*cmath.exp(1j*(.31+.0002*(index-.5)))
       if half:lib.stream_mid(stream,midpoint.real,midpoint.imag)
       assert lib.stream_symbol(stream,z.real,z.imag)>=0
      assert sources==list(range(7,8*len(received),8)),(baud,rate,sources[:3])
      assert len(received)>=4*p,(baud,rate,len(received))
      assert received==expected[:len(received)],(baud,rate,'stream mismatch',next((i for i,(a,bits) in enumerate(zip(received,expected)) if a!=bits),None))
     finally:lib.destroy(stream)
    previous=0
    for frame in range(7*p*3):
     n=schedule[frame%p];actual_k=k-(n<b)
     index=rng.randrange(1<<actual_k)
     if frame%7==0:index=(1<<actual_k)-1
     rings=(C.c_int*8)();lib.legacy_encode(C.byref(oracle),index,rings)
     expected=[(index>>i)&1 for i in range(actual_k)];labels=[];prev=previous
     for pair in range(4):
      a=rng.randrange(4);d=rng.randrange(4);bb=(a+d)%4
      qa=rng.randrange(1<<q);qb=rng.randrange(1<<q);diff=(a-prev)%4
      expected += [d>>1,diff&1,diff>>1]+[(qa>>i)&1 for i in range(q)]+[(qb>>i)&1 for i in range(q)]
      labels += [(rings[2*pair]<<(q+2))|(qa<<2)|a,(rings[2*pair+1]<<(q+2))|(qb<<2)|bb];prev=a
     wire=(C.c_uint16*8)(*labels);out=(C.c_uint8*80)(*([165]*80));nxt=C.c_uint(99)
     assert lib.v90_mapping_decode(s,frame,previous,wire,out,80,C.byref(nxt))==n
     assert list(out)[:n]==expected and list(out)[n:]==[165]*(80-n) and nxt.value==prev
     # Capacity and out-of-constellation rejection are transactional.
     for cap,bad in [(n-1,False),(80,True)]:
      if bad:wire[0]=4*m<<q
      out[:]=[165]*80;nxt.value=99
      assert not lib.v90_mapping_decode(s,frame,previous,wire,out,cap,C.byref(nxt))
      assert list(out)==[165]*80 and nxt.value==99
     # A low frame's inserted highest shell bit must be zero.
     if n<b:
      lib.legacy_encode(C.byref(oracle),1<<(k-1),rings)
      wire=(C.c_uint16*8)(*[x<<(q+2) for x in rings]);out[:]=[165]*80;nxt.value=99
      assert not lib.v90_mapping_decode(s,frame,previous,wire,out,80,C.byref(nxt))
      assert list(out)==[165]*80 and nxt.value==99
     previous=prev;cases+=1
   finally:lib.destroy(s)
 for rate,baud in [(0,3000),(4801,3000),(31200,3000),(33600,3200),(4800,3429)]:assert not lib.create(rate,baud)
 assert not lib.v90_mapping_frame_bits(None,0)
 print('PASS:',cases,'independent mapping frames; 23 profiles, published schedules, independent B1/stream bits, carrier/noise, inversion, boundaries and rejection')
