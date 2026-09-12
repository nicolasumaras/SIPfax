#!/usr/bin/env python3
"""Check actual 4800-bit/s B1 rotations against V34 clause9.3.2 grouping."""
import argparse,os,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('binary',type=Path);a=p.parse_args()
with tempfile.TemporaryDirectory() as tmp:
    root=Path(tmp)
    env={k:v for k,v in os.environ.items() if not k.startswith('SIPFAX_')}
    env.update(SIPFAX_B1_REFERENCE=str(root/'symbols'),SIPFAX_B1_RATE='4800',SIPFAX_B1_TRELLIS='64',SIPFAX_B1_H='0,0,0,0,0,0',SIPFAX_ENCDUMP=str(root/'encoder'))
    subprocess.run([str(a.binary.resolve())],env=env,capture_output=True,check=True)
    rows=[list(map(int,line.split())) for line in (root/'encoder').read_text().splitlines()]
    assert len(rows)==60
    # 168 bits/frame at 3429 baud. GPC scrambled all-ones input from zero state.
    bits=[]
    for n in range(168):bits.append(1^(bits[n-18] if n>=18 else 0)^(bits[n-23] if n>=23 else 0))
    at=0;previous_z=0
    for frame in range(15):
        size=12 if frame%5==4 else 11
        for group in range(4):
            z0,z1,u0,*_=rows[frame*4+group]
            delta=(z0-previous_z)%4;previous_z=z0
            i0=((z1-z0-u0)%4)//2;i1=delta&1;i2=delta>>1
            expected=[bits[at],bits[at+1]];at+=2
            if group<size-8:expected.append(bits[at]);at+=1
            else:expected.append(0)
            assert [i0,i1,i2]==expected,(frame,group,[i0,i1,i2],expected)
    assert at==168
    pattern='0111011111111010'
    sync=root/'sync'
    sync.write_text(''.join(f'0 0 0 {int(pattern[(n%480)//30]) if n%30==0 else 0}\n' for n in range(16000)))
    for shape in ('0','1'):
        env={k:v for k,v in os.environ.items() if not k.startswith('SIPFAX_')}
        env.update(SIPFAX_DATALOOP='1',SIPFAX_DATA_R='4800',SIPFAX_DL_SHAPE=shape,
                   SIPFAX_HALF='2',SIPFAX_V0ORACLE=str(sync))
        subprocess.run([str(a.binary.resolve())],env=env,capture_output=True,check=True)
        tx=Path('/tmp/dl_tx.txt').read_bytes();rx=Path('/tmp/dl_rx.txt').read_bytes()
        n=min(len(tx),len(rx));assert n>40000 and tx[:n]==rx[:n],(shape,n)
print('PASS: actual low-rate B1 rotations carry all168 bits in clause9.3.2 order')
