# Copyright (c) 2026 Scintillae Ltd. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later WITH Tigryx-Linking-Exception
# See COPYING and EXCEPTION.md.
#
# =============================================================================
# Tigryx - cross-platform compiler/VM (GPL source release)
#
# Build:
#   cmake -S . -B build -DTIGRYX_BACKEND=opengl-win32 -DTIGRYX_JIT=x64-win
#   cmake --build build --config Release
#
# Per-target front-ends live under platforms/<id>/ and invoke this project
# with the appropriate -D options + toolchain.
#
# Binaries produced:
#   tigryx      dispatcher  (tigryx run / pp)
#   tigryxri   interpreter / bytecode compiler
#   tigryxpp   preprocessor
#   (plus the *w GUI-subsystem twins on Windows)
#
# Third-party dependencies are NOT part of this release - see README.md:
#   thirdparty/miniz/{miniz.c,miniz.h}   ZIP inflate for @unzip (vm.c)
#   include/stb_image.h                  image loading (vm.c)
#   include/stb_truetype.h               font rendering / .fnt caches
#   src/backends/allegro-dos/include/    Allegro 4 headers (DOS backend)
#   src/backends/allegro-dos/lib/djgpp/liballeg.a   (DOS backend)
#   thirdparty/lwip/                     lwIP 2.2.x (optional DOS HTTP)
# =============================================================================
cmake_minimum_required(VERSION 3.16)
project(tigryx C)

# DOS/DJGPP drops back to C99 with GNU extensions: strict ANSI mode hides
# POSIX declarations like `struct stat` (guarded behind `#ifndef
# __STRICT_ANSI__` in DJGPP's sys/stat.h).
if(CMAKE_SYSTEM_NAME STREQUAL "DOS")
    set(CMAKE_C_STANDARD 99)
    set(CMAKE_C_EXTENSIONS ON)
else()
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_EXTENSIONS OFF)
endif()
set(CMAKE_C_STANDARD_REQUIRED ON)

# Backend selection. Each backend lives in src/backends/<name>/ and exports
# a single symbol named `tigryx_backend`. Exactly one backend is linked.
set(TIGRYX_BACKEND "auto" CACHE STRING "Backend to link (auto|opengl-win32|allegro-dos|allegro-fixed-dos|relic3d-win32)")
set_property(CACHE TIGRYX_BACKEND PROPERTY STRINGS auto opengl-win32 allegro-dos allegro-fixed-dos relic3d-win32)

# TCP/IP support for the DOS backend (opt-in, default OFF).
# When enabled, builds the in-tree lwIP port (src/dosnet: lwIP NO_SYS +
# packet-driver client + the tdn_* socket shim) against a lwIP 2.2.x
# source tree dropped into thirdparty/lwip/ (not part of this release),
# links libtdosnet.a and defines HAVE_DOSNET so the DOS backend gets real
# HTTP support. When disabled, @http_* functions return -1 (not supported).
option(TIGRYX_DOSNET "Enable lwIP TCP/IP stack for DOS HTTP support" OFF)

option(TIGRYX_PROFILE "Compile with -pg for gprof profiling (disables -fomit-frame-pointer)" OFF)

# Soft-float support for FPU-less targets (386SX/486SX). Off by default; the
# dos-frelic3d-djgpp-i386 build turns it on.
#
# When ON:
#   - tigryx_core is compiled with -msoft-float and gains the soft-float
#     runtime in src/softfp/: the libgcc entry points i386 GCC does not
#     provide for -msoft-float (see src/softfp/softfp32.h), a matching f32
#     and f64 libm subset, and a correctly-rounded strtod for float literals.
#   - -msoft-float is a PUBLIC compile option of tigryx_core, so CMake
#     propagates it to every target that links tigryx_core (tigryx_backend,
#     tigryx_jit and the executables).
#
# The propagation is a correctness requirement, not a convenience:
# -msoft-float changes how float/double are RETURNED (EAX, or EDX:EAX for
# double, instead of ST(0)). Compiling only part of the dependency graph with
# it would silently corrupt any float-returning call across that boundary.
#
# The prebuilt liballeg.a is not recompiled and does not need to be: the
# backend calls no float/double-returning Allegro function, and float
# arguments are stack-passed identically in both ABIs.
option(TIGRYX_SOFTFP "Compile with -msoft-float + the src/softfp/ runtime, for FPU-less targets" OFF)

