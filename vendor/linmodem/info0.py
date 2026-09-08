import struct,math,cmath
raw=open("/tmp/rtp.pcap.up.s16","rb").read();n=len(raw)//2
s=struct.unpack("<%dh"%n,raw);SR=8000
def g(x,f):
    w=2*math.pi*f/SR;c=2*math.cos(w);s1=s2=0.0
    for v in x:s0=v+c*s1-s2;s2=s1;s1=s0
    return math.sqrt(max(s1*s1+s2*s2-c*s1*s2,0))/len(x)
# Map t=4.8..8.0s at 40ms to separate V.21 FSK (980&1180 both) from DPSK (single carrier)
print("fine map t=4.8-8.0s (FSK=980&1180 both present; DPSK INFO=single ~1200 carrier):")
W=320
for k in range(int(4.8*SR),int(8.0*SR),W):
    x=s[k:k+W];rms=math.sqrt(sum(v*v for v in x)/len(x))
    if rms<80:print("t=%.2fs rms=%4.0f silence"%(k/SR,rms));continue
    f980=g(x,980);f1180=g(x,1180);f1200=g(x,1200);f1650=g(x,1650);f1850=g(x,1850)
    tag="FSK?" if (f980>200 and f1180>200) else ("DPSK~1200?" if f1200>300 else "")
    print("t=%.2fs rms=%4.0f 980=%4.0f 1180=%4.0f 1200=%4.0f 1650=%4.0f 1850=%4.0f %s"%(k/SR,rms,f980,f1180,f1200,f1650,f1850,tag))
