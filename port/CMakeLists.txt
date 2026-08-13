cmake_minimum_required(VERSION 3.20)
project(TomJerryWOW LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The original game is a 32-bit x86 binary. Structures decompiled from it assume
# 4-byte pointers, so the port is built as x86 during the incremental porting stage.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(WARNING "Configure with the x86 (Win32) toolchain: cmake -A Win32")
endif()

add_executable(TomJerryWOW WIN32
    src/shell/main.cpp
    src/shell/config.cpp
    src/shell/display.cpp
    src/runtime/input/xinput_pad.cpp
)

target_include_directories(TomJerryWOW PRIVATE src)
target_compile_definitions(TomJerryWOW PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(TomJerryWOW PRIVATE
    d3d11 dxgi dxguid
    xinput          # XInput 1.4 (Win8+); swap for xinput9_1_0 for broader compat
    user32 gdi32
)

set_target_properties(TomJerryWOW PROPERTIES
    VS_DPI_AWARE "PerMonitor"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# --- Native runtime (Xbox platform replacement) -----------------------------
add_library(tj_runtime STATIC
    src/runtime/kernel/xboxkrnl.cpp
    src/runtime/gfx/d3d8.cpp
    src/runtime/engine/sys_video.cpp
    src/runtime/assets/xmf_texture.cpp
    src/runtime/assets/xmf_mesh.cpp
    src/runtime/assets/xbd_anim.cpp
    src/runtime/assets/tec_collision.cpp
    src/runtime/input/xinput_pad.cpp
    src/runtime/snd/xadpcm.cpp
    src/runtime/snd/xaudio_backend.cpp
    src/runtime/snd/wav_dump.cpp
)
target_include_directories(tj_runtime PUBLIC src)
target_compile_definitions(tj_runtime PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# xaudio2 = XAudio2.9 (Windows 10+ inbox, no redist); ole32 for the CoInitializeEx fallback.
target_link_libraries(tj_runtime PUBLIC d3d11 dxgi dxguid d3dcompiler xinput user32 xaudio2 ole32)

add_executable(runtime_demo WIN32 src/runtime/runtime_demo.cpp)
target_link_libraries(runtime_demo PRIVATE tj_runtime)
target_compile_definitions(runtime_demo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(runtime_demo PROPERTIES VS_DPI_AWARE "PerMonitor"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(sys_init_demo WIN32 src/runtime/sys_init_demo.cpp)
target_link_libraries(sys_init_demo PRIVATE tj_runtime)
target_compile_definitions(sys_init_demo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(sys_init_demo PROPERTIES VS_DPI_AWARE "PerMonitor"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(cube_demo WIN32 src/runtime/cube_demo.cpp)
target_link_libraries(cube_demo PRIVATE tj_runtime)
target_compile_definitions(cube_demo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(cube_demo PROPERTIES VS_DPI_AWARE "PerMonitor"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(level_demo WIN32 src/runtime/level_demo.cpp)
target_link_libraries(level_demo PRIVATE tj_runtime)
target_compile_definitions(level_demo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(level_demo PROPERTIES VS_DPI_AWARE "PerMonitor"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(play_demo WIN32 src/runtime/play_demo.cpp)
target_link_libraries(play_demo PRIVATE tj_runtime)
target_compile_definitions(play_demo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(play_demo PROPERTIES VS_DPI_AWARE "PerMonitor"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(bg_demo WIN32 src/runtime/bg_demo.cpp)
target_link_libraries(bg_demo PRIVATE tj_runtime)
target_compile_definitions(bg_demo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(bg_demo PROPERTIES VS_DPI_AWARE "PerMonitor"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- Ported game code + differential correctness test -----------------------
add_library(tj_game STATIC
    src/game/data_image.cpp
    src/game/ported.cpp
)
target_include_directories(tj_game PUBLIC src)
# The original code uses x87 float math; build the ported math with x87 too so
# multiply-accumulate chains keep identical intermediate precision (byte-exact diff).
target_compile_options(tj_game PRIVATE /arch:IA32 /fp:precise)

# --- Hybrid XBE bring-up (run the original x86 game code in-process) ---------
# tj_hybrid.dll: based HIGH (above the XBE image). Maps default.xbe at real VAs,
# wires the kernel thunks + XDK bridges, and drives the game's startup.
add_library(tj_hybrid SHARED
    src/hybrid/xbe_image.cpp
    src/hybrid/kernel.cpp
    src/hybrid/symbolize.cpp
    src/hybrid/xdk_patch.cpp
    src/hybrid/crt_glue.cpp
    src/hybrid/fs_fixup.cpp
    src/hybrid/d3d8_bridge.cpp
    src/hybrid/dsound_bridge.cpp
    src/hybrid/fe_menu.cpp
    src/hybrid/audio_ui.cpp
    src/hybrid/xcrypt.cpp
    src/hybrid/net_sync.cpp
    src/hybrid/net_lan.cpp
    src/hybrid/lan_match.cpp
    src/hybrid/lan_ui.cpp
    src/hybrid/meat_rush.cpp
    src/hybrid/meat_ui.cpp
    src/hybrid/meat_menu.cpp
    src/hybrid/file_io.cpp
    src/hybrid/startup.cpp
    src/hybrid/hybrid_run.cpp
)
target_include_directories(tj_hybrid PRIVATE src)
target_compile_definitions(tj_hybrid PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(tj_hybrid PRIVATE tj_runtime tj_game)
target_link_options(tj_hybrid PRIVATE /BASE:0x30000000)
set_target_properties(tj_hybrid PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# tj_loader.exe: based LOW at 0x10000 with a ~23 MB committed bss that reserves the
# entire XBE VA range, forcing stack/TEB/heaps above it. Loads tj_hybrid + RunGame.
add_executable(tj_loader src/hybrid/loader_main.cpp src/hybrid/loader.rc)
target_include_directories(tj_loader PRIVATE src)
target_compile_definitions(tj_loader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_options(tj_loader PRIVATE
    /BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE
    /STACK:0x100000,0x100000)
set_target_properties(tj_loader PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- Installer (tj_setup) ----------------------------------------------------
# The installer ships NO game data: the user supplies their own disc image and it extracts
# from that. /MT so the installer itself needs no VC redist to run.
add_library(tj_setup_core STATIC src/setup/xdvdfs.cpp src/setup/xmf_inject.cpp)
target_include_directories(tj_setup_core PUBLIC src)
target_compile_definitions(tj_setup_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_property(TARGET tj_setup_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The installer embeds the runtime as RCDATA. Those files are build OUTPUTS, so the paths are
# resolved here and the .rc is generated. NOTE: tj_setup is a RELEASE artifact -- it embeds the
# Release binaries by path, so build it with --config Release.
set(TJ_SETUP_ICON  "${CMAKE_CURRENT_SOURCE_DIR}/src/setup/tjsetup.ico")
set(TJ_README_TXT  "${CMAKE_CURRENT_SOURCE_DIR}/tools/dist_README.txt")
set(TJ_LOADER_EXE  "${CMAKE_BINARY_DIR}/bin/Release/tj_loader.exe")
set(TJ_HYBRID_DLL  "${CMAKE_BINARY_DIR}/bin/Release/tj_hybrid.dll")

# Same VC++ redist the dist folder ships. Windows does not include it; app-local copies are
# the supported way to avoid making the player install a redist.
file(GLOB TJ_CRT_DIRS "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Redist/MSVC/*/x86/Microsoft.VC*.CRT")
list(SORT TJ_CRT_DIRS)
list(REVERSE TJ_CRT_DIRS)
if(TJ_CRT_DIRS)
    list(GET TJ_CRT_DIRS 0 TJ_CRT_DIR)
    set(CRT_MSVCP140        "${TJ_CRT_DIR}/msvcp140.dll")
    set(CRT_MSVCP140_1      "${TJ_CRT_DIR}/msvcp140_1.dll")
    set(CRT_MSVCP140_2      "${TJ_CRT_DIR}/msvcp140_2.dll")
    set(CRT_MSVCP140_ATOMIC "${TJ_CRT_DIR}/msvcp140_atomic_wait.dll")
    set(CRT_VCRUNTIME140    "${TJ_CRT_DIR}/vcruntime140.dll")
    set(CRT_VCRUNTIME140_THR "${TJ_CRT_DIR}/vcruntime140_threads.dll")
    set(CRT_CONCRT140       "${TJ_CRT_DIR}/concrt140.dll")
    configure_file(src/setup/payload.rc.in ${CMAKE_BINARY_DIR}/payload.rc @ONLY)
    # The .rc pulls these in by PATH, so CMake cannot see that the installer depends on their
    # CONTENT. Without this, rebuilding tj_hybrid.dll and not rebuilding tj_setup silently
    # ships the PREVIOUS DLL -- an installer that looks fine and installs a stale game.
    set_source_files_properties(${CMAKE_BINARY_DIR}/payload.rc PROPERTIES
        OBJECT_DEPENDS "${TJ_LOADER_EXE};${TJ_HYBRID_DLL};${TJ_SETUP_ICON};${TJ_README_TXT}")

    add_executable(tj_setup WIN32
        src/setup/setup_main.cpp
        src/setup/payload.cpp
        ${CMAKE_BINARY_DIR}/payload.rc
    )
    add_dependencies(tj_setup tj_loader tj_hybrid)
    target_link_libraries(tj_setup PRIVATE tj_setup_core comctl32 shell32 ole32 shlwapi advapi32 uuid)
    target_include_directories(tj_setup PRIVATE src)
    target_compile_definitions(tj_setup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_property(TARGET tj_setup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    set_target_properties(tj_setup PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")
    # Same installer, WITHOUT the requireAdministrator manifest, so `/silent` can be driven by
    # a test script into a scratch folder rather than needing a human to click through UAC.
    # It is a test artifact only -- the shipped installer is tj_setup.exe above.
    add_executable(tj_setup_probe WIN32
        src/setup/setup_main.cpp
        src/setup/payload.cpp
        ${CMAKE_BINARY_DIR}/payload.rc
    )
    add_dependencies(tj_setup_probe tj_loader tj_hybrid)
    target_link_libraries(tj_setup_probe PRIVATE tj_setup_core comctl32 shell32 ole32 shlwapi advapi32 uuid)
    target_include_directories(tj_setup_probe PRIVATE src)
    target_compile_definitions(tj_setup_probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_property(TARGET tj_setup_probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    set_target_properties(tj_setup_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
else()
    message(WARNING "No x86 VC++ redist found -- the tj_setup installer target is unavailable")
endif()

add_executable(xdvdfs_test src/setup/xdvdfs_test.cpp)
target_link_libraries(xdvdfs_test PRIVATE tj_setup_core)
target_compile_definitions(xdvdfs_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_property(TARGET xdvdfs_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(xdvdfs_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(xmf_test src/setup/xmf_test.cpp)
target_link_libraries(xmf_test PRIVATE tj_setup_core)
target_compile_definitions(xmf_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_property(TARGET xmf_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(xmf_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(diff_test tests/diff_test.cpp)
target_link_libraries(diff_test PRIVATE tj_game)
set_target_properties(diff_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# Stage the extracted images + relocation table next to the test binary.
add_custom_command(TARGET diff_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/src/game/generated/data_image.bin
        ${CMAKE_SOURCE_DIR}/src/game/generated/text_image.bin
        ${CMAKE_SOURCE_DIR}/src/game/generated/text_relocs.bin
        $<TARGET_FILE_DIR:diff_test>
)
