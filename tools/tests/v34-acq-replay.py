#!/usr/bin/env python3
"""Check accepted acquisition sample order and settled generated-audio decoding.

This does not qualify automatic E detection or hardware B1/PPP. The data entry
is explicitly supplied, as in v34-generated-audio.py.
"""
import argparse,os,re,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('binary',type=Path);binary=p.parse_args().binary.resolve()
base={k:v for k,v in os.environ.items() if not k.startswith('SIPFAX_')}
with tempfile.TemporaryDirectory() as tmp:
 d=Path(tmp);audio=d/'audio';bits=d/'bits';seq=d/'sequence'
 common=dict(SIPFAX_MP_CA='5',SIPFAX_MP_AC='5',SIPFAX_SHAPE='0',SIPFAX_MP_SHAPE='0')
 gen=dict(base,**common,SIPFAX_GEN_DATA=str(audio),SIPFAX_GEN_R='12000',SIPFAX_GEN_TREL='64',SIPFAX_GEN_CALLING='1',SIPFAX_GEN_SEAM='1',SIPFAX_GEN_P4S='4',SIPFAX_GEN_SEC='16')
 subprocess.run([str(binary)],env=gen,capture_output=True,check=True,timeout=45)
 for count in (64,2000):
  first=[]
  for replay in (0,1):
   env=dict(base,**common,SIPFAX_STREAM_FILE=str(audio),SIPFAX_STREAM_LIVE_INIT='1',SIPFAX_FORCE_DATA='12000',SIPFAX_FORCE_DATA_AT='4.2',SIPFAX_DATABITS=str(bits),SIPFAX_DATA_FEED_SEQUENCE=str(seq),SIPFAX_ACQ_N=str(count),SIPFAX_ACQ_REPLAY=str(replay),SIPFAX_STREAM_TIMING='1')
   r=subprocess.run([str(binary)],env=env,capture_output=True,text=True,check=True,timeout=60)
   timing=re.search(r'\[stream-timing\] calls=(\d+) mean_ms=([\d.]+) worst_ms=([\d.]+) worst_sample=(\d+) deadline_overruns=(\d+) block_samples=(\d+)',r.stderr)
   assert timing and int(timing[1])==800 and int(timing[6])==160
   assert 0<=float(timing[2])<=float(timing[3]) and 0<=int(timing[5])<=800
   indices=[int(x) for x in seq.read_text().splitlines()];assert indices
   assert all(b>a for a,b in zip(indices,indices[1:])), 'duplicate/out-of-order sample'
   first.append(indices[0])
   original_bits=bits.read_bytes();original_seq=seq.read_bytes()
   assert '2-D (gain,phase) sweep' not in r.stderr
   audit=subprocess.run([str(binary)],env=dict(env,SIPFAX_ACQ_AUDIT='1'),capture_output=True,text=True,check=True,timeout=60)
   assert '2-D (gain,phase) sweep' in audit.stderr
   assert bits.read_bytes()==original_bits and seq.read_bytes()==original_seq, 'diagnostic scan changed decoder output'

   if replay:
    assert indices[:count+1]==list(range(indices[0],indices[0]+count+1)), 'accepted acquisition samples lost'
    assert f'replaying {count} accepted acquisition symbols' in r.stderr
   data=bits.read_bytes();assert len(data)>100000
   assert data[40000:].count(b'0')==0, (count,replay,len(data),data[40000:].count(b'0'))
  assert first[0]-first[1]==count, (count,first)
  print(f'PASS window={count}: accepted prefix retained once, monotonic sample order, zero settled generated-audio errors')
