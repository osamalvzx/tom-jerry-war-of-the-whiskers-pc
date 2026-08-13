// XMF object injector -- C++ port of port/tools/xmf_inject.py. See that file for the full
// derivation; the constraints the engine loader (FUN_00079EE0) actually enforces, and which
// dictate every placement decision here, are:
//
//   * IXMF is a bare instance array; RXMF is a bare material array -- both append cleanly.
//   * OXMF ends with a 12-byte VB-RESOURCE ARRAY whose element count the loader derives as
//     (oxmfSize - object[0].descArray[0].+4) / 12, so new bulk data goes BEFORE it, never after.
//   * TXMF is records | headers | palettes, and the loader locates the palette region with
//     36*n - 20*dup where `dup` counts records repeating the PREVIOUS record's header. Every
//     injected record therefore gets its OWN header so `dup` cannot move.
//   * FXMF is bulk GPU bytes; every resource's Data(+4) is FXMF-tag-relative -- append.
//
// THE BUG THAT ALMOST SHIPPED, and why validate() exists: the new object record is appended to
// the END of the object array, so pointers INTO that array (every instance's +0x44, every
// part's object back-pointer) must NOT shift. Shifting them made instance i resolve to object
// i+1, and the mesh reader in re/item_render/ never reads +0x44 -- it assumes instance index ==
// object index -- so it validated the broken file happily.
#include "setup/xmf_inject.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace tj::setup {

namespace {

const uint32_t INST = 0x54;   // IXMF instance record
const uint32_t OBJ  = 0x4C;   // OXMF object record
const uint32_t PART = 0x54;   // OXMF mesh part
const uint32_t DESC = 0x50;   // OXMF vertex-buffer descriptor
const uint32_t RES  = 0x0C;   // bare D3D resource header (VB / index buffer / palette)
const uint32_t MAT  = 0x50;   // RXMF material
const uint32_t TREC = 0x10;   // TXMF table record
const uint32_t THDR = 0x14;   // D3DPixelContainer texture header
const uint32_t NONE = 0xFFFFFFFFu;

// Engine per-token component sizes (DAT_00179CE0, used by the stride calc FUN_0007C270).
const int TOK[16] = { 0, 12, 8, 12, 12, 4, 4, 4, 4, 8, 12, 0, 4, 8, 4, 6 };
const uint32_t TOK_END = 0xB;

typedef std::vector<uint8_t> Bytes;

inline uint32_t U32(const Bytes& b, size_t o) { uint32_t v; memcpy(&v, b.data() + o, 4); return v; }
inline int32_t  I32(const Bytes& b, size_t o) { int32_t v;  memcpy(&v, b.data() + o, 4); return v; }
inline uint16_t U16(const Bytes& b, size_t o) { uint16_t v; memcpy(&v, b.data() + o, 2); return v; }
inline void W32(Bytes& b, size_t o, uint32_t v) { memcpy(b.data() + o, &v, 4); }
inline void W16(Bytes& b, size_t o, uint16_t v) { memcpy(b.data() + o, &v, 2); }
inline uint32_t Align(uint32_t n, uint32_t a) { return (n + a - 1) & ~(a - 1); }

void Append(Bytes& dst, const uint8_t* p, size_t n) { dst.insert(dst.end(), p, p + n); }
void Append(Bytes& dst, const Bytes& src) { dst.insert(dst.end(), src.begin(), src.end()); }
void AppendRange(Bytes& dst, const Bytes& src, size_t from, size_t to) {
    dst.insert(dst.end(), src.begin() + from, src.begin() + to);
}
Bytes Slice(const Bytes& b, size_t from, size_t n) {
    return Bytes(b.begin() + from, b.begin() + from + n);
}

bool ReadWholeFile(const wchar_t* path, Bytes& out, std::string& err) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot open file"; return false; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > (LONGLONG)0x7FFFFFFF) {
        CloseHandle(h); err = "file is missing or too large"; return false;
    }
    out.resize((size_t)sz.QuadPart);
    size_t done = 0;
    while (done < out.size()) {
        DWORD got = 0;
        DWORD want = (DWORD)std::min<size_t>(out.size() - done, 1u << 20);
        if (!ReadFile(h, out.data() + done, want, &got, nullptr) || got == 0) {
            CloseHandle(h); err = "file could not be read"; return false;
        }
        done += got;
    }
    CloseHandle(h);
    return true;
}

bool WriteWholeFile(const wchar_t* path, const Bytes& data, std::string& err) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot write file"; return false; }
    size_t done = 0;
    while (done < data.size()) {
        DWORD put = 0;
        DWORD want = (DWORD)std::min<size_t>(data.size() - done, 1u << 20);
        if (!WriteFile(h, data.data() + done, want, &put, nullptr) || put != want) {
            CloseHandle(h); err = "file could not be written"; return false;
        }
        done += put;
    }
    CloseHandle(h);
    return true;
}

// ---------------------------------------------------------------- container

