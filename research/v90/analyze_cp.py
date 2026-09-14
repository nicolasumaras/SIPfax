#!/usr/bin/env python3
"""CRC-validate four-point upstream V.90 CP frames from a PCM recording."""
import sys
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'v34-rx'))
from v34_front import Rate,front_end
from analyze_training import descramble

def decode(path,start=17,end=20,fixture=None):
    x=np.fromfile(path,dtype='<i2').astype(float)
    rate=Rate(4,True);z=front_end(x[int(start*8000):int(end*8000)],rate,beta=.1)
    results=[];seen=set()
    for phase in range(rate.SPS):
        y=z[phase::rate.SPS]
        d=(-np.rint(np.angle(y[1:]*y[:-1].conj())/(np.pi/2)).astype(int))%4
        b=descramble(np.column_stack((d&1,d>>1)).reshape(-1))
        for i in range(len(b)-292):
            if not np.all(b[i:i+17]) or b[i+17]:continue
            a=b[i:];field=lambda s,n:sum(int(a[s+j])<<j for j in range(n))
            indices=[field(s,4) for s in [103,107,111,115,120,124]]
            if max(indices)>5:continue
            gamma=136*max(indices);delta=gamma if not a[128] else 2*gamma+136
            length=292+delta;cs=273+delta
            if len(a)<length or any(a[k] for k in range(17,cs,17)):continue
            c=0xffff
            for k in range(18,cs):
                if k%17==0:continue
                f=(c^int(a[k]))&1;c>>=1
                if f:c^=0x8408
            if c!=field(cs,16):continue
            masks=[]
            for j in range(max(indices)+1):
                masks.append([u for u in range(128) if a[137+j*136+(u//16)*17+u%16]])
            result={'time':start+(phase+(i/2+1)*rate.SPS)/rate.FSu,'phase':phase,'type':int(a[19]),'drn':field(20,5),'Sr':field(31,2),'ack':int(a[33]),'law':int(a[35]),'lookahead':field(49,2),'gain_q13':field(52,16),'filter':[field(s,8) for s in [69,77,86,94]],'indices':indices,'codec_masks':int(a[128]),'mask_sizes':[len(m) for m in masks],'masks':masks,'crc':hex(c),'length':length}
            results.append(result)
            if fixture:
                Path(fixture).write_bytes(bytes(a[:length]));fixture=None
            key=bytes(a[:length])
            if key not in seen:print(result);seen.add(key)
    return results
if __name__=='__main__':
    import argparse
    ap=argparse.ArgumentParser();ap.add_argument('recording');ap.add_argument('--start',type=float,default=17);ap.add_argument('--end',type=float,default=20);ap.add_argument('--fixture')
    a=ap.parse_args();result=decode(a.recording,a.start,a.end,a.fixture)
    print('CRC-valid frames:',len(result))
