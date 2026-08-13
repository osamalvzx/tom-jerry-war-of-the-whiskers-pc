// .TEC level collision — reverse-engineered from the loader (FUN_00019630 /
// FUN_00019770 via the level-path table at DAT_00114ad0) and consumers.
//
// File layout:
//   [u32 countA][countA x 0x44 face records]
//   [u32 zoneCount][per zone: vec3 a, vec3 b, u32 n, n x 0x44 face records]
//   [tail: ... +2000: 12 x 0x18 spawn records {vec3 pos, vec3 dir}, more arrays]
// Face record (0x44B): +0 float3 unit normal, +0xC float d (plane n.x + d = 0),
//   +0x10/0x1C/0x28 triangle verts, +0x34 normal duplicate,
//   +0x40 bytes {?, active, id, surfaceType}.
// Coordinates are GAME/entity space (the playable arena, e.g. Kitchen 0..160) —
// the same space the spawn points and characters live in.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tj::assets {

struct TecFace {
    float n[3], d;
    float v[3][3];
    uint8_t active, id, type;
};

struct TecSpawn { float pos[3], dir[3]; };

struct TecCollision {
    bool ok = false;
    std::string error;
    std::vector<TecFace> faces;      // A-faces + all zone faces
    std::vector<TecSpawn> spawns;    // 12 player spawn records

    // Highest walkable face (n.y > 0.5) under (x, z) with plane-y <= yFrom + stepUp.
    // Returns true and writes outY; false if no ground found.
    bool GroundY(float x, float z, float yFrom, float stepUp, float* outY) const;

    // Push a foot-sphere (centre pos + (0,r,0), radius r) out of wall faces
    // (|n.y| <= 0.6). Modifies pos x/z. Returns true if any contact.
    bool ResolveWalls(float pos[3], float r) const;
};

TecCollision LoadTec(const std::wstring& path);

} // namespace tj::assets