struct Xmf {
    Bytes b;
    uint32_t ixmfCount = 0, objCount = 0, rxmfCount = 0, txmfCount = 0, flag = 0;
    uint32_t idsfSize = 0, ixmfSize = 0, oxmfSize = 0, rxmfSize = 0, txmfSize = 0, fxmfSize = 0;
    uint32_t ixmf = 0, oxmf = 0, rxmf = 0, txmf = 0, idsf = 0, fxmf = 0;

    bool Parse(std::string& err);
    bool CheckLayout(std::string& err) const;

    uint32_t InstOff(uint32_t i) const { return ixmf + 4 + i * INST; }
    uint32_t ObjOff(uint32_t i)  const { return oxmf + 4 + i * OBJ; }
    uint32_t TrecOff(uint32_t i) const { return txmf + 4 + i * TREC; }

    uint32_t TxmfDup() const {
        uint32_t dup = 0; bool have = false; uint32_t prev = 0;
        for (uint32_t t = 0; t < txmfCount; ++t) {
            uint32_t h = U32(b, TrecOff(t) + 4);
            if (have && h == prev) ++dup;
            prev = h; have = true;
        }
        return dup;
    }
    uint32_t HdrStart() const { return 4 + txmfCount * TREC; }
    uint32_t PalStart() const { return HdrStart() + (txmfCount - TxmfDup()) * THDR; }

    // OXMF-relative start of the VB-resource array, derived exactly the way the loader does.
    bool VbResStart(uint32_t& out, std::string& err) const {
        if (!objCount) { err = "container has no objects"; return false; }
        uint32_t descArr = U32(b, oxmf + 4 + 0x10);
        if (descArr + 8 > oxmfSize) { err = "object 0 has no usable declaration array"; return false; }
        out = U32(b, oxmf + descArr + 4);
        return true;
    }

    int Find(const char* name) const {
        for (uint32_t i = 0; i < ixmfCount; ++i)
            if (memcmp(b.data() + InstOff(i) + 0x48, name, 4) == 0) return (int)i;
        return -1;
    }

    // Every FXMF-relative Data offset referenced anywhere, mapped to the distance to the next
    // one. Exact, needs no per-format maths, and preserves the exporter's inter-blob padding.
    bool FxmfExtents(std::map<uint32_t, uint32_t>& out, std::string& err) const {
        uint32_t vbs;
        if (!VbResStart(vbs, err)) return false;
        std::set<uint32_t> offs;
        for (uint32_t o = vbs; o + RES <= oxmfSize; o += RES)
            offs.insert(U32(b, oxmf + o + 4));
        for (uint32_t t = 0; t < txmfCount; ++t) {
            uint32_t rec = TrecOff(t);
            for (int k = 0; k < 2; ++k) {
                uint32_t rel = U32(b, rec + 4 + k * 4);
                if (rel != NONE) offs.insert(U32(b, txmf + rel + 4));
            }
        }
        std::vector<uint32_t> ord(offs.begin(), offs.end());
        for (size_t i = 0; i < ord.size(); ++i)
            out[ord[i]] = (i + 1 < ord.size() ? ord[i + 1] : fxmfSize) - ord[i];
        return true;
    }
};

bool Xmf::Parse(std::string& err) {
    if (b.size() < 0x30 || memcmp(b.data(), "XMF", 3) != 0) { err = "not an XMF container"; return false; }
    int ver = b[3] - 0x30;
    if (ver != 1) { err = "only XMF v1 is supported"; return false; }
    ixmfCount = U32(b, 0x04); objCount = U32(b, 0x08);
    rxmfCount = U32(b, 0x0C); txmfCount = U32(b, 0x10);
    flag = U32(b, 0x14);
    idsfSize = (flag == 0xabcd) ? 0 : U32(b, 0x18);
    ixmfSize = U32(b, 0x1C); oxmfSize = U32(b, 0x20); rxmfSize = U32(b, 0x24);
    txmfSize = U32(b, 0x28); fxmfSize = U32(b, 0x2C);

    ixmf = 0x30;
    oxmf = ixmf + ixmfSize;
    rxmf = oxmf + oxmfSize;
    txmf = rxmf + rxmfSize;
    idsf = txmf + txmfSize;
    fxmf = idsf + idsfSize;

    struct { const char* tag; uint32_t off; } tags[] = {
        { "IXMF", ixmf }, { "OXMF", oxmf }, { "RXMF", rxmf }, { "TXMF", txmf }, { "FXMF", fxmf } };
    for (auto& t : tags) {
        if (t.off + 4 > b.size() || memcmp(b.data() + t.off, t.tag, 4) != 0) {
            err = std::string("expected ") + t.tag + " section tag"; return false;
        }
    }
    if (idsfSize && memcmp(b.data() + idsf, "IDSF", 4) != 0) { err = "expected IDSF"; return false; }
    if (fxmf + fxmfSize != b.size()) { err = "FXMF does not end at the end of the file"; return false; }
    return CheckLayout(err);
}

