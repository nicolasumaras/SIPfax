#!/usr/bin/env python3
"""Check MP numeric fields with independent LSB-first frames (V.34 Tables20/21).

Checks emitted bits separately from the native decoder. Independent CRC and
GPC/QPSK synthesis exercise the actual block decoder in both frame formats.
This is generated-symbol validation, not hardware startup qualification.
"""
import argparse,json,os,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('binary',type=Path);p.add_argument('--receive-only',action='store_true');a=p.parse_args();binary=a.binary.resolve()
base={k:v for k,v in os.environ.items() if not k.startswith('SIPFAX_')}
e=dict(base,SIPFAX_MPTEST_RUN='1',SIPFAX_MPTEST_RX_TRELLIS='1')
r=subprocess.run([str(binary)],env=e,capture_output=True,text=True,check=True,timeout=10)
selections=[tuple(map(int,line.split())) for line in r.stdout.splitlines()]
assert len(selections)==12, 'missing receiver trellis selection cases'
for advertised,peer,states in selections:
 assert states==(16,32,64)[peer if advertised<0 else advertised], (advertised,peer,states)
powers={}
for global_shape in (0,1):
 e=dict(base,SIPFAX_MPTEST_RUN='1',SIPFAX_MPTEST_SHAPING='1',SIPFAX_SHAPE=str(global_shape))
 r=subprocess.run([str(binary)],env=e,capture_output=True,text=True,check=True,timeout=30)
 rows=[line.split() for line in r.stdout.splitlines()]
 assert len(rows)==8, 'missing directional shaping cases'
 for row in rows:
  rate,peer,own,tx,rx,txok,rxok=map(int,row[:7]);power=float(row[7])
  assert (tx,rx,txok,rxok)==(peer,own,1,1),row
  assert power>0,row
  key=(rate,peer)
  if key in powers:assert power==powers[key], 'power depends on opposite direction/global shape'
  powers[key]=power
for rate in (12000,24000):
 assert powers[rate,0]!=powers[rate,1], 'power estimator ignored shaping'
with tempfile.TemporaryDirectory() as tmp:
 d=Path(tmp);frames=d/'frames';symbols=d/'symbols'
 for kind in (0,1):
  stable=d/f'stable-{kind}'
  e=dict(base,SIPFAX_MPTEST_RUN='1',SIPFAX_MPTEST=str(stable),SIPFAX_MPTEST_STABLE=str(kind),SIPFAX_MP_CA='5',SIPFAX_MP_AC='5')
  subprocess.run([str(binary)],env=e,capture_output=True,check=True,timeout=10)
  rows=[[int(b) for b in line] for line in stable.read_text().splitlines()]
  assert len(rows)==4
  co=171 if kind else 69
  for row in rows:
   assert len(row)==(188 if kind else 88)
   crc=0xffff
   for k in range(17,co):
    start=k in (17,34) or (k>=51 and (k-51)%17==0 if kind else k in (51,68))
    if not start:crc=(crc>>1)^(0x8408 if (crc^row[k])&1 else 0)
   assert sum(row[co+j]<<j for j in range(16))==crc, 'invalid frozen-frame CRC'
  assert rows[0]==rows[1], 'MP changed after later peer parameters/channel estimate'
  assert [k for k in range(co) if rows[0][k]!=rows[2][k]]==[33], 'MP-prime changed information beyond ACK'
  read=lambda row,start,n:sum(row[start+j]<<j for j in range(n))
  assert (read(rows[0],20,4),read(rows[0],24,4),read(rows[0],29,2))==(5,5,0)
  assert (read(rows[3],20,4),read(rows[3],24,4),read(rows[3],29,2))==(3,4,2), 'new negotiation retained stale advertisement'
  if kind:
   assert read(rows[0],52,16)==1234 and read(rows[3],52,16)==5678, 'coefficient snapshot/reset failed'

 if not a.receive_only:
  e=dict(base,SIPFAX_MPTEST_RUN='1',SIPFAX_MPTEST=str(frames),SIPFAX_MP_CA='5',SIPFAX_MP_AC='5')
  subprocess.run([str(binary)],env=e,capture_output=True,check=True,timeout=10)
  rows=[[int(b) for b in line] for line in frames.read_text().splitlines()]
  for row,(ca,ac,trel) in zip(rows,((5,4,2),(5,4,2),(5,5,0)),strict=True):
   read=lambda start,count:sum(row[start+j]<<j for j in range(count))
   assert (read(20,4),read(24,4),read(29,2))==(ca,ac,trel),'emitted MP fields are not LSB-first'
 def frame(ca,ac,trel,kind,corrupt=False):
  co=171 if kind else 69;bits=[0]*(188 if kind else 88);bits[:17]=[1]*17;bits[18]=kind
  def put(at,n,v):bits[at:at+n]=[(v>>j)&1 for j in range(n)]
  put(20,4,ca);put(24,4,ac);put(29,2,trel);put(35,15,0x3fff);bits[33]=bits[50]=1
  crc=0xffff
  for k in range(17,co):
   start=k in (17,34) or (k>=51 and (k-51)%17==0 if kind else k in (51,68))
   if not start:crc=(crc>>1)^(0x8408 if (crc^bits[k])&1 else 0)
  put(co,16,crc)
  if corrupt:bits[40]^=1
  return bits
 def decode(bits):
  # Scramble the complete repeated sequence using GPC, then differential CW QPSK.
  scrambled=[]
  for k,b in enumerate(bits*6):scrambled.append(b^(scrambled[k-18] if k>=18 else 0)^(scrambled[k-23] if k>=23 else 0))
  z=0;pairs=[(1,1)];rotations=((1,1),(1,-1),(-1,-1),(-1,1))
  for k in range(0,len(scrambled),2):
   z=(z+scrambled[k]+2*scrambled[k+1])%4;pairs.append(rotations[z])
  symbols.write_text(''.join(f'{x} {y}\n' for x,y in pairs))
  e=dict(base,SIPFAX_MPTEST_RUN='1',SIPFAX_MPTEST_SYMBOLS=str(symbols))
  r=subprocess.run([str(binary)],env=e,capture_output=True,text=True,check=True,timeout=10)
  return json.loads(r.stdout)
 count=0
 for kind in (0,1):
  for ca in range(1,15):
   ac=15-ca
   for trel in (0,1,2):
    got=decode(frame(ca,ac,trel,kind))
    assert got['frames']>=2 and (got['ca'],got['ac'],got['trellis'],got['ack'])==(ca,ac,trel,1),(kind,ca,ac,trel,got)
    count+=1
  assert decode(frame(5,13,2,kind,True))['frames']==0,'corrupt frame accepted'
 print(f'PASS: {count} independent MP cases and corrupt-frame controls; 12 trellis selections; 16 directional shaping/power cases; Type0/1 snapshot, ACK CRC and reset cases; emitted fields checked={not a.receive_only}')
