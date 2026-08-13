#!/usr/bin/env python3
"""xmf_inject.py -- copy one named object (mesh + materials + textures) from one XMF
container into another, producing a file the retail engine loads unmodified.

Why this exists
---------------
MEAT RUSH needs THE SAME turkey leg in every arena, but `WPH2` is not one asset: it is a
per-arena NAME SLOT and each arena's object file defines its own geometry under it (KITCHEN
= the turkey leg, LAB/HELL = a bone, CABIN = a rapier, HAUNTED = a cutlass, SCRAP = a
sledgehammer, and seven arenas have no WPH2 at all).  Rather than fight the engine's
per-arena class/type design, we inject KITCHEN's turkey-leg object into every arena's object
file under a new name and point the repurposed class record's model name at it.  The item is
then genuinely there, so spawn tables, collision, carry, the drop animation and the LAN item
stream all keep working untouched.

Container layout (RE'd from the engine loader FUN_00079EE0 -- see port/RUNTIME.md):

    [0x2C header][u32 fxmfSize][u32 prefixSize (v>=2)][prefix][IXMF][OXMF][RXMF][TXMF][IDSF][FXMF]

    header: +4 ixmfCount  +8 objCount  +0xC rxmfCount  +0x10 txmfCount
            +0x14 flag (0xabcd -> no IDSF)
            +0x18/1C/20/24/28/2C = IDSF/IXMF/OXMF/RXMF/TXMF/FXMF sizes (each INCLUDES its tag)

The loader turns section-relative offsets into pointers in place, and that fix-up pass is what
constrains where new records may go:

  * IXMF  = 'IXMF' + n x 0x54 instances; instance+0x44 is OXMF-relative.       (pure array)
  * OXMF  = 'OXMF' + objCount x 0x4C objects + <parts / descs / IB / index words>
            + a contiguous 12-byte VB-RESOURCE ARRAY THAT MUST END THE SECTION: the loader
            derives its element count as (oxmfSize - object[0].descArray[0].+4) / 12
            (0x7A361-0x7A39F).  So new bulk data goes BEFORE that array, never after.
  * RXMF  = 'RXMF' + rxmfCount x 0x50 materials.                               (pure array)
  * TXMF  = 'TXMF' + n x 0x10 records + (n - dup) x 0x14 texture headers + m x 0x0C palettes.
            `dup` counts records whose +4 equals the PREVIOUS record's +4; the loader uses
            36*n - 20*dup to find where the palette resources start (0x7A528).  We therefore
            give every injected record its OWN header (never a shared one) so `dup` cannot
            move; palettes may still be shared, which has no such rule.
  * FXMF  = 'FXMF' + bulk GPU bytes; every resource's Data(+4) is FXMF-tag-relative. (append)

Usage
-----
    python xmf_inject.py <src.xmf> <SRCNAME> <dst.xmf> <NEWNAME> [-o out.xmf] [--force]

Both names are the 4-character IXMF instance names the engine looks up (FUN_00079B50 compares
them as one dword, so they are exact and case sensitive).  Without -o the destination is
rewritten in place.  Re-running is a no-op unless --force: if NEWNAME already exists the file
is left alone, which is what makes this safe to call from make_dist.ps1 and the installer.
"""

import argparse, os, struct, sys

# ---------------------------------------------------------------- primitives

def u32(b, o):  return struct.unpack_from('<I', b, o)[0]
def i32(b, o):  return struct.unpack_from('<i', b, o)[0]
def u16(b, o):  return struct.unpack_from('<H', b, o)[0]
def w32(b, o, v): struct.pack_into('<I', b, o, v & 0xFFFFFFFF)
def w16(b, o, v): struct.pack_into('<H', b, o, v & 0xFFFF)

INST = 0x54   # IXMF instance record
OBJ  = 0x4C   # OXMF object record
PART = 0x54   # OXMF mesh part
DESC = 0x50   # OXMF vertex-buffer descriptor
RES  = 0x0C   # a bare D3D resource header (VB / index buffer / palette)
MAT  = 0x50   # RXMF material
TREC = 0x10   # TXMF table record
THDR = 0x14   # D3DPixelContainer texture header

# Engine per-token component sizes (DAT_00179CE0, used by the stride calc FUN_0007C270).
TOK = [0, 12, 8, 12, 12, 4, 4, 4, 4, 8, 12, 0, 4, 8, 4, 6]
TOK_END = 0xB
NONE = 0xFFFFFFFF