bool Xmf::CheckLayout(std::string& err) const {
    if (ixmfSize != 4 + ixmfCount * INST) { err = "IXMF is not a bare instance array"; return false; }
    if (rxmfSize != 4 + rxmfCount * MAT)  { err = "RXMF is not a bare material array"; return false; }
    uint32_t pal = PalStart();
    if (pal > txmfSize || (txmfSize - pal) % RES) {
        err = "TXMF palette region is not a whole number of resources"; return false;
    }
    uint32_t vbs;
    if (!VbResStart(vbs, err)) return false;
    if (oxmfSize <= vbs || (oxmfSize - vbs) % RES) {
        err = "OXMF VB-resource region is not a multiple of 12"; return false;
    }
    return true;
}

bool DeclStride(const Bytes& desc, uint32_t& out, std::string& err) {
    uint32_t stride = 0;
    for (int i = 0; i < 16; ++i) {
        uint32_t tok = U32(desc, 0x10 + i * 4);
        if (tok == TOK_END) break;
        stride += (tok < 16) ? TOK[tok] : 0;
    }
    if (!stride) { err = "vertex declaration has zero stride"; return false; }
    out = stride;
    return true;
}

// ---------------------------------------------------------------- extraction

struct TexRec {
    uint32_t srel;
    Bytes    rec;
    bool     hasHdr = false;
    Bytes    hdr;
    uint32_t hdrBlobKey = 0;
    bool     hasPal = false;
    uint32_t palSrel = 0;
};

struct Payload {
    const Xmf* src = nullptr;
    Bytes inst, obj, ibRes, idx;
    std::vector<Bytes> parts, descs;
    std::map<uint32_t, int> descIndex;          // source desc offset -> index
    std::vector<uint32_t> strides;
    std::vector<Bytes> vbdata;
    // Insertion-ordered, because FXMF placement order must match the Python exactly.
    std::vector<uint32_t> blobOrder;
    std::map<uint32_t, Bytes> blobs;
    std::vector<std::pair<uint32_t, Bytes>> mats;
    std::vector<TexRec> trecs;
    struct Pal { uint32_t srel; Bytes res; uint32_t blobKey; };
    std::vector<Pal> pals;

    bool Build(const Xmf& s, const char* name, std::string& err);
    bool TakeBlob(uint32_t resOff, const std::map<uint32_t, uint32_t>& ext,
                  uint32_t& outKey, std::string& err);
};

bool Payload::TakeBlob(uint32_t resOff, const std::map<uint32_t, uint32_t>& ext,
                       uint32_t& outKey, std::string& err) {
    uint32_t data = U32(src->b, resOff + 4);
    auto it = ext.find(data);
    if (it == ext.end()) { err = "an FXMF offset is not a known resource"; return false; }
    if (blobs.find(data) == blobs.end()) blobOrder.push_back(data);
    blobs[data] = Slice(src->b, src->fxmf + data, it->second);
    outKey = data;
    return true;
}

