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
m,k,q_bits=(10,26,5) if "--31200" in sys.argv else (12,28,4) if "--28800" in sys.argv else (14,30,3) if "--26400" in sys.argv else (8,24,3) if '--24000' in sys.argv else (10,26,2) if '--21600' in sys.argv else (12,28,1) if '--19200' in sys.argv else (14,30,0) if '--16800' in sys.argv else (8,24,0)
frame_bits=k+12+8*q_bits
point_count=4*m*(1<<q_bits)
converter=[[0,0,1,1,8,8,9,9],[3,2,2,3,11,10,10,11],
 [5,5,4,4,13,13,12,12],[6,7,7,6,14,15,15,14],
 [8,8,9,9,0,0,1,1],[11,10,10,11,3,2,2,3],
 [13,13,12,12,5,5,4,4],[14,15,15,14,6,7,7,6]]
# Figure 5: energy-ordered quarter lattice, ties from greatest imaginary value.
quarter=[complex(x,y) for x in range(-63,66,4) for y in range(-63,66,4)]
quarter.sort(key=lambda z:(int(z.real)**2+int(z.imag)**2,-z.imag))
points=[z*(-1j)**q for z in quarter[:m*(1<<q_bits)] for q in range(4)]

def subset(z):
    x=((int(z.real)+3)//2)&3;y=((int(z.imag)+3)//2)&3
    return ((x^y)&1)|((x&1)<<1)|((((x>>1)^(y>>1)^x^y)&1)<<2)
pattern=[int(x) for x in '01110111111110'];rng=random.Random(1440032)
oracle=ShellReference(m)
indices=sorted({0,2**k-1,*[x for x in oracle.starts if x<2**k],*[x-1 for x in oracle.starts if 0<x<=2**k],*[rng.randrange(2**k) for _ in range(4000)]})
with tempfile.TemporaryDirectory() as td:
    d=Path(td);w=d/'w.c';so=d/'x.so'
    w.write_text('''#include <stdlib.h>
#include <string.h>
#include "v90qam8.h"
int peek(V90Trellis*s,unsigned age,unsigned bits,unsigned*a,unsigned*b) {
 V90Trellis old=*s;int ready=v90_trellis_peek(s,age,bits,a,b);
 return memcmp(s,&old,sizeof(old))?-1:ready;
}
void*trellis(void){V90Trellis*s=malloc(sizeof(*s));v90_trellis_init(s);return s;}
void*frames(unsigned previous){V90Qam32Frames*s=malloc(sizeof(*s));v90_qam32_frames_init(s,previous);return s;}
void*stream(void (*cb)(void*,const uint8_t*)) {V90Qam8Stream*s=malloc(sizeof(*s));v90_qam_stream_init_rate(s,14400);s->receive_bits=cb;return s;}
unsigned label(V90Qam8Stream*s,unsigned n){return s->b1.labels[n];}
unsigned long long output_symbol(V90Qam8Stream*s){return s->output_symbol;}
unsigned long long rejected(V90Qam8Stream*s){return s->rejected_frames;}
void destroy(void*s){free(s);}
unsigned long long count(V90Trellis*s){return s->pairs;}
''')
    if "--31200" in sys.argv:w.write_text(w.read_text().replace("V90Qam32Frames","V90Qam1280Frames").replace("v90_qam32_frames_init","v90_qam1280_frames_init").replace("s,14400","s,31200"))
    elif "--28800" in sys.argv:w.write_text(w.read_text().replace("V90Qam32Frames","V90Qam768Frames").replace("v90_qam32_frames_init","v90_qam768_frames_init").replace("s,14400","s,28800"))
    elif "--26400" in sys.argv:w.write_text(w.read_text().replace("V90Qam32Frames","V90Qam448Frames").replace("v90_qam32_frames_init","v90_qam448_frames_init").replace("s,14400","s,26400"))
    elif q_bits==3:w.write_text(w.read_text().replace('V90Qam32Frames','V90Qam256Frames').replace('v90_qam32_frames_init','v90_qam256_frames_init').replace('s,14400','s,24000'))
    elif q_bits==2:w.write_text(w.read_text().replace('V90Qam32Frames','V90Qam160Frames').replace('v90_qam32_frames_init','v90_qam160_frames_init').replace('s,14400','s,21600'))
    elif q_bits:w.write_text(w.read_text().replace('V90Qam32Frames','V90Qam96Frames').replace('v90_qam32_frames_init','v90_qam96_frames_init').replace('s,14400','s,19200'))
    elif m==14:w.write_text(w.read_text().replace('V90Qam32Frames','V90Qam56Frames').replace('v90_qam32_frames_init','v90_qam56_frames_init').replace('s,14400','s,16800'))
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC','-I'+str(root/'vendor/linmodem'),str(w),*[str(root/'vendor/linmodem'/n) for n in ['v90trellis.c','v90qam8.c','v90equalizer.c','v90shell.c','v90mapping.c','v42detect.c','v90odp.c']],'-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.trellis.restype=lib.frames.restype=C.c_void_p;lib.frames.argtypes=[C.c_uint];lib.destroy.argtypes=[C.c_void_p]
    lib.count.argtypes=[C.c_void_p];lib.count.restype=C.c_ulonglong
    frame_fn=getattr(lib,'v90_qam%d_frame'%point_count)
    pair_fn=getattr(lib,'v90_trellis_qam%d_pair'%point_count)
    label_type=C.c_uint16 if point_count>256 else C.c_uint8
    frame_fn.argtypes=[C.c_void_p,C.POINTER(label_type),C.POINTER(C.c_uint8)]
    lib.v90_qam20_frame.argtypes=[C.c_void_p,C.POINTER(C.c_uint8),C.POINTER(C.c_uint8)]
    pair_fn.argtypes=[C.c_void_p,*([C.c_double]*4),C.c_uint,C.POINTER(C.c_uint),C.POINTER(C.c_uint)]
    lib.peek.argtypes=[C.c_void_p,C.c_uint,C.c_uint,C.POINTER(C.c_uint),C.POINTER(C.c_uint)]
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
        assert frame_fn(f,(label_type*8)(*labels),out) and list(out)==bits,index
    assert seen==set(range(point_count))
    if point_count<1<<(8*C.sizeof(label_type)):
        out=(C.c_uint8*frame_bits)(*([99]*frame_bits));assert not frame_fn(f,(label_type*8)(point_count,0,0,0,0,0,0,0),out) and list(out)==[99]*frame_bits
    small=(C.c_uint8*30)(*([99]*30));assert not lib.v90_qam20_frame(f,(C.c_uint8*8)(),small) and list(small)==[99]*30
    if m in [10,12,14]:
        # The first unused shell is rejected without overwriting output.
        out=(C.c_uint8*frame_bits)(*([99]*frame_bits))
        assert not frame_fn(f,(label_type*8)(*[4*(r<<q_bits) for r in oracle[1<<k]]),out)
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
                ready=pair_fn(s,x.real,x.imag,y.real,y.imag,inv,C.byref(aa),C.byref(bb))
                if ready:decoded.append((aa.value,bb.value))
                pa=C.c_uint(999);pb=C.c_uint(999);width=(point_count-1).bit_length()
                for age,bits in [(64,width),(0,1),(0,12),(count+1,width)]:
                    assert lib.peek(s,age,bits,C.byref(pa),C.byref(pb))==0 and pa.value==pb.value==999
                early=lib.peek(s,8,width,C.byref(pa),C.byref(pb))
                assert early==int(count>=8)
                if count>=72:assert (pa.value,pb.value)==pairs[count-8],(initial,noisy,count)
                if ready:
                    assert lib.peek(s,63,width,C.byref(pa),C.byref(pb))==1
                    assert (pa.value,pb.value)==(aa.value,bb.value)

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
    print('PASS:',frame_bits*400,f'B1, continuous {frame_bits}-bit frames, gain/carrier/noise, source timing and reacquisition')
print(f'PASS: {point_count} points, all trellis states, noise, {frame_bits}-bit shell/differential mapping, energy-bucket boundaries and rejection bounds')