# Platform id: the per-target build scripts under platforms/<id>/ always
# pass -DRELIC_PLATFORM_ID=<id>. Provide a fallback so a bare `cmake`
# invocation still produces a sensible bin/ directory name.
if(NOT DEFINED RELIC_PLATFORM_ID)
    set(RELIC_PLATFORM_ID "${CMAKE_SYSTEM_NAME}-${CMAKE_C_COMPILER_ID}-${CMAKE_SYSTEM_PROCESSOR}")
    string(TOLOWER "${RELIC_PLATFORM_ID}" RELIC_PLATFORM_ID)
endif()

if(TIGRYX_DOSNET AND CMAKE_SYSTEM_NAME STREQUAL "DOS" AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    # tdosnet: lwIP 2.2.x (BSD-3) core + DJGPP packet-driver port + socket
    # shim. lwIP itself is a third-party drop-in under thirdparty/lwip/.
    add_library(tdosnet STATIC
        src/dosnet/pktdrv.c
        src/dosnet/sys_arch_dos.c
        src/dosnet/tdosnet.c
        thirdparty/lwip/src/core/def.c
        thirdparty/lwip/src/core/dns.c
        thirdparty/lwip/src/core/inet_chksum.c
        thirdparty/lwip/src/core/init.c
        thirdparty/lwip/src/core/ip.c
        thirdparty/lwip/src/core/mem.c
        thirdparty/lwip/src/core/memp.c
        thirdparty/lwip/src/core/netif.c
        thirdparty/lwip/src/core/pbuf.c
        thirdparty/lwip/src/core/sys.c
        thirdparty/lwip/src/core/timeouts.c
        thirdparty/lwip/src/core/tcp.c
        thirdparty/lwip/src/core/tcp_in.c
        thirdparty/lwip/src/core/tcp_out.c
        thirdparty/lwip/src/core/udp.c
        thirdparty/lwip/src/core/ipv4/acd.c
        thirdparty/lwip/src/core/ipv4/dhcp.c
        thirdparty/lwip/src/core/ipv4/etharp.c
        thirdparty/lwip/src/core/ipv4/icmp.c
        thirdparty/lwip/src/core/ipv4/ip4.c
        thirdparty/lwip/src/core/ipv4/ip4_addr.c
        thirdparty/lwip/src/core/ipv4/ip4_frag.c
        thirdparty/lwip/src/netif/ethernet.c)
    target_include_directories(tdosnet PUBLIC
        "${CMAKE_SOURCE_DIR}/src/dosnet"
        "${CMAKE_SOURCE_DIR}/thirdparty/lwip/src/include")
    set_target_properties(tdosnet PROPERTIES PREFIX "lib" SUFFIX ".a")
endif()

if(TIGRYX_BACKEND STREQUAL "auto")
    if(WIN32)
        set(TIGRYX_BACKEND "opengl-win32")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "DOS")
        set(TIGRYX_BACKEND "allegro-dos")
    else()
        message(FATAL_ERROR "No TIGRYX_BACKEND specified and none can be auto-detected for this platform")
    endif()
endif()

# JIT selection. Each JIT lives in src/jits/<name>/ and exports a single
# symbol named `tigryx_jit`. The "null" JIT is a stub whose compile()
# returns false, so the engine falls back to the interpreter.
set(TIGRYX_JIT "auto" CACHE STRING "JIT to link (auto|null|x86-win|x86-dos|x64-win)")
set_property(CACHE TIGRYX_JIT PROPERTY STRINGS auto null x86-win x86-dos x64-win)