bool Payload::Build(const Xmf& s, const char* name, std::string& err) {
    src = &s;
    const Bytes& b = s.b;
    int oi = s.Find(name);
    if (oi < 0) { err = "the source container has no object with that name"; return false; }

    inst = Slice(b, s.InstOff(oi), INST);
    uint32_t objOff = s.ObjOff(oi);
    obj = Slice(b, objOff, OBJ);
    if (U32(b, s.InstOff(oi) + 0x44) != 4 + (uint32_t)oi * OBJ) {
        err = "instance does not point at its own object record"; return false;
    }
    if (U16(b, objOff + 0x1C)) { err = "skinned objects are not supported"; return false; }

    uint32_t partOff = U32(b, objOff + 0x00);
    uint32_t partCnt = U16(b, objOff + 0x04);
    uint32_t ibOff   = U32(b, objOff + 0x08);
    uint32_t descArr = U32(b, objOff + 0x10);
    uint32_t descCnt = U16(b, objOff + 0x14);
    if (!partCnt || !descCnt) { err = "object has no parts or no vertex declarations"; return false; }

    for (uint32_t i = 0; i < partCnt; ++i) parts.push_back(Slice(b, s.oxmf + partOff + i * PART, PART));
    for (uint32_t i = 0; i < descCnt; ++i) descs.push_back(Slice(b, s.oxmf + descArr + i * DESC, DESC));
    ibRes = Slice(b, s.oxmf + ibOff, RES);
    uint32_t idxWords = U32(b, s.oxmf + ibOff + 4);

    for (uint32_t i = 0; i < descCnt; ++i) descIndex[descArr + i * DESC] = (int)i;
    for (const Bytes& p : parts) {
        if (descIndex.find(U32(p, 0x40)) == descIndex.end()) {
            err = "a part uses a descriptor outside the object array"; return false;
        }
        if (!U16(p, 0x14)) { err = "non-indexed parts are not supported"; return false; }
    }

    // The contiguous span of index words this object's parts reference.
    uint32_t loW = 0xFFFFFFFFu, hiW = 0;
    for (const Bytes& p : parts) {
        loW = std::min(loW, U32(p, 0x10));
        hiW = std::max(hiW, U32(p, 0x10) + U32(p, 8) + U32(p, 0xC));
    }
    std::vector<uint16_t> words;
    words.reserve(hiW - loW);
    for (uint32_t w = loW; w < hiW; ++w) words.push_back(U16(b, s.oxmf + idxWords + w * 2));
    if (words.empty()) { err = "the object references no index words"; return false; }

    // Absolute vertex = part.baseVertex + indexWord (the D3D8 SetIndices BaseVertexIndex,
    // confirmed at 0x843C5). Copy [vmin..vmax] per declaration, shift every copied index word
    // down by one global constant S and compensate per part:
    //     newBase = base - vmin + S      newWord = word - S
    // so newBase + newWord == base + word - vmin, i.e. the vertex we actually copied.
    std::map<int, int64_t> vmin, vmax;
    for (const Bytes& p : parts) {
        int di = descIndex[U32(p, 0x40)];
        int32_t base = I32(p, 0x44);
        uint32_t i0 = U32(p, 0x10), n = U32(p, 8) + U32(p, 0xC);
        if (!n) { err = "a part has no indices"; return false; }
        if (i0 - loW + n > words.size()) { err = "a part's index range runs past the buffer"; return false; }
        uint16_t wmin = 0xFFFF, wmax = 0;
        for (uint32_t k = 0; k < n; ++k) {
            uint16_t w = words[i0 - loW + k];
            wmin = std::min(wmin, w); wmax = std::max(wmax, w);
        }
        auto itn = vmin.find(di);
        vmin[di] = (itn == vmin.end()) ? (int64_t)base + wmin
                                       : std::min(itn->second, (int64_t)base + wmin);
        auto itx = vmax.find(di);
        vmax[di] = (itx == vmax.end()) ? (int64_t)base + wmax
                                       : std::max(itx->second, (int64_t)base + wmax);
    }
    uint16_t shift = *std::min_element(words.begin(), words.end());

    for (uint32_t di = 0; di < descCnt; ++di) {
        uint32_t stride = 0;
        if (!DeclStride(descs[di], stride, err)) return false;
        strides.push_back(stride);
        if (vmin.find((int)di) == vmin.end()) { vbdata.push_back(Bytes()); continue; }
        uint32_t data = U32(b, s.oxmf + U32(descs[di], 4) + 4);   // VB resource Data, FXMF-rel
        int64_t first = (int64_t)s.fxmf + data + vmin[(int)di] * stride;
        int64_t count = vmax[(int)di] - vmin[(int)di] + 1;
        if (first < 0 || first + count * stride > (int64_t)b.size()) {
            err = "vertex window runs past the end of the file"; return false;
        }
        vbdata.push_back(Slice(b, (size_t)first, (size_t)(count * stride)));
    }

    idx.resize(words.size() * 2);
    for (size_t i = 0; i < words.size(); ++i) {
        uint16_t v = (uint16_t)(words[i] - shift);
        memcpy(idx.data() + i * 2, &v, 2);
    }
    for (Bytes& p : parts) {
        int di = descIndex[U32(p, 0x40)];
        int64_t newBase = (int64_t)I32(p, 0x44) - vmin[di] + shift;
        if (newBase < 0) { err = "rebased BaseVertexIndex would be negative"; return false; }
        W32(p, 0x44, (uint32_t)newBase);
        W32(p, 0x10, U32(p, 0x10) - loW);
    }

    // Materials, texture records/headers, palettes and the GPU blobs they point at.
    std::map<uint32_t, uint32_t> ext;
    if (!s.FxmfExtents(ext, err)) return false;

    std::set<uint32_t> seenMat;
    for (const Bytes& p : parts) {
        uint32_t mrel = U32(p, 0x18);
        if (seenMat.insert(mrel).second)
            mats.push_back({ mrel, Slice(b, s.rxmf + mrel, MAT) });
    }

    std::set<uint32_t> seenTrec, seenPal;
    for (const auto& m : mats) {
        for (int slot = 0; slot < 4; ++slot) {
            uint32_t rel = U32(m.second, 8 + slot * 4);
            if (rel == NONE || seenTrec.count(rel)) continue;
            TexRec tr;
            tr.srel = rel;
            tr.rec = Slice(b, s.txmf + rel, TREC);
            uint32_t hrel = U32(tr.rec, 4), prel = U32(tr.rec, 8);
            if (hrel != NONE) {
                tr.hasHdr = true;
                tr.hdr = Slice(b, s.txmf + hrel, THDR);
                if (!TakeBlob(s.txmf + hrel, ext, tr.hdrBlobKey, err)) return false;
            }
            if (prel != NONE) {
                tr.hasPal = true;
                tr.palSrel = prel;
                if (seenPal.insert(prel).second) {
                    Pal pal;
                    pal.srel = prel;
                    pal.res = Slice(b, s.txmf + prel, RES);
                    if (!TakeBlob(s.txmf + prel, ext, pal.blobKey, err)) return false;
                    pals.push_back(pal);
                }
            }
            seenTrec.insert(rel);
            trecs.push_back(tr);
        }
    }
    return true;
}

// ---------------------------------------------------------------- injection

