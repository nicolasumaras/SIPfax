import math,struct
SR=8000.0
def crc_lin(bits):
    crc=0xffff
    for b in bits:
        x=(crc&1)^b; crc>>=1
        if x: crc^=(1<<15)|(1<<10)|(1<<3)
    return crc  # on-wire LSB-first matches this raw value
def build_info0a():
    b=[0]*49
    b[0:4]=[1,1,1,1]                       # fill
    b[4:12]=[0,1,1,1,0,0,1,0]              # sync 01110010 (bit4 first)
    # capabilities (answer side): support all symbol rates
    b[12]=1;b[13]=1;b[14]=1               # 2743,2800,3429
    b[15]=1;b[16]=1;b[17]=1;b[18]=1       # 3000lo/hi,3200lo/hi
    b[19]=1                               # 3429 allowed
    b[20]=1                               # can power-reduce
    b[21]=b[22]=b[23]=0                   # sym-rate diff 0
    b[24]=0                               # not CME
    b[25]=1                               # 1664-pt
    b[26]=b[27]=0                         # tx clock internal
    b[28]=0                               # ack
    crc=crc_lin(b[12:29])                 # protected = bits 12:28
    for i in range(16): b[29+i]=(crc>>i)&1   # LSB-first on wire
    b[45:49]=[1,1,1,1]                     # fill
    return b
def dpsk_mod(bits,fc,baud,amp=6000):
    sp=SR/baud; out=[]; phase=0.0; cur=1.0  # start point arbitrary
    # bit1 -> rotate 180, bit0 -> rotate 0
    sym_phase=0.0
    n=0
    for bit in bits:
        if bit==1: sym_phase+=math.pi
        for k in range(int(round((n+1)*sp))-int(round(n*sp))):
            t=(int(round(n*sp))+k)
            out.append(int(amp*math.cos(2*math.pi*fc*t/SR+sym_phase)))
        n+=1
    return out
def dpsk_demod(seg,fc,baud,toff=0):
    sp=SR/baud; syms=[]; i=float(toff)
    while i+sp<len(seg):
        a=int(i);b=int(i+sp);I=Q=0.0
        for k in range(a,b):
            ph=2*math.pi*fc*k/SR; I+=seg[k]*math.cos(ph);Q-=seg[k]*math.sin(ph)
        syms.append(complex(I,Q));i+=sp
    bits=[]
    for j in range(1,len(syms)):
        if abs(syms[j])<100 or abs(syms[j-1])<100: bits.append(None);continue
        d=syms[j]*syms[j-1].conjugate()
        bits.append(1 if abs(math.degrees(math.atan2(d.imag,d.real)))>90 else 0)
    return bits
info=build_info0a()
print("INFO0a tx bits:","".join(map(str,info)))
# answer modem carrier = 2400 Hz
sig=dpsk_mod(info,2400,600)
# prepend a reference symbol-worth so first diff is defined
sig=dpsk_mod([0],2400,600)+sig
for toff in range(0,13):
    d=dpsk_demod(sig,2400,600,toff)
    bs="".join("x" if x is None else str(x) for x in d)
    idx=bs.find("1111"+"01110010")
    if idx>=0:
        rx=d[idx:idx+49]
        if all(x is not None for x in rx):
            ok=crc_lin(rx[12:29])==sum(rx[29+i]<<i for i in range(16))
            print("toff=%d roundtrip sync OK, CRC %s, bits-match=%s"%(toff,"PASS" if ok else "FAIL",rx==info))
            break
