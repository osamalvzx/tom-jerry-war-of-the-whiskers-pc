#!/usr/bin/env python3
# read dwords / find dword references in the XBE image.  usage:
#   rd.py d <va> [count]      dump dwords (symbolized)
#   rd.py b <va> [count]      dump bytes
#   rd.py x <value>           find every 4-byte-aligned dword equal to value
#   rd.py s <va> [max]        read a C string
import sys, struct, csv
P="D:/Projects/Tom and Jerry in War of the Whiskers (U)"
data=open(f"{P}/extracted/default.xbe","rb").read()
base=0x10000
nsec=struct.unpack_from('<I',data,0x11c)[0]; secoff=struct.unpack_from('<I',data,0x120)[0]-base
SECS=[]
for i in range(nsec):
    o=secoff+i*0x38
    f,va,vsz,ro,rsz,na=struct.unpack_from('<IIIIII',data,o)
    nm=data[na-base:data.index(b'\0',na-base)].decode()
    SECS.append((nm,va,vsz,ro,rsz))
def raw_of(va):
    for nm,v,vsz,ro,rsz in SECS:
        if v<=va<v+vsz:
            off=va-v
            if off<rsz: return ro+off
    return None
funcs=[]
for r in csv.reader(open(f"{P}/re/functions.csv")):
    if r[0]=='address':continue
    funcs.append((int(r[0],16),int(r[1]),r[3]))
funcs.sort()
xdk={}
for ln in open(f"{P}/re/xdk_symbols.txt",encoding='utf-8-sig'):
    if '=' in ln:
        k,v=ln.split('='); xdk[int(v.strip(),16)]=k.strip()
def nm(va):
    if va in xdk: return xdk[va]
    for a,s,n in funcs:
        if a<=va<a+s: return n if va==a else f'{n}+{va-a:#x}'
    return ''
c=sys.argv[1]
if c=='d':
    va=int(sys.argv[2],16); n=int(sys.argv[3],0) if len(sys.argv)>3 else 16
    ro=raw_of(va)
    for i in range(n):
        v=struct.unpack_from('<I',data,ro+i*4)[0]
        print(f'  {va+i*4:#08x} [+{i*4:#04x}] = {v:#010x}  {nm(v)}')
elif c=='b':
    va=int(sys.argv[2],16); n=int(sys.argv[3],0) if len(sys.argv)>3 else 64
    ro=raw_of(va)
    for i in range(0,n,16):
        chunk=data[ro+i:ro+i+16]
        print(f'  {va+i:#08x}: '+' '.join(f'{b:02x}' for b in chunk)+'  '+''.join(chr(b) if 32<=b<127 else '.' for b in chunk))
elif c=='x':
    tgt=int(sys.argv[2],16)
    pk=struct.pack('<I',tgt)
    for nmn,v,vsz,ro,rsz in SECS:
        off=0
        while True:
            k=data.find(pk,ro+off,ro+rsz)
            if k<0: break
            va=v+(k-ro)
            print(f'  {va:#08x} ({nmn}) {nm(va)}')
            off=k-ro+1
elif c=='s':
    va=int(sys.argv[2],16); ro=raw_of(va)
    e=data.index(b'\0',ro)
    print(repr(data[ro:e].decode('latin1')))
