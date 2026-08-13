#!/usr/bin/env python3
import sys,re,os
P=r"D:/Projects/Tom and Jerry in War of the Whiskers (U)"
txt=open(os.path.join(P,'re','game_code_decompiled.c'),encoding='utf-8',errors='replace').read()
parts=re.split(r'/\* ===== (\S+) @ (0x[0-9A-Fa-f]+) ===== \*/',txt)
pat=sys.argv[1]
rx=re.compile(pat)
for i in range(1,len(parts),3):
    n=parts[i];a=parts[i+1];b=parts[i+2]
    c=len(rx.findall(b))
    if c: print(f'{a} {n} x{c}')