def align(n, a):
    return (n + a - 1) & ~(a - 1)


class XmfError(Exception):
    pass


def decl_stride(desc) -> int:
    stride = 0
    for i in range(16):
        tok = u32(desc, 0x10 + i * 4)
        if tok == TOK_END:
            break
        stride += TOK[tok] if tok < 16 else 0
    if stride <= 0:
        raise XmfError('vertex declaration has zero stride')
    return stride


# ---------------------------------------------------------------- container

class Xmf:
    """Parsed view over an XMF container.  Accessors return absolute file offsets so callers
    never redo the section maths."""

    def __init__(self, path, buf=None):
        self.path = path
        if buf is None:
            with open(path, 'rb') as fh:
                buf = bytearray(fh.read())
        self.buf = bytearray(buf)
        b = self.buf
        if bytes(b[:3]) != b'XMF':
            raise XmfError(f'{path}: not an XMF container')
        self.ver = b[3] - 0x30
        if self.ver != 1:
            # v>=2 adds the prefix/pushbuffer section and an extra indirection on the index
            # buffer.  Every arena object file ships as v1; refuse the rest rather than guess.
            raise XmfError(f'{path}: only XMF v1 is supported (this is v{self.ver})')
        self.ixmfCount = u32(b, 0x04)
        self.objCount  = u32(b, 0x08)
        self.rxmfCount = u32(b, 0x0C)
        self.txmfCount = u32(b, 0x10)
        self.flag      = u32(b, 0x14)
        self.idsfSize  = 0 if self.flag == 0xabcd else u32(b, 0x18)
        self.ixmfSize  = u32(b, 0x1C)
        self.oxmfSize  = u32(b, 0x20)
        self.rxmfSize  = u32(b, 0x24)
        self.txmfSize  = u32(b, 0x28)
        self.fxmfSize  = u32(b, 0x2C)

        self.ixmf = 0x30
        self.oxmf = self.ixmf + self.ixmfSize
        self.rxmf = self.oxmf + self.oxmfSize
        self.txmf = self.rxmf + self.rxmfSize
        self.idsf = self.txmf + self.txmfSize
        self.fxmf = self.idsf + self.idsfSize
        for tag, off in (('IXMF', self.ixmf), ('OXMF', self.oxmf), ('RXMF', self.rxmf),
                         ('TXMF', self.txmf), ('FXMF', self.fxmf)):
            if bytes(b[off:off + 4]) != tag.encode():
                raise XmfError(f'{path}: expected {tag} at {off:#x}, found {bytes(b[off:off+4])!r}')
        if self.idsfSize and bytes(b[self.idsf:self.idsf + 4]) != b'IDSF':
            raise XmfError(f'{path}: expected IDSF at {self.idsf:#x}')
        if self.fxmf + self.fxmfSize != len(b):
            raise XmfError(f'{path}: FXMF ends at {self.fxmf + self.fxmfSize:#x}, file is {len(b):#x}')
        self._check_layout()

    # -- layout invariants the engine loader depends on ---------------------
    def _check_layout(self):
        if self.ixmfSize != 4 + self.ixmfCount * INST:
            raise XmfError(f'{self.path}: IXMF is not a bare instance array')
        if self.rxmfSize != 4 + self.rxmfCount * MAT:
            raise XmfError(f'{self.path}: RXMF is not a bare material array')
        if self.pal_start() > self.txmfSize or (self.txmfSize - self.pal_start()) % RES:
            raise XmfError(f'{self.path}: TXMF palette region is not a whole number of resources')
        if self.oxmfSize <= self.vb_res_start() or (self.oxmfSize - self.vb_res_start()) % RES:
            raise XmfError(f'{self.path}: OXMF VB-resource region is not a multiple of 12')

    def txmf_dup(self):
        """How many TXMF records repeat the PREVIOUS record's header offset.  The loader
        registers each header once and uses this count to locate the palette region."""
        dup, prev = 0, None
        for t in range(self.txmfCount):
            h = u32(self.buf, self.trec_off(t) + 4)
            if prev is not None and h == prev:
                dup += 1
            prev = h
        return dup

    def hdr_start(self):
        return 4 + self.txmfCount * TREC

    def pal_start(self):
        return self.hdr_start() + (self.txmfCount - self.txmf_dup()) * THDR

    def vb_res_start(self):
        """OXMF-relative offset of the VB-resource array that must remain at the section's
        tail.  Derived exactly the way the loader derives it, from object 0."""
        if not self.objCount:
            raise XmfError(f'{self.path}: container has no objects (texture-only?)')
        desc_arr = u32(self.buf, self.oxmf + 4 + 0x10)
        if desc_arr + 8 > self.oxmfSize:
            raise XmfError(f'{self.path}: object 0 has no usable declaration array')
        return u32(self.buf, self.oxmf + desc_arr + 4)

    # -- record accessors ---------------------------------------------------
    def inst_off(self, i):  return self.ixmf + 4 + i * INST
    def obj_off(self, i):   return self.oxmf + 4 + i * OBJ
    def trec_off(self, i):  return self.txmf + 4 + i * TREC

    def names(self):
        return [bytes(self.buf[self.inst_off(i) + 0x48:self.inst_off(i) + 0x4C]).decode('latin1')
                for i in range(self.ixmfCount)]

    def find(self, name):
        try:
            return self.names().index(name)
        except ValueError:
            return -1

    def fxmf_extents(self):
        """Every FXMF-relative Data offset referenced anywhere in the file, mapped to the
        distance to the next one.  Exact, needs no per-format maths, and keeps whatever
        inter-blob padding the exporter emitted."""
        offs = set()
        for o in range(self.vb_res_start(), self.oxmfSize, RES):
            offs.add(u32(self.buf, self.oxmf + o + 4))
        for t in range(self.txmfCount):
            rec = self.trec_off(t)
            for rel in (u32(self.buf, rec + 4), u32(self.buf, rec + 8)):
                if rel != NONE:
                    offs.add(u32(self.buf, self.txmf + rel + 4))
        ordered = sorted(offs)
        return {o: (ordered[i + 1] if i + 1 < len(ordered) else self.fxmfSize) - o
                for i, o in enumerate(ordered)}


