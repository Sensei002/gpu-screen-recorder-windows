# =============================================================================
# GPU Screen Recorder — Windows Port — build system (Phase 2)
#
# Builds the *portable* core of the upstream engine on Windows with
# MinGW-w64 (GCC) + Ninja. Only files that are platform-independent are
# compiled here; platform backends (capture, audio, IPC, windowing) land in
# later phases behind the abstractions documented in docs/architecture.md.
#
# Toolchain: MinGW-w64 (MSYS2 MINGW64 environment), see docs/build-windows.md.
# FFmpeg:   built from source by scripts/build-ffmpeg-windows.sh (mirrors the
#           upstream static-ffmpeg recipe); located via pkg-config.
# =============================================================================
cmake_minimum_required(VERSION 3.25)
# Phase 5: the WGC capture backend (gsr_capture_wgc.cpp) is C++/WinRT.
project(gpu_screen_recorder_windows C CXX)
# Phase 13: the three executables embed packaging/gsr.rc (icon + version
# info); MinGW compiles .rc files with windres (part of binutils).
enable_language(RC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# MSYS2's cppwinrt headers hard-require C++20 (winrt/base.h #errors on
# missing coroutine support), so the backend compiles as C++20.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MINGW)
    message(FATAL_ERROR
        "The Windows port currently requires the MinGW-w64 (GCC) toolchain. "
        "Use the MSYS2 MINGW64 environment (see docs/build-windows.md).")
endif()

# Match upstream compiler flags where meaningful (meson: -Wshadow, warning_level=2).
# -std=gnu11 is scoped to C: CMAKE_CXX_STANDARD 20 provides -std=gnu++20 for
# the Phase 5 C++/WinRT backend (a global -std would fight it).
add_compile_options($<$<COMPILE_LANGUAGE:C>:-std=gnu11> -Wshadow -Wall -Wextra)

# The engine version string (upstream meson passes -DGSR_VERSION="...").
# Single source of truth: the same string feeds the exe version-info
# resource (packaging/gsr.rc) via RC_FLAGS below.
set(GSR_VERSION_STR "6.0.0-w2")
add_compile_definitions(GSR_VERSION="${GSR_VERSION_STR}")

# Phase 13: exe branding — gsr.rc (packaging/) embeds the original logo as
# the exe icon and the version info. The -I flag lets windres find gsr.ico
# (referenced by path in the .rc) without quoting fragility.
set(GSR_RC_FLAGS "-I${CMAKE_SOURCE_DIR}/packaging")

# Optional gcov coverage instrumentation (Phase 3 CI deliverable: a
# coverage report artifact). Enabled in a separate build dir by the
# workflow; never in the release build.
option(GSR_ENABLE_COVERAGE "Build with gcov coverage instrumentation" OFF)
if(GSR_ENABLE_COVERAGE)
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

# Force-include the Windows portability shim into every translation unit.
add_compile_options("-include" "${CMAKE_SOURCE_DIR}/platform/windows/gsr_win32_compat.h")

# ---------------------------------------------------------------------------
# FFmpeg via pkg-config (static build from scripts/build-ffmpeg-windows.sh).
# The upstream engine links: libavformat libavcodec libavutil libswresample
# libavfilter.
# ---------------------------------------------------------------------------
# MSYS2 may ship pkg-config as a shell-script wrapper (not discoverable by
# CMake's find_program on Windows); fall back to pkgconf.exe directly.
find_program(PKG_CONFIG_EXECUTABLE pkg-config)
if(NOT PKG_CONFIG_EXECUTABLE)
    find_program(PKG_CONFIG_EXECUTABLE pkgconf)
endif()
if(NOT PKG_CONFIG_EXECUTABLE)
    message(FATAL_ERROR
        "pkg-config/pkgconf not found. Install mingw-w64-x86_64-pkgconf in the "
        "MSYS2 MINGW64 environment (see docs/build-windows.md).")
endif()

set(GSR_FFMPEG_LIBS libavformat libavcodec libavutil libswresample libavfilter)
execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --cflags ${GSR_FFMPEG_LIBS}
    OUTPUT_VARIABLE GSR_FFMPEG_CFLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE GSR_PKG_RESULT)
if(NOT GSR_PKG_RESULT EQUAL 0)
    message(FATAL_ERROR
        "pkg-config could not find the FFmpeg libraries. Build them first with "
        "scripts/build-ffmpeg-windows.sh and set PKG_CONFIG_PATH accordingly "
        "(see docs/build-windows.md).")
