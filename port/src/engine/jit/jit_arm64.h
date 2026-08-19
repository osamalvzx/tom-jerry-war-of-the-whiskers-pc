// THE BLOCK JIT — M2a's aarch64 INSTRUCTION ENCODER (port/JIT_PLAN.md §5 M2).
//
// aarch64 ONLY, BY CONSTRUCTION. Like everything in this directory, no Windows target
// ever names this file; it is reached only through jit_blocks.cpp, which x86_interp.cpp
// includes inside `#if defined(__aarch64__)`.
//
// A hand-rolled emitter: plain uint32 instruction words, no external assembler, no
// runtime dependency. Every function below returns ONE encoded A64 word and does
// nothing else — the encodings are transcribed from the ARM ARM (C4.1 "A64 instruction
// set encoding") field by field, so a reader can check them against the manual without
// reading any of the JIT's logic. `Asm` adds the two things raw words cannot express:
// LABELS (branch fixups, patched when the label's position is known) and a per-block
// LITERAL POOL (the code cache lives >4 GB from the binary, so every helper address and
// every wide constant is loaded with one `ldr <x>, <literal>` rather than a 4-word
// movz/movk chain or an adrp that cannot reach).
//
// ⚠ ENCODER DISCIPLINE: nothing here knows what x86 is. If a stencil needs a form this
// file does not encode, ADD THE ENCODING HERE (with its manual field layout) rather
// than hand-assembling a word at the call site — an inline magic constant is exactly
// the kind of thing that survives review and then costs a session.
#pragma once
#include <cstdint>
#include <cstring>