if(TIGRYX_JIT STREQUAL "auto")
    if(CMAKE_SYSTEM_NAME STREQUAL "DOS" OR DJGPP)
        set(TIGRYX_JIT "x86-dos")
    elseif(WIN32)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(TIGRYX_JIT "x64-win")
        else()
            set(TIGRYX_JIT "x86-win")
        endif()
    else()
        set(TIGRYX_JIT "null")
    endif()
endif()

# -----------------------------------------------------------------------------
# Core library - portable C, no OS calls.
# thirdparty/miniz is not part of this release; drop miniz.c + miniz.h into
# thirdparty/miniz/ before configuring (see README.md).
# -----------------------------------------------------------------------------
set(RELIC_CORE_SOURCES
    src/core/lexer.c
    src/core/parser.c
    src/core/typecheck.c
    src/core/ir.c
    src/core/vm.c
    src/core/mvm.c
    src/core/debug.c
    src/core/console_win.c
    src/core/pp_tmp.c
    src/core/compile.c
    src/core/far16.c
    src/core/tier.c
    thirdparty/miniz/miniz.c
)
if(TIGRYX_SOFTFP)
    list(APPEND RELIC_CORE_SOURCES
        src/softfp/softfp32.c
        src/softfp/softfp64.c
        src/softfp/softfp_math.c
        src/softfp/softfp_math64.c
        src/softfp/softfp_strtod.c
    )
endif()
add_library(tigryx_core STATIC ${RELIC_CORE_SOURCES})
target_include_directories(tigryx_core PUBLIC include thirdparty/miniz)
target_compile_definitions(tigryx_core PRIVATE MINIZ_NO_DEFLATE_APIS MINIZ_NO_TIME)
if(NOT MSVC)
    set_source_files_properties(thirdparty/miniz/miniz.c PROPERTIES
        COMPILE_OPTIONS "-Wno-unused-function")
endif()

if(TIGRYX_SOFTFP)
    target_include_directories(tigryx_core PRIVATE src/softfp)
    # PUBLIC so every dependent inherits -msoft-float (see the option above).
    target_compile_options(tigryx_core PUBLIC -msoft-float)
    # RELIC_SOFTFP lets source ask "is this a soft-float build" at compile
    # time, e.g. the lexer parses float literals with softfp_strtod.
    target_compile_definitions(tigryx_core PUBLIC RELIC_SOFTFP=1)
    message(STATUS "Tigryx: soft-float (-msoft-float) ENABLED for tigryx_core and all dependents")
endif()

# Production builds: enable the production splash/sign-on screen.
option(TIGRYX_PRODUCTION "Enable production splash screen" OFF)
if(TIGRYX_PRODUCTION)
    target_compile_definitions(tigryx_core PUBLIC RELIC_PRODUCTION=1)
    message(STATUS "Tigryx: production splash ENABLED")
endif()

# vm.c uses cosf/sinf - on Unix-like systems libm must be linked explicitly.
if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "DOS")
    target_link_libraries(tigryx_core PUBLIC m)
endif()

# On DOS/DJGPP, attach libm.a as an INTERFACE link library on tigryx_core so
# it appears AFTER tigryx_core's own objects on the link line.
#
# Not under TIGRYX_SOFTFP: DJGPP's libm.a is x87 code and its sinf/floorf/...
# collide with the identically-named functions in src/softfp/softfp_math.c.
# Leaving libm.a out also turns any missed double-precision call into a link
# error instead of silently resolving to x87 code.
if(CMAKE_SYSTEM_NAME STREQUAL "DOS" AND DEFINED ENV{DJDIR} AND NOT TIGRYX_SOFTFP)
    target_link_libraries(tigryx_core INTERFACE "$ENV{DJDIR}/lib/libm.a")
endif()

