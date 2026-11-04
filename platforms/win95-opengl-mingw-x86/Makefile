# Copyright (c) 2026 Scintillae Ltd. All rights reserved.

# Makefile for tigryx - Windows 95/98/ME/XP build with legacy MinGW (GCC)
# Backend: opengl-win32 (OpenGL graphics with TrueType fonts)
# JIT:     x86-win (32-bit x86 code generation)
#
# Invoke from the REPO ROOT (all source paths are repo-root relative):
#   make -f platforms/win95-opengl-mingw-x86/Makefile
#
# This is a self-contained Makefile build (no CMake) so it works under the
# legacy MSYS 1.0 shell that ships with old MinGW on Windows 9x hosts.

# =============================================================================
# Platform Configuration
# =============================================================================
PLATFORM_ID = win95-opengl-mingw-x86
BACKEND_ID  = opengl-win32

# Subsystem version baked into the PE header (4.0 = Windows 95+).
SUBSYS_MAJOR = 4
SUBSYS_MINOR = 0

# =============================================================================
# Compiler and Tools
# =============================================================================
CC = gcc
AR = ar
RANLIB = ranlib
STRIP = strip
WINDRES = windres
# Legacy windres invokes its preprocessor using the absolute path of gcc, which
# breaks when MinGW lives under "C:\Program Files (x86)" (cmd.exe splits at the
# space). Force a space-free command resolved through PATH instead.
WINDRES_PP = '--preprocessor=gcc -E -xc-header -DRC_INVOKED'

# =============================================================================
# Compiler Flags
# =============================================================================
CFLAGS = -Wall -Wno-unused-function -Os -std=c99
CFLAGS += -D_WIN32_WINNT=0x0400 -DWINVER=0x0400
CFLAGS += -DRELIC_PLATFORM_ID=\"$(PLATFORM_ID)\"
CFLAGS += -DRELIC_BACKEND_ID=\"$(BACKEND_ID)\"
CFLAGS += -DRELIC_WIN95
# stb_image is only decoded on the main thread; dropping __thread keeps the
# rt archive from pulling emutls/libgcc_eh at link time.
CFLAGS += -DSTBI_NO_THREAD_LOCALS
# Appended last so callers can add flags without clobbering the above.
# Never pass CFLAGS= on the make command line: that overrides every
# assignment here, silently dropping RELIC_WIN95 / the platform IDs.
CFLAGS += $(EXTRA_CFLAGS)

# =============================================================================
# Include Directories
# =============================================================================
INCLUDE_DIRS = -Iinclude -Ithirdparty/miniz

# =============================================================================
# Source Files
# =============================================================================
CORE_SOURCES = src/core/lexer.c \
               src/core/parser.c \
               src/core/typecheck.c \
               src/core/ir.c \
               src/core/vm.c \
               src/core/mvm.c \
               src/core/debug.c \
               src/core/console_win.c \
               src/core/pp_tmp.c \
               src/core/compile.c \
               src/core/far16.c \
               src/core/tier.c \
               thirdparty/miniz/miniz.c

BACKEND_SOURCES = src/backends/opengl-win32/backend_opengl_win32.c \
                  src/backends/opengl-win32/musdec.c \
                  src/backends/syswin32/syswin32.c

JIT_SOURCES = src/jits/x86-win/jit_x86_win.c

# tigryxri: interpreter / bytecode compiler
TIGRYX_RI_SOURCES = src/core/main.c

# tigryxvm: VM runtime for bound executables
TIGRYX_VM_SOURCES = src/core/tigryx.c \
                   src/core/web_bridge.c

# tigryxpp: preprocessor (tier.c supplies the Adaptive Build
# relic_tier_*()/relic_backend_caps() used for the TGX_TIER_* defines)
TIGRYX_PP_SOURCES = src/preprocessor/tigryxpp.c \
                    src/core/tier.c

