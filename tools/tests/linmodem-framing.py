#!/usr/bin/env python3
"""Exercise framed audio while legacy printf diagnostics are enabled."""
import struct
import subprocess
import sys

binary = sys.argv[1] if len(sys.argv) > 1 else 'vendor/linmodem/lm'
frame = struct.pack('!H', 160) + b'\xff' * 160
result = subprocess.run([binary, '-P', '-v'], input=frame * 100,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        timeout=15, check=True)
data = result.stdout
for index in range(100):
    assert len(data) >= 2, f'missing frame {index}'
    size = struct.unpack('!H', data[:2])[0]
    assert size == 160, f'corrupted frame {index}: length {size}'
    assert len(data) >= 2 + size
    data = data[2 + size:]
assert not data, 'unexpected bytes after audio'
assert b'state:' in result.stderr, 'legacy printf diagnostics were not exercised'
print('100 complete audio frames; legacy diagnostics stayed on stderr')
