#!/usr/bin/env python3
import sys
D="/root/linmodem/"
def rd(p):
    return open(D+p,encoding="latin-1").read()
def wr(p,s):
    open(D+p,"w",encoding="latin-1").write(s)

m=rd("Makefile")
if "linpipe.o" not in m:
    m=m.replace("serial.o atparser.o","serial.o atparser.o linpipe.o",1)
    wr("Makefile",m); print("Makefile: +linpipe.o")
else: print("Makefile already has linpipe.o")

c=rd("lm.c")
if "MODE_PIPE" not in c:
    c=c.replace("MODE_V90TEST,","MODE_V90TEST,\n    MODE_PIPE,",1)
    c=c.replace('getopt(argc, argv, "hvstrac:d:m:")','getopt(argc, argv, "hvstrac:d:m:P")',1)
    anchor="        case 's':\n            mode = MODE_LINESIM;"
    assert anchor in c, "linesim case anchor not found"
    c=c.replace(anchor,"        case 'P':\n            mode = MODE_PIPE;\n            break;\n"+anchor,1)
    disp="    case MODE_SOUNDCARD:\n        soundcard_modem();\n        break;"
    assert disp in c, "soundcard dispatch not found"
    c=c.replace(disp,disp+"\n    case MODE_PIPE:\n        { extern void pipe_modem(void); pipe_modem(); }\n        break;",1)
    wr("lm.c",c); print("lm.c: +MODE_PIPE +-P")
else: print("lm.c already patched")
