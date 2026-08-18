// THE APP<->GAME SHARED-MEMORY PROTOCOL — Stage 6's subprocess architecture (session 28).
//
// WHY A SUBPROCESS: the engine cannot share an address space with Android's ART runtime —
// ART's `dalvik-main space` reservation owns 0x02000000..0x12000000, and the guest's
// contiguous (GPU) memory MUST live in the low 256 MB (the game masks GPU pointers to 28
// bits and uses the 0x80000000|phys WC alias). So the game runs in an ART-FREE exec'd
// subprocess (the pristine-low-memory shape Gate S5c proved on this very device), and the
// app process — which owns the window, GLES, and input — talks to it through ONE shared
// memfd region described here.
//
// WHY A COMMAND STREAM (not a framebuffer): the subprocess renders NOTHING. It runs the
// bridge's CPU-side semantic work and emits the resulting tj::gfx::Device calls — plain
// textured triangle lists — into the ring; the app replays them into the real GLES3 device
// on the window surface. Gate S5d proved this stream is a complete, deterministic
// description of every frame (draws+verts+textures+state, byte-identical cross-arch), and
// keeping ALL GPU work in the app process sidesteps every "can an exec'd child use EGL"
// unknown. Bandwidth: worst measured frame ≈ 730 draws / 78K verts ≈ 2.5 MB → ~150 MB/s at
// 60 fps through memcpy — noise.
//
// Region layout:  [ Header page (4 KB) ][ command ring (kRingBytes) ]
// Roles: the GAME is the ring's single producer; the APP is its single consumer. The pad
// block flows the other way (app writes, game reads) under a seqlock. Cross-process
// std::atomic is valid here: arm64 lock-free atomics are address-free.
#pragma once
#include <atomic>
#include <cstdint>

namespace tj::ipc {

constexpr uint32_t kMagic     = 0x544A4642;   // 'TJFB'
constexpr uint32_t kVersion   = 1;
constexpr uint32_t kRingBytes = 64u << 20;    // >> largest single op (a 4 MB texture) + slack
constexpr uint32_t kHeaderBytes = 4096;
constexpr uint32_t kRegionBytes = kHeaderBytes + kRingBytes;

// The Xbox pad state the app forwards (same fields as tj::input::XboxGamepad, kept as a
// separate POD so the protocol header depends on nothing).
struct PadShm {
    uint16_t buttons;        // 0x01 up 0x02 down 0x04 left 0x08 right 0x10 start 0x20 back
    uint8_t  analog[8];      // A,B,X,Y,Black,White,LT,RT (0..255)
    int16_t  lx, ly, rx, ry;
};

struct Header {
    uint32_t magic, version;
    uint32_t surfW, surfH;                    // app -> game, set BEFORE spawn (render res)
    std::atomic<uint32_t> quit;               // app -> game: exit at the next safe point
    std::atomic<uint32_t> gameReady;          // game -> app: Device::Create ran
    // Pad seqlock: writer bumps to odd, writes, bumps to even; reader retries on odd/change.
    std::atomic<uint32_t> padSeq;
    PadShm pad;
    // SPSC ring cursors — MONOTONIC byte counts (offset = value % kRingBytes).
    std::atomic<uint64_t> head;               // producer (game) publish cursor
    std::atomic<uint64_t> tail;               // consumer (app) read cursor
    std::atomic<uint32_t> framesProduced;     // game Present count (pacing)
    std::atomic<uint32_t> framesConsumed;     // app Present count
    uint32_t ringBytes;                       // == kRingBytes (cross-checked at attach)
};
static_assert(sizeof(Header) <= kHeaderBytes, "header must fit its page");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "cross-process atomics");

// Command encoding: [CmdHdr][payload], total size 8-byte aligned so the ring tail-gap is
// always 0 or >= sizeof(CmdHdr) and an OP_WRAP marker always fits. OP_WRAP's `bytes` is the
// number of padding bytes to skip to reach the ring start.
enum Op : uint32_t {
    OP_CLEAR = 1,           // u32 flags, u32 argb, f32 z, u32 stencil
    OP_SET_TRANSFORM,       // f32[16]
    OP_DRAW_PC,             // u32 vcount; VertexPC[vcount]
    OP_DRAW_PTC,            // u32 vcount, u32 icount; VertexPTC[vcount]; u16[icount]
    OP_DRAW_SHINY,          // u32 vcount, u32 icount, i32 t0..t3; VertexPT2C[vcount]; u16[icount]
    OP_CREATE_TEX,          // i32 handle, u32 w, u32 h; u32 pixels[w*h]
    OP_UPDATE_TEX,          // i32 handle, u32 w, u32 h, u32 genMips; u32 pixels[w*h]
    OP_DESTROY_TEX,         // i32 handle
    OP_CREATE_RT,           // i32 handle, u32 w, u32 h
    OP_SET_RT,              // i32 handle
    OP_SET_RT_BACKBUFFER,   // (none)
    OP_CREATE_CAPTURE,      // i32 handle
    OP_COPY_BACKBUFFER,     // i32 handle
    OP_SET_TEXTURE,         // i32 handle (-1 = none)
    OP_SET_UVCLAMP,         // u32 clampU, u32 clampV
    OP_SET_DEPTH,           // u32 enable
    OP_SET_ALPHA,           // u32 enable
    OP_SET_BLEND,           // u32 mode, u32 depthWrite
    OP_PRESENT,             // (none) — frame boundary
    OP_WRAP = 0xFFFFFFFFu,  // skip `bytes` of tail-gap padding, continue at ring start
};
struct CmdHdr { uint32_t op, bytes; };        // bytes = payload only (excl. this header)
static_assert(sizeof(CmdHdr) == 8);

inline uint32_t CmdTotal(uint32_t payloadBytes) {   // header + payload, 8-aligned
    return (uint32_t)sizeof(CmdHdr) + ((payloadBytes + 7u) & ~7u);
}

} // namespace tj::ipc
