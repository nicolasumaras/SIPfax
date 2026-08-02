import struct,math
raw=open("/tmp/rtp.pcap.up.s16","rb").read();n=len(raw)//2
s=struct.unpack("<%dh"%n,raw);SR=8000
seg=s[int(6.90*SR):int(7.20*SR)]
def demod(fc,baud):
    sp=SR/baud
    # integrate I/Q over each symbol period (matched filter)
    syms=[];i=0.0
    while i+sp<len(seg):
        a=int(i);b=int(i+sp);I=Q=0.0
        for k in range(a,b):
            ph=2*math.pi*fc*k/SR
            I+=seg[k]*math.cos(ph);Q-=seg[k]*math.sin(ph)
        syms.append(complex(I,Q));i+=sp
    # differential phase between consecutive symbols
    diffs=[]
    for j in range(1,len(syms)):
        if abs(syms[j])>1e3 and abs(syms[j-1])>1e3:
            d=math.degrees(math.atan2((syms[j]*syms[j-1].conjugate()).imag,(syms[j]*syms[j-1].conjugate()).real))
            diffs.append(d)
        else:diffs.append(None)
    return syms,diffs
# carrier offset search: which fc gives the cleanest bimodal differential phase (near 0 or 180)
best=None
for off in range(-15,16,3):
    fc=1200+off
    _,diffs=demod(fc,600)
    vals=[d for d in diffs if d is not None]
    if not vals:continue
    # cleanness = fraction within 35deg of {0,180}
    good=sum(1 for d in vals if min(abs(d),abs(abs(d)-180))<35)/len(vals)
    if best is None or good>best[1]:best=(fc,good,diffs)
fc,good,diffs=best
print("best carrier=%dHz clean=%.2f"%(fc,good))
bits="".join("0" if (d is None) else ("1" if min(abs(d),abs(abs(d)-180))<90 and abs(d)<90 else "0") for d in diffs)
print("diff phases:"," ".join("%4.0f"%d if d is not None else "  --" for d in diffs[:40]))
