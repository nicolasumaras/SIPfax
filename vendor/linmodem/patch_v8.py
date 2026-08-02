#!/usr/bin/env python3
"""Widen linmodem V.8 ANSWER-side timeouts so it doesn't hang up while a real
modem is still negotiating. Whitespace-robust: edits the line after each marker."""
D="/root/linmodem/"
p=D+"v8.c"
lines=open(p,encoding="latin-1").read().split("\n")
changes=0
for i,l in enumerate(lines):
    if "wait at most 5 seconds" in l:
        lines[i]=l.replace("wait at most 5 seconds","wait at most 30 seconds for the calling modem's CM")
    if "/* timeout for JM */" in l and i+1 < len(lines) and "5000" in lines[i+1]:
        lines[i+1]=lines[i+1].replace("5000","15000"); changes+=1
    if "wait at most 30 seconds" in lines[i] and i+1 < len(lines) and "5000" in lines[i+1]:
        lines[i+1]=lines[i+1].replace("5000","30000"); changes+=1
assert changes==2, f"expected 2 timeout edits, made {changes}"
open(p,"w",encoding="latin-1").write("\n".join(lines))
print(f"v8.c answer-side timeouts widened ({changes} edits: CM_WAIT->30s, JM->15s)")
