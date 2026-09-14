#!/usr/bin/env python3
"""Offline 4800-bit/s,3200-baud V.90 upstream demapping and PPP validation."""
import sys
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'v34-rx'))
from v34_front import Rate,front_end,clock_ppm,timing_offset

def descramble(b):
    b=np.asarray(b,dtype=np.uint8);p=b.copy();p[5:]^=b[:-5];p[23:]^=b[:-23];return p

def uart(bits):
    out=[];positions=[];i=0
    while i+10<=len(bits):
        if bits[i]==0 and bits[i+9]==1:
            out.append(sum(int(bits[i+1+k])<<k for k in range(8)));positions.append(i);i+=10
        else:i+=1
    return bytes(out),positions

def ppp_frames(data):
    found=[];buf=bytearray();esc=False
    for i,v in enumerate(data):
        if v==0x7e:
            if len(buf)>=4:
                crc=0xffff
                for b in buf:
                    crc^=b
                    for _ in range(8):crc=(crc>>1)^ (0x8408 if crc&1 else 0)
                if crc==0xf0b8:found.append((i,bytes(buf)))
            buf=bytearray();esc=False
        elif v==0x7d:esc=True
        else:buf.append(v^(0x20 if esc else 0));esc=False
    return found

def analyze(path,start=29,end=34):
    x=np.fromfile(path,dtype='<i2').astype(float);rate=Rate(4,True)
    z=front_end(x[int(start*8000):int(end*8000)],rate,beta=.1)
    ppm=clock_ppm(z,rate);off=timing_offset(z[:8000],rate)
    print('timing ppm',ppm,'offset',off)
    found=[]
    for delta in np.arange(-.2,.21,.05):
        pos=(off+delta)*5+np.arange(20,int(len(z)/5)-20)*5*(1+ppm*1e-6)
        y=np.interp(pos,np.arange(len(z)),z.real)+1j*np.interp(pos,np.arange(len(z)),z.imag)
        for pair in range(2):
            a=y[pair::2];b=y[pair+1::2];n=min(len(a),len(b));a=a[:n];b=b[:n]
            d=(-np.rint(np.angle(b*np.conj(a))/(np.pi/2)).astype(int))%4
            q=(-np.rint(np.angle(a[1:]*np.conj(a[:-1]))/(np.pi/2)).astype(int))%4
            bits=np.column_stack((d[1:]>>1,q&1,q>>1)).reshape(-1)
            plain=descramble(bits);data,positions=uart(plain);frames=ppp_frames(data)
            print('offset',round(delta,2),'pair',pair,'uart bytes',len(data),'flags',data.count(0x7e),'FCS valid',len(frames))
            for index,frame in frames:
                print('PPP time',start+positions[index]/4800,'length',len(frame),'header',frame[:8].hex());found.append(frame)
    return found
if __name__=='__main__':
    import argparse
    ap=argparse.ArgumentParser();ap.add_argument('recording');ap.add_argument('--start',type=float,default=29);ap.add_argument('--end',type=float,default=34)
    a=ap.parse_args();analyze(a.recording,a.start,a.end)
