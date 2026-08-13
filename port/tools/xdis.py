#!/usr/bin/env python3
# Section-aware XBE disassembler + symbol lookup. Usage:
#   python dis.py <va> [size]         disassemble
#   python dis.py sym <va>            name a VA
#   python dis.py callers <va>        find callers of a function
#   python dis.py xdk <substr>        grep xdk_symbols
import sys, struct, csv
import capstone
PROJ="D:/Projects/Tom and Jerry in War of the Whiskers (U)"
data=open(f"{PROJ}/extracted/default.xbe","rb").read()
base=0x10000
# section table
nsec=struct.unpack_from('<I',data,0x11c)[0]; secoff=struct.unpack_from('<I',data,0x120)[0]-base
SECS=[]
for i in range(nsec):
    o=secoff+i*0x38
    flags,va,vsz,ro,rsz,na=struct.unpack_from('<IIIIII',data,o)
    nm=data[na-base:data.index(b'\0',na-base)].decode()
    SECS.append((nm,va,vsz,ro,rsz))
def raw_of(va):
    for nm,v,vsz,ro,rsz in SECS:
        if v<=va<v+vsz:
            off=va-v
            if off<rsz: return ro+off
    return None
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
funcs=[]
for r in csv.reader(open(f"{PROJ}/re/functions.csv")):
    if r[0]=='address':continue
    funcs.append((int(r[0],16),int(r[1]),r[3]))
funcs.sort()
byaddr={a:(s,n) for a,s,n in funcs}
xdk={}
for ln in open(f"{PROJ}/re/xdk_symbols.txt",encoding='utf-8-sig'):
    if '=' in ln:
        k,v=ln.split('='); xdk[int(v.strip(),16)]=k.strip()
def nm(va):
    if va in xdk: return xdk[va]
    best='?'
    for a,s,n in funcs:
        if a<=va<a+s: return n if va==a else f'{n}+{va-a:#x}'
        if a<=va: best=n if va-a<0x800 else best
    return best
def dis(va,size):
    raw=raw_of(va)
    if raw is None: print(f'  {va:#x}: (uninit/bss, no raw bytes)'); return
    for i in md.disasm(data[raw:raw+size], va):
        t=''
        if i.mnemonic in('call','jmp') and i.op_str.startswith('0x'):
            tgt=int(i.op_str,16); t=' ; '+nm(tgt)
        elif 'fs:' in i.op_str: t='   <-- FS'
        print(f'  {i.address:#08x}: {i.mnemonic} {i.op_str}{t}')
cmd=sys.argv[1]
if cmd=='sym': print(nm(int(sys.argv[2],16)))
elif cmd=='xdk':
    s=sys.argv[2].lower()
    for va,k in sorted(xdk.items(),key=lambda x:x[1]):
        if s in k.lower(): print(f'  {va:#08x} {k}')
elif cmd=='callers':
    tgt=int(sys.argv[2],16)
    for a,s,n in funcs:
        raw=raw_of(a)
        if raw is None: continue
        for i in md.disasm(data[raw:raw+s], a):
            if i.mnemonic=='call' and i.op_str.startswith('0x') and int(i.op_str,16)==tgt:
                print(f'  {n} ({a:#x}) at {i.address:#x}')
else:
    va=int(cmd,16); size=int(sys.argv[2],0) if len(sys.argv)>2 else byaddr.get(va,(0x80,))[0]
    print(f'=== {nm(va)} ({va:#x}) size={size:#x} ===')
    dis(va,size)
