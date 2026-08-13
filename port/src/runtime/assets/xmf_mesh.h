// XMF visual-mesh (OXMF) loader — the game's static level / prop geometry.
//
// Reverse-engineered from the XMF scene loader (default.xbe FUN_00079ee0) and the
// renderer's draw path (FUN_00083ab0 / FUN_000840a0). Container layout (file order,
// matching the loader's sequential reads):
//   [0x2c header][u32 fxmfSize][u32 prefixSize if v>=2]
//   [prefix(pushbuffer)][IXMF][OXMF][RXMF][TXMF][IDSF][FXMF]
// Header: +4 ixmfCount, +8 objCount, +0xC rxmfCount, +0x10 txmfCount,
//         +0x14 flag(0xabcd -> no IDSF), +0x18..0x28 IDSF/IXMF/OXMF/RXMF/TXMF sizes.
//
// OXMF object (0x4C B): +0 -> part array (OXMF-rel), +4 u16 partCount, +8 -> index
//   record, +0x10 -> VB descriptor array, +0x14 u16 descCount, +0x1C u16 boneCount.
// Mesh part (0x54 B): +4 vertexCount, +8 stripIdxCount, +0xC listIdxCount,
//   +0x10 indexStart (16-bit words), +0x14 u16 indexed, +0x18 -> RXMF material,
//   +0x40 -> VB descriptor, +0x44 baseVertex (bit31 set = indices are absolute).
// VB descriptor (0x50 B): +4 -> 12-byte VB resource (Data@+4 = FXMF-rel offset of the
//   vertex stream), +0x10 dword component tokens terminated by 0xB; stride = sum of
//   the engine's per-token size table (1=pos f3, 2=uv f2, 4=normal f3, 7=color dword).
// Index words live in the OXMF section: v1 obj+8 -> IB resource, Data(+4) -> words;
//   v>=2 obj+8 -> prefix record, +4 -> IB resource, Data(+4) -> words.
// RXMF material (0x50 B): +8/+0xC/+0x10/+0x14 texture slots = TXMF-rel offsets of
//   D3DPixelContainer texture headers (-1 = none); pixels at FXMF + header.Data.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "runtime/assets/xmf_texture.h"

namespace tj::assets {

struct XmfVertex { float x, y, z, u, v; uint32_t color; };

struct XmfMeshPart {
    std::vector<XmfVertex> verts;
    std::vector<uint16_t>  indices;    // triangulated (list)
    int object   = 0;                  // owning OXMF object index
    int texture  = -1;                 // index into XmfScene::textures, -1 = untextured
    bool alpha   = false;              // texture has translucent pixels -> draw blended, after opaques
    // Skinning (present when the vertex decl carries tokens 13/14): 4 entries per
    // vertex — joint index (0xFF = unused) and weight (/255).
    std::vector<uint8_t> skinBones;
    std::vector<uint8_t> skinWeights;
    bool skinned = false;
};

struct XmfScene {
    bool ok = false;
    std::string error;
    int version = 0;
    std::vector<XmfMeshPart>     parts;
    std::vector<DecodedTexture>  textures;
    float bbMin[3] = {0,0,0}, bbMax[3] = {0,0,0};
};

// Load every static mesh part of an XMF scene (levels, props). Skinned objects
// (boneCount > 0) are skipped for now — characters come later.
XmfScene LoadScene(const std::wstring& path);

} // namespace tj::assets
