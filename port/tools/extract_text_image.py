"""Dump the .text section bytes + its virtual address, so the differential test can
map the original machine code at its true VA and call it alongside the ported C."""
import struct, os
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XBE = os.path.join(ROOT, "extracted", "default.xbe")
OUT = os.path.join(ROOT, "port", "src", "game", "generated")
data = open(XBE, "rb").read()
base_addr, = struct.unpack_from("<I", data, 0x104)
num_sections, section_hdr_addr = struct.unpack_from("<II", data, 0x11C)
def hva(a): return a - base_addr
s = hva(section_hdr_addr)
for i in range(num_sections):
    flags, vaddr, vsize, raw_off, raw_size, name_addr = struct.unpack_from("<6I", data, s + i*0x38)
    name = data[hva(name_addr):hva(name_addr)+32].split(b"\0")[0].decode("latin-1")
    if name == ".text":
        open(os.path.join(OUT, "text_image.bin"), "wb").write(data[raw_off:raw_off+raw_size])
        with open(os.path.join(OUT, "text_image.h"), "w") as f:
            f.write("// GENERATED. .text machine code, for differential testing.\n#pragma once\n#include <cstdint>\n")
            f.write("namespace tj::game {\n")
            f.write(f"constexpr uint32_t kTextBase = 0x{vaddr:08X}u;\n")
            f.write(f"constexpr uint32_t kTextSize = 0x{raw_size:08X}u;\n")
            f.write("extern uint8_t* g_textImage;\n")
            f.write("bool LoadTextImage(const char* binPath);\n")
            f.write("} // namespace tj::game\n")
        print(f".text vaddr=0x{vaddr:X} raw_size=0x{raw_size:X} ({raw_size/1024:.0f} KB) written")