if(MSVC)
    target_compile_options(tigryx_core PRIVATE /W3 /D_CRT_SECURE_NO_WARNINGS /O1 /Gy)
elseif(CMAKE_SYSTEM_NAME STREQUAL "DOS")
    # The interpreter dispatch loop is hot; -O3 measurably helps a 486.
    if(TIGRYX_PROFILE)
        target_compile_options(tigryx_core PRIVATE -Wall -Wno-unused-function -O1 -pg -DRELIC_PROFILE)
    else()
        target_compile_options(tigryx_core PRIVATE -Wall -Wno-unused-function -O3 -fomit-frame-pointer)
    endif()
    # Keep compiler tables small so the toolchain fits in DOS memory.
    # See the RELIC_LOW_MEMORY block in include/tigryx.h.
    target_compile_definitions(tigryx_core PUBLIC RELIC_LOW_MEMORY)
    message(STATUS "Tigryx: RELIC_LOW_MEMORY enabled (DOS)")
else()
    target_compile_options(tigryx_core PRIVATE -Wall -Wno-unused-function -Os)
endif()

# -----------------------------------------------------------------------------
# Backend selection
# -----------------------------------------------------------------------------
set(BACKEND_DIR "${CMAKE_SOURCE_DIR}/src/backends/${TIGRYX_BACKEND}")
if(NOT EXISTS "${BACKEND_DIR}")
    message(FATAL_ERROR "Unknown TIGRYX_BACKEND='${TIGRYX_BACKEND}' (expected src/backends/${TIGRYX_BACKEND}/ to exist)")
endif()

file(GLOB BACKEND_SOURCES "${BACKEND_DIR}/*.c")
# opengl-win32 delegates its platform/clipboard/HTTP hooks to the shared
# Win32 system-services module.
if(TIGRYX_BACKEND STREQUAL "opengl-win32")
    list(APPEND BACKEND_SOURCES "${CMAKE_SOURCE_DIR}/src/backends/syswin32/syswin32.c")
endif()
add_library(tigryx_backend STATIC ${BACKEND_SOURCES})
target_link_libraries(tigryx_backend PUBLIC tigryx_core)
target_compile_definitions(tigryx_backend PRIVATE
    RELIC_PLATFORM_ID="${RELIC_PLATFORM_ID}"
    RELIC_BACKEND_ID="${TIGRYX_BACKEND}"
)

if(MSVC)
    target_compile_options(tigryx_backend PRIVATE /W3 /D_CRT_SECURE_NO_WARNINGS /O1 /Gy)
elseif(CMAKE_SYSTEM_NAME STREQUAL "DOS")
    # The software rasteriser inner loop is performance-critical: -O3
    # inlines the column/span drawers, -fomit-frame-pointer frees a
    # register in the pixel loop.
    if(TIGRYX_PROFILE)
        target_compile_options(tigryx_backend PRIVATE -Wall -O1 -pg -DRELIC_PROFILE)
    else()
        target_compile_options(tigryx_backend PRIVATE -Wall -O3 -fomit-frame-pointer)
    endif()
else()
    target_compile_options(tigryx_backend PRIVATE -Wall -Os)
endif()

if(TIGRYX_BACKEND STREQUAL "opengl-win32")
    target_link_libraries(tigryx_backend PUBLIC winmm user32 gdi32 opengl32 wininet)
