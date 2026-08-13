import struct,csv,sys,collections
import capstone
P=r"D:/Projects/Tom and Jerry in War of the Whiskers (U)"
data=open(f"{P}/extracted/default.xbe","rb").read()
base=0x10000
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
funcs={}
for r in csv.reader(open(f"{P}/re/functions.csv")):
    if r[0]=='address':continue
    funcs[int(r[0],16)]=int(r[1])
def edges(a):
    s=funcs.get(a,0x200); ro=raw_of(a)
    out=set()
    if ro is None: return out
    for i in md.disasm(data[ro:ro+s],a):
        if i.mnemonic in ('call','jmp') and i.op_str.startswith('0x'):
            out.add(int(i.op_str,16))
    return out
RNG={0x11f20:'MT_raw',0x12050:'MT_float',0x12070:'MT_thunk',0x12080:'MT_?',0x120a0:'MT_range',0x8cc4c:'LCG_rand',0x11ee0:'MT_seed',0x8cc3f:'LCG_srand'}
def closure(root):
    seen=set();stack=[root]
    while stack:
        a=stack.pop()
        if a in seen: continue
        seen.add(a)
        for t in edges(a):
            if t not in seen: stack.append(t)
    return seen
for arg in sys.argv[1:]:
    a=int(arg,16)
    c=closure(a)
    hits=sorted(set(c)&set(RNG))
    print(f'{a:#x}: closure={len(c)} RNG={[RNG[h] for h in hits]}')
