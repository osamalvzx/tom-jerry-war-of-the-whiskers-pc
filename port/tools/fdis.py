#!/usr/bin/env python3
# linear sweep from VA until ret followed by int3 padding; prints calls annotated
import sys,struct,csv
import capstone
P=r"D:/Projects/Tom and Jerry in War of the Whiskers (U)"
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
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
funcs=[]
for r in csv.reader(open(f"{P}/re/functions.csv")):
    if r[0]=='address':continue
    funcs.append((int(r[0],16),int(r[1]),r[3]))
funcs.sort()
NAMED={0x11f20:'MT_raw',0x12050:'MT_float01',0x12070:'MT_thunk',0x12080:'MT_x',0x120a0:'MT_range',0x8cc4c:'CRT_rand',0x11ee0:'MT_seed',0x8cc3f:'CRT_srand',
 0x339b0:'SpawnObject',0x33e50:'SetObjState',0x4ad20:'FX_Spawn',0x45be0:'DamageFighter',0x317e0:'DropFoodItems',0x2e0a0:'GetIdleTick'}
def nm(va):
    if va in NAMED: return NAMED[va]
    for a,s,n in funcs:
        if a<=va<a+s: return n if va==a else f'{n}+{va-a:#x}'
    return '?'
va=int(sys.argv[1],16); maxn=int(sys.argv[2],0) if len(sys.argv)>2 else 0x800
ro=raw_of(va)
out=[]
prev_ret=False
onlycalls = '-c' in sys.argv
for i in md.disasm(data[ro:ro+maxn],va):
    if i.mnemonic=='int3' and prev_ret: break
    prev_ret = i.mnemonic in ('ret','jmp')
    t=''
    if i.mnemonic in('call','jmp') and i.op_str.startswith('0x'):
        t=' ; '+nm(int(i.op_str,16))
    if onlycalls:
        if i.mnemonic=='call': print(f'  {i.address:#08x}: {i.mnemonic} {i.op_str}{t}')
    else:
        print(f'  {i.address:#08x}: {i.mnemonic} {i.op_str}{t}')