# ---------------------------------------------------------------- extraction

class Payload:
    """Everything needed to recreate one object inside another container.  Offsets in the
    copied records are still SOURCE-relative here; inject() rewrites them."""

    def __init__(self, src: Xmf, name: str):
        self.src = src
        b = src.buf
        oi = src.find(name)
        if oi < 0:
            raise XmfError(f'{src.path}: no object named {name!r}')
        self.name = name
        self.inst = bytearray(b[src.inst_off(oi):src.inst_off(oi) + INST])
        obj = src.obj_off(oi)
        self.obj = bytearray(b[obj:obj + OBJ])
        if u32(b, src.inst_off(oi) + 0x44) != 4 + oi * OBJ:
            raise XmfError(f'{src.path}: instance {oi} does not point at its own object record')
        if u16(b, obj + 0x1C):
            raise XmfError(f'{name}: skinned objects are not supported')

        part_off = u32(b, obj + 0x00)
        part_cnt = u16(b, obj + 0x04)
        ib_off   = u32(b, obj + 0x08)
        desc_arr = u32(b, obj + 0x10)
        desc_cnt = u16(b, obj + 0x14)
        if not part_cnt or not desc_cnt:
            raise XmfError(f'{name}: object has no parts or no vertex declarations')

        self.parts = [bytearray(b[src.oxmf + part_off + i * PART:
                                  src.oxmf + part_off + (i + 1) * PART]) for i in range(part_cnt)]
        self.descs = [bytearray(b[src.oxmf + desc_arr + i * DESC:
                                  src.oxmf + desc_arr + (i + 1) * DESC]) for i in range(desc_cnt)]
        self.ib_res = bytearray(b[src.oxmf + ib_off:src.oxmf + ib_off + RES])
        idx_words = u32(b, src.oxmf + ib_off + 4)

        self.desc_index = {desc_arr + i * DESC: i for i in range(desc_cnt)}
        for p in self.parts:
            if u32(p, 0x40) not in self.desc_index:
                raise XmfError(f'{name}: a part uses a descriptor outside the object array')
            if not u16(p, 0x14):
                raise XmfError(f'{name}: non-indexed parts are not supported')

        # -- the contiguous span of index words this object's parts reference -----------
        lo_w = min(u32(p, 0x10) for p in self.parts)
        hi_w = max(u32(p, 0x10) + u32(p, 8) + u32(p, 0xC) for p in self.parts)
        words = [u16(b, src.oxmf + idx_words + w * 2) for w in range(lo_w, hi_w)]

        # -- vertex window per declaration ---------------------------------------------
        # Absolute vertex = part.baseVertex + indexWord (the D3D8 SetIndices BaseVertexIndex,
        # confirmed at 0x843C5).  Copy [vmin..vmax] per declaration, shift every copied index
        # word down by one global constant S, and compensate per part:
        #     newBase = base - vmin + S      newWord = word - S
        # so  newBase + newWord == base + word - vmin, i.e. the vertex we copied.
        vmin, vmax = {}, {}
        for p in self.parts:
            di = self.desc_index[u32(p, 0x40)]
            base = i32(p, 0x44)
            i0, n = u32(p, 0x10), u32(p, 8) + u32(p, 0xC)
            if not n:
                raise XmfError(f'{name}: a part has no indices')
            w = words[i0 - lo_w:i0 - lo_w + n]
            vmin[di] = min(vmin.get(di, 1 << 30), base + min(w))
            vmax[di] = max(vmax.get(di, -1), base + max(w))
        shift = min(words)

        self.strides, self.vbdata = [], []
        for di, d in enumerate(self.descs):
            stride = decl_stride(d)
            self.strides.append(stride)
            if di not in vmin:
                self.vbdata.append(b'')                      # declaration no part uses
                continue
            data = u32(b, src.oxmf + u32(d, 4) + 4)          # VB resource Data, FXMF-rel
            first = src.fxmf + data + vmin[di] * stride
            count = vmax[di] - vmin[di] + 1
            if first < 0 or first + count * stride > len(b):
                raise XmfError(f'{name}: vertex window runs past the end of the file')
            self.vbdata.append(bytes(b[first:first + count * stride]))

        idx = bytearray()
        for w in words:
            idx += struct.pack('<H', w - shift)
        self.idx = bytes(idx)
        for p in self.parts:
            di = self.desc_index[u32(p, 0x40)]
            new_base = i32(p, 0x44) - vmin[di] + shift
            if new_base < 0:
                raise XmfError(f'{name}: rebased BaseVertexIndex would be negative')
            w32(p, 0x44, new_base)
            w32(p, 0x10, u32(p, 0x10) - lo_w)                # iStart into the copied span

        # -- materials, texture records/headers, palettes, GPU blobs -------------------
        ext = src.fxmf_extents()
        self.blobs = {}                    # FXMF-rel source offset -> bytes

        def take_blob(res_off):
            data = u32(b, res_off + 4)
            size = ext.get(data)
            if size is None:
                raise XmfError(f'{name}: FXMF offset {data:#x} is not a known resource')
            self.blobs[data] = bytes(b[src.fxmf + data:src.fxmf + data + size])
            return data

        self.mats = []                     # [(srcRel, bytes)]
        seen_mat = {}
        for p in self.parts:
            mrel = u32(p, 0x18)
            if mrel not in seen_mat:
                seen_mat[mrel] = len(self.mats)
                self.mats.append((mrel, bytearray(b[src.rxmf + mrel:src.rxmf + mrel + MAT])))

        # One header per record: never share, so the loader's `dup` count cannot change.
        self.trecs = []                    # [(srcRel, bytes, hdrBytes|None, palSrcRel|None)]
        self.pals = []                     # [(srcRel, bytes, blobKey)]
        seen_trec, seen_pal = {}, {}
        for _, m in self.mats:
            for s in range(4):
                rel = u32(m, 8 + s * 4)
                if rel == NONE or rel in seen_trec:
                    continue
                rec = bytearray(b[src.txmf + rel:src.txmf + rel + TREC])
                hrel, prel = u32(rec, 4), u32(rec, 8)
                hdr = None
                if hrel != NONE:
                    hdr = (bytearray(b[src.txmf + hrel:src.txmf + hrel + THDR]),
                           take_blob(src.txmf + hrel))
                if prel != NONE and prel not in seen_pal:
                    seen_pal[prel] = len(self.pals)
                    self.pals.append((prel, bytearray(b[src.txmf + prel:src.txmf + prel + RES]),
                                      take_blob(src.txmf + prel)))
                seen_trec[rel] = len(self.trecs)
                self.trecs.append((rel, rec, hdr, prel if prel != NONE else None))

    def summary(self):
        gpu = sum(len(v) for v in self.vbdata) + sum(len(v) for v in self.blobs.values())
        return (f'{len(self.parts)} part(s), {len(self.descs)} decl(s), {len(self.idx)//2} '
                f'index words, {len(self.mats)} material(s), {len(self.trecs)} texture(s), '
                f'{len(self.pals)} palette(s), {gpu} B of GPU data')


