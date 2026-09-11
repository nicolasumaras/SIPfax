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
void *create(unsigned rate,unsigned baud){V90Mapping*s=malloc(sizeof(*s));if(!s)return NULL;if(!v90_mapping_init(s,rate,baud)){free(s);return NULL;}return s;}
void destroy(void*s){free(s);}
void params(V90Mapping*s,unsigned*out){out[0]=s->b;out[1]=s->k;out[2]=s->m;out[3]=s->q;out[4]=s->p;out[5]=s->j;}
double carrier(V90Mapping*s,unsigned high){return high?s->high_carrier:s->low_carrier;}
typedef struct {int M,g2_tab[137],g4_tab[137],z8_tab[138];} V34DSPState;
'''+legacy+'''
void legacy_encode(V34DSPState*s,int index,int*rings){index_to_rings(s,(int(*)[2])rings,index);}
''')
 subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror','-I'+str(native),str(w),str(native/'v90mapping.c'),str(native/'v90shell.c'),'-o',str(so)],check=True)
 lib=C.CDLL(str(so));lib.create.argtypes=[C.c_uint,C.c_uint];lib.create.restype=C.c_void_p
 lib.destroy.argtypes=[C.c_void_p];lib.params.argtypes=[C.c_void_p,C.POINTER(C.c_uint)]
 lib.carrier.argtypes=[C.c_void_p,C.c_uint];lib.carrier.restype=C.c_double
 lib.v90_mapping_frame_bits.argtypes=[C.c_void_p,C.c_ulonglong]
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
 print('PASS:',cases,'independent mapping frames; 23 profiles, published schedules, boundaries and rejection')