namespace tj::engine {
namespace jit {
namespace a64 {

// ---------------------------------------------------------------- registers/conditions
enum : uint32_t { WZR = 31, XZR = 31, SP_REG = 31 };

enum CC : uint32_t {   // named CC, not Cond: `using namespace a64` must not shadow the interpreter's Cond()
    C_EQ = 0, C_NE = 1, C_CS = 2, C_CC = 3, C_MI = 4, C_PL = 5, C_VS = 6, C_VC = 7,
    C_HI = 8, C_LS = 9, C_GE = 10, C_LT = 11, C_GT = 12, C_LE = 13, C_AL = 14,
};
inline CC Invert(CC c) { return (CC)((uint32_t)c ^ 1u); }

// ---------------------------------------------------------------- move wide (C4.1.93)
// sf | opc(2) | 100101 | hw(2) | imm16 | Rd     opc: MOVN=00 MOVZ=10 MOVK=11
inline uint32_t MoveWide(uint32_t opc, bool is64, uint32_t rd, uint32_t imm16, uint32_t hw) {
    return ((uint32_t)is64 << 31) | (opc << 29) | (0x25u << 23) | (hw << 21) |
           ((imm16 & 0xFFFF) << 5) | rd;
}
inline uint32_t MOVZ(uint32_t rd, uint32_t imm16, uint32_t hw = 0, bool is64 = false) {
    return MoveWide(2, is64, rd, imm16, hw);
}
inline uint32_t MOVN(uint32_t rd, uint32_t imm16, uint32_t hw = 0, bool is64 = false) {
    return MoveWide(0, is64, rd, imm16, hw);
}
inline uint32_t MOVK(uint32_t rd, uint32_t imm16, uint32_t hw = 0, bool is64 = false) {
    return MoveWide(3, is64, rd, imm16, hw);
}

// ---------------------------------------------------------------- add/sub immediate
// sf | op | S | 100010 | sh | imm12 | Rn | Rd
inline uint32_t AddSubImm(bool sub, bool setFlags, bool is64, uint32_t rd, uint32_t rn,
                          uint32_t imm12, bool shift12 = false) {
    return ((uint32_t)is64 << 31) | ((uint32_t)sub << 30) | ((uint32_t)setFlags << 29) |
           (0x22u << 23) | ((uint32_t)shift12 << 22) | ((imm12 & 0xFFF) << 10) |
           (rn << 5) | rd;
}
inline uint32_t ADDi (uint32_t rd, uint32_t rn, uint32_t i, bool sh = false) { return AddSubImm(false, false, false, rd, rn, i, sh); }
inline uint32_t SUBi (uint32_t rd, uint32_t rn, uint32_t i, bool sh = false) { return AddSubImm(true,  false, false, rd, rn, i, sh); }
inline uint32_t SUBSi(uint32_t rd, uint32_t rn, uint32_t i, bool sh = false) { return AddSubImm(true,  true,  false, rd, rn, i, sh); }
inline uint32_t ADDSi(uint32_t rd, uint32_t rn, uint32_t i, bool sh = false) { return AddSubImm(false, true,  false, rd, rn, i, sh); }
inline uint32_t CMPi (uint32_t rn, uint32_t i, bool sh = false)              { return AddSubImm(true,  true,  false, WZR, rn, i, sh); }
inline uint32_t ADDXi(uint32_t rd, uint32_t rn, uint32_t i, bool sh = false) { return AddSubImm(false, false, true,  rd, rn, i, sh); }
inline uint32_t SUBXi(uint32_t rd, uint32_t rn, uint32_t i, bool sh = false) { return AddSubImm(true,  false, true,  rd, rn, i, sh); }
inline uint32_t ADDSXi(uint32_t rd, uint32_t rn, uint32_t i, bool sh = false){ return AddSubImm(false, true,  true,  rd, rn, i, sh); }

// ---------------------------------------------------------------- add/sub shifted reg
// sf | op | S | 01011 | shift(2) | 0 | Rm | imm6 | Rn | Rd
inline uint32_t AddSubReg(bool sub, bool setFlags, bool is64, uint32_t rd, uint32_t rn,
                          uint32_t rm, uint32_t shift = 0, uint32_t amount = 0) {
    return ((uint32_t)is64 << 31) | ((uint32_t)sub << 30) | ((uint32_t)setFlags << 29) |
           (0x0Bu << 24) | (shift << 22) | (rm << 16) | ((amount & 0x3F) << 10) |
           (rn << 5) | rd;
}
inline uint32_t ADD (uint32_t rd, uint32_t rn, uint32_t rm, uint32_t lsl = 0) { return AddSubReg(false, false, false, rd, rn, rm, 0, lsl); }
inline uint32_t ADDS(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t lsl = 0) { return AddSubReg(false, true,  false, rd, rn, rm, 0, lsl); }
inline uint32_t SUB (uint32_t rd, uint32_t rn, uint32_t rm, uint32_t lsl = 0) { return AddSubReg(true,  false, false, rd, rn, rm, 0, lsl); }
inline uint32_t SUBS(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t lsl = 0) { return AddSubReg(true,  true,  false, rd, rn, rm, 0, lsl); }
inline uint32_t CMPr(uint32_t rn, uint32_t rm)                                { return AddSubReg(true,  true,  false, WZR, rn, rm, 0, 0); }

// ---------------------------------------------------------------- adc/sbc (with carry)
// sf | op | S | 11010000 | Rm | 000000 | Rn | Rd
inline uint32_t AddSubCarry(bool sub, bool setFlags, uint32_t rd, uint32_t rn, uint32_t rm) {
    return ((uint32_t)sub << 30) | ((uint32_t)setFlags << 29) | (0xD0u << 21) |
           (rm << 16) | (rn << 5) | rd;
}
inline uint32_t ADCS(uint32_t rd, uint32_t rn, uint32_t rm) { return AddSubCarry(false, true, rd, rn, rm); }
inline uint32_t SBCS(uint32_t rd, uint32_t rn, uint32_t rm) { return AddSubCarry(true,  true, rd, rn, rm); }

// ---------------------------------------------------------------- logical shifted reg
// sf | opc(2) | 01010 | shift(2) | N | Rm | imm6 | Rn | Rd
// opc: AND=00 ORR=01 EOR=10 ANDS=11 ;  N=1 inverts Rm (BIC/ORN/EON/BICS)
inline uint32_t LogicReg(uint32_t opc, bool n, bool is64, uint32_t rd, uint32_t rn,
                         uint32_t rm, uint32_t shift = 0, uint32_t amount = 0) {
    return ((uint32_t)is64 << 31) | (opc << 29) | (0x0Au << 24) | (shift << 22) |
           ((uint32_t)n << 21) | (rm << 16) | ((amount & 0x3F) << 10) | (rn << 5) | rd;
}
inline uint32_t AND (uint32_t rd, uint32_t rn, uint32_t rm)                   { return LogicReg(0, false, false, rd, rn, rm); }
inline uint32_t ANDS(uint32_t rd, uint32_t rn, uint32_t rm)                   { return LogicReg(3, false, false, rd, rn, rm); }
inline uint32_t ORR (uint32_t rd, uint32_t rn, uint32_t rm, uint32_t lsl = 0) { return LogicReg(1, false, false, rd, rn, rm, 0, lsl); }
inline uint32_t EOR (uint32_t rd, uint32_t rn, uint32_t rm, uint32_t lsl = 0) { return LogicReg(2, false, false, rd, rn, rm, 0, lsl); }
inline uint32_t EORlsr(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t sh)    { return LogicReg(2, false, false, rd, rn, rm, 1, sh); }
inline uint32_t MOV (uint32_t rd, uint32_t rm)                                { return ORR(rd, WZR, rm); }
inline uint32_t MOVX(uint32_t rd, uint32_t rm)                                { return LogicReg(1, false, true, rd, XZR, rm); }

// ---------------------------------------------------------------- bitfield (UBFM/SBFM/BFM)
// sf | opc(2) | 100110 | N | immr(6) | imms(6) | Rn | Rd     SBFM=00 BFM=01 UBFM=10
inline uint32_t Bitfield(uint32_t opc, bool is64, uint32_t rd, uint32_t rn,
                         uint32_t immr, uint32_t imms) {
    return ((uint32_t)is64 << 31) | (opc << 29) | (0x26u << 23) |
           ((uint32_t)is64 << 22) | ((immr & 0x3F) << 16) | ((imms & 0x3F) << 10) |
           (rn << 5) | rd;
}
// 32-bit aliases (ARM ARM C6.2 alias tables)
inline uint32_t UBFX(uint32_t rd, uint32_t rn, uint32_t lsb, uint32_t width) { return Bitfield(2, false, rd, rn, lsb, lsb + width - 1); }
inline uint32_t SBFX(uint32_t rd, uint32_t rn, uint32_t lsb, uint32_t width) { return Bitfield(0, false, rd, rn, lsb, lsb + width - 1); }
inline uint32_t BFI (uint32_t rd, uint32_t rn, uint32_t lsb, uint32_t width) { return Bitfield(1, false, rd, rn, (32 - lsb) & 31, width - 1); }
inline uint32_t UXTB(uint32_t rd, uint32_t rn)                { return Bitfield(2, false, rd, rn, 0, 7); }
inline uint32_t UXTH(uint32_t rd, uint32_t rn)                { return Bitfield(2, false, rd, rn, 0, 15); }
inline uint32_t SXTB(uint32_t rd, uint32_t rn)                { return Bitfield(0, false, rd, rn, 0, 7); }
inline uint32_t LSRi(uint32_t rd, uint32_t rn, uint32_t sh)   { return Bitfield(2, false, rd, rn, sh, 31); }
inline uint32_t LSLi(uint32_t rd, uint32_t rn, uint32_t sh)   { return Bitfield(2, false, rd, rn, (32 - sh) & 31, 31 - sh); }

// ---------------------------------------------------------------- conditional select
// sf | op | S | 11010100 | Rm | cond | 0 1 | Rn | Rd     (CSINC)
inline uint32_t CSINC(uint32_t rd, uint32_t rn, uint32_t rm, CC c) {
    return (0xD4u << 21) | (rm << 16) | ((uint32_t)c << 12) | (1u << 10) | (rn << 5) | rd;
}
// CSET Rd, cond  ==  CSINC Rd, WZR, WZR, invert(cond)
inline uint32_t CSET(uint32_t rd, CC c) { return CSINC(rd, WZR, WZR, Invert(c)); }

// ---------------------------------------------------------------- load/store immediate
// size(2) | 111 | V=0 | 01 | opc(2) | imm12 | Rn | Rt        (unsigned scaled offset)
// size: 00=B 01=H 10=W 11=X   opc: 00=store 01=load(zero-ext) 11=LDRS->32-bit
inline uint32_t LdStImm(uint32_t size, uint32_t opc, uint32_t rt, uint32_t rn, uint32_t scaledImm) {
    return (size << 30) | (0x7u << 27) | (1u << 24) | (opc << 22) |
           ((scaledImm & 0xFFF) << 10) | (rn << 5) | rt;
}
inline uint32_t STRw (uint32_t rt, uint32_t rn, uint32_t byteOff) { return LdStImm(2, 0, rt, rn, byteOff >> 2); }
inline uint32_t LDRw (uint32_t rt, uint32_t rn, uint32_t byteOff) { return LdStImm(2, 1, rt, rn, byteOff >> 2); }
inline uint32_t STRx (uint32_t rt, uint32_t rn, uint32_t byteOff) { return LdStImm(3, 0, rt, rn, byteOff >> 3); }
inline uint32_t LDRx (uint32_t rt, uint32_t rn, uint32_t byteOff) { return LdStImm(3, 1, rt, rn, byteOff >> 3); }
inline uint32_t STRB (uint32_t rt, uint32_t rn, uint32_t byteOff) { return LdStImm(0, 0, rt, rn, byteOff); }
inline uint32_t LDRB (uint32_t rt, uint32_t rn, uint32_t byteOff) { return LdStImm(0, 1, rt, rn, byteOff); }
inline uint32_t STRH (uint32_t rt, uint32_t rn, uint32_t byteOff) { return LdStImm(1, 0, rt, rn, byteOff >> 1); }
inline uint32_t LDRH (uint32_t rt, uint32_t rn, uint32_t byteOff) { return LdStImm(1, 1, rt, rn, byteOff >> 1); }
inline uint32_t LDRSBw(uint32_t rt, uint32_t rn, uint32_t byteOff){ return LdStImm(0, 3, rt, rn, byteOff); }

// ---------------------------------------------------------------- load/store reg offset
// size | 111 | V=0 | 00 | opc | 1 | Rm | option(3) | S | 10 | Rn | Rt
// option: 010=UXTW 011=LSL 110=SXTW 111=SXTX
inline uint32_t LdStReg(uint32_t size, uint32_t opc, uint32_t rt, uint32_t rn,
                        uint32_t rm, uint32_t option, bool scaled) {
    return (size << 30) | (0x7u << 27) | (opc << 22) | (1u << 21) | (rm << 16) |
           (option << 13) | ((uint32_t)scaled << 12) | (2u << 10) | (rn << 5) | rt;
}
inline uint32_t LDRBreg(uint32_t rt, uint32_t rn, uint32_t rm) { return LdStReg(0, 1, rt, rn, rm, 2, false); }  // uxtw

// ---------------------------------------------------------------- load/store pair
// opc(2) | 101 | V=0 | mode(3) | L | imm7 | Rt2 | Rn | Rt
// opc: 00=32-bit 10=64-bit ; mode: 001=post 010=signed 011=pre
inline uint32_t LdStPair(uint32_t opc, uint32_t mode, bool load, uint32_t rt, uint32_t rt2,
                         uint32_t rn, int32_t scaledImm) {
    return (opc << 30) | (0x5u << 27) | (mode << 23) | ((uint32_t)load << 22) |
           (((uint32_t)scaledImm & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt;
}
inline uint32_t STPw(uint32_t t1, uint32_t t2, uint32_t rn, int32_t byteOff)  { return LdStPair(0, 2, false, t1, t2, rn, byteOff / 4); }
inline uint32_t LDPw(uint32_t t1, uint32_t t2, uint32_t rn, int32_t byteOff)  { return LdStPair(0, 2, true,  t1, t2, rn, byteOff / 4); }
inline uint32_t STPx(uint32_t t1, uint32_t t2, uint32_t rn, int32_t byteOff)  { return LdStPair(2, 2, false, t1, t2, rn, byteOff / 8); }
inline uint32_t LDPx(uint32_t t1, uint32_t t2, uint32_t rn, int32_t byteOff)  { return LdStPair(2, 2, true,  t1, t2, rn, byteOff / 8); }
inline uint32_t STPxPre (uint32_t t1, uint32_t t2, uint32_t rn, int32_t byteOff) { return LdStPair(2, 3, false, t1, t2, rn, byteOff / 8); }
inline uint32_t LDPxPost(uint32_t t1, uint32_t t2, uint32_t rn, int32_t byteOff) { return LdStPair(2, 1, true,  t1, t2, rn, byteOff / 8); }

// ---------------------------------------------------------------- branches
inline uint32_t B (int32_t words)                 { return (0x5u << 26) | ((uint32_t)words & 0x03FFFFFFu); }
inline uint32_t BCOND(CC c, int32_t words)      { return (0x54u << 24) | ((((uint32_t)words) & 0x7FFFFu) << 5) | (uint32_t)c; }
inline uint32_t CBZ (uint32_t rt, int32_t words, bool is64 = false)  { return ((uint32_t)is64 << 31) | (0x1Au << 25) | ((((uint32_t)words) & 0x7FFFFu) << 5) | rt; }
inline uint32_t CBNZ(uint32_t rt, int32_t words, bool is64 = false)  { return ((uint32_t)is64 << 31) | (0x1Au << 25) | (1u << 24) | ((((uint32_t)words) & 0x7FFFFu) << 5) | rt; }
inline uint32_t BR (uint32_t rn)  { return 0xD61F0000u | (rn << 5); }
inline uint32_t BLR(uint32_t rn)  { return 0xD63F0000u | (rn << 5); }
inline uint32_t RET(uint32_t rn = 30) { return 0xD65F0000u | (rn << 5); }

// ---------------------------------------------------------------- pc-relative address
// 0 | immlo(2) | 10000 | immhi(19) | Rd     (ADR: signed 21-bit BYTE offset)
inline uint32_t ADR(uint32_t rd, int32_t byteOff) {
    uint32_t imm = (uint32_t)byteOff & 0x1FFFFFu;
    return ((imm & 3u) << 29) | (0x10u << 24) | (((imm >> 2) & 0x7FFFFu) << 5) | rd;
}

// ---------------------------------------------------------------- literal load
// opc(2) | 011 | V=0 | 00 | imm19 | Rt      opc: 00=32-bit 01=64-bit
inline uint32_t LDRlitX(uint32_t rt, int32_t words) { return (1u << 30) | (0x18u << 24) | ((((uint32_t)words) & 0x7FFFFu) << 5) | rt; }
inline uint32_t LDRlitW(uint32_t rt, int32_t words) { return (0x18u << 24) | ((((uint32_t)words) & 0x7FFFFu) << 5) | rt; }

// ---------------------------------------------------------------- hints
inline uint32_t NOP()   { return 0xD503201Fu; }
inline uint32_t BTI_C() { return 0xD503241Fu; }   // HINT #34 — risk #10, a NOP without PROT_BTI

// =====================================================================================
// Asm — the emission buffer: words + labels + a literal pool.
//
// Positions are WORD indices into `w`. Labels are integer ids; a branch to an
// unresolved label records a fixup that Finish() patches. Literals are 8-byte entries
// appended after the code (LDR-literal reaches +-1 MB, so a per-block pool always
// resolves); a literal may be SELF-RELATIVE, meaning its value is a byte offset within
// the block's own emitted image and is completed only when the image's final address in
// the code cache is known (that is how a block carries its own SMC generation list
// without pointing at anything the block cache might free).
// =====================================================================================
struct Asm {
    static constexpr uint32_t kMaxWords  = 8192;
    static constexpr uint32_t kMaxLabels = 512;
    static constexpr uint32_t kMaxFix    = 1024;
    static constexpr uint32_t kMaxLit    = 192;
    static constexpr uint32_t kMaxData   = 64;    // extra trailing data words (gen lists)

    enum FixKind : uint8_t { FK_B26, FK_BCOND19, FK_CB19, FK_ADR21 };

    uint32_t w[kMaxWords];
    uint32_t n = 0;
    bool     bad = false;                 // any overflow: the caller must decline the block

    int32_t  lblPos[kMaxLabels];
    uint32_t nLbl = 0;

    struct Fix { uint32_t at; uint16_t lbl; uint8_t kind; };
    Fix      fix[kMaxFix];
    uint32_t nFix = 0;

    uint64_t lit[kMaxLit];
    bool     litSelf[kMaxLit];
    uint32_t nLit = 0;
    struct LFix { uint32_t at; uint16_t idx; bool wide; };
    LFix     lfix[kMaxFix];
    uint32_t nLFix = 0;

    uint32_t data[kMaxData];
    uint32_t nData = 0;
    uint32_t dataByteOff = 0;             // filled by Finish()

    void Reset() {
        n = 0; nLbl = 0; nFix = 0; nLit = 0; nLFix = 0; nData = 0; bad = false;
        dataByteOff = 0;
    }
    void Put(uint32_t insn) { if (n < kMaxWords) w[n++] = insn; else bad = true; }

    uint32_t NewLabel() {
        if (nLbl >= kMaxLabels) { bad = true; return 0; }
        lblPos[nLbl] = -1;
        return nLbl++;
    }
    void Bind(uint32_t lbl) { if (lbl < nLbl) lblPos[lbl] = (int32_t)n; }
    uint32_t Here() const { return n; }

    void AddFix(uint32_t lbl, FixKind k) {
        if (nFix >= kMaxFix) { bad = true; return; }
        fix[nFix++] = { n, (uint16_t)lbl, (uint8_t)k };
    }
    void Br(uint32_t lbl)              { AddFix(lbl, FK_B26);     Put(B(0)); }
    void Bc(CC c, uint32_t lbl)      { AddFix(lbl, FK_BCOND19); Put(BCOND(c, 0)); }
    void Cbz(uint32_t rt, uint32_t l)  { AddFix(l, FK_CB19);      Put(CBZ(rt, 0)); }
    void Cbnz(uint32_t rt, uint32_t l) { AddFix(l, FK_CB19);      Put(CBNZ(rt, 0)); }
    void Adr(uint32_t rd, uint32_t l)  { AddFix(l, FK_ADR21);     Put(ADR(rd, 0)); }

    // Deduplicated literal (host addresses and wide constants).
    uint32_t Lit(uint64_t v, bool self = false) {
        for (uint32_t i = 0; i < nLit; ++i)
            if (lit[i] == v && litSelf[i] == self) return i;
        if (nLit >= kMaxLit) { bad = true; return 0; }
        lit[nLit] = v; litSelf[nLit] = self;
        return nLit++;
    }
    void LdLitX(uint32_t rt, uint64_t v, bool self = false) {
        uint32_t idx = Lit(v, self);
        if (nLFix >= kMaxFix) { bad = true; return; }
        lfix[nLFix++] = { n, (uint16_t)idx, true };
        Put(LDRlitX(rt, 0));
    }
    void LdLitW(uint32_t rt, uint32_t v) {                 // reads the literal's low word
        uint32_t idx = Lit((uint64_t)v, false);
        if (nLFix >= kMaxFix) { bad = true; return; }
        lfix[nLFix++] = { n, (uint16_t)idx, false };
        Put(LDRlitW(rt, 0));
    }
    // Trailing per-block DATA words (the SMC generation list a store's slow path reads).
    // Returns the word slot; reference it with LdLitX(rd, slot * 4, /*self=*/true) — the
    // literal's value is completed to an absolute address by Finish()+PatchSelf().
    uint32_t PutData(uint32_t v) {                          // returns the word slot index
        if (nData >= kMaxData) { bad = true; return 0; }
        data[nData] = v;
        return nData++;
    }

    // Materialize a 32-bit constant in the fewest words (movz / movn / movz+movk).
    void MovImm32(uint32_t rd, uint32_t v) {
        if ((v & 0xFFFF0000u) == 0)            { Put(MOVZ(rd, v & 0xFFFF, 0)); return; }
        if ((v & 0x0000FFFFu) == 0)            { Put(MOVZ(rd, v >> 16, 1));    return; }
        if (((~v) & 0xFFFF0000u) == 0)         { Put(MOVN(rd, (~v) & 0xFFFF, 0)); return; }
        Put(MOVZ(rd, v & 0xFFFF, 0));
        Put(MOVK(rd, v >> 16, 1));
    }
    // rd = rn + (int32)imm, folding into one add/sub when the immediate encodes.
    void AddImm32(uint32_t rd, uint32_t rn, int32_t imm, uint32_t scratch) {
        if (imm == 0) { if (rd != rn) Put(MOV(rd, rn)); return; }
        uint32_t mag = (uint32_t)(imm < 0 ? -(int64_t)imm : (int64_t)imm);
        if (mag <= 0xFFF)                       { Put(imm < 0 ? SUBi(rd, rn, mag) : ADDi(rd, rn, mag)); return; }
        if ((mag & 0xFFF) == 0 && (mag >> 12) <= 0xFFF) {
            Put(imm < 0 ? SUBi(rd, rn, mag >> 12, true) : ADDi(rd, rn, mag >> 12, true));
            return;
        }
        MovImm32(scratch, (uint32_t)imm);
        Put(ADD(rd, rn, scratch));
    }

    // Resolve labels + append the literal pool and trailing data. `imageWords` becomes
    // the block's full size; the caller copies w[0..imageWords) to the code cache and
    // then calls PatchSelf() with the final address.
    bool Finish() {
        for (uint32_t i = 0; i < nFix; ++i) {
            const Fix& f = fix[i];
            if (f.lbl >= nLbl || lblPos[f.lbl] < 0) { bad = true; continue; }
            int32_t d = lblPos[f.lbl] - (int32_t)f.at;
            switch (f.kind) {
            case FK_B26:     if (d < -(1 << 25) || d >= (1 << 25)) bad = true;
                             w[f.at] = (w[f.at] & 0xFC000000u) | ((uint32_t)d & 0x03FFFFFFu); break;
            case FK_BCOND19:
            case FK_CB19:    if (d < -(1 << 18) || d >= (1 << 18)) bad = true;
                             w[f.at] = (w[f.at] & 0xFF00001Fu) | (((uint32_t)d & 0x7FFFFu) << 5); break;
            case FK_ADR21: { int32_t db = d * 4;
                             if (db < -(1 << 20) || db >= (1 << 20)) bad = true;
                             uint32_t im = (uint32_t)db & 0x1FFFFFu;
                             w[f.at] = (w[f.at] & 0x9F00001Fu) | ((im & 3u) << 29) |
                                       (((im >> 2) & 0x7FFFFu) << 5); break; }
            }
        }
        if (n & 1) Put(NOP());                       // 8-byte align the pool
        uint32_t litBase = n;
        for (uint32_t i = 0; i < nLit; ++i) {
            uint64_t v = lit[i];
            Put((uint32_t)(v & 0xFFFFFFFFu));
            Put((uint32_t)(v >> 32));
        }
        for (uint32_t i = 0; i < nLFix; ++i) {
            const LFix& f = lfix[i];
            int32_t d = (int32_t)(litBase + f.idx * 2) - (int32_t)f.at;
            if (d < -(1 << 18) || d >= (1 << 18)) bad = true;
            w[f.at] = (w[f.at] & 0xFF00001Fu) | (((uint32_t)d & 0x7FFFFu) << 5);
        }
        litWordBase = litBase;
        dataByteOff = n * 4;
        for (uint32_t i = 0; i < nData; ++i) Put(data[i]);
        // Self-relative literals were recorded as DATA-area byte offsets; now that the
        // data area's position within the image is known, rebase them to image offsets.
        // Their pool slots hold placeholder bytes until PatchSelf() writes the address.
        for (uint32_t i = 0; i < nLit; ++i)
            if (litSelf[i]) lit[i] += dataByteOff;
        return !bad;
    }
    uint32_t litWordBase = 0;

    // Complete SELF-RELATIVE literals once the image's final address is known. `img`
    // points at the copy inside the code cache; the literal's stored value is a byte
    // offset within the image.
    void PatchSelf(uint32_t* img) const {
        for (uint32_t i = 0; i < nLit; ++i) {
            if (!litSelf[i]) continue;
            uint64_t abs = (uint64_t)(uintptr_t)img + lit[i];
            memcpy(img + litWordBase + i * 2, &abs, 8);
        }
    }
};

} // namespace a64
} // namespace jit
} // namespace tj::engine