# ---------------------------------------------------------------- injection

def inject(dst: Xmf, pay: Payload, new_name: str) -> bytes:
    b = dst.buf
    if len(new_name.encode('latin1')) != 4:
        raise XmfError('the new name must be exactly 4 characters')

    vbs       = dst.vb_res_start()          # start of the untouchable VB-resource array
    mid_start = 4 + dst.objCount * OBJ      # where the new object record goes
    if not (mid_start <= vbs <= dst.oxmfSize):
        raise XmfError('unexpected OXMF layout: VB-resource array is not at the tail')

    # ---- the new OXMF middle block: parts | descs | IB resource | index words ---------
    mid = bytearray()
    off_parts = 0
    mid += b''.join(bytes(p) for p in pay.parts)
    off_descs = len(mid)
    mid += b''.join(bytes(d) for d in pay.descs)
    off_ib = len(mid)
    mid += bytes(pay.ib_res)
    off_idx = align(len(mid), 4)
    mid += b'\0' * (off_idx - len(mid))
    mid += pay.idx
    mid += b'\0' * (align(len(mid), 4) - len(mid))
    new_mid = len(mid)

    delta_mid = OBJ                          # existing middle data shifts by one object record
    delta_vb  = OBJ + new_mid                # the VB array shifts by that plus the new block

    def shift(v):
        # The object array does not move -- the new record is appended to its end -- so
        # pointers INTO it (every instance's +0x44, every part's object back-pointer) must
        # be left alone.  Only the middle data and the VB-resource array slide.
        if v < mid_start:
            return v
        return v + (delta_vb if v >= vbs else delta_mid)

    base_mid    = vbs + delta_mid                                # where `mid` lands (OXMF-rel)
    new_obj_rel = mid_start
    new_vb_rel  = vbs + delta_vb + (dst.oxmfSize - vbs)          # first new VB resource

    # ---- RXMF / TXMF placement -------------------------------------------------------
    n_new_mat  = len(pay.mats)
    n_new_trec = len(pay.trecs)
    n_new_hdr  = sum(1 for t in pay.trecs if t[2] is not None)
    old_hdr_start, old_pal_start = dst.hdr_start(), dst.pal_start()
    n_old_pal  = (dst.txmfSize - old_pal_start) // RES
    new_hdr_start = 4 + (dst.txmfCount + n_new_trec) * TREC
    new_pal_start = new_hdr_start + (dst.txmfCount - dst.txmf_dup() + n_new_hdr) * THDR
    d_hdr = new_hdr_start - old_hdr_start
    d_pal = new_pal_start - old_pal_start

    # ---- FXMF placement --------------------------------------------------------------
    fx_tail, blob_at = bytearray(), {}
    fx_base = dst.fxmfSize
    for key, data in pay.blobs.items():
        fx_tail += b'\0' * (align(fx_base + len(fx_tail), 0x80) - (fx_base + len(fx_tail)))
        blob_at[key] = fx_base + len(fx_tail)
        fx_tail += data
    vb_at = []
    for v in pay.vbdata:
        if not v:
            vb_at.append(None)
            continue
        fx_tail += b'\0' * (align(fx_base + len(fx_tail), 0x20) - (fx_base + len(fx_tail)))
        vb_at.append(fx_base + len(fx_tail))
        fx_tail += v

    # ---- destination-relative offsets for every new record ---------------------------
    new_mat_rel  = {srel: 4 + (dst.rxmfCount + i) * MAT for i, (srel, _) in enumerate(pay.mats)}
    new_trec_rel = {t[0]: 4 + (dst.txmfCount + i) * TREC for i, t in enumerate(pay.trecs)}
    new_hdr_rel, new_pal_rel = {}, {}
    h = 0
    for t in pay.trecs:
        if t[2] is not None:
            new_hdr_rel[t[0]] = new_hdr_start + (dst.txmfCount - dst.txmf_dup() + h) * THDR
            h += 1
    for i, (srel, _, _) in enumerate(pay.pals):
        new_pal_rel[srel] = new_pal_start + (n_old_pal + i) * RES

    # ---- rewrite the payload's own records -------------------------------------------
    inst = bytearray(pay.inst)
    inst[0x48:0x4C] = new_name.encode('latin1')
    w32(inst, 0x44, new_obj_rel)
    inst[0x40:0x44] = b[dst.inst_off(0) + 0x40:dst.inst_off(0) + 0x44]   # exporter constant

    obj = bytearray(pay.obj)
    obj[0x20:0x24] = new_name.encode('latin1')
    w32(obj, 0x00, base_mid + off_parts)
    w32(obj, 0x08, base_mid + off_ib)
    w32(obj, 0x10, base_mid + off_descs)
    w32(obj, 0x18, 0)                                                   # bone matrices
    w16(obj, 0x16, u16(b, dst.obj_off(0) + 0x16))                       # exporter constants
    w16(obj, 0x1E, u16(b, dst.obj_off(0) + 0x1E))

    ib_res = bytearray(pay.ib_res)
    w32(ib_res, 4, base_mid + off_idx)
    mid[off_ib:off_ib + RES] = ib_res

    for i, p in enumerate(pay.parts):
        q = bytearray(p)
        w32(q, 0x00, new_obj_rel)                                       # object back-pointer
        w32(q, 0x18, new_mat_rel[u32(p, 0x18)])                         # RXMF-rel material
        w32(q, 0x40, base_mid + off_descs + pay.desc_index[u32(p, 0x40)] * DESC)
        mid[off_parts + i * PART:off_parts + (i + 1) * PART] = q

    new_vb_res, slot = bytearray(), 0
    for di, d in enumerate(pay.descs):
        q = bytearray(d)
        if vb_at[di] is None:
            w32(q, 4, 0)
        else:
            res = bytearray(pay.src.buf[pay.src.oxmf + u32(d, 4):pay.src.oxmf + u32(d, 4) + RES])
            w32(res, 4, vb_at[di])
            w32(q, 4, new_vb_rel + slot * RES)
            new_vb_res += res
            slot += 1
        mid[off_descs + di * DESC:off_descs + (di + 1) * DESC] = q

    new_mats = bytearray()
    for _, m in pay.mats:
        q = bytearray(m)
        for s in range(4):
            rel = u32(q, 8 + s * 4)
            if rel != NONE:
                w32(q, 8 + s * 4, new_trec_rel[rel])
        new_mats += q

    new_trecs, new_hdrs, new_pals = bytearray(), bytearray(), bytearray()
    for srel, rec, hdr, prel in pay.trecs:
        q = bytearray(rec)
        w32(q, 4, new_hdr_rel[srel] if hdr is not None else NONE)
        w32(q, 8, new_pal_rel[prel] if prel is not None else NONE)
        new_trecs += q
        if hdr is not None:
            hq = bytearray(hdr[0])
            w32(hq, 4, blob_at[hdr[1]])
            new_hdrs += hq
    for _, res, key in pay.pals:
        q = bytearray(res)
        w32(q, 4, blob_at[key])
        new_pals += q

    # ---- assemble --------------------------------------------------------------------
    hdr = bytearray(b[0:0x30])
    w32(hdr, 0x04, dst.ixmfCount + 1)
    w32(hdr, 0x08, dst.objCount + 1)
    w32(hdr, 0x0C, dst.rxmfCount + n_new_mat)
    w32(hdr, 0x10, dst.txmfCount + n_new_trec)
    w32(hdr, 0x1C, dst.ixmfSize + INST)
    w32(hdr, 0x20, dst.oxmfSize + OBJ + new_mid + len(new_vb_res))
    w32(hdr, 0x24, dst.rxmfSize + len(new_mats))
    w32(hdr, 0x28, dst.txmfSize + len(new_trecs) + len(new_hdrs) + len(new_pals))
    w32(hdr, 0x2C, dst.fxmfSize + len(fx_tail))
    out = bytearray(hdr)

    # IXMF: existing instances (object pointers shifted) then the new one
    ixmf = bytearray(b[dst.ixmf:dst.ixmf + dst.ixmfSize])
    for i in range(dst.ixmfCount):
        o = 4 + i * INST + 0x44
        w32(ixmf, o, shift(u32(ixmf, o)))
    out += ixmf + inst

    # OXMF: every internal pointer moves.  Collect the SITES first -- objects may share a
    # part array, an index buffer or a declaration array, and a site must be shifted once.
    oxmf = bytearray(b[dst.oxmf:dst.oxmf + dst.oxmfSize])
    sites = set()
    for i in range(dst.objCount):
        o = 4 + i * OBJ
        sites.update((o + 0x00, o + 0x08, o + 0x10))
        part_off, part_cnt = u32(oxmf, o + 0x00), u16(oxmf, o + 0x04)
        for pi in range(part_cnt):
            sites.add(part_off + pi * PART + 0x40)
        sites.add(u32(oxmf, o + 0x08) + 4)                          # IB resource -> words
        desc_arr, desc_cnt = u32(oxmf, o + 0x10), u16(oxmf, o + 0x14)
        for k in range(desc_cnt):
            sites.add(desc_arr + k * DESC + 4)                      # descriptor -> VB resource
    for s in sorted(sites):
        w32(oxmf, s, shift(u32(oxmf, s)))
    out += oxmf[0:4]                          # 'OXMF'
    out += oxmf[4:mid_start]                  # existing object records
    out += obj                                # the new object record
    out += oxmf[mid_start:vbs]                # existing middle data
    out += mid                                # the new middle block
    out += oxmf[vbs:dst.oxmfSize]             # existing VB resources
    out += new_vb_res                         # the new VB resources

    # RXMF: materials are a pure array -- append.  Existing offsets are unaffected.
    out += b[dst.rxmf:dst.rxmf + dst.rxmfSize] + new_mats

    # TXMF: records | headers | palettes, each group appended to.  Records never move, so
    # the material texture slots that point at them stay valid.
    txmf = bytearray(b[dst.txmf:dst.txmf + dst.txmfSize])
    for t in range(dst.txmfCount):
        r = 4 + t * TREC
        if u32(txmf, r + 4) != NONE:
            w32(txmf, r + 4, u32(txmf, r + 4) + d_hdr)
        if u32(txmf, r + 8) != NONE:
            w32(txmf, r + 8, u32(txmf, r + 8) + d_pal)
    out += txmf[0:4] + txmf[4:old_hdr_start] + new_trecs
    out += txmf[old_hdr_start:old_pal_start] + new_hdrs
    out += txmf[old_pal_start:dst.txmfSize] + new_pals

    if dst.idsfSize:
        out += b[dst.idsf:dst.idsf + dst.idsfSize]
    out += b[dst.fxmf:dst.fxmf + dst.fxmfSize] + fx_tail
    return bytes(out)