endif()
execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --static --libs ${GSR_FFMPEG_LIBS}
    OUTPUT_VARIABLE GSR_FFMPEG_LIBS_FLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE)

separate_arguments(GSR_FFMPEG_CFLAGS_LIST UNIX_COMMAND "${GSR_FFMPEG_CFLAGS}")
separate_arguments(GSR_FFMPEG_LIBS_FLAGS_LIST UNIX_COMMAND "${GSR_FFMPEG_LIBS_FLAGS}")

# srt.pc's Libs.private contributes MinGW runtime flags that resolve to
# DLL import libraries: -lpthread (libpthread.dll.a), -lstdc++
# (libstdc++-6.dll), -lgcc_s (libgcc_s_seh-1.dll), -latomic
# (libatomic-1.dll). Two problems:
#   1. -lpthread next to our static winpthread gives `multiple definition
#      of pthread_*` once a member of both ends up in the link.
#   2. The others only become DLL dependencies when a member is pulled;
#      binaries that pull deep FFmpeg members (via muxer.o, which touches
#      SRT/C++) then fail on the plain runner with a silent missing-DLL
#      crash. Filter all four; they are provided statically by the
#      runtime block below and by -static-libgcc/-static-libstdc++ on the
#      executable targets.
set(GSR_FILTERED_RUNTIME_LIBS "-lpthread" "-lstdc++" "-lgcc_s" "-latomic")
set(GSR_FFMPEG_LIBS_FLAGS_FILTERED "")
foreach(flag IN LISTS GSR_FFMPEG_LIBS_FLAGS_LIST)
    if(NOT flag IN_LIST GSR_FILTERED_RUNTIME_LIBS)
        list(APPEND GSR_FFMPEG_LIBS_FLAGS_FILTERED "${flag}")
    endif()
endforeach()
set(GSR_FFMPEG_LIBS_FLAGS_LIST ${GSR_FFMPEG_LIBS_FLAGS_FILTERED})

# ---------------------------------------------------------------------------
# pangoft2 for the mgl text pipeline (Phase 10 milestone B): text.c,
# text_edit.c and font_atlas.c rasterize glyphs through pango + fontconfig +
# freetype, exactly as on Linux (no platform divergence in shaping or
# layout). MSYS2 ships these as shared DLLs, so the mgl test executables
# bundle them next to the exe (workflow ldd step) — the eventual installer
# bundles them too (Phase 13/17).
# ---------------------------------------------------------------------------
execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --cflags pangoft2
    OUTPUT_VARIABLE MGL_PANGO_CFLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE MGL_PANGO_PKG_RESULT)
if(NOT MGL_PANGO_PKG_RESULT EQUAL 0)
    message(FATAL_ERROR
        "pkg-config could not find pangoft2. Install mingw-w64-x86_64-pango "
        "(pulls fontconfig/freetype/glib2/harfbuzz) in the MSYS2 MINGW64 "
        "environment.")
endif()
execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --libs pangoft2
    OUTPUT_VARIABLE MGL_PANGO_LIBS_FLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE)

separate_arguments(MGL_PANGO_CFLAGS_LIST UNIX_COMMAND "${MGL_PANGO_CFLAGS}")
separate_arguments(MGL_PANGO_LIBS_FLAGS_LIST UNIX_COMMAND "${MGL_PANGO_LIBS_FLAGS}")

