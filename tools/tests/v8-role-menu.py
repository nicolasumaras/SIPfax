#!/usr/bin/env python3
"""Verify emitted V.8 octets for digital/analogue V.90 and V.34 menus."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'vendor/linmodem/v8.c').read_text()
a=s.index('static void cm_send(');b=s.index('\n/* selection the modulation',a)
macros='\n'.join(x for x in (root/'vendor/linmodem/v8.h').read_text().splitlines() if x.startswith('#define'))
code='''#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
typedef struct {int calling,tx_fifo;} V8State;
static unsigned char menu[32]; static int size;
static void sm_put_bits(int *f,int v,int n) {(void)f;(void)v;(void)n;}
static void v8_put_byte(V8State *s,int byte) {(void)s;menu[size++]=byte;}
'''+macros+'\n'+s[a:b]+'''
int main(void) {
 V8State s={0};
 cm_send(&s,V8_MOD_V90|V8_MOD_V34); assert(menu[size-2]==0xb1 && menu[size-1]==0xe2);
 size=0;s.calling=1;
 cm_send(&s,V8_MOD_V90|V8_MOD_V34); assert(menu[size-2]==0xb0 && menu[size-1]==0xe4);
 size=0;s.calling=0;
 cm_send(&s,V8_MOD_V34); assert(size==6 && menu[size-1]==0xb0);
 puts("PASS: V.90 digital/analogue roles and unchanged V.34 access menu");
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t)/'test.c';p.write_text(code);exe=Path(t)/'test'
 subprocess.run(['gcc','-Wall','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
