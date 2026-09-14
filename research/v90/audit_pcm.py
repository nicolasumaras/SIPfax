#!/usr/bin/env python3
"""Audit a stable V.90 downstream PCM interval (mu-law, Sr=1 only).

The CP JSON is a list of {"frames": [...]} windows. A data CP entry has
"type": 1, "sr", "alaw", "drn", six "indices", and "masks": two lists of
constellations, each a list of enabled Ucodes. Choose an interval entirely
within one data-mode constellation, excluding training and renegotiation.

Independently invert magnitude mapping, spectral sign coding and GPC, then
check asynchronous PPP FCS. The first partial frame is ignored. Output is
metadata only: no decoded payloads or authentication exchanges are emitted.
This diagnoses generated PCM, not analogue reception or full conformance.
"""
import argparse,json,sys,collections
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_upstream import uart
ap=argparse.ArgumentParser();ap.add_argument('recording');ap.add_argument('cp_json');ap.add_argument('--window',type=int,default=0);ap.add_argument('--start',type=float,required=True);ap.add_argument('--end',type=float,required=True);ap.add_argument('--output',required=True);ap.add_argument('--require-clean',action='store_true');a=ap.parse_args()
if not (np.isfinite(a.start) and np.isfinite(a.end) and 0 <= a.start < a.end):ap.error('Require finite 0 <= start < end')
if round(a.end*8000)*2 > Path(a.recording).stat().st_size:ap.error('Interval exceeds recording length')
cp=next(c for c in json.loads(Path(a.cp_json).read_text())[a.window]['frames'] if c['type']==1)
if cp['sr']!=1 or cp['alaw']!=0:ap.error('Only Sr=1 mu-law data is supported')
x=np.fromfile(a.recording,dtype='<i2')[round(a.start*8000):round(a.end*8000)].astype(np.int32)
lookup=[];masks=cp['masks'][0]
if any(0 in masks[idx] for idx in cp['indices']):ap.error('Signed PCM cannot distinguish the two mu-law zero signs')
for idx in cp['indices']:
 lut=np.full(32769,-1,dtype=np.int64)
 for label,u in enumerate(sorted(masks[idx],reverse=True)):lut[((u%16*8+132)<<(u//16))-132]=label
 lookup.append(lut)
results=[]
for offset in range(6):
 pcm=x[offset:offset+(len(x)-offset)//6*6].reshape(-1,6)
 labels=np.column_stack([lookup[j][abs(pcm[:,j])] for j in range(6)])
 bad=int(np.count_nonzero(labels<0))
 if bad:results.append({'offset':offset,'invalid_constellation_samples':bad});continue
 t=np.sum((pcm>0)*(1<<np.arange(6)),axis=1).astype(np.uint8);p=t^np.r_[0,t[:-1]];q=np.bitwise_xor.accumulate(p&1);prior=np.r_[0,q[:-1]]
 p^=np.array([0,0x55,0xff,0xaa],dtype=np.uint8)[(q<<1)|prior]
 signs=(p[:,None]>>np.arange(1,6))&1
 odd=signs[:,[0,2,4]].reshape(-1);signs[:,[0,2,4]]=(odd^np.r_[0,odd[:-1]]).reshape(-1,3)
 value=np.zeros(len(pcm),dtype=np.int64)
 for j in range(5,-1,-1):value=value*len(masks[cp['indices'][j]])+labels[:,j]
 k=cp['drn']+20-5
 bits=np.column_stack([signs,(value[:,None]>>np.arange(k))&1]).reshape(-1).astype(np.uint8)
 plain=bits.copy();plain[18:]^=bits[:-18];plain[23:]^=bits[:-23]
 data,pos=uart(plain);buf=bytearray();esc=False;framed=False;good=badfcs=0;bad_times=[];stats=collections.Counter()
 for i,v in enumerate(data):
  if v==0x7e:
   if framed and len(buf)>=4:
    crc=0xffff
    for b in buf:
     crc^=b
     for _ in range(8):crc=(crc>>1)^(0x8408 if crc&1 else 0)
    if crc==0xf0b8:
     good+=1;f=buf[2:] if buf[:2]==b'\xff\x03' else buf;proto=f[0] if f[0]&1 else int.from_bytes(f[:2],'big');stats[hex(proto)]+=1
    else:badfcs+=1;bad_times.append(round(a.start+(offset+pos[i]/(k+5)*6)/8000,6))
   buf=bytearray();esc=False;framed=True
  elif v==0x7d:esc=True
  else:buf.append(v^(0x20 if esc else 0));esc=False
 results.append({'offset':offset,'invalid_constellation_samples':0,'uart_bytes':len(data),'valid_frames':good,'invalid_fcs_frames':badfcs,'invalid_fcs_times':bad_times,'protocol_counts':dict(stats),'out_of_range_mapping_frames':int(np.count_nonzero(value>=(1<<k)))})
 print(results[-1],flush=True)
Path(a.output).write_text(json.dumps({'start':a.start,'end':a.end,'results':results},indent=2)+'\n')

if a.require_clean:
 clean=[r for r in results if r.get('valid_frames',0)>0 and r.get('invalid_fcs_frames')==0 and r.get('out_of_range_mapping_frames')==0]
 if not clean:raise SystemExit('No clean data alignment found')