# tigryx: public dispatcher
TIGRYX_SOURCES = src/core/tigryx_main.c

# =============================================================================
# Libraries
# =============================================================================
BACKEND_LIBS = -lwinmm -luser32 -lgdi32 -lopengl32 -lwininet
MATH_LIB = -lm

# =============================================================================
# Linker Flags
# =============================================================================
LDFLAGS = -Wl,--major-subsystem-version,$(SUBSYS_MAJOR) -Wl,--minor-subsystem-version,$(SUBSYS_MINOR)
LDFLAGS += $(EXTRA_LDFLAGS)

# Link flags for the GUI-subsystem twins (tigryxw, tigryxriw, tigryxvmw,
# tigryxppw). --subsystem,windows is passed to ld EXPLICITLY and is not
# optional: legacy MinGW's -mwindows only adds the GUI import libraries, it
# does not set the PE subsystem, so on its own it loses to the
# -Wl,--subsystem=console that build.ps1 puts in EXTRA_LDFLAGS - which
# silently produced CONSOLE-subsystem "GUI" twins that Windows handed a
# console window to before main() even ran. Appended after $(LDFLAGS) so this
# assignment is the last one ld sees.
GUI_LDFLAGS = -mwindows -Wl,--subsystem,windows

# =============================================================================
# Build Directories
# =============================================================================
BUILD_DIR = build/$(PLATFORM_ID)
BIN_DIR = bin/$(PLATFORM_ID)
BACKEND_DIRS = $(dir $(BACKEND_SOURCES))

# =============================================================================
# Object Files (must come after BUILD_DIR is defined)
# =============================================================================
CORE_OBJECTS = $(CORE_SOURCES:.c=.o)
CORE_OBJECTS := $(addprefix $(BUILD_DIR)/,$(CORE_OBJECTS))

BACKEND_OBJECTS = $(BACKEND_SOURCES:.c=.o)
BACKEND_OBJECTS := $(addprefix $(BUILD_DIR)/,$(BACKEND_OBJECTS))

JIT_OBJECTS = $(JIT_SOURCES:.c=.o)
JIT_OBJECTS := $(addprefix $(BUILD_DIR)/,$(JIT_OBJECTS))

TIGRYX_RI_OBJECTS = $(TIGRYX_RI_SOURCES:.c=.o)
TIGRYX_RI_OBJECTS := $(addprefix $(BUILD_DIR)/,$(TIGRYX_RI_OBJECTS))

TIGRYX_VM_OBJECTS = $(TIGRYX_VM_SOURCES:.c=.o)
TIGRYX_VM_OBJECTS := $(addprefix $(BUILD_DIR)/,$(TIGRYX_VM_OBJECTS))

TIGRYX_PP_OBJECTS = $(TIGRYX_PP_SOURCES:.c=.o)
TIGRYX_PP_OBJECTS := $(addprefix $(BUILD_DIR)/,$(TIGRYX_PP_OBJECTS))

TIGRYX_OBJECTS = $(TIGRYX_SOURCES:.c=.o)
TIGRYX_OBJECTS := $(addprefix $(BUILD_DIR)/,$(TIGRYX_OBJECTS))

# =============================================================================
# Targets
# =============================================================================
.PHONY: all clean dirs

all: dirs $(BIN_DIR)/tigryxri $(BIN_DIR)/tigryxriw $(BIN_DIR)/tigryxvm $(BIN_DIR)/tigryxvmw \
	$(BIN_DIR)/tigryxpp $(BIN_DIR)/tigryxppw $(BIN_DIR)/tigryx $(BIN_DIR)/tigryxw

dirs:
	@mkdir -p $(BUILD_DIR)/src/core
	@for d in $(BACKEND_DIRS); do mkdir -p $(BUILD_DIR)/$$d; done
	@mkdir -p $(BUILD_DIR)/src/jits/x86-win
	@mkdir -p $(BUILD_DIR)/src/preprocessor
	@mkdir -p $(BUILD_DIR)/thirdparty/miniz
	@mkdir -p $(BIN_DIR)

