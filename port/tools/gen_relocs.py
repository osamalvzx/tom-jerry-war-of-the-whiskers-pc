"""Generate a precise relocation table for absolute data references in .text.

Byte-scanning for address-shaped values corrupts overlapping instruction bytes, so we
disassemble instead and record the exact file offset of each disp32 operand that points
into the data segment [kDataImageBase, kDataImageEnd). The runtime adds the load delta
at exactly those offsets -- no false positives. This table is also what the eventual
XBE loader uses to run the original code from a relocated image.

Output: port/src/game/generated/text_relocs.bin  (array of little-endian uint32 offsets)
"""
import struct, os, capstone

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT  = os.path.join(ROOT, "port", "src", "game", "generated")
text = open(os.path.join(OUT, "text_image.bin"), "rb").read()

# Must match data_image.h.
TEXT_BASE = 0x11000
DATA_BASE = 0xEE000
DATA_END  = 0x165217C

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

# Disassemble per-function using Ghidra's function boundaries. A single linear sweep of
# the whole section desyncs on inter-function padding / embedded data and then misplaces
# or drops operands; per-function keeps instruction boundaries exact.
import csv
funcs = []
csv_path = os.path.join(ROOT, "re", "functions.csv")
for row in csv.DictReader(open(csv_path)):
    if row["section"] != ".text":
        continue
    va = int(row["address"], 16)
    size = int(row["size"])
    funcs.append((va, size))

# Pass 1: collect every value that appears as an absolute memory-operand address.
# These are provably data pointers. An immediate is only treated as an address if it
# is in this set -- this distinguishes real pointers (e.g. the RNG loop bound 0x183f3c,
# which is also accessed as [0x183f3c]) from fill constants (e.g. 0x01010101).
known_addrs = set()
for va, size in funcs:
    code = text[va - TEXT_BASE: va - TEXT_BASE + size]
    for ins in md.disasm(code, va):
        if ins.disp_size == 4 and (DATA_BASE <= (ins.disp & 0xFFFFFFFF) < DATA_END):
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == 0:
                    known_addrs.add(ins.disp & 0xFFFFFFFF)
                    break

offsets = []
for va, size in funcs:
    off = va - TEXT_BASE
    code = text[off:off + size]
    for ins in md.disasm(code, va):
        # (a) absolute memory-operand displacements. All addressing forms count —
        #     including [reg + disp] (register-indexed global tables like
        #     `test byte [edx + 0x172d01]`): no game struct is remotely 0xEE000+
        #     bytes, so a data-range disp is always a global address.
        if ins.disp_size == 4 and (DATA_BASE <= (ins.disp & 0xFFFFFFFF) < DATA_END):
            for op in ins.operands:
                if op.type == capstone.x86.X86_OP_MEM:
                    offsets.append((ins.address - TEXT_BASE) + ins.disp_offset)
                    break
        # (b) address-valued immediates. A data-range immediate is a pointer if EITHER
        #     it also appears as a memory address (loop bounds like cmp ecx,0x183f3c) OR
        #     it is PUSHed (a pointer argument -- e.g. format strings `push 0xefeac`
        #     that are never dereferenced within this binary). Pushed data addresses are
        #     always real pointers, so this is safe; other immediates still require the
        #     known-address gate to avoid relocating in-range fill/size constants.
        if ins.imm_size == 4:
            for op in ins.operands:
                if op.type != capstone.x86.X86_OP_IMM:
                    continue
                imm = op.imm & 0xFFFFFFFF
                in_data = DATA_BASE <= imm < DATA_END
                is_addr = imm in known_addrs
                # Pointer-formation idioms carrying a data-segment address:
                #   push imm            -> pointer argument (format strings, literals)
                #   add reg, imm        -> base + index table addressing (add ecx,0x16a148)
                #   lea reg, [imm]      -> load table base
                # These are always real pointers; arithmetic/size constants that happen
                # to land in the data band use mov/cmp/and/etc. and are left alone.
                is_ptr_form = in_data and ins.mnemonic in ("push", "add", "lea")
                if is_addr or is_ptr_form:
                    offsets.append((ins.address - TEXT_BASE) + ins.imm_offset)
                    break

offsets = sorted(set(offsets))

# Pass 3: switch jump tables. MSVC emits `jmp [idx*4 + T]` with T (and often a byte
# index table) embedded in .text right after the function. When the code runs
# relocated, both the table BASE displacements and the table ENTRIES (absolute code
# addresses) must be shifted by the text load delta.
TEXT_END = TEXT_BASE + len(text)
func_starts = sorted(v for v, s in funcs)
import bisect
text_offsets = []
for va, size in funcs:
    off = va - TEXT_BASE
    code = text[off:off + size]
    table_bases = []
    for ins in md.disasm(code, va):
        if ins.disp_size != 4:
            continue
        disp = ins.disp & 0xFFFFFFFF
        if not (TEXT_BASE <= disp < TEXT_END):
            continue
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == 0 and op.mem.scale == 4:
                # dword jump/handler table base
                text_offsets.append((ins.address - TEXT_BASE) + ins.disp_offset)
                table_bases.append(disp)
                break
    if not table_bases:
        continue
    # table entries: dwords holding code addresses; stop at a non-code value or the
    # next function's start (tables live in the gap after the function).
    for tb in table_bases:
        nxt = bisect.bisect_right(func_starts, tb)
        limit = func_starts[nxt] if nxt < len(func_starts) else TEXT_END
        p = tb
        while p + 4 <= limit:
            v = struct.unpack_from("<I", text, p - TEXT_BASE)[0]
            if not (TEXT_BASE <= v < TEXT_END):
                break
            text_offsets.append(p - TEXT_BASE)
            p += 4
    # byte index tables: other text-range displacements adjacent to a dword table
    # (e.g. `movzx eax, byte [eax + T2]`); only the base moves, contents are indices.
    for ins in md.disasm(code, va):
        if ins.disp_size != 4:
            continue
        disp = ins.disp & 0xFFFFFFFF
        if not (TEXT_BASE <= disp < TEXT_END):
            continue
        if any(abs(disp - tb) < 0x400 for tb in table_bases):
            site = (ins.address - TEXT_BASE) + ins.disp_offset
            if site not in text_offsets:
                text_offsets.append(site)

text_offsets = sorted(set(text_offsets))

# v2 container: magic, counts, then data-delta sites followed by text-delta sites.
with open(os.path.join(OUT, "text_relocs.bin"), "wb") as f:
    f.write(b"TRL2")
    f.write(struct.pack("<II", len(offsets), len(text_offsets)))
    for o in offsets:
        f.write(struct.pack("<I", o))
    for o in text_offsets:
        f.write(struct.pack("<I", o))

with open(os.path.join(OUT, "text_relocs.h"), "w") as f:
    f.write("// GENERATED by gen_relocs.py -- offsets into .text of disp32 relocation sites.\n")
    f.write("// v2: data-segment sites (+data delta) and jump-table sites (+text delta).\n")
    f.write("#pragma once\n#include <cstdint>\nnamespace tj::game {\n")
    f.write(f"constexpr uint32_t kNumTextRelocs = {len(offsets)}u;\n")
    f.write(f"constexpr uint32_t kNumJumpTableRelocs = {len(text_offsets)}u;\n")
    f.write("bool ApplyTextRelocs(const char* binPath);  // add load deltas at each site\n")
    f.write("} // namespace tj::game\n")

print(f"{len(offsets)} data-segment + {len(text_offsets)} jump-table relocation sites written")
