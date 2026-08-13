// XBD character skeleton + animation (CXBD/JXBD/SXBD blocks after the XMF container).
//
// Reverse-engineered from the XBD loader (default.xbe FUN_0007f430), pose composer
// (FUN_0007f920), key sampler (FUN_0007f240) and frame blender (FUN_0007ed70):
//   CXBD hdr: u16 jointCount, u16 animCount, u32 jxbdSize, u32 axbdSize.
//   JXBD: joint records 0x38B: {u16 depthSlot, u8 cond, u8 charMask, float3 bindPos,
//     float3x3 bindRot}. Hierarchy is a depth-slot matrix stack (DFS order).
//   SXBD/AXBD: anim records 0x10B: {u16 id, u16 keyCount, u32, u32 keysOff, u32}.
//     Key 0x20B: {u32 frameOff, float3 rootPos, ...}. A frame = jointCount packed
//     14-byte records: s16 quat[4] (x,y,z,w  /32768), s16 mode/X, s16 Y, s16 Z —
//     mode<0: translation delta (masked s16, /1024); else scale (/128, non-propagating).
//   Bone matrix (model-space deformation, column-vector, row-major storage):
//     M = M_parentchain * T(bind+delta) * R(quat) * T(-bind);   root gets
//     T(keyRoot - joint0.bind). Vertices stay in model space; bind pose == identity.
//   Skinning streams (vertex decl tokens 13/14): 4 x s16 shader-constant register
//     (bone = index into the engine table DAT_00181f90, 0xCDCD = unused) and
//     4 x u8 weights (/255).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tj::assets {

struct Mat34 { float m[3][4]; };   // rows; translation in column 3

struct XbdJoint {
    uint16_t depthSlot;
    float bindPos[3];
    float bindRot[9];
};

struct XbdKey {
    uint32_t frameOff;             // offset of the frame block (rel. to anim section)
    float root[3];
};

struct XbdAnim {
    std::vector<XbdKey> keys;
};

struct XbdCharacter {
    bool ok = false;
    std::string error;
    std::vector<XbdJoint> joints;
    std::vector<XbdAnim> anims;
    std::vector<uint8_t> animData;     // the whole SXBD/AXBD section (frame blocks)

    // Compose bone matrices for animation `anim` at time t in keys (e.g. 3.7 =
    // between key 3 and 4, wrapping). Returns jointCount matrices.
    void EvaluatePose(int anim, float t, std::vector<Mat34>& bones) const;
};

// Parse the skeleton/animation blocks of an .XBD file (after the XMF container).
XbdCharacter LoadCharacter(const std::wstring& path);

// Map a vertex bone register (s16 from the vertex stream) to a joint index, or -1.
int BoneFromRegister(int reg);

} // namespace tj::assets
