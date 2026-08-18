# Stage 5 (port/ANDROID_PLAN.md §4): the headless aarch64 cross of the execution
# engine. This build contains NO platform layer — it exists so the interpreter and the
# EXACT soft-float unit compile and (on a device, later) run headless for Gate S5c
# (ARM EXACT detlog == Windows EXACT detlog). The Windows build is untouched by
# anything in this directory.
#
# Configure (NDK + SDK CMake, see PROJECT_STATUS session 25):
#   <sdk>\cmake\3.22.1\bin\cmake.exe -S port/android -B port/build-arm64 -G Ninja ^
#     -DCMAKE_MAKE_PROGRAM=<sdk>\cmake\3.22.1\bin\ninja.exe ^
#     -DCMAKE_TOOLCHAIN_FILE=<ndk>\build\cmake\android.toolchain.cmake ^
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release
cmake_minimum_required(VERSION 3.22)
project(tj_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(ENG ${CMAKE_CURRENT_SOURCE_DIR}/../src/engine)

# --- Berkeley SoftFloat-3e (vendored; BSD) — pure C, compiles anywhere little-endian.
# The PUBLIC defines are LOAD-BEARING (softfloat.h API shape + extFloat80M field
# order); see the Windows CMakeLists for the full rationale. Do not make them PRIVATE.
file(GLOB SOFTFLOAT_SRC ${ENG}/softfloat/*.c)
add_library(tj_softfloat STATIC ${SOFTFLOAT_SRC})
target_include_directories(tj_softfloat PUBLIC ${ENG}/softfloat)
target_compile_definitions(tj_softfloat
    PUBLIC  SOFTFLOAT_FAST_INT64 LITTLEENDIAN=1
    PRIVATE INLINE_LEVEL=5 SOFTFLOAT_FAST_DIV32TO16 SOFTFLOAT_FAST_DIV64TO32)

# --- the execution engine, ARM subset: the interpreter + the EXACT x87 unit.
# x87_native.cpp (host-FPU thunks) and eng_gate.cpp (naked-asm gate) are x86-host
# machinery by design — on ARM the EXACT unit carries the x87 and the call boundary
# becomes Engine::Call (the Stage-5 dispatch-table work).
add_library(tj_engine STATIC
    ${ENG}/x86_interp.cpp
    ${ENG}/x87_exact.cpp
    ${ENG}/fpu_host.cpp
)
target_include_directories(tj_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(tj_engine PRIVATE tj_softfloat)

# --- on-device smoke (run via adb the moment a phone is attached):
#   adb push port/build-arm64/softfloat_smoke /data/local/tmp/ && adb shell /data/local/tmp/softfloat_smoke
# The 8 hard-coded x87 bit patterns must give the SAME output as the Windows build —
# the first physical evidence for Gate S5c's premise (same soft-float code, any host).
add_executable(softfloat_smoke ${CMAKE_CURRENT_SOURCE_DIR}/../tests/softfloat_smoke.cpp)
target_link_libraries(softfloat_smoke PRIVATE tj_softfloat)

# The interpreter smoke: a hand-assembled x86 blob (integer loop + EXACT-unit fsqrt)
# through Run(), guest window mapped below 4 GB (Gate S5a's premise on real hardware).
# The same test builds on Windows (target eng_smoke there too) — outputs must match
# line for line: a miniature Gate S5c.
add_executable(eng_smoke ${CMAKE_CURRENT_SOURCE_DIR}/../tests/eng_smoke.cpp)
target_link_libraries(eng_smoke PRIVATE tj_engine tj_softfloat)

# On-device interpreted-x86 speed probe (the Stage-4 JIT decision's datapoint).
add_executable(eng_bench ${CMAKE_CURRENT_SOURCE_DIR}/../tests/eng_bench.cpp)
target_link_libraries(eng_bench PRIVATE tj_engine tj_softfloat)

# Stage-5 CALL-BOUNDARY smoke: the FIRST hybrid-side code in the ARM build — the
# dispatch registry + typed shims (tag-driven here) + Engine::Call marshaling, with a
# synthetic-handle key space and the virtual host FPU. Must print line-identically to
# the Windows eng_call_smoke.
add_executable(eng_call_smoke
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/eng_call_smoke.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/hybrid/dispatch.cpp)
target_link_libraries(eng_call_smoke PRIVATE tj_engine tj_softfloat)

# --- tj_headless: THE GAME LAYER, headless (Stage 5 §4; Gates S5a..S5d) -------------
# The whole hybrid layer + game bring-up against the null gfx recorder and the null
# audio backend, engine armed from process start. TJ_DETLOG from this binary vs the
# Windows EXACT leg is Gate S5c; TJ_DRAWLOG streams are Gate S5d.
set(HYB ${CMAKE_CURRENT_SOURCE_DIR}/../src/hybrid)
set(RT  ${CMAKE_CURRENT_SOURCE_DIR}/../src/runtime)
# The whole hybrid game layer + game bring-up, SHARED by the headless binary and the Android
# app. The two differ only in the ENTRY (headless_main vs android/native_main), the GFX
# BACKEND (null_gfx recorder vs gles_gfx), and (app only) native_app_glue. Everything else —
# including hybrid_boot.cpp, the boot sequence Gate S5c proved — is identical.
set(HYB_GAME_SRC
    ${HYB}/host_compat.cpp
    ${HYB}/hybrid_boot.cpp
    ${HYB}/dispatch.cpp
    ${HYB}/xdk_patch.cpp
    ${HYB}/kernel.cpp
    ${HYB}/crt_glue.cpp
    ${HYB}/fs_fixup.cpp
    ${HYB}/xbe_image.cpp
    ${HYB}/xcrypt.cpp
    ${HYB}/symbolize.cpp
    ${HYB}/file_io.cpp
    ${HYB}/startup.cpp
    ${HYB}/engine_mode.cpp
    ${HYB}/net_sync.cpp
    ${HYB}/net_lan_null.cpp
    ${HYB}/lan_match.cpp
    ${HYB}/lan_ui.cpp
    ${HYB}/fe_menu.cpp
    ${HYB}/audio_ui.cpp
    ${HYB}/meat_rush.cpp
    ${HYB}/meat_ui.cpp
    ${HYB}/meat_menu.cpp
    ${HYB}/dsound_bridge.cpp
    ${HYB}/d3d8_bridge.cpp
    ${HYB}/prof.cpp
    ${RT}/snd/null_snd.cpp
    ${RT}/snd/xadpcm.cpp
    ${RT}/snd/wav_dump.cpp
    ${RT}/assets/xmf_texture.cpp)
add_executable(tj_headless
    ${HYB}/headless_main.cpp
    ${RT}/gfx/null_gfx.cpp
    ${HYB_GAME_SRC})
target_include_directories(tj_headless PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
# DYNAMIC PIE, deliberately — the shape PROVEN on the device (eng_call_smoke): Android
# loads PIE high, far above the guest window, and the kernel honors
# MAP_FIXED_NOREPLACE, so the fixed sub-4GB layout maps cleanly.
# ⚠ Do NOT link -static here: the static base (0x200000) sits INSIDE the guest window
# and the image-window map wipes the program's own code (measured under qemu).

# --- tj_headless_static: the qemu-user leg (a LOCAL aarch64 S5c rig on this PC).
# Same sources; static (qemu can't load bionic's dynamic linker) with the image based
# at 0xF0000000 — above the whole guest layout, below 4 GB. Run it as:
#   qemu-aarch64-static -B 0x2000000000 tj_headless_static <xbe>   (guest_base shifts
# every guest mapping clear of qemu's own low host memory; qemu 8.2 emulates
# MAP_FIXED_NOREPLACE, so no TJ_MMAP_TRUST_FIXED needed).
add_executable(tj_headless_static $<TARGET_PROPERTY:tj_headless,SOURCES>)
target_include_directories(tj_headless_static PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_options(tj_headless_static PRIVATE -static "-Wl,--image-base=0xF0000000")
target_link_libraries(tj_headless_static PRIVATE tj_engine tj_softfloat)
target_link_libraries(tj_headless PRIVATE tj_engine tj_softfloat)

# --- Stage 6: the GLES3 gfx backend (runtime/gfx/d3d8.h reimplemented on GLES3). Built as a
# library here so it compiles/links against the NDK's GLES3/EGL headers on its own, ahead of
# the full APK harness. It replaces null_gfx.cpp in the shipping app (never both).
add_library(tj_gles STATIC ${RT}/gfx/gles_gfx.cpp)
target_include_directories(tj_gles PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(tj_gles PRIVATE GLESv3 EGL android)

# --- Stage 6: libtjapp.so — THE GAME ON THE PHONE. The full hybrid game layer (HYB_GAME_SRC,
# the exact code Gate S5c proved) + the GLES3 backend (gles_gfx) + the Android entry
# (native_main: game boot thread + gamepad/touch input on port 0). Packaged by
# port/tools/build_apk.ps1. Renders the real game to the device surface.
add_library(tjapp SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/android/native_main.cpp
    ${RT}/gfx/gles_gfx.cpp
    ${HYB_GAME_SRC}
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(tjapp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${ANDROID_NDK}/sources/android/native_app_glue)
target_link_libraries(tjapp PRIVATE tj_engine tj_softfloat GLESv3 EGL android log)
# native_app_glue provides ANativeActivity_onCreate; the linker must keep it (nothing in the
# .so references it directly — the framework loads it by name).
target_link_options(tjapp PRIVATE -u ANativeActivity_onCreate)