# ---------------------------------------------------------------------------
# gsr_core — the portable engine core (upstream sources + Windows shims).
# ---------------------------------------------------------------------------
set(GSR_CORE_SOURCES
    upstream/src/defs.c
    upstream/src/json.c
    upstream/src/log.c
    upstream/src/args_parser.c
    upstream/src/library_loader.c
    upstream/src/ffmpeg_utils.c
    upstream/src/encoder/encoder.c
    upstream/src/replay_buffer/replay_buffer.c
    upstream/src/replay_buffer/replay_buffer_ram.c
    upstream/src/replay_buffer/replay_buffer_disk.c
    upstream/src/recorder/audio_codec.c
    upstream/src/recorder/video_codec.c
    upstream/src/recorder/recording_clock.c
    upstream/src/recorder/audio_capture.c
    upstream/src/recorder/audio_input.c
    upstream/src/recorder/capture_source.c
    # upstream/src/capture/capture.c: the gsr_capture_* vtable wrappers (pure
    # C, no X11 deps) — the public API the engine and self-tests call.
    upstream/src/capture/capture.c
    # recorder/muxer.c: the real upstream save-filepath naming function
    # (gsr_create_new_recording_filepath_from_timestamp) that the
    # platform filesystem layer delegates to.
    upstream/src/recorder/muxer.c
    # Phase 5b: the GL pipeline (architecture §3.3 Option B — upstream's GL
    # color-conversion/shader code runs unchanged on ANGLE). egl.c keeps its
    # Linux loader for reference and branches to gsr_egl_load_win32() on
    # Windows (see upstream-porting-notes.md §3f).
    upstream/src/color_conversion.c
    upstream/src/shader.c
    upstream/src/egl.c
    # Phase 2 shims.
    platform/windows/gsr_win32_compat.c
    platform/windows/gsr_utils_win32.c
    # Phase 3 platform layer (pure-logic backends + small helpers).
    platform/windows/gsr_filesystem_win32.c
    platform/windows/gsr_ipc_protocol.c
    platform/windows/gsr_config_win32.c
    platform/windows/gsr_codec_caps_win32.c
    platform/windows/gsr_display_win32.c
    platform/windows/gsr_platform_win32.c
    # Phase 5: WGC capture backend — pure logic helpers (C, headless-tested)
    # plus the C++/WinRT backend itself (extern "C" surface in capture.h).
    platform/windows/gsr_capture_wgc_helpers.c
    platform/windows/gsr_capture_wgc.cpp
    # Phase 5b: the Windows ANGLE-on-D3D11 gsr_egl loader + the D3D11-texture
    # import helpers (EGL_ANGLE_d3d_texture_client_buffer) + the Windows
    # gl_get_gpu_info (upstream's lives in the X11/DRM utils.c, not built).
    platform/windows/gsr_egl_win32.c
    # Phase 6: DXGI Desktop Duplication capture backend (monitor-only
    # fallback; plain C — DD is a DXGI/D3D11 COM interface).
    platform/windows/gsr_capture_dxgi.c
    # Phase 7 (milestone A): the upstream recorder end-to-end. recorder.c
    # runs unchanged; its X11-only deps (damage/cursor/window.c, the
    # X11/DRM utils.c) are replaced by the win32 shims below, and the
    # capture_setup seam by gsr_capture_setup_win32.c (WGC/DXGI dispatch).
    # codec_select.c has #ifdef _WIN32 guards for the VAAPI/Vulkan/NVENC
    # queries (NVENC lands in milestone B).
    upstream/src/recorder/recorder.c
    upstream/src/recorder/codec_select.c
    upstream/src/recorder/replay_save.c
    upstream/src/recorder/screenshot.c
    upstream/src/encoder/video/video.c
    upstream/src/encoder/video/software.c
    upstream/src/plugins.c
    upstream/src/image_writer.c
    platform/windows/gsr_recorder_win32.c
    platform/windows/gsr_capture_setup_win32.c
    # Phase 8: the upstream sound_device API + -a device listing over
    # WASAPI (replaces upstream/src/sound.c, PulseAudio/PipeWire, which
    # is not built on Windows).
    platform/windows/audio_wasapi.c
    # Phase 7 (milestone B): the NVIDIA NVENC encoder over d3d11va (replaces
    # upstream's GL+CUDA encoder/video/nvenc.c) + the honest capability
    # probe (upstream codec_query/nvenc.c, which needs CUDA). Pure
    # generation-table logic lives in gsr_nvenc_internal.h for headless
    # testing.
    platform/windows/gsr_nvenc_win32.c
    # Phase 11: the engine's Windows command handlers (--info/--list-*/
    # -sc script execution), the windowing abstraction (ANGLE loader wrap),
    # and the named-pipe IPC transport (server + client). Upstream's
    # commands.c / windowing.c / ipc.c are Linux-coupled and not built.
    platform/windows/gsr_commands_win32.c
    platform/windows/gsr_windowing_win32.c
    platform/windows/gsr_ipc_win32.c
)

add_library(gsr_core STATIC ${GSR_CORE_SOURCES})
# platform/windows is on the include path so that the <libgen.h> and
# <dlfcn.h> shims (MinGW-w64 ships neither) resolve for the upstream sources.
target_include_directories(gsr_core PUBLIC
    ${CMAKE_SOURCE_DIR}/platform/include   # the port's own interfaces (Phase 3)
    ${CMAKE_SOURCE_DIR}/upstream/include
    ${CMAKE_SOURCE_DIR}/platform/windows
    # Stub X11/DRM headers so upstream headers that reference X11/KMS types
    # (cursor.h, capture_setup.h) compile on Windows; see
    # docs/upstream-porting-notes.md §3c.
    ${CMAKE_SOURCE_DIR}/platform/windows/stubs)
