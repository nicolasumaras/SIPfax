import struct,math,glob
# decode upstream (modem->VM) from the working slmodem call
data=open("/tmp/rtp.pcap","rb").read(); off=24; ups=[]
def u2l(u):
    u=~u&0xff;t=((u&0x0f)<<3)+0x84;t<<=(u&0x70)>>4
    return (0x84-t) if(u&0x80)else(t-0x84)
while off+16<=len(data):
    _,_,cl,_=struct.unpack("<IIII",data[off:off+16]);off+=16
    pkt=data[off:off+cl];off+=cl
    if len(pkt)<54 or pkt[12:14]!=b"\x08\x00" or pkt[23]!=17:continue
    ihl=(pkt[14]&0x0f)*4;dst=".".join(str(b) for b in pkt[30:34]);rtp=pkt[14+ihl+8:]
    if len(rtp)<12 or (rtp[0]&0xc0)!=0x80:continue
    if dst=="192.168.1.31":ups.append((struct.unpack(">H",rtp[2:4])[0],rtp[12:]))
ups.sort();pcm=[]
for _,p in ups:
    for b in p:pcm.append(u2l(b))
SR=8000
def g(x,f):
    w=2*math.pi*f/SR;c=2*math.cos(w);s1=s2=0.0
    for v in x:s0=v+c*s1-s2;s2=s1;s1=s0
    return math.sqrt(max(s1*s1+s2*s2-c*s1*s2,0))/len(x)
print("WORKING slmodem call - modem upstream, V.34 startup map (200ms windows, t=3..22s):")
W=1600
for k in range(3*SR,min(22*SR,len(pcm)-W),W):
    x=pcm[k:k+W];rms=math.sqrt(sum(v*v for v in x)/len(x))
    if rms<80:print("t=%4.1fs rms=%5.0f silence"%(k/SR,rms));continue
    # peak freq + bandwidth indicator (is it a tone or wideband modulated signal?)
    pk=max(((f,g(x,f)) for f in range(300,3401,25)),key=lambda kv:kv[1])
    # energy spread: ratio of peak to total
    tot=sum(g(x,f) for f in range(300,3401,200))
    ratio=pk[1]/(tot+1e-9)
    kind="TONE" if ratio>1.5 else "modulated/wideband"
    print("t=%4.1fs rms=%5.0f peak=%4dHz %s"%(k/SR,rms,pk[0],kind))
