#!/usr/bin/env python3
"""Independent 8-QAM transmitter -> C trellis -> 7200-bit/s frame bits."""
import cmath
import ctypes as C
import itertools
from pathlib import Path
import random
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
# Full Table 13, transcribed independently; the 16-state encoder uses Y2/Y1.
converter=[
 [0,0,1,1,8,8,9,9],[3,2,2,3,11,10,10,11],
 [5,5,4,4,13,13,12,12],[6,7,7,6,14,15,15,14],
 [8,8,9,9,0,0,1,1],[11,10,10,11,3,2,2,3],
 [13,13,12,12,5,5,4,4],[14,15,15,14,6,7,7,6]]
rings=sorted(itertools.product(range(2),repeat=8),key=lambda r:
 (sum(r),sum(r[:4]),sum(r[4:6]),r[6],r[4],sum(r[:2]),r[2],r[0]))[:64]
pattern=[int(x) for x in '01110111111110']
rng=random.Random(907200)

def transmit(frames,state,previous,source_bits=None,pair_offset=0):
    labels=[];inversions=[];source=[]
    for frame in range(frames):
        bits=([rng.randrange(2) for _ in range(18)] if source_bits is None else source_bits[18*frame:18*frame+18]);source.append(bits)
        shell=rings[sum(bits[i]<<i for i in range(6))]
        for j in range(4):
            i=4*frame+j+pair_offset
            inversion=pattern[(i//32)%14] if i%32==0 else 0
            a=(previous+bits[7+3*j]+2*bits[8+3*j])%4
            b=(a+2*bits[6+3*j]+((state&1)^inversion))%4
            previous=a
            x=a+4*shell[2*j];y=b+4*shell[2*j+1]
            labels.append((x,y));inversions.append(inversion)
            v=converter[x][y];u=state&1
            state=(state>>1)^(v&1)^(((v>>1)&1)<<1)^((((v>>1)&1)^u)<<2)^(u<<3)
    return labels,inversions,source

def point(label):return [1+1j,-3+1j][label>>2]*(-1j)**(label&3)

with tempfile.TemporaryDirectory() as directory:
    d=Path(directory);w=d/'wrapper.c';so=d/'qam8.so'
    w.write_text('''#include <stdlib.h>
#include "v90trellis.h"
#include "v90qam8.h"
void *trellis(void){V90Trellis*s=malloc(sizeof(*s));v90_trellis_init(s);return s;}
void *frames(unsigned previous){V90Qam8Frames*s=malloc(sizeof(*s));v90_qam8_frames_init(s,previous);return s;}
void destroy(void*s){free(s);}
void *frames12(unsigned previous){V90Qam12Frames*s=malloc(sizeof(*s));v90_qam12_frames_init(s,previous);return s;}
void *frames20(unsigned previous){V90Qam20Frames*s=malloc(sizeof(*s));v90_qam20_frames_init(s,previous);return s;}
void *b1(void){V90Qam8B1*s=malloc(sizeof(*s));v90_qam8_b1_init(s);return s;}
unsigned b1_label(V90Qam8B1*s,unsigned i){return s->labels[i];}
void *stream(void (*cb)(void*,const uint8_t*)) {
 V90Qam8Stream*s=malloc(sizeof(*s));v90_qam8_stream_init(s);s->receive_bits=cb;return s;
}
void *stream_rate(unsigned rate,void (*cb)(void*,const uint8_t*)) {
 V90Qam8Stream*s=malloc(sizeof(*s));if(!v90_qam_stream_init_rate(s,rate)){free(s);return NULL;}s->receive_bits=cb;return s;
}
void *b1_rate(unsigned rate){V90Qam8B1*s=malloc(sizeof(*s));if(!v90_qam_b1_init_rate(s,rate)){free(s);return NULL;}return s;}
uint64_t output_symbol(V90Qam8Stream*s){return s->output_symbol;}
uint64_t rejected(V90Qam8Stream*s){return s->rejected_frames;}
uint64_t count(V90Trellis*s){return s->pairs;}
''')
    subprocess.run(['gcc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',
        '-I'+str(root/'vendor/linmodem'),str(w),
        *[str(root/'vendor/linmodem'/x) for x in ['v90trellis.c','v90qam8.c','v90shell.c']],
        '-lm','-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.trellis.restype=lib.frames.restype=C.c_void_p
    lib.frames.argtypes=[C.c_uint];lib.destroy.argtypes=[C.c_void_p]
    lib.count.argtypes=[C.c_void_p];lib.count.restype=C.c_uint64
    lib.v90_trellis_qam8_pair.argtypes=[C.c_void_p,*([C.c_double]*4),C.c_uint,C.POINTER(C.c_uint),C.POINTER(C.c_uint)]
    lib.v90_qam8_frame.argtypes=[C.c_void_p,C.POINTER(C.c_uint8),C.POINTER(C.c_uint8)]
    hard_errors=0
    for initial in range(16):
        previous=initial%4
        labels,inversions,source=transmit(180,initial,previous)
        for disturb in [False,True]:
            s=lib.trellis();decoded=[]
            for i,((a,b),inversion) in enumerate(zip(labels,inversions)):
                values=[point(a),point(b)]
                if disturb:
                    values=[v+complex(rng.gauss(0,.04),rng.gauss(0,.04)) for v in values]
                    if 100<i<len(labels)-100 and i%101==0 and a<4:
                        values[0]*=cmath.exp(1j*55*cmath.pi/180)
                    hard_errors+=sum(min(range(8),key=lambda q:abs(v-point(q)))!=x for v,x in zip(values,[a,b]))
                aa=C.c_uint(99);bb=C.c_uint(99)
                if i==150:
                    old=lib.count(s)
                    for bad in [float('nan'),float('inf'),1e200]:
                        assert lib.v90_trellis_qam8_pair(s,bad,0,0,0,0,C.byref(aa),C.byref(bb))==-1
                        assert lib.count(s)==old and aa.value==bb.value==99
                x,y=values
                result=lib.v90_trellis_qam8_pair(s,x.real,x.imag,y.real,y.imag,inversion,C.byref(aa),C.byref(bb))
                assert bool(result)==(i>=63)
                if result:decoded.append((aa.value,bb.value))
            assert decoded==labels[:len(decoded)],(initial,disturb,next((i for i,(x,y) in enumerate(zip(decoded,labels)) if x!=y),None))
            f=lib.frames(previous)
            flat=[v for pair in decoded for v in pair]
            for i in range(len(flat)//8):
                out=(C.c_uint8*18)();packet=(C.c_uint8*8)(*flat[8*i:8*i+8])
                assert lib.v90_qam8_frame(f,packet,out)
                assert list(out)==source[i],(initial,disturb,i)
            lib.destroy(f);lib.destroy(s)
    assert hard_errors>0
    # Reject an unused shell but retain differential history for the next frame.
    f=lib.frames(0);out=(C.c_uint8*18)(*([99]*18))
    bad=(C.c_uint8*8)(*([7]*8));assert not lib.v90_qam8_frame(f,bad,out)
    assert list(out)==[99]*18
    labels,_,source=transmit(1,0,3)
    good=(C.c_uint8*8)(*[x for p in labels for x in p])
    assert lib.v90_qam8_frame(f,good,out) and list(out)==source[0]
    invalid=(C.c_uint8*8)(255,0,0,0,0,0,0,0);out=(C.c_uint8*18)(*([99]*18))
    assert not lib.v90_qam8_frame(f,invalid,out) and list(out)==[99]*18
    lib.destroy(f)
    print('PASS: 8-QAM soft pairs, 18-bit frames, all 16 states, noise/corrected decisions, rejection recovery')

# Independently encode one data frame using the final two V0 bits of J=7.
# A shift-register GPA scrambler is deliberately different from the C recurrence.
    register=0;ones=[]
    for i in range(288):
        b=1^((register>>22)&1)
        register=(register<<1)&0x7fffff
        if b:register^=1|(1<<18)
        ones.append(b)
    labels,_,_=transmit(16,0,0,ones,384)
    reference=[point(x) for pair in labels for x in pair]
    lib.b1.restype=C.c_void_p
    lib.b1_label.argtypes=[C.c_void_p,C.c_uint]
    lib.v90_qam8_b1_symbol.argtypes=[C.c_void_p,C.c_double,C.c_double,*([C.POINTER(C.c_double)]*3)]
    s=lib.b1()
    assert [lib.b1_label(s,i) for i in range(128)]==[x for pair in labels for x in pair]
    lib.destroy(s)
    def matches(values):
        s=lib.b1();found=[]
        try:
            for i,v in enumerate(values):
                gain=C.c_double(-99);phase=C.c_double(-99);score=C.c_double(-99)
                if lib.v90_qam8_b1_symbol(s,v.real,v.imag,C.byref(gain),C.byref(phase),C.byref(score)):
                    found.append((i,gain.value,phase.value,score.value))
                else:assert gain.value==phase.value==score.value==-99
        finally:lib.destroy(s)
        return found
    for gain in [.001,1,3000]:
        for phase in [-2.9,-.3,0,1.8]:
            for noisy in [False,True]:
                prefix=[complex(rng.gauss(0,1),rng.gauss(0,1)) for _ in range(211)]
                signal=[gain*(cmath.exp(1j*phase)*v+(complex(rng.gauss(0,.03),rng.gauss(0,.03)) if noisy else 0)) for v in reference]
                found=matches(prefix+signal)
                assert len(found)==1 and found[0][0]==338,found
                _,g,p,c=found[0]
                assert abs(g/gain-1)<.01 and abs(cmath.phase(cmath.exp(1j*(p-phase))))<.01 and c>.999
    assert not matches([complex(rng.gauss(0,1),rng.gauss(0,1)) for _ in range(4000)])
    assert not matches([0j]*300)
    assert not matches(reference[:-1])
    assert not matches(reference[::-1])
    assert not matches([cmath.exp(-1j*(x&3)*cmath.pi/2) for pair in labels for x in pair])
    for bad in [complex(float('nan'),0),complex(0,float('inf')),complex(1e200,0)]:
        found=matches(reference[:100]+[bad]+reference)
        assert len(found)==1 and found[0][0]==228
    print('PASS: 7200 B1 reset/GPA, independent full-frame encoding, gain/phase/boundary acquisition and negative controls')

    callback_type=C.CFUNCTYPE(None,C.c_void_p,C.POINTER(C.c_uint8))
    lib.stream.argtypes=[callback_type];lib.stream.restype=C.c_void_p
    lib.v90_qam8_stream_symbol.argtypes=[C.c_void_p,C.c_double,C.c_double]
    lib.output_symbol.argtypes=lib.rejected.argtypes=[C.c_void_p]
    lib.output_symbol.restype=lib.rejected.restype=C.c_uint64
    source_bits=ones+[rng.randrange(2) for _ in range(18*700)]
    labels,_,source=transmit(716,0,0,source_bits,384)
    clean=[point(x) for pair in labels for x in pair]
    for frequency in [-1.,0.,1.]:
        for gain in [.02,2000]:
            received=[];positions=[]
            def callback(_,bits):
                received.append(list(bits[:18]) if bits else None)
                positions.append(lib.output_symbol(stream))
            cb=callback_type(callback);stream=lib.stream(cb)
            prefix=37
            for i in range(prefix):assert lib.v90_qam8_stream_symbol(stream,0,0)==0
            for i,v in enumerate(clean):
                signal=gain*(1+.08*i/len(clean))*v*cmath.exp(1j*(.43+2*cmath.pi*frequency*i/3200))
                signal+=gain*complex(rng.gauss(0,.015),rng.gauss(0,.015))
                status=lib.v90_qam8_stream_symbol(stream,signal.real,signal.imag)
                assert status==int(i>=127),(i,status)
            assert len(received)==(len(clean)-126)//8
            assert received==source[:len(received)],(frequency,gain,next((i for i,(a,b) in enumerate(zip(received,source)) if a!=b),None))
            assert positions==[prefix+8*(i+1)-1 for i in range(len(received))]
            assert lib.rejected(stream)==0
            # A discontinuity must drop lock, then another B1 starts a fresh epoch.
            assert lib.v90_qam8_stream_symbol(stream,float('nan'),0)==-1
            received.clear();positions.clear()
            for v in clean[:400]:lib.v90_qam8_stream_symbol(stream,v.real,v.imag)
            assert received==source[:len(received)] and len(received)==34
            assert positions[0]==prefix+len(clean)+1+7
            lib.destroy(stream)
    # Next-rate kernel: derive Figure 9 subsets from coordinates, rather than
    # treating a ring index as a Table 13 subset number (ring 2 repeats subset 4).
    lib.v90_trellis_qam12_pair.argtypes=lib.v90_trellis_qam8_pair.argtypes
    def point12(label):return [1+1j,-3+1j,1-3j][label>>2]*(-1j)**(label&3)
    def subset(z):
        x=((int(z.real)+3)//2)&3;y=((int(z.imag)+3)//2)&3
        return ((x^y)&1)|((x&1)<<1)|((((x>>1)^(y>>1)^x^y)&1)<<2)
    for initial in range(16):
        state=initial;labels=[];inversions=[]
        for i in range(1600):
            inv=pattern[(i//32)%14] if i%32==0 else 0
            a=rng.randrange(4);b=(a+2*rng.randrange(2)+((state&1)^inv))%4
            a+=4*rng.randrange(3);b+=4*rng.randrange(3)
            labels.append((a,b));inversions.append(inv)
            v=converter[subset(point12(a))][subset(point12(b))];u=state&1
            state=(state>>1)^(v&1)^(((v>>1)&1)<<1)^((((v>>1)&1)^u)<<2)^(u<<3)
        for noisy in [False,True]:
            state=lib.trellis();decoded=[]
            for (a,b),inv in zip(labels,inversions):
                x,y=point12(a),point12(b)
                if noisy:
                    x+=complex(rng.gauss(0,.08),rng.gauss(0,.08))
                    y+=complex(rng.gauss(0,.08),rng.gauss(0,.08))
                aa=C.c_uint();bb=C.c_uint()
                ready=lib.v90_trellis_qam12_pair(state,x.real,x.imag,y.real,y.imag,inv,C.byref(aa),C.byref(bb))
                if ready:decoded.append((aa.value,bb.value))
            assert decoded[64:]==labels[64:len(decoded)],(initial,noisy)
            lib.destroy(state)
    # Figure 5 quarter points, Figure 9 coordinate subsets and full Table 13
    # independently exercise the five-bit labels needed at 12000/3200.
    def point20(label):return [1+1j,-3+1j,1-3j,-3-3j,1+5j][label>>2]*(-1j)**(label&3)
    lib.v90_trellis_qam20_pair.argtypes=lib.v90_trellis_qam12_pair.argtypes
    seen=set()
    for initial in range(16):
        encoder=initial;encoded=[];inversions=[]
        for i in range(1600):
            inv=pattern[(i//32)%14] if i%32==0 else 0
            a=rng.randrange(4);b=(a+2*rng.randrange(2)+((encoder&1)^inv))%4
            a+=4*rng.randrange(5);b+=4*rng.randrange(5)
            seen.update((a,b));encoded.append((a,b));inversions.append(inv)
            v=converter[subset(point20(a))][subset(point20(b))];u=encoder&1
            encoder=(encoder>>1)^(v&1)^(((v>>1)&1)<<1)^((((v>>1)&1)^u)<<2)^(u<<3)
        for noisy in [False,True]:
            state=lib.trellis();decoded=[]
            for (a,b),inv in zip(encoded,inversions):
                x,y=point20(a),point20(b)
                if noisy:
                    x+=complex(rng.gauss(0,.08),rng.gauss(0,.08))
                    y+=complex(rng.gauss(0,.08),rng.gauss(0,.08))
                aa=C.c_uint(99);bb=C.c_uint(99)
                old=lib.count(state)
                for bad in [float('nan'),float('inf'),1e200]:
                    assert lib.v90_trellis_qam20_pair(state,bad,0,0,0,inv,C.byref(aa),C.byref(bb))==-1
                    assert lib.count(state)==old and aa.value==bb.value==99
                ready=lib.v90_trellis_qam20_pair(state,x.real,x.imag,y.real,y.imag,inv,C.byref(aa),C.byref(bb))
                if ready:decoded.append((aa.value,bb.value))
            assert decoded[64:]==encoded[64:len(decoded)],(initial,noisy)
            lib.destroy(state)
    assert seen==set(range(20))
    print('PASS: twenty-point kernel, full-width history, independent subsets, all states, noise and rejection')
    shells20=sorted(itertools.product(range(5),repeat=8),key=lambda r:
        (sum(r),sum(r[:4]),sum(r[4:6]),r[6],r[4],sum(r[:2]),r[2],r[0]))
    lib.frames20.argtypes=[C.c_uint];lib.frames20.restype=C.c_void_p
    lib.v90_qam20_frame.argtypes=lib.v90_qam12_frame.argtypes=lib.v90_qam8_frame.argtypes
    previous=0;f=lib.frames20(previous)
    for index,shell in enumerate(shells20[:1<<18]):
        expected=[(index>>i)&1 for i in range(18)]+[rng.randrange(2) for _ in range(12)]
        labels=[]
        for p in range(4):
            a=(previous+expected[19+3*p]+2*expected[20+3*p])%4
            b=(a+2*expected[18+3*p]+rng.randrange(2))%4;previous=a
            labels.extend([a+4*shell[2*p],b+4*shell[2*p+1]])
        out=(C.c_uint8*30)()
        assert lib.v90_qam20_frame(f,(C.c_uint8*8)(*labels),out) and list(out)==expected,index
    out=(C.c_uint8*30)(*([99]*30))
    assert not lib.v90_qam20_frame(f,(C.c_uint8*8)(*[4*r+3 for r in shells20[1<<18]]),out)
    assert list(out)==[99]*30
    # An unused shell still advances differential history, unlike invalid labels.
    labels=(C.c_uint8*8)(*([0]*8));out=(C.c_uint8*30)()
    assert lib.v90_qam20_frame(f,labels,out) and list(out)==[0]*19+[1]+[0]*10
    invalid=(C.c_uint8*8)(20,0,0,0,0,0,0,0);out=(C.c_uint8*30)(*([99]*30))
    assert not lib.v90_qam20_frame(f,invalid,out) and list(out)==[99]*30
    small=(C.c_uint8*24)(*([99]*24))
    assert not lib.v90_qam12_frame(f,labels,small) and list(small)==[99]*24
    lib.destroy(f)
    print('PASS: all 262144 twenty-point shell frames, 30 bits, differential history and rejection bounds')

    shells12=sorted(itertools.product(range(3),repeat=8),key=lambda r:
        (sum(r),sum(r[:4]),sum(r[4:6]),r[6],r[4],sum(r[:2]),r[2],r[0]))[:4096]
    lib.frames12.argtypes=[C.c_uint];lib.frames12.restype=C.c_void_p
    lib.v90_qam12_frame.argtypes=lib.v90_qam8_frame.argtypes
    previous=0;f=lib.frames12(previous);state=0
    for index,shell in enumerate(shells12):
        expected=[(index>>i)&1 for i in range(12)]+[rng.randrange(2) for _ in range(12)]
        labels=[]
        for p in range(4):
            i=4*index+p;inv=pattern[(i//32)%14] if i%32==0 else 0
            a=(previous+expected[13+3*p]+2*expected[14+3*p])%4
            b=(a+2*expected[12+3*p]+((state&1)^inv))%4;previous=a
            x=a+4*shell[2*p];y=b+4*shell[2*p+1];labels.extend([x,y])
            v=converter[subset(point12(x))][subset(point12(y))];u=state&1
            state=(state>>1)^(v&1)^(((v>>1)&1)<<1)^((((v>>1)&1)^u)<<2)^(u<<3)
        packet=(C.c_uint8*8)(*labels);out=(C.c_uint8*24)()
        assert lib.v90_qam12_frame(f,packet,out) and list(out)==expected,index
    out=(C.c_uint8*24)(*([99]*24))
    assert not lib.v90_qam12_frame(f,(C.c_uint8*8)(*([11]*8)),out)
    assert list(out)==[99]*24
    # Cross-rate misuse must not overrun the smaller 18-bit destination.
    small=(C.c_uint8*18)(*([99]*18))
    assert not lib.v90_qam8_frame(f,(C.c_uint8*8)(),small) and list(small)==[99]*18
    lib.destroy(f)
    print('PASS: all 4096 twelve-point shell frames, differential bits and rejection bounds')
    print('PASS: twelve-point kernel, coordinate-derived Figure 9 subsets, Table 13, all states and noise')
    lib.stream_rate.argtypes=[C.c_uint,callback_type];lib.stream_rate.restype=C.c_void_p
    lib.b1_rate.argtypes=[C.c_uint];lib.b1_rate.restype=C.c_void_p
    for rate in [0,4800,7201,9599,9601,11999,12001,14400]:assert not lib.b1_rate(rate)
    for rate,k,shells,point_for_rate in [(9600,12,shells12,point12),(12000,18,shells20,point20)]:
        frame_bits=k+12
        register=0;ones12=[]
        for _ in range(16*frame_bits):
            b=1^((register>>22)&1);register=(register<<1)&0x7fffff
            if b:register^=1|(1<<18)
            ones12.append(b)
        data12=ones12+[rng.randrange(2) for _ in range(frame_bits*700)]
        source12=[data12[i:i+frame_bits] for i in range(0,len(data12),frame_bits)]
        previous=state=0;labels12=[]
        for frame,v in enumerate(source12):
            shell=shells[sum(v[i]<<i for i in range(k))]
            for p in range(4):
                i=4*frame+p+384;inv=pattern[(i//32)%14] if i%32==0 else 0
                a=(previous+v[k+1+3*p]+2*v[k+2+3*p])%4
                b=(a+2*v[k+3*p]+((state&1)^inv))%4;previous=a
                x=a+4*shell[2*p];y=b+4*shell[2*p+1];labels12.extend([x,y])
                t=converter[subset(point_for_rate(x))][subset(point_for_rate(y))];u=state&1
                state=(state>>1)^(t&1)^(((t>>1)&1)<<1)^((((t>>1)&1)^u)<<2)^(u<<3)
        b1=lib.b1_rate(rate)
        assert [lib.b1_label(b1,i) for i in range(128)]==labels12[:128]
        lib.destroy(b1)
        for frequency in [-1,0,1]:
            received=[];positions=[]
            def callback12(_,bits):
                received.append(list(bits[:frame_bits]) if bits else None)
                positions.append(lib.output_symbol(stream))
            cb=callback_type(callback12);stream=lib.stream_rate(rate,cb)
            for i,label in enumerate(labels12):
                z=point_for_rate(label)*(1+.06*i/len(labels12))*cmath.exp(1j*(.7+2*cmath.pi*frequency*i/3200))
                z+=complex(rng.gauss(0,.02),rng.gauss(0,.02))
                assert lib.v90_qam8_stream_symbol(stream,z.real,z.imag)==int(i>=127)
            assert received==source12[:len(received)] and len(received)==(len(labels12)-126)//8,(rate,frequency,len(received),next(((i,a,b) for i,(a,b) in enumerate(zip(received,source12)) if a!=b),None))
            assert positions==[8*(i+1)-1 for i in range(len(received))]
            assert lib.rejected(stream)==0
            assert lib.v90_qam8_stream_symbol(stream,float('nan'),0)==-1
            received.clear();positions.clear()
            for label in labels12[:400]:
                z=point_for_rate(label);lib.v90_qam8_stream_symbol(stream,z.real,z.imag)
            assert received==source12[:34]
            assert positions[0]==len(labels12)+8
            lib.destroy(stream)
        print('PASS:',rate,'B1, continuous frames, carrier offset/gain/noise and reacquisition')
print('PASS: continuous B1/data decoding, carrier offset/gain drift/noise, source positions and reacquisition')
