/*============================================================================

platform.h for the vendored Berkeley SoftFloat-3e extFloat80 subset.

This is the ONLY non-upstream file in this directory (every build/ directory
in the upstream repo provides its own platform.h; this is ours, for MSVC x86
and any other little-endian compiler). Everything else here is byte-identical
to github.com/ucb-bar/berkeley-softfloat-3 (BSD-3-Clause, see COPYING.txt).

Deliberately NO result-changing options: LITTLEENDIAN + INLINE only.
SOFTFLOAT_FAST_INT64 / INLINE_LEVEL / FAST_DIV* come from the build system
(see port/CMakeLists.txt, target tj_softfloat) -- none of them change results,
only speed, so the engine stays bit-deterministic across compilers.

=============================================================================*/

#ifndef tj_softfloat_platform_h
#define tj_softfloat_platform_h 1

/*----------------------------------------------------------------------------
| The engine targets are little-endian only (x86-32 now, ARM Android later).
*----------------------------------------------------------------------------*/
#define LITTLEENDIAN 1

/*----------------------------------------------------------------------------
| 'static' keeps the INLINE_LEVEL definitions in primitives.h internal to each
| translation unit (the upstream GCC builds use gnu89 'extern inline'; MSVC's
| C compiler has no equivalent, and 'static inline' is portable C99/MSVC2015+).
| The out-of-line s_*.c fallbacks still provide extern definitions.
*----------------------------------------------------------------------------*/
#define INLINE static inline

#endif
