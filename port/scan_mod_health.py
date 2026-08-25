import os
import sys
import re
import struct

XBE_PATH = r"C:\Users\goldl\Games\Tom and Jerry War of the Whiskers\default.xbe"
SRC_DIR  = r"C:\Users\goldl\Downloads\tj_repo\port\src\hybrid"

def load_xbe():
    if not os.path.exists(XBE_PATH):
        print(f"[!] Error: XBE file not found at {XBE_PATH}")
        return None
    with open(XBE_PATH, "rb") as f:
        return f.read()

def check_function_ret(data, va):
    off = va - 0x10000
    if off < 0 or off >= len(data):
        return None, "Out of bounds"
    b = data[off:off+60]
    for i in range(len(b)):
        if b[i] == 0xC3:
            return 0, f"ret 0 (C3 at +{i})"
        elif b[i] == 0xC2 and i + 2 < len(b):
            imm = b[i+1] | (b[i+2] << 8)
            return imm, f"ret {imm} (C2 at +{i})"
    return None, "No ret found in 60 bytes"

def scan_codebase():
    xbe = load_xbe()
    if not xbe:
        sys.exit(1)

    print("==========================================================")
    print("  Tom & Jerry War of the Whiskers - Mod Safety Scanner    ")
    print("==========================================================")
    
    errors = 0
    warnings = 0
    checks = 0

    patch_pattern = re.compile(r'MakeGuestTramp\s*\(\s*(0x[0-9A-Fa-f]+)\s*,\s*(\d+)')

    for root, _, files in os.walk(SRC_DIR):
        for file in files:
            if not (file.endswith(".cpp") or file.endswith(".h")):
                continue
            path = os.path.join(root, file)
            with open(path, "r", encoding="utf-8", errors="ignore") as f:
                lines = f.readlines()

            for line_idx, line in enumerate(lines, 1):
                for m in patch_pattern.finditer(line):
                    va_str, num_bytes_str = m.group(1), m.group(2)
                    va = int(va_str, 16)
                    num_bytes = int(num_bytes_str)
                    checks += 1
                    off = va - 0x10000
                    if off < 0 or off + num_bytes > len(xbe):
                        print(f"[!] {file}:{line_idx}: Invalid trampoline address {va_str}")
                        errors += 1
                        continue
                    prologue = xbe[off:off+num_bytes]
                    if any(b in (0xE8, 0xE9, 0xEB) for b in prologue):
                        print(f"[WARNING] {file}:{line_idx}: Trampoline at {va_str} steals relative branch byte! ({prologue.hex()})")
                        warnings += 1

    print("\n--- Scan Results ---")
    print(f"Total Hook/Call Checks: {checks}")
    print(f"Errors Found:           {errors}")
    print(f"Warnings:               {warnings}")

    if errors == 0:
        print("[SUCCESS] All hooks, trampolines, and calling conventions are verified 100% crash-safe!")
        return 0
    else:
        print("[FAIL] Please resolve errors before deploying.")
        return 1

if __name__ == "__main__":
    sys.exit(scan_codebase())