# ---------------------------------------------------------------- verification

def validate(x: Xmf):
    """Walk every cross-reference in the container and confirm it lands in the region the
    engine's fix-up pass expects.  This is what catches an injector arithmetic slip: the
    mesh reader in re/item_render/ assumes instance index == object index and would happily
    decode a file whose instance pointers had all slid by one record."""
    b, bad = x.buf, []
    obj_end = 4 + x.objCount * OBJ
    vbs = x.vb_res_start()
    hdr_start, pal_start = x.hdr_start(), x.pal_start()

    def want(cond, msg):
        if not cond:
            bad.append(msg)

    for i in range(x.ixmfCount):
        v = u32(b, x.inst_off(i) + 0x44)
        want(4 <= v < obj_end and (v - 4) % OBJ == 0, f'instance {i}: object ptr {v:#x} is not an object record')
        want(v == 4 + i * OBJ, f'instance {i}: points at object {(v - 4)//OBJ}, not itself')

    for i in range(x.objCount):
        o = x.obj_off(i)
        part_off, part_cnt = u32(b, o + 0x00), u16(b, o + 0x04)
        ib, desc_arr, desc_cnt = u32(b, o + 0x08), u32(b, o + 0x10), u16(b, o + 0x14)
        want(obj_end <= part_off and part_off + part_cnt * PART <= vbs,
             f'object {i}: part array {part_off:#x}+{part_cnt} outside the middle region')
        want(obj_end <= ib and ib + RES <= vbs, f'object {i}: IB resource {ib:#x} outside the middle region')
        want(obj_end <= desc_arr and desc_arr + desc_cnt * DESC <= vbs,
             f'object {i}: decl array {desc_arr:#x} outside the middle region')
        if bad:
            break
        words = u32(b, x.oxmf + ib + 4)
        want(obj_end <= words <= vbs, f'object {i}: index words {words:#x} outside the middle region')
        for k in range(desc_cnt):
            d = desc_arr + k * DESC
            vb = u32(b, x.oxmf + d + 4)
            want(vbs <= vb < x.oxmfSize and (vb - vbs) % RES == 0,
                 f'object {i} decl {k}: VB resource {vb:#x} is not in the tail array')
            want(u32(b, x.oxmf + vb + 4) < x.fxmfSize, f'object {i} decl {k}: VB data past FXMF')
        for pi in range(part_cnt):
            p = x.oxmf + part_off + pi * PART
            want(desc_arr <= u32(b, p + 0x40) < desc_arr + desc_cnt * DESC,
                 f'object {i} part {pi}: declaration {u32(b, p+0x40):#x} outside this object\'s array')
            m = u32(b, p + 0x18)
            want(4 <= m < x.rxmfSize and (m - 4) % MAT == 0,
                 f'object {i} part {pi}: material {m:#x} is not a material record')
            want(words + (u32(b, p + 0x10) + u32(b, p + 8) + u32(b, p + 0xC)) * 2 <= vbs,
                 f'object {i} part {pi}: index range runs past the middle region')

    for m in range(x.rxmfCount):
        r = x.rxmf + 4 + m * MAT
        for s in range(4):
            rel = u32(b, r + 8 + s * 4)
            if rel == NONE:
                continue
            want(4 <= rel < hdr_start and (rel - 4) % TREC == 0,
                 f'material {m} slot {s}: {rel:#x} is not a TXMF record')
    for t in range(x.txmfCount):
        rec = x.trec_off(t)
        h, p = u32(b, rec + 4), u32(b, rec + 8)
        if h != NONE:
            want(hdr_start <= h < pal_start and (h - hdr_start) % THDR == 0,
                 f'texture record {t}: header {h:#x} outside the header region')
            want(u32(b, x.txmf + h + 4) < x.fxmfSize, f'texture record {t}: pixels past FXMF')
        if p != NONE:
            want(pal_start <= p < x.txmfSize and (p - pal_start) % RES == 0,
                 f'texture record {t}: palette {p:#x} outside the palette region')
            want(u32(b, x.txmf + p + 4) < x.fxmfSize, f'texture record {t}: palette data past FXMF')
    return bad


