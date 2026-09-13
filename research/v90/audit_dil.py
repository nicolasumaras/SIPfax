#!/usr/bin/env python3
"""Compare private RX Ja and TX PCM captures against a complete requested DIL.

Prints training metadata only. Captures may contain PPP credentials; do not commit
or publish them. The expected waveform is built independently of v90train_tx.c.
Uses the first decoded Ja. Candidate mismatches can be normal DIL termination or
a later retrain with a different descriptor; they are not automatically defects.
An exact cycle proves sample content only, not start timing or analog delivery.
"""
import argparse
import ctypes as C
import json
from pathlib import Path
import subprocess
import tempfile
import numpy as np

class Dil(C.Structure):
    _fields_=[('n',C.c_uint),('lsp',C.c_uint),('ltp',C.c_uint),
              ('sp',C.c_uint8*128),('tp',C.c_uint8*128),('h',C.c_uint8*8),
              ('reference',C.c_uint8*8),('ucodes',C.c_uint8*255)]

def linear(ucode, alaw):
    # Decode the positive G.711 wire code, independently of the transmitter.
    if alaw:
        bits=((ucode ^ 0x55) | 0x80) ^ 0x55
        exponent=(bits >> 4)&7
        value=((bits&15)<<4)+8
        if exponent:value=(value+256) << (exponent-1)
        return value
    bits=(255-ucode)^255
    return (((bits&15)<<3)+132) * (1 << ((bits>>4)&7))-132

def audit(rx,tx,alaw=False):
    root=Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory() as directory:
        wrapper=Path(directory)/'ja.c';so=Path(directory)/'ja.so'
        wrapper.write_text('''#include <stdlib.h>
#include "v90training.h"
void *create(void){V90Training *s=malloc(sizeof(*s));if(s)v90_training_init(s);return s;}
void destroy(void *s){free(s);}
void descriptor(V90Training *s,V90Dil *d){*d=s->dil;}
''')
        sources=[str(root/'vendor/linmodem'/name) for name in
                 ['v90training.c','v90dil.c','v90cp.c']]
        subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Werror',
                        '-I'+str(root/'vendor/linmodem'),str(wrapper),*sources,
                        '-lm','-o',str(so)],check=True)
        lib=C.CDLL(str(so));lib.create.restype=C.c_void_p
        lib.destroy.argtypes=[C.c_void_p]
        lib.descriptor.argtypes=[C.c_void_p,C.POINTER(Dil)]
        lib.v90_training_receive.argtypes=[C.c_void_p,
            np.ctypeslib.ndpointer(dtype=np.int16,flags='C_CONTIGUOUS'),C.c_int]
        pcm=np.fromfile(rx,dtype='<i2');state=lib.create()
        if not state:raise MemoryError()
        try:
            for offset in range(0,min(len(pcm),40*8000),160):
                block=pcm[offset:offset+160]
                if lib.v90_training_receive(state,block,len(block)):break
            else:raise ValueError('No CRC-valid Ja in first 40 seconds')
            d=Dil();lib.descriptor(state,C.byref(d))
        finally:lib.destroy(state)
    if not d.n:raise ValueError('Caller requested no DIL')
    expected=[]
    for ucode in d.ucodes[:d.n]:
        chord=ucode//16
        for pos in range((d.h[chord]+1)*6):
            value=ucode if d.tp[pos%d.ltp] else d.reference[chord]
            expected.append(linear(value,alaw)*(1 if d.sp[pos%d.lsp] else -1))
    expected=np.asarray(expected,dtype='<i2')
    wire=Path(tx).read_bytes();prefix=expected[:96].tobytes()
    matches=[];cursor=0
    while True:
        start=wire.find(prefix,cursor)
        if start<0:break
        cursor=start+2
        if start%2 or start+len(expected)*2>len(wire):continue
        actual=np.frombuffer(wire,dtype='<i2',count=len(expected),offset=start)
        bad=np.flatnonzero(actual!=expected)
        matches.append({'start_seconds':start/16000,'mismatched_samples':len(bad),
                        'first_mismatch':int(bad[0]) if len(bad) else None})
    return {'segments':d.n,'cycle_samples':len(expected),
            'cycle_seconds':len(expected)/8000,'candidates':matches,
            'complete_cycle_match':any(m['mismatched_samples']==0 for m in matches)}

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('rx');p.add_argument('tx');p.add_argument('--alaw',action='store_true')
    a=p.parse_args();result=audit(a.rx,a.tx,a.alaw);print(json.dumps(result))
    raise SystemExit(0 if result['complete_cycle_match'] else 1)
