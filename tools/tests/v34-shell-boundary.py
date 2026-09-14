#!/usr/bin/env python3
"""Exhaust the shell domains used by minimum-shaping power-of-two rings.

Compile the production mapper functions directly. Verify range, uniqueness,
inverse mapping, shell ordering, and the independently known endpoint tuples.
This tests shell indexing, not the complete V.34 waveform or interoperability.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'vendor/linmodem/v34.c').read_text(encoding='latin1')
functions = source[source.index('static inline int g2('):source.index('/* parameters for each symbol rate */')]
header = '''#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define M_MAX 18
typedef struct {
 int M;
 int g2_tab[8*(M_MAX-1)+1], g4_tab[8*(M_MAX-1)+1];
 int g8_tab[8*(M_MAX-1)+1], z8_tab[8*(M_MAX-1)+1];
} V34DSPState;
'''
main = '''
int main(void) {
 for (int M=1; M<=4; M*=2) {
  V34DSPState s={0}; s.M=M; build_rings(&s);
  int count=1, previous_shell=-1;
  for (int j=0;j<8;j++) count*=M;
  unsigned char *seen=calloc(count,1); assert(seen);
  for (int index=0;index<count;index++) {
   int rings[4][2], code=0, shell=0;
   index_to_rings(&s,rings,index);
   for (int j=0;j<8;j++) {
    int ring=rings[j/2][j%2]; assert(ring>=0 && ring<M);
    code=code*M+ring; shell+=ring;
    if (!index) assert(ring==0);
    if (index==count-1) assert(ring==M-1);
   }
   assert(!seen[code]);seen[code]=1;
   assert(shell>=previous_shell);previous_shell=shell;
   assert(rings_to_index(&s,rings)==index);
  }
  for(int j=0;j<count;j++) assert(seen[j]);
  free(seen); printf("PASS M=%d: all %d shell indices and endpoint tuples\\n",M,count);
 }
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp)/'shell.c'; binary = Path(tmp)/'shell'
    c.write_text(header+functions+main)
    subprocess.run([os.environ.get('CC','cc'), '-std=c99', '-O1', '-g',
                    '-fsanitize=undefined,bounds', '-fno-sanitize-recover=all',
                    str(c), '-o', str(binary),
                    *shlex.split(os.environ.get('LDFLAGS', ''))], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