# =============================================================================
# Core Library
# =============================================================================
$(BUILD_DIR)/src/core/%.o: src/core/%.c
	$(CC) $(CFLAGS) $(INCLUDE_DIRS) -c $< -o $@

$(BUILD_DIR)/thirdparty/miniz/%.o: thirdparty/miniz/%.c
	@mkdir -p $(BUILD_DIR)/thirdparty/miniz
	$(CC) $(CFLAGS) -DMINIZ_NO_DEFLATE_APIS -DMINIZ_NO_TIME -Wno-unused-function $(INCLUDE_DIRS) -c $< -o $@

# =============================================================================
# Backend
# =============================================================================
$(BUILD_DIR)/src/backends/opengl-win32/%.o: src/backends/opengl-win32/%.c
	$(CC) $(CFLAGS) $(INCLUDE_DIRS) -c $< -o $@

$(BUILD_DIR)/src/backends/syswin32/%.o: src/backends/syswin32/%.c
	@mkdir -p $(BUILD_DIR)/src/backends/syswin32
	$(CC) $(CFLAGS) $(INCLUDE_DIRS) -c $< -o $@

# =============================================================================
# JIT
# =============================================================================
$(BUILD_DIR)/src/jits/x86-win/%.o: src/jits/x86-win/%.c
	$(CC) $(CFLAGS) $(INCLUDE_DIRS) -c $< -o $@

# =============================================================================
# Preprocessor
# =============================================================================
$(BUILD_DIR)/src/preprocessor/%.o: src/preprocessor/%.c
	$(CC) $(CFLAGS) $(INCLUDE_DIRS) -c $< -o $@

