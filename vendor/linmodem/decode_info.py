import struct,math
raw=open("/tmp/rtp.pcap.up.s16","rb").read();n=len(raw)//2
s=struct.unpack("<%dh"%n,raw);SR=8000.0
def calc_crc(bits):
    # bits: list of info bits (LSB first per spec, bit0 first in time), poly x16+x12+x5+1
    crc=0xffff
    for b in bits:
        x=(crc&1)^b
        crc=(crc>>1)
        if x: crc^=(1<<15)|(1<<10)|(1<<3)
    inv=0
    for i in range(16): inv|=((crc>>i)&1)<<(15-i)
    return inv
SYNC="01110010"  # bits 4:11, leftmost first in time
def demod(seg,fc,baud,toff):
    sp=SR/baud; syms=[]; i=float(toff)
    while i+sp<len(seg):
        a=int(i);b=int(i+sp);I=Q=0.0
        for k in range(a,b):
            ph=2*math.pi*fc*k/SR
            I+=seg[k]*math.cos(ph);Q-=seg[k]*math.sin(ph)
        syms.append(complex(I,Q));i+=sp
    bits=[]
    for j in range(1,len(syms)):
        if abs(syms[j])<500 or abs(syms[j-1])<500: bits.append(None); continue
        d=(syms[j]*syms[j-1].conjugate())
        ang=abs(math.degrees(math.atan2(d.imag,d.real)))
        bits.append(1 if ang>90 else 0)  # bit1=180deg rotation
    return bits
# scan whole upstream for an INFO0c (1200Hz, look for fill 1111 + sync 01110010)
found=False
for start in range(int(3.0*SR),int(8.0*SR),200):
    if found:break
    seg=s[start:start+int(0.20*SR)]
    for fc in (1191,1200,1185,1196):
        for toff in range(0,13,2):
            bits=demod(seg,fc,600,toff)
            bs="".join("x" if b is None else str(b) for b in bits)
            idx=bs.find("1111"+SYNC)
            if idx>=0 and idx+49<=len(bits) and all(b is not None for b in bits[idx:idx+49]):
                info=bits[idx:idx+49]
                body=info[4:29]  # bits 4:28 inclusive are CRC-protected? spec: exclude sync,start,fill
                # CRC covers info bits except frame sync(4:11), start bits, fill(0:3,45:48). Protected=bits12:28
                prot=info[12:29]
                crc_rx=0
                for i,b in enumerate(info[29:45]): crc_rx|=b<<i
                print("t=%.2fs fc=%d toff=%d INFO0 candidate"%(start/SR,fc,toff))
                print("  bits:",("".join(map(str,info))))
                print("  sym2743=%d 2800=%d 3429=%d 3000lo=%d 3000hi=%d 3200lo=%d 3200hi=%d"%(info[12],info[13],info[14],info[15],info[16],info[17],info[18]))
                print("  1664pt=%d ack=%d"%(info[25],info[28]))
                print("  CRC field=0x%04x  calc(12:28)=0x%04x"%(crc_rx,calc_crc(prot)))
                found=True;break
        if found:break
if not found:print("no INFO0c sync found in t=3-8s")

# --- CRC brute force on the decoded INFO0 ---
info=[int(c) for c in "1111011100101111111110000100010001111110110010000"]
prot=info[12:29]
def crc_lin(bits):
    crc=0xffff
    for b in bits:
        x=(crc&1)^b; crc>>=1
        if x: crc^=(1<<15)|(1<<10)|(1<<3)
    return crc
raw_crc=crc_lin(prot)
inv=0
for i in range(16): inv|=((raw_crc>>i)&1)<<(15-i)
rx_lsb=sum(info[29+i]<<i for i in range(16))
rx_msb=sum(info[29+i]<<(15-i) for i in range(16))
print("raw=0x%04x inv=0x%04x | rx_lsb=0x%04x rx_msb=0x%04x"%(raw_crc,inv,rx_lsb,rx_msb))
# also try prot reversed
pr=prot[::-1]; raw2=crc_lin(pr); inv2=0
for i in range(16): inv2|=((raw2>>i)&1)<<(15-i)
print("rev-input raw=0x%04x inv=0x%04x"%(raw2,inv2))
for name,v in [("raw",raw_crc),("inv",inv),("raw2",raw2),("inv2",inv2)]:
    for rn,rv in [("lsb",rx_lsb),("msb",rx_msb)]:
        if v==rv: print("MATCH:",name,"==",rn)
