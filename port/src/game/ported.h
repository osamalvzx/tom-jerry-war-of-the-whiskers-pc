// Ported game functions (decompile-first). Each keeps the original's calling
// convention and entry virtual address (documented per function) so it can be
// swapped in for the original one-for-one and validated against it byte-for-byte.
//
// Stage: raw-offset struct access. Structs are typed/named incrementally later.
#pragma once
#include <cstdint>

namespace tj::game {

// --- field-init / reset helpers (operate on an entity/object at `self`) ---
void     __fastcall F_00011000(int self);  // zero 5 dwords [+4..+0x14]
void     __fastcall F_00011020(int self);  // set colour(?) fields to 0x01010101
uint32_t __fastcall F_00016e80(int self);  // (bytes[+1]||bytes[+2]) != 0
void     __fastcall F_0002ea80(int self);  // clear anim/state fields
void     __fastcall F_0002f680(int self);  // per-frame value approach (physics/anim)
uint8_t  __fastcall F_0003f7d0(int self);  // state byte[+0x5510] in {0x17,0x18,0x2d}
uint8_t  __fastcall F_00037b00(int self);  // state byte[+0x4ca8] in (2,6)
void     __fastcall F_00014ea0(int self);  // init small record
void     __fastcall F_00019990(int self);  // init transform (scale=1.0)
void     __fastcall F_00019bd0(int self);  // snapshot transform to shadow fields
void     __fastcall F_000461e0(int self);  // clear 3 fields
void     __fastcall F_00048610(int self);  // clamp-decrement short[+0x120]
void     __fastcall F_0005a010(int self);  // clamp-decrement int[+0x758]
uint32_t __fastcall F_00072670(int self);  // ++(*(int*)(self+4)+4); return it
uint32_t            F_00088e90(int self);  // (self? *(u32*)(self+0xc) : 0)  [cdecl]

// --- 3D math primitives (character-update subsystem foundation) [cdecl] ---
float F_000111f0(const float* v);                          // |v|  (vector length)
void  F_000112f0(float* out, const float* mat, const float* v);  // mat4x4 * point (w-divide)
void  F_000112b0(float* out, const float* a, const float* b);    // a x b (cross product)

// --- RNG (Mersenne-Twister-style) + helpers ---
uint32_t F_00011f20();                     // next raw random (15-bit), advances state [cdecl]
float    __fastcall F_00012050(int unused);// F_00011f20() scaled to a float
void     F_00011220(float* v);             // normalize v in place [cdecl]

// --- character animation ---
void __fastcall F_0002eab0(int self);      // build/apply the animated skew matrix @ self+0x25c
void F_000117b0(float* out, const float* a, const float* b);  // 4x4 matrix multiply [cdecl]

// --- character-update mid layer ---
void __fastcall F_0002f5b0(int self);      // per-limb nearest-target distance accumulation
void F_0002e110(float* out, const float* eye, const float* target, const float* up); // look-at matrix [cdecl]
void __fastcall F_0002e7f0(int self);      // impact/camera shake (RNG + sin/cos)

// --- character-update, world-state dependent (read the global world ptr @ 0x15c470c) ---
uint8_t __fastcall F_0002e6d0(int self);   // on-screen bounds test for two points
void    __fastcall F_0002eca0(int self);   // compute aim/centre point from limbs
uint32_t F_0002e3e0(int self, int boneIndex);  // per-bone screen-region class + camera pan (0/1/2)
void __fastcall F_0002f0f0(int self);      // two-limb spread -> lateral impulse (self+0x208/+0x218)

// --- character-update dispatcher ---
void __fastcall F_0002fce0(int self);      // full per-frame movement/AI update

// --- batch 8 (porting factory) ---
void    __fastcall F_000241c0(int self);   // copy 12 option bytes (self+0x60c8..) into the level object
void    __fastcall F_00037b20(int self);   // derive facing/side class +0x928 from state byte +0x4ca8
void    __fastcall F_00030ea0(int self);   // build + RNG-shuffle index array @+0x1d90 (count @+0x1d8d)
void    __fastcall F_00046150(int self);   // step a 0..100 gauge at +0xc from world char stat
void    __fastcall F_00071fd0(int self, float dt); // countdown timer +0x24 w/ event bits 0x10000/0x20000
uint8_t __fastcall F_00030360(int self);   // extents-vs-bounds test (world mode 2 only)
void    __fastcall F_00030f30(int self);   // build + RNG-shuffle index array @+0x1d95 (count @+0x1da2)
uint8_t __fastcall F_00047c20(const float* p); // point inside any flagged world AABB (flag bit 2)
void    __fastcall F_00045a30(int self, float p2); // advance meter +0x18, snap to 100.0f
uint8_t __fastcall F_00047cb0(const float* p); // point inside any flagged world AABB (flag bit 1)
uint8_t __fastcall F_000380f0(int self);   // in-range test vs target entity (dist < radii sum)
uint8_t __fastcall F_00038670(int self, char p2); // grab/interact eligibility from state + props table

// --- batch 9 (porting factory) ---
uint8_t __fastcall F_0001b0c0(int self, uint8_t p2);       // state-transition lookup (selector byte +0x721)
void    __fastcall F_00052d00(int self);                   // camera preset init (mode 0 vs 7)
void    __fastcall F_00013310(int self);                   // object init (vtable + tables)
void    __fastcall F_000302e0(int self);                   // collect tagged entries into list @+0x1fe8
void    __fastcall F_000596f0(int self);                   // init + 180-entry sin/cos table
void    __fastcall F_00019c60(int self);                   // vec4 lerp driver (elapsed/duration)
void    __fastcall F_00031250(int self, uint8_t idx);      // release record owned by player idx
uint8_t __fastcall F_00034ab0(int self);                   // mode/state pair test (world mode byte)
void    __fastcall F_0003d170(int self, int srcObj);       // bind record to character sub-object
void    __fastcall F_00043e50(int self, uint8_t idx);      // bind to world block + snapshot margin box
void    __fastcall F_000119f0(int self);                   // renormalize 3 basis rows of a 4x4 (calls 11220)
uint8_t __fastcall F_00047d40(int self, const float* p);   // point in own AABB (or any, in modes 6/7)
void    __fastcall F_00062a70(int self, uint8_t idx);      // init 5 particle records (RNG + world block)
void    __fastcall F_000113a0(float* out, float* dir);     // build orthonormal basis from direction

// --- batch 10 (porting factory) ---
void    __fastcall F_00011750(uint32_t* dst, uint32_t* src); // expand 3x3 matrix into 4x4 (identity row/col)
void    __fastcall F_000521f0(int self);                   // reset per-player status arrays
void    __fastcall F_000707e0(int self);                   // zero list-header object
void    __fastcall F_000320f0(int self);                   // reset 0x174-byte entry states (2->4, keep 3)
void    __fastcall F_00070e70(int self, int* node);        // RB-tree right-rotate (std::_Tree)
void    __fastcall F_00070f30(int* it);                    // RB-tree iterator-- (predecessor)
void    __fastcall F_00070f90(int self, int node);         // RB-tree left-rotate (std::_Tree)
uint32_t __fastcall F_00015040(int self, const char* name); // case-insensitive name lookup in ptr table
void    __fastcall F_00070ed0(int* it);                    // RB-tree iterator++ (successor)
void    __fastcall F_0001b290(int self);                   // roster byte via table 0x16a1f8 into level object
void    __fastcall F_00045b70(int self);                   // decay+clamp meter, lerp follower (world-gated)
uint8_t __fastcall F_0002e0b0(char* buf, uint32_t t);      // format countdown seconds (sprintf "%d"/"%c")
void    __fastcall F_000135f0(int self, uint8_t flag);     // broadcast virtual calls over child array
void    __fastcall F_00011c40(int self, float* out, const float* m); // 3x3 matrix multiply (by-value orig)

// --- batch 11 (porting factory) ---
void    __fastcall F_0003fd60(int self);                   // zero 12 velocity/accel fields
uint32_t __fastcall F_0002d410(int self);                  // reset two 6x transform arrays; return self
void    __fastcall F_000386f0(int self, char p2);          // set/clear flag bit 0x20000 (guarded by type!=9)
void    __fastcall F_000401e0(int self);                   // integrate predicted position (+velocity*dt)
void    __fastcall F_00046590(int self);                   // gather flagged child ptrs into output list
uint8_t __fastcall F_00072130(int self, int* outCount);    // count leading busy elements; return more-follow
uint8_t __fastcall F_00059960(int self, uint8_t p2);       // rotate roster slot; return next index
uint8_t __fastcall F_000465e0(int self);                   // partner-alive test (flags + health > 0)
void    __fastcall F_00014ff0(int self);                   // rebuild child array (release+create+init, virtual)
void    __fastcall F_00014fb0(int self);                   // destroy child array (virtual dtor calls)
float   __fastcall F_00045b20(int self, uint16_t p2);      // scale value by world stat (returns float)
uint8_t __fastcall F_0002db80(uint8_t row, char match);    // scan roster table 0x16a148 for a byte
float   F_00011260(float* v);                              // normalize v in place, return |v| [cdecl]
uint8_t __fastcall F_00031170(const float* p);             // point inside any active world circle

// --- batch 12 (porting factory) ---
void    __fastcall F_00046430(int self);                   // init record (0xff markers + zeroed vecs)
uint8_t __fastcall F_000151e0(int self);                   // any child has flag byte +0xc set
void    __fastcall F_00045430(int self, uint8_t p2);       // set state byte +3 if eligible (<7, gated)
void    __fastcall F_00038850(int self);                   // derive can-move flag +0x703c from state
void    __fastcall F_0001bad0(int self, uint8_t p2);       // reset 11 slot fields, select one (=0x80)
void    __fastcall F_0003fbf0(int self, uint32_t* src);    // copy vec3 into +0x6fc0 (world-gated)
void    __fastcall F_0002f0c0(int self, uint8_t p2);       // release timeslot p2 (decrement counters)
uint32_t __fastcall F_00013430(int self, uint32_t mask);   // count children whose flags & mask
uint8_t __fastcall F_0006fd20(int self, char key);         // find key in table +0xc; return +0x15 idx or 0xc7
void    __fastcall F_00045af0(int self);                   // advance+clamp meter to 100, mirror to +0x2c
void    __fastcall F_00015420(int self);                   // build 13 children + patch from global table
void    __fastcall F_00015180(int self);                   // advance one child (virtual call + world tick)
uint8_t __fastcall F_00047b30(float* p, float* out);       // first box containing p; write box point to out
float   F_00011bd0(const float* a, const float* b);        // distance between two 3D points [cdecl]

// --- batch 13 (porting factory) ---
void    __fastcall F_0001a540(int self);                   // init record (0x80 vecs, 0x7f caps, flags)
void    __fastcall F_00013560(int self, uint8_t p2);       // broadcast byte to children +0xc0
uint8_t __fastcall F_00043f60(int self, uint8_t* out);     // ready check; fetch child stat to *out
void    __fastcall F_00013530(int self, uint8_t p2);       // broadcast byte to children +0xbf
void    __fastcall F_00011ba0(float* out, float angle);    // out = (cos, 0, sin) of scaled angle [cdecl]
uint8_t __fastcall F_00034a80(int self, char key);         // count entries matching key (50 x 0x1414)
uint32_t __fastcall F_000488c0(int self, int src);         // bump per-type counter; return its address
void    __fastcall F_000302b0(int self);                   // decay+clamp value at +4 to -5.0f [stdcall]
void    __fastcall F_00046120(int self, uint16_t p2);      // add p2 to meter +0x24, clamp to 100
uint8_t __fastcall F_000133d0(int self);                   // AND-reduce virtual predicate over children
void    __fastcall F_0001a5a0(int self);                   // two sub-object virtual calls (gated +0x48)
void    __fastcall F_0001da80(int self);                   // release 22 child slots (virtual dtor +1)
void    __fastcall F_00033e00(int self, uint32_t p2);      // free timeslot p2 (calls 31250 + 2f0c0)
uint8_t __fastcall F_00047dd0(const float* p);             // nearest flagged world box index to p

// --- batch 14 (porting factory) ---
void    __fastcall F_00040170(int self, uint32_t* src);    // copy vec3 into +0x6f78
void    __fastcall F_0003d600(int self);                   // reset 5 weights, set selected +0x32 to 1.0
void    __fastcall F_000322c0(int self, uint8_t idx);      // record state 2->4 in 0x174 array
void    __fastcall F_00067220(int self);                   // ctor (vtable 0xf2034 + 4 fields)
void    __fastcall F_00067620(int self);                   // ctor (vtable 0xf20fc + 4 fields)
void    __fastcall F_0001cff0(int self);                   // ctor (vtable 0xeef14, zero rest)
void    __fastcall F_00066f20(int self);                   // set count bytes @+0x54..
uint8_t __fastcall F_000720c0(int* self);                  // entity busy test (flags +0x44/+0x48)
void    __fastcall F_00036790(uint32_t* out);              // copy global vec3 (0x15c471c..)
uint32_t __fastcall F_00014f80(int self);                  // ctor: vtable 0xee6e0 + zero 250 dwords
uint32_t __fastcall F_0002d9e0(uint8_t row);               // roster table 0x16a148 lookup (world)
uint32_t __fastcall F_0002d9b0(uint8_t row);               // roster table 0x16a14a lookup (world)
float   __fastcall F_00011aa0(float* v);                   // 2D direction -> angle/quadrant [cdecl]
uint8_t __fastcall F_0006b170(float* shape, float* p);     // point in oriented/AABB shape test

// --- batch 15 (porting factory) ---
void    __fastcall F_00045830(int self);                   // init record (0xff markers, zeroed fields)
uint8_t __fastcall F_00043f40(int self);                   // ready-state check (byte +0x3c in {2,3,4})
void    __fastcall F_00019d30(int self, uint32_t* src);    // copy 4 dwords into +0x50
void    __fastcall F_00032250(int self, uint8_t idx);      // record state 5->0 in 0x174 array
void    __fastcall F_00070e30(int self);                   // list walk to first flagged node (via +8)
void    __fastcall F_00072070(int self, int* src);         // store pair + refcount++ on entity
uint32_t __fastcall F_00070cd0(int self);                  // find first empty pair slot (or -1)
void    __fastcall F_0001d010(int self);                   // ctor (vtable 0xeef14, zero rest)
void    __fastcall F_0003d630(int self, uint8_t p2);       // set state from table 0x174c44, reset on change
uint32_t __fastcall F_0001e120(int self);                  // derived ctor (base + 4x 19990, vtable 0xef01c)
uint32_t __fastcall F_0001d2c0(int self);                  // walk list, virtual call each node; return 4
void    __fastcall F_00070e50(int self);                   // list walk to first flagged node (via +0)
void    __fastcall F_00038080(int self, float p2);         // steer angle handle by p2 (calls 11aa0/11ba0)
void    __fastcall F_00037fa0(int self);                   // turn toward target angle (calls 11aa0/11ba0)

} // namespace tj::game
