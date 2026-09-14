#!/usr/bin/env python3
"""Check Figure 9 labels and the checked-in experimental branch table.

This does not establish decoder interoperability or a clean bit round trip.
"""
import importlib.util
from pathlib import Path
import re
import sys
sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('branch_generator', root/'tools/gen_trellis_trans_16.py')
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)
# Figure 9, rows Im=-3,-1,+1,+3; columns Re=-3,-1,+1,+3.
labels = ((0b000,0b111,0b100,0b011), (0b101,0b010,0b001,0b110),
          (0b100,0b011,0b000,0b111), (0b001,0b110,0b101,0b010))
for v in range(4):
    for u in range(4):
        bits = gen.label(u, v, 'x0')
        assert (bits[0]<<2 | bits[1]<<1 | bits[2]) == labels[v][u]
source = (root/'vendor/linmodem/v34fig9.c').read_text()
legacy = (root/'vendor/linmodem/v34table.c').read_text()
header = (root/'vendor/linmodem/v34priv.h').read_bytes().decode('latin1')
for transitions in (4,8,16):
    def rows_in(text,name):
        body=re.search(r'\b'+name+r'\[256\]\[4\]\s*=\s*\{(.*?)\n\};',text,re.S).group(1)
        return [tuple(map(int,m)) for m in re.findall(r'\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}',body)]
    rows=rows_in(source,f'trellis_trans_{transitions}_fig9')
    blocks=gen.build('x0',transitions);size=128//transitions
    assert len(rows)==len(set(rows))==256
    assert rows==[r for b in range(2*transitions) for r in blocks[b]]
    # Independently classify every tuple using the fixed Figure9 grid above.
    for block in range(2*transitions):
        for a,b,c,d in rows[block*size:(block+1)*size]:
            u,v=labels[b][a],labels[d][c]
            y1=((u&1)&(~v&1))^((u>>1)&1)^((v>>1)&1)
            transition=y1|((u&1)<<1)|((((u^v)>>2)&1)<<2)|(((u>>1)&1)<<3)
            half=(a^b^c^d)&1
            assert block==(transition&(transitions-1))+half*transitions
    old=rows_in(legacy,f'trellis_trans_{transitions}')
    oldblocks=gen.build('y0',transitions)
    assert all(set(old[b*size:(b+1)*size])==set(oldblocks[b]) for b in oldblocks)
    assert f'extern u8 trellis_trans_{transitions}_fig9[256][4];' in header
print('PASS: fixed Figure9 labels, 768 independently classified tuples, all legacy tables reproduced')