target_compile_options(gsr_core PUBLIC ${GSR_FFMPEG_CFLAGS_LIST})
target_link_libraries(gsr_core PUBLIC
    m
    ${GSR_FFMPEG_LIBS_FLAGS_LIST}
    advapi32 # SystemFunction036 (RtlGenRandom) for generate_random_characters
    shell32  # SHGetKnownFolderPath (gsr_platform_get_videos_dir)
    ole32    # CoTaskMemFree (gsr_platform_get_videos_dir)
    dxgi     # Phase 4 monitor enumeration (CreateDXGIFactory1, IDXGIOutput6)
    shcore   # Phase 4 per-monitor DPI (GetDpiForMonitor)
    d3d11        # Phase 5 WGC backend (D3D11CreateDevice, ID3D11Texture2D)
    windowsapp   # Phase 5 WGC backend (RoGetActivationFactory/RoInitialize,
                 # exported by mingw-w64's libwindowsapp.a)
    # MinGW runtime libs, statically (the executables must run without the
    # MSYS2 runtime DLLs, which the workflow's `test` job relies on):
    #   - winpthreads: MSYS2's gcc 16 dropped the -static-libwinpthread
    #     driver flag, so force the static archive at the linker level
    #     (encoder.c uses pthread mutexes).
    #   - atomic: pulled by deep FFmpeg members; link the static archive so
    #     no libatomic-1.dll is needed.
    #   - stdc++: libsrt.a is C++; -static-libstdc++ is a no-op when linking
    #     with the C driver (cc), so force the static archive here too or
    #     the exe either fails to link or needs libstdc++-6.dll.
    "-Wl,-Bstatic"
    "-lwinpthread"
    "-latomic"
    "-lstdc++"
    "-Wl,-Bdynamic")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
# Phase 1 CI toolchain smoke test (compiler/OS info).
add_executable(ci-smoke tests/ci-smoke/main.c)

# Probe which POSIX symbols the MinGW-w64 runtime provides (CI diagnostics).
add_executable(compat-probe tests/compat-probe/main.c)
target_link_libraries(compat-probe PRIVATE gsr_core)

# Core unit tests: CLI parsing, audio input parsing, recording clock,
# replay buffer (RAM + disk), JSON.
add_executable(gsr-core-test tests/gsr-core-test/main.c)
target_link_libraries(gsr-core-test PRIVATE gsr_core)

# Phase 3 platform-layer unit tests: filesystem naming/sanitization, the
# IPC protocol codec, config round-trip, codec capability logic, and the
# --list-monitors/--info output formats.
add_executable(platform-test tests/platform-test/main.c)
target_link_libraries(platform-test PRIVATE gsr_core)

# Phase 5 WGC capture self-test: runs a real WGC capture of the primary
# monitor on CI's virtual display and validates the damage/clear/capture
# contract plus the D3D11 texture. SKIPs (exit 0) when WGC is unavailable
# in the session; FAILs on any real capture failure. The ANGLE interop
# probe inside is informational (needs libEGL.dll, only in the MSYS2
# ctest step).
add_executable(wgc-self-test tests/wgc-self-test/main.c)
target_link_libraries(wgc-self-test PRIVATE gsr_core)

# Phase 5b render pipeline self-test: D3D11 texture -> ANGLE import
# (EGL_ANGLE_d3d_texture_client_buffer) -> upstream gsr_color_conversion
# draw -> readback validation. Runs headless on the WARP adapter in the
# MSYS2 ctest step (ANGLE installed); SKIPs (exit 0) where libEGL.dll is
# not loadable (plain runner).
add_executable(render-self-test tests/render-self-test/main.c)
target_link_libraries(render-self-test PRIVATE gsr_core)

# Phase 6 DXGI Desktop Duplication self-test: pure rotation logic (always
# runs) + a live DD capture of the primary monitor. SKIPs (exit 0) where
# DD is unavailable (locked-down sessions); on CI the runner's Basic
# Display Adapter usually supports DD, exercising a REAL capture path.
add_executable(dxgi-self-test tests/dxgi-self-test/main.c)
target_link_libraries(dxgi-self-test PRIVATE gsr_core)

# Phase 7 (milestone A) recorder self-test: drives the upstream recorder
# end-to-end (live capture -> ANGLE color conversion -> libx264 -> MKV)
# and validates the output file. SKIPs (exit 0) where no capture backend
# is available; on CI the runner's Desktop Duplication makes this a REAL
# end-to-end recording.
add_executable(recorder-self-test tests/recorder-self-test/main.c)
target_link_libraries(recorder-self-test PRIVATE gsr_core)

