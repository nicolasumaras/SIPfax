#!/usr/bin/env python3
"""Offline ODP-assisted equalizer experiment; supplied alignment is an oracle.

Manifest: JSON list of {rate, baud, recording, start_bit, label}. Recording paths
resolve relative to the manifest. Echo-corrected signed little-endian PCM, 8 kHz.
This does not change production code or force decoded bits into PPP. It compares
native decoding with equalizer targets generated from a supplied detection start.
Never treat this non-causal diagnostic as a deployable receiver or conformance test.
GPL-2.0.
"""
from pathlib import Path
import ctypes as C,subprocess,tempfile,json,argparse
import numpy as np
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('manifest',type=Path)
parser.add_argument('output',type=Path)
parser.add_argument('--start',type=float,default=15)
parser.add_argument('--end',type=float,default=32)
args=parser.parse_args()
if not 0<=args.start<args.end:parser.error('require 0 <= start < end')
root=Path(__file__).resolve().parents[2]/'vendor/linmodem'
cases=[]
for row in json.loads(args.manifest.read_text()):
 rate,baud,start=row['rate'],row['baud'],row['start_bit']
 if baud not in (3000,3200) or rate<4800 or rate%2400 or rate>(28800 if baud==3000 else 31200):parser.error('unsupported profile')
 if not isinstance(start,int) or start<rate//25 or start>4000:parser.error('invalid supplied ODP start')
 cases.append((rate,baud,row['label'],start,(args.manifest.parent/row['recording']).resolve()))
results=[]
gen='unsigned reference_idle('+ (root/'v90mapping.c').read_text().split('unsigned v90_mapping_b1(')[1]
gen=gen.replace('const V90Mapping *s,uint16_t *labels,unsigned capacity)', 'const V90Mapping *s,uint16_t *labels,unsigned capacity,unsigned start)')
gen=gen.replace('unsigned state=0,previous=0;', 'unsigned state=0,previous=0,plain_index=0;')
gen=gen.replace('bits[i]=(uint8_t)(1^', 'unsigned pi=plain_index++,at=(pi-start)%48,by=at<24?17:145;at%=24;unsigned plain=pi<start?1:!at?0:at<=8?(by>>(at-1))&1:1;\n            bits[i]=(uint8_t)(plain^')
gen=gen.replace('capacity<8*s->p','capacity<4096').replace('out[128]','out[4096]').replace('f<s->p','f<512').replace('8*s->p*sizeof(*labels)','4096*sizeof(*labels)').replace('return 8*s->p','return 4096')
for limit in [0,128,600,1200,3000]:
 with tempfile.TemporaryDirectory() as td:
  d=Path(td);s=(root/'v90qam8.c').read_text()
  s='extern unsigned short oracle[4096];\n'+s
  needle='double r,i;point(j?early_b:early_a,&r,&i);';assert s.count(needle)==1
  s=s.replace(needle,needle+f'if(n<{limit})point(oracle[n],&r,&i);');(d/'q.c').write_text(s)
  (d/'w.c').write_text('''#include <stdlib.h>
#include <string.h>
#include "v90upstream.h"
unsigned short oracle[4096];
'''+gen+'''
void*create(unsigned rate,unsigned baud,unsigned start,void(*cb)(void*,const uint8_t*,unsigned)){V90Upstream*s=calloc(1,sizeof(*s));v90_upstream_init_profile(s,rate,baud,1);s->require_b1=1;s->receive_frame=cb;if(reference_idle(&s->qam[0].stream.mapping,oracle,4096,start)!=4096)abort();return s;}
void run(V90Upstream*s,const short*x,unsigned n){for(unsigned i=0;i<n;++i)v90_upstream_receive(s,x[i]);}
unsigned lcp(V90Upstream*s){return s->lcp_seen;}
void destroy(void*s){free(s);}
''')
  so=d/'x.so';subprocess.run(['gcc','-O2','-shared','-fPIC','-I'+str(root),str(d/'q.c'),str(d/'w.c'),*[str(root/f) for f in ['v90upstream.c','v90trellis.c','v90equalizer.c','v90shell.c','v90mapping.c','v42detect.c','v90odp.c']],'-lm','-o',str(so)],check=True)
  lib=C.CDLL(str(so));cbtype=C.CFUNCTYPE(None,C.c_void_p,C.POINTER(C.c_uint8),C.c_uint);lib.create.argtypes=[C.c_uint,C.c_uint,C.c_uint,cbtype];lib.create.restype=C.c_void_p;lib.run.argtypes=[C.c_void_p,C.c_void_p,C.c_uint];lib.destroy.argtypes=[C.c_void_p];lib.lcp.argtypes=[C.c_void_p]
  rows=[]
  for rate,baud,name,start,recording in cases:
   frames=[]
   def cb(_,p,n):
    data=bytes(p[:n]);frames.append({'bytes':n,'lcp':data[:4]==b'\xff\x03\xc0\x21','code':data[4] if len(data)>4 else None})
   callback=cbtype(cb);s=lib.create(rate,baud,start,callback);x=np.ascontiguousarray(np.fromfile(recording,dtype='<i2')[int(args.start*8000):int(args.end*8000)]);lib.run(s,x.ctypes.data,len(x));rows.append({'case':name,'frames':len(frames),'initial_lcp_seen':bool(lib.lcp(s)),'metadata':frames});lib.destroy(s)
  results.append({'supervised_symbols':limit,'cases':rows});print(limit,[(r['case'],r['frames'],r['initial_lcp_seen']) for r in rows],flush=True);args.output.write_text(json.dumps(results,indent=2))
