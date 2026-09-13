#!/usr/bin/env python3
"""Verify live peer-role initialization selects the correct data descrambler.

Generated clean symbols with specification-derived synchronization isolate role
selection from analog acquisition. Both carrier-role directions and legacy
local-role initialization must recover the complete payload exactly.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('binary',type=Path)
a=p.parse_args();binary=a.binary.resolve();checked=0
with tempfile.TemporaryDirectory() as tmp:
    d=Path(tmp);sync=d/'sync';symbols=d/'symbols';pattern='0111011111111010'
    sync.write_text(''.join(f'0 0 0 {int(pattern[(n%480)//30]) if n%30==0 else 0}\n' for n in range(16000)))
    for rate in (12000,16800):
        for shape in (0,1):
            fingerprints={}
            for calling in (0,1):
                for peer in (0,1):
                    env={k:v for k,v in os.environ.items() if not k.startswith('SIPFAX_')}
                    env.update(SIPFAX_DATALOOP='1',SIPFAX_DATA_R=str(rate),
                               SIPFAX_DL_CALLING=str(calling),SIPFAX_DL_PEER_ROLE=str(peer),
                               SIPFAX_DL_SHAPE=str(shape),SIPFAX_HALF='2',
                               SIPFAX_V0ORACLE=str(sync),SIPFAX_V0ALIGN='0',
                               SIPFAX_SYMDUMP=str(symbols))
                    result=subprocess.run([str(binary)],env=env,capture_output=True,check=True,timeout=30)
                    if peer:
                        expected=f"[dataloop] peer RX calling={calling} data_poly={33 if calling else 262145}".encode()
                        assert expected in result.stderr, "peer initializer was not exercised"
                    tx=Path('/tmp/dl_tx.txt').read_bytes();rx=Path('/tmp/dl_rx.txt').read_bytes()
                    n=min(len(tx),len(rx));assert n>100000 and rx[:n]==tx[:n],(rate,shape,calling,peer)
                    checked+=n
                    fingerprint=hashlib.sha256(symbols.read_bytes()).hexdigest()
                    if calling in fingerprints:assert fingerprints[calling]==fingerprint
                    fingerprints[calling]=fingerprint
            assert fingerprints[0]!=fingerprints[1], 'transmitter role option was ignored'
print(f'PASS: both peer roles and legacy initialization; {checked} clean payload bits, zero errors')