elseif(TIGRYX_BACKEND STREQUAL "allegro-dos")
    # Allegro 4 for DJGPP is not part of this release. Place the headers in
    # src/backends/allegro-dos/include/ and liballeg.a in
    # src/backends/allegro-dos/lib/djgpp/ (see README.md).
    target_include_directories(tigryx_backend PRIVATE "${CMAKE_SOURCE_DIR}/src/backends/allegro-dos/include")
    target_link_libraries(tigryx_backend PUBLIC "${CMAKE_SOURCE_DIR}/src/backends/allegro-dos/lib/djgpp/liballeg.a")
    if(TIGRYX_DOSNET)
        target_compile_definitions(tigryx_backend PRIVATE HAVE_DOSNET)
        target_include_directories(tigryx_backend PRIVATE "${CMAKE_SOURCE_DIR}/src/dosnet")
        target_link_libraries(tigryx_backend PUBLIC tdosnet)
        message(STATUS "Tigryx: Allegro DOS backend enabled (with lwIP HTTP)")
    else()
        message(STATUS "Tigryx: Allegro DOS backend enabled")
    endif()
    if(TIGRYX_SOFTFP)
        # Live TTF baking (stb_truetype) needs double-precision geometry that
        # the soft-float runtime does not cover; ship .fnt caches instead.
        target_compile_definitions(tigryx_backend PRIVATE RELIC_NO_TTF_BAKE=1)
        message(STATUS "Tigryx: live TTF baking DISABLED (TIGRYX_SOFTFP) - .fnt caches required")
    endif()
elseif(TIGRYX_BACKEND STREQUAL "allegro-fixed-dos")
    # Fixed-point/soft-float sector renderer for FPU-less DOS machines.
    # Allegro headers/libs are reused from the allegro-dos backend folder.
    target_include_directories(tigryx_backend PRIVATE "${CMAKE_SOURCE_DIR}/src/backends/allegro-dos/include")
    target_link_libraries(tigryx_backend PUBLIC "${CMAKE_SOURCE_DIR}/src/backends/allegro-dos/lib/djgpp/liballeg.a")
    # No live TTF baking on a soft-float target - ship .fnt caches instead.
    target_compile_definitions(tigryx_backend PRIVATE RELIC_NO_TTF_BAKE=1)
    if(TIGRYX_DOSNET)
        target_compile_definitions(tigryx_backend PRIVATE HAVE_DOSNET)
        target_include_directories(tigryx_backend PRIVATE
            "${CMAKE_SOURCE_DIR}/src/backends/allegro-dos"
            "${CMAKE_SOURCE_DIR}/src/dosnet")
        target_link_libraries(tigryx_backend PUBLIC tdosnet)
        message(STATUS "Tigryx: Allegro fixed-point DOS backend enabled (with lwIP HTTP)")
    else()
        message(STATUS "Tigryx: Allegro fixed-point DOS backend enabled")
    endif()
endif()

# -----------------------------------------------------------------------------
# JIT selection
# -----------------------------------------------------------------------------
set(JIT_DIR "${CMAKE_SOURCE_DIR}/src/jits/${TIGRYX_JIT}")
if(NOT EXISTS "${JIT_DIR}")
    message(FATAL_ERROR "Unknown TIGRYX_JIT='${TIGRYX_JIT}' (expected src/jits/${TIGRYX_JIT}/ to exist)")
endif()

file(GLOB JIT_SOURCES "${JIT_DIR}/*.c")
if(NOT JIT_SOURCES)
    message(FATAL_ERROR "No sources found in JIT directory: ${JIT_DIR}")
endif()
add_library(tigryx_jit STATIC ${JIT_SOURCES})
target_link_libraries(tigryx_jit PUBLIC tigryx_core)

if(MSVC)
    target_compile_options(tigryx_jit PRIVATE /W3 /D_CRT_SECURE_NO_WARNINGS /O1 /Gy)
elseif(CMAKE_SYSTEM_NAME STREQUAL "DOS")
    if(TIGRYX_PROFILE)
        target_compile_options(tigryx_jit PRIVATE -Wall -O1 -pg -DRELIC_PROFILE)
    else()
        target_compile_options(tigryx_jit PRIVATE -Wall -O3 -fomit-frame-pointer)
    endif()
else()
    target_compile_options(tigryx_jit PRIVATE -Wall -Os)
endif()

