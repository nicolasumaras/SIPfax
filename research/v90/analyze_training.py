#!/usr/bin/env python3
"""Offline V.90 upstream PP/Ja analysis at the negotiated V.34 rate."""
import sys
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'v34-rx'))
from v34_front import Rate,front_end

def descramble(bits):
    reg=0;out=[]
    for b in bits:
        out.append(((reg>>22)^int(b))&1)
        reg=(reg<<1)&((1<<23)-1)
        if b:reg^=1|(1<<18)
    return np.array(out,dtype=np.uint8)

def analyze(path,start=7.0,end=12.0,rate_index=4,descriptor=None):
    x=np.fromfile(path,dtype='<i2').astype(float)
    rate=Rate(rate_index,True)
    z=front_end(x[int(start*8000):int(end*8000)],rate,beta=.1)
    pp=np.array([np.exp(2j*np.pi*((k*i+(4 if k%3==1 else 0))%12)/12) for k in range(12) for i in range(4)])
    ref=np.tile(pp,3);best=None
    for phase in range(rate.SPS):
        y=z[phase::rate.SPS]
        c=abs(np.correlate(y,ref,'valid'))/np.sqrt(np.convolve(abs(y)**2,np.ones(len(ref)),'valid')*len(ref)+1e-20)
        k=int(np.argmax(c))
        if best is None or c[k]>best[0]:best=(float(c[k]),phase,k)
    score,phase,k=best
    print('PP correlation',score,'time',start+(phase+k*rate.SPS)/rate.FSu,'sampling phase',phase)
    y=z[phase::rate.SPS]
    d=(-np.rint(np.angle(y[1:]*y[:-1].conj())/(np.pi/2)).astype(int))%4
    bits=np.column_stack((d&1,d>>1)).reshape(-1)
    plain=descramble(bits)
    found=[]
    for i in range(len(plain)-400):
        if np.all(plain[i:i+17]) and plain[i+17]==0:
            b=plain[i:]
            field=lambda st,n:sum(int(v)<<j for j,v in enumerate(b[st:st+n]))
            if b[34] or b[51]:continue
            found.append((start+(phase+(i/2+1)*rate.SPS)/rate.FSu,field(18,8),field(35,7)+1,field(43,7)+1,i))
    print('Ja sync candidates (time,N,LSP,LTP,bit)',found[:30])
    for time,N,lsp,ltp,index in found:
        b=plain[index:];alpha=((lsp+15)//16)*17;beta=alpha+((ltp+15)//16)*17
        crc_start=188+beta+((N+1)//2)*17
        if len(b)<crc_start+16:continue
        if any(b[k] for k in range(17,crc_start,17)):continue
        c=0xffff
        for k in range(18,crc_start):
            if k%17==0:continue
            feedback=(c^int(b[k]))&1;c>>=1
            if feedback:c^=0x8408
        received=sum(int(b[crc_start+j])<<j for j in range(16))
        print('Ja CRC',hex(c),hex(received),'valid',c==received,'time',time,'length',crc_start+16)
        if c==received:
            def field(st,n):return sum(int(b[st+j])<<j for j in range(n))
            H=[field(52+beta+(j//2)*17+(j%2)*8,7) for j in range(8)]
            REF=[field(120+beta+(j//2)*17+(j%2)*8,7) for j in range(8)]
            U=[field(188+beta+(j//2)*17+(j%2)*8,7) for j in range(N)]
            print('DIL H',H,'REF',REF,'segments',len(U))
            if descriptor is not None:
                length=(crc_start+18)&~1
                Path(descriptor).write_bytes(bytes(b[:length]))
                descriptor=None

    return plain,found
if __name__=='__main__':
    import argparse
    ap=argparse.ArgumentParser();ap.add_argument('recording');ap.add_argument('--start',type=float,default=7)
    ap.add_argument('--end',type=float,default=12);ap.add_argument('--descriptor')
    args=ap.parse_args();analyze(args.recording,args.start,args.end,descriptor=args.descriptor)
