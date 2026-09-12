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
rows = [tuple(map(int, m)) for m in re.findall(r'\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}', source)]
blocks = gen.build('x0')
assert len(rows) == len(set(rows)) == 256
assert rows == [row for block in range(32) for row in blocks[block]]
header = (root/'vendor/linmodem/v34priv.h').read_bytes().decode('latin1')
assert 'extern u8 trellis_trans_16_fig9[256][4];' in header
print('PASS: all 16 Figure 9 labels; 256 unique generated branch tuples; external table declaration')
