#include "runtime/assets/xmf_mesh.h"
#include "runtime/assets/xbd_anim.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <algorithm>

namespace tj::assets {
namespace {

inline uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline int32_t  rds32(const uint8_t* p) { int32_t v; memcpy(&v, p, 4); return v; }
inline uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
inline float    rdf(const uint8_t* p)  { float v; memcpy(&v, p, 4); return v; }

std::vector<uint8_t> ReadAll(const std::wstring& path) {
    std::vector<uint8_t> data;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return data;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    data.resize(n);
    if (n > 0) fread(data.data(), 1, n, f);
    fclose(f);
    return data;
}

// Engine per-token component sizes (DAT_00179ce0, used by the stride calc FUN_0007c270).
// Only tokens 1/2/4/7 appear in shipped data; the rest are kept for completeness.
constexpr int kTokSize[16] = { 0, 12, 8, 12, 12, 4, 4, 4, 4, 8, 12, 0, 4, 8, 4, 6 };
constexpr uint32_t kTokEnd = 0xb;

struct Decl {
    int stride = 0;
    int posOff = -1, uvOff = -1, colorOff = -1;   // byte offsets, -1 = absent
    int boneRegOff = -1, boneWtOff = -1;          // skinning: 4 x s16 regs, 4 x u8 weights
};

Decl ParseDecl(const uint8_t* f, size_t off) {
    Decl d;
    for (int i = 0; i < 16; ++i) {
        uint32_t tok = rd32(f + off + i*4);
        if (tok == kTokEnd) break;
        if (tok == 1) d.posOff = d.stride;
        else if (tok == 2) d.uvOff = d.stride;
        else if (tok == 7) d.colorOff = d.stride;
        else if (tok == 13) d.boneRegOff = d.stride;
        else if (tok == 14) d.boneWtOff = d.stride;
        d.stride += (tok < 16) ? kTokSize[tok] : 0;
    }
    return d;
}

} // namespace

XmfScene LoadScene(const std::wstring& path) {
    XmfScene sc;
    std::vector<uint8_t> f = ReadAll(path);
    auto fail = [&](const char* why) { sc.error = why; return sc; };
    if (f.size() < 0x34) return fail("file too small");
    if (memcmp(f.data(), "XMF", 3) != 0) return fail("no XMF magic");
    sc.version = f[3] - '0';
    if (sc.version < 1 || sc.version > 3) return fail("unsupported version");

    const uint32_t objCount  = rd32(&f[8]);
    const uint32_t rxmfCount = rd32(&f[0xc]);
    const uint32_t idsfSize  = (rd32(&f[0x14]) == 0xabcd) ? 0 : rd32(&f[0x18]);
    const uint32_t ixmfSize  = rd32(&f[0x1c]);
    const uint32_t oxmfSize  = rd32(&f[0x20]);
    const uint32_t rxmfSize  = rd32(&f[0x24]);
    const uint32_t txmfSize  = rd32(&f[0x28]);
    const uint32_t fxmfSize  = rd32(&f[0x2c]);
    const uint32_t prefixSize = (sc.version >= 2) ? rd32(&f[0x30]) : 0;

    const size_t prefixOff = (sc.version >= 2) ? 0x34 : 0x30;
    const size_t ixmfOff = prefixOff + prefixSize;
    const size_t oxmfOff = ixmfOff + ixmfSize;
    const size_t rxmfOff = oxmfOff + oxmfSize;
    const size_t txmfOff = rxmfOff + rxmfSize;
    const size_t idsfOff = txmfOff + txmfSize;
    const size_t fxmfOff = idsfOff + idsfSize;
    // .XBD characters append skeleton/animation blocks (CXBD/JXBD) after the container,
    // so allow trailing data; the sections themselves must fit.
    if (fxmfOff + fxmfSize > f.size()) return fail("section sizes disagree with file size");
    if (memcmp(&f[ixmfOff], "IXMF", 4) || memcmp(&f[oxmfOff], "OXMF", 4) ||
        memcmp(&f[rxmfOff], "RXMF", 4) || memcmp(&f[txmfOff], "TXMF", 4) ||
        memcmp(&f[fxmfOff], "FXMF", 4)) return fail("section tag mismatch");

    const uint8_t* F = f.data();
    // Guarded readers: every game-supplied offset is validated before use.
    auto in = [&](size_t off, size_t need) { return off + need <= f.size(); };

    // A material texture slot is the TXMF-rel offset of a 0x10-byte TXMF table
    // record: +4 -> texture header (TXMF-rel), +8 -> palette resource (TXMF-rel,
    // for P8) or -1. Headers are D3DPixelContainer: Data(+4) = FXMF-rel pixels,
    // Format(+0xC) packed. Decode lazily, dedup by record offset.
    std::map<uint32_t, int> texByOff;
    auto textureFor = [&](uint32_t slotRel) -> int {
        if (slotRel == 0xffffffffu) return -1;
        auto it = texByOff.find(slotRel);
        if (it != texByOff.end()) return it->second;
        texByOff[slotRel] = -1;
        size_t rec = txmfOff + slotRel;
        if (!in(rec, 0x10)) return -1;
        uint32_t hdrRel = rd32(F + rec + 4);
        uint32_t palRel = rd32(F + rec + 8);
        size_t hdr = txmfOff + hdrRel;
        if (hdrRel == 0xffffffffu || !in(hdr, 0x14)) return -1;
        if ((rd32(F + hdr) & 0x00070000) != 0x00040000) return -1;   // not a texture
        uint32_t data = rd32(F + hdr + 4);             // FXMF-rel pixel offset
        uint32_t fmtd = rd32(F + hdr + 0xc);
        int fmt = (fmtd >> 8) & 0xff, us = (fmtd >> 20) & 0xf, vs = (fmtd >> 24) & 0xf;
        if (!us || !vs || !in(fxmfOff + data, 8)) return -1;
        int W = 1 << us, H = 1 << vs;
        size_t avail = f.size() - fxmfOff - data;
        const uint32_t* pal = nullptr;                 // palette for P8
        if (palRel != 0xffffffffu && in(txmfOff + palRel, 12)) {
            size_t pres = txmfOff + palRel;
            if ((rd32(F + pres) & 0x00070000) == 0x00030000) {       // D3DPalette
                size_t pdata = fxmfOff + rd32(F + pres + 4);
                if (in(pdata, 256 * 4)) pal = reinterpret_cast<const uint32_t*>(F + pdata);
            }
        }
        DecodedTexture t = DecodeXboxTexture(fmt, F + fxmfOff + data, avail, pal, W, H);
        int idx = -1;
        if (t.ok) { idx = (int)sc.textures.size(); sc.textures.push_back(std::move(t)); }
        texByOff[slotRel] = idx;
        return idx;
    };

    bool first = true;
    for (uint32_t oi = 0; oi < objCount; ++oi) {
        size_t obj = oxmfOff + 4 + oi * 0x4c;
        if (!in(obj, 0x4c)) break;
        // Skinned objects (boneCount > 0) load in bind pose: the decl parser skips the
        // bone weight/index components (tokens 13/14) via the size table. Animation later.
        uint32_t partOff  = rd32(F + obj);
        uint16_t partCnt  = rd16(F + obj + 4);
        uint32_t ibField  = rd32(F + obj + 8);

        // Locate this object's raw 16-bit index words (OXMF-rel).
        uint32_t idxWords;
        if (sc.version >= 2) {
            if (!in(prefixOff + ibField + 8, 4)) continue;
            uint32_t inner = rd32(F + prefixOff + ibField + 4);
            if (!in(oxmfOff + inner + 8, 4)) continue;
            idxWords = rd32(F + oxmfOff + inner + 4);
        } else {
            if (!in(oxmfOff + ibField + 8, 4)) continue;
            idxWords = rd32(F + oxmfOff + ibField + 4);
        }

        for (uint16_t pi = 0; pi < partCnt; ++pi) {
            size_t p = oxmfOff + partOff + pi * 0x54;
            if (!in(p, 0x54)) break;
            uint32_t nStrip = rd32(F + p + 8);
            uint32_t nList  = rd32(F + p + 0xc);
            uint32_t iStart = rd32(F + p + 0x10);
            if (!rd16(F + p + 0x14)) continue;         // non-indexed: not seen in data
            int32_t baseV   = rds32(F + p + 0x44);
            uint32_t nIdx   = nStrip + nList;
            if (nIdx < 3) continue;
            size_t iw = oxmfOff + idxWords + (size_t)iStart * 2;
            if (!in(iw, (size_t)nIdx * 2)) continue;

            size_t d = oxmfOff + rd32(F + p + 0x40);
            if (!in(d, 0x50)) continue;
            if (rds32(F + d) == -1) continue;          // dynamic/UP stream: skip
            Decl decl = ParseDecl(F, d + 0x10);
            if (decl.stride <= 0 || decl.posOff < 0) continue;
            // Position-only streams (decl [1], stride 12) are shadow/mask geometry
            // drawn by a dedicated technique — not visible surfaces. Skip until
            // shadows are implemented (they render as solid white otherwise).
            if (decl.uvOff < 0 && decl.colorOff < 0 && decl.boneRegOff < 0) continue;
            size_t vbRes = oxmfOff + rd32(F + d + 4);
            if (!in(vbRes, 12)) continue;
            size_t vb = fxmfOff + rd32(F + vbRes + 4); // VB resource Data = FXMF-rel
            uint32_t base = (baseV < 0) ? 0 : (uint32_t)baseV;

            // Referenced index range -> compact per-part vertex array.
            const uint8_t* iwp = F + iw;
            uint16_t lo = 0xffff, hi = 0;
            for (uint32_t k = 0; k < nIdx; ++k) {
                uint16_t ix = rd16(iwp + k*2);
                lo = std::min(lo, ix); hi = std::max(hi, ix);
            }
            size_t vFirst = vb + ((size_t)base + lo) * decl.stride;
            size_t vCount = (size_t)hi - lo + 1;
            if (!in(vFirst, vCount * decl.stride)) continue;

            XmfMeshPart part;
            part.object = (int)oi;
            uint32_t matRel = rd32(F + p + 0x18);      // RXMF-rel material instance
            if (in(rxmfOff + matRel + 0x18, 4))
                part.texture = textureFor(rd32(F + rxmfOff + matRel + 8));
            if (part.texture >= 0) part.alpha = sc.textures[part.texture].hasAlpha;

            part.verts.resize(vCount);
            part.skinned = decl.boneRegOff >= 0 && decl.boneWtOff >= 0;
            if (part.skinned) {
                part.skinBones.assign(vCount * 4, 0xFF);
                part.skinWeights.assign(vCount * 4, 0);
            }
            for (size_t v = 0; v < vCount; ++v) {
                const uint8_t* vp = F + vFirst + v * decl.stride;
                if (part.skinned) {
                    for (int i = 0; i < 4; ++i) {
                        uint16_t raw = rd16(vp + decl.boneRegOff + 2*i);
                        if (raw == 0xcdcd) continue;
                        int bone = BoneFromRegister((int16_t)raw);
                        if (bone < 0) continue;
                        part.skinBones[v*4 + i]   = (uint8_t)bone;
                        part.skinWeights[v*4 + i] = vp[decl.boneWtOff + i];
                    }
                }
                XmfVertex& o = part.verts[v];
                o.x = rdf(vp + decl.posOff); o.y = rdf(vp + decl.posOff + 4); o.z = rdf(vp + decl.posOff + 8);
                o.u = decl.uvOff >= 0 ? rdf(vp + decl.uvOff) : 0.0f;
                o.v = decl.uvOff >= 0 ? rdf(vp + decl.uvOff + 4) : 0.0f;
                o.color = decl.colorOff >= 0 ? rd32(vp + decl.colorOff) : 0xffffffffu;
                if (first) {
                    memcpy(sc.bbMin, &o.x, 12); memcpy(sc.bbMax, &o.x, 12); first = false;
                } else {
                    sc.bbMin[0] = std::min(sc.bbMin[0], o.x); sc.bbMax[0] = std::max(sc.bbMax[0], o.x);
                    sc.bbMin[1] = std::min(sc.bbMin[1], o.y); sc.bbMax[1] = std::max(sc.bbMax[1], o.y);
                    sc.bbMin[2] = std::min(sc.bbMin[2], o.z); sc.bbMax[2] = std::max(sc.bbMax[2], o.z);
                }
            }

            // Triangulate: strip (degenerate-aware, alternating winding), then list.
            part.indices.reserve(nIdx * 3);
            auto rebase = [&](uint16_t ix) { return (uint16_t)(ix - lo); };
            for (uint32_t k = 0; k + 2 < nStrip; ++k) {
                uint16_t a = rd16(iwp + k*2), b = rd16(iwp + (k+1)*2), c = rd16(iwp + (k+2)*2);
                if (a == b || b == c || a == c) continue;
                if (k & 1) { part.indices.push_back(rebase(a)); part.indices.push_back(rebase(c)); part.indices.push_back(rebase(b)); }
                else       { part.indices.push_back(rebase(a)); part.indices.push_back(rebase(b)); part.indices.push_back(rebase(c)); }
            }
            for (uint32_t k = nStrip; k + 2 < nIdx; k += 3) {
                part.indices.push_back(rebase(rd16(iwp + k*2)));
                part.indices.push_back(rebase(rd16(iwp + (k+1)*2)));
                part.indices.push_back(rebase(rd16(iwp + (k+2)*2)));
            }
            if (!part.indices.empty()) sc.parts.push_back(std::move(part));
        }
    }
    (void)rxmfCount;
    sc.ok = !sc.parts.empty();
    if (!sc.ok && sc.error.empty()) sc.error = "no static mesh parts";
    return sc;
}

} // namespace tj::assets