# Build core library
$(BUILD_DIR)/libtigryx_core.a: $(CORE_OBJECTS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

# Build backend library
$(BUILD_DIR)/libtigryx_backend.a: $(BACKEND_OBJECTS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

# Build JIT library
$(BUILD_DIR)/libtigryx_jit.a: $(JIT_OBJECTS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

# Windows icon resource - optional. The .ico files are trademarked
# brand assets and may be absent from a source release; when
# icons/win95/ is missing the binaries simply link without an
# embedded icon. When present it uses BMP-format ICOs (not PNG),
# the only format Win95/98/ME's shell and GDI can read.
RESOURCE_OBJ :=
ifneq ($(wildcard icons/win95/tigryx.rc),)
RESOURCE_OBJ := $(BUILD_DIR)/tigryx_resource.o
$(BUILD_DIR)/tigryx_resource.o: icons/win95/tigryx.rc icons/win95/tigryx.ico icons/win95/tigryx-tgx.ico icons/win95/tigryx-tlx.ico icons/win95/tigryx-tla.ico
	@mkdir -p $(BUILD_DIR)
	$(WINDRES) $(WINDRES_PP) -Iicons/win95 -o $@ icons/win95/tigryx.rc
endif

# tigryxri: interpreter
$(BIN_DIR)/tigryxri: $(TIGRYX_RI_OBJECTS) $(BUILD_DIR)/libtigryx_core.a $(BUILD_DIR)/libtigryx_backend.a $(BUILD_DIR)/libtigryx_jit.a $(RESOURCE_OBJ)
	mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(BACKEND_LIBS) $(MATH_LIB)
	-$(STRIP) $@ 2>/dev/null || $(STRIP) $@.exe

# tigryxriw: GUI-subsystem twin of tigryxri. THIS is what the installer
# registers for .tp/.tgx/.tlx/.tla on Win9x, so a double-clicked script is run
# by a single GUI-subsystem process and Windows never creates a console for
# it. On Windows 95/98/ME the console-hiding tricks that work on NT are not
# available - GetConsoleWindow()/AttachConsole() don't exist, CREATE_NO_WINDOW
# is ignored, and DETACHED_PROCESS is documented-buggy - so the PE subsystem
# of the launched image is the only reliable guarantee.
$(BIN_DIR)/tigryxriw: $(TIGRYX_RI_OBJECTS) $(BUILD_DIR)/libtigryx_core.a $(BUILD_DIR)/libtigryx_backend.a $(BUILD_DIR)/libtigryx_jit.a $(RESOURCE_OBJ)
	mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $(GUI_LDFLAGS) -o $@ $^ $(BACKEND_LIBS) $(MATH_LIB)
	-$(STRIP) $@ 2>/dev/null || $(STRIP) $@.exe

# tigryxvm: VM runtime
$(BIN_DIR)/tigryxvm: $(TIGRYX_VM_OBJECTS) $(BUILD_DIR)/libtigryx_core.a $(BUILD_DIR)/libtigryx_backend.a $(BUILD_DIR)/libtigryx_jit.a $(RESOURCE_OBJ)
	mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(BACKEND_LIBS) $(MATH_LIB)
	-$(STRIP) $@ 2>/dev/null || $(STRIP) $@.exe

# tigryxvmw: GUI-subsystem twin of tigryxvm. `tigryx bind --no-console` copies
# this as the stub, so a bound executable never gets a console either.
$(BIN_DIR)/tigryxvmw: $(TIGRYX_VM_OBJECTS) $(BUILD_DIR)/libtigryx_core.a $(BUILD_DIR)/libtigryx_backend.a $(BUILD_DIR)/libtigryx_jit.a $(RESOURCE_OBJ)
	mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $(GUI_LDFLAGS) -o $@ $^ $(BACKEND_LIBS) $(MATH_LIB)
	-$(STRIP) $@ 2>/dev/null || $(STRIP) $@.exe

# tigryxpp: preprocessor
$(BIN_DIR)/tigryxpp: $(TIGRYX_PP_OBJECTS) $(RESOURCE_OBJ)
	mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $^
	-$(STRIP) $@ 2>/dev/null || $(STRIP) $@.exe

# tigryxppw: GUI-subsystem twin of tigryxpp, spawned by tigryxriw/tigryxw when
# no console may be shown. Without it, preprocessing a double-clicked .tgx
# would flash a console window for the preprocessor child on Win9x.
$(BIN_DIR)/tigryxppw: $(TIGRYX_PP_OBJECTS) $(RESOURCE_OBJ)
	mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $(GUI_LDFLAGS) -o $@ $^
	-$(STRIP) $@ 2>/dev/null || $(STRIP) $@.exe

# tigryx: public dispatcher
$(BIN_DIR)/tigryx: $(TIGRYX_OBJECTS) $(BUILD_DIR)/libtigryx_core.a $(RESOURCE_OBJ)
	mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(MATH_LIB)
	-$(STRIP) $@ 2>/dev/null || $(STRIP) $@.exe

# tigryxw: GUI-subsystem twin of tigryx, for Start Menu / shortcut launches.
# Same source, same behaviour - the only difference is the PE subsystem (see
# GUI_LDFLAGS), which prevents Windows from creating a console window before
# main() runs. File associations go to tigryxriw instead (one process, not
# three); tigryx.exe stays CONSOLE so `tigryx ...` at a prompt behaves like a
# normal CLI tool.
$(BIN_DIR)/tigryxw: $(TIGRYX_OBJECTS) $(BUILD_DIR)/libtigryx_core.a $(RESOURCE_OBJ)
	mkdir -p $(BIN_DIR)
	$(CC) $(LDFLAGS) $(GUI_LDFLAGS) -o $@ $^ $(MATH_LIB)
	-$(STRIP) $@ 2>/dev/null || $(STRIP) $@.exe

# =============================================================================
# Clean
# =============================================================================
clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(BIN_DIR)