# Phase 8 audio conversion self-test: headless tests of the WASAPI
# mix-format -> codec-format conversion pipeline (decode/downmix/resample/
# quantize/ring) with synthetic data. The live WASAPI capture path needs a
# real endpoint (recorder-self-test covers it on machines that have one);
# the CI runner has none, so the math is proven here instead.
add_executable(audio-conv-test tests/audio-conv-test/main.c)
target_link_libraries(audio-conv-test PRIVATE gsr_core)

# Phase 7 (milestone B) NVENC self-test: headless GPU-generation table
# checks + the honest live probe (must report nothing on a non-NVIDIA
# machine, h264 on a real one) + an end-to-end -encoder gpu recording with
# -fallback-cpu-encoding that must fall back to libx264. The recording half
# needs ANGLE + a capture backend and SKIPs like recorder-self-test.
add_executable(nvenc-self-test tests/nvenc-self-test/main.c)
target_link_libraries(nvenc-self-test PRIVATE gsr_core)

# Phase 8 (milestone B) audio tests, both headless (the runner has no
# audio endpoints):
#   audio-list-test — the device/session listing contract behind
#   --list-audio-devices / --list-application-audio (WASAPI endpoints +
#   IAudioSessionManager2 sessions), the "name (description)" line format,
#   the device-change notification register/unregister round-trip, and the
#   -a app:NAME parse surface (which the engine rejects honestly on
#   Windows: no per-session WASAPI capture).
#   audio-sync-test — the A/V sync contract: exact period-sized chunk
#   delivery, duration-preserving 44.1k->48k resampling, sample-accurate
#   quantization, the codec delay formulas, and the derived audio-PTS
#   start offset that aligns the first audio sample with the first frame.
add_executable(audio-list-test tests/audio-list-test/main.c)
target_link_libraries(audio-list-test PRIVATE gsr_core)
add_executable(audio-sync-test tests/audio-sync-test/main.c)
target_link_libraries(audio-sync-test PRIVATE gsr_core)

# Phase 9 replay-save test: disk replay-buffer trim (forced file rollover +
# time-based removal), keyframe boundaries across trimmed files, and the
# simulated-crash cleanup (stale gsr-replay-*.gsr directories swept when the
# next session starts). The end-to-end save path lives in recorder-self-test's
# replay pass (clone -> thread -> Replay_*.mkv, restart-on-save).
add_executable(replay-save-test tests/replay-save-test/main.c)
target_link_libraries(replay-save-test PRIVATE gsr_core)

# ---------------------------------------------------------------------------
# Phase 11 — the engine binary, gsr-cli, and the IPC transport.
# ---------------------------------------------------------------------------
# gpu-screen-recorder.exe — the engine executable (platform/windows/
# gsr_main_win32.c mirrors upstream src/cli/main.c: the -ipc handlers, the
# deferred-request completion, screenshot path, args validation).
add_executable(gpu-screen-recorder platform/windows/gsr_main_win32.c packaging/gsr.rc)
target_link_libraries(gpu-screen-recorder PRIVATE gsr_core)
set_target_properties(gpu-screen-recorder PROPERTIES RC_FLAGS "${GSR_RC_FLAGS}")

# gsr-cli.exe — the Windows port of upstream tools/gsr-cli/main.c (status/
# stop/toggle-pause/set-paused/save-replay/... over the named-pipe client).
add_executable(gsr-cli platform/windows/gsr_cli_win32.c packaging/gsr.rc)
target_link_libraries(gsr-cli PRIVATE gsr_core)
set_target_properties(gsr-cli PROPERTIES RC_FLAGS "${GSR_RC_FLAGS}")

# Phase 11 engine-IPC test: part 1 (always runs) exercises the named-pipe
# transport in-process — server with test handlers, client connects, sends
# requests, asserts the ok/error reply JSON and the deferred-request
# completion round-trip. Part 2 (SKIPs like recorder-self-test when ANGLE is
# unavailable) spawns the real gpu-screen-recorder.exe with -ipc and asserts
# the status/toggle-pause/stop contract through gsr-cli.
add_executable(engine-ipc-test tests/engine-ipc-test/main.c)
target_link_libraries(engine-ipc-test PRIVATE gsr_core)

