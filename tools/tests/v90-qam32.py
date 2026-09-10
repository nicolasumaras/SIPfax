#!/usr/bin/env python3
"""Independent V.34 Figure 5/9, Table 13 and shell-ordering test for 14400."""
import ctypes as C
from pathlib import Path
import random
import subprocess
import tempfile
import sys
from v90_shell_reference import ShellReference
root=Path(__file__).resolve().parents[2]
m,k,q_bits=(12,28,1) if '--19200' in sys.argv else (14,30,0) if '--16800' in sys.argv else (8,24,0)
frame_bits=k+12+8*q_bits
point_count=4*m*(1<<q_bits)
converter=[[0,0,1,1,8,8,9,9],[3,2,2,3,11,10,10,11],
 [5,5,4,4,13,13,12,12],[6,7,7,6,14,15,15,14],
 [8,8,9,9,0,0,1,1],[11,10,10,11,3,2,2,3],
 [13,13,12,12,5,5,4,4],[14,15,15,14,6,7,7,6]]
points=[z*(-1j)**q for z in [1+1j,-3+1j,1-3j,-3-3j,1+5j,5+1j,-3+5j,5-3j,5+5j,-7+1j,1-7j,-7-3j,-3-7j,-7+5j,5-7j,1+9j,9+1j,-3+9j,9-3j,-7-7j,5+9j,9+5j,-11+1j,1-11j][:m*(1<<q_bits)] for q in range(4)]
def subset(z):
    x=((int(z.real)+3)//2)&3;y=((int(z.imag)+3)//2)&3
    return ((x^y)&1)|((x&1)<<1)|((((x>>1)^(y>>1)^x^y)&1)<<2)
pattern=[int(x) for x in '01110111111110'];rng=random.Random(1440032)
oracle=ShellReference(m)
indices=sorted({0,2**k-1,*[x for x in oracle.starts if x<2**k],*[x-1 for x in oracle.starts if 0<x<=2**k],*[rng.randrange(2**k) for _ in range(4000)]})
with tempfile.TemporaryDirectory() as td:
    d=Path(td);w=d/'w.c';so=d/'x.so'
    w.write_text('''#include <stdlib.h>
#include "v90qam8.h"
void*trellis(void){V90Trellis*s=malloc(sizeof(*s));v90_trellis_init(s);return s;}
void*frames(unsigned previous){V90Qam32Frames*s=malloc(sizeof(*s));v90_qam32_frames_init(s,previous);return s;}
void*stream(void (*cb)(void*,const uint8_t*)) {V90Qam8Stream*s=malloc(sizeof(*s));v90_qam_stream_init_rate(s,14400);s->receive_bits=cb;return s;}
unsigned label(V90Qam8Stream*s,unsigned n){return s->b1.labels[n];}
unsigned long long output_symbol(V90Qam8Stream*s){return s->output_symbol;}
unsigned long long rejected(V90Qam8Stream*s){return s->rejected_frames;}
void destroy(void*s){free(s);}
unsigned long long count(V90Trellis*s){return s->pairs;}
''')
    if q_bits:w.write_text(w.read_text().replace('V90Qam32Frames','V90Qam96Frames').replace('v90_qam32_frames_init','v90_qam96_frames_init').replace('s,14400','s,19200'))
    elif m==14:w.write_text(w.read_text().replace('V90Qam32Frames','V90Qam56Frames').replace('v90_qam32_frames_init','v90_qam56_frames_init').replace('s,14400','s,16800'))
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC','-I'+str(root/'vendor/linmodem'),str(w),*[str(root/'vendor/linmodem'/n) for n in ['v90trellis.c','v90qam8.c','v90equalizer.c','v90shell.c']],'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.trellis.restype=lib.frames.restype=C.c_void_p;lib.frames.argtypes=[C.c_uint];lib.destroy.argtypes=[C.c_void_p]
    lib.count.argtypes=[C.c_void_p];lib.count.restype=C.c_ulonglong
    frame_fn=getattr(lib,'v90_qam%d_frame'%point_count)
    pair_fn=getattr(lib,'v90_trellis_qam%d_pair'%point_count)
    frame_fn.argtypes=lib.v90_qam20_frame.argtypes=[C.c_void_p,C.POINTER(C.c_uint8),C.POINTER(C.c_uint8)]
    pair_fn.argtypes=[C.c_void_p,*([C.c_double]*4),C.c_uint,C.POINTER(C.c_uint),C.POINTER(C.c_uint)]
    f=lib.frames(0);previous=0;seen=set()
    for index in indices:
        shell=oracle[index];bits=[(index>>i)&1 for i in range(k)]+[rng.randrange(2) for _ in range(12+8*q_bits)];labels=[]
        for p in range(4):
            g=k+(3+2*q_bits)*p
            a=(previous+bits[g+1]+2*bits[g+2])%4
            b=(a+2*bits[g]+rng.randrange(2))%4;previous=a
            qa=sum(bits[g+3+j]<<j for j in range(q_bits))
            qb=sum(bits[g+3+q_bits+j]<<j for j in range(q_bits))
            labels.extend([a+4*((shell[2*p]<<q_bits)|qa),b+4*((shell[2*p+1]<<q_bits)|qb)])
        seen.update(labels);out=(C.c_uint8*frame_bits)()
        assert frame_fn(f,(C.c_uint8*8)(*labels),out) and list(out)==bits,index
    assert seen==set(range(point_count))
    out=(C.c_uint8*frame_bits)(*([99]*frame_bits));assert not frame_fn(f,(C.c_uint8*8)(point_count,0,0,0,0,0,0,0),out) and list(out)==[99]*frame_bits
    small=(C.c_uint8*30)(*([99]*30));assert not lib.v90_qam20_frame(f,(C.c_uint8*8)(),small) and list(small)==[99]*30
    if m in [12,14]:
        # The first unused shell is rejected without overwriting output.
        out=(C.c_uint8*frame_bits)(*([99]*frame_bits))
        assert not frame_fn(f,(C.c_uint8*8)(*[4*(r<<q_bits) for r in oracle[1<<k]]),out)
        assert list(out)==[99]*frame_bits
    lib.destroy(f)
    for initial in range(16):
        encoder=initial;pairs=[];invbits=[]
        for n in range(1600):
            inv=pattern[(n//32)%14] if n%32==0 else 0
            a=rng.randrange(4);b=(a+2*rng.randrange(2)+((encoder&1)^inv))%4
            a+=4*rng.randrange(m*(1<<q_bits));b+=4*rng.randrange(m*(1<<q_bits));pairs.append((a,b));invbits.append(inv)
            y=converter[subset(points[a])][subset(points[b])];u=encoder&1
            encoder=(encoder>>1)^(y&1)^(((y>>1)&1)<<1)^((((y>>1)&1)^u)<<2)^(u<<3)
        for noisy in [False,True]:
            s=lib.trellis();decoded=[]
            for (a,b),inv in zip(pairs,invbits):
                x,y=points[a],points[b]
                if noisy:x+=complex(rng.gauss(0,.08),rng.gauss(0,.08));y+=complex(rng.gauss(0,.08),rng.gauss(0,.08))
                aa=C.c_uint(99);bb=C.c_uint(99);count=lib.count(s)
                for bad in [float('nan'),float('inf'),1e200]:
                    assert pair_fn(s,bad,0,0,0,inv,C.byref(aa),C.byref(bb))==-1 and lib.count(s)==count and aa.value==bb.value==99
                if pair_fn(s,x.real,x.imag,y.real,y.imag,inv,C.byref(aa),C.byref(bb)):decoded.append((aa.value,bb.value))
            assert decoded[64:]==pairs[64:len(decoded)],(initial,noisy)
            lib.destroy(s)
    cbtype=C.CFUNCTYPE(None,C.c_void_p,C.POINTER(C.c_uint8))
    lib.stream.argtypes=[cbtype];lib.stream.restype=C.c_void_p
    lib.label.argtypes=[C.c_void_p,C.c_uint]
    for name in ['output_symbol','rejected']:
        getattr(lib,name).argtypes=[C.c_void_p];getattr(lib,name).restype=C.c_ulonglong
    lib.v90_qam8_stream_symbol.argtypes=[C.c_void_p,C.c_double,C.c_double]
    register=0;bits=[]
    for plain in [1]*(16*frame_bits)+[rng.randrange(2) for _ in range(frame_bits*700)]:
        b=plain^((register>>22)&1);register=(register<<1)&0x7fffff
        if b:register^=1|(1<<18)
        bits.append(b)
    source=[bits[i:i+frame_bits] for i in range(0,len(bits),frame_bits)];labels=[];encoder=previous=0
    for n,bits in enumerate(source):
        rings=oracle[sum(bits[i]<<i for i in range(k))]
        for p in range(4):
            pair=4*n+p+384;inv=pattern[(pair//32)%14] if pair%32==0 else 0
            g=k+(3+2*q_bits)*p
            a=(previous+bits[g+1]+2*bits[g+2])%4
            b=(a+2*bits[g]+((encoder&1)^inv))%4;previous=a
            qa=sum(bits[g+3+j]<<j for j in range(q_bits))
            qb=sum(bits[g+3+q_bits+j]<<j for j in range(q_bits))
            a+=4*((rings[2*p]<<q_bits)|qa);b+=4*((rings[2*p+1]<<q_bits)|qb);labels.extend([a,b])
            y=converter[subset(points[a])][subset(points[b])];u=encoder&1
            encoder=(encoder>>1)^(y&1)^(((y>>1)&1)<<1)^((((y>>1)&1)^u)<<2)^(u<<3)
    import cmath
    for frequency in [-1,0,1]:
        output=[];positions=[]
        def receive(_,bits):output.append(list(bits[:frame_bits]) if bits else None);positions.append(lib.output_symbol(s))
        cb=cbtype(receive);s=lib.stream(cb)
        assert [lib.label(s,i) for i in range(128)]==labels[:128]
        for i,label in enumerate(labels):
            z=points[label]*(1+.06*i/len(labels))*cmath.exp(1j*(.7+2*cmath.pi*frequency*i/3200))
            z+=complex(rng.gauss(0,.02),rng.gauss(0,.02))
            assert lib.v90_qam8_stream_symbol(s,z.real,z.imag)==int(i>=127)
        assert output==source[:len(output)] and len(output)==(len(labels)-129)//8,frequency
        assert positions==[8*(i+1)-1 for i in range(len(output))] and lib.rejected(s)==0
        assert lib.v90_qam8_stream_symbol(s,float('nan'),0)==-1
        output.clear();positions.clear()
        for label in labels[:400]:
            z=points[label];lib.v90_qam8_stream_symbol(s,z.real,z.imag)
        assert output==source[:33] and positions[0]==len(labels)+8
        lib.destroy(s)
    print('PASS:',frame_bits*400,'B1, continuous 36-bit frames, gain/carrier/noise, source timing and reacquisition')
print(f'PASS: {point_count} points, all trellis states, noise, {frame_bits}-bit shell/differential mapping, energy-bucket boundaries and rejection bounds')
