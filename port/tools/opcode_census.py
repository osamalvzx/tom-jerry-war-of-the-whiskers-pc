#!/usr/bin/env python3
"""Opcode census over the game's XBE, driven by the Ghidra function list.

Disassembles every function in functions.csv (skipping .data rows) with
capstone in 32-bit x86 mode and produces:
  - per-mnemonic counts: overall, per section, and for the differential-tested set
  - per-function records (address, section, insn count, mnemonic set)
  - lists of FS/GS-override sites, indirect jmps, unusual/privileged insns
  - x87 mnemonic counts
  - every position where capstone stopped early (undecoded bytes)

Outputs census.json and a markdown report.
"""

import argparse
import csv
import json
import struct
import sys
from collections import Counter, defaultdict

import capstone

UNUSUAL_MNEMONICS = {
    "in", "out", "ins", "insb", "insw", "insd", "outs", "outsb", "outsw", "outsd",
    "int", "int1", "int3", "into", "iret", "iretd", "hlt", "cpuid", "rdtsc",
    "rdpmc", "rdmsr", "wrmsr", "sti", "cli", "clts", "invd", "wbinvd", "invlpg",
    "lgdt", "sgdt", "lidt", "sidt", "lldt", "sldt", "ltr", "str", "lmsw", "smsw",
    "arpl", "lar", "lsl", "verr", "verw", "sysenter", "sysexit", "syscall",
    "sysret", "ud0", "ud1", "ud2", "lds", "les", "lfs", "lgs", "lss",
    "ljmp", "lcall", "retf", "bound", "salc", "xlatb", "wait", "fwait",
}


def parse_xbe_sections(xbe_path):
    """Parse the XBE section table. Returns (base, [section dict])."""
    with open(xbe_path, "rb") as f:
        data = f.read()

    def u32(off):
        return struct.unpack_from("<I", data, off)[0]

    base = u32(0x104)
    nsec = u32(0x11C)
    sec_hdr_va = u32(0x120)
    hdr_off = sec_hdr_va - base

    sections = []
    for i in range(nsec):
        off = hdr_off + i * 0x38
        flags = u32(off + 0)
        va = u32(off + 4)
        vsize = u32(off + 8)
        raw_off = u32(off + 0xC)
        raw_size = u32(off + 0x10)
        name_addr = u32(off + 0x14)
        name_off = name_addr - base
        end = data.index(b"\x00", name_off)
        name = data[name_off:end].decode("ascii", "replace")
        sections.append({
            "name": name, "flags": flags, "va": va, "vsize": vsize,
            "raw_off": raw_off, "raw_size": raw_size,
        })
    return base, sections, data


def va_to_off(sections, va):
    """Map a VA to a file offset, or None if it lands in no section's raw data."""
    for s in sections:
        if s["va"] <= va < s["va"] + s["raw_size"]:
            return s["raw_off"] + (va - s["va"])
    return None