# ---------------------------------------------------------------------------
# mgl_core — the vendored upstream UI rendering library (ui/depends/mglpp/
# depends/mgl), compiled for Win32 with the new Win32 window backend, the
# WGL graphics backend (Phase 10 milestone A) and the pango text pipeline
# (Phase 10 milestone B: font_atlas.c/text.c/text_edit.c rasterize glyphs
# through pangoft2 + fontconfig + freetype, exactly as on Linux). WGL +
# opengl32.dll needs no extra packages; pango comes from MSYS2 as shared
# DLLs (bundled next to the test exes by the workflow). CI's GDI Generic
# software GL (GL 1.1) covers the headless smoke test — the text pipeline
# uses only GL 1.1 (client arrays, no VBOs).
# ---------------------------------------------------------------------------
set(MGL_CORE_SOURCES
    ui/depends/mglpp/depends/mgl/src/graphics/backend/graphics.c
    ui/depends/mglpp/depends/mgl/src/graphics/backend/wgl.c
    ui/depends/mglpp/depends/mgl/src/graphics/font_atlas.c
    ui/depends/mglpp/depends/mgl/src/graphics/glyph_map.c
    ui/depends/mglpp/depends/mgl/src/graphics/image.c
    ui/depends/mglpp/depends/mgl/src/graphics/primitive_type.c
    ui/depends/mglpp/depends/mgl/src/graphics/quad_batch.c
    ui/depends/mglpp/depends/mgl/src/graphics/rectangle.c
    ui/depends/mglpp/depends/mgl/src/graphics/shader.c
    ui/depends/mglpp/depends/mgl/src/graphics/sprite.c
    ui/depends/mglpp/depends/mgl/src/graphics/text.c
    ui/depends/mglpp/depends/mgl/src/graphics/text_edit.c
    ui/depends/mglpp/depends/mgl/src/graphics/texture.c
    ui/depends/mglpp/depends/mgl/src/graphics/vertex.c
    ui/depends/mglpp/depends/mgl/src/graphics/vertex_buffer.c
    ui/depends/mglpp/depends/mgl/src/system/clock.c
    ui/depends/mglpp/depends/mgl/src/system/fileutils.c
    ui/depends/mglpp/depends/mgl/src/system/utf8.c
    ui/depends/mglpp/depends/mgl/src/mgl.c
    ui/depends/mglpp/depends/mgl/src/window/key.c
    ui/depends/mglpp/depends/mgl/src/window/win32.c
    ui/depends/mglpp/depends/mgl/src/window/window.c
    ui/depends/mglpp/depends/mgl/src/gl.c
)
add_library(mgl_core STATIC ${MGL_CORE_SOURCES})
target_include_directories(mgl_core PUBLIC ${CMAKE_SOURCE_DIR}/ui/depends/mglpp/depends/mgl/include)
target_compile_options(mgl_core PRIVATE ${MGL_PANGO_CFLAGS_LIST})
target_link_libraries(mgl_core PUBLIC opengl32 user32 gdi32 ${MGL_PANGO_LIBS_FLAGS_LIST})

# mglpp — the vendored upstream C++ wrapper over mgl (ui/depends/mglpp).
# Purely portable: it wraps the mgl C API (Window/Text/Texture/...), which on
# Windows is the Win32 backend from mgl_core. The gsr-ui app and its tests
# use the C++ API (mgl::Window, mgl::vec2i, ...).
set(MGLPP_CORE_SOURCES
    ui/depends/mglpp/src/graphics/Text.cpp
    ui/depends/mglpp/src/graphics/TextEdit.cpp
    ui/depends/mglpp/src/graphics/Image.cpp
    ui/depends/mglpp/src/graphics/VertexBuffer.cpp
    ui/depends/mglpp/src/graphics/Rectangle.cpp
    ui/depends/mglpp/src/graphics/Texture.cpp
    ui/depends/mglpp/src/graphics/Sprite.cpp
    ui/depends/mglpp/src/graphics/Shader.cpp
    ui/depends/mglpp/src/system/MemoryMappedFile.cpp
    ui/depends/mglpp/src/system/Clock.cpp
    ui/depends/mglpp/src/system/Utf8.cpp
    ui/depends/mglpp/src/window/Window.cpp
    ui/depends/mglpp/src/window/Keyboard.cpp
    ui/depends/mglpp/src/mglpp.cpp
)
add_library(mglpp_core STATIC ${MGLPP_CORE_SOURCES})
target_include_directories(mglpp_core PUBLIC
    ${CMAKE_SOURCE_DIR}/ui/depends/mglpp/include
    ${CMAKE_SOURCE_DIR}/ui/depends/mglpp/depends/mgl/include)
target_compile_options(mglpp_core PRIVATE ${MGL_PANGO_CFLAGS_LIST})
target_link_libraries(mglpp_core PUBLIC mgl_core)

