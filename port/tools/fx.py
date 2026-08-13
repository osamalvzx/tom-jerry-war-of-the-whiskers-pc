#!/usr/bin/env python3
import sys,re,os
P=r"D:/Projects/Tom and Jerry in War of the Whiskers (U)"
SRC=os.path.join(P,'re','game_code_decompiled.c')
txt=open(SRC,encoding='utf-8',errors='replace').read()
parts=re.split(r'/\* ===== (\S+) @ (0x[0-9A-Fa-f]+) ===== \*/',txt)
idx={};order=[]
for i in range(1,len(parts),3):
    n=parts[i];a=int(parts[i+1],16);b=parts[i+2]
    idx[n.lower()]=b; idx[a]=b; order.append((a,n))
order.sort()
c=sys.argv[1] if len(sys.argv)>1 else ''
if c=='list':
    for a,n in order: print(f'{a:#08x} {n}')
elif c=='has':
    for q in sys.argv[2:]:
        a=int(q,16); print(f'{a:#x} {"YES" if a in idx else "no"}')
else:
    for q in sys.argv[1:]:
        k=q.lower()
        try: k=int(q,16)
        except ValueError: pass
        print(f'/* ===== {q} ===== */')
        print(idx.get(k, f'// {q} NOT DECOMPILED'))
