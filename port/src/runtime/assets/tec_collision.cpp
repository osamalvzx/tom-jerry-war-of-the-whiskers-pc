#include "runtime/assets/tec_collision.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace tj::assets {
namespace {

inline uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline float    rdf(const uint8_t* p) { float v; memcpy(&v, p, 4); return v; }

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

TecFace ParseFace(const uint8_t* p) {
    TecFace f{};
    for (int i = 0; i < 3; ++i) f.n[i] = rdf(p + 4*i);
    f.d = rdf(p + 0xc);
    for (int v = 0; v < 3; ++v)
        for (int i = 0; i < 3; ++i) f.v[v][i] = rdf(p + 0x10 + v*0xc + 4*i);
    f.active = p[0x41]; f.id = p[0x42]; f.type = p[0x43];
    return f;
}

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection).
void ClosestOnTri(const float a[3], const float b[3], const float c[3],
                  const float p[3], float out[3]) {
    auto sub = [](const float* x, const float* y, float* o) {
        o[0]=x[0]-y[0]; o[1]=x[1]-y[1]; o[2]=x[2]-y[2]; };
    auto dot = [](const float* x, const float* y) {
        return x[0]*y[0]+x[1]*y[1]+x[2]*y[2]; };
    float ab[3], ac[3], ap[3];
    sub(b, a, ab); sub(c, a, ac); sub(p, a, ap);
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) { memcpy(out, a, 12); return; }
    float bp[3]; sub(p, b, bp);
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) { memcpy(out, b, 12); return; }
    float vc = d1*d4 - d3*d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        float t = d1 / (d1 - d3);
        for (int i = 0; i < 3; ++i) out[i] = a[i] + t*ab[i];
        return;
    }
    float cp[3]; sub(p, c, cp);
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) { memcpy(out, c, 12); return; }
    float vb = d5*d2 - d1*d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        float t = d2 / (d2 - d6);
        for (int i = 0; i < 3; ++i) out[i] = a[i] + t*ac[i];
        return;
    }
    float va = d3*d6 - d5*d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        float t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int i = 0; i < 3; ++i) out[i] = b[i] + t*(c[i] - b[i]);
        return;
    }
    float denom = 1.0f / (va + vb + vc);
    float sv = vb * denom, tv = vc * denom;
    for (int i = 0; i < 3; ++i) out[i] = a[i] + ab[i]*sv + ac[i]*tv;
}

} // namespace

TecCollision LoadTec(const std::wstring& path) {
    TecCollision t;
    std::vector<uint8_t> f = ReadAll(path);
    auto fail = [&](const char* why) { t.error = why; return t; };
    if (f.size() < 8) return fail("file too small");
    const uint8_t* F = f.data();
    uint32_t countA = rd32(F);
    size_t o = 4;
    if (o + (size_t)countA * 0x44 + 4 > f.size()) return fail("bad A count");
    for (uint32_t i = 0; i < countA; ++i, o += 0x44) t.faces.push_back(ParseFace(F + o));
    uint32_t zones = rd32(F + o); o += 4;
    for (uint32_t z = 0; z < zones; ++z) {
        if (o + 28 > f.size()) return fail("bad zone header");
        uint32_t sub = rd32(F + o + 24); o += 28;
        if (o + (size_t)sub * 0x44 > f.size()) return fail("bad zone faces");
        for (uint32_t i = 0; i < sub; ++i, o += 0x44) t.faces.push_back(ParseFace(F + o));
    }
    size_t sp = o + 2000;
    for (int i = 0; i < 12 && sp + 0x18 <= f.size(); ++i, sp += 0x18) {
        TecSpawn s{};
        for (int k = 0; k < 3; ++k) s.pos[k] = rdf(F + sp + 4*k);
        for (int k = 0; k < 3; ++k) s.dir[k] = rdf(F + sp + 12 + 4*k);
        t.spawns.push_back(s);
    }
    t.ok = true;
    return t;
}

bool TecCollision::GroundY(float x, float z, float yFrom, float stepUp, float* outY) const {
    bool found = false;
    float best = -1e9f;
    for (const TecFace& f : faces) {
        if (f.n[1] <= 0.5f) continue;
        // XZ point-in-triangle (2D cross products)
        float ax = f.v[0][0], az = f.v[0][2];
        float bx = f.v[1][0], bz = f.v[1][2];
        float cx = f.v[2][0], cz = f.v[2][2];
        float c1 = (bx-ax)*(z-az) - (bz-az)*(x-ax);
        float c2 = (cx-bx)*(z-bz) - (cz-bz)*(x-bx);
        float c3 = (ax-cx)*(z-cz) - (az-cz)*(x-cx);
        bool inside = (c1 >= 0 && c2 >= 0 && c3 >= 0) || (c1 <= 0 && c2 <= 0 && c3 <= 0);
        if (!inside) continue;
        float y = -(f.d + f.n[0]*x + f.n[2]*z) / f.n[1];
        if (y <= yFrom + stepUp && y > best) { best = y; found = true; }
    }
    if (found) *outY = best;
    return found;
}

bool TecCollision::ResolveWalls(float pos[3], float r) const {
    bool hit = false;
    for (int iter = 0; iter < 3; ++iter) {
        float c[3] = { pos[0], pos[1] + r + 0.1f, pos[2] };
        bool any = false;
        for (const TecFace& f : faces) {
            if (fabsf(f.n[1]) > 0.6f) continue;   // walls only
            // quick plane reject
            float dist = f.n[0]*c[0] + f.n[1]*c[1] + f.n[2]*c[2] + f.d;
            if (dist > r || dist < -r) continue;
            float q[3];
            ClosestOnTri(f.v[0], f.v[1], f.v[2], c, q);
            float dx = c[0]-q[0], dy = c[1]-q[1], dz = c[2]-q[2];
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 >= r*r || d2 < 1e-9f) continue;
            float d = sqrtf(d2);
            float push = (r - d) / d;
            // push horizontally only (keep the character grounded)
            c[0] += dx * push; c[2] += dz * push;
            any = hit = true;
        }
        pos[0] = c[0]; pos[2] = c[2];
        if (!any) break;
    }
    return hit;
}

} // namespace tj::assets
