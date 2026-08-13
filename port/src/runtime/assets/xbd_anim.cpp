#include "runtime/assets/xbd_anim.h"
#include <cstdio>
#include <cstring>
#include <cmath>

namespace tj::assets {
namespace {

constexpr float kQuatScale  = 1.0f / 32768.0f;
constexpr float kTransScale = 1.0f / 1024.0f;

// Engine bone-register table (DAT_00181f90): shader constant register of bone i.
constexpr int16_t kRegTable[] = {
    -96,-93,-90,-87,-84,-81,-78,-75,-72,-69,-66,-63,-60,-57,-54,-51,-48,-45,
    -42,-36,-33,-30,-27,-24,-21,-18,-15,-12,-9,-6,-3,0,10,13,16,19,22,25,28,
    31,34,44,47,50,53,56,59,62,65,68,71,74,77,80,83,86,89,92 };

inline uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
inline int16_t  rds16(const uint8_t* p) { int16_t v; memcpy(&v, p, 2); return v; }
inline float    rdf(const uint8_t* p) { float v; memcpy(&v, p, 4); return v; }

struct Mat4 { float m[4][4]; };

Mat4 Identity() {
    Mat4 r{};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
    return r;
}

Mat4 Mul(const Mat4& a, const Mat4& b) {          // row-major a*b (column vectors)
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

Mat4 Translate(float x, float y, float z) {
    Mat4 r = Identity();
    r.m[0][3] = x; r.m[1][3] = y; r.m[2][3] = z;
    return r;
}

// Quaternion (x,y,z,w) to matrix — element-for-element mirror of FUN_0007f140.
Mat4 QuatMat(float x, float y, float z, float w) {
    Mat4 r = Identity();
    float x2 = x + x, y2 = y + y, z2 = z + z;
    r.m[0][0] = 1 - (z2*z + y2*y);
    r.m[0][1] = z2*w + y2*x;
    r.m[0][2] = z2*x - y2*w;
    r.m[1][0] = y2*x - z2*w;
    r.m[1][1] = 1 - (z2*z + x2*x);
    r.m[1][2] = z2*y + x2*w;
    r.m[2][0] = y2*w + z2*x;
    r.m[2][1] = z2*y - x2*w;
    r.m[2][2] = 1 - (y2*y + x2*x);
    return r;
}

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

// One decoded per-joint frame record.
struct FrameRec {
    float q[4];
    bool hasTrans;
    float trans[3];
};

FrameRec DecodeRec(const uint8_t* r) {
    FrameRec o{};
    for (int i = 0; i < 4; ++i) o.q[i] = rds16(r + 2*i) * kQuatScale;
    int16_t mode = rds16(r + 8);
    o.hasTrans = mode < 0;
    if (o.hasTrans) {
        uint16_t mm = (uint16_t)mode;
        if (!(mm & 0x4000)) mm &= 0x7fff;
        o.trans[0] = (int16_t)mm * kTransScale;
        o.trans[1] = rds16(r + 10) * kTransScale;
        o.trans[2] = rds16(r + 12) * kTransScale;
    }
    // mode >= 0 is a scale track (mode/128 etc.); non-unit scale is rare and
    // non-propagating — ignored for now.
    return o;
}

} // namespace

int BoneFromRegister(int reg) {
    for (int i = 0; i < (int)(sizeof kRegTable / sizeof *kRegTable); ++i)
        if (kRegTable[i] == reg) return i;
    return -1;
}

XbdCharacter LoadCharacter(const std::wstring& path) {
    XbdCharacter c;
    std::vector<uint8_t> f = ReadAll(path);
    auto fail = [&](const char* why) { c.error = why; return c; };
    if (f.size() < 0x34 || memcmp(f.data(), "XMF", 3)) return fail("not an XMF container");
    int ver = f[3] - '0';
    uint32_t idsf = (rd32(&f[0x14]) == 0xabcd) ? 0 : rd32(&f[0x18]);
    size_t end = (ver >= 2 ? 0x34 + rd32(&f[0x30]) : 0x30)
               + rd32(&f[0x1c]) + rd32(&f[0x20]) + rd32(&f[0x24]) + rd32(&f[0x28])
               + idsf + rd32(&f[0x2c]);
    if (end + 0x10 > f.size() || memcmp(&f[end], "CXBD", 4)) return fail("no CXBD block");

    uint16_t jointCount = rd16(&f[end + 4]);
    uint16_t animCount  = rd16(&f[end + 6]);
    uint32_t jxbdSize   = rd32(&f[end + 8]);
    uint32_t axbdSize   = rd32(&f[end + 12]);
    size_t jx = end + 0x10;
    if (jx + jxbdSize + axbdSize > f.size() || memcmp(&f[jx], "JXBD", 4))
        return fail("bad JXBD");
    size_t sx = jx + jxbdSize;
    if (memcmp(&f[sx], "SXBD", 4) && memcmp(&f[sx], "AXBD", 4)) return fail("bad SXBD/AXBD");

    c.joints.resize(jointCount);
    for (int j = 0; j < jointCount; ++j) {
        const uint8_t* r = &f[jx + 4 + (size_t)j * 0x38];
        c.joints[j].depthSlot = rd16(r);
        for (int k = 0; k < 3; ++k) c.joints[j].bindPos[k] = rdf(r + 4 + 4*k);
        for (int k = 0; k < 9; ++k) c.joints[j].bindRot[k] = rdf(r + 0x10 + 4*k);
    }

    c.animData.assign(f.begin() + sx, f.begin() + sx + axbdSize);
    c.anims.resize(animCount);
    for (int a = 0; a < animCount; ++a) {
        const uint8_t* r = &c.animData[4 + (size_t)a * 0x10];
        uint16_t keys = rd16(r + 2);
        uint32_t keysOff = rd32(r + 8);
        if (keysOff + (size_t)keys * 0x20 > c.animData.size()) continue;
        c.anims[a].keys.resize(keys);
        for (int k = 0; k < keys; ++k) {
            const uint8_t* kr = &c.animData[keysOff + (size_t)k * 0x20];
            c.anims[a].keys[k].frameOff = rd32(kr);
            for (int i = 0; i < 3; ++i) c.anims[a].keys[k].root[i] = rdf(kr + 4 + 4*i);
        }
    }
    c.ok = true;
    return c;
}

void XbdCharacter::EvaluatePose(int anim, float t, std::vector<Mat34>& bones) const {
    bones.assign(joints.size(), Mat34{{{1,0,0,0},{0,1,0,0},{0,0,1,0}}});
    if (anim < 0 || anim >= (int)anims.size() || anims[anim].keys.empty()) return;
    const XbdAnim& A = anims[anim];
    int n = (int)A.keys.size();
    float tw = fmodf(t, (float)n); if (tw < 0) tw += n;
    int k0 = (int)tw, k1 = (k0 + 1) % n;
    float bl = tw - k0;
    const XbdKey& ka = A.keys[k0];
    const XbdKey& kb = A.keys[k1];
    if (ka.frameOff + joints.size() * 14 > animData.size() ||
        kb.frameOff + joints.size() * 14 > animData.size()) return;

    float root[3];
    for (int i = 0; i < 3; ++i) root[i] = ka.root[i] + (kb.root[i] - ka.root[i]) * bl;

    Mat4 stack[32], stack2[32];
    // Horizontal root motion is cancelled so the character animates in place
    // (the real game advances the entity by it; a viewer wants a stable subject).
    Mat4 G = Translate(0.0f, root[1] - joints[0].bindPos[1], 0.0f);
    Mat4 cur = G, cur2 = G;
    stack[0] = G; stack2[0] = G;
    int top = 0;

    for (size_t j = 0; j < joints.size(); ++j) {
        const XbdJoint& J = joints[j];
        FrameRec ra = DecodeRec(&animData[ka.frameOff + j * 14]);
        FrameRec rb = DecodeRec(&animData[kb.frameOff + j * 14]);
        // blend (nlerp with shortest-path sign fix, mirroring FUN_0007ed70's intent)
        float dot = ra.q[0]*rb.q[0] + ra.q[1]*rb.q[1] + ra.q[2]*rb.q[2] + ra.q[3]*rb.q[3];
        float sgn = dot < 0 ? -1.0f : 1.0f;
        float q[4];
        for (int i = 0; i < 4; ++i) q[i] = ra.q[i] + (sgn * rb.q[i] - ra.q[i]) * bl;
        float ql = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
        if (ql > 1e-6f) for (float& v : q) v /= ql;
        float tr[3] = {0, 0, 0};
        if (ra.hasTrans) {
            for (int i = 0; i < 3; ++i)
                tr[i] = rb.hasTrans ? ra.trans[i] + (rb.trans[i] - ra.trans[i]) * bl
                                    : ra.trans[i];
        }

        int d = J.depthSlot;
        if (top < d) {
            ++top;                        // game pushes to slot top+1 (base stack+0x40)
            if (top < 32) { stack[top] = cur; stack2[top] = cur2; }
        } else {
            cur = stack[d]; cur2 = stack2[d]; top = d;
        }
        cur = Mul(cur, Translate(J.bindPos[0] + tr[0], J.bindPos[1] + tr[1],
                                 J.bindPos[2] + tr[2]));
        cur = Mul(cur, QuatMat(q[0], q[1], q[2], q[3]));
        cur = Mul(cur, Translate(-J.bindPos[0], -J.bindPos[1], -J.bindPos[2]));
        for (int r = 0; r < 3; ++r)
            for (int cc = 0; cc < 4; ++cc) bones[j].m[r][cc] = cur.m[r][cc];
        cur2 = cur;
    }
}

} // namespace tj::assets