def main():
    ap = argparse.ArgumentParser(description="x86-32 opcode census over an XBE")
    ap.add_argument("--xbe", required=True, help="path to default.xbe")
    ap.add_argument("--functions", required=True, help="functions.csv (address,size,section,name)")
    ap.add_argument("--tested", required=True, help="file with hex VAs (no 0x) of tested functions")
    ap.add_argument("--out-json", required=True, help="census.json output path")
    ap.add_argument("--out-report", required=True, help="markdown report output path")
    args = ap.parse_args()

    base, sections, xbe = parse_xbe_sections(args.xbe)
    print(f"XBE base 0x{base:08X}, {len(sections)} sections:")
    print(f"  {'name':<10} {'va':>10} {'vsize':>8} {'rawOff':>8} {'rawSize':>8}")
    for s in sections:
        print(f"  {s['name']:<10} 0x{s['va']:08X} {s['vsize']:>8} 0x{s['raw_off']:06X} {s['raw_size']:>8}")

    tested = set()
    with open(args.tested) as f:
        for line in f:
            line = line.strip()
            if line:
                tested.add(int(line, 16))
    print(f"{len(tested)} tested-function VAs loaded")

    funcs = []
    with open(args.functions, newline="") as f:
        for row in csv.DictReader(f):
            if row["section"] == ".data":
                continue
            funcs.append({
                "address": int(row["address"], 16),
                "size": int(row["size"]),
                "section": row["section"],
                "name": row["name"],
            })
    print(f"{len(funcs)} functions to disassemble")

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    total_counts = Counter()
    section_counts = defaultdict(Counter)
    tested_counts = Counter()
    fn_records = []
    undecoded = []          # {va, bytes, function, skipped}
    fs_gs_sites = []        # {va, mnemonic+op_str, prefix}
    indirect_jmps = []      # {va, text}
    unusual = []            # {va, text} — excluding pure int3 padding tallied separately
    int3_count = 0
    total_insns = 0

    X86_PREFIX_FS = 0x64
    X86_PREFIX_GS = 0x65

    for fn in funcs:
        va, size = fn["address"], fn["size"]
        off = va_to_off(sections, va)
        if off is None:
            undecoded.append({"va": f"0x{va:08X}", "bytes": "", "function": fn["name"],
                              "skipped": size, "reason": "VA not in any section raw data"})
            continue
        code = xbe[off:off + size]
        mnem_set = set()
        n = 0
        last_end = va
        for insn in md.disasm(code, va):
            n += 1
            last_end = insn.address + insn.size
            m = insn.mnemonic
            total_counts[m] += 1
            section_counts[fn["section"]][m] += 1
            if fn["address"] in tested:
                tested_counts[m] += 1
            mnem_set.add(m)

            if X86_PREFIX_FS in insn.prefix or X86_PREFIX_GS in insn.prefix:
                fs_gs_sites.append({
                    "va": f"0x{insn.address:08X}",
                    "text": f"{m} {insn.op_str}".strip(),
                    "prefix": "fs" if X86_PREFIX_FS in insn.prefix else "gs",
                })
            if m == "jmp" and insn.operands and insn.operands[0].type != capstone.x86.X86_OP_IMM:
                indirect_jmps.append({"va": f"0x{insn.address:08X}",
                                      "text": f"{m} {insn.op_str}".strip()})
            if m == "int3":
                int3_count += 1
            elif m in UNUSUAL_MNEMONICS:
                unusual.append({"va": f"0x{insn.address:08X}",
                                "text": f"{m} {insn.op_str}".strip()})

        total_insns += n
        if last_end < va + size:
            skipped = (va + size) - last_end
            bad_off = off + (last_end - va)
            undecoded.append({
                "va": f"0x{last_end:08X}",
                "bytes": xbe[bad_off:bad_off + min(skipped, 16)].hex(),
                "function": fn["name"],
                "skipped": skipped,
            })
        fn_records.append({
            "address": f"0x{va:08X}",
            "section": fn["section"],
            "name": fn["name"],
            "size": size,
            "insn_count": n,
            "mnemonics": sorted(mnem_set),
            "tested": fn["address"] in tested,
        })

    x87 = {m: c for m, c in sorted(total_counts.items())
           if m.startswith("f") and m not in ("fs",)}  # capstone x87 mnemonics all start with f

    census = {
        "xbe": args.xbe,
        "base": f"0x{base:08X}",
        "sections": [{k: (f"0x{v:X}" if k in ("va", "raw_off", "flags") else v)
                      for k, v in s.items()} for s in sections],
        "total_instructions": total_insns,
        "distinct_mnemonics": len(total_counts),
        "mnemonic_counts": dict(total_counts.most_common()),
        "section_mnemonic_counts": {sec: dict(c.most_common())
                                    for sec, c in section_counts.items()},
        "tested_set_mnemonic_counts": dict(tested_counts.most_common()),
        "tested_set_distinct": len(tested_counts),
        "x87_mnemonic_counts": x87,
        "fs_gs_override_sites": fs_gs_sites,
        "indirect_jmps": indirect_jmps,
        "unusual_instructions": unusual,
        "int3_count": int3_count,
        "undecoded_sites": undecoded,
        "functions": fn_records,
    }
    with open(args.out_json, "w") as f:
        json.dump(census, f, indent=1)
    print(f"wrote {args.out_json}")

    # ---- markdown report ----
    lines = []
    lines.append("# Opcode census — default.xbe\n")
    lines.append(f"- Total instructions: **{total_insns}**")
    lines.append(f"- Distinct mnemonics (overall): **{len(total_counts)}**")
    for sec in sorted(section_counts):
        c = section_counts[sec]
        lines.append(f"- {sec}: {sum(c.values())} insns, {len(c)} distinct mnemonics")
    lines.append(f"- Tested 141-set: {sum(tested_counts.values())} insns, "
                 f"{len(tested_counts)} distinct mnemonics")
    lines.append(f"- int3 (padding) instructions inside function ranges: {int3_count}")
    lines.append(f"- FS/GS override sites: {len(fs_gs_sites)}")
    lines.append(f"- Indirect jmps: {len(indirect_jmps)}")
    lines.append(f"- Undecoded sites: {len(undecoded)}\n")

    lines.append("## Mnemonic counts (overall, descending)\n")
    lines.append("| mnemonic | total | .text | tested-141 |")
    lines.append("|---|---|---|---|")
    text_counts = section_counts.get(".text", Counter())
    for m, c in total_counts.most_common():
        lines.append(f"| {m} | {c} | {text_counts.get(m, 0)} | {tested_counts.get(m, 0)} |")

    lines.append("\n## Mnemonics in .text missing from the tested 141-set\n")
    missing = sorted(set(text_counts) - set(tested_counts))
    lines.append(", ".join(missing) if missing else "(none)")

    lines.append("\n## x87 mnemonics\n")
    lines.append("| mnemonic | count |")
    lines.append("|---|---|")
    for m, c in sorted(x87.items(), key=lambda kv: -kv[1]):
        lines.append(f"| {m} | {c} |")

    lines.append("\n## FS/GS override sites\n")
    for s in fs_gs_sites:
        lines.append(f"- {s['va']}: `{s['text']}` ({s['prefix']})")

    lines.append("\n## Indirect jmps\n")
    for j in indirect_jmps:
        lines.append(f"- {j['va']}: `{j['text']}`")

    lines.append("\n## Unusual / privileged instructions (excluding int3)\n")
    for u in unusual:
        lines.append(f"- {u['va']}: `{u['text']}`")

    lines.append("\n## Undecoded sites\n")
    for u in undecoded:
        lines.append(f"- {u['va']} in {u['function']}: skipped {u['skipped']} bytes, "
                     f"first bytes `{u['bytes']}`")

    with open(args.out_report, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {args.out_report}")


if __name__ == "__main__":
    sys.exit(main())
