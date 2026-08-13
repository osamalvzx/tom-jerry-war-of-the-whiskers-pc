// x87 transcendental helpers. The original code uses the x87 fcos/fsin/fpatan
// instructions; MSVC's library cos()/sin() use different approximations, so for
// closeness to the original we invoke the actual instructions via inline asm (x86).
// NOTE: in MSVC inline asm, ';' starts a comment -- each instruction gets its own line.
#pragma once

namespace tj::game {

inline double x87cos(double x) {
    double r;
    __asm {
        fld  x
        fcos
        fstp r
    }
    return r;
}

inline double x87sin(double x) {
    double r;
    __asm {
        fld  x
        fsin
        fstp r
    }
    return r;
}

inline double x87sqrt(double x) {
    double r;
    __asm {
        fld  x
        fsqrt
        fstp r
    }
    return r;
}

// atan2(y, x) via fpatan: computes atan(st1/st0) with correct quadrant.
inline double x87atan2(double y, double x) {
    double r;
    __asm {
        fld  y
        fld  x
        fpatan
        fstp r
    }
    return r;
}

} // namespace tj::game
