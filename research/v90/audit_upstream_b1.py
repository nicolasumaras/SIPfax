#!/usr/bin/env python3
"""Audit known B1 labels and held-out distortion in an echo-corrected PCM recording.

Builds an instrumented receiver in a temporary directory. Reports aggregate errors
without decoded payloads; never alters the production receiver. NumPy and GCC required.
Frame counts are limited to the selected replay window, not whole-call qualification.
GPL-2.0.
"""
from pathlib import Path
import argparse,tempfile,subprocess,ctypes as C,json
import numpy as np
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('rate',type=int)
parser.add_argument('baud',type=int,choices=[3000,3200])
parser.add_argument('recording',type=Path)
parser.add_argument('output',type=Path)
parser.add_argument('--start',type=float,default=15)
parser.add_argument('--end',type=float,default=32)
args=parser.parse_args()
if not 0<=args.start<args.end:parser.error('require 0 <= start < end')
if args.rate<4800 or args.rate%2400 or args.rate>(28800 if args.baud==3000 else 31200):parser.error('unsupported profile')
root=Path(__file__).resolve().parents[2]/'vendor'/'linmodem'
rate,baud=args.rate,args.baud
with tempfile.TemporaryDirectory() as td:
 d=Path(td);q=d/'qam.c';w=d/'w.c';so=d/'test.so'
 src=(root/'v90qam8.c').read_text()
 needle='static void qam8_locked('
 src=src.replace(needle,'extern void symbol_sink(void*,double,double);\nextern void label_sink(void*,unsigned,unsigned);\n'+needle)
 src=src.replace('double ar,ai;if(!normalized(s,re,im,&ar,&ai))return;','double ar,ai;if(!normalized(s,re,im,&ar,&ai))return;symbol_sink(s,ar,ai);')
 assert src.count('    if(ready!=1)return;')==1
 src=src.replace('    if(ready!=1)return;','    if(ready!=1)return;label_sink(s,a,b);')
 src+='\nvoid reference_points(const uint16_t *v,double *r,double *i,unsigned n){for(unsigned j=0;j<n;++j)point(v[j],r+j,i+j);}\n'
 q.write_text(src)
 w.write_text('''#include <stdlib.h>
#include <string.h>
#include "v90upstream.h"
static V90Upstream *live;static unsigned nc[10],ns[10],frames;static long first_frame;static uint16_t labels[10][256];static double re[10][256],im[10][256];
void symbol_sink(void*s,double r,double i){for(unsigned j=0;j<10;++j)if(s==&live->qam[j].stream && ns[j]<256){unsigned n=ns[j]++;re[j][n]=r;im[j][n]=i;}}
void label_sink(void*s,unsigned a,unsigned b){for(unsigned j=0;j<10;++j)if(s==&live->qam[j].stream && nc[j]<256){labels[j][nc[j]++]=a;labels[j][nc[j]++]=b;}}
static void frame(void*o,const uint8_t*b,unsigned n){(void)o;(void)b;(void)n;if(!frames)first_frame=live->samples;++frames;}
void *create(unsigned rate,unsigned baud){live=calloc(1,sizeof(*live));if(!v90_upstream_init_profile(live,rate,baud,1))abort();live->require_b1=1;live->receive_frame=frame;return live;}
void run(V90Upstream*s,const short*x,unsigned n){for(unsigned i=0;i<n;++i)v90_upstream_receive(s,x[i]);}
unsigned reference(V90Upstream*s,uint16_t*v){return v90_mapping_b1(&s->qam[0].stream.mapping,v,128);}
unsigned get(unsigned lane,uint16_t*v,double*r,double*i){memcpy(v,labels[lane],sizeof(labels[lane]));memcpy(r,re[lane],sizeof(re[lane]));memcpy(i,im[lane],sizeof(im[lane]));return nc[lane];}
unsigned frame_count(void){return frames;}
unsigned lcp_seen(void){return live->lcp_seen;}
long first_frame_sample(void){return first_frame;}
long b1_sample(void){return live->b1_sample;}
void destroy(void*s){free(s);}
''')
 subprocess.run(['gcc','-shared','-fPIC','-O2','-I'+str(root),str(w),str(q),*[str(root/f) for f in ['v90upstream.c','v90trellis.c','v90equalizer.c','v90shell.c','v90mapping.c']],'-lm','-o',str(so)],check=True)
 lib=C.CDLL(str(so));lib.create.argtypes=[C.c_uint,C.c_uint];lib.create.restype=C.c_void_p
 lib.run.argtypes=[C.c_void_p,C.c_void_p,C.c_uint];lib.destroy.argtypes=[C.c_void_p];lib.reference.argtypes=[C.c_void_p,C.POINTER(C.c_uint16)]
 lib.get.argtypes=[C.c_uint,C.POINTER(C.c_uint16),C.POINTER(C.c_double),C.POINTER(C.c_double)]
 lib.reference_points.argtypes=[C.POINTER(C.c_uint16),C.POINTER(C.c_double),C.POINTER(C.c_double),C.c_uint]
 lib.first_frame_sample.restype=C.c_long;lib.b1_sample.restype=C.c_long
 s=lib.create(rate,baud);x=np.ascontiguousarray(np.fromfile(args.recording,dtype='<i2')[int(args.start*8000):int(args.end*8000)]);lib.run(s,x.ctypes.data,len(x))
 ref=(C.c_uint16*128)();n=lib.reference(s,ref);rr=(C.c_double*128)();ri=(C.c_double*128)();lib.reference_points(ref,rr,ri,n)
 target=np.asarray(rr)[:n]+1j*np.asarray(ri)[:n];rows=[]
 for lane in range(10):
  lab=(C.c_uint16*256)();r=(C.c_double*256)();i=(C.c_double*256)();count=lib.get(lane,lab,r,i)
  if count<n:continue
  observed=np.asarray(r)[:n]+1j*np.asarray(i)[:n];wrong=np.flatnonzero(np.asarray(lab)[:n]!=np.asarray(ref)[:n]).tolist();error=abs(observed-target)**2
  models={}
  scale=float(np.sqrt(np.mean(abs(observed[7:87])**2)))
  z=observed/scale
  features={'linear':np.column_stack([z,np.ones(n)]),'widely_linear':np.column_stack([z,np.conj(z),np.ones(n)]),'radial_cubic':np.column_stack([z,z*abs(z)**2,np.ones(n)]),'combined':np.column_stack([z,np.conj(z),z*abs(z)**2,np.ones(n)])}
  for name,X in features.items():
   fit=slice(7,87);test=slice(87,n-7)
   coef=np.linalg.lstsq(X[fit],target[fit],rcond=None)[0]
   residual=abs(X@coef-target)**2
   models[name]={'fit_mse':float(np.mean(residual[fit])),'heldout_mse':float(np.mean(residual[test]))}
  rows.append({'bit_error_counts':[int(np.count_nonzero(((np.asarray(lab)[:n]^np.asarray(ref)[:n])>>bit)&1)) for bit in range(11)],'models':models,'lane':lane,'wrong_b1_labels':wrong,'wrong_interior':sum(7<=p<n-7 for p in wrong),'mse_fit':float(np.mean(error[7:87])),'mse_heldout':float(np.mean(error[87:n-7]))})
 result={'rate':rate,'baud':baud,'replayed_seconds':[args.start,args.end],'crc_frames':lib.frame_count(),'initial_lcp_seen':bool(lib.lcp_seen()),'b1_symbols':n,'first_frame_sample':lib.first_frame_sample(),'b1_sample':lib.b1_sample(),'lanes':rows}
 args.output.write_text(json.dumps(result,indent=2));print(json.dumps(result));lib.destroy(s)