# tigryxpp_lib: the preprocessor as a library (RELIC_PP_NO_MAIN omits
# tigryxpp.c's main(), leaving tigryx_pp_run()). Linked into tigryxri so it
# can preprocess .tgx files in-process. tier.c rides along: the Adaptive
# Build defines (TGX_TIER_*) are resolved inside tigryx_pp_run(); the ids
# feed relic_backend_caps()/TGX_BACKEND.
add_library(tigryxpp_lib STATIC src/preprocessor/tigryxpp.c src/core/tier.c)
target_include_directories(tigryxpp_lib PRIVATE include)
target_compile_definitions(tigryxpp_lib PRIVATE RELIC_PP_NO_MAIN RELIC_PLATFORM_ID="${RELIC_PLATFORM_ID}" RELIC_BACKEND_ID="${TIGRYX_BACKEND}")
if(MSVC)
    target_compile_options(tigryxpp_lib PRIVATE /W3 /D_CRT_SECURE_NO_WARNINGS /O1)
else()
    target_compile_options(tigryxpp_lib PRIVATE -Wall -Os)
endif()

# -----------------------------------------------------------------------------
# Executables
# -----------------------------------------------------------------------------

# tigryxri: interpreter / bytecode compiler
add_executable(tigryxri src/core/main.c)
target_link_libraries(tigryxri PRIVATE tigryx_backend tigryx_jit tigryxpp_lib)
if(CMAKE_SYSTEM_NAME STREQUAL "DOS" AND DEFINED ENV{DJDIR} AND NOT TIGRYX_SOFTFP)
    target_link_libraries(tigryxri PRIVATE "$ENV{DJDIR}/lib/libm.a")
endif()
target_compile_definitions(tigryxri PRIVATE RELIC_PLATFORM_ID="${RELIC_PLATFORM_ID}" RELIC_BACKEND_ID="${TIGRYX_BACKEND}")
if(MSVC)
    target_link_options(tigryxri PRIVATE /OPT:REF /OPT:ICF /OPT:LBR)
elseif(APPLE)
    target_link_options(tigryxri PRIVATE -Wl,-dead_strip)
else()
    # Console subsystem on Windows too: tigryxri is always spawned by
    # tigryx, which owns the console-hiding decision.
    target_link_options(tigryxri PRIVATE -Wl,--gc-sections)
endif()

# tigryxvm: standalone VM host - runs .tlx bytecode directly (tigryx.c
# driver + web_bridge.c embedded-runtime shim).
add_executable(tigryxvm src/core/tigryx.c src/core/web_bridge.c)
target_link_libraries(tigryxvm PRIVATE tigryx_backend tigryx_jit)
if(CMAKE_SYSTEM_NAME STREQUAL "DOS" AND DEFINED ENV{DJDIR} AND NOT TIGRYX_SOFTFP)
    target_link_libraries(tigryxvm PRIVATE "$ENV{DJDIR}/lib/libm.a")
endif()
target_compile_definitions(tigryxvm PRIVATE RELIC_PLATFORM_ID="${RELIC_PLATFORM_ID}" RELIC_BACKEND_ID="${TIGRYX_BACKEND}")
if(MSVC)
    target_link_options(tigryxvm PRIVATE /OPT:REF /OPT:ICF /OPT:LBR)
elseif(APPLE)
    target_link_options(tigryxvm PRIVATE -Wl,-dead_strip)
else()
    target_link_options(tigryxvm PRIVATE -Wl,--gc-sections)
endif()

# tigryxpp: standalone preprocessor, no backend/JIT dependency.
# tier.c supplies relic_tier_*()/relic_backend_caps() for the Adaptive
# Build defines; RELIC_BACKEND_ID feeds relic_backend_caps()/TGX_BACKEND.
add_executable(tigryxpp src/preprocessor/tigryxpp.c src/core/tier.c)
target_include_directories(tigryxpp PRIVATE include)
target_compile_definitions(tigryxpp PRIVATE RELIC_PLATFORM_ID="${RELIC_PLATFORM_ID}" RELIC_BACKEND_ID="${TIGRYX_BACKEND}")
if(MSVC)
    target_compile_options(tigryxpp PRIVATE /W3 /D_CRT_SECURE_NO_WARNINGS /O1 /Gy)
    target_link_options(tigryxpp PRIVATE /OPT:REF /OPT:ICF /OPT:LBR)