def verify(path, name):
    """Re-parse the produced file with the same reader the renderer uses and report what the
    new object actually decodes to."""
    here = os.path.dirname(os.path.abspath(__file__))
    render = os.path.normpath(os.path.join(here, '..', '..', 're', 'item_render'))
    if render not in sys.path:
        sys.path.insert(0, render)
    import xmf as reader                                    # re/item_render/xmf.py
    s = reader.load(path, name)
    return len(s['parts']), sum(len(p['tri']) for p in s['parts'])


def inject_file(src_path, src_name, dst_path, new_name, out_path=None, force=False, quiet=False):
    dst = Xmf(dst_path)
    out_path = out_path or dst_path
    if dst.find(new_name) >= 0 and not force:
        if not quiet:
            print(f'{os.path.basename(dst_path)}: already has {new_name!r} -- skipped')
        if os.path.abspath(out_path) != os.path.abspath(dst_path):
            with open(out_path, 'wb') as fh:
                fh.write(dst.buf)
        return False
    pay = Payload(Xmf(src_path), src_name)
    data = inject(dst, pay, new_name)
    out = Xmf('<output>', data)                             # re-validates the section layout
    bad = validate(out)                                     # ...and every cross-reference
    if bad:
        raise XmfError(f'{os.path.basename(dst_path)}: injected file fails validation:\n  '
                       + '\n  '.join(bad[:8]))
    tmp = out_path + '.tmp'
    with open(tmp, 'wb') as fh:
        fh.write(data)
    try:
        parts, tris = verify(tmp, new_name)
        if parts == 0:
            raise XmfError('the injected object decodes to zero renderable parts')
    except Exception:
        os.remove(tmp)
        raise
    os.replace(tmp, out_path)
    if not quiet:
        print(f'{os.path.basename(dst_path):14s} +{new_name} <- {src_name}: {pay.summary()} '
              f'-> {parts} part(s) / {tris} tris, {len(dst.buf):,} -> {len(data):,} B')
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('src'); ap.add_argument('srcname')
    ap.add_argument('dst'); ap.add_argument('newname')
    ap.add_argument('-o', '--out', default=None)
    ap.add_argument('--force', action='store_true',
                    help='inject even if NEWNAME already exists in the destination')
    ap.add_argument('-q', '--quiet', action='store_true')
    a = ap.parse_args(argv)
    inject_file(a.src, a.srcname, a.dst, a.newname, a.out, a.force, a.quiet)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except XmfError as e:
        print('xmf_inject: ' + str(e), file=sys.stderr)
        sys.exit(2)