# Phase 10 (milestone A) mgl Win32 backend smoke test: window creation
# (hidden), WGL context + GL sanity, input events (key/char/mouse/wheel via
# synthetic messages), clipboard round-trip, monitor enumeration, fullscreen
# and size limits, and init_from_existing_window (subclassing). All headless
# on CI's GDI Generic software GL.
add_executable(mgl-win32-test tests/mgl-win32-test/main.c)
target_link_libraries(mgl-win32-test PRIVATE mgl_core)

# Phase 10 (remaining) RPC layer: the gsr-ui<->gsr-ui-cli transport. On
# Windows this is a named pipe (\\.\pipe\gsr-ui), on POSIX an abstract unix
# domain socket — the public API (create/open/write/poll/add_handler) is
# identical. The test is headless and self-contained.
add_library(ui_rpc STATIC ui/src/Rpc.cpp)
target_include_directories(ui_rpc PUBLIC ${CMAKE_SOURCE_DIR}/ui/include)
add_executable(gsr-ui-cli ui/tools/gsr-ui-cli/main.c)
target_include_directories(gsr-ui-cli PRIVATE ${CMAKE_SOURCE_DIR}/ui/include)
add_executable(ui-rpc-test tests/ui-rpc-test/main.cpp)
target_link_libraries(ui-rpc-test PRIVATE ui_rpc)

# ---------------------------------------------------------------------------
# ui_core — the gsr-ui application sources that compile on Windows. This is
# the upstream UI code (settings pages, widgets, Overlay) plus the ported
# platform layer: Process/Utils/WindowUtils are Windows implementations,
# the platform modules (GlobalHotkeys/CursorTracker/DesktopEnvironment/
# Clipboard/RegionSelector) are the Win32 backends, AudioPlayer is the
# waveOut backend, LedIndicator is a no-op (Linux sysfs feature). The
# X11/Wayland-only sources (WindowUtils.cpp, the *X11/*Wayland/*Linux/
# *Joystick modules, window_texture.c, WaylandHostBridge, Hotplug, the
# generated wayland protocol) are not built; Overlay.cpp and main.cpp carry
# #ifndef _WIN32 guards around their X11/Wayland code paths.
# ---------------------------------------------------------------------------
set(UI_CORE_SOURCES
    ui/src/Process.cpp
    ui/src/Utils.cpp
    ui/src/WindowUtilsWin32.cpp
    ui/src/GsrInfo.cpp
    ui/src/Config.cpp
    ui/src/Theme.cpp
    ui/src/Translation.cpp
    ui/src/AudioPlayer.cpp
    ui/src/LedIndicatorWin32.cpp
    ui/src/GlobalHotkeys/GlobalHotkeysWin32.cpp
    ui/src/CursorTracker/CursorTrackerWin32.cpp
    ui/src/DesktopEnvironment/DesktopEnvironmentWin32.cpp
    ui/src/Clipboard/ClipboardWin32.cpp
    ui/src/RegionSelector/RegionSelectorWin32.cpp
    ui/src/gui/Widget.cpp
    ui/src/gui/ScrollablePage.cpp
    ui/src/gui/Button.cpp
    ui/src/gui/RadioButton.cpp
    ui/src/gui/Entry.cpp
    ui/src/gui/CheckBox.cpp
    ui/src/gui/ComboBox.cpp
    ui/src/gui/Page.cpp
    ui/src/gui/StaticPage.cpp
    ui/src/gui/PageStack.cpp
    ui/src/gui/List.cpp
    ui/src/gui/Utils.cpp
    ui/src/gui/DropdownButton.cpp
    ui/src/gui/Label.cpp
    ui/src/gui/Image.cpp
    ui/src/gui/LineSeparator.cpp
    ui/src/gui/CustomRendererWidget.cpp
    ui/src/gui/FileChooser.cpp
    ui/src/gui/SettingsPage.cpp
    ui/src/gui/ScreenshotSettingsPage.cpp
    ui/src/gui/GlobalSettingsPage.cpp
    ui/src/gui/GsrPage.cpp
    ui/src/gui/Subsection.cpp
    ui/src/gui/Tooltip.cpp
    ui/src/Overlay.cpp
)
add_library(ui_core STATIC ${UI_CORE_SOURCES})
target_include_directories(ui_core PUBLIC
    ${CMAKE_SOURCE_DIR}/ui/include
    # GlobalSettingsPage.cpp has a vestigial <X11/Xlib.h> include; the empty
    # stub resolves it on Windows (no X11 types are used).
    ${CMAKE_SOURCE_DIR}/platform/windows/stubs)