elseif(APPLE)
    target_compile_options(tigryxpp PRIVATE -Wall -Os)
    target_link_options(tigryxpp PRIVATE -Wl,-dead_strip)
else()
    target_compile_options(tigryxpp PRIVATE -Wall -Os)
    target_link_options(tigryxpp PRIVATE -Wl,--gc-sections)
endif()

# tigryx: dispatcher
add_executable(tigryx src/core/tigryx_main.c)
target_link_libraries(tigryx PRIVATE tigryx_core)
if(CMAKE_SYSTEM_NAME STREQUAL "DOS" AND DEFINED ENV{DJDIR} AND NOT TIGRYX_SOFTFP)
    target_link_libraries(tigryx PRIVATE "$ENV{DJDIR}/lib/libm.a")
endif()
target_compile_definitions(tigryx PRIVATE RELIC_PLATFORM_ID="${RELIC_PLATFORM_ID}" RELIC_BACKEND_ID="${TIGRYX_BACKEND}")
if(MSVC)
    target_compile_options(tigryx PRIVATE /W3 /D_CRT_SECURE_NO_WARNINGS /O1 /Gy)
    target_link_options(tigryx PRIVATE /OPT:REF /OPT:ICF /OPT:LBR)
elseif(APPLE)
    target_compile_options(tigryx PRIVATE -Wall -Os)
    target_link_options(tigryx PRIVATE -Wl,-dead_strip)
else()
    target_compile_options(tigryx PRIVATE -Wall -Os)
    target_link_options(tigryx PRIVATE -Wl,--gc-sections)
endif()

# tigryxw: GUI-subsystem twin of tigryx, for Explorer / file-association
# launches - a GUI-subsystem binary never gets a console window created by
# Windows, so there is nothing to flash and nothing to hide.
if(WIN32)
    add_executable(tigryxw src/core/tigryx_main.c)
    target_link_libraries(tigryxw PRIVATE tigryx_core)
    target_compile_definitions(tigryxw PRIVATE RELIC_PLATFORM_ID="${RELIC_PLATFORM_ID}" RELIC_BACKEND_ID="${TIGRYX_BACKEND}")
    if(MSVC)
        target_compile_options(tigryxw PRIVATE /W3 /D_CRT_SECURE_NO_WARNINGS /O1 /Gy)
        target_link_options(tigryxw PRIVATE /OPT:REF /OPT:ICF /OPT:LBR /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    else()
        target_compile_options(tigryxw PRIVATE -Wall -Os)
        target_link_options(tigryxw PRIVATE -Wl,--gc-sections -mwindows)
    endif()
endif()

# Binaries land in bin/<platform-id>/ so multiple platform builds coexist.
foreach(_t tigryx tigryxw tigryxri tigryxvm tigryxpp)
    if(TARGET ${_t})
        set_target_properties(${_t} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY                "${CMAKE_SOURCE_DIR}/bin/${RELIC_PLATFORM_ID}"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_SOURCE_DIR}/bin/${RELIC_PLATFORM_ID}"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_SOURCE_DIR}/bin/${RELIC_PLATFORM_ID}"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/bin/${RELIC_PLATFORM_ID}")
    endif()
endforeach()

message(STATUS "Tigryx: platform-id = ${RELIC_PLATFORM_ID}")
message(STATUS "Tigryx: backend     = ${TIGRYX_BACKEND}")
message(STATUS "Tigryx: jit         = ${TIGRYX_JIT}")
message(STATUS "Tigryx: output dir  = ${CMAKE_SOURCE_DIR}/bin/${RELIC_PLATFORM_ID}")