bool Inject(const Xmf& dst, Payload& pay, const char* newName, Bytes& out, std::string& err) {
    const Bytes& b = dst.b;
    if (strlen(newName) != 4) { err = "the new name must be exactly 4 characters"; return false; }

    uint32_t vbs;
    if (!dst.VbResStart(vbs, err)) return false;
    uint32_t midStart = 4 + dst.objCount * OBJ;
    if (!(midStart <= vbs && vbs <= dst.oxmfSize)) {
        err = "unexpected OXMF layout: the VB-resource array is not at the tail"; return false;
    }

    // The new OXMF middle block: parts | descs | IB resource | index words.
    Bytes mid;
    uint32_t offParts = 0;
    for (const Bytes& p : pay.parts) Append(mid, p);
    uint32_t offDescs = (uint32_t)mid.size();
    for (const Bytes& d : pay.descs) Append(mid, d);
    uint32_t offIb = (uint32_t)mid.size();
    Append(mid, pay.ibRes);
    uint32_t offIdx = Align((uint32_t)mid.size(), 4);
    mid.resize(offIdx, 0);
    Append(mid, pay.idx);
    mid.resize(Align((uint32_t)mid.size(), 4), 0);
    uint32_t newMid = (uint32_t)mid.size();

    const uint32_t deltaMid = OBJ;              // existing middle data shifts by one object record
    const uint32_t deltaVb  = OBJ + newMid;     // the VB array shifts by that plus the new block

    // The object array does NOT move -- the new record is appended to its end -- so pointers
    // INTO it must be left alone. Only the middle data and the VB-resource array slide.
    auto shift = [&](uint32_t v) -> uint32_t {
        if (v < midStart) return v;
        return v + (v >= vbs ? deltaVb : deltaMid);
    };

    uint32_t baseMid   = vbs + deltaMid;
    uint32_t newObjRel = midStart;
    uint32_t newVbRel  = vbs + deltaVb + (dst.oxmfSize - vbs);

    uint32_t nNewMat  = (uint32_t)pay.mats.size();
    uint32_t nNewTrec = (uint32_t)pay.trecs.size();
    uint32_t nNewHdr  = 0;
    for (const TexRec& t : pay.trecs) if (t.hasHdr) ++nNewHdr;
    uint32_t oldHdrStart = dst.HdrStart(), oldPalStart = dst.PalStart();
    uint32_t nOldPal = (dst.txmfSize - oldPalStart) / RES;
    uint32_t newHdrStart = 4 + (dst.txmfCount + nNewTrec) * TREC;
    uint32_t dstDup = dst.TxmfDup();
    uint32_t newPalStart = newHdrStart + (dst.txmfCount - dstDup + nNewHdr) * THDR;
    uint32_t dHdr = newHdrStart - oldHdrStart;
    uint32_t dPal = newPalStart - oldPalStart;

    // FXMF placement: blobs first (0x80-aligned), then vertex data (0x20-aligned).
    Bytes fxTail;
    std::map<uint32_t, uint32_t> blobAt;
    uint32_t fxBase = dst.fxmfSize;
    for (uint32_t key : pay.blobOrder) {
        const Bytes& data = pay.blobs[key];
        fxTail.resize(Align(fxBase + (uint32_t)fxTail.size(), 0x80) - fxBase, 0);
        blobAt[key] = fxBase + (uint32_t)fxTail.size();
        Append(fxTail, data);
    }
    std::vector<int64_t> vbAt;
    for (const Bytes& v : pay.vbdata) {
        if (v.empty()) { vbAt.push_back(-1); continue; }
        fxTail.resize(Align(fxBase + (uint32_t)fxTail.size(), 0x20) - fxBase, 0);
        vbAt.push_back(fxBase + (int64_t)fxTail.size());
        Append(fxTail, v);
    }

    // Destination-relative offsets for every new record.
    std::map<uint32_t, uint32_t> newMatRel, newTrecRel, newHdrRel, newPalRel;
    for (size_t i = 0; i < pay.mats.size(); ++i)
        newMatRel[pay.mats[i].first] = 4 + (dst.rxmfCount + (uint32_t)i) * MAT;
    for (size_t i = 0; i < pay.trecs.size(); ++i)
        newTrecRel[pay.trecs[i].srel] = 4 + (dst.txmfCount + (uint32_t)i) * TREC;
    uint32_t h = 0;
    for (const TexRec& t : pay.trecs)
        if (t.hasHdr) newHdrRel[t.srel] = newHdrStart + (dst.txmfCount - dstDup + h++) * THDR;
    for (size_t i = 0; i < pay.pals.size(); ++i)
        newPalRel[pay.pals[i].srel] = newPalStart + (nOldPal + (uint32_t)i) * RES;

    // Rewrite the payload's own records.
    Bytes inst = pay.inst;
    memcpy(inst.data() + 0x48, newName, 4);
    W32(inst, 0x44, newObjRel);
    memcpy(inst.data() + 0x40, b.data() + dst.InstOff(0) + 0x40, 4);   // exporter constant

    Bytes obj = pay.obj;
    memcpy(obj.data() + 0x20, newName, 4);
    W32(obj, 0x00, baseMid + offParts);
    W32(obj, 0x08, baseMid + offIb);
    W32(obj, 0x10, baseMid + offDescs);
    W32(obj, 0x18, 0);                                                 // bone matrices
    W16(obj, 0x16, U16(b, dst.ObjOff(0) + 0x16));                      // exporter constants
    W16(obj, 0x1E, U16(b, dst.ObjOff(0) + 0x1E));

    Bytes ibRes = pay.ibRes;
    W32(ibRes, 4, baseMid + offIdx);
    memcpy(mid.data() + offIb, ibRes.data(), RES);

    for (size_t i = 0; i < pay.parts.size(); ++i) {
        Bytes q = pay.parts[i];
        W32(q, 0x00, newObjRel);                                       // object back-pointer
        W32(q, 0x18, newMatRel[U32(pay.parts[i], 0x18)]);              // RXMF-rel material
        W32(q, 0x40, baseMid + offDescs + pay.descIndex[U32(pay.parts[i], 0x40)] * DESC);
        memcpy(mid.data() + offParts + i * PART, q.data(), PART);
    }

    Bytes newVbRes;
    uint32_t slot = 0;
    for (size_t di = 0; di < pay.descs.size(); ++di) {
        Bytes q = pay.descs[di];
        if (vbAt[di] < 0) {
            W32(q, 4, 0);
        } else {
            Bytes res = Slice(pay.src->b, pay.src->oxmf + U32(pay.descs[di], 4), RES);
            W32(res, 4, (uint32_t)vbAt[di]);
            W32(q, 4, newVbRel + slot * RES);
            Append(newVbRes, res);
            ++slot;
        }
        memcpy(mid.data() + offDescs + di * DESC, q.data(), DESC);
    }

    Bytes newMats;
    for (const auto& m : pay.mats) {
        Bytes q = m.second;
        for (int s = 0; s < 4; ++s) {
            uint32_t rel = U32(q, 8 + s * 4);
            if (rel != NONE) W32(q, 8 + s * 4, newTrecRel[rel]);
        }
        Append(newMats, q);
    }

    Bytes newTrecs, newHdrs, newPals;
    for (const TexRec& t : pay.trecs) {
        Bytes q = t.rec;
        W32(q, 4, t.hasHdr ? newHdrRel[t.srel] : NONE);
        W32(q, 8, t.hasPal ? newPalRel[t.palSrel] : NONE);
        Append(newTrecs, q);
        if (t.hasHdr) {
            Bytes hq = t.hdr;
            W32(hq, 4, blobAt[t.hdrBlobKey]);
            Append(newHdrs, hq);
        }
    }
    for (const auto& p : pay.pals) {
        Bytes q = p.res;
        W32(q, 4, blobAt[p.blobKey]);
        Append(newPals, q);
    }

    // ---- assemble ----------------------------------------------------------------------
    Bytes hdr = Slice(b, 0, 0x30);
    W32(hdr, 0x04, dst.ixmfCount + 1);
    W32(hdr, 0x08, dst.objCount + 1);
    W32(hdr, 0x0C, dst.rxmfCount + nNewMat);
    W32(hdr, 0x10, dst.txmfCount + nNewTrec);
    W32(hdr, 0x1C, dst.ixmfSize + INST);
    W32(hdr, 0x20, dst.oxmfSize + OBJ + newMid + (uint32_t)newVbRes.size());
    W32(hdr, 0x24, dst.rxmfSize + (uint32_t)newMats.size());
    W32(hdr, 0x28, dst.txmfSize + (uint32_t)(newTrecs.size() + newHdrs.size() + newPals.size()));
    W32(hdr, 0x2C, dst.fxmfSize + (uint32_t)fxTail.size());
    out = hdr;

    // IXMF: existing instances (object pointers shifted) then the new one.
    Bytes ixmf = Slice(b, dst.ixmf, dst.ixmfSize);
    for (uint32_t i = 0; i < dst.ixmfCount; ++i) {
        uint32_t o = 4 + i * INST + 0x44;
        W32(ixmf, o, shift(U32(ixmf, o)));
    }
    Append(out, ixmf);
    Append(out, inst);

    // OXMF: collect the SITES first -- objects may share a part array, an index buffer or a
    // declaration array, and a site must be shifted exactly once.
    Bytes oxmf = Slice(b, dst.oxmf, dst.oxmfSize);
    std::set<uint32_t> sites;
    for (uint32_t i = 0; i < dst.objCount; ++i) {
        uint32_t o = 4 + i * OBJ;
        sites.insert(o + 0x00); sites.insert(o + 0x08); sites.insert(o + 0x10);
        uint32_t partOff = U32(oxmf, o + 0x00), partCnt = U16(oxmf, o + 0x04);
        for (uint32_t pi = 0; pi < partCnt; ++pi) sites.insert(partOff + pi * PART + 0x40);
        sites.insert(U32(oxmf, o + 0x08) + 4);                     // IB resource -> words
        uint32_t descArr = U32(oxmf, o + 0x10), descCnt = U16(oxmf, o + 0x14);
        for (uint32_t k = 0; k < descCnt; ++k) sites.insert(descArr + k * DESC + 4);
    }
    for (uint32_t s : sites) {
        if (s + 4 > oxmf.size()) { err = "a pointer site lies outside OXMF"; return false; }
        W32(oxmf, s, shift(U32(oxmf, s)));
    }
    AppendRange(out, oxmf, 0, 4);                     // 'OXMF'
    AppendRange(out, oxmf, 4, midStart);              // existing object records
    Append(out, obj);                                 // the new object record
    AppendRange(out, oxmf, midStart, vbs);            // existing middle data
    Append(out, mid);                                 // the new middle block
    AppendRange(out, oxmf, vbs, dst.oxmfSize);        // existing VB resources
    Append(out, newVbRes);                            // the new VB resources

    // RXMF: a pure array -- append. Existing offsets are unaffected.
    AppendRange(out, b, dst.rxmf, dst.rxmf + dst.rxmfSize);
    Append(out, newMats);

    // TXMF: records | headers | palettes, each group appended to. Records never move, so the
    // material texture slots that point at them stay valid.
    Bytes txmf = Slice(b, dst.txmf, dst.txmfSize);
    for (uint32_t t = 0; t < dst.txmfCount; ++t) {
        uint32_t r = 4 + t * TREC;
        if (U32(txmf, r + 4) != NONE) W32(txmf, r + 4, U32(txmf, r + 4) + dHdr);
        if (U32(txmf, r + 8) != NONE) W32(txmf, r + 8, U32(txmf, r + 8) + dPal);
    }
    AppendRange(out, txmf, 0, 4);
    AppendRange(out, txmf, 4, oldHdrStart);
    Append(out, newTrecs);
    AppendRange(out, txmf, oldHdrStart, oldPalStart);
    Append(out, newHdrs);
    AppendRange(out, txmf, oldPalStart, dst.txmfSize);
    Append(out, newPals);

    if (dst.idsfSize) AppendRange(out, b, dst.idsf, dst.idsf + dst.idsfSize);
    AppendRange(out, b, dst.fxmf, dst.fxmf + dst.fxmfSize);
    Append(out, fxTail);
    return true;
}