target_compile_options(ui_core PRIVATE ${MGL_PANGO_CFLAGS_LIST})
target_link_libraries(ui_core PUBLIC
    ui_rpc
    mglpp_core
    user32    # GetForegroundWindow/GetCursorPos/RegisterHotKey/EnumDisplayMonitors
    gdi32     # RegionSelectorWin32 GDI drawing
    advapi32  # RegOpenKeyExA (set_xdg_autostart), CreateProcess
    shell32   # SHGetFolderPathW (config dir)
    ole32     # OleSetClipboard (ClipboardWin32)
    winmm     # waveOut (AudioPlayer)
)

# The gsr-ui application itself. Not exercised end-to-end in CI yet: it needs
# the gpu-screen-recorder engine binary (Phase 11) for --info at startup, so
# CI validates the UI through ui-rpc-test + ui-module-test + gsr-ui-cli for
# now.
add_executable(gsr-ui ui/src/main.cpp packaging/gsr.rc)
target_link_libraries(gsr-ui PRIVATE ui_core)
target_compile_definitions(gsr-ui PRIVATE GSR_UI_VERSION="${GSR_VERSION_STR}")
set_target_properties(gsr-ui PROPERTIES RC_FLAGS "${GSR_RC_FLAGS}")

# Phase 10 (remaining) UI platform-module smoke test: the Win32 equivalents of
# the X11/Wayland modules (WindowUtils, cursor tracker, desktop environment,
# global hotkeys, clipboard, audio, region selector) exercised headlessly,
# plus the overlay window behavior (click-through/taskbar/topmost/fullscreen/
# alpha styles via a real hidden mgl window).
add_executable(ui-module-test tests/ui-module-test/main.cpp)
target_link_libraries(ui-module-test PRIVATE ui_core)

# Phase 10 (remaining) golden render test: renders the real settings page
# headless (theme textures + fontconfig text + widget draw + WGL swap) and
# compares against tests/golden/ui-settings-golden.ppm with a per-pixel
# tolerance. Self-bootstrapping: if the golden is missing the test passes
# and writes ui-golden-render.ppm to the CWD (uploaded as an artifact, then
# committed as the golden); GSR_GOLDEN_UPDATE=1 re-baselines explicitly.
add_executable(ui-golden-test tests/ui-golden-test/main.cpp)
target_link_libraries(ui-golden-test PRIVATE ui_core)

# Link the test executables against the static MinGW runtime so they can be
# re-run by the workflow's `test` job without the MSYS2 environment.
# -static-libgcc works with the C driver; the C++ runtime (libstdc++.a),
# winpthread and atomic are already linked statically via the gsr_core
# PUBLIC link flags above (-static-libstdc++/-static-libwinpthread are
# driver options that do nothing when linking with cc).
foreach(target ci-smoke compat-probe gsr-core-test platform-test wgc-self-test render-self-test dxgi-self-test recorder-self-test audio-conv-test nvenc-self-test audio-list-test audio-sync-test replay-save-test mgl-win32-test ui-rpc-test ui-module-test ui-golden-test gsr-ui gsr-ui-cli gpu-screen-recorder gsr-cli engine-ipc-test)
    set_target_properties(${target} PROPERTIES
        LINK_FLAGS "-static-libgcc")
endforeach()

enable_testing()
add_test(NAME ci-smoke COMMAND ci-smoke)
add_test(NAME gsr-core-test COMMAND gsr-core-test)
add_test(NAME platform-test COMMAND platform-test)
add_test(NAME wgc-self-test COMMAND wgc-self-test)
add_test(NAME render-self-test COMMAND render-self-test)
add_test(NAME dxgi-self-test COMMAND dxgi-self-test)
add_test(NAME recorder-self-test COMMAND recorder-self-test)
add_test(NAME audio-conv-test COMMAND audio-conv-test)
add_test(NAME nvenc-self-test COMMAND nvenc-self-test)
add_test(NAME audio-list-test COMMAND audio-list-test)
add_test(NAME audio-sync-test COMMAND audio-sync-test)
add_test(NAME replay-save-test COMMAND replay-save-test)
add_test(NAME engine-ipc-test COMMAND engine-ipc-test)
add_test(NAME mgl-win32-test COMMAND mgl-win32-test)
add_test(NAME ui-rpc-test COMMAND ui-rpc-test)
add_test(NAME ui-module-test COMMAND ui-module-test)
add_test(NAME ui-golden-test COMMAND ui-golden-test ${CMAKE_SOURCE_DIR})
