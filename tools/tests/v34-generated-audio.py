#!/usr/bin/env python3
"""Check settled receive bits through the native audio modulator and CMA path.

A generated 12-kbit/s caller sends a Phase4 prefix and all-ones data. The test
supplies data-entry time; it does not qualify automatic E recognition, hardware
startup, precoding, nonlinear channels, or PPP.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('binary',type=Path)
a=p.parse_args();binary=a.binary.resolve()
with tempfile.TemporaryDirectory() as tmp:
    d=Path(tmp);audio=d/'audio.s16';bits=d/'bits'
    env={k:v for k,v in os.environ.items() if not k.startswith('SIPFAX_')}
    common=dict(SIPFAX_MP_CA='5',SIPFAX_MP_AC='5',SIPFAX_SHAPE='0',SIPFAX_MP_SHAPE='0')
    gen=dict(env,**common,SIPFAX_GEN_DATA=str(audio),SIPFAX_GEN_R='12000',
             SIPFAX_GEN_TREL='64',SIPFAX_GEN_CALLING='1',SIPFAX_GEN_SEAM='1',
             SIPFAX_GEN_P4S='4',SIPFAX_GEN_SEC='16')
    subprocess.run([str(binary)],env=gen,capture_output=True,check=True,timeout=45)
    assert audio.stat().st_size==256000
    rx=dict(env,**common,SIPFAX_STREAM_FILE=str(audio),SIPFAX_STREAM_LIVE_INIT='1',
            SIPFAX_DATABITS=str(bits),SIPFAX_FORCE_DATA='12000',SIPFAX_FORCE_DATA_AT='4.2')
    r=subprocess.run([str(binary)],env=rx,capture_output=True,check=True,timeout=60)
    data=bits.read_bytes();assert len(data)>100000 and set(data)<={48,49}
    # Fixed settling boundary, chosen before the fix; errors are retained above it.
    settled=data[40000:]
    assert len(settled)>80000 and settled.count(b'0')==0, (len(data),settled.count(b'0'))
    assert b'RX params: R=12000' in r.stderr
    bridged=dict(rx,SIPFAX_STREAM_MP='12000,2,0',SIPFAX_STREAM_TREL='0')
    r2=subprocess.run([str(binary)],env=bridged,capture_output=True,check=True,timeout=60)
    assert bits.read_bytes()==data, 'recorded MP replay differs from equivalent receive parameters'
    assert b'recorded MP: ca=12000 trellis=2 shape=0' in r2.stderr
    for invalid in ('12000,3,0','12001,0,0','12000,0,2','12000,0,0junk'):
        rejected=subprocess.run([str(binary)],env=dict(rx,SIPFAX_STREAM_MP=invalid),capture_output=True,timeout=10)
        assert rejected.returncode==2, invalid
    print(f'PASS: native generated audio, {len(settled)} settled bits, zero errors; startup {data[:40000].count(b"0")} errors retained')