// ---------------------------------------------------------------- verification

// Walk every cross-reference and confirm it lands where the engine's fix-up pass expects.
// This is what catches an injector arithmetic slip -- see the header comment.
bool Validate(const Xmf& x, std::string& err) {
    const Bytes& b = x.b;
    uint32_t objEnd = 4 + x.objCount * OBJ;
    uint32_t vbs;
    if (!x.VbResStart(vbs, err)) return false;
    uint32_t hdrStart = x.HdrStart(), palStart = x.PalStart();

    for (uint32_t i = 0; i < x.ixmfCount; ++i) {
        uint32_t v = U32(b, x.InstOff(i) + 0x44);
        if (!(v >= 4 && v < objEnd && (v - 4) % OBJ == 0) || v != 4 + i * OBJ) {
            err = "an instance does not point at its own object record"; return false;
        }
    }
    for (uint32_t i = 0; i < x.objCount; ++i) {
        uint32_t o = x.ObjOff(i);
        uint32_t partOff = U32(b, o + 0x00), partCnt = U16(b, o + 0x04);
        uint32_t ib = U32(b, o + 0x08), descArr = U32(b, o + 0x10), descCnt = U16(b, o + 0x14);
        if (partOff < objEnd || partOff + partCnt * PART > vbs) { err = "part array outside the middle region"; return false; }
        if (ib < objEnd || ib + RES > vbs) { err = "IB resource outside the middle region"; return false; }
        if (descArr < objEnd || descArr + descCnt * DESC > vbs) { err = "decl array outside the middle region"; return false; }
        uint32_t wordsOff = U32(b, x.oxmf + ib + 4);
        if (wordsOff < objEnd || wordsOff > vbs) { err = "index words outside the middle region"; return false; }
        for (uint32_t k = 0; k < descCnt; ++k) {
            uint32_t d = descArr + k * DESC;
            uint32_t vb = U32(b, x.oxmf + d + 4);
            if (!(vb >= vbs && vb < x.oxmfSize && (vb - vbs) % RES == 0)) { err = "VB resource is not in the tail array"; return false; }
            if (U32(b, x.oxmf + vb + 4) >= x.fxmfSize) { err = "VB data past FXMF"; return false; }
        }
        for (uint32_t pi = 0; pi < partCnt; ++pi) {
            uint32_t p = x.oxmf + partOff + pi * PART;
            uint32_t dref = U32(b, p + 0x40);
            if (!(dref >= descArr && dref < descArr + descCnt * DESC)) { err = "part declaration outside this object's array"; return false; }
            uint32_t m = U32(b, p + 0x18);
            if (!(m >= 4 && m < x.rxmfSize && (m - 4) % MAT == 0)) { err = "part material is not a material record"; return false; }
            if (wordsOff + (U32(b, p + 0x10) + U32(b, p + 8) + U32(b, p + 0xC)) * 2 > vbs) {
                err = "part index range runs past the middle region"; return false;
            }
        }
    }
    for (uint32_t m = 0; m < x.rxmfCount; ++m) {
        uint32_t r = x.rxmf + 4 + m * MAT;
        for (int s = 0; s < 4; ++s) {
            uint32_t rel = U32(b, r + 8 + s * 4);
            if (rel == NONE) continue;
            if (!(rel >= 4 && rel < hdrStart && (rel - 4) % TREC == 0)) { err = "material slot is not a TXMF record"; return false; }
        }
    }
    for (uint32_t t = 0; t < x.txmfCount; ++t) {
        uint32_t rec = x.TrecOff(t);
        uint32_t hh = U32(b, rec + 4), pp = U32(b, rec + 8);
        if (hh != NONE) {
            if (!(hh >= hdrStart && hh < palStart && (hh - hdrStart) % THDR == 0)) { err = "texture header outside the header region"; return false; }
            if (U32(b, x.txmf + hh + 4) >= x.fxmfSize) { err = "texture pixels past FXMF"; return false; }
        }
        if (pp != NONE) {
            if (!(pp >= palStart && pp < x.txmfSize && (pp - palStart) % RES == 0)) { err = "palette outside the palette region"; return false; }
            if (U32(b, x.txmf + pp + 4) >= x.fxmfSize) { err = "palette data past FXMF"; return false; }
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------- public API

const ArenaFile kArenaFiles[13] = {
    {  0, "KITCHEN",  "OBJECTS.xmf" }, {  1, "HAUNTED",  "OBJECTS.xmf" },
    {  2, "SCRAP",    "WEAPONS.xmf" }, {  3, "SHIP",     "OBJECTS.xmf" },
    {  4, "CABIN",    "OBJECTS.xmf" }, {  5, "BANQUET",  "OBJECTS.xmf" },
    {  6, "ISLAND",   "OBJECTS.xmf" }, {  7, "BUILDING", "OBJECTS.xmf" },
    {  8, "LAB",      "OBJECTS.xmf" }, {  9, "WILDWEST", "OBJECTS.xmf" },
    { 10, "BOXING",   "OBJECTS.xmf" }, { 11, "MARKET",   "OBJECTS.xmf" },
    { 12, "HELL",     "OBJECTS.xmf" },
};

bool XmfHasObject(const wchar_t* path, const char* name, std::string& err) {
    Bytes buf;
    if (!ReadWholeFile(path, buf, err)) return false;
    Xmf x; x.b.swap(buf);
    if (!x.Parse(err)) return false;
    return x.Find(name) >= 0;
}

bool InjectXmfObject(const wchar_t* srcPath, const char* srcName,
                     const wchar_t* dstPath, const char* newName,
                     bool force, std::string& err) {
    Bytes dbuf;
    if (!ReadWholeFile(dstPath, dbuf, err)) return false;
    Xmf dst; dst.b.swap(dbuf);
    if (!dst.Parse(err)) return false;
    if (dst.Find(newName) >= 0 && !force) return false;      // already injected: not an error

    Bytes sbuf;
    if (!ReadWholeFile(srcPath, sbuf, err)) return false;
    Xmf src; src.b.swap(sbuf);
    if (!src.Parse(err)) return false;

    Payload pay;
    if (!pay.Build(src, srcName, err)) return false;

    Bytes out;
    if (!Inject(dst, pay, newName, out, err)) return false;

    // Re-parse the produced bytes with the same reader, then walk every cross-reference.
    Xmf check; check.b = out;
    if (!check.Parse(err)) { err = "the injected file failed to re-parse: " + err; return false; }
    if (!Validate(check, err)) { err = "the injected file failed validation: " + err; return false; }
    if (check.Find(newName) < 0) { err = "the injected object is not present in the result"; return false; }

    // Write via a temporary so a failure part-way cannot leave a half-written arena file.
    std::wstring tmp = std::wstring(dstPath) + L".tmp";
    if (!WriteWholeFile(tmp.c_str(), out, err)) return false;
    if (!MoveFileExW(tmp.c_str(), dstPath, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        err = "could not replace the arena file";
        return false;
    }
    return true;
}

MeatInjectResult InjectMeatIntoAllArenas(const wchar_t* gameRoot) {
    MeatInjectResult r;
    std::wstring gfx = std::wstring(gameRoot) + L"\\GFX";
    std::wstring src = gfx + L"\\KITCHEN\\OBJECTS.xmf";

    for (const ArenaFile& a : kArenaFiles) {
        wchar_t folder[64], file[64];
        MultiByteToWideChar(CP_ACP, 0, a.folder, -1, folder, 64);
        MultiByteToWideChar(CP_ACP, 0, a.file, -1, file, 64);
        std::wstring path = gfx + L"\\" + folder + L"\\" + file;

        std::string err;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            ++r.failed;
            if (r.firstError.empty()) r.firstError = std::string(a.folder) + ": file is missing";
            continue;
        }
        if (InjectXmfObject(src.c_str(), "WPH2", path.c_str(), "MEAT", false, err)) {
            ++r.injected;
        } else if (err.empty()) {
            ++r.present;                       // already carried MEAT
        } else {
            ++r.failed;
            if (r.firstError.empty()) r.firstError = std::string(a.folder) + ": " + err;
        }
    }
    return r;
}

} // namespace tj::setup
