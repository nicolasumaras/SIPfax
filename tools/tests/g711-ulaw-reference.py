#!/usr/bin/env python3
"""Compare both bridge encoders exhaustively against installed SpanDSP.
Requires gcc and libspandsp-dev. Run from the repository root.
"""
from pathlib import Path
import subprocess
import tempfile

sources = [('vendor/linmodem/linpipe.c', 'lin2ulaw'),
           ('vendor/slmodem-bridge/bridge.c', 'linear2ulaw')]
with tempfile.TemporaryDirectory() as directory:
    for path, name in sources:
        source = Path(path).read_text()
        import re
        match = re.search(r'static (?:u8|uint8_t) '+name+r'\([^)]*\)\s*\{', source)
        start = match.start()
        pos = match.end()
        depth = 1
        while depth:
            depth += (source[pos] == '{') - (source[pos] == '}')
            pos += 1
        function = source[start:pos]
        code = '''#include <stdint.h>
#include <stdio.h>
#include <spandsp.h>
typedef uint8_t u8;
typedef int16_t s16;
#define G711_BIAS 0x84
''' + function + '''
int main(void) {
    for (int pcm = -32768; pcm <= 32767; pcm++) {
        if (ENCODER(pcm) != linear_to_ulaw(pcm)) {
            fprintf(stderr, "mismatch at %d\\n", pcm); return 1;
        }
    }
    return 0;
}
'''.replace('ENCODER', name)
        c = Path(directory) / 'test.c'
        exe = Path(directory) / 'test'
        c.write_text(code)
        subprocess.run(['gcc', '-Wall', '-Werror', str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        print(f'{path}: all 65536 PCM inputs match SpanDSP')
