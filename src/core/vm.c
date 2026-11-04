/*
 * vm.c
 *
 * Copyright (c) 2026 Scintillae Ltd. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * vm.c - Relic interpreter / threaded VM
 */
#include "tigryx.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#ifndef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 0
#endif
#include <mach-o/dyld.h>
#endif

#if defined(RELIC_DOS)
/* DOS-only: optional runtime override for the backend screen colour depth.
 *
 * Set by mains (relic / relic-ri / relic-vm) when the user passes
 *   --colour-depth N    (N in {8,16,24,32})
 * on the command line. Only the DOS+TinyGL backend currently honours it
 * (see src/backends/allegro-tinygl-dos/backend_allegro_tinygl_dos.c);
 * other DOS backends (allegro-dos) simply never read this symbol. Zero
 * means "no override: use the backend's default depth ladder". This
 * symbol is gated to DJGPP builds so the 30+ non-DOS backends are not
 * polluted with platform-specific knobs. */
int g_relic_preferred_color_depth = 0;

/* DOS-only: physical mode override for WINFLAG_STRETCH windows, set by
 * `--res WxH`. Zero means "no override: try the request-snapped mode
 * first, then step down". Ignored entirely without WINFLAG_STRETCH. */
int g_relic_stretch_mode_w = 0;
int g_relic_stretch_mode_h = 0;

/* DOS-only: opt-in to skipping the colour-buffer clear in gfx_clear when
 * a 3D camera is active. Set by `--skip-clear`. Useful for samples whose
 * 3D scene fully covers the framebuffer every frame (e.g. tdoom's BSP
 * walk paints a sky cylinder + walls + floors + ceilings everywhere) --
 * the per-frame full-screen colour memset is then pure waste and skipping
 * it can buy several ms per frame. The depth-buffer clear is always
 * preserved; only the colour clear is skipped. Programs whose 3D scene
 * leaves background pixels visible will show stale frame N-1 pixels,
 * so this is opt-in per program. Default 0 keeps the safe behaviour.
 * Both allegro-tinygl-dos and allegro-dos honour this flag. */
int g_relic_skip_clear = 0;

/* DOS-only: opt-in 3D rendering profiler. Set by `--gfx-prof`.
 * When set, the allegro-dos backend accumulates per-frame counters
 * (mesh calls, vertices transformed, triangles considered/rasterised,
 * scanlines, span pixels, pixels written) and dumps them to TIGRYX.LOG
 * at shutdown.  Zero overhead when the flag is absent. */
int g_relic_gfx_prof = 0;
#endif

#if defined(RELIC_RISCOS)
/* RISC OS-only: force the @window fullscreen flag for every program.
 * Set by `--fullscreen`; lets the windowed test suite run in a claimed
 * games mode (MODE 13/28) without editing each test. Only the
 * vidc-riscos backend reads it. */
int g_relic_fullscreen = 0;
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#ifdef __DJGPP__
#include <dpmi.h>               /* heap clamp in relic_vm_init */
#endif

/* stb_image for @file_load_png — PNG decoding in the VM core.
 *
 * When compiled into the interpreter (tigryxri, tigryxvm, etc.) we
 * include the implementation here. When compiled by libtcc alongside
 * tigryx_rc_shim.c (which has its own STB_IMAGE_IMPLEMENTATION), we
 * skip the implementation to avoid duplicate symbols. Not built on
 * RELIC_IA16: PNG decode buffers (RGBA) cannot fit 16-bit DOS, so
 * @file_load_png reports failure there instead. */
#if !defined(REIC_SHIM_HAS_STB_IMAGE) && !defined(RELIC_IA16)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_GIF
#define STBI_NO_PNM
#define STBI_NO_BMP
#define STBI_NO_JPEG
#ifdef RELIC_SOFTFP
/* STBI_NO_HDR above only removes the ability to read the .hdr FILE FORMAT;
 * stbi_loadf() (load-as-float, with LDR->float gamma decode via pow()) is a
 * separate API that stays compiled in regardless, and nothing in this
 * codebase calls it -- confirmed by grep before adding this guard. It is
 * still part of relic_core's linked-in stb_image surface, though, so a
 * user's own native shim code calling stbi_loadf() directly would reach
 * live double-precision pow() and trap on an FPU-less machine. STBI_NO_LINEAR
 * removes stbi_loadf() and its gamma tables entirely, closing that off
 * rather than relying on it staying provably dead. */
#define STBI_NO_LINEAR
#endif
/* stb_image is third-party code; silence its unused-function warnings
 * locally rather than patching the header. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#else
#include "stb_image.h"
#endif

#ifndef RELIC_IA16
/* stbi_io_callbacks for streaming a FILE* through stb_image — used by
 * @file_load_png so the decode doesn't need a whole-file buffer on the
 * libc heap (fragmentation on low-memory targets). */
static int relic_stbi_fread(void *user, char *data, int size) {
    return (int)fread(data, 1, (size_t)size, (FILE *)user);
}
static void relic_stbi_fskip(void *user, int n) {
    fseek((FILE *)user, n, SEEK_CUR);
}
static int relic_stbi_feof(void *user) {
    return feof((FILE *)user);
}
#endif

/* miniz for @unzip — ZIP archive extraction in the VM core.
 * We disable deflate/compression and archive writing to minimise the
 * compiled footprint; only inflate + archive reading remain. The build
 * system already passes these as -D on some targets; guard so neither
 * side redefines. */
#ifndef MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_DEFLATE_APIS
#endif
#ifndef MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#endif
#ifndef MINIZ_NO_TIME
#define MINIZ_NO_TIME
#endif
#include "miniz.h"

/* Platform-specific headers for directory/path operations */
#ifdef _WIN32
  /* Needed by @register (registry / shell integration). Every Win32 entry
   * point it uses beyond kernel32 is resolved at runtime via GetProcAddress,
   * so this header contributes types and constants only -- no new import
   * libraries. That matters because `tigryx` and `tigryxpp` link no Windows
   * libraries at all, yet both contain vm.o. */
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #ifndef RELIC_WIN95
    #include <io.h>
    #include <direct.h>
  #else
    /* Legacy MinGW (GCC 4.7): io.h and direct.h both pull in off64_t
     * which doesn't exist in this toolchain. Declare what we need. */
    struct _finddata_t {
        unsigned attrib;
        long       time_create;
        long       time_access;
        long       time_write;
        long       size;
        char       name[260];
    };
    intptr_t _findfirst(const char *filespec, struct _finddata_t *fileinfo);
    int _findnext(intptr_t handle, struct _finddata_t *fileinfo);
    int _findclose(intptr_t handle);
    char *_getcwd(char *buf, int size);
    int _access(const char *path, int mode);
    int _mkdir(const char *path);
    int _rmdir(const char *path);
    #define _A_SUBDIR 0x10
    #define _A_RDONLY 0x01
  #endif
  #define R_GETCWD _getcwd
  #define R_ACCESS _access
  #define R_F_OK 0
  #define R_W_OK 2
  #define R_MKDIR(p) _mkdir(p)
  #define R_RMDIR(p) _rmdir(p)
#else
  #include <unistd.h>
  #include <dirent.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <errno.h>
  #define R_GETCWD getcwd
  #define R_ACCESS access
  #define R_F_OK F_OK
  #define R_W_OK W_OK
  #define R_MKDIR(p) mkdir(p, 0755)
  #define R_RMDIR(p) rmdir(p)
#endif

/* -------------------------------------------------------------------------
 * Float bit-cast helper (single precision)
 * ---------------------------------------------------------------------- */
typedef union { i32 i; float f; } RelicFBits;

/* -------------------------------------------------------------------------
 * Unified file-handle table - one per RelicVM (see VMFileHandle in
 * tigryx.h). Each slot is either an asset cursor (is_asset=true,
 * fp=NULL) or a real host FILE* (is_asset=false); the Relic program
 * sees an opaque i32 index. Per-VM so co-resident VMs in the TigryxOS
 * host cannot collide.
 * ---------------------------------------------------------------------- */
static i32 vm_handle_alloc_asset(RelicVM *vm, const u8 RELIC_FAR *data, u32 size) {
    for(int i=1;i<VM_MAX_FILES;i++) {
        if(!vm->files[i].fp && !vm->files[i].asset_data) {
            vm->files[i].is_asset   = true;
            vm->files[i].fp         = NULL;
            vm->files[i].asset_data = data;
            vm->files[i].asset_size = size;
            vm->files[i].asset_pos  = 0;
            return (i32)i;
        }
    }
    return -1;
}
static i32 vm_handle_alloc_file(RelicVM *vm, FILE *f) {
    for(int i=1;i<VM_MAX_FILES;i++) {
        if(!vm->files[i].fp && !vm->files[i].asset_data) {
            vm->files[i].is_asset   = false;
            vm->files[i].fp         = f;
            vm->files[i].asset_data = NULL;
            vm->files[i].asset_size = 0;
            vm->files[i].asset_pos  = 0;
            return (i32)i;
        }
    }
    return -1;
}
static VMFileHandle *vm_handle_get(RelicVM *vm, i32 idx) {
    if(idx<1||idx>=VM_MAX_FILES) return NULL;
    if(!vm->files[idx].fp && !vm->files[idx].asset_data) return NULL;
    return &vm->files[idx];
}
static void vm_handle_free(RelicVM *vm, i32 idx) {
    if(idx<1||idx>=VM_MAX_FILES) return;
    if(vm->files[idx].fp) fclose(vm->files[idx].fp);
    memset(&vm->files[idx], 0, sizeof(vm->files[idx]));
}

/* -------------------------------------------------------------------------
 * Far-memory helpers live in src/core/far16.c - on ia16 they do real-mode
 * DOS allocation and 64K-safe copies; elsewhere they are plain
 * malloc/memcpy/fread wrappers. See tigryx.h for the declarations.
 * ---------------------------------------------------------------------- */
#ifdef RELIC_IA16
#include <libi86/string.h>   /* _fmemcpy, _fmemset, _fmemmove */
#endif

/* File I/O into/out of guest memory: on ia16 libc stdio only takes near
 * pointers, so these bounce through the far16.c helpers. */
static size_t vm_fread_mem(RelicVM *vm, u32 off, u32 n, FILE *fp) {
    return relic_fread_far(fp, relic_fp_add(vm->mem, off), n);
}
static size_t vm_fwrite_mem(RelicVM *vm, u32 off, u32 n, FILE *fp) {
    return relic_fwrite_far(fp, relic_cfp_add(vm->mem, off), n);
}

/* -------------------------------------------------------------------------
 * Memory helpers (all accesses go through these to support big-endian ports)
 * ---------------------------------------------------------------------- */
static inline i32 mem_read32(RelicVM *vm, u32 addr) {
    if(addr + 4 > vm->mem_sz) return 0;
    i32 v;
    relic_far_read(vm->mem, addr, &v, 4);
    return v;
}
static inline void mem_write32(RelicVM *vm, u32 addr, i32 val) {
    if(addr + 4 > vm->mem_sz) return;
    relic_far_write(vm->mem, addr, &val, 4);
}
static inline double mem_read64(RelicVM *vm, u32 addr) {
    /* Byte copy into a host double, like mem_read32: host-endian on
     * purpose, so DLD/DST round-trip what DST wrote regardless of the
     * platform's byte order. Except on FPA: the FPA double is mixed
     * endian (high word first), while guest memory must stay canonical
     * little-endian - @memcpy reads of &x must match other platforms -
     * so the two u32 words are swapped across the boundary. */
    if(addr + 8 > vm->mem_sz) return 0.0;
#if defined(RELIC_RISCOS_FPA)
    u32 w[2];
    relic_far_read(vm->mem, addr, w, 8);
    u32 t = w[0]; w[0] = w[1]; w[1] = t;
    double v;
    memcpy(&v, w, 8);
    return v;
#else
    double v;
    relic_far_read(vm->mem, addr, &v, 8);
    return v;
#endif
}
static inline void mem_write64(RelicVM *vm, u32 addr, double val) {
    if(addr + 8 > vm->mem_sz) return;
#if defined(RELIC_RISCOS_FPA)
    u32 w[2];
    memcpy(w, &val, 8);
    u32 t = w[0]; w[0] = w[1]; w[1] = t;
    relic_far_write(vm->mem, addr, w, 8);
#else
    relic_far_write(vm->mem, addr, &val, 8);
#endif
}
static inline u8 mem_read8(RelicVM *vm, u32 addr) {
    if(addr >= vm->mem_sz) return 0;
    return VMMEM8(vm, addr);
}
static inline void mem_write8(RelicVM *vm, u32 addr, u8 val) {
    if(addr >= vm->mem_sz) return;
    VMMEM8(vm, addr) = val;
}
static inline u16 mem_read16(RelicVM *vm, u32 addr) {
    if(addr + 2 > vm->mem_sz) return 0;
    u16 v;
    relic_far_read(vm->mem, addr, &v, 2);
    return v;
}
static inline void mem_write16(RelicVM *vm, u32 addr, u16 val) {
    if(addr + 2 > vm->mem_sz) return;
    relic_far_write(vm->mem, addr, &val, 2);
}

/* -------------------------------------------------------------------------
 * Stack helpers
 * ---------------------------------------------------------------------- */
static inline void stack_push(RelicVM *vm, i32 val) {
    vm->sp -= 4;
    mem_write32(vm, vm->sp, val);
}
static inline i32 stack_pop(RelicVM *vm) {
    i32 v = mem_read32(vm, vm->sp);
    vm->sp += 4;
    return v;
}

/* -------------------------------------------------------------------------
 * Register access (r0 is always 0)
 * ---------------------------------------------------------------------- */
/* Special register indices (must match REG_* in ir.c):
 *   r0  -> always zero
 *   r14 -> frame pointer  (lives in vm->fp, NOT in vm->r[])
 *   r15 -> stack pointer  (lives in vm->sp, NOT in vm->r[])
 * Reads of these return the live vm->fp / vm->sp so that IR generated
 * with `LD32/ST32 r?, r14, off` uses the actual frame pointer. */
#define VM_REG_FP   14
#define VM_REG_SP   15

static inline i32 reg_r(RelicVM *vm, u8 idx) {
    if(idx == 0)         return 0;
    if(idx == VM_REG_FP) return (i32)vm->fp;
    if(idx == VM_REG_SP) return (i32)vm->sp;
    return vm->r[idx & 0xF];
}
static inline void reg_w(RelicVM *vm, u8 idx, i32 val) {
    if(idx == 0)         return;
    if(idx == VM_REG_FP) { vm->fp = (u32)val; return; }
    if(idx == VM_REG_SP) { vm->sp = (u32)val; return; }
    vm->r[idx & 0xF] = val;
}

/* -------------------------------------------------------------------------
 * VM init / free
 * ---------------------------------------------------------------------- */
bool relic_vm_init(RelicVM *vm, IRBuffer *ir, RelicBackend *backend, u32 mem_sz) {
    memset(vm, 0, sizeof(*vm));
#ifdef RELIC_IA16
    /* Real mode: conventional memory is whatever DOS left after loading
     * the exe image and our far allocations (~200-400K on a 640K machine).
     * Clamp the guest heap to what is actually available, keeping a small
     * reserve for the C library / stdio buffers. */
    {
        u32 avail = relic_far_avail();
        if(avail > 0x8000uL) avail -= 0x8000uL;   /* keep 32K in reserve */
        if(mem_sz > avail) mem_sz = avail;
    }
    vm->mem = relic_far_alloc(mem_sz);
    if(vm->mem) relic_far_set(vm->mem, 0, 0, mem_sz);   /* allocmem doesn't zero */
#elif defined(RELIC_TOS)
    /* Clamp the guest heap to the largest free GEMDOS block, keeping a
     * reserve for the C library.  When the machine has no TT-RAM the
     * heap shares ST-RAM with the VIDEL framebuffers, so reserve enough
     * for two screen pages as well: the largest mode the backend sets
     * is 640x480x16bpp = 600K per page, so 2 x 600K + slack = 2MB.
     * Under-reserving is silently bad - the backend falls back to a
     * single page and loses double buffering (on TT-RAM machines the
     * heap prefers TT-RAM and Mxalloc ST-RAM pages stay free). */
    {
        extern long tos_mxalloc(long size, short mode); /* backend helper */
        long avail  = tos_mxalloc(-1L, 3);              /* prefer TT-RAM  */
        long has_tt = tos_mxalloc(-1L, 1);              /* >0 = TT-RAM    */
        /* The libc share covers malloc-heavy builtins too: stb_image
         * needs ~file+RGBA+window bytes (the 800x600 PNG in
         * tests/language/T09B is ~3MB) so 256K starved it -
         * @file_load_png returned -1 with the file and guest heap
         * both fine. */
        long reserve = 4096L*1024L + (has_tt > 0 ? 0L : 2048L*1024L);
        if(avail > 0) {
            if(avail > reserve) avail -= reserve;
            if((long)mem_sz > avail) mem_sz = (u32)avail;
        }
    }
    vm->mem = calloc(1, mem_sz);
#elif defined(__DJGPP__)
    /* No paging on purpose: the Tigryx DOS extender commits memory up
     * front (paging made low-RAM machines crawl), so a heap larger than
     * free memory just fails. Clamp it to the largest free DPMI block,
     * keeping a reserve for libc/Allegro mallocs, and say so - the user
     * tunes with --heap. Under a paging host (CWSDPMI) the reported block
     * includes virtual memory, so this rarely triggers there. */
    {
        __dpmi_free_mem_info fmi;
        const u32 reserve = 2048u * 1024u;
        if(__dpmi_get_free_memory_information(&fmi) == 0) {
            u32 avail = (u32)fmi.largest_available_free_block_in_bytes;
            avail = avail > reserve ? (avail - reserve) & ~4095u : 0;
            if(avail >= 256u * 1024u && mem_sz > avail) {
                fprintf(stderr, "warning: heap reduced from %luK to %luK "
                        "(free memory); use --heap to tune\n",
                        (unsigned long)(mem_sz >> 10),
                        (unsigned long)(avail >> 10));
                mem_sz = avail;
            }
        }
    }
    vm->mem = calloc(1, mem_sz);
    /* Under VCPI the free-memory answer can overstate (the server's pool
     * grows on demand, so the host can't know its real ceiling): step the
     * heap down rather than failing outright. */
    if(!vm->mem) {
        u32 want = mem_sz;
        while(!vm->mem && mem_sz > 1024u * 1024u) {
            mem_sz = (mem_sz - mem_sz / 4) & ~4095u;
            vm->mem = calloc(1, mem_sz);
        }
        if(vm->mem)
            fprintf(stderr, "warning: heap reduced from %luK to %luK "
                    "(free memory); use --heap to tune\n",
                    (unsigned long)(want >> 10), (unsigned long)(mem_sz >> 10));
    }
#elif defined(RELIC_RISCOS)
    /* Application space on RO3.x is hard-capped at 28MB-32K - a 24MB
     * heap alone nearly fills it, leaving SCL malloc too little room for
     * the backend's wr_* tables and the JIT dispatch table. Route the
     * heap through relic_far_alloc, which serves large requests from an
     * OS dynamic area outside application space. */
    vm->mem = relic_far_alloc(mem_sz);
    if(vm->mem) relic_far_set(vm->mem, 0, 0, mem_sz);
#else
    vm->mem = calloc(1, mem_sz);
#endif
    if(!vm->mem) return false;
    vm->mem_sz  = mem_sz;
    vm->ir      = ir;
    vm->backend = backend;
    vm->sp      = mem_sz;           /* stack grows down from top */
    vm->fp      = mem_sz;
    vm->pc      = 0;
    vm->running = true;
    vm->exit_code = 0;

    /* Copy the IR string table into the data segment of vm memory,
     * immediately after globals. String literals are emitted as
     * absolute addresses of the form (globals_size + str_off). */
    if(ir->strings_len > 0) {
        u32 base = (u32)ir->globals_size;
        u32 n    = (u32)ir->strings_len;
        if(base + n <= mem_sz) {
            relic_far_write(vm->mem, base, ir->strings, n);
        }
    }

    /* Heap starts after globals + strings + platform strings scratch,
     * 4-byte aligned. Grows up; the stack grows down from mem_sz.
     * @alloc/@free use this region. */
    {
        u32 data_end = (u32)ir->globals_size + (u32)ir->strings_len;
        data_end = (data_end + 3u) & ~3u;
        /* Reserve platform-strings scratch (1KB) for backend sys_platform. */
        vm->platform_strings_base = data_end;
        vm->platform_strings_size = 1024u;
        data_end += vm->platform_strings_size;
        if(data_end + RELIC_STACK_SIZE > mem_sz) {
            fprintf(stderr,
                "error: VM memory (%u bytes) is too small for this program; "
                "data segment needs %u bytes and stack reserves %u bytes\n",
                mem_sz, data_end, (u32)RELIC_STACK_SIZE);
            relic_far_free(vm->mem);
            vm->mem = NULL;
            return false;
        }
        vm->heap_base = data_end;
        vm->heap_top  = data_end;
    }

    srand((unsigned)time(NULL));

    if(backend && backend->init) backend->init(vm);
    return true;
}

void relic_vm_free(RelicVM *vm) {
    /* Drain any file handles still open in the per-VM table. */
    for(int i = 0; i < VM_MAX_FILES; i++) {
        if(vm->files[i].fp) {
            fclose(vm->files[i].fp);
            vm->files[i].fp = NULL;
        }
        vm->files[i].asset_data = NULL;
    }
    if(vm->backend && vm->backend->shutdown) vm->backend->shutdown(vm);
    relic_far_free(vm->mem);
    vm->mem = NULL;
}

/* -------------------------------------------------------------------------
 * Pre-production splash banner
 * -------------------------------------------------------------------------
 * Opens a temporary 640x480 window, draws the "Made with Tigryx" /
 * "Preproduction Alpha Code" text, waits for space, then resets the
 * backend so the program's own @window call works cleanly.
 * Skipped on Emscripten (web VM) and headless (null) backends.
 * ---------------------------------------------------------------------- */

#if defined(RELIC_PRODUCTION) && !defined(__EMSCRIPTEN__)
static u32 splash_write_str(RelicVM *vm, u32 *pp, const char *s) {
    u32 off = *pp;
    while(*s && *pp + 1 < vm->mem_sz)
        VMMEM8(vm, (*pp)++) = (u8)*s++;
    VMMEM8(vm, (*pp)++) = 0;
    return off;
}
#endif

void relic_splash_banner(RelicVM *vm) {
#if !defined(RELIC_PRODUCTION)
    /* Splash screen disabled for non-production builds. */
    (void)vm;
#elif defined(__EMSCRIPTEN__)
    (void)vm;
#else
    if(!vm || !vm->backend) return;
    RelicBackend *b = vm->backend;
    if(!b->gfx_window || !b->gfx_clear || !b->gfx_text || !b->gfx_flip || !b->inp_key)
        return;

    i32 saved_r[16];
    memcpy(saved_r, vm->r, sizeof(vm->r));
    u32 saved_heap_top = vm->heap_top;

    /* Scratch area on the heap for strings + colours. */
    u32 p = vm->heap_top;
    u32 white_off = p;
    VMMEM8(vm, p++) = 255; VMMEM8(vm, p++) = 255; VMMEM8(vm, p++) = 255; VMMEM8(vm, p++) = 255;
    u32 black_off = p;
    VMMEM8(vm, p++) = 0; VMMEM8(vm, p++) = 0; VMMEM8(vm, p++) = 0; VMMEM8(vm, p++) = 255;
    u32 font_off = p;
    VMMEM8(vm, p++) = 0;  /* empty font name = default face */

    u32 s_made  = splash_write_str(vm, &p, "Made with");
    u32 s_tigryx = splash_write_str(vm, &p, "T I G R Y X");
    u32 s_alpha = splash_write_str(vm, &p, "Preproduction Alpha Code. Not for distribution");
    u32 s_space = splash_write_str(vm, &p, "Press space to continue.");
    u32 s_copy  = splash_write_str(vm, &p, "Copyright (c) 2026 Scintillae Ltd.");
    vm->heap_top = p;

    /* Open a 640x480 window (x=0,y=0 = default position). */
    vm->r[1] = 0; vm->r[2] = 0; vm->r[3] = 640; vm->r[4] = 480; vm->r[5] = 0;
    b->gfx_window(vm);

    /* Clear to black. */
    vm->r[1] = (i32)black_off;
    b->gfx_clear(vm);

    /* Draw text lines (justify=1: centre-horizontal, top-vertical). */
    vm->r[6] = (i32)font_off; vm->r[7] = 1; vm->r[8] = (i32)white_off;

    vm->r[1] = (i32)s_made;  vm->r[2] = 320; vm->r[3] = 120; vm->r[4] = 18; vm->r[5] = 400;
    b->gfx_text(vm);

    vm->r[1] = (i32)s_tigryx; vm->r[2] = 320; vm->r[3] = 185; vm->r[4] = 48; vm->r[5] = 700;
    b->gfx_text(vm);

    vm->r[1] = (i32)s_alpha; vm->r[2] = 320; vm->r[3] = 255; vm->r[4] = 16; vm->r[5] = 400;
    b->gfx_text(vm);

    vm->r[1] = (i32)s_space; vm->r[2] = 320; vm->r[3] = 290; vm->r[4] = 16;
    b->gfx_text(vm);

    vm->r[1] = (i32)s_copy;  vm->r[2] = 320; vm->r[3] = 330; vm->r[4] = 12;
    b->gfx_text(vm);

    b->gfx_flip(vm);

    /* Wait for space (32) or enter (13). */
    for(;;) {
        b->inp_key(vm);
        if(vm->r[1] == 32 || vm->r[1] == 13) break;
        if(!vm->running) break;
        b->gfx_flip(vm);
    }

    /* Reset backend so the program's own @window works cleanly. */
    if(b->shutdown) b->shutdown(vm);
    if(b->init) b->init(vm);

    /* Restore VM state. */
    vm->heap_top = saved_heap_top;
    memcpy(vm->r, saved_r, sizeof(vm->r));
    vm->running = true;
#endif
}

/* -------------------------------------------------------------------------
 * In-VM heap allocator (used by @alloc / @free)
 *
 * Layout: a sequence of contiguous blocks starting at vm->heap_base, each
 * preceded by a 4-byte header containing the total block size (header +
 * payload, aligned to 4). The low bit of the size word is the FREE flag.
 * Returned pointers are vm->mem offsets to the payload, so they're directly
 * usable with OP_LOAD/OP_STORE, @print, fread, etc.
 * ---------------------------------------------------------------------- */
#define VM_HEAP_HDR    4u
#define VM_HEAP_FREE   1u

static u32 vm_heap_alloc(RelicVM *vm, u32 nbytes) {
    if(nbytes == 0) return 0;
    u32 need = ((nbytes + 3u) & ~3u) + VM_HEAP_HDR;

    /* First-fit over existing blocks */
    u32 cur = vm->heap_base;
    while(cur < vm->heap_top) {
        u32 hdr;
        relic_far_read(vm->mem, cur, &hdr, 4);
        u32 sz = hdr & ~VM_HEAP_FREE;
        if(sz < VM_HEAP_HDR + 4u || cur + sz > vm->heap_top) break; /* corrupt */
        if((hdr & VM_HEAP_FREE) && sz >= need) {
            /* Split if the leftover can hold another block */
            if(sz - need >= VM_HEAP_HDR + 4u) {
                u32 rest_hdr = (sz - need) | VM_HEAP_FREE;
                relic_far_write(vm->mem, cur + need, &rest_hdr, 4);
                u32 used_hdr = need;
                relic_far_write(vm->mem, cur, &used_hdr, 4);
            } else {
                u32 used_hdr = sz; /* keep size, clear FREE bit */
                relic_far_write(vm->mem, cur, &used_hdr, 4);
            }
            return cur + VM_HEAP_HDR;
        }
        cur += sz;
    }

    /* Extend heap upward, but stay clear of the stack region. */
    u32 stack_floor = (vm->mem_sz > RELIC_STACK_SIZE)
                      ? (vm->mem_sz - RELIC_STACK_SIZE) : vm->mem_sz;
    if(vm->heap_top + need > stack_floor) return 0;
    u32 blk = vm->heap_top;
    u32 hdr = need;
    relic_far_write(vm->mem, blk, &hdr, 4);
    vm->heap_top += need;
    return blk + VM_HEAP_HDR;
}

static void vm_heap_free(RelicVM *vm, u32 ptr) {
    if(ptr == 0) return;
    if(ptr < vm->heap_base + VM_HEAP_HDR || ptr >= vm->heap_top) return;
    u32 blk = ptr - VM_HEAP_HDR;
    u32 hdr;
    relic_far_read(vm->mem, blk, &hdr, 4);
    u32 sz = hdr & ~VM_HEAP_FREE;
    if(sz < VM_HEAP_HDR + 4u || blk + sz > vm->heap_top) return;
    if(hdr & VM_HEAP_FREE) return; /* double-free; ignore */

    /* Mark free, then coalesce forward with adjacent free blocks. */
    u32 next = blk + sz;
    while(next < vm->heap_top) {
        u32 nhdr;
        relic_far_read(vm->mem, next, &nhdr, 4);
        u32 nsz = nhdr & ~VM_HEAP_FREE;
        if(!(nhdr & VM_HEAP_FREE) || nsz < VM_HEAP_HDR + 4u) break;
        sz   += nsz;
        next += nsz;
    }
    if(blk + sz == vm->heap_top) {
        /* Top-most block: just shrink. */
        vm->heap_top = blk;
    } else {
        u32 fhdr = sz | VM_HEAP_FREE;
        relic_far_write(vm->mem, blk, &fhdr, 4);
    }
}

/* -------------------------------------------------------------------------
 * Host-builtin opcode bodies, factored out of step_body() so they can be
 * called directly from compiled code. The JS code generator under
 * src/jits/js/ uses these as host trampolines for OP_GFX_*, OP_SYS_*,
 * OP_INP_*, OP_SND_*. EMSCRIPTEN_KEEPALIVE keeps them visible to JS via
 * `Module._relic_op_*`. step_body() still dispatches through them, so
 * interpreter and generated-code semantics never drift.
 *
 * Calling convention matches the interpreter exactly: arguments arrive
 * in vm->r[1..], return values are written back into vm->r[1] (or vm->r[]
 * for multi-out builtins like @platform). Caller is responsible for
 * shuttling its working register copies in/out around the call.
 * ---------------------------------------------------------------------- */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define RELIC_OP_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define RELIC_OP_EXPORT
#endif

/* --- Graphics --- */
RELIC_OP_EXPORT void relic_op_gfx_clear(RelicVM *vm)    { if(vm->backend && vm->backend->gfx_clear)    vm->backend->gfx_clear(vm); }
RELIC_OP_EXPORT void relic_op_gfx_sprite(RelicVM *vm)   { if(vm->backend && vm->backend->gfx_sprite)   vm->backend->gfx_sprite(vm); }
RELIC_OP_EXPORT void relic_op_gfx_plot(RelicVM *vm)     { if(vm->backend && vm->backend->gfx_plot)     vm->backend->gfx_plot(vm); }
RELIC_OP_EXPORT void relic_op_gfx_flip(RelicVM *vm)     { if(vm->backend && vm->backend->gfx_flip)     vm->backend->gfx_flip(vm); }
RELIC_OP_EXPORT void relic_op_gfx_line(RelicVM *vm)     { if(vm->backend && vm->backend->gfx_line)     vm->backend->gfx_line(vm); }
RELIC_OP_EXPORT void relic_op_gfx_window(RelicVM *vm)   { if(vm->backend && vm->backend->gfx_window)   vm->backend->gfx_window(vm); }
RELIC_OP_EXPORT void relic_op_gfx_polygon(RelicVM *vm)  { if(vm->backend && vm->backend->gfx_polygon)  vm->backend->gfx_polygon(vm); }
RELIC_OP_EXPORT void relic_op_gfx_polyline(RelicVM *vm) { if(vm->backend && vm->backend->gfx_polyline) vm->backend->gfx_polyline(vm); }
RELIC_OP_EXPORT void relic_op_gfx_ellipse(RelicVM *vm)        { if(vm->backend && vm->backend->gfx_ellipse)        vm->backend->gfx_ellipse(vm); }
RELIC_OP_EXPORT void relic_op_gfx_ellipse_filled(RelicVM *vm) { if(vm->backend && vm->backend->gfx_ellipse_filled) vm->backend->gfx_ellipse_filled(vm); }
RELIC_OP_EXPORT void relic_op_gfx_rounded_rect(RelicVM *vm)        { if(vm->backend && vm->backend->gfx_rounded_rect)        vm->backend->gfx_rounded_rect(vm); }
RELIC_OP_EXPORT void relic_op_gfx_rounded_rect_filled(RelicVM *vm) { if(vm->backend && vm->backend->gfx_rounded_rect_filled) vm->backend->gfx_rounded_rect_filled(vm); }
RELIC_OP_EXPORT void relic_op_gfx_camera(RelicVM *vm)   { if(vm->backend && vm->backend->gfx_camera)   vm->backend->gfx_camera(vm); }
RELIC_OP_EXPORT void relic_op_gfx_texture(RelicVM *vm) {
    if(vm->backend && vm->backend->gfx_texture) vm->backend->gfx_texture(vm);
    else vm->r[1] = 0;
}
RELIC_OP_EXPORT void relic_op_gfx_light(RelicVM *vm)    { if(vm->backend && vm->backend->gfx_light)    vm->backend->gfx_light(vm); }
RELIC_OP_EXPORT void relic_op_gfx_mesh(RelicVM *vm)     { if(vm->backend && vm->backend->gfx_mesh)     vm->backend->gfx_mesh(vm); }
RELIC_OP_EXPORT void relic_op_gfx_fog(RelicVM *vm)      { if(vm->backend && vm->backend->gfx_fog)      vm->backend->gfx_fog(vm); }
RELIC_OP_EXPORT void relic_op_gfx_text(RelicVM *vm)     { if(vm->backend && vm->backend->gfx_text)     vm->backend->gfx_text(vm); }
RELIC_OP_EXPORT void relic_op_gfx_viewport(RelicVM *vm) { if(vm->backend && vm->backend->gfx_viewport) vm->backend->gfx_viewport(vm); }
RELIC_OP_EXPORT void relic_op_gfx_text_measure(RelicVM *vm) { if(vm->backend && vm->backend->gfx_text_measure) vm->backend->gfx_text_measure(vm); }
RELIC_OP_EXPORT void relic_op_gfx_clip(RelicVM *vm) { if(vm->backend && vm->backend->gfx_clip) vm->backend->gfx_clip(vm); }
RELIC_OP_EXPORT void relic_op_gfx_view(RelicVM *vm) { if(vm->backend && vm->backend->gfx_view) vm->backend->gfx_view(vm); }
RELIC_OP_EXPORT void relic_op_gfx_view_capture(RelicVM *vm) { if(vm->backend && vm->backend->gfx_view_capture) vm->backend->gfx_view_capture(vm); }
RELIC_OP_EXPORT void relic_op_gfx_view_present(RelicVM *vm) { if(vm->backend && vm->backend->gfx_view_present) vm->backend->gfx_view_present(vm); }
RELIC_OP_EXPORT void relic_op_gfx_palette_upload(RelicVM *vm) { if(vm->backend && vm->backend->gfx_palette_upload) vm->backend->gfx_palette_upload(vm); }
RELIC_OP_EXPORT void relic_op_gfx_draw_list(RelicVM *vm) { if(vm->backend && vm->backend->gfx_draw_list) vm->backend->gfx_draw_list(vm); }
/* Reports support via r[1] so @draw_mode(m) can tell the caller whether
 * XOR self-erasing draws are real on this backend or it must fall back
 * to clear+redraw. */
RELIC_OP_EXPORT void relic_op_gfx_draw_mode(RelicVM *vm) {
    if(vm->backend && vm->backend->gfx_draw_mode) {
        vm->backend->gfx_draw_mode(vm);
        vm->r[1] = 1;
    } else {
        vm->r[1] = 0;
    }
}
/* Backend-native batch wireframe projection (see OP_GFX_LINES3D). r[1]
 * reports -1 when the backend has no implementation so the caller uses
 * its portable bytecode loop instead. */
RELIC_OP_EXPORT void relic_op_gfx_lines3d(RelicVM *vm) {
    if(vm->backend && vm->backend->gfx_lines3d) {
        vm->backend->gfx_lines3d(vm);
    } else {
        vm->r[1] = -1;
    }
}

/* Backend-native grid raycaster (see OP_GFX_GRID3D). Same -1 contract
 * as @lines3d: backends without it return -1 and the caller falls
 * back to computing columns in bytecode. */
RELIC_OP_EXPORT void relic_op_gfx_grid3d(RelicVM *vm) {
    if(vm->backend && vm->backend->gfx_grid3d) {
        vm->backend->gfx_grid3d(vm);
    } else {
        vm->r[1] = -1;
    }
}

/* --- Sector-based world renderer (see OP_WR_* in tigryx.h). Implemented by
 * the allegro-fixed-dos, allegro-dos, allegro-tinygl-dos, opengl-win32,
 * opengl-linux, opengl-macos, opengl-emscripten and relic3d-win32 backends
 * via the shared wr_render.h engine. No portable fallback is meaningful
 * here, so these just no-op on any backend that hasn't implemented them. --- */
RELIC_OP_EXPORT void relic_op_wr_palette(RelicVM *vm) { if(vm->backend && vm->backend->wr_palette) vm->backend->wr_palette(vm); }
RELIC_OP_EXPORT void relic_op_wr_texture(RelicVM *vm) { if(vm->backend && vm->backend->wr_texture) vm->backend->wr_texture(vm); }
RELIC_OP_EXPORT void relic_op_wr_flat(RelicVM *vm)    { if(vm->backend && vm->backend->wr_flat)    vm->backend->wr_flat(vm); }
RELIC_OP_EXPORT void relic_op_wr_test(RelicVM *vm)    { if(vm->backend && vm->backend->wr_test)    vm->backend->wr_test(vm); }
RELIC_OP_EXPORT void relic_op_wr_map(RelicVM *vm)     { if(vm->backend && vm->backend->wr_map)     vm->backend->wr_map(vm); }
RELIC_OP_EXPORT void relic_op_wr_render(RelicVM *vm)  { if(vm->backend && vm->backend->wr_render)  vm->backend->wr_render(vm); }
RELIC_OP_EXPORT void relic_op_wr_texmap(RelicVM *vm)  { if(vm->backend && vm->backend->wr_texmap)  vm->backend->wr_texmap(vm); }
RELIC_OP_EXPORT void relic_op_wr_sky(RelicVM *vm)     { if(vm->backend && vm->backend->wr_sky)     vm->backend->wr_sky(vm); }
RELIC_OP_EXPORT void relic_op_wr_flatshade(RelicVM *vm) { if(vm->backend && vm->backend->wr_flatshade) vm->backend->wr_flatshade(vm); }
RELIC_OP_EXPORT void relic_op_wr_patch(RelicVM *vm)     { if(vm->backend && vm->backend->wr_patch)     vm->backend->wr_patch(vm); }
RELIC_OP_EXPORT void relic_op_wr_sprite(RelicVM *vm)    { if(vm->backend && vm->backend->wr_sprite)    vm->backend->wr_sprite(vm); }
RELIC_OP_EXPORT void relic_op_wr_blockmap(RelicVM *vm)  { if(vm->backend && vm->backend->wr_blockmap)  vm->backend->wr_blockmap(vm); }
RELIC_OP_EXPORT void relic_op_wr_blocked(RelicVM *vm)   { vm->r[1] = -1; if(vm->backend && vm->backend->wr_blocked)   vm->backend->wr_blocked(vm); }
RELIC_OP_EXPORT void relic_op_wr_blocked_line(RelicVM *vm) { vm->r[1] = -1; if(vm->backend && vm->backend->wr_blocked_line) vm->backend->wr_blocked_line(vm); }

/* --- Input --- */
RELIC_OP_EXPORT void relic_op_inp_key(RelicVM *vm) {
    if(vm->backend && vm->backend->inp_key) vm->backend->inp_key(vm);
    else vm->r[1] = 0;
}
RELIC_OP_EXPORT void relic_op_inp_keyheld(RelicVM *vm) {
    if(vm->backend && vm->backend->inp_keyheld) vm->backend->inp_keyheld(vm);
    else vm->r[1] = 0;
}
RELIC_OP_EXPORT void relic_op_inp_mouse(RelicVM *vm) {
    /* @mouse(out: ref Mouse)
     * r1 = byte offset into vm->mem of a 16-byte Mouse struct:
     *   i32 x, i32 y, i32 buttons (bit0=L,1=R,2=M), i32 wheel.
     * Backends fill the struct; the VM zeroes it as a portable fallback
     * (and as a safety net if a backend forgets to implement). */
    u32 ptr = (u32)vm->r[1];
    if(vm->backend && vm->backend->inp_mouse) {
        vm->backend->inp_mouse(vm);
        return;
    }
    if(ptr + 16u <= vm->mem_sz) {
        relic_far_set(vm->mem, ptr, 0, 16);
    }
}

RELIC_OP_EXPORT void relic_op_inp_gamepad_count(RelicVM *vm) {
    /* @gamepad_count() -> i32
     * Returns number of connected controllers in r1. */
    if(vm->backend && vm->backend->inp_gamepad_count) {
        vm->backend->inp_gamepad_count(vm);
        return;
    }
    vm->r[1] = 0;
}

RELIC_OP_EXPORT void relic_op_inp_gamepad(RelicVM *vm) {
    /* @gamepad(idx: i32, out: ref Gamepad) -> i32
     * r1 = controller index (0-based)
     * r2 = byte offset into vm->mem of a 28-byte Gamepad struct:
     *   i32 buttons (bitmask), i32 left_x, i32 left_y, i32 right_x,
     *   i32 right_y, i32 left_trigger, i32 right_trigger.
     * Returns 1 in r1 if controller is connected, 0 otherwise.
     * Backends fill the struct; the VM zeroes it as a portable fallback. */
    i32 idx = vm->r[1];
    u32 ptr = (u32)vm->r[2];
    if(vm->backend && vm->backend->inp_gamepad) {
        vm->backend->inp_gamepad(vm);
        return;
    }
    vm->r[1] = 0;
    if(ptr + 28u <= vm->mem_sz) {
        relic_far_set(vm->mem, ptr, 0, 28);
    }
    (void)idx;
}

/* --- System --- */
RELIC_OP_EXPORT void relic_op_sys_rand(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_rand) vm->backend->sys_rand(vm);
    else vm->r[1] = (i32)(rand() & 0xFFFF);
}
RELIC_OP_EXPORT void relic_op_sys_millis(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_millis) vm->backend->sys_millis(vm);
    else {
        clock_t t = clock();
        vm->r[1] = (i32)((t * 1000) / CLOCKS_PER_SEC);
    }
}
RELIC_OP_EXPORT void relic_op_sys_console(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_console) vm->backend->sys_console(vm);
    else {
        /* Un-hide the console if it was hidden as an Explorer launch (see
         * tigryx.h) - a script actually printing is exactly the signal that
         * should make it visible. */
        relic_console_ensure();
        u32 ptr = (u32)vm->r[1];
        while(ptr < vm->mem_sz) {
            u8 c = VMMEM8(vm, ptr++);
            if(c == 0) break;
            putchar(c);
        }
        fflush(stdout);
    }
}

#ifdef RELIC_IA16
/* gcc-ia16's snprintf %f is unusable: it emits nothing on its own and
 * hangs outright once any earlier conversion has run. Format floats
 * ourselves - float ops only (no double), no libc %f. Handles the
 * [%][flags][width][.prec]f subset we actually emit; unknown flags are
 * parsed and ignored rather than breaking the spec. */
static int ia16_fmt_f(char *dst, int cap, const char *spec, float f) {
    int prec = 6, width = 0, left = 0, nl = 0, pad, out = 0, i;
    const char *p = spec + 1;
    char num[48];
    while(*p == '-' || *p == '+' || *p == ' ' || *p == '0' || *p == '#') {
        if(*p == '-') left = 1;
        p++;
    }
    while(*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
    if(*p == '.') {
        p++; prec = 0;
        while(*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
    }
    if(f < 0.0f) { num[nl++] = '-'; f = -f; }
    {
        long ip = (long)f;
        float frac = f - (float)ip;
        long scale = 1;
        long fdig;
        char ib[16], fbb[16];
        int il = 0, fl = 0;
        for(i = 0; i < prec; i++) scale *= 10;
        fdig = (long)(frac * (float)scale + 0.5f);
        if(fdig >= scale) { ip++; fdig -= scale; }
        if(ip == 0) ib[il++] = '0';
        while(ip > 0 && il < 15) { ib[il++] = (char)('0' + (ip % 10)); ip /= 10; }
        while(il > 0) num[nl++] = ib[--il];
        if(prec > 0) {
            num[nl++] = '.';
            while(fdig > 0 && fl < 15) { fbb[fl++] = (char)('0' + (fdig % 10)); fdig /= 10; }
            while(fl < prec) fbb[fl++] = '0';
            while(fl > 0) num[nl++] = fbb[--fl];
        }
    }
    pad = width - nl;
    if(!left) while(pad-- > 0 && out < cap - 1) dst[out++] = ' ';
    for(i = 0; i < nl && out < cap - 1; i++) dst[out++] = num[i];
    if(left) while(pad-- > 0 && out < cap - 1) dst[out++] = ' ';
    return out;
}
/* Integer conversions by hand too: ia16 snprintf doesn't implement 'l'
 * length modifiers, so %ld can't carry our i32 guest values. Supports
 * flags -, +, space, 0, #, width, and .prec for d/x/X/o/u. */
static int ia16_fmt_i(char *dst, int cap, const char *spec, i32 v, char conv) {
    int width = 0, prec = -1, left = 0, zero = 0, sign = 0, upper = 0;
    int alt = 0, nl = 0, pad, out = 0, i, il = 0;
    unsigned long uv;
    unsigned base = 10;
    const char *p = spec + 1;
    char num[24], ib[16];
    for(; ; p++) {
        if(*p == '-') left = 1;
        else if(*p == '+') sign = 1;
        else if(*p == ' ') { if(!sign) sign = 2; }
        else if(*p == '0') zero = 1;
        else if(*p == '#') alt = 1;
        else break;
    }
    while(*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
    if(*p == '.') {
        p++; prec = 0;
        while(*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        zero = 0;
    }
    uv = (unsigned long)(u32)v;
    if(conv == 'd') {
        if(v < 0) { num[nl++] = '-'; uv = (unsigned long)(-(long)v); }
        else if(sign == 1) num[nl++] = '+';
        else if(sign == 2) num[nl++] = ' ';
    } else if(conv == 'x' || conv == 'X') {
        base = 16; upper = (conv == 'X');
        if(alt && uv) { num[nl++] = '0'; num[nl++] = conv; }
    } else if(conv == 'o') {
        base = 8;
        if(alt) num[nl++] = '0';
    }
    if(uv == 0) ib[il++] = '0';
    while(uv && il < 15) {
        unsigned dgt = (unsigned)(uv % base);
        ib[il++] = (char)(dgt < 10 ? '0' + dgt
                      : (upper ? 'A' : 'a') + dgt - 10);
        uv /= base;
    }
    while(il < prec) ib[il++] = '0';
    while(il > 0) num[nl++] = ib[--il];
    pad = width - nl;
    if(zero && !left) while(pad-- > 0 && out < cap - 1) dst[out++] = '0';
    if(!left && !zero) while(pad-- > 0 && out < cap - 1) dst[out++] = ' ';
    for(i = 0; i < nl && out < cap - 1; i++) dst[out++] = num[i];
    if(left) while(pad-- > 0 && out < cap - 1) dst[out++] = ' ';
    return out;
}
#endif

RELIC_OP_EXPORT void relic_op_sys_snprint(RelicVM *vm) {
    /* @snprint(n: u32, s: *u8, fmt: *u8, args...) -> *u8
     * r1 = n      (capacity of destination buffer, including trailing NUL)
     * r2 = s      (vm->mem offset of destination buffer)
     * r3 = fmt    (vm->mem offset of format string)
     * r4..r8 = up to 5 args (i32 or f32 bits)
     * Writes at most n-1 bytes to s and NUL-terminates when n > 0.
     * Returns r1 = s (the buffer pointer, for convenience).
     * Supports %d, %x, %X, %o, %u, %c, %f, %e, %g, %s, %% and
     * width/precision modifiers. An 'l' length modifier on a float
     * conversion (%lf/%le/%lg) takes an f64 argument from the d bank
     * (arg index mirrors the CALL convention); without it the arg is
     * f32 bits in the r bank. */
    u32 n_cap   = (u32)vm->r[1];
    u32 buf_ptr = (u32)vm->r[2];
    u32 fmt_ptr = (u32)vm->r[3];

    /* Bounds check: refuse to touch memory outside vm->mem. */
    if(buf_ptr >= vm->mem_sz || n_cap == 0) { vm->r[1] = (i32)buf_ptr; return; }
    u32 max_avail = vm->mem_sz - buf_ptr;
    if(n_cap > max_avail) n_cap = max_avail;

#ifdef RELIC_IA16
    /* libc snprintf writes to near pointers only - format into a near
     * bounce buffer, then copy out to guest memory.  Output beyond the
     * bounce capacity is truncated. */
    char bounce[4096];
    if(n_cap > sizeof(bounce)) n_cap = sizeof(bounce);
    char *out = bounce;
#else
    char *out     = (char*)(vm->mem + buf_ptr);
#endif
    int   out_cap = (int)n_cap - 1;     /* reserve room for the NUL */
    if(out_cap < 0) out_cap = 0;
    int   out_len = 0;
    int   argi    = 4;
    u32   fp      = fmt_ptr;
    while(fp < vm->mem_sz && out_len < out_cap) {
        u8 c = VMMEM8(vm, fp++);
        if(c == 0) break;
        if(c != '%') { out[out_len++] = (char)c; continue; }
        char spec[32];
        int  sl = 0;
        spec[sl++] = '%';
        while(fp < vm->mem_sz && sl < (int)sizeof(spec)-2) {
            u8 sc = VMMEM8(vm, fp);
            if(sc == 'd' || sc == 'f' || sc == 'F' || sc == '%' ||
               sc == 'e' || sc == 'E' || sc == 'g' || sc == 'G' ||
               sc == 's' || sc == 'c' || sc == 'x' ||
               sc == 'X' || sc == 'o' || sc == 'u' || sc == 0) break;
            spec[sl++] = (char)sc;
            fp++;
        }
        u8 conv = (fp < vm->mem_sz) ? VMMEM8(vm, fp) : 0;
        if(conv) fp++;
        spec[sl++] = (char)conv;
        spec[sl]   = '\0';
        int rem = out_cap - out_len;
        int n = 0;
        if(conv == '%') {
            if(rem > 0) { out[out_len++] = '%'; }
            continue;
        } else if(conv == 'd' || conv == 'x' || conv == 'X' ||
                  conv == 'o' || conv == 'u' || conv == 'c') {
            i32 v = (argi < 16) ? vm->r[argi++] : 0;
#ifdef RELIC_IA16
            if(conv == 'c') {
                if(rem > 0) { out[out_len++] = (char)v; n = 1; }
            } else {
                n = ia16_fmt_i(out + out_len, rem + 1, spec, v, (char)conv);
            }
#else
            n = snprintf(out + out_len, (size_t)rem + 1, spec, (int)v);
#endif
        } else if(conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
                  conv == 'g' || conv == 'G') {
            /* The 'l' length modifier selects an f64 argument from the
             * d bank (%lf/%le/%lg) - the same arg-index convention CALL
             * uses for f64 parameters. Bare %f etc. read f32 bits from r.
             * Either way the arg index advances by one slot. */
            if(sl >= 2 && spec[sl-2] == 'l') {
                double dv = (argi < 16) ? vm->d[argi++] : 0.0;
#ifdef RELIC_IA16
                /* ia16 has no f64 formatter (and its %e/%g were never
                 * supported): narrow to the float path. */
                n = ia16_fmt_f(out + out_len, rem + 1, spec, (float)dv);
#else
                n = snprintf(out + out_len, (size_t)rem + 1, spec, dv);
#endif
            } else {
                RelicFBits fb;
                fb.i = (argi < 16) ? vm->r[argi++] : 0;
#ifdef RELIC_IA16
                n = ia16_fmt_f(out + out_len, rem + 1, spec, fb.f);
#else
                n = snprintf(out + out_len, (size_t)rem + 1, spec, (double)fb.f);
#endif
            }
        } else if(conv == 's') {
            u32 sp = (argi < 16) ? (u32)vm->r[argi++] : 0;
            while(sp < vm->mem_sz && out_len < out_cap) {
                u8 sc = VMMEM8(vm, sp++);
                if(sc == 0) break;
                out[out_len++] = (char)sc;
            }
            continue;
        } else {
            for(int i=0;i<sl && out_len<out_cap;i++) out[out_len++] = spec[i];
            continue;
        }
        if(n < 0) n = 0;
        if(n > rem) n = rem;
        out_len += n;
    }
    if(out_len > out_cap) out_len = out_cap;
    out[out_len] = '\0';
#ifdef RELIC_IA16
    relic_far_write(vm->mem, buf_ptr, out, (u32)out_len + 1);
#endif
    vm->r[1] = (i32)buf_ptr;
}
/* Stack-overflow report shared by the interpreter's OP_ENTER guard and
 * the AOT compilers' compiled ENTER trap. Compiled code cannot re-run
 * the ENTER via relic_vm_step1 (a slim embedded blob may omit the
 * instruction stream entirely), so it sets vm->pc to one past the ENTER
 * instruction and calls the exported wrapper below; `at` therefore
 * resolves to the ENTER's own pc, matching the interpreter convention. */
static void vm_stack_overflow_at(RelicVM *vm, u32 at) {
    int line = (vm->ir && at < (u32)vm->ir->len) ? (int)IRLINE_RD(vm->ir, at) : 0;
    int file_id = (vm->ir && at < (u32)vm->ir->len) ? (int)IRFILEID_RD(vm->ir, at) : -1;
    if(file_id >= 0 && vm->ir && file_id < vm->ir->nsource_files) {
        char sfile[RELIC_MAX_SOURCE_PATH];
        relic_far_str(sfile, IRSRCFILE(vm->ir, file_id), sizeof(sfile));
        fprintf(stderr, "Stack overflow at %s:%d "
            "(function locals exceed the %u KB VM stack)\n",
            sfile, line,
            (unsigned)(RELIC_STACK_SIZE / 1024u));
    }
    else
        fprintf(stderr, "Stack overflow "
            "(function locals exceed the %u KB VM stack)\n",
            (unsigned)(RELIC_STACK_SIZE / 1024u));
    fflush(stderr);
    vm->trap.kind    = RELIC_TRAP_STACK_OVERFLOW;
    vm->trap.pc      = at;
    vm->trap.line    = line;
    vm->trap.file_id = file_id;
    snprintf(vm->trap.message, sizeof(vm->trap.message),
             "Stack overflow");
    vm->running   = false;
    vm->exit_code = 1;
}
RELIC_OP_EXPORT void relic_vm_stack_overflow(RelicVM *vm) {
    u32 at = (vm->pc > 0) ? vm->pc - 1 : vm->pc;
    vm_stack_overflow_at(vm, at);
}
RELIC_OP_EXPORT void relic_op_sys_assert(RelicVM *vm) {
    if(vm->r[1] == 0) {
        relic_console_ensure();

        /* vm->pc has already been advanced past this instruction by the
         * dispatch loop, so the failing @assert is at pc-1. line_for_pc /
         * file_for_pc are only meaningful (non-zero / non-negative) when
         * the program was compiled from source with a debug section, or
         * run directly from source without a save/reload through a
         * debug-info-free .tlx - see relic_compile_source() and the
         * --debug-info flag. */
        u32 at = (vm->pc > 0) ? vm->pc - 1 : vm->pc;
        int line = (vm->ir && at < (u32)vm->ir->len) ? IRLINE_RD(vm->ir, at) : 0;
        int file_id = (vm->ir && at < (u32)vm->ir->len) ? IRFILEID_RD(vm->ir, at) : -1;

        if(file_id >= 0 && vm->ir && file_id < vm->ir->nsource_files)
            {
                char fbuf[RELIC_MAX_SOURCE_PATH];
                relic_far_str(fbuf, IRSRCFILE(vm->ir, file_id), sizeof fbuf);
                fprintf(stderr, "Assertion failed at %s:%d\n", fbuf, line);
            }
        else if(line > 0)
            fprintf(stderr, "Assertion failed at line %d\n", line);
        else
            fprintf(stderr, "Assertion failed\n");

        vm->trap.kind    = RELIC_TRAP_ASSERT;
        vm->trap.pc      = at;
        vm->trap.line    = line;
        vm->trap.file_id = file_id;
        snprintf(vm->trap.message, sizeof(vm->trap.message), "Assertion failed");

        vm->running = false;
        vm->exit_code = 1;
    }
}
RELIC_OP_EXPORT void relic_op_sys_alloc(RelicVM *vm) {
    /* @alloc(n) -> r1 = vm->mem offset (0 == out of memory).
     * Backend overrides intentionally ignored: pointers in this
     * VM are 32-bit offsets into vm->mem, not host addresses. */
    vm->r[1] = (i32)vm_heap_alloc(vm, (u32)vm->r[1]);
}
RELIC_OP_EXPORT void relic_op_sys_free(RelicVM *vm) {
    vm_heap_free(vm, (u32)vm->r[1]);
}
/* Clamp a [ptr, ptr+n) VM-memory range to the allocated memory, returning
 * the number of bytes that are actually addressable. A pointer past the end
 * of memory, or a negative count, yields 0. */
static u32 vm_clamp_range(RelicVM *vm, u32 ptr, i32 n) {
    if(n <= 0 || ptr >= vm->mem_sz) return 0;
    u32 avail = vm->mem_sz - ptr;
    return ((u32)n < avail) ? (u32)n : avail;
}
RELIC_OP_EXPORT void relic_op_sys_memcpy(RelicVM *vm) {
    /* @memcpy(dst, src, n) -> r1 = bytes copied.
     * Uses memmove so overlapping ranges behave predictably. */
    u32 dst = (u32)vm->r[1];
    u32 src = (u32)vm->r[2];
    u32 n   = vm_clamp_range(vm, dst, vm->r[3]);
    u32 m   = vm_clamp_range(vm, src, vm->r[3]);
    if(m < n) n = m;
    if(n) relic_far_move(vm->mem, dst, src, n);
    vm->r[1] = (i32)n;
}
RELIC_OP_EXPORT void relic_op_sys_memset(RelicVM *vm) {
    /* @memset(dst, val, n) -> r1 = bytes set. Only the low byte of val
     * is written, matching C's memset. */
    u32 dst = (u32)vm->r[1];
    u32 n   = vm_clamp_range(vm, dst, vm->r[3]);
    if(n) relic_far_set(vm->mem, dst, vm->r[2] & 0xFF, n);
    vm->r[1] = (i32)n;
}
/* Guest code writes portable '/' paths. On the RISC OS SCL build, SCL
 * filename translation treats '/' as the leaf "extension" delimiter rather
 * than a directory separator, so "fonts/T83B.ZIP" never resolves - the
 * RISC OS '.' directory separator is what reaches the filesystem. Convert
 * '/' to '.' for paths handed to host FS calls; paths used for packed-
 * asset lookup keep '/' (trailer keys are '/'-formed). UnixLib builds
 * handle '/' natively and need no conversion. Safe to call in-place. */
static void vm_fs_path(char *dst, int cap, const char *src) {
    int i = 0;
#ifdef RELIC_RISCOS_SCL
    for(; i < cap - 1 && src[i]; i++) dst[i] = (src[i] == '/') ? '.' : src[i];
#else
    for(; i < cap - 1 && src[i]; i++) dst[i] = src[i];
#endif
    dst[i] = 0;
}
#ifdef RELIC_RISCOS_SCL
#define VM_PATHSEP(c) ((c) == '.' || (c) == '/' || (c) == '\\')
#else
#define VM_PATHSEP(c) ((c) == '/' || (c) == '\\')
#endif
RELIC_OP_EXPORT void relic_op_sys_fopen(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_fopen) vm->backend->sys_fopen(vm);
    else {
        u32 ptr = (u32)vm->r[1];
        i32 len = vm->r[2];
        i32 mode = vm->r[3];     /* 0 = read, 1 = write */
        char path[512] = {0};
        /* The compiler only knows the path length for string literals; for
         * dynamic paths it passes 0. In that case derive the length from the
         * NUL terminator so runtime-built filenames (e.g. from @args) work. */
        if(len <= 0) {
            len = 0;
            while(len < 511 && ptr + (u32)len < vm->mem_sz && VMMEM8(vm, ptr + len) != 0) len++;
        }
        int n = len < 511 ? len : 511;
        char fsp[512];
        relic_far_read(vm->mem, ptr, path, n);
        vm_fs_path(fsp, (int)sizeof fsp, path);
        if(mode == 1) {
            /* Write mode: bypass asset lookup; assets are read-only blobs. */
            FILE *f = fopen(fsp, "wb");
            vm->r[1] = f ? vm_handle_alloc_file(vm, f) : -1;
            if(f && vm->r[1]==-1) fclose(f);
        } else if(mode == 2) {
            /* FILE_MODE_READ_DISK: real filesystem only, never assets - for
             * state files the program itself writes (a bundled snapshot must
             * not shadow the on-disk copy). */
            FILE *f = fopen(fsp, "rb");
            vm->r[1] = f ? vm_handle_alloc_file(vm, f) : -1;
            if(f && vm->r[1]==-1) fclose(f);
        } else {
            /* Asset-first lookup for read mode. */
            u32 asset_sz = 0;
            const u8 RELIC_FAR *asset = relic_assets_find(vm->assets, path, &asset_sz);
            if(asset) {
                vm->r[1] = vm_handle_alloc_asset(vm, asset, asset_sz);
            } else {
                FILE *f = fopen(fsp, "rb");
                vm->r[1] = f ? vm_handle_alloc_file(vm, f) : -1;
                if(f && vm->r[1]==-1) fclose(f);
            }
        }
    }
}
RELIC_OP_EXPORT void relic_op_sys_fread(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_fread) vm->backend->sys_fread(vm);
    else {
        VMFileHandle *h = vm_handle_get(vm, vm->r[1]);
        u32 buf = (u32)vm->r[2];
        u32 n   = (u32)vm->r[3];
        if(!h || buf + n > vm->mem_sz) { vm->r[1] = 0; return; }
        if(h->is_asset) {
            u32 avail = h->asset_size - h->asset_pos;
            u32 rd = n < avail ? n : avail;
            if(rd) relic_far_write(vm->mem, buf,
                      relic_cfp_add(h->asset_data, h->asset_pos), rd);
            h->asset_pos += rd;
            vm->r[1] = (i32)rd;
        } else {
#ifdef RELIC_IA16
            vm->r[1] = (i32)vm_fread_mem(vm, buf, n, h->fp);
#else
            vm->r[1] = (i32)fread(vm->mem + buf, 1, n, h->fp);
#endif
        }
    }
}
RELIC_OP_EXPORT void relic_op_sys_fclose(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_fclose) vm->backend->sys_fclose(vm);
    else { vm_handle_free(vm, vm->r[1]); }
}
RELIC_OP_EXPORT void relic_op_sys_fwrite(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_fwrite) vm->backend->sys_fwrite(vm);
    else {
        VMFileHandle *h = vm_handle_get(vm, vm->r[1]);
        u32 buf = (u32)vm->r[2];
        u32 n   = (u32)vm->r[3];
        if(!h || buf + n > vm->mem_sz) { vm->r[1] = 0; return; }
        if(h->is_asset) {
            /* Assets are read-only. */
            vm->r[1] = 0;
        } else {
#ifdef RELIC_IA16
            vm->r[1] = (i32)vm_fwrite_mem(vm, buf, n, h->fp);
#else
            vm->r[1] = (i32)fwrite(vm->mem + buf, 1, n, h->fp);
#endif
        }
    }
}
RELIC_OP_EXPORT void relic_op_sys_fseek(RelicVM *vm) {
    /* @file_seek(fd: i32, offset: i32, whence: i32) -> i32
     *   r1 = fd, r2 = offset, r3 = whence (0=SET, 1=CUR, 2=END)
     *   Returns new position in r1, or -1 on error. */
    if(vm->backend && vm->backend->sys_fseek) vm->backend->sys_fseek(vm);
    else {
        VMFileHandle *h = vm_handle_get(vm, vm->r[1]);
        i32 offset = vm->r[2];
        i32 whence = vm->r[3];
        if(!h) { vm->r[1] = -1; return; }
        if(h->is_asset) {
            u32 new_pos;
            switch(whence) {
                case 0: new_pos = (u32)offset; break;
                case 1: new_pos = h->asset_pos + (u32)offset; break;
                case 2: new_pos = h->asset_size + (u32)offset; break;
                default: vm->r[1] = -1; return;
            }
            if(new_pos > h->asset_size) new_pos = h->asset_size;
            h->asset_pos = new_pos;
            vm->r[1] = (i32)new_pos;
        } else {
            int w = (whence == 0) ? SEEK_SET :
                    (whence == 1) ? SEEK_CUR :
                    (whence == 2) ? SEEK_END : SEEK_SET;
            if(fseek(h->fp, (long)offset, w) != 0) { vm->r[1] = -1; return; }
            vm->r[1] = (i32)ftell(h->fp);
        }
    }
}
RELIC_OP_EXPORT void relic_op_sys_flen(RelicVM *vm) {
    /* @file_length(fd: i32) -> i32
     *   Returns total file size in r1, or -1 on error. */
    if(vm->backend && vm->backend->sys_flen) vm->backend->sys_flen(vm);
    else {
        VMFileHandle *h = vm_handle_get(vm, vm->r[1]);
        if(!h) { vm->r[1] = -1; return; }
        if(h->is_asset) {
            vm->r[1] = (i32)h->asset_size;
        } else {
            long cur = ftell(h->fp);
            if(cur < 0 || fseek(h->fp, 0, SEEK_END) != 0) { vm->r[1] = -1; return; }
            long end = ftell(h->fp);
            fseek(h->fp, cur, SEEK_SET);
            vm->r[1] = (i32)end;
        }
    }
}

/* DirEntry layout in VM memory (276 bytes per entry):
 *   offset   0: name[256]  (NUL-terminated filename)
 *   offset 256: size       (i32)
 *   offset 260: is_dir     (i32, 0 or 1)
 *   offset 264: readable   (i32, 0 or 1)
 *   offset 268: writable   (i32, 0 or 1)
 *   offset 272: mtime      (i32, Unix epoch seconds, truncated to 32 bits)
 */
#define DIR_ENTRY_SIZE 276
#define DIR_NAME_SIZE  256

RELIC_OP_EXPORT void relic_op_sys_fdir(RelicVM *vm) {
    /* @file_directory(path: *u8, buf: *u8, max_entries: i32) -> i32
     *   r1 = path (vm->mem offset, NUL-terminated)
     *   r2 = buf  (vm->mem offset for output DirEntry array)
     *   r3 = max_entries
     *   Returns number of entries written in r1, or -1 on error. */
    if(vm->backend && vm->backend->sys_fdir) vm->backend->sys_fdir(vm);
    else {
        u32 path_ptr = (u32)vm->r[1];
        u32 buf_ptr  = (u32)vm->r[2];
        i32 max_entries = vm->r[3];
        if(max_entries <= 0 || buf_ptr + (u32)(max_entries * DIR_ENTRY_SIZE) > vm->mem_sz) {
            vm->r[1] = -1; return;
        }
        char path[512] = {0};
        int plen = 0;
        while(plen < 511 && path_ptr + (u32)plen < vm->mem_sz && VMMEM8(vm, path_ptr + plen) != 0) plen++;
        relic_far_read(vm->mem, path_ptr, path, plen);
        path[plen] = 0;
        char fsp[512];
        vm_fs_path(fsp, (int)sizeof fsp, path);

        int count = 0;
#ifdef _WIN32
        char search[600];
        snprintf(search, sizeof(search), "%s\\*", path);
        struct _finddata_t fd;
        intptr_t hFind = _findfirst(search, &fd);
        if(hFind == -1) { vm->r[1] = -1; return; }
        do {
            if(count >= max_entries) break;
            if(strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0) continue;
            u32 off = buf_ptr + (u32)(count * DIR_ENTRY_SIZE);
            relic_far_set(vm->mem, off, 0, DIR_ENTRY_SIZE);
            {
                size_t nl = strlen(fd.name);
                if(nl > DIR_NAME_SIZE - 1) nl = DIR_NAME_SIZE - 1;
                relic_far_write(vm->mem, off, fd.name, nl);
            }
            i32 sz = (i32)fd.size;
            i32 is_dir = (fd.attrib & _A_SUBDIR) ? 1 : 0;
            i32 readable = 1;
            i32 writable = !(fd.attrib & _A_RDONLY) ? 1 : 0;
            i32 mtime = (i32)fd.time_write;
            relic_far_write(vm->mem, off + 256, &sz, 4);
            relic_far_write(vm->mem, off + 260, &is_dir, 4);
            relic_far_write(vm->mem, off + 264, &readable, 4);
            relic_far_write(vm->mem, off + 268, &writable, 4);
            relic_far_write(vm->mem, off + 272, &mtime, 4);
            count++;
        } while(_findnext(hFind, &fd) == 0);
        _findclose(hFind);
#elif defined(__ia16__)
        /* DOS: libi86 _dos_findfirst/_dos_findnext.  Names come back in
         * 8.3 form, which is what a real-mode program should see anyway. */
        char search[80];
        snprintf(search, sizeof(search), "%s\\*.*", path);
        struct find_t fd;
        if(_dos_findfirst(search, _A_NORMAL | _A_RDONLY | _A_SUBDIR | _A_ARCH,
                          &fd) != 0) { vm->r[1] = -1; return; }
        do {
            if(count >= max_entries) break;
            if(strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0) continue;
            u32 off = buf_ptr + (u32)(count * DIR_ENTRY_SIZE);
            relic_far_set(vm->mem, off, 0, DIR_ENTRY_SIZE);
            {
                size_t nl = strlen(fd.name);
                if(nl > DIR_NAME_SIZE - 1) nl = DIR_NAME_SIZE - 1;
                relic_far_write(vm->mem, off, fd.name, nl);
            }
            i32 sz = (i32)fd.size;
            i32 is_dir = (fd.attrib & _A_SUBDIR) ? 1 : 0;
            i32 readable = 1;
            i32 writable = (fd.attrib & _A_RDONLY) ? 0 : 1;
            i32 mtime = 0;
            relic_far_write(vm->mem, off + 256, &sz, 4);
            relic_far_write(vm->mem, off + 260, &is_dir, 4);
            relic_far_write(vm->mem, off + 264, &readable, 4);
            relic_far_write(vm->mem, off + 268, &writable, 4);
            relic_far_write(vm->mem, off + 272, &mtime, 4);
            count++;
        } while(_dos_findnext(&fd) == 0);
        _dos_findclose(&fd);
#else
        DIR *dir = opendir(fsp);
        if(!dir) { vm->r[1] = -1; return; }
        struct dirent *ent;
        while((ent = readdir(dir)) != NULL && count < max_entries) {
            if(strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            u32 off = buf_ptr + (u32)(count * DIR_ENTRY_SIZE);
            relic_far_set(vm->mem, off, 0, DIR_ENTRY_SIZE);
            {
                size_t nl = strlen(ent->d_name);
                if(nl > DIR_NAME_SIZE - 1) nl = DIR_NAME_SIZE - 1;
                relic_far_write(vm->mem, off, ent->d_name, nl);
            }
            char full[600];
            snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
            vm_fs_path(full, (int)sizeof full, full);
            struct stat st;
            i32 sz = 0, is_dir = 0, readable = 0, writable = 0, mtime = 0;
            if(stat(full, &st) == 0) {
                sz = (i32)st.st_size;
                is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
                readable = (R_ACCESS(full, R_F_OK) == 0) ? 1 : 0;
                writable = (R_ACCESS(full, R_W_OK) == 0) ? 1 : 0;
                mtime = (i32)st.st_mtime;
            }
            relic_far_write(vm->mem, off + 256, &sz, 4);
            relic_far_write(vm->mem, off + 260, &is_dir, 4);
            relic_far_write(vm->mem, off + 264, &readable, 4);
            relic_far_write(vm->mem, off + 268, &writable, 4);
            relic_far_write(vm->mem, off + 272, &mtime, 4);
            count++;
        }
        closedir(dir);
#endif
        vm->r[1] = count;
    }
}
RELIC_OP_EXPORT void relic_op_sys_fpath(RelicVM *vm) {
    /* @file_path(buf: *u8, buf_size: i32) -> i32
     *   r1 = buf (vm->mem offset for output)
     *   r2 = buf_size (capacity)
     *   Returns length of path string (excluding NUL) in r1, or -1 on error. */
    if(vm->backend && vm->backend->sys_fpath) vm->backend->sys_fpath(vm);
    else {
        u32 buf_ptr = (u32)vm->r[1];
        i32 buf_size = vm->r[2];
        if(buf_size <= 0 || buf_ptr + (u32)buf_size > vm->mem_sz) { vm->r[1] = -1; return; }
        char cwd[1024] = {0};
        if(!R_GETCWD(cwd, sizeof(cwd))) { vm->r[1] = -1; return; }
        i32 len = (i32)strlen(cwd);
        if(len >= buf_size) len = buf_size - 1;
        relic_far_write(vm->mem, buf_ptr, cwd, (size_t)len);
        VMMEM8(vm, buf_ptr + (u32)len) = 0;
        vm->r[1] = len;
    }
}
RELIC_OP_EXPORT void relic_op_sys_load_png(RelicVM *vm) {
    /* @file_load_png(path: *u8, buf: *u8, buf_size: i32, out_wh: *i32) -> i32
     *   r1 = path  (vm->mem offset, NUL-terminated string)
     *   r2 = buf   (vm->mem offset for RGBA output)
     *   r3 = buf_size (max bytes caller can hold)
     *   r4 = out_wh (vm->mem offset for 2 x i32: [width, height])
     *   Returns total RGBA bytes (w*h*4) in r1, or -1 on error. */
    u32 path_ptr = (u32)vm->r[1];
    u32 buf_ptr  = (u32)vm->r[2];
    i32 buf_size = vm->r[3];
    u32 wh_ptr   = (u32)vm->r[4];

    if(path_ptr >= vm->mem_sz || buf_ptr >= vm->mem_sz ||
       wh_ptr + 8 > vm->mem_sz || buf_size <= 0) {
        vm->r[1] = -1; return;
    }

    /* Read path string from VM memory (NUL-terminated). */
    char path[512] = {0};
    int plen = 0;
    while(plen < 511 && path_ptr + (u32)plen < vm->mem_sz &&
          VMMEM8(vm, path_ptr + plen) != 0) {
        path[plen] = (char)VMMEM8(vm, path_ptr + plen);
        plen++;
    }
    if(plen == 0) { vm->r[1] = -1; return; }

#ifdef RELIC_IA16
    /* stb_image is not compiled on 16-bit DOS - PNG decode is unsupported. */
    vm->r[1] = -1;
    return;
#else
    /* Source: packed asset blob (memory) or filesystem (streamed via
     * stbi callbacks - a whole-file buffer on the libc heap needlessly
     * fragments small arenas on low-memory targets). Dimensions are
     * probed with stbi_info first so an image that can never fit the
     * caller's buffer is rejected BEFORE paying for a full decode
     * (the decode allocates w*h*4 of RGBA - the worst allocation in
     * this path). */
    int w = 0, h = 0, channels = 0;
    unsigned char *rgba = NULL;

    u32 asset_sz = 0;
    const u8 RELIC_FAR *asset = relic_assets_find(vm->assets, path, &asset_sz);
    FILE *f = NULL;
    stbi_io_callbacks cb;
    if(!asset) {
        char fsp[512];
        vm_fs_path(fsp, (int)sizeof fsp, path);
        f = fopen(fsp, "rb");
        if(!f) { vm->r[1] = -1; return; }
        cb.read  = relic_stbi_fread;
        cb.skip  = relic_stbi_fskip;
        cb.eof   = relic_stbi_feof;
        if(!stbi_info_from_callbacks(&cb, f, &w, &h, &channels)) {
            fclose(f); vm->r[1] = -1; return;
        }
    } else if(!stbi_info_from_memory(asset, (int)asset_sz,
                                     &w, &h, &channels)) {
        vm->r[1] = -1; return;
    }

    /* Validate output fits in caller's buffer before decoding. */
    uint64_t need = (uint64_t)w * (uint64_t)h * 4ull;
    if(need > (uint64_t)buf_size ||
       buf_ptr + (u32)need > vm->mem_sz) {
        if(f) fclose(f);
        vm->r[1] = -1; return;
    }

    if(asset) {
        rgba = stbi_load_from_memory(asset, (int)asset_sz,
                                     &w, &h, &channels, 4);
    } else {
        rewind(f);
        rgba = stbi_load_from_callbacks(&cb, f, &w, &h, &channels, 4);
        fclose(f);
    }
    if(!rgba) { vm->r[1] = -1; return; }

    /* Copy RGBA pixels into VM memory. */
    relic_far_write(vm->mem, buf_ptr, rgba, (size_t)need);
    stbi_image_free(rgba);

    /* Write width/height to out_wh. */
    i32 wh[2] = { w, h };
    relic_far_write(vm->mem, wh_ptr, wh, 8);

    vm->r[1] = (i32)need;
#endif /* !RELIC_IA16 */
}
RELIC_OP_EXPORT void relic_op_sys_fdelete(RelicVM *vm) {
    /* @file_delete(path: *u8) -> i32
     *   r1 = path (vm->mem offset, NUL-terminated string)
     *   Removes a file, or an empty directory, from the real filesystem.
     *   Never touches embedded assets (they aren't real files, so there is
     *   nothing to delete there). Intended for uninstallers.
     *   Returns 0 on success, -1 on failure. */
    if(vm->backend && vm->backend->sys_fdelete) { vm->backend->sys_fdelete(vm); return; }

    u32 path_ptr = (u32)vm->r[1];
    char path[512] = {0};
    int n = 0;
    while(n < 511 && path_ptr + (u32)n < vm->mem_sz && VMMEM8(vm, path_ptr + n) != 0) {
        path[n] = (char)VMMEM8(vm, path_ptr + n);
        n++;
    }
    if(n == 0) { vm->r[1] = -1; return; }
    vm_fs_path(path, (int)sizeof path, path);

    /* POSIX remove() unlinks files and directories alike, but the MSVCRT one
     * only handles files, so fall back to rmdir. Doing it in that order keeps
     * the common (file) case a single syscall on every platform. */
    if(remove(path) == 0) { vm->r[1] = 0; return; }
    vm->r[1] = (R_RMDIR(path) == 0) ? 0 : -1;
}
RELIC_OP_EXPORT void relic_op_sys_fmkdir(RelicVM *vm) {
    /* @file_mkdir(path: *u8) -> i32
     *   r1 = path (vm->mem offset, NUL-terminated string)
     *   Creates a single directory level on the real filesystem (like
     *   mkdir, not mkdir -p - the parent must already exist).
     *   Returns 0 on success, -1 on failure (including "already exists"). */
    if(vm->backend && vm->backend->sys_fmkdir) { vm->backend->sys_fmkdir(vm); return; }

    u32 path_ptr = (u32)vm->r[1];
    char path[512] = {0};
    int n = 0;
    while(n < 511 && path_ptr + (u32)n < vm->mem_sz && VMMEM8(vm, path_ptr + n) != 0) {
        path[n] = (char)VMMEM8(vm, path_ptr + n);
        n++;
    }
    if(n == 0) { vm->r[1] = -1; return; }
    vm_fs_path(path, (int)sizeof path, path);

    vm->r[1] = (R_MKDIR(path) == 0) ? 0 : -1;
}

/* --- System clipboard --- Text only. No portable fallback exists (there
 * is no libc clipboard), so a backend without these hooks simply fails. */
RELIC_OP_EXPORT void relic_op_sys_clip_set(RelicVM *vm) {
    /* @clipboard_set(buf: *u8, len: i32) -> i32
     *   r1 = buf (vm->mem offset), r2 = len (bytes to copy)
     *   Returns 1 on success, 0 on failure. */
    if(vm->backend && vm->backend->sys_clip_set) { vm->backend->sys_clip_set(vm); return; }
    vm->r[1] = 0;
}
RELIC_OP_EXPORT void relic_op_sys_clip_get(RelicVM *vm) {
    /* @clipboard_get(buf: *u8, cap: i32) -> i32
     *   r1 = buf (vm->mem offset, output), r2 = cap (buffer capacity)
     *   Writes a NUL-terminated copy of the clipboard's text, truncated to
     *   fit `cap` (including the terminator). Returns the number of bytes
     *   written (excluding the terminator), 0 if the clipboard is empty,
     *   or -1 on failure. */
    if(vm->backend && vm->backend->sys_clip_get) { vm->backend->sys_clip_get(vm); return; }
    vm->r[1] = -1;
}

RELIC_OP_EXPORT void relic_op_sys_args(RelicVM *vm) {
    /* @args(idx: i32, buf: *u8) -> i32
     *   r1 = idx, r2 = buf (vm->mem offset)
     *   Returns total argc in r1.
     *   If buf != 0 and idx in [0, argc), copies argv[idx] (NUL-terminated)
     *   into the caller's buffer. */
    if(vm->backend && vm->backend->sys_args) vm->backend->sys_args(vm);
    else {
        i32 idx = vm->r[1];
        u32 buf = (u32)vm->r[2];
        int argc = vm->cli_argc;
        char **argv = vm->cli_argv;
        if(buf && argv && idx >= 0 && idx < argc && argv[idx]) {
            const char *s = argv[idx];
            u32 len = (u32)strlen(s);
            /* Bounds-check: leave room for trailing NUL. */
            if(buf + len + 1 <= vm->mem_sz) {
                relic_far_write(vm->mem, buf, s, len);
                VMMEM8(vm, buf + len) = 0;
            }
        }
        vm->r[1] = argc;
    }
}
RELIC_OP_EXPORT void relic_op_sys_isnum(RelicVM *vm) {
    /* @is_number(v: f32) -> i32
     *   0 = NaN, 1 = +Infinity, 2 = -Infinity, 3 = Normal/finite */
    if(vm->backend && vm->backend->sys_isnum) vm->backend->sys_isnum(vm);
    else {
        RelicFBits in = { .i = vm->r[1] };
        float f = in.f;
        int r;
        if(f != f)                              r = 0;  /* NaN */
        else if(f >  3.4028234663852886e+38f)   r = 1;  /* +Inf */
        else if(f < -3.4028234663852886e+38f)   r = 2;  /* -Inf */
        else                                    r = 3;  /* Normal/finite */
        vm->r[1] = r;
    }
}
RELIC_OP_EXPORT void relic_op_sys_sin(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_sin) vm->backend->sys_sin(vm);
    else {
        RelicFBits in  = { .i = vm->r[1] };
        RelicFBits out = { .f = sinf(in.f) };
        vm->r[1] = out.i;
    }
}
RELIC_OP_EXPORT void relic_op_sys_cos(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_cos) vm->backend->sys_cos(vm);
    else {
        RelicFBits in  = { .i = vm->r[1] };
        RelicFBits out = { .f = cosf(in.f) };
        vm->r[1] = out.i;
    }
}
RELIC_OP_EXPORT void relic_op_sys_sqrt(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_sqrt) vm->backend->sys_sqrt(vm);
    else {
        RelicFBits in  = { .i = vm->r[1] };
        RelicFBits out = { .f = sqrtf(in.f) };
        vm->r[1] = out.i;
    }
}
/* Extended math: same r1-in / r1-out convention as sin/cos/sqrt; the
 * two-arg forms read r1 and r2. Backend hooks may override (e.g. a
 * platform with no libm); the fallback calls the C library directly. */
#define RELIC_DEF_MATH1(NAME, FN)                                              \
RELIC_OP_EXPORT void relic_op_sys_##NAME(RelicVM *vm) {                        \
    if(vm->backend && vm->backend->sys_##NAME) vm->backend->sys_##NAME(vm);    \
    else {                                                                   \
        RelicFBits in  = { .i = vm->r[1] };                                  \
        RelicFBits out = { .f = FN(in.f) };                                  \
        vm->r[1] = out.i;                                                    \
    }                                                                        \
}
#define RELIC_DEF_MATH2(NAME, FN)                                              \
RELIC_OP_EXPORT void relic_op_sys_##NAME(RelicVM *vm) {                        \
    if(vm->backend && vm->backend->sys_##NAME) vm->backend->sys_##NAME(vm);    \
    else {                                                                   \
        RelicFBits a   = { .i = vm->r[1] };                                  \
        RelicFBits b   = { .i = vm->r[2] };                                  \
        RelicFBits out = { .f = FN(a.f, b.f) };                              \
        vm->r[1] = out.i;                                                    \
    }                                                                        \
}
/* roundf() is C99 and absent from some older libms (msvcrt, DJGPP).
 * This is the same round-half-away-from-zero, built from floorf. */
static float relic_round_half_away(float x) {
    return x < 0 ? -floorf(-x + 0.5f) : floorf(x + 0.5f);
}
/* f64 counterpart for OP_DROUND - keeps @round identical across widths. */
static double relic_dround_half_away(double x) {
    return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5);
}
RELIC_DEF_MATH1(exp,   expf)
RELIC_DEF_MATH1(log,   logf)
RELIC_DEF_MATH1(log10, log10f)
RELIC_DEF_MATH2(pow,   powf)
RELIC_DEF_MATH1(atan,  atanf)
RELIC_DEF_MATH2(atan2, atan2f)
RELIC_DEF_MATH1(tan,   tanf)
RELIC_DEF_MATH1(asin,  asinf)
RELIC_DEF_MATH1(acos,  acosf)
RELIC_DEF_MATH1(floor, floorf)
RELIC_DEF_MATH1(ceil,  ceilf)
RELIC_DEF_MATH1(round, relic_round_half_away)
RELIC_DEF_MATH2(fmod,  fmodf)
RELIC_DEF_MATH1(fabs,  fabsf)
RELIC_DEF_MATH1(sinh,  sinhf)
RELIC_DEF_MATH1(cosh,  coshf)
RELIC_DEF_MATH1(tanh,  tanhf)

RELIC_OP_EXPORT void relic_op_sys_platform(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_platform) vm->backend->sys_platform(vm);
    else {
        /* Default fallback: write zeros to struct at r1 (116 bytes) */
        u32 ptr = (u32)vm->r[1];
        if(ptr + 116 <= vm->mem_sz) relic_far_set(vm->mem, ptr, 0, 116);
    }
}

/* --- Web / HTTP (poll-based, non-blocking; see docs/web.md) ---
 * Networking lives entirely in the backend. When a backend does not
 * implement these, the VM returns "not supported": handle/status/read all
 * yield -1 and close is a no-op, so scripts degrade gracefully. */
RELIC_OP_EXPORT void relic_op_sys_http_request(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_http_request) vm->backend->sys_http_request(vm);
    else vm->r[1] = -1;
}
RELIC_OP_EXPORT void relic_op_sys_http_poll(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_http_poll) vm->backend->sys_http_poll(vm);
    else vm->r[1] = -1;
}
RELIC_OP_EXPORT void relic_op_sys_http_status(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_http_status) vm->backend->sys_http_status(vm);
    else vm->r[1] = -1;
}
RELIC_OP_EXPORT void relic_op_sys_http_read(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_http_read) vm->backend->sys_http_read(vm);
    else vm->r[1] = -1;
}
RELIC_OP_EXPORT void relic_op_sys_http_close(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_http_close) vm->backend->sys_http_close(vm);
}

/* --- Embedded-host OS syscalls (OP_SYS_OS; see RelicOSOps in tigryx.h) ---
 * The sub-op selector rides in the instruction's SRCA field; call args
 * stay in r1..rN. Path/pointer args are guest-memory offsets, translated
 * here before the host sees them. No vtable -> not an OS host -> -1. */
RELIC_OP_EXPORT void relic_op_sys_os(RelicVM *vm, u32 instr) {
    const RelicOSOps *ops = vm->os_ops;
    if(!ops) { vm->r[1] = -1; return; }
    switch(IR_SRCA(instr)) {
        case RELIC_OS_SPAWN: {
            u32 off = (u32)vm->r[1];
            char path[256];
            int n = 0;
            if(!ops->spawn || off >= vm->mem_sz) { vm->r[1] = -1; break; }
            while(n < 255 && off + (u32)n < vm->mem_sz &&
                  VMMEM8(vm, off + n) != 0) n++;
            relic_far_read(vm->mem, off, path, n);
            path[n] = 0;
            vm->r[1] = ops->spawn(vm, path);
            break;
        }
        case RELIC_OS_KILL:
            vm->r[1] = ops->kill ? ops->kill(vm, vm->r[1]) : -1; break;
        case RELIC_OS_STATUS:
            vm->r[1] = ops->status ? ops->status(vm, vm->r[1]) : -1; break;
        case RELIC_OS_FOCUS:
            vm->r[1] = ops->focus ? ops->focus(vm, vm->r[1]) : -1; break;
        case RELIC_OS_COUNT:
            vm->r[1] = ops->count ? ops->count(vm) : -1; break;
        case RELIC_OS_HANDLE:
            vm->r[1] = ops->handle_at ? ops->handle_at(vm, vm->r[1]) : -1;
            break;
        case RELIC_OS_SET_RECT:
            vm->r[1] = ops->set_rect
                       ? ops->set_rect(vm, vm->r[1], vm->r[2], vm->r[3],
                                       vm->r[4], vm->r[5])
                       : -1;
            break;
        case RELIC_OS_GET_RECT: {
            u32 off = (u32)vm->r[2];
            i32 out[4];
            if(!ops->get_rect) { vm->r[1] = -1; break; }
            int ok = ops->get_rect(vm, vm->r[1], out);
            /* off + 16 <= mem_sz would wrap for off near 2^32-1;
             * test the difference instead */
            if(ok && off <= vm->mem_sz && vm->mem_sz - off >= 16)
                relic_far_write(vm->mem, off, out, 16);
            vm->r[1] = ok;
            break;
        }
        case RELIC_OS_NAME: {
            u32 off = (u32)vm->r[2];
            i32 cap = vm->r[3];
            i32 room;
            char buf[96];
            if(!ops->name || off >= vm->mem_sz || cap <= 0)
                { vm->r[1] = -1; break; }
            int n = ops->name(vm, vm->r[1], buf, sizeof buf);
            if(n < 0) { vm->r[1] = -1; break; }
            if(n > cap - 1) n = cap - 1;
            room = (i32)(vm->mem_sz - off) - 1;   /* keep the NUL inside */
            if(room < 0) room = 0;
            if(n > room) n = room;
            relic_far_write(vm->mem, off, buf, n + 1); /* + NUL */
            vm->r[1] = n;
            break;
        }
        case RELIC_OS_ICONIFY:
            vm->r[1] = ops->iconify
                       ? ops->iconify(vm, vm->r[1], vm->r[2]) : -1;
            break;
        case RELIC_OS_DOCK:
            vm->r[1] = ops->dock
                       ? ops->dock(vm, vm->r[1], vm->r[2], vm->r[3],
                                   vm->r[4], vm->r[5])
                       : -1;
            break;
        default:
            vm->r[1] = -1;
            break;
    }
}

/* --- Subprocess management (poll-based, non-blocking; same pattern as
 * the HTTP ops above). Process spawning lives entirely in the backend.
 * When a backend does not implement these, the VM returns "not supported":
 * spawn/poll/exit_code/read_stdout/read_stderr/kill all yield -1 and
 * close is a no-op, so scripts degrade gracefully. */
RELIC_OP_EXPORT void relic_op_sys_proc_spawn(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_proc_spawn) vm->backend->sys_proc_spawn(vm);
    else vm->r[1] = -1;
}
RELIC_OP_EXPORT void relic_op_sys_proc_poll(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_proc_poll) vm->backend->sys_proc_poll(vm);
    else vm->r[1] = -1;
}
RELIC_OP_EXPORT void relic_op_sys_proc_exit_code(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_proc_exit_code) vm->backend->sys_proc_exit_code(vm);
    else vm->r[1] = -1;
}
RELIC_OP_EXPORT void relic_op_sys_proc_read_out(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_proc_read_out) vm->backend->sys_proc_read_out(vm);
    else vm->r[1] = -1;
}
RELIC_OP_EXPORT void relic_op_sys_proc_read_err(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_proc_read_err) vm->backend->sys_proc_read_err(vm);
    else vm->r[1] = -1;
}
RELIC_OP_EXPORT void relic_op_sys_proc_kill(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_proc_kill) vm->backend->sys_proc_kill(vm);
    else vm->r[1] = -1;
}
RELIC_OP_EXPORT void relic_op_sys_proc_close(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_proc_close) vm->backend->sys_proc_close(vm);
    else vm->r[1] = -1;
}

/* --- Tiny socket server (@serve / @serve_reply) ---
 * A listening socket lives entirely in the backend. When a backend does
 * not implement these (anything not built with TIGRYX_EXPERIMENTAL_SERVER),
 * the VM returns "not supported": @serve yields -1 for every cmd and
 * @serve_reply yields -1, so scripts degrade gracefully. */
RELIC_OP_EXPORT void relic_op_sys_serve(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_serve) vm->backend->sys_serve(vm);
    else vm->r[1] = -1;
}
RELIC_OP_EXPORT void relic_op_sys_serve_reply(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_serve_reply) vm->backend->sys_serve_reply(vm);
    else vm->r[1] = -1;
}

/* --- ZIP extraction (@unzip) ---
 * Uses miniz to extract all files from a ZIP archive to a destination
 * directory. Streaming extraction to disk via mz_zip_reader_extract_to_file
 * keeps memory usage low (fixed ~32KB inflate buffer), which is critical
 * for low-memory targets like DOS. */
static void vm_mkdir_p(const char *path) {
    char tmp[512];
    int n = 0;
    while(path[n] && n < 511) { tmp[n] = path[n]; n++; }
    tmp[n] = 0;
    vm_fs_path(tmp, (int)sizeof tmp, tmp);
    for(int i = 1; i < n; i++) {
        if(VM_PATHSEP(tmp[i])) {
            char c = tmp[i];
            tmp[i] = 0;
            if(tmp[0]) R_MKDIR(tmp);
            tmp[i] = c;
        }
    }
    if(tmp[0]) R_MKDIR(tmp);
}

RELIC_OP_EXPORT void relic_op_sys_unzip(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_unzip) { vm->backend->sys_unzip(vm); return; }

#ifdef RELIC_IA16
    /* miniz is not linked into the real-mode DOS build (inflate tables
     * would not fit near data); report failure. */
    vm->r[1] = -1; return;
#else
    /* r1 = zip_path ptr, r2 = dest_path ptr */
    u32 zip_ptr = (u32)vm->r[1];
    u32 dest_ptr = (u32)vm->r[2];
    char zip_path[512];
    char dest_path[512];

    /* Read zip_path from VM memory (NUL-terminated) */
    int n = 0;
    while(n < 511 && zip_ptr + (u32)n < vm->mem_sz && VMMEM8(vm, zip_ptr + n) != 0)
        zip_path[n] = VMMEM8(vm, zip_ptr + n), n++;
    zip_path[n] = 0;

    /* Read dest_path from VM memory (NUL-terminated) */
    n = 0;
    while(n < 511 && dest_ptr + (u32)n < vm->mem_sz && VMMEM8(vm, dest_ptr + n) != 0)
        dest_path[n] = VMMEM8(vm, dest_ptr + n), n++;
    dest_path[n] = 0;

    if(zip_path[0] == 0 || dest_path[0] == 0) { vm->r[1] = -1; return; }

    /* Ensure the destination directory exists */
    vm_mkdir_p(dest_path);

    /* On a failure we log <dest>\UNZIPERR.TXT — the GUI installer cannot
     * show stderr, so the detail must land somewhere the user can read
     * after exit. */
    char log_path[512];
    int lp = 0;
    while(dest_path[lp] && lp < 500) log_path[lp] = dest_path[lp], lp++;
    if(lp > 0 && log_path[lp-1] != '/' && log_path[lp-1] != '\\')
        log_path[lp++] = '/';
    log_path[lp] = 0;
    {
        const char *lf = "UNZIPERR.TXT";
        for(int k = 0; lf[k]; k++) log_path[lp++] = lf[k];
        log_path[lp] = 0;
    }

    /* Asset-first lookup, same convention as @file_open / @file_load_png:
     * a single-file installer packs its payload.zip into the .tlx asset
     * trailer via `tigryx pack`, so zip_path is looked up there before
     * falling back to a real file on disk. */
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    u32 asset_sz = 0;
#ifdef RELIC_IA16
    /* miniz reads from near memory only; on 16-bit DOS the embedded-asset
     * path is skipped and the zip must be a real file. */
    const u8 RELIC_FAR *asset = NULL;
    (void)asset; (void)asset_sz;
    if(!mz_zip_reader_init_file(&zip, zip_path, 0)) { vm->r[1] = -1; return; }
#else
    const u8 RELIC_FAR *asset = relic_assets_find(vm->assets, zip_path, &asset_sz);
    const char *init_err = NULL;
    if(asset) {
        if(!mz_zip_reader_init_mem(&zip, asset, asset_sz, 0))
            init_err = "not a valid packed zip";
    } else {
        char fs_zip[512];
        vm_fs_path(fs_zip, (int)sizeof fs_zip, zip_path);
        if(!mz_zip_reader_init_file(&zip, fs_zip, 0))
            init_err = "cannot open archive";
    }
    if(init_err) {
        fprintf(stderr, "unzip: %s: %s\n", init_err, zip_path);
        FILE *lf = fopen(log_path, "ab");
        if(!lf) lf = fopen("UNZIPERR.TXT", "ab");
        if(lf) { fprintf(lf, "unzip: %s: %s\n", init_err, zip_path); fclose(lf); }
        vm->r[1] = -1; return;
    }
#endif

    int num_files = (int)mz_zip_reader_get_num_files(&zip);
    int extracted = 0;
    for(int i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat stat;
        if(!mz_zip_reader_file_stat(&zip, i, &stat)) continue;

        /* Build full output path: dest_path / stat.m_filename */
        char out_path[1024];
        int dl = 0;
        while(dest_path[dl] && dl < 510) out_path[dl] = dest_path[dl], dl++;
        if(dl > 0 && out_path[dl-1] != '/' && out_path[dl-1] != '\\')
            out_path[dl++] = '/';
        int fl = 0;
        while(stat.m_filename[fl] && dl + fl < 1023) out_path[dl + fl] = stat.m_filename[fl], fl++;
        out_path[dl + fl] = 0;

        /* Skip directory entries (they end with /) */
        if(stat.m_is_directory) {
            vm_mkdir_p(out_path);
            extracted++;
            continue;
        }

        /* Host-FS view of the output path. On SCL, dots inside the zip
         * member name (FILE1.TXT) become directory boundaries too, so the
         * parent directory must be derived from this translated path. */
        char fs_out[1024];
        vm_fs_path(fs_out, (int)sizeof fs_out, out_path);

        /* Create parent directories for the file */
        char parent[1024];
        int pl = 0;
        while(fs_out[pl] && pl < 1023) parent[pl] = fs_out[pl], pl++;
        parent[pl] = 0;
        for(int j = pl - 1; j >= 0; j--) {
            if(VM_PATHSEP(parent[j])) { parent[j] = 0; break; }
        }
        if(parent[0]) vm_mkdir_p(parent);

        /* Extract directly to disk (streaming, low memory) */
        errno = 0;
        if(mz_zip_reader_extract_to_file(&zip, i, fs_out, 0)) {
            extracted++;
            /* Extraction is synchronous, so the guest never reaches the
             * event loop for the whole archive (minutes on DOS).  Flip
             * every few members: repaints the last frame, pumps keys and
             * keeps the software cursor alive so it doesn't look hung. */
            if((i & 7) == 0 && vm->backend && vm->backend->gfx_flip)
                vm->backend->gfx_flip(vm);
        } else {
            /* The member path + errno makes "permission denied"-class
             * install failures diagnosable — the guest only sees the -1. */
            fprintf(stderr, "unzip: cannot write %s (errno %d)\n",
                    fs_out, errno);
            FILE *lf = fopen(log_path, "ab");
            if(!lf) lf = fopen("UNZIPERR.TXT", "ab");
            if(lf) {
                fprintf(lf, "unzip: cannot write %s (errno %d)\n",
                        fs_out, errno);
                fclose(lf);
            }
        }
    }

    mz_zip_reader_end(&zip);
    vm->r[1] = (extracted == num_files) ? 0 : -1;
#endif /* RELIC_IA16 */
}

/* --- Platform registration (@register) ------------------------------------
 * Installers need to tell the host OS about the program: which file
 * extensions it owns, what icon they get, how to launch it, and where it
 * lives. That is inherently per-OS, so the dispatch below is a thin switch
 * over RelicRegisterAction with one implementation block per platform.
 *
 * This lives in the VM core rather than in a graphics backend for two
 * reasons: the registry logic is identical for opengl-win32 and
 * relic3d-win32 (no reason to maintain it twice), and `tigryxvm` /
 * `tigryxri` have no GUI backend at all yet are the binaries an installer
 * is most likely to invoke.
 * ------------------------------------------------------------------------- */

/* Copy a NUL-terminated string out of VM memory, bounds-checked against
 * mem_sz. Returns the length; the result is always NUL-terminated. */
static int vm_read_cstr(RelicVM *vm, u32 ptr, char *out, int cap) {
    int n = 0;
    while(n < cap - 1 && ptr + (u32)n < vm->mem_sz && VMMEM8(vm, ptr + n) != 0)
        out[n] = (char)VMMEM8(vm, ptr + n), n++;
    out[n] = 0;
    return n;
}

#ifdef _WIN32
/* ---- Win32 -------------------------------------------------------------- */

/* Every API below is resolved at runtime so that vm.o adds no import-library
 * dependency. advapi32/shell32/user32/ole32 are all present on Win95 through
 * Windows 11, so the lookups succeed everywhere in practice; a missing entry
 * point degrades to RELIC_REG_ERR_UNSUP rather than failing to load. */
typedef LONG (WINAPI *PFN_RegCreateKeyExA)(HKEY, LPCSTR, DWORD, LPSTR, DWORD,
                                           REGSAM, LPVOID, PHKEY, LPDWORD);
typedef LONG (WINAPI *PFN_RegSetValueExA)(HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD);
typedef LONG (WINAPI *PFN_RegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG (WINAPI *PFN_RegOpenKeyExA)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LONG (WINAPI *PFN_RegDeleteKeyA)(HKEY, LPCSTR);
typedef LONG (WINAPI *PFN_RegCloseKey)(HKEY);
typedef void (WINAPI *PFN_SHChangeNotify)(LONG, UINT, LPCVOID, LPCVOID);
typedef LRESULT (WINAPI *PFN_SendMessageTimeoutA)(HWND, UINT, WPARAM, LPARAM,
                                                  UINT, UINT, PDWORD_PTR);
typedef HRESULT (WINAPI *PFN_SHGetFolderPathA)(HWND, int, HANDLE, DWORD, LPSTR);
typedef BOOL (WINAPI *PFN_CreateProcessA)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES,
    LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR,
    LPSTARTUPINFOA, LPPROCESS_INFORMATION);

static struct {
    int loaded;
    PFN_RegCreateKeyExA     RegCreateKeyExA_;
    PFN_RegSetValueExA      RegSetValueExA_;
    PFN_RegQueryValueExA    RegQueryValueExA_;
    PFN_RegOpenKeyExA       RegOpenKeyExA_;
    PFN_RegDeleteKeyA       RegDeleteKeyA_;
    PFN_RegCloseKey         RegCloseKey_;
    PFN_SHChangeNotify      SHChangeNotify_;
    PFN_SendMessageTimeoutA SendMessageTimeoutA_;
    PFN_SHGetFolderPathA    SHGetFolderPathA_;
    PFN_CreateProcessA      CreateProcessA_;
} g_vmreg;

static void vmreg_load(void) {
    HMODULE h;
    if(g_vmreg.loaded) return;
    g_vmreg.loaded = 1;

    h = LoadLibraryA("advapi32.dll");
    if(h) {
        g_vmreg.RegCreateKeyExA_  = (PFN_RegCreateKeyExA) (void*)GetProcAddress(h, "RegCreateKeyExA");
        g_vmreg.RegSetValueExA_   = (PFN_RegSetValueExA)  (void*)GetProcAddress(h, "RegSetValueExA");
        g_vmreg.RegQueryValueExA_ = (PFN_RegQueryValueExA)(void*)GetProcAddress(h, "RegQueryValueExA");
        g_vmreg.RegOpenKeyExA_    = (PFN_RegOpenKeyExA)   (void*)GetProcAddress(h, "RegOpenKeyExA");
        g_vmreg.RegDeleteKeyA_    = (PFN_RegDeleteKeyA)   (void*)GetProcAddress(h, "RegDeleteKeyA");
        g_vmreg.RegCloseKey_      = (PFN_RegCloseKey)     (void*)GetProcAddress(h, "RegCloseKey");
    }
    h = LoadLibraryA("shell32.dll");
    if(h) {
        g_vmreg.SHChangeNotify_ = (PFN_SHChangeNotify)(void*)GetProcAddress(h, "SHChangeNotify");
        g_vmreg.SHGetFolderPathA_ = (PFN_SHGetFolderPathA)(void*)GetProcAddress(h, "SHGetFolderPathA");
    }
    /* SHGetFolderPath is in SHFOLDER.dll on pre-IE5 Win9x; fall back there. */
    if(!g_vmreg.SHGetFolderPathA_) {
        h = LoadLibraryA("shfolder.dll");
        if(h) g_vmreg.SHGetFolderPathA_ = (PFN_SHGetFolderPathA)(void*)GetProcAddress(h, "SHGetFolderPathA");
    }
    h = LoadLibraryA("user32.dll");
    if(h) g_vmreg.SendMessageTimeoutA_ =
              (PFN_SendMessageTimeoutA)(void*)GetProcAddress(h, "SendMessageTimeoutA");
    /* CreateProcessA lives in kernel32.dll, which is always loaded in any
     * Windows process, but we resolve it dynamically to keep vm.o free of
     * import-library dependencies (same rationale as the registry APIs). */
    h = GetModuleHandleA("kernel32.dll");
    if(h) g_vmreg.CreateProcessA_ =
              (PFN_CreateProcessA)(void*)GetProcAddress(h, "CreateProcessA");
}

/* Windows 95/98/ME do not merge HKCU\Software\Classes into HKEY_CLASSES_ROOT
 * (that arrived with IE4/Win98 shell updates and is only dependable on NT).
 * On those systems we must write HKCR directly. Detected at runtime rather
 * than via RELIC_WIN95 so a 9x-targeted binary still does the right thing
 * when run on NT. */
static int vmreg_is_win9x(void) {
    return (GetVersion() & 0x80000000u) != 0;
}

/* Build the full subkey path for a class registration, choosing the hive
 * appropriate to the OS. Returns the root key; writes the subkey to buf. */
static HKEY vmreg_class_key(const char *leaf, char *buf, int cap) {
    if(vmreg_is_win9x()) {
        _snprintf(buf, (size_t)cap, "%s", leaf);
        buf[cap - 1] = 0;
        return HKEY_CLASSES_ROOT;
    }
    /* Per-user classes need no elevation, which keeps the installer usable
     * without an admin prompt. */
    _snprintf(buf, (size_t)cap, "Software\\Classes\\%s", leaf);
    buf[cap - 1] = 0;
    return HKEY_CURRENT_USER;
}

/* Create (or open) a key and write one string value of the given type.
 * name==NULL writes the key's default value. */
static int vmreg_write_typed(HKEY root, const char *subkey, const char *name,
                             const char *data, DWORD type) {
    HKEY hk = NULL;
    LONG rc;
    if(!g_vmreg.RegCreateKeyExA_ || !g_vmreg.RegSetValueExA_ || !g_vmreg.RegCloseKey_)
        return RELIC_REG_ERR_UNSUP;
    rc = g_vmreg.RegCreateKeyExA_(root, subkey, 0, NULL, REG_OPTION_NON_VOLATILE,
                                  KEY_WRITE, NULL, &hk, NULL);
    if(rc != ERROR_SUCCESS) return RELIC_REG_ERR_OS;
    rc = g_vmreg.RegSetValueExA_(hk, name, 0, type,
                                 (const BYTE *)data, (DWORD)(strlen(data) + 1));
    g_vmreg.RegCloseKey_(hk);
    return (rc == ERROR_SUCCESS) ? RELIC_REG_OK : RELIC_REG_ERR_OS;
}

static int vmreg_write(HKEY root, const char *subkey, const char *name, const char *data) {
    return vmreg_write_typed(root, subkey, name, data, REG_SZ);
}

/* Read a string value.
 * Returns the length, -1 if the value is absent, or -2 if it exists but does
 * not fit in the caller's buffer. Callers that rewrite a value MUST treat -2
 * as a hard error: mistaking "too long" for "absent" would replace the user's
 * data with a truncated version of it. */
#define VMREG_READ_ABSENT   (-1)
#define VMREG_READ_TOO_LONG (-2)
static int vmreg_read(HKEY root, const char *subkey, const char *name,
                      char *out, int cap, DWORD *out_type) {
    HKEY hk = NULL;
    DWORD type = 0, sz = (DWORD)cap;
    LONG rc;
    out[0] = 0;
    if(out_type) *out_type = REG_SZ;
    if(!g_vmreg.RegOpenKeyExA_ || !g_vmreg.RegQueryValueExA_ || !g_vmreg.RegCloseKey_)
        return VMREG_READ_ABSENT;
    if(g_vmreg.RegOpenKeyExA_(root, subkey, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return VMREG_READ_ABSENT;
    rc = g_vmreg.RegQueryValueExA_(hk, name, NULL, &type, (LPBYTE)out, &sz);
    g_vmreg.RegCloseKey_(hk);
    if(rc == ERROR_MORE_DATA) { out[0] = 0; return VMREG_READ_TOO_LONG; }
    if(rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        out[0] = 0;
        return VMREG_READ_ABSENT;
    }
    out[cap - 1] = 0;
    if(out_type) *out_type = type;
    return (int)strlen(out);
}

/* Recursively delete a key. RegDeleteKeyA on NT refuses keys with subkeys,
 * and the ProgID trees we create are shallow but not flat
 * (ProgID\shell\open\command), so walk the known leaves bottom-up. */
static void vmreg_delete_tree(HKEY root, const char *subkey) {
    static const char *suffixes[] = {
        "\\shell\\open\\command", "\\shell\\open", "\\shell",
        "\\DefaultIcon", ""
    };
    char path[512];
    int i;
    if(!g_vmreg.RegDeleteKeyA_) return;
    for(i = 0; i < (int)(sizeof suffixes / sizeof suffixes[0]); i++) {
        _snprintf(path, sizeof path, "%s%s", subkey, suffixes[i]);
        path[sizeof path - 1] = 0;
        g_vmreg.RegDeleteKeyA_(root, path);
    }
}

/* On Windows 9x, PATH lives in AUTOEXEC.BAT on the boot drive, not in the
 * registry. We append a SET PATH line (idempotent: if the directory is
 * already present we skip the write). A reboot is required for the change
 * to take effect, so RELIC_REG_OK_REBOOT is returned on success.
 *
 * The boot drive is inferred from COMSPEC (e.g. C:\COMMAND.COM -> C:).
 * Falls back to C: if COMSPEC is unset or malformed. */
static int vmreg_add_path_9x(const char *dir) {
    char path[MAX_PATH];
    char buf[8192];
    int len = 0, dlen = (int)strlen(dir);
    const char *comspec;
    FILE *f;

    comspec = getenv("COMSPEC");
    if(comspec && comspec[0] && comspec[1] == ':')
        _snprintf(path, sizeof path, "%c:\\AUTOEXEC.BAT", comspec[0]);
    else
        _snprintf(path, sizeof path, "C:\\AUTOEXEC.BAT");
    path[sizeof path - 1] = 0;

    f = fopen(path, "r");
    if(!f) return RELIC_REG_ERR_OS;
    len = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = 0;

    /* Case-insensitive search for dir in PATH= lines, ;-separated. */
    {
        int i = 0;
        while(i < len) {
            int line_start = i, pos;
            while(i < len && buf[i] != '\n' && buf[i] != '\r') i++;
            {
                int line_end = i;
                while(i < len && (buf[i] == '\n' || buf[i] == '\r')) i++;

                pos = line_start;
                while(pos < line_end && (buf[pos] == ' ' || buf[pos] == '\t')) pos++;

                /* Optional "SET " prefix */
                if(pos + 4 <= line_end && _strnicmp(buf + pos, "SET ", 4) == 0) {
                    pos += 4;
                    while(pos < line_end && (buf[pos] == ' ' || buf[pos] == '\t')) pos++;
                }

                if(pos + 4 > line_end || _strnicmp(buf + pos, "PATH", 4) != 0)
                    continue;

                {
                    int after = pos + 4;
                    if(after < line_end && buf[after] != '=' && buf[after] != ' ' && buf[after] != '\t')
                        continue;
                    pos = after;
                }

                while(pos < line_end && (buf[pos] == '=' || buf[pos] == ' ' || buf[pos] == '\t'))
                    pos++;

                while(pos < line_end) {
                    int seg_start = pos, seg_end, seg_len;
                    while(pos < line_end && buf[pos] != ';') pos++;
                    seg_end = pos;
                    while(seg_end > seg_start && (buf[seg_end-1] == ' ' || buf[seg_end-1] == '\t'))
                        seg_end--;
                    seg_len = seg_end - seg_start;
                    if(seg_len == dlen && _strnicmp(buf + seg_start, dir, dlen) == 0)
                        return RELIC_REG_OK;
                    if(pos < line_end && buf[pos] == ';') pos++;
                    else break;
                }
            }
        }
    }

    f = fopen(path, "a");
    if(!f) return RELIC_REG_ERR_OS;
    if(len > 0 && buf[len - 1] != '\n' && buf[len - 1] != '\r')
        fputs("\r\n", f);
    fprintf(f, "SET PATH=%%PATH%%;%s\r\n", dir);
    fclose(f);

    return RELIC_REG_OK_REBOOT;
}

/* Append a directory to the user's PATH, skipping the write if it is already
 * present so repeated installs do not grow the variable without bound. */
static int vmreg_add_path(const char *dir) {
    char cur[4096], next[4608];
    DWORD type = REG_SZ;
    int len;

    if(vmreg_is_win9x())
        return vmreg_add_path_9x(dir);

    len = vmreg_read(HKEY_CURRENT_USER, "Environment", "Path", cur, (int)sizeof cur, &type);
    if(len == VMREG_READ_TOO_LONG) {
        /* The existing PATH does not fit in our buffer. Appending would mean
         * writing back a truncated copy and destroying entries, so refuse. */
        return RELIC_REG_ERR_OS;
    }
    if(len < 0) { cur[0] = 0; len = 0; }
    if(len + (int)strlen(dir) + 2 > (int)sizeof next) return RELIC_REG_ERR_OS;

    /* Case-insensitive substring test bounded by ';' so "C:\Rel" does not
     * match an existing "C:\Tigryx". */
    {
        const char *p = cur;
        size_t dlen = strlen(dir);
        while(*p) {
            const char *end = strchr(p, ';');
            size_t seg = end ? (size_t)(end - p) : strlen(p);
            if(seg == dlen && _strnicmp(p, dir, dlen) == 0) return RELIC_REG_OK;
            if(!end) break;
            p = end + 1;
        }
    }

    if(len > 0 && cur[len - 1] != ';')
        _snprintf(next, sizeof next, "%s;%s", cur, dir);
    else
        _snprintf(next, sizeof next, "%s%s", cur, dir);
    next[sizeof next - 1] = 0;

    /* Write back with the type it already had. PATH is usually REG_EXPAND_SZ
     * because it contains %USERPROFILE%-style references; downgrading it to
     * REG_SZ would leave those unexpanded. */
    if(vmreg_write_typed(HKEY_CURRENT_USER, "Environment", "Path", next, type) != RELIC_REG_OK)
        return RELIC_REG_ERR_OS;

    /* Tell already-running shells to re-read the environment. Without this
     * the new PATH is invisible until the next logon. */
    if(g_vmreg.SendMessageTimeoutA_) {
        DWORD_PTR unused = 0;
        g_vmreg.SendMessageTimeoutA_(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                                     (LPARAM)"Environment", SMTO_ABORTIFHUNG,
                                     5000, &unused);
    }
    return RELIC_REG_OK;
}

/* Remove a directory previously added by vmreg_add_path / REG_PATH. If the
 * directory is not present, returns RELIC_REG_OK without modifying anything
 * (idempotent, so uninstallers can call this unconditionally). */
static int vmreg_remove_path(const char *dir) {
    char cur[4096], next[4096];
    DWORD type = REG_SZ;
    int len, found = 0;
    size_t dlen = strlen(dir);

    if(vmreg_is_win9x())
        return RELIC_REG_ERR_UNSUP; /* AUTOEXEC.BAT editing is not implemented for removal */

    len = vmreg_read(HKEY_CURRENT_USER, "Environment", "Path", cur, (int)sizeof cur, &type);
    if(len == VMREG_READ_TOO_LONG) return RELIC_REG_ERR_OS;
    if(len <= 0) return RELIC_REG_OK; /* nothing set -> nothing to remove */

    /* Rebuild the PATH, dropping any segment that case-insensitively matches
     * dir, and any redundant ';' this creates. */
    {
        const char *p = cur;
        int out = 0;
        next[0] = 0;
        while(*p) {
            const char *end = strchr(p, ';');
            size_t seg = end ? (size_t)(end - p) : strlen(p);
            int is_match = (seg == dlen && _strnicmp(p, dir, dlen) == 0);
            if(is_match) {
                found = 1;
            } else if(seg > 0) {
                if(out > 0) next[out++] = ';';
                if(out + (int)seg >= (int)sizeof next) return RELIC_REG_ERR_OS;
                memcpy(next + out, p, seg);
                out += (int)seg;
            }
            if(!end) break;
            p = end + 1;
        }
        next[out] = 0;
    }

    if(!found) return RELIC_REG_OK;

    if(vmreg_write_typed(HKEY_CURRENT_USER, "Environment", "Path", next, type) != RELIC_REG_OK)
        return RELIC_REG_ERR_OS;

    if(g_vmreg.SendMessageTimeoutA_) {
        DWORD_PTR unused = 0;
        g_vmreg.SendMessageTimeoutA_(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                                     (LPARAM)"Environment", SMTO_ABORTIFHUNG,
                                     5000, &unused);
    }
    return RELIC_REG_OK;
}

/* Shortcut creation needs IShellLink + IPersistFile. Pulling in <shlobj.h>
 * would require dropping WIN32_LEAN_AND_MEAN and is unreliable on the legacy
 * GCC 4.7 MinGW used for the Win95 target, so the two interfaces are declared
 * by hand here. Only the vtable layout matters, and it has been frozen since
 * Windows 95 -- these are binary-compatible ABI contracts, not headers that
 * can drift. Slots we never call are typed as generic function pointers.
 *
 * IUnknown is spelled as void* throughout to avoid needing unknwn.h. */
#define VMREG_CLSCTX_INPROC_SERVER 0x1u

typedef struct VmRegShellLink VmRegShellLink;
typedef struct VmRegPersistFile VmRegPersistFile;

typedef struct {
    HRESULT (WINAPI *QueryInterface)(VmRegShellLink *, const GUID *, void **);
    ULONG   (WINAPI *AddRef)(VmRegShellLink *);
    ULONG   (WINAPI *Release)(VmRegShellLink *);
    HRESULT (WINAPI *GetPath)(VmRegShellLink *, char *, int, void *, DWORD);
    HRESULT (WINAPI *GetIDList)(VmRegShellLink *, void **);
    HRESULT (WINAPI *SetIDList)(VmRegShellLink *, void *);
    HRESULT (WINAPI *GetDescription)(VmRegShellLink *, char *, int);
    HRESULT (WINAPI *SetDescription)(VmRegShellLink *, const char *);
    HRESULT (WINAPI *GetWorkingDirectory)(VmRegShellLink *, char *, int);
    HRESULT (WINAPI *SetWorkingDirectory)(VmRegShellLink *, const char *);
    HRESULT (WINAPI *GetArguments)(VmRegShellLink *, char *, int);
    HRESULT (WINAPI *SetArguments)(VmRegShellLink *, const char *);
    HRESULT (WINAPI *GetHotkey)(VmRegShellLink *, WORD *);
    HRESULT (WINAPI *SetHotkey)(VmRegShellLink *, WORD);
    HRESULT (WINAPI *GetShowCmd)(VmRegShellLink *, int *);
    HRESULT (WINAPI *SetShowCmd)(VmRegShellLink *, int);
    HRESULT (WINAPI *GetIconLocation)(VmRegShellLink *, char *, int, int *);
    HRESULT (WINAPI *SetIconLocation)(VmRegShellLink *, const char *, int);
    HRESULT (WINAPI *SetRelativePath)(VmRegShellLink *, const char *, DWORD);
    HRESULT (WINAPI *Resolve)(VmRegShellLink *, HWND, DWORD);
    HRESULT (WINAPI *SetPath)(VmRegShellLink *, const char *);
} VmRegShellLinkVtbl;
struct VmRegShellLink { const VmRegShellLinkVtbl *lpVtbl; };

typedef struct {
    HRESULT (WINAPI *QueryInterface)(VmRegPersistFile *, const GUID *, void **);
    ULONG   (WINAPI *AddRef)(VmRegPersistFile *);
    ULONG   (WINAPI *Release)(VmRegPersistFile *);
    HRESULT (WINAPI *GetClassID)(VmRegPersistFile *, GUID *);
    HRESULT (WINAPI *IsDirty)(VmRegPersistFile *);
    HRESULT (WINAPI *Load)(VmRegPersistFile *, const WCHAR *, DWORD);
    HRESULT (WINAPI *Save)(VmRegPersistFile *, const WCHAR *, BOOL);
    HRESULT (WINAPI *SaveCompleted)(VmRegPersistFile *, const WCHAR *);
    HRESULT (WINAPI *GetCurFile)(VmRegPersistFile *, WCHAR **);
} VmRegPersistFileVtbl;
struct VmRegPersistFile { const VmRegPersistFileVtbl *lpVtbl; };

/* Create a .lnk via IShellLink. ole32 is loaded on demand and released
 * afterwards, so non-installer programs never pay for COM.
 *
 * icon_spec is an optional "path,index" string; when supplied the shortcut
 * gets an explicit icon rather than inheriting the target's. */
static int vmreg_shortcut(const char *target, const char *lnk_path, const char *icon_spec) {
    typedef HRESULT (WINAPI *PFN_CoInitialize)(void *);
    typedef void    (WINAPI *PFN_CoUninitialize)(void);
    typedef HRESULT (WINAPI *PFN_CoCreateInstance)(const GUID *, void *, DWORD,
                                                   const GUID *, void **);

    static const GUID clsid_shelllink =
        { 0x00021401, 0x0000, 0x0000, { 0xC0,0,0,0,0,0,0,0x46 } };
    static const GUID iid_ishelllinka =
        { 0x000214EE, 0x0000, 0x0000, { 0xC0,0,0,0,0,0,0,0x46 } };
    static const GUID iid_ipersistfile =
        { 0x0000010B, 0x0000, 0x0000, { 0xC0,0,0,0,0,0,0,0x46 } };

    HMODULE ole;
    PFN_CoInitialize     co_init;
    PFN_CoUninitialize   co_uninit;
    PFN_CoCreateInstance co_create;
    VmRegShellLink   *link = NULL;
    VmRegPersistFile *file = NULL;
    WCHAR wpath[512];
    HRESULT hr;
    int result = RELIC_REG_ERR_OS;

    ole = LoadLibraryA("ole32.dll");
    if(!ole) return RELIC_REG_ERR_UNSUP;
    co_init   = (PFN_CoInitialize)    (void*)GetProcAddress(ole, "CoInitialize");
    co_uninit = (PFN_CoUninitialize)  (void*)GetProcAddress(ole, "CoUninitialize");
    co_create = (PFN_CoCreateInstance)(void*)GetProcAddress(ole, "CoCreateInstance");
    if(!co_init || !co_create) { FreeLibrary(ole); return RELIC_REG_ERR_UNSUP; }

    /* S_FALSE means COM was already initialised on this thread, which is
     * still usable -- only a hard failure aborts. */
    hr = co_init(NULL);
    if(FAILED(hr)) { FreeLibrary(ole); return RELIC_REG_ERR_OS; }

    hr = co_create(&clsid_shelllink, NULL, VMREG_CLSCTX_INPROC_SERVER,
                   &iid_ishelllinka, (void **)&link);
    if(SUCCEEDED(hr) && link) {
        link->lpVtbl->SetPath(link, target);

        /* Default the working directory to the target's own folder so the
         * program starts where its data files are. */
        {
            char dir[512];
            int i;
            _snprintf(dir, sizeof dir, "%s", target);
            dir[sizeof dir - 1] = 0;
            for(i = (int)strlen(dir) - 1; i >= 0; i--) {
                if(dir[i] == '\\' || dir[i] == '/') { dir[i] = 0; break; }
            }
            if(dir[0]) link->lpVtbl->SetWorkingDirectory(link, dir);
        }

        if(icon_spec && icon_spec[0]) {
            /* Split "path,index"; a missing index means 0. */
            char ipath[512];
            int idx = 0;
            const char *comma = strrchr(icon_spec, ',');
            if(comma) {
                int n = (int)(comma - icon_spec);
                if(n > (int)sizeof ipath - 1) n = (int)sizeof ipath - 1;
                memcpy(ipath, icon_spec, (size_t)n);
                ipath[n] = 0;
                idx = atoi(comma + 1);
            } else {
                _snprintf(ipath, sizeof ipath, "%s", icon_spec);
                ipath[sizeof ipath - 1] = 0;
            }
            link->lpVtbl->SetIconLocation(link, ipath, idx);
        }

        hr = link->lpVtbl->QueryInterface(link, &iid_ipersistfile, (void **)&file);
        if(SUCCEEDED(hr) && file) {
            MultiByteToWideChar(CP_ACP, 0, lnk_path, -1, wpath,
                                (int)(sizeof wpath / sizeof wpath[0]));
            hr = file->lpVtbl->Save(file, wpath, TRUE);
            if(SUCCEEDED(hr)) result = RELIC_REG_OK;
            file->lpVtbl->Release(file);
        }
        link->lpVtbl->Release(link);
    }

    if(co_uninit) co_uninit();
    FreeLibrary(ole);
    return result;
}

/* CSIDL_DESKTOPDIRECTORY = 0x0010 -- the user's Desktop folder (the
 * physical directory, not the virtual CSIDL_DESKTOP namespace root). */
#define RELIC_CSIDL_DESKTOPDIRECTORY 0x0010

/* CSIDL_PROGRAMS = 0x0002 -- the user's Start Menu\Programs folder.
 * On NT this is %APPDATA%\Microsoft\Windows\Start Menu\Programs.
 * On 9x it is %WINDIR%\Start Menu\Programs (or %USERPROFILE%\Start Menu). */
#define RELIC_CSIDL_PROGRAMS 0x0002

/* Resolve a shell special folder to a filesystem path. SHGetFolderPathA
 * (shell32, or SHFOLDER.dll on pre-IE5 9x) is the modern route, but early
 * SHFOLDER versions implement only a subset of CSIDLs - on Win95 without
 * the Desktop Update, CSIDL_DESKTOPDIRECTORY fails while CSIDL_PROGRAMS
 * succeeds. Fall back to Explorer's "Shell Folders" registry values (the
 * shell writes them on every 9x and NT), then to <windir>\<leaf>, which is
 * correct on single-user 9x without profiles. Returns the length or -1. */
static int vmreg_shell_folder(int csidl, const char *reg_value,
                              const char *windir_leaf, char *out, int cap) {
    int len = -1;
    if(g_vmreg.SHGetFolderPathA_ &&
       SUCCEEDED(g_vmreg.SHGetFolderPathA_(NULL, csidl, NULL, 0, out))) {
        out[cap - 1] = 0;
        len = (int)strlen(out);
    }
    if(len <= 0) {
        int n = vmreg_read(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",
            reg_value, out, cap, NULL);
        if(n > 0) len = n;
    }
    if(len <= 0 && windir_leaf && vmreg_is_win9x()) {
        UINT n = GetWindowsDirectoryA(out, (UINT)(cap - 16));
        if(n > 0 && n + strlen(windir_leaf) + 2 < (UINT)cap) {
            if(out[n - 1] != '\\') out[n++] = '\\';
            memcpy(out + n, windir_leaf, strlen(windir_leaf) + 1);
            len = (int)strlen(out);
        }
    }
    return len;
}

static int vmreg_startmenu(const char *target, const char *link_name) {
    char path[MAX_PATH];
    char full[MAX_PATH];
    int len;

    if(vmreg_shell_folder(RELIC_CSIDL_PROGRAMS, "Programs",
                          "Start Menu\\Programs", path, (int)sizeof path) <= 0)
        return RELIC_REG_ERR_UNSUP;
    path[sizeof path - 1] = 0;

    len = (int)strlen(path);
    if(len <= 0) return RELIC_REG_ERR_OS;
    if(path[len - 1] != '\\' && path[len - 1] != '/') {
        path[len] = '\\'; path[len + 1] = 0; len++;
    }
    _snprintf(full, sizeof full, "%s%s", path, link_name);
    full[sizeof full - 1] = 0;
    return vmreg_shortcut(target, full, NULL);
}

/* Delete a .lnk from the user's Start Menu\Programs folder. */
static int vmreg_startmenu_delete(const char *link_name) {
    char path[MAX_PATH];
    char full[MAX_PATH];
    int len;

    if(vmreg_shell_folder(RELIC_CSIDL_PROGRAMS, "Programs",
                          "Start Menu\\Programs", path, (int)sizeof path) <= 0)
        return RELIC_REG_ERR_UNSUP;
    path[sizeof path - 1] = 0;

    len = (int)strlen(path);
    if(len <= 0) return RELIC_REG_ERR_OS;
    if(path[len - 1] != '\\' && path[len - 1] != '/') {
        path[len] = '\\'; path[len + 1] = 0; len++;
    }
    _snprintf(full, sizeof full, "%s%s", path, link_name);
    full[sizeof full - 1] = 0;
    return DeleteFileA(full) ? RELIC_REG_OK : RELIC_REG_ERR_OS;
}

/* CSIDL_LOCAL_APPDATA = 0x001C -- %LOCALAPPDATA% (NT) / a per-user profile
 * folder on 9x via the shfolder.dll shim. This is the standard home for
 * per-user, no-elevation-needed app installs (VS Code, most Electron apps,
 * winget's default scope all use "<this>\Programs\<AppName>"). */
#define RELIC_CSIDL_LOCAL_APPDATA 0x001C

/* Write a DWORD registry value (RegSetValueExA needs the 4 raw bytes, unlike
 * vmreg_write_typed's REG_SZ path which writes strlen(data)+1). */
static int vmreg_write_dword(HKEY root, const char *subkey, const char *name, DWORD value) {
    HKEY hk = NULL;
    LONG rc;
    if(!g_vmreg.RegCreateKeyExA_ || !g_vmreg.RegSetValueExA_ || !g_vmreg.RegCloseKey_)
        return RELIC_REG_ERR_UNSUP;
    rc = g_vmreg.RegCreateKeyExA_(root, subkey, 0, NULL, REG_OPTION_NON_VOLATILE,
                                  KEY_WRITE, NULL, &hk, NULL);
    if(rc != ERROR_SUCCESS) return RELIC_REG_ERR_OS;
    rc = g_vmreg.RegSetValueExA_(hk, name, 0, REG_DWORD, (const BYTE *)&value, sizeof value);
    g_vmreg.RegCloseKey_(hk);
    return (rc == ERROR_SUCCESS) ? RELIC_REG_OK : RELIC_REG_ERR_OS;
}

/* Build "<LOCAL_APPDATA>\Programs\<app_name>". Returns the length written to
 * `out`, or -1 on failure (no SHGetFolderPathA, or the app name/path is too
 * long for `cap`). */
static int vmreg_suggest_path(const char *app_name, char *out, int cap) {
    char base[MAX_PATH];
    int len;
    if(!app_name[0]) return -1;
    vmreg_load();
    if(!g_vmreg.SHGetFolderPathA_) return -1;
    if(FAILED(g_vmreg.SHGetFolderPathA_(NULL, RELIC_CSIDL_LOCAL_APPDATA, NULL, 0, base)))
        return -1;
    base[sizeof base - 1] = 0;
    len = _snprintf(out, (size_t)cap, "%s\\Programs\\%s", base, app_name);
    if(len < 0 || len >= cap) return -1;
    return len;
}

static int vmreg_dispatch(int action, const char *key, const char *value) {
    char subkey[512];
    HKEY root;

    vmreg_load();
    if(!g_vmreg.RegCreateKeyExA_) return RELIC_REG_ERR_UNSUP;

    switch(action) {
    case RELIC_REG_CAPS:
        return (1 << RELIC_REG_CAPS)     | (1 << RELIC_REG_PROGID)
             | (1 << RELIC_REG_ASSOC)    | (1 << RELIC_REG_COMMAND)
             | (1 << RELIC_REG_ICON)     | (1 << RELIC_REG_PATH)
             | (1 << RELIC_REG_SHORTCUT) | (1 << RELIC_REG_APPPATH)
             | (1 << RELIC_REG_UNREGISTER) | (1 << RELIC_REG_COMMIT)
             | (1 << RELIC_REG_PATH_REMOVE)
             | (1 << RELIC_REG_STARTMENU)
             | (1 << RELIC_REG_SUGGEST_PATH) | (1 << RELIC_REG_UNINSTALL)
             | (1 << RELIC_REG_SELFDELETE) | (1 << RELIC_REG_EXE_PATH)
             | (1 << RELIC_REG_DESKTOPDIR)
             | (1 << RELIC_REG_IS_LEGACY_9X)
             | (1 << RELIC_REG_FONTSMOOTHING)
             | (1 << RELIC_REG_SYSTEM_THEME)
             | (1 << RELIC_REG_SYSTEM_COLORS)
             | (1 << RELIC_REG_SYSTEM_FONT)
             | (1 << RELIC_REG_ENV_GET) | (1 << RELIC_REG_ENV_SET)
             | (1 << RELIC_REG_ENV_UNSET) | (1 << RELIC_REG_ENV_COUNT)
             | (1 << RELIC_REG_ENV_NAME);

    case RELIC_REG_IS_LEGACY_9X:
        return vmreg_is_win9x() ? 1 : 0;

    case RELIC_REG_PROGID:
        if(!key[0]) return RELIC_REG_ERR_ARGS;
        root = vmreg_class_key(key, subkey, (int)sizeof subkey);
        return vmreg_write(root, subkey, NULL, value);

    case RELIC_REG_ASSOC:
        /* key must be an extension including the dot, else we would create a
         * bogus top-level class key. */
        if(key[0] != '.' || !value[0]) return RELIC_REG_ERR_ARGS;
        root = vmreg_class_key(key, subkey, (int)sizeof subkey);
        return vmreg_write(root, subkey, NULL, value);

    case RELIC_REG_COMMAND: {
        char leaf[512];
        if(!key[0] || !value[0]) return RELIC_REG_ERR_ARGS;
        _snprintf(leaf, sizeof leaf, "%s\\shell\\open\\command", key);
        leaf[sizeof leaf - 1] = 0;
        root = vmreg_class_key(leaf, subkey, (int)sizeof subkey);
        return vmreg_write(root, subkey, NULL, value);
    }

    case RELIC_REG_ICON: {
        char leaf[512];
        if(!key[0] || !value[0]) return RELIC_REG_ERR_ARGS;
        _snprintf(leaf, sizeof leaf, "%s\\DefaultIcon", key);
        leaf[sizeof leaf - 1] = 0;
        root = vmreg_class_key(leaf, subkey, (int)sizeof subkey);
        return vmreg_write(root, subkey, NULL, value);
    }

    case RELIC_REG_PATH:
        if(!key[0]) return RELIC_REG_ERR_ARGS;
        return vmreg_add_path(key);

    case RELIC_REG_PATH_REMOVE:
        if(!key[0]) return RELIC_REG_ERR_ARGS;
        return vmreg_remove_path(key);

    case RELIC_REG_SHORTCUT:
        if(!key[0] || !value[0]) return RELIC_REG_ERR_ARGS;
        /* NULL icon: the shortcut inherits the target executable's own icon,
         * which is resource 1 (the Tigryx logo) for every Tigryx binary. */
        return vmreg_shortcut(key, value, NULL);

    case RELIC_REG_STARTMENU:
        if(!key[0] || !value[0]) return RELIC_REG_ERR_ARGS;
        return vmreg_startmenu(key, value);

    case RELIC_REG_APPPATH: {
        /* App Paths lets "tigryx" be typed into Run/Start without a full
         * path, independent of PATH. Always HKLM-shaped but we mirror the
         * per-user hive on NT to avoid needing elevation. */
        char leaf[512];
        if(!key[0] || !value[0]) return RELIC_REG_ERR_ARGS;
        _snprintf(leaf, sizeof leaf,
                  "Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%s", key);
        leaf[sizeof leaf - 1] = 0;
        return vmreg_write(vmreg_is_win9x() ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER,
                           leaf, NULL, value);
    }

    case RELIC_REG_UNINSTALL: {
        /* value = DisplayName\nDisplayVersion\nPublisher\nUninstallString\n
         *         DisplayIcon\nInstallLocation (see tigryx.h). Split in place
         * on a local copy since strtok-style splitting is destructive. */
        char fields[1024];
        char *f[6] = {0};
        int nf = 0;
        char *p, *start;
        if(!key[0] || !value[0]) return RELIC_REG_ERR_ARGS;
        _snprintf(fields, sizeof fields, "%s", value);
        fields[sizeof fields - 1] = 0;
        start = fields;
        for(p = fields; nf < 6; p++) {
            if(*p == '\n' || *p == 0) {
                int end = (*p == 0);
                *p = 0;
                f[nf++] = start;
                if(end) break;
                start = p + 1;
            }
        }
        for( ; nf < 6; nf++) f[nf] = "";

        _snprintf(subkey, sizeof subkey,
                  "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\%s", key);
        subkey[sizeof subkey - 1] = 0;
        root = vmreg_is_win9x() ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;

        {
            int all_ok = 1;
            if(f[0][0]) all_ok &= (vmreg_write(root, subkey, "DisplayName", f[0]) == RELIC_REG_OK);
            if(f[1][0]) all_ok &= (vmreg_write(root, subkey, "DisplayVersion", f[1]) == RELIC_REG_OK);
            if(f[2][0]) all_ok &= (vmreg_write(root, subkey, "Publisher", f[2]) == RELIC_REG_OK);
            if(f[3][0]) all_ok &= (vmreg_write(root, subkey, "UninstallString", f[3]) == RELIC_REG_OK);
            if(f[4][0]) all_ok &= (vmreg_write(root, subkey, "DisplayIcon", f[4]) == RELIC_REG_OK);
            if(f[5][0]) all_ok &= (vmreg_write(root, subkey, "InstallLocation", f[5]) == RELIC_REG_OK);
            /* Tigryx installers don't support Modify/Repair, so tell the
             * "Apps & Features" UI not to offer them. */
            vmreg_write_dword(root, subkey, "NoModify", 1);
            vmreg_write_dword(root, subkey, "NoRepair", 1);
            return all_ok ? RELIC_REG_OK : RELIC_REG_ERR_OS;
        }
    }

    case RELIC_REG_UNREGISTER:
        if(!key[0]) return RELIC_REG_ERR_ARGS;
        /* If the key ends in .lnk, treat it as a Start Menu shortcut to delete. */
        {
            int klen = (int)strlen(key);
            if(klen >= 4 && (key[klen-4] == '.') &&
               (key[klen-3] == 'l' || key[klen-3] == 'L') &&
               (key[klen-2] == 'n' || key[klen-2] == 'N') &&
               (key[klen-1] == 'k' || key[klen-1] == 'K')) {
                return vmreg_startmenu_delete(key);
            }
        }
        root = vmreg_class_key(key, subkey, (int)sizeof subkey);
        vmreg_delete_tree(root, subkey);
        /* Best-effort: `key` may also be an app key registered via
         * RELIC_REG_UNINSTALL. Deleting a subkey that was never created (the
         * common case, when `key` really is a ProgID/extension) is a no-op. */
        {
            char uninst_subkey[512];
            _snprintf(uninst_subkey, sizeof uninst_subkey,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\%s", key);
            uninst_subkey[sizeof uninst_subkey - 1] = 0;
            if(g_vmreg.RegDeleteKeyA_)
                g_vmreg.RegDeleteKeyA_(vmreg_is_win9x() ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER,
                                       uninst_subkey);
        }
        return RELIC_REG_OK;

    case RELIC_REG_COMMIT:
        /* Explorer caches associations aggressively; without this the new
         * icons do not appear until the shell restarts. */
        if(g_vmreg.SHChangeNotify_) {
            const LONG SHCNE_ASSOCCHANGED_ = 0x08000000L;
            const UINT SHCNF_IDLIST_       = 0x0000u;
            g_vmreg.SHChangeNotify_(SHCNE_ASSOCCHANGED_, SHCNF_IDLIST_, NULL, NULL);
        }
        return RELIC_REG_OK;

    case RELIC_REG_SELFDELETE: {
        /* key = running uninstaller exe path, value = install directory.
         * Spawn a detached cmd.exe that waits ~2s (ping -n 3), then force-
         * deletes the exe and recursively removes the directory. /f /q on
         * del suppresses prompts and forces deletion; /s /q on rd removes
         * any remaining contents silently. CREATE_NO_WINDOW prevents a
         * console popup. We don't wait -- the caller must exit right after. */
        char cmdline[1024];
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        if(!key[0] || !value[0]) return RELIC_REG_ERR_ARGS;
        if(!g_vmreg.CreateProcessA_) return RELIC_REG_ERR_UNSUP;
        _snprintf(cmdline, sizeof cmdline,
                  "cmd.exe /c ping 127.0.0.1 -n 3 >nul & del /f /q \"%s\" & rd /s /q \"%s\"",
                  key, value);
        cmdline[sizeof cmdline - 1] = 0;
        memset(&si, 0, sizeof si); si.cb = sizeof si;
        memset(&pi, 0, sizeof pi);
        if(!g_vmreg.CreateProcessA_(NULL, cmdline, NULL, NULL, FALSE,
                                    0x08000000 /* CREATE_NO_WINDOW */,
                                    NULL, NULL, &si, &pi))
            return RELIC_REG_ERR_OS;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return RELIC_REG_OK;
    }

    case RELIC_REG_FONTSMOOTHING: {
        /* Enable system-wide grayscale font smoothing. Win95 lacks this API
         * entirely (it was introduced in Win98/NT4 SP3); XP+ defaults to
         * ClearType so the call is harmless but unnecessary there. The change
         * persists across reboots via SPIF_UPDATEINIFILE. */
        const UINT SPI_SETFONTSMOOTHING_ = 0x004Bu;
        const UINT SPIF_UPDATEINIFILE_   = 0x0001u;
        const UINT SPIF_SENDCHANGE_      = 0x0002u;
        if(!SystemParametersInfoA(SPI_SETFONTSMOOTHING_, 1, NULL,
                                  SPIF_UPDATEINIFILE_ | SPIF_SENDCHANGE_))
            return RELIC_REG_ERR_OS;
        return RELIC_REG_OK;
    }

    default:
        return RELIC_REG_ERR_UNSUP;
    }
}
#endif /* _WIN32 */

#ifdef RELIC_DOS
/* ---- DOS --------------------------------------------------------------- *
 * DOS has no registry, no shell, and no file associations. The only useful
 * registration action is adding a directory to the PATH, which on DOS means
 * appending a line to AUTOEXEC.BAT. A reboot is required for the change to
 * take effect, so REG_PATH returns RELIC_REG_OK_REBOOT on success.
 *
 * The boot drive is inferred from the COMSPEC environment variable (e.g.
 * D:\COMMAND.COM means D: is the boot drive), defaulting to C:.
 * ------------------------------------------------------------------------- */

static int vmreg_dos_ci_strncmp(const char *a, const char *b, int n) {
    int i;
    for(i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if(ca >= 'a' && ca <= 'z') ca -= 32;
        if(cb >= 'a' && cb <= 'z') cb -= 32;
        if(ca != cb) return 1;
        if(ca == 0) break;
    }
    return 0;
}

/* Resolve the AUTOEXEC.BAT path on the boot drive. Returns 0 on success.
 *
 * FreeDOS 1.3 RC4 sets up the system to use FDAUTO.BAT instead of AUTOEXEC.BAT
 * (so FreeDOS and MS-DOS can dual-boot from the same drive). Try AUTOEXEC.BAT
 * first — MS-DOS and older FreeDOS installs use it — then fall back to
 * FDAUTO.BAT. */
static int vmreg_dos_autoexec_path(char *out, int cap) {
    const char *comspec = getenv("COMSPEC");
    char drive[3] = "C:";
    const char *names[2] = { "AUTOEXEC.BAT", "FDAUTO.BAT" };
    int i;
    if(comspec && comspec[0] && comspec[1] == ':')
        drive[0] = comspec[0];
    for(i = 0; i < 2; i++) {
        snprintf(out, (size_t)cap, "%s\\%s", drive, names[i]);
        out[cap - 1] = 0;
        {
            FILE *f = fopen(out, "r");
            if(f) { fclose(f); return 0; }
        }
    }
    return -1;
}

/* Check whether dir already appears in a PATH= line within the buffer.
 * Handles "PATH dir", "PATH=dir", "SET PATH=dir" forms (case-insensitive). */
static int vmreg_dos_path_has(const char *buf, int buflen, const char *dir) {
    int i = 0;
    int dlen = (int)strlen(dir);

    while(i < buflen) {
        int line_start = i;
        int pos;

        /* Find end of line */
        while(i < buflen && buf[i] != '\n' && buf[i] != '\r') i++;
        int line_end = i;
        while(i < buflen && (buf[i] == '\n' || buf[i] == '\r')) i++;

        pos = line_start;
        /* Skip leading whitespace */
        while(pos < line_end && (buf[pos] == ' ' || buf[pos] == '\t')) pos++;

        /* Optional "SET " prefix */
        if(pos + 4 <= line_end && vmreg_dos_ci_strncmp(buf + pos, "SET ", 4) == 0) {
            pos += 4;
            while(pos < line_end && (buf[pos] == ' ' || buf[pos] == '\t')) pos++;
        }

        /* Must start with "PATH" */
        if(pos + 4 > line_end || vmreg_dos_ci_strncmp(buf + pos, "PATH", 4) != 0)
            continue;

        /* Char after PATH must be '=', space, tab, or end-of-line */
        {
            int after = pos + 4;
            if(after < line_end && buf[after] != '=' && buf[after] != ' ' && buf[after] != '\t')
                continue;
            pos = after;
        }

        /* Skip '=' and whitespace to reach the first path entry */
        while(pos < line_end && (buf[pos] == '=' || buf[pos] == ' ' || buf[pos] == '\t'))
            pos++;

        /* Scan ;-separated entries */
        while(pos < line_end) {
            int seg_start = pos;
            while(pos < line_end && buf[pos] != ';') pos++;
            int seg_end = pos;
            int seg_len;

            /* Trim trailing whitespace */
            while(seg_end > seg_start && (buf[seg_end-1] == ' ' || buf[seg_end-1] == '\t'))
                seg_end--;

            seg_len = seg_end - seg_start;
            if(seg_len == dlen && vmreg_dos_ci_strncmp(buf + seg_start, dir, dlen) == 0)
                return 1;

            if(pos < line_end && buf[pos] == ';') pos++;
            else break;
        }
    }
    return 0;
}

/* Append "SET PATH=%PATH%;<dir>" to AUTOEXEC.BAT. Returns RELIC_REG_OK if
 * the directory is already on the PATH, or RELIC_REG_OK_REBOOT if the file
 * was modified (a reboot is needed for DOS to pick up the new PATH). */
static int vmreg_dos_add_path(const char *dir) {
    char path[256];
    char buf[8192];
    int len = 0;
    FILE *f;

    if(vmreg_dos_autoexec_path(path, (int)sizeof path) < 0)
        return RELIC_REG_ERR_OS;

    f = fopen(path, "r");
    if(!f) return RELIC_REG_ERR_OS;
    len = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = 0;

    if(vmreg_dos_path_has(buf, len, dir))
        return RELIC_REG_OK;

    f = fopen(path, "a");
    if(!f) return RELIC_REG_ERR_OS;
    /* Ensure previous content ends with a newline */
    if(len > 0 && buf[len - 1] != '\n' && buf[len - 1] != '\r')
        fputs("\r\n", f);
    fprintf(f, "SET PATH=%%PATH%%;%s\r\n", dir);
    fclose(f);

    return RELIC_REG_OK_REBOOT;
}

/* REG_PATH_REMOVE is intentionally NOT supported on DOS. The PATH entry was
 * written by appending "SET PATH=%PATH%;<dir>" to AUTOEXEC.BAT / FDAUTO.BAT,
 * but the user may have since edited that file, moved the install directory,
 * or merged PATH lines in ways our parser can't safely undo. Attempting to
 * rewrite AUTOEXEC.BAT risks corrupting the user's startup file. Instead the
 * uninstaller tells the user to remove the PATH entry by hand. */
static int vmreg_dos_dispatch(int action, const char *key, const char *value) {
    (void)value;
    switch(action) {
    case RELIC_REG_CAPS:
        return (1 << RELIC_REG_PATH) | (1 << RELIC_REG_COMMIT)
             | (1 << RELIC_REG_ENV_GET) | (1 << RELIC_REG_ENV_SET)
             | (1 << RELIC_REG_ENV_UNSET) | (1 << RELIC_REG_ENV_COUNT)
             | (1 << RELIC_REG_ENV_NAME);

    case RELIC_REG_PATH:
        if(!key[0]) return RELIC_REG_ERR_ARGS;
        return vmreg_dos_add_path(key);

    case RELIC_REG_COMMIT:
        /* No shell to notify on DOS; the reboot after REG_PATH suffices. */
        return RELIC_REG_OK;

    case RELIC_REG_IS_LEGACY_9X:
        return 0; /* DOS is handled separately via REG_CAPS detection. */

    default:
        return RELIC_REG_ERR_UNSUP;
    }
}
#endif /* RELIC_DOS */

#ifdef __linux__
/* ---- Linux -------------------------------------------------------------- *
 * Linux has no registry. OS integration uses the XDG Desktop Entry spec:
 *   - File associations: .desktop files in ~/.local/share/applications/
 *     plus MIME type definitions in ~/.local/share/mime/packages/ and
 *     default-application entries in ~/.config/mimeapps.list.
 *   - PATH: append "export PATH=$PATH:<dir>" to ~/.bashrc.
 *   - Shortcuts: .desktop files placed at the requested path.
 *   - App Path: symlink in ~/.local/bin/ (typically on $PATH).
 *
 * The .desktop file for a ProgID accumulates fields across multiple
 * @register calls (PROGID sets Name, COMMAND sets Exec, ICON sets Icon,
 * ASSOC adds MimeType), mirroring how the Windows registry accumulates
 * subkeys.
 * ------------------------------------------------------------------------- */

/* Recursive mkdir -p. Returns 0 on success or if dir already exists. */
static int vmreg_linux_mkdir_p(const char *path) {
    char tmp[512];
    int i, len;
    snprintf(tmp, sizeof tmp, "%s", path);
    tmp[sizeof tmp - 1] = 0;
    len = (int)strlen(tmp);
    if(len == 0) return -1;
    /* Strip trailing / */
    if(tmp[len - 1] == '/') tmp[len - 1] = 0;
    for(i = 1; i < len; i++) {
        if(tmp[i] == '/') {
            tmp[i] = 0;
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    if(mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Build a path under $HOME. fmt should start with "/". */
static int vmreg_linux_home_path(char *out, int cap, const char *fmt) {
    const char *home = getenv("HOME");
    if(!home || !home[0]) return -1;
    snprintf(out, (size_t)cap, "%s%s", home, fmt);
    out[cap - 1] = 0;
    return 0;
}

/* Build the .desktop file path for a ProgID. */
static int vmreg_linux_desktop_path(char *out, int cap, const char *progid) {
    char base[512];
    snprintf(base, sizeof base, "/.local/share/applications/%s.desktop", progid);
    base[sizeof base - 1] = 0;
    return vmreg_linux_home_path(out, cap, base);
}

/* Build the MIME package XML path for a ProgID. */
static int vmreg_linux_mime_xml_path(char *out, int cap, const char *progid) {
    char base[512];
    snprintf(base, sizeof base, "/.local/share/mime/packages/%s.xml", progid);
    base[sizeof base - 1] = 0;
    return vmreg_linux_home_path(out, cap, base);
}

/* Build the mimeapps.list path. */
static int vmreg_linux_mimeapps_path(char *out, int cap) {
    return vmreg_linux_home_path(out, cap, "/.config/mimeapps.list");
}

/* Derive a MIME type name from a ProgID: "Tigryx.TGXFile" -> "application/x-tigryx-rxfile" */
static void vmreg_linux_mime_type(char *out, int cap, const char *progid) {
    int i;
    snprintf(out, (size_t)cap, "application/x-");
    out[cap - 1] = 0;
    for(i = 0; progid[i] && (int)strlen(out) < cap - 1; i++) {
        char c = progid[i];
        if(c == '.') c = '-';
        else if(c >= 'A' && c <= 'Z') c += 32;
        out[strlen(out)] = c;
        out[strlen(out) + 1] = 0;
    }
}

/* Set a field in a .desktop file (read-modify-write).
 * Creates the file with [Desktop Entry] header if it doesn't exist.
 * If the field already exists, replaces its value; otherwise appends it. */
static int vmreg_linux_desktop_set_field(const char *filepath, const char *field, const char *value) {
    char buf[4096];
    int len = 0, found = 0;
    FILE *f;

    /* Ensure parent directory exists */
    {
        char dir[512];
        int i;
        snprintf(dir, sizeof dir, "%s", filepath);
        dir[sizeof dir - 1] = 0;
        for(i = (int)strlen(dir) - 1; i >= 0; i--) {
            if(dir[i] == '/') { dir[i] = 0; break; }
        }
        vmreg_linux_mkdir_p(dir);
    }

    /* Read existing content */
    f = fopen(filepath, "r");
    if(f) {
        len = (int)fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[len] = 0;
    }

    /* If file is empty or has no [Desktop Entry], create from scratch */
    if(len == 0 || !strstr(buf, "[Desktop Entry]")) {
        snprintf(buf, sizeof buf,
                 "[Desktop Entry]\n"
                 "Type=Application\n");
        len = (int)strlen(buf);
    }

    /* Search for the field and replace, or append */
    {
        char newbuf[4096];
        int pos = 0, outpos = 0;
        int flen = (int)strlen(field);

        while(pos < len) {
            int line_start = pos;
            while(pos < len && buf[pos] != '\n') pos++;
            int line_end = pos;
            if(pos < len) pos++; /* skip \n */

            /* Check if this line starts with "field=" */
            if(line_end - line_start > flen && buf[line_start + flen] == '=' &&
               strncmp(buf + line_start, field, flen) == 0) {
                /* Replace this line */
                outpos += snprintf(newbuf + outpos, sizeof newbuf - outpos,
                                   "%s=%s\n", field, value);
                found = 1;
            } else {
                /* Copy this line as-is */
                int copylen = line_end - line_start;
                if(outpos + copylen + 2 < (int)sizeof newbuf) {
                    memcpy(newbuf + outpos, buf + line_start, copylen);
                    outpos += copylen;
                    newbuf[outpos++] = '\n';
                }
            }
        }

        if(!found) {
            outpos += snprintf(newbuf + outpos, sizeof newbuf - outpos,
                               "%s=%s\n", field, value);
        }

        newbuf[outpos] = 0;
        snprintf(buf, sizeof buf, "%s", newbuf);
        len = (int)strlen(buf);
    }

    /* Write back */
    f = fopen(filepath, "w");
    if(!f) return -1;
    fwrite(buf, 1, len, f);
    fclose(f);
    return 0;
}

/* Convert Windows %1 to XDG %f in a command string. */
static void vmreg_linux_fix_exec(char *cmd) {
    char tmp[1024];
    int i, j = 0;
    for(i = 0; cmd[i] && j < (int)sizeof(tmp) - 1; i++) {
        if(cmd[i] == '%' && cmd[i + 1] == '1') {
            tmp[j++] = '%';
            tmp[j++] = 'f';
            i++; /* skip the '1' */
        } else {
            tmp[j++] = cmd[i];
        }
    }
    tmp[j] = 0;
    strcpy(cmd, tmp);
}

/* Append "export PATH=$PATH:<dir>" to ~/.bashrc (idempotent). */
static int vmreg_linux_add_path(const char *dir) {
    char bashrc[512];
    char buf[8192];
    int len = 0;
    char needle[600];
    FILE *f;

    if(vmreg_linux_home_path(bashrc, (int)sizeof bashrc, "/.bashrc") < 0)
        return RELIC_REG_ERR_OS;

    snprintf(needle, sizeof needle, "export PATH=\"$PATH:%s\"", dir);
    needle[sizeof needle - 1] = 0;

    f = fopen(bashrc, "r");
    if(f) {
        len = (int)fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[len] = 0;
        if(strstr(buf, needle))
            return RELIC_REG_OK;
    }

    f = fopen(bashrc, "a");
    if(!f) return RELIC_REG_ERR_OS;
    if(len > 0 && buf[len - 1] != '\n')
        fputc('\n', f);
    fprintf(f, "%s\n", needle);
    fclose(f);

    return RELIC_REG_OK;
}

/* Create a MIME type XML for an extension under a ProgID. */
static int vmreg_linux_write_mime_xml(const char *progid, const char *ext) {
    char path[512];
    char mimetype[256];
    FILE *f;

    if(vmreg_linux_mime_xml_path(path, (int)sizeof path, progid) < 0)
        return -1;

    /* Ensure parent dir exists */
    {
        char dir[512];
        int i;
        snprintf(dir, sizeof dir, "%s", path);
        dir[sizeof dir - 1] = 0;
        for(i = (int)strlen(dir) - 1; i >= 0; i--) {
            if(dir[i] == '/') { dir[i] = 0; break; }
        }
        vmreg_linux_mkdir_p(dir);
    }

    vmreg_linux_mime_type(mimetype, (int)sizeof mimetype, progid);

    f = fopen(path, "w");
    if(!f) return -1;
    fprintf(f,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n"
            "  <mime-type type=\"%s\">\n"
            "    <glob pattern=\"*%s\"/>\n"
            "  </mime-type>\n"
            "</mime-info>\n",
            mimetype, ext);
    fclose(f);
    return 0;
}

/* Add or replace a [Default Applications] entry in mimeapps.list. */
static int vmreg_linux_mimeapps_set(const char *mimetype, const char *desktop_file) {
    char path[512];
    char buf[4096];
    int len = 0;
    FILE *f;
    char section[512];
    int found_section = 0, found_entry = 0;

    if(vmreg_linux_mimeapps_path(path, (int)sizeof path) < 0)
        return -1;

    /* Ensure parent dir */
    {
        char dir[512];
        int i;
        snprintf(dir, sizeof dir, "%s", path);
        dir[sizeof dir - 1] = 0;
        for(i = (int)strlen(dir) - 1; i >= 0; i--) {
            if(dir[i] == '/') { dir[i] = 0; break; }
        }
        vmreg_linux_mkdir_p(dir);
    }

    f = fopen(path, "r");
    if(f) {
        len = (int)fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[len] = 0;
    }

    snprintf(section, sizeof section, "%s=", mimetype);
    section[sizeof section - 1] = 0;

    /* Parse and rebuild */
    {
        char newbuf[4096];
        int pos = 0, outpos = 0;
        int in_default = 0;

        while(pos < len) {
            int line_start = pos;
            while(pos < len && buf[pos] != '\n') pos++;
            int line_end = pos;
            if(pos < len) pos++;

            /* Check for section header */
            if(line_end > line_start && buf[line_start] == '[') {
                int secend = line_start + 1;
                while(secend < line_end && buf[secend] != ']') secend++;
                in_default = (secend - line_start == 25 &&
                              strncmp(buf + line_start, "[Default Applications]", 23) == 0);
            }

            /* Check if this line is our mimetype entry */
            if(in_default && (line_end - line_start > (int)strlen(section)) &&
               strncmp(buf + line_start, section, strlen(section)) == 0) {
                outpos += snprintf(newbuf + outpos, sizeof newbuf - outpos,
                                   "%s%s\n", section, desktop_file);
                found_entry = 1;
                found_section = 1;
            } else {
                int copylen = line_end - line_start;
                if(outpos + copylen + 2 < (int)sizeof newbuf) {
                    memcpy(newbuf + outpos, buf + line_start, copylen);
                    outpos += copylen;
                    newbuf[outpos++] = '\n';
                }
            }
        }

        if(!found_entry) {
            if(!found_section) {
                outpos += snprintf(newbuf + outpos, sizeof newbuf - outpos,
                                   "[Default Applications]\n");
            }
            outpos += snprintf(newbuf + outpos, sizeof newbuf - outpos,
                               "%s%s\n", section, desktop_file);
        }

        newbuf[outpos] = 0;
        snprintf(buf, sizeof buf, "%s", newbuf);
        len = (int)strlen(buf);
    }

    f = fopen(path, "w");
    if(!f) return -1;
    fwrite(buf, 1, len, f);
    fclose(f);
    return 0;
}

/* Remove a mimetype entry from mimeapps.list. */
static int vmreg_linux_mimeapps_remove(const char *mimetype) {
    char path[512];
    char buf[4096];
    int len = 0;
    FILE *f;
    char section[512];

    if(vmreg_linux_mimeapps_path(path, (int)sizeof path) < 0)
        return -1;

    f = fopen(path, "r");
    if(!f) return 0; /* nothing to remove */
    len = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = 0;

    snprintf(section, sizeof section, "%s=", mimetype);
    section[sizeof section - 1] = 0;

    {
        char newbuf[4096];
        int pos = 0, outpos = 0;
        int in_default = 0;

        while(pos < len) {
            int line_start = pos;
            while(pos < len && buf[pos] != '\n') pos++;
            int line_end = pos;
            if(pos < len) pos++;

            if(line_end > line_start && buf[line_start] == '[') {
                int secend = line_start + 1;
                while(secend < line_end && buf[secend] != ']') secend++;
                in_default = (secend - line_start == 25 &&
                              strncmp(buf + line_start, "[Default Applications]", 23) == 0);
            }

            if(in_default && (line_end - line_start > (int)strlen(section)) &&
               strncmp(buf + line_start, section, strlen(section)) == 0) {
                continue; /* skip this line */
            }

            {
                int copylen = line_end - line_start;
                if(outpos + copylen + 2 < (int)sizeof newbuf) {
                    memcpy(newbuf + outpos, buf + line_start, copylen);
                    outpos += copylen;
                    newbuf[outpos++] = '\n';
                }
            }
        }
        newbuf[outpos] = 0;

        f = fopen(path, "w");
        if(!f) return -1;
        fwrite(newbuf, 1, outpos, f);
        fclose(f);
    }
    return 0;
}

/* Create a .desktop shortcut file at the given path. */
static int vmreg_linux_shortcut(const char *target, const char *shortcut_path) {
    char name[256];
    char *base;
    FILE *f;

    /* Ensure parent dir */
    {
        char dir[512];
        int i;
        snprintf(dir, sizeof dir, "%s", shortcut_path);
        dir[sizeof dir - 1] = 0;
        for(i = (int)strlen(dir) - 1; i >= 0; i--) {
            if(dir[i] == '/') { dir[i] = 0; break; }
        }
        vmreg_linux_mkdir_p(dir);
    }

    /* Derive a Name from the target basename */
    base = strrchr(target, '/');
    snprintf(name, sizeof name, "%s", base ? base + 1 : target);
    name[sizeof name - 1] = 0;

    f = fopen(shortcut_path, "w");
    if(!f) return -1;
    fprintf(f,
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=%s\n"
            "Exec=%s\n"
            "Terminal=false\n",
            name, target);
    fclose(f);

    /* Make it executable */
    chmod(shortcut_path, 0755);
    return 0;
}

/* Create a symlink in ~/.local/bin/ for APPPATH. */
static int vmreg_linux_apppath(const char *key, const char *fullpath) {
    char linkpath[512];
    char bin_dir[512];

    /* Strip .exe suffix if present */
    {
        int klen = (int)strlen(key);
        if(klen > 4 && strcmp(key + klen - 4, ".exe") == 0) {
            char tmp[256];
            int nlen = klen - 4;
            if(nlen >= (int)sizeof tmp) nlen = (int)sizeof tmp - 1;
            memcpy(tmp, key, nlen);
            tmp[nlen] = 0;
            snprintf(linkpath, sizeof linkpath, "/.local/bin/%s", tmp);
        } else {
            snprintf(linkpath, sizeof linkpath, "/.local/bin/%s", key);
        }
    }
    linkpath[sizeof linkpath - 1] = 0;

    if(vmreg_linux_home_path(bin_dir, (int)sizeof bin_dir, "/.local/bin") < 0)
        return RELIC_REG_ERR_OS;
    vmreg_linux_mkdir_p(bin_dir);

    if(vmreg_linux_home_path(linkpath, (int)sizeof linkpath, linkpath) < 0)
        return RELIC_REG_ERR_OS;

    /* Remove existing symlink/file first */
    unlink(linkpath);

    if(symlink(fullpath, linkpath) != 0)
        return RELIC_REG_ERR_OS;

    return RELIC_REG_OK;
}

/* Remove a file if it exists. */
static int vmreg_linux_remove_file(const char *path) {
    if(unlink(path) != 0 && errno != ENOENT) return -1;
    return 0;
}

static int vmreg_linux_dispatch(int action, const char *key, const char *value) {
    switch(action) {
    case RELIC_REG_CAPS:
        return (1 << RELIC_REG_CAPS)     | (1 << RELIC_REG_PROGID)
             | (1 << RELIC_REG_ASSOC)    | (1 << RELIC_REG_COMMAND)
             | (1 << RELIC_REG_ICON)     | (1 << RELIC_REG_PATH)
             | (1 << RELIC_REG_SHORTCUT) | (1 << RELIC_REG_APPPATH)
             | (1 << RELIC_REG_UNREGISTER) | (1 << RELIC_REG_COMMIT)
             | (1 << RELIC_REG_ENV_GET) | (1 << RELIC_REG_ENV_SET)
             | (1 << RELIC_REG_ENV_UNSET) | (1 << RELIC_REG_ENV_COUNT)
             | (1 << RELIC_REG_ENV_NAME);

    case RELIC_REG_PROGID: {
        /* Create/update the .desktop file with Name=<value> */
        char path[512];
        if(!key[0]) return RELIC_REG_ERR_ARGS;
        if(vmreg_linux_desktop_path(path, (int)sizeof path, key) < 0)
            return RELIC_REG_ERR_OS;
        if(vmreg_linux_desktop_set_field(path, "Name", value[0] ? value : key) < 0)
            return RELIC_REG_ERR_OS;
        return RELIC_REG_OK;
    }

    case RELIC_REG_ASSOC: {
        /* key=".ext", value=ProgID. Create MIME XML + mimeapps.list entry. */
        char desktop_path[512];
        char mimetype[256];
        char desktop_file[300];
        if(key[0] != '.' || !value[0]) return RELIC_REG_ERR_ARGS;

        vmreg_linux_mime_type(mimetype, (int)sizeof mimetype, value);
        snprintf(desktop_file, sizeof desktop_file, "%s.desktop", value);
        desktop_file[sizeof desktop_file - 1] = 0;

        if(vmreg_linux_write_mime_xml(value, key) < 0)
            return RELIC_REG_ERR_OS;

        if(vmreg_linux_mimeapps_set(mimetype, desktop_file) < 0)
            return RELIC_REG_ERR_OS;

        /* Also add MimeType to the .desktop file */
        if(vmreg_linux_desktop_path(desktop_path, (int)sizeof desktop_path, value) < 0)
            return RELIC_REG_ERR_OS;
        vmreg_linux_desktop_set_field(desktop_path, "MimeType", mimetype);

        return RELIC_REG_OK;
    }

    case RELIC_REG_COMMAND: {
        /* key=ProgID, value=command line. Set Exec= in .desktop file. */
        char path[512];
        char exec[1024];
        if(!key[0] || !value[0]) return RELIC_REG_ERR_ARGS;
        if(vmreg_linux_desktop_path(path, (int)sizeof path, key) < 0)
            return RELIC_REG_ERR_OS;
        snprintf(exec, sizeof exec, "%s", value);
        exec[sizeof exec - 1] = 0;
        vmreg_linux_fix_exec(exec);
        if(vmreg_linux_desktop_set_field(path, "Exec", exec) < 0)
            return RELIC_REG_ERR_OS;
        return RELIC_REG_OK;
    }

    case RELIC_REG_ICON: {
        /* key=ProgID, value=icon path or name. Set Icon= in .desktop file. */
        char path[512];
        if(!key[0] || !value[0]) return RELIC_REG_ERR_ARGS;
        if(vmreg_linux_desktop_path(path, (int)sizeof path, key) < 0)
            return RELIC_REG_ERR_OS;
        if(vmreg_linux_desktop_set_field(path, "Icon", value) < 0)
            return RELIC_REG_ERR_OS;
        return RELIC_REG_OK;
    }

    case RELIC_REG_PATH:
        if(!key[0]) return RELIC_REG_ERR_ARGS;
        return vmreg_linux_add_path(key);

    case RELIC_REG_SHORTCUT:
        if(!key[0] || !value[0]) return RELIC_REG_ERR_ARGS;
        if(vmreg_linux_shortcut(key, value) < 0)
            return RELIC_REG_ERR_OS;
        return RELIC_REG_OK;

    case RELIC_REG_APPPATH:
        if(!key[0] || !value[0]) return RELIC_REG_ERR_ARGS;
        return vmreg_linux_apppath(key, value);

    case RELIC_REG_UNREGISTER: {
        if(!key[0]) return RELIC_REG_ERR_ARGS;
        if(key[0] == '.') {
            /* Extension: find and remove MIME XML with matching glob, clean mimeapps */
            char mime_dir[512];
            DIR *d;
            struct dirent *ent;
            if(vmreg_linux_home_path(mime_dir, (int)sizeof mime_dir,
                                     "/.local/share/mime/packages") < 0)
                return RELIC_REG_ERR_OS;
            d = opendir(mime_dir);
            if(d) {
                while((ent = readdir(d)) != NULL) {
                    int nlen = (int)strlen(ent->d_name);
                    if(nlen < 5 || strcmp(ent->d_name + nlen - 4, ".xml") != 0) continue;
                    {
                        char fpath[600];
                        char fbuf[4096];
                        int flen;
                        char glob_pattern[256];
                        FILE *mf;
                        snprintf(fpath, sizeof fpath, "%s/%s", mime_dir, ent->d_name);
                        fpath[sizeof fpath - 1] = 0;
                        snprintf(glob_pattern, sizeof glob_pattern, "pattern=\"*%s\"", key);
                        glob_pattern[sizeof glob_pattern - 1] = 0;
                        mf = fopen(fpath, "r");
                        if(!mf) continue;
                        flen = (int)fread(fbuf, 1, sizeof(fbuf) - 1, mf);
                        fclose(mf);
                        fbuf[flen] = 0;
                        if(strstr(fbuf, glob_pattern)) {
                            /* Extract MIME type from this XML and clean mimeapps */
                            char *mt_start = strstr(fbuf, "type=\"");
                            if(mt_start) {
                                mt_start += 6;
                                char *mt_end = strchr(mt_start, '"');
                                if(mt_end) {
                                    char mimetype[256];
                                    int mlen = mt_end - mt_start;
                                    if(mlen >= (int)sizeof mimetype) mlen = (int)sizeof mimetype - 1;
                                    memcpy(mimetype, mt_start, mlen);
                                    mimetype[mlen] = 0;
                                    vmreg_linux_mimeapps_remove(mimetype);
                                }
                            }
                            vmreg_linux_remove_file(fpath);
                        }
                    }
                }
                closedir(d);
            }
        } else {
            /* ProgID: remove .desktop file, MIME XML, mimeapps entry */
            char path[512];
            char mimetype[256];
            char desktop_file[300];

            vmreg_linux_mime_type(mimetype, (int)sizeof mimetype, key);
            snprintf(desktop_file, sizeof desktop_file, "%s.desktop", key);
            desktop_file[sizeof desktop_file - 1] = 0;

            vmreg_linux_mimeapps_remove(mimetype);

            if(vmreg_linux_mime_xml_path(path, (int)sizeof path, key) == 0)
                vmreg_linux_remove_file(path);

            if(vmreg_linux_desktop_path(path, (int)sizeof path, key) == 0)
                vmreg_linux_remove_file(path);
        }
        return RELIC_REG_OK;
    }

    case RELIC_REG_COMMIT: {
        /* Best-effort: update desktop and MIME databases. */
        char cmd[600];
        char app_dir[512], mime_dir[512];
        if(vmreg_linux_home_path(app_dir, (int)sizeof app_dir,
                                 "/.local/share/applications") == 0) {
            snprintf(cmd, sizeof cmd, "update-desktop-database %s 2>/dev/null", app_dir);
            system(cmd);
        }
        if(vmreg_linux_home_path(mime_dir, (int)sizeof mime_dir,
                                 "/.local/share/mime") == 0) {
            snprintf(cmd, sizeof cmd, "update-mime-database %s 2>/dev/null", mime_dir);
            system(cmd);
        }
        return RELIC_REG_OK;
    }

    case RELIC_REG_IS_LEGACY_9X:
        return 0; /* Linux is never Windows 9x. */

    default:
        return RELIC_REG_ERR_UNSUP;
    }
}
#endif /* __linux__ */

#if defined(__APPLE__) && defined(__MACH__) && !TARGET_OS_IPHONE
/* ---- macOS -------------------------------------------------------------- *
 * macOS uses Launch Services for file associations. Applications are .app
 * bundles containing an Info.plist that declares UTI types, icons, and the
 * executable. We create a minimal .app bundle in ~/Applications/ for each
 * ProgID, accumulating fields across @register calls (PROGID sets
 * CFBundleName, COMMAND sets CFBundleExecutable, ICON sets CFBundleIconFile,
 * ASSOC adds CFBundleTypeExtensions). REG_COMMIT runs lsregister to tell
 * Launch Services about the bundle.
 *
 * PATH: append "export PATH=$PATH:<dir>" to ~/.zprofile (the default shell
 * since macOS Catalina is zsh, and ~/.zprofile runs for login shells).
 * SHORTCUT: creates a .app bundle at the requested path.
 * APPPATH: symlink in /usr/local/bin.
 * ------------------------------------------------------------------------- */

/* Recursive mkdir -p. Returns 0 on success or if dir already exists. */
static int vmreg_mac_mkdir_p(const char *path) {
    char tmp[512];
    int i, len;
    snprintf(tmp, sizeof tmp, "%s", path);
    tmp[sizeof tmp - 1] = 0;
    len = (int)strlen(tmp);
    if(len == 0) return -1;
    if(tmp[len - 1] == '/') tmp[len - 1] = 0;
    for(i = 1; i < len; i++) {
        if(tmp[i] == '/') {
            tmp[i] = 0;
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    if(mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Build a path under $HOME. fmt should start with "/". */
static int vmreg_mac_home_path(char *out, int cap, const char *fmt) {
    const char *home = getenv("HOME");
    if(!home || !home[0]) return -1;
    snprintf(out, (size_t)cap, "%s%s", home, fmt);
    out[cap - 1] = 0;
    return 0;
}

/* Build the .app bundle path for a ProgID in ~/Applications/.
 * e.g. "Tigryx.TGXFile" -> ~/Applications/Tigryx.TGXFile.app */
static int vmreg_mac_bundle_path(char *out, int cap, const char *progid) {
    char base[512];
    snprintf(base, sizeof base, "/Applications/%s.app", progid);
    base[sizeof base - 1] = 0;
    return vmreg_mac_home_path(out, cap, base);
}

/* Build the Info.plist path inside a bundle. */
static int vmreg_mac_plist_path(char *out, int cap, const char *bundle_path) {
    snprintf(out, (size_t)cap, "%s/Contents/Info.plist", bundle_path);
    out[cap - 1] = 0;
    return 0;
}

/* Derive a UTI from a ProgID: "Tigryx.TGXFile" -> "com.tigryx.tgxfile" */
static void vmreg_mac_uti(char *out, int cap, const char *progid) {
    int i;
    snprintf(out, (size_t)cap, "com.");
    out[cap - 1] = 0;
    for(i = 0; progid[i] && (int)strlen(out) < cap - 1; i++) {
        char c = progid[i];
        if(c == '.') c = '.';
        else if(c >= 'A' && c <= 'Z') c += 32;
        out[strlen(out)] = c;
        out[strlen(out) + 1] = 0;
    }
}

/* Ensure the Contents directory exists inside a bundle. */
static void vmreg_mac_ensure_bundle(const char *bundle_path) {
    char contents[600];
    snprintf(contents, sizeof contents, "%s/Contents", bundle_path);
    contents[sizeof contents - 1] = 0;
    vmreg_mac_mkdir_p(contents);
}

/* Set a key in an Info.plist file (read-modify-write).
 * Creates a minimal plist if it doesn't exist. If the key already exists,
 * replaces its value; otherwise inserts it before </dict>. */
static int vmreg_mac_plist_set(const char *plist_path, const char *key, const char *value) {
    char buf[8192];
    int len = 0, found = 0;
    FILE *f;

    /* Read existing content */
    f = fopen(plist_path, "r");
    if(f) {
        len = (int)fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[len] = 0;
    }

    /* If file is empty or has no <plist, create from scratch */
    if(len == 0 || !strstr(buf, "<plist")) {
        snprintf(buf, sizeof buf,
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                 "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                 "<plist version=\"1.0\">\n"
                 "<dict>\n"
                 "    <key>CFBundlePackageType</key>\n"
                 "    <string>APPL</string>\n"
                 "</dict>\n"
                 "</plist>\n");
        len = (int)strlen(buf);
    }

    /* Build the key+value block to search for / insert */
    {
        char oldblock[1024];
        char newblock[1024];
        char *pos;

        snprintf(oldblock, sizeof oldblock,
                 "    <key>%s</key>\n    <string>", key);
        oldblock[sizeof oldblock - 1] = 0;

        snprintf(newblock, sizeof newblock,
                 "    <key>%s</key>\n    <string>%s</string>\n", key, value);
        newblock[sizeof newblock - 1] = 0;

        /* Search for existing key and replace its value */
        pos = strstr(buf, oldblock);
        if(pos) {
            char *val_start = pos + strlen(oldblock);
            char *val_end = strstr(val_start, "</string>");
            if(val_end) {
                char newbuf[8192];
                int prefix_len = (int)(pos - buf);
                /* Reconstruct: prefix + newblock + suffix (after </string>\n) */
                memcpy(newbuf, buf, prefix_len);
                newbuf[prefix_len] = 0;
                strcat(newbuf, newblock);
                /* Find what's after </string> (skip the newline if present) */
                {
                    char *after = val_end + strlen("</string>");
                    if(*after == '\n') after++;
                    strcat(newbuf, after);
                }
                snprintf(buf, sizeof buf, "%s", newbuf);
                len = (int)strlen(buf);
                found = 1;
            }
        }

        if(!found) {
            /* Insert before </dict> */
            char newbuf[8192];
            char *dict_end = strstr(buf, "</dict>");
            if(dict_end) {
                int prefix_len = (int)(dict_end - buf);
                memcpy(newbuf, buf, prefix_len);
                newbuf[prefix_len] = 0;
                strcat(newbuf, newblock);
                strcat(newbuf, dict_end);
                snprintf(buf, sizeof buf, "%s", newbuf);
                len = (int)strlen(buf);
            }
        }
    }

    /* Write back */
    f = fopen(plist_path, "w");
    if(!f) return -1;
    fwrite(buf, 1, len, f);
    fclose(f);
    return 0;
}

/* Add a CFBundleTypeExtension entry to the plist.
 * This adds a new dict to CFBundleDocumentTypes array. */
static int vmreg_mac_plist_add_type(const char *plist_path, const char *ext,
                                     const char *uti) {
    char buf[8192];
    int len = 0;
    FILE *f;
    char needle[256];

    /* Read existing */
    f = fopen(plist_path, "r");
    if(!f) return -1;
    len = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = 0;

    /* Check if this extension is already registered */
    snprintf(needle, sizeof needle, "<string>%s</string>", ext + 1);
    needle[sizeof needle - 1] = 0;
    if(strstr(buf, needle)) return 0; /* already there */

    /* Check if CFBundleDocumentTypes key exists */
    if(!strstr(buf, "CFBundleDocumentTypes")) {
        /* Insert the key + array before </dict> */
        char newbuf[8192];
        char *dict_end = strstr(buf, "</dict>");
        if(!dict_end) return -1;
        int prefix_len = (int)(dict_end - buf);
        char block[1024];
        snprintf(block, sizeof block,
                 "    <key>CFBundleDocumentTypes</key>\n"
                 "    <array>\n"
                 "        <dict>\n"
                 "            <key>CFBundleTypeName</key>\n"
                 "            <string>%s</string>\n"
                 "            <key>CFBundleTypeExtensions</key>\n"
                 "            <array>\n"
                 "                <string>%s</string>\n"
                 "            </array>\n"
                 "            <key>LSItemContentTypes</key>\n"
                 "            <array>\n"
                 "                <string>%s</string>\n"
                 "            </array>\n"
                 "            <key>CFBundleTypeRole</key>\n"
                 "            <string>Viewer</string>\n"
                 "        </dict>\n"
                 "    </array>\n",
                 uti, ext + 1, uti);
        block[sizeof block - 1] = 0;
        memcpy(newbuf, buf, prefix_len);
        newbuf[prefix_len] = 0;
        strcat(newbuf, block);
        strcat(newbuf, dict_end);
        snprintf(buf, sizeof buf, "%s", newbuf);
    } else {
        /* Insert a new <dict> before </array> that closes CFBundleDocumentTypes.
         * We find the first </array> after CFBundleDocumentTypes. */
        char *key_pos = strstr(buf, "CFBundleDocumentTypes");
        if(!key_pos) return -1;
        char *array_end = strstr(key_pos, "</array>");
        if(!array_end) return -1;
        char newbuf[8192];
        int prefix_len = (int)(array_end - buf);
        char block[512];
        snprintf(block, sizeof block,
                 "        <dict>\n"
                 "            <key>CFBundleTypeName</key>\n"
                 "            <string>%s</string>\n"
                 "            <key>CFBundleTypeExtensions</key>\n"
                 "            <array>\n"
                 "                <string>%s</string>\n"
                 "            </array>\n"
                 "            <key>LSItemContentTypes</key>\n"
                 "            <array>\n"
                 "                <string>%s</string>\n"
                 "            </array>\n"
                 "            <key>CFBundleTypeRole</key>\n"
                 "            <string>Viewer</string>\n"
                 "        </dict>\n",
                 uti, ext + 1, uti);
        block[sizeof block - 1] = 0;
        memcpy(newbuf, buf, prefix_len);
        newbuf[prefix_len] = 0;
        strcat(newbuf, block);
        strcat(newbuf, array_end);
        snprintf(buf, sizeof buf, "%s", newbuf);
    }
    len = (int)strlen(buf);

    f = fopen(plist_path, "w");
    if(!f) return -1;
    fwrite(buf, 1, len, f);
    fclose(f);
    return 0;
}

/* Append "export PATH=$PATH:<dir>" to ~/.zprofile (idempotent). */
static int vmreg_mac_add_path(const char *dir) {
    char zprofile[512];
    char buf[8192];
    int len = 0;
    char needle[600];
    FILE *f;

    if(vmreg_mac_home_path(zprofile, (int)sizeof zprofile, "/.zprofile") < 0)
        return RELIC_REG_ERR_OS;

    snprintf(needle, sizeof needle, "export PATH=\"$PATH:%s\"", dir);
    needle[sizeof needle - 1] = 0;

    f = fopen(zprofile, "r");
    if(f) {
        len = (int)fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[len] = 0;
        if(strstr(buf, needle))
            return RELIC_REG_OK;
    }

    f = fopen(zprofile, "a");
    if(!f) return RELIC_REG_ERR_OS;
    if(len > 0 && buf[len - 1] != '\n')
        fputc('\n', f);
    fprintf(f, "%s\n", needle);
    fclose(f);

    return RELIC_REG_OK;
}

/* Create a minimal .app bundle at the given path. */
static int vmreg_mac_create_bundle(const char *bundle_path, const char *target) {
    char contents[600];
    char plist[700];
    char name[256];
    char *base;
    FILE *f;

    snprintf(contents, sizeof contents, "%s/Contents", bundle_path);
    contents[sizeof contents - 1] = 0;
    vmreg_mac_mkdir_p(contents);

    vmreg_mac_plist_path(plist, (int)sizeof plist, bundle_path);

    /* Derive a Name from the target basename */
    base = strrchr(target, '/');
    snprintf(name, sizeof name, "%s", base ? base + 1 : target);
    name[sizeof name - 1] = 0;

    /* Write minimal Info.plist */
    f = fopen(plist, "w");
    if(!f) return -1;
    fprintf(f,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n"
            "<dict>\n"
            "    <key>CFBundlePackageType</key>\n"
            "    <string>APPL</string>\n"
            "    <key>CFBundleName</key>\n"
            "    <string>%s</string>\n"
            "    <key>CFBundleExecutable</key>\n"
            "    <string>%s</string>\n"
            "</dict>\n"
            "</plist>\n",
            name, target);
    fclose(f);
    return 0;
}

/* Create a symlink in /usr/local/bin for APPPATH. */
static int vmreg_mac_apppath(const char *key, const char *fullpath) {
    char linkpath[512];

    /* Strip .exe suffix if present */
    {
        int klen = (int)strlen(key);
        if(klen > 4 && strcmp(key + klen - 4, ".exe") == 0) {
            char tmp[256];
            int nlen = klen - 4;
            if(nlen >= (int)sizeof tmp) nlen = (int)sizeof tmp - 1;
            memcpy(tmp, key, nlen);
            tmp[nlen] = 0;
            snprintf(linkpath, sizeof linkpath, "/usr/local/bin/%s", tmp);
        } else {
            snprintf(linkpath, sizeof linkpath, "/usr/local/bin/%s", key);
        }
    }
    linkpath[sizeof linkpath - 1] = 0;

    /* Ensure /usr/local/bin exists */
    vmreg_mac_mkdir_p("/usr/local/bin");

    /* Remove existing symlink/file first */
    unlink(linkpath);

    if(symlink(fullpath, linkpath) != 0)
        return RELIC_REG_ERR_OS;

    return RELIC_REG_OK;
}

/* Remove a directory tree (rm -rf). */
static int vmreg_mac_rmrf(const char *path) {
    DIR *d = opendir(path);
    struct dirent *ent;
    char child[600];

    if(d) {
        while((ent = readdir(d)) != NULL) {
            if(strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            snprintf(child, sizeof child, "%s/%s", path, ent->d_name);
            child[sizeof child - 1] = 0;
            vmreg_mac_rmrf(child);
        }
        closedir(d);
    }
    return rmdir(path);
}

static int vmreg_mac_dispatch(int action, const char *key, const char *value) {
    switch(action) {
    case RELIC_REG_CAPS:
        return (1 << RELIC_REG_CAPS)     | (1 << RELIC_REG_PROGID)
             | (1 << RELIC_REG_ASSOC)    | (1 << RELIC_REG_COMMAND)
             | (1 << RELIC_REG_ICON)     | (1 << RELIC_REG_PATH)
             | (1 << RELIC_REG_SHORTCUT) | (1 << RELIC_REG_APPPATH)
             | (1 << RELIC_REG_UNREGISTER) | (1 << RELIC_REG_COMMIT)
             | (1 << RELIC_REG_SYSTEM_THEME)
             | (1 << RELIC_REG_ENV_GET) | (1 << RELIC_REG_ENV_SET)
             | (1 << RELIC_REG_ENV_UNSET) | (1 << RELIC_REG_ENV_COUNT)
             | (1 << RELIC_REG_ENV_NAME);

    case RELIC_REG_PROGID: {
        /* Create/update the .app bundle's Info.plist with CFBundleName */
        char bundle[512], plist[600];
        if(!key[0]) return RELIC_REG_ERR_ARGS;
        if(vmreg_mac_bundle_path(bundle, (int)sizeof bundle, key) < 0)
            return RELIC_REG_ERR_OS;
        vmreg_mac_ensure_bundle(bundle);
        vmreg_mac_plist_path(plist, (int)sizeof plist, bundle);
        if(vmreg_mac_plist_set(plist, "CFBundleName",
                               value[0] ? value : key) < 0)
            return RELIC_REG_ERR_OS;
        return RELIC_REG_OK;
    }

    case RELIC_REG_ASSOC: {
        /* key=".ext", value=ProgID. Add CFBundleTypeExtensions entry. */
        char bundle[512], plist[600];
        char uti[256];
        if(key[0] != '.' || !value[0]) return RELIC_REG_ERR_ARGS;

        vmreg_mac_uti(uti, (int)sizeof uti, value);

        if(vmreg_mac_bundle_path(bundle, (int)sizeof bundle, value) < 0)
            return RELIC_REG_ERR_OS;
        vmreg_mac_ensure_bundle(bundle);
        vmreg_mac_plist_path(plist, (int)sizeof plist, bundle);

        if(vmreg_mac_plist_add_type(plist, key, uti) < 0)
            return RELIC_REG_ERR_OS;
        return RELIC_REG_OK;
    }

    case RELIC_REG_COMMAND: {
        /* key=ProgID, value=executable path. Set CFBundleExecutable. */
        char bundle[512], plist[600];
        if(!key[0] || !value[0]) return RELIC_REG_ERR_ARGS;
        if(vmreg_mac_bundle_path(bundle, (int)sizeof bundle, key) < 0)
            return RELIC_REG_ERR_OS;
        vmreg_mac_ensure_bundle(bundle);
        vmreg_mac_plist_path(plist, (int)sizeof plist, bundle);
        if(vmreg_mac_plist_set(plist, "CFBundleExecutable", value) < 0)
            return RELIC_REG_ERR_OS;
        return RELIC_REG_OK;
    }

    case RELIC_REG_ICON: {
        /* key=ProgID, value=icon file path. Set CFBundleIconFile. */
        char bundle[512], plist[600];
        if(!key[0] || !value[0]) return RELIC_REG_ERR_ARGS;
        if(vmreg_mac_bundle_path(bundle, (int)sizeof bundle, key) < 0)
            return RELIC_REG_ERR_OS;
        vmreg_mac_ensure_bundle(bundle);
        vmreg_mac_plist_path(plist, (int)sizeof plist, bundle);
        if(vmreg_mac_plist_set(plist, "CFBundleIconFile", value) < 0)
            return RELIC_REG_ERR_OS;
        return RELIC_REG_OK;
    }

    case RELIC_REG_PATH:
        if(!key[0]) return RELIC_REG_ERR_ARGS;
        return vmreg_mac_add_path(key);

    case RELIC_REG_SHORTCUT: {
        /* key=target exe path, value=bundle path. Create .app bundle. */
        if(!key[0] || !value[0]) return RELIC_REG_ERR_ARGS;
        if(vmreg_mac_create_bundle(value, key) < 0)
            return RELIC_REG_ERR_OS;
        return RELIC_REG_OK;
    }

    case RELIC_REG_APPPATH:
        if(!key[0] || !value[0]) return RELIC_REG_ERR_ARGS;
        return vmreg_mac_apppath(key, value);

    case RELIC_REG_UNREGISTER: {
        if(!key[0]) return RELIC_REG_ERR_ARGS;
        if(key[0] == '.') {
            /* Extension: find the bundle that claims this extension and
             * remove it. We scan ~/Applications/<app>.app/Contents/Info.plist
             * for the extension string. */
            char apps_dir[512];
            DIR *d;
            struct dirent *ent;
            if(vmreg_mac_home_path(apps_dir, (int)sizeof apps_dir,
                                   "/Applications") < 0)
                return RELIC_REG_ERR_OS;
            d = opendir(apps_dir);
            if(d) {
                while((ent = readdir(d)) != NULL) {
                    int nlen = (int)strlen(ent->d_name);
                    if(nlen < 5 || strcmp(ent->d_name + nlen - 4, ".app") != 0)
                        continue;
                    {
                        char bpath[600];
                        char ppath[700];
                        char fbuf[8192];
                        int flen;
                        char needle[256];
                        FILE *pf;
                        snprintf(bpath, sizeof bpath, "%s/%s", apps_dir, ent->d_name);
                        bpath[sizeof bpath - 1] = 0;
                        snprintf(ppath, sizeof ppath, "%s/Contents/Info.plist", bpath);
                        ppath[sizeof ppath - 1] = 0;
                        snprintf(needle, sizeof needle, "<string>%s</string>", key + 1);
                        needle[sizeof needle - 1] = 0;
                        pf = fopen(ppath, "r");
                        if(!pf) continue;
                        flen = (int)fread(fbuf, 1, sizeof(fbuf) - 1, pf);
                        fclose(pf);
                        fbuf[flen] = 0;
                        if(strstr(fbuf, needle)) {
                            char cmd[700];
                            vmreg_mac_rmrf(bpath);
                            snprintf(cmd, sizeof cmd,
                                     "lsregister -u %s 2>/dev/null", bpath);
                            cmd[sizeof cmd - 1] = 0;
                            system(cmd);
                        }
                    }
                }
                closedir(d);
            }
        } else {
            /* ProgID: remove the .app bundle and unregister with Launch Services */
            char bundle[512];
            char cmd[700];
            if(vmreg_mac_bundle_path(bundle, (int)sizeof bundle, key) < 0)
                return RELIC_REG_ERR_OS;
            vmreg_mac_rmrf(bundle);
            snprintf(cmd, sizeof cmd, "lsregister -u %s 2>/dev/null", bundle);
            cmd[sizeof cmd - 1] = 0;
            system(cmd);
        }
        return RELIC_REG_OK;
    }

    case RELIC_REG_COMMIT: {
        /* Run lsregister on ~/Applications to pick up all bundles. */
        char apps_dir[512];
        char cmd[600];
        if(vmreg_mac_home_path(apps_dir, (int)sizeof apps_dir, "/Applications") == 0) {
            snprintf(cmd, sizeof cmd,
                     "lsregister %.511s/*.app 2>/dev/null", apps_dir);
            cmd[sizeof cmd - 1] = 0;
            system(cmd);
        }
        return RELIC_REG_OK;
    }

    case RELIC_REG_IS_LEGACY_9X:
        return 0; /* macOS is never Windows 9x. */

    default:
        return RELIC_REG_ERR_UNSUP;
    }
}
#endif /* __APPLE__ && __MACH__ && !TARGET_OS_IPHONE */

/* ---- process environment (RELIC_REG_ENV_*) ------------------------------
 * The C runtime's getenv/setenv/unsetenv/environ is uniform across the
 * platforms that have one, so these live beside the op rather than in the
 * per-OS vmreg_*_dispatch() tables. Children spawned by @process_spawn
 * inherit the process env block, so SET reaches them with no API change.
 * Platforms with no env notion return RELIC_REG_ERR_UNSUP. */

#if defined(_WIN32) || defined(RELIC_DOS) || defined(__linux__) || \
    (defined(__APPLE__) && defined(__MACH__)) || defined(__sun) || \
    defined(__EMSCRIPTEN__)
#define RELIC_HAVE_ENV 1
#else
#define RELIC_HAVE_ENV 0
#endif

#if RELIC_HAVE_ENV && !defined(_WIN32)
extern char **environ;
#endif
#ifdef __EMSCRIPTEN__
/* Emscripten's musl does implement setenv/unsetenv (JS-backed ENV), but
 * -std=c11 sets __STRICT_ANSI__ which hides their declarations. */
int setenv(const char *, const char *, int);
int unsetenv(const char *);
#endif

#if RELIC_HAVE_ENV
static int relic_env_set(const char *name, const char *value) {
    if(!name[0] || strchr(name, '=')) return RELIC_REG_ERR_ARGS;
#ifdef _WIN32
    /* _putenv updates the CRT copy that getenv() reads AND the Win32
     * environment block children inherit. The formatted "NAME=VALUE"
     * string intentionally leaks: putenv() stores the pointer itself. */
    {
        size_t n = strlen(name) + strlen(value) + 2;
        char *kv = (char *)malloc(n);
        if(!kv) return RELIC_REG_ERR_OS;
        _snprintf(kv, n, "%s=%s", name, value);
        kv[n - 1] = 0;
        if(_putenv(kv) != 0) { free(kv); return RELIC_REG_ERR_OS; }
        return RELIC_REG_OK;
    }
#else
    return setenv(name, value, 1) == 0 ? RELIC_REG_OK : RELIC_REG_ERR_OS;
#endif
}

static int relic_env_unset(const char *name) {
    if(!name[0]) return RELIC_REG_ERR_ARGS;
#ifdef _WIN32
    /* Both: SetEnvironmentVariable(name,NULL) clears the Win32 block,
     * _putenv("NAME=") clears the CRT copy getenv() reads. */
    SetEnvironmentVariableA(name, NULL);
    {
        size_t n = strlen(name) + 2;
        char *kv = (char *)malloc(n);
        if(kv) {
            _snprintf(kv, n, "%s=", name);
            kv[n - 1] = 0;
            _putenv(kv);
        }
    }
    return RELIC_REG_OK;
#else
    unsetenv(name);
    return RELIC_REG_OK;
#endif
}

static int relic_env_count(void) {
#ifdef _WIN32
    char *blk = GetEnvironmentStringsA();
    const char *p;
    int n = 0;
    if(!blk) return 0;
    p = blk;
    while(*p) {
        int l = (int)strlen(p);
        /* Skip the hidden "=C:" drive-cwd records and malformed lines. */
        if(l > 0 && p[0] != '=' && strchr(p, '=')) n++;
        p += l + 1;
    }
    FreeEnvironmentStringsA(blk);
    return n;
#else
    int n = 0, v = 0;
    if(!environ) return 0;
    while(environ[n]) { if(strchr(environ[n], '=')) v++; n++; }
    return v;
#endif
}

/* Write the idx-th "NAME=VALUE" entry into out; returns its length or a
 * negative RELIC_REG_ERR_* code. */
static int relic_env_entry(int idx, char *out, int cap) {
#ifdef _WIN32
    char *blk = GetEnvironmentStringsA();
    const char *p;
    int n = 0;
    if(!blk) return RELIC_REG_ERR_OS;
    p = blk;
    while(*p) {
        int l = (int)strlen(p);
        if(l > 0 && p[0] != '=' && strchr(p, '=')) {
            if(n == idx) {
                int c = l < cap - 1 ? l : cap - 1;
                memcpy(out, p, (size_t)c);
                out[c] = 0;
                FreeEnvironmentStringsA(blk);
                return c;
            }
            n++;
        }
        p += l + 1;
    }
    FreeEnvironmentStringsA(blk);
    return RELIC_REG_ERR_ARGS;   /* out of range */
#else
    int n = 0;
    if(idx < 0 || !environ) return RELIC_REG_ERR_ARGS;
    while(environ[n]) {
        const char *e = environ[n];
        if(strchr(e, '=')) {
            if(idx == 0) {
                int l = (int)strlen(e);
                int c = l < cap - 1 ? l : cap - 1;
                memcpy(out, e, (size_t)c);
                out[c] = 0;
                return c;
            }
            idx--;
        }
        n++;
    }
    return RELIC_REG_ERR_ARGS;
#endif
}
#endif /* RELIC_HAVE_ENV */

RELIC_OP_EXPORT void relic_op_sys_register(RelicVM *vm) {
    if(vm->backend && vm->backend->sys_register) { vm->backend->sys_register(vm); return; }

    {
        int action = (int)vm->r[1];
        u32 key_ptr = (u32)vm->r[2];
        char key[512], value[1024];

        /* Window-chrome actions route to the backend's win_ctl hook: the
         * window handle lives in backend state the generic layer cannot
         * see. No hook (headless, DOS, web, backends that don't set it)
         * -> unsupported, per the REG_CAPS contract. */
        if(action == RELIC_REG_WIN_MINIMIZE ||
           action == RELIC_REG_WIN_MAXIMIZE ||
           action == RELIC_REG_WIN_STATE) {
            if(vm->backend && vm->backend->win_ctl) {
                vm->backend->win_ctl(vm);
            } else {
                vm->r[1] = RELIC_REG_ERR_UNSUP;
            }
            return;
        }

        /* RELIC_REG_SUGGEST_PATH writes THROUGH `key` (it's an output
         * buffer address, not a string to read) rather than returning a
         * plain status code, so it's handled before the normal read/dispatch
         * below - see tigryx.h. */
        if(action == RELIC_REG_SUGGEST_PATH) {
            vm_read_cstr(vm, (u32)vm->r[3], value, (int)sizeof value);
#ifdef _WIN32
            char suggested[MAX_PATH];
            int len = vmreg_suggest_path(value, suggested, (int)sizeof suggested);
            if(len > 0 && key_ptr + (u32)len < vm->mem_sz) {
                relic_far_write(vm->mem, key_ptr, suggested, (size_t)len);
                VMMEM8(vm, key_ptr + (u32)len) = 0;
                vm->r[1] = len;
            } else {
                vm->r[1] = -1;
            }
#else
            vm->r[1] = -1;  /* no equivalent notion on this platform yet */
#endif
            return;
        }

        /* RELIC_REG_DESKTOPDIR writes THROUGH `key` (output buffer): the
         * current user's Desktop folder path, or -1 when the platform has
         * no such notion. */
        if(action == RELIC_REG_DESKTOPDIR) {
            char desk[1024];
            int len = -1;
#ifdef _WIN32
            vmreg_load();
            len = vmreg_shell_folder(RELIC_CSIDL_DESKTOPDIRECTORY, "Desktop",
                                     "Desktop", desk, (int)sizeof desk);
#endif
            if(len > 0 && key_ptr + (u32)len < vm->mem_sz) {
                relic_far_write(vm->mem, key_ptr, desk, (size_t)len);
                VMMEM8(vm, key_ptr + (u32)len) = 0;
                vm->r[1] = len;
            } else {
                vm->r[1] = -1;
            }
            return;
        }

        /* RELIC_REG_EXE_PATH also writes THROUGH `key` (output buffer). */
        if(action == RELIC_REG_EXE_PATH) {
            char exe[1024];
            int len = -1;
#ifdef _WIN32
            len = (int)GetModuleFileNameA(NULL, exe, (int)sizeof exe);
            if(len <= 0 || len >= (int)sizeof exe) len = -1;
#elif defined(__linux__)
            ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
            if(n > 0) { exe[n] = 0; len = (int)n; }
#elif defined(__sun)
            /* Solaris: path/a.out needs 8+; getexecname covers 7. */
            if(relic_sun_exepath(exe, sizeof exe)) len = (int)strlen(exe);
#elif defined(__APPLE__) && defined(__MACH__)
            uint32_t sz = (uint32_t)sizeof exe;
            if(_NSGetExecutablePath(exe, &sz) == 0) len = (int)strlen(exe);
#elif defined(RELIC_DOS)
            /* DJGPP: the process's own path (host argv[0]), resolved
             * against CWD if relative. cli_argv holds only user
             * passthrough args - argv[0] is stripped by the runners, so
             * exe_path is the only reliable source. */
            const char *a0 = vm->exe_path;
            if(!a0 || !a0[0])
                a0 = (vm->cli_argv && vm->cli_argv[0]) ? vm->cli_argv[0] : NULL;
            if(a0) {
                if(a0[0] == '/' || a0[0] == '\\' || (a0[0] && a0[1] == ':')) {
                    /* Absolute path already. */
                    snprintf(exe, sizeof exe, "%s", a0);
                } else {
                    /* Relative: prepend CWD. */
                    char cwd[1024];
                    if(R_GETCWD(cwd, sizeof cwd)) {
                        snprintf(exe, sizeof exe, "%s/%s", cwd, a0);
                    } else {
                        snprintf(exe, sizeof exe, "%s", a0);
                    }
                }
                exe[sizeof exe - 1] = 0;
                len = (int)strlen(exe);
            }
#endif
            if(len > 0 && key_ptr + (u32)len < vm->mem_sz) {
                relic_far_write(vm->mem, key_ptr, exe, (size_t)len);
                VMMEM8(vm, key_ptr + (u32)len) = 0;
                vm->r[1] = len;
            } else {
                vm->r[1] = -1;
            }
            return;
        }

        /* RELIC_REG_SYSTEM_THEME writes the accent colour (6-char hex
         * "RRGGBB") through `key` (output buffer) and returns 0=dark,
         * 1=light, or RELIC_REG_ERR_UNSUP. Handled before the normal
         * read/dispatch since `key` is an output buffer, not a string. */
        if(action == RELIC_REG_SYSTEM_THEME) {
            char accent[8] = "";
            int mode = -1;  /* default: unsupported */
#ifdef _WIN32
            {
                /* Dark/light: read AppsUseLightTheme from the registry.
                 * 0 = dark, 1 (or absent) = light. Win95 has no such key,
                 * so it defaults to light. */
                HKEY hKey;
                vmreg_load();
                mode = 1; /* default light */
                if(g_vmreg.RegOpenKeyExA_ && g_vmreg.RegQueryValueExA_) {
                    if(g_vmreg.RegOpenKeyExA_(HKEY_CURRENT_USER,
                            "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
                        DWORD val = 0, sz = sizeof(val);
                        if(g_vmreg.RegQueryValueExA_(hKey, "AppsUseLightTheme",
                                NULL, NULL, (LPBYTE)&val, &sz) == ERROR_SUCCESS)
                            mode = (int)val;
                        g_vmreg.RegCloseKey_(hKey);
                    }
                }

                /* Accent colour: the UI accent (shown on buttons, toggles,
                 * selection highlights) lives in Explorer\Accent, NOT in
                 * DWM\ColorizationColor (which is the window border/glass
                 * colorization and often differs from the Settings picker).
                 *
                 * Try in order:
                 *   1. Explorer\Accent\AccentColorMenu  (Win10/11, COLORREF)
                 *   2. Explorer\Accent\StartColorMenu   (Win10/11, COLORREF)
                 *   3. DWM\ColorizationColor            (Vista+, 0xAARRGGBB)
                 *   4. GetSysColor(COLOR_HIGHLIGHT)     (all Win, COLORREF)
                 *
                 * IMPORTANT: AccentColorMenu/StartColorMenu use COLORREF byte
                 * order (0xAABBGGRR — R is the LOWEST byte), same as
                 * GetSysColor. ColorizationColor uses ARGB (0xAARRGGBB — R is
                 * the third byte). The extraction differs per source.
                 */
                {
                    HKEY hKey;
                    DWORD val = 0, sz = sizeof(val);
                    int got = 0;
                    /* src_colorref: 1 = COLORREF (0xAABBGGRR), 0 = ARGB */
                    int src_colorref = 1;
                    if(g_vmreg.RegOpenKeyExA_ && g_vmreg.RegQueryValueExA_) {
                        /* 1. AccentColorMenu (COLORREF) */
                        if(g_vmreg.RegOpenKeyExA_(HKEY_CURRENT_USER,
                                "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                                0, KEY_READ, &hKey) == ERROR_SUCCESS) {
                            if(g_vmreg.RegQueryValueExA_(hKey, "AccentColorMenu",
                                    NULL, NULL, (LPBYTE)&val, &sz) == ERROR_SUCCESS)
                                got = 1;
                            /* 2. StartColorMenu (COLORREF) */
                            if(!got && g_vmreg.RegQueryValueExA_(hKey, "StartColorMenu",
                                    NULL, NULL, (LPBYTE)&val, &sz) == ERROR_SUCCESS)
                                got = 1;
                            g_vmreg.RegCloseKey_(hKey);
                        }
                        /* 3. DWM ColorizationColor (ARGB, Vista+ fallback) */
                        if(!got && g_vmreg.RegOpenKeyExA_(HKEY_CURRENT_USER,
                                "Software\\Microsoft\\Windows\\DWM",
                                0, KEY_READ, &hKey) == ERROR_SUCCESS) {
                            if(g_vmreg.RegQueryValueExA_(hKey, "ColorizationColor",
                                    NULL, NULL, (LPBYTE)&val, &sz) == ERROR_SUCCESS) {
                                got = 1;
                                src_colorref = 0;
                            }
                            g_vmreg.RegCloseKey_(hKey);
                        }
                    }
                    if(got) {
                        int cr, cg, cb;
                        if(src_colorref) {
                            /* 0xAABBGGRR: R = lowest byte */
                            cr = (int)(val & 0xFF);
                            cg = (int)((val >> 8) & 0xFF);
                            cb = (int)((val >> 16) & 0xFF);
                        } else {
                            /* 0xAARRGGBB: R = third byte */
                            cr = (int)((val >> 16) & 0xFF);
                            cg = (int)((val >> 8) & 0xFF);
                            cb = (int)(val & 0xFF);
                        }
                        _snprintf(accent, sizeof accent, "%02X%02X%02X",
                            cr, cg, cb);
                    } else {
                        /* 4. GetSysColor(COLOR_HIGHLIGHT) returns
                         * COLORREF (0x00BBGGRR). Available on all Win. */
                        DWORD cref = GetSysColor(13 /* COLOR_HIGHLIGHT */);
                        _snprintf(accent, sizeof accent, "%02X%02X%02X",
                            (int)(cref & 0xFF),
                            (int)((cref >> 8) & 0xFF),
                            (int)((cref >> 16) & 0xFF));
                    }
                    accent[sizeof accent - 1] = 0;
                }
            }
#elif defined(__APPLE__) && defined(__MACH__) && !TARGET_OS_IPHONE
            {
                /* macOS: `defaults read -g AppleInterfaceStyle` returns
                 * "Dark" (or "DarkAqua") when dark mode is active. The
                 * command fails (non-zero exit) in light mode. */
                FILE *fp = popen(
                    "defaults read -g AppleInterfaceStyle 2>/dev/null", "r");
                mode = 1; /* default light */
                if(fp) {
                    char buf[64];
                    if(fgets(buf, sizeof buf, fp)) {
                        if(strstr(buf, "Dark")) mode = 0;
                    }
                    pclose(fp);
                }

                /* Accent colour: `defaults read -g AppleAccentColor` returns
                 * an index (0=-1 multicolor, 1=blue, 2=purple, 3=pink,
                 * 4=red, 5=orange, 6=yellow, 7=green, 8=graphite). Map the
                 * common ones to RGB; -1 (multicolor) or unknown -> blue. */
                fp = popen("defaults read -g AppleAccentColor 2>/dev/null", "r");
                if(fp) {
                    char buf[64];
                    int idx = -1;
                    if(fgets(buf, sizeof buf, fp)) idx = atoi(buf);
                    pclose(fp);
                    /* Map index to RGB hex. Default to system blue. */
                    switch(idx) {
                        case 2: strcpy(accent, "8940C8"); break; /* purple */
                        case 3: strcpy(accent, "D4027D"); break; /* pink   */
                        case 4: strcpy(accent, "E03A3A"); break; /* red    */
                        case 5: strcpy(accent, "E07B2A"); break; /* orange */
                        case 6: strcpy(accent, "E0B500"); break; /* yellow */
                        case 7: strcpy(accent, "12B312"); break; /* green  */
                        case 8: strcpy(accent, "808080"); break; /* graphite*/
                        default: strcpy(accent, "0A84FF"); break; /* blue/-1 */
                    }
                } else {
                    strcpy(accent, "0A84FF"); /* macOS system blue */
                }
            }
#endif
            /* Write accent colour through the key buffer (6 chars + NUL). */
            {
                int alen = (int)strlen(accent);
                if(alen > 0 && key_ptr + (u32)alen < vm->mem_sz) {
                    relic_far_write(vm->mem, key_ptr, accent, (size_t)alen);
                    VMMEM8(vm, key_ptr + (u32)alen) = 0;
                } else if(key_ptr + 1 < vm->mem_sz) {
                    VMMEM8(vm, key_ptr) = 0;
                }
            }
            vm->r[1] = mode;
            return;
        }

        /* RELIC_REG_SYSTEM_COLORS writes eleven ';'-separated "RRGGBB" hex
         * fields into `key` (output buffer, >=96 bytes) - the classic
         * GetSysColor set. See tigryx.h for the field order. */
        if(action == RELIC_REG_SYSTEM_COLORS) {
            int len = -1;
#ifdef _WIN32
            /* 3DFACE, 3DHILIGHT, 3DLIGHT, 3DSHADOW, 3DDKSHADOW, WINDOW,
             * WINDOWTEXT, BTNTEXT, HIGHLIGHT, HIGHLIGHTTEXT, GRAYTEXT */
            static const int ids[11] = { 15, 20, 22, 16, 21, 5, 8, 18, 13, 14, 17 };
            char buf[96];
            char *p = buf;
            int i;
            for(i = 0; i < 11; i++) {
                DWORD c = GetSysColor(ids[i]);
                p += _snprintf(p, (size_t)(buf + sizeof buf - p),
                               "%02X%02X%02X",
                               (int)(c & 0xFF), (int)((c >> 8) & 0xFF),
                               (int)((c >> 16) & 0xFF));
                if(i < 10) *p++ = ';';
            }
            *p = 0;
            len = (int)strlen(buf);
            if(key_ptr + (u32)len + 1 < vm->mem_sz) {
                relic_far_write(vm->mem, key_ptr, buf, (size_t)len);
                VMMEM8(vm, key_ptr + (u32)len) = 0;
                vm->r[1] = len;
            } else {
                len = -1;
            }
#endif
            if(len < 0) vm->r[1] = RELIC_REG_ERR_UNSUP;
            return;
        }

        /* RELIC_REG_SYSTEM_FONT writes the OS default GUI font face name
         * into `key` (output buffer, >=32 bytes). Uses
         * SystemParametersInfo(SPI_GETNONCLIENTMETRICS).lfMessageFont - the
         * message-box/UI font: "MS Sans Serif" on 9x/NT4, "Tahoma" on
         * 2000/XP, "Segoe UI" on Vista+. user32 only (no gdi32 dependency). */
        if(action == RELIC_REG_SYSTEM_FONT) {
            int len = -1;
#ifdef _WIN32
            {
                NONCLIENTMETRICSA ncm;
                memset(&ncm, 0, sizeof ncm);
                ncm.cbSize = sizeof(ncm);
                /* Pre-Vista Windows lacks iPaddedBorderWidth and rejects the
                 * larger struct size; retry with the trimmed size. The member
                 * does not exist in old (Win95-era) SDK headers either, so
                 * subtract sizeof(int) rather than naming it. */
                if(!SystemParametersInfoA(SPI_GETNONCLIENTMETRICS,
                        sizeof ncm, &ncm, 0)) {
                    ncm.cbSize = sizeof(ncm) - sizeof(int);
                    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS,
                                          ncm.cbSize, &ncm, 0);
                }
                if(ncm.lfMessageFont.lfFaceName[0]) {
                    len = (int)strlen(ncm.lfMessageFont.lfFaceName);
                    if(len > 0 && len < 32
                            && key_ptr + (u32)len + 1 < vm->mem_sz) {
                        relic_far_write(vm->mem, key_ptr, ncm.lfMessageFont.lfFaceName, (size_t)len);
                        VMMEM8(vm, key_ptr + (u32)len) = 0;
                    } else {
                        len = -1;
                    }
                }
            }
#endif
            vm->r[1] = len > 0 ? len : RELIC_REG_ERR_UNSUP;
            return;
        }

        /* Process-environment actions: `key` stays an input (the name,
         * or a decimal index for ENV_NAME); string results are written
         * THROUGH `value`. Handled in the common section because the C
         * runtime API is uniform; RELIC_HAVE_ENV==0 platforms all answer
         * RELIC_REG_ERR_UNSUP. */
        if(action >= RELIC_REG_ENV_GET && action <= RELIC_REG_ENV_NAME) {
            char name[512], ebuf[1024];
            int len = -1, rc = RELIC_REG_ERR_UNSUP;
            u32 val_ptr = (u32)vm->r[3];
            vm_read_cstr(vm, key_ptr, name, (int)sizeof name);
            ebuf[0] = 0;
#if RELIC_HAVE_ENV
            if(action == RELIC_REG_ENV_GET) {
                const char *v = name[0] ? getenv(name) : NULL;
                if(v) {
                    size_t vl = strlen(v);
                    if(vl >= sizeof ebuf) vl = sizeof ebuf - 1;
                    memcpy(ebuf, v, vl);
                    ebuf[vl] = 0;
                    len = (int)vl;
                } else {
                    len = 0;   /* unset reads as empty */
                }
                rc = len;
            } else if(action == RELIC_REG_ENV_SET) {
                vm_read_cstr(vm, val_ptr, ebuf, (int)sizeof ebuf);
                rc = relic_env_set(name, ebuf);
            } else if(action == RELIC_REG_ENV_UNSET) {
                rc = relic_env_unset(name);
            } else if(action == RELIC_REG_ENV_COUNT) {
                rc = relic_env_count();
            } else {   /* RELIC_REG_ENV_NAME */
                rc = relic_env_entry(atoi(name), ebuf, (int)sizeof ebuf);
                len = rc;
            }
            if(len >= 0) {
                if(val_ptr + (u32)len < vm->mem_sz) {
                    if(len > 0)
                        relic_far_write(vm->mem, val_ptr, ebuf, (size_t)len);
                    VMMEM8(vm, val_ptr + (u32)len) = 0;
                } else {
                    rc = RELIC_REG_ERR_ARGS;   /* buffer past guest RAM */
                }
            }
#endif
            vm->r[1] = rc;
            return;
        }

        vm_read_cstr(vm, key_ptr, key,   (int)sizeof key);
        vm_read_cstr(vm, (u32)vm->r[3], value, (int)sizeof value);

#ifdef _WIN32
        vm->r[1] = vmreg_dispatch(action, key, value);
#elif defined(RELIC_DOS)
        vm->r[1] = vmreg_dos_dispatch(action, key, value);
#elif defined(__linux__)
        vm->r[1] = vmreg_linux_dispatch(action, key, value);
#elif defined(__APPLE__) && defined(__MACH__) && !TARGET_OS_IPHONE
        vm->r[1] = vmreg_mac_dispatch(action, key, value);
#else
        /* Unknown platform. Report "unsupported" so installer scripts can
         * branch on RELIC_REG_CAPS and skip cleanly. */
        (void)action; (void)key; (void)value;
        vm->r[1] = (action == RELIC_REG_CAPS) ? 0 : RELIC_REG_ERR_UNSUP;
#endif
        /* The RELIC_REG_WIN_* actions live behind the backend hook, so
         * the per-OS caps masks above don't know about them: advertise
         * them here when a win_ctl implementation exists. */
        if(action == RELIC_REG_CAPS &&
           vm->backend && vm->backend->win_ctl) {
            vm->r[1] |= (1 << RELIC_REG_WIN_MINIMIZE) |
                        (1 << RELIC_REG_WIN_MAXIMIZE) |
                        (1 << RELIC_REG_WIN_STATE);
        }
    }
}

/* --- Audio --- */
RELIC_OP_EXPORT void relic_op_snd_tone_freq(RelicVM *vm) {
    if(vm->backend && vm->backend->snd_tone_freq) vm->backend->snd_tone_freq(vm);
}
RELIC_OP_EXPORT void relic_op_snd_tone_id(RelicVM *vm) {
    if(vm->backend && vm->backend->snd_tone_id) vm->backend->snd_tone_id(vm);
}
RELIC_OP_EXPORT void relic_op_snd_play(RelicVM *vm) {
    if(vm->backend && vm->backend->snd_play) vm->backend->snd_play(vm);
}
RELIC_OP_EXPORT void relic_op_snd_music(RelicVM *vm) {
    if(vm->backend && vm->backend->snd_music) vm->backend->snd_music(vm);
}
RELIC_OP_EXPORT void relic_op_snd_mixer(RelicVM *vm) {
    if(vm->backend && vm->backend->snd_mixer) vm->backend->snd_mixer(vm);
}

/* -------------------------------------------------------------------------
 * Single-instruction step body.
 *
 * Executes exactly one already-fetched instruction (`instr`), assuming
 * vm->pc has already been advanced past it. This is the canonical place
 * where opcode semantics live: both relic_vm_run() and relic_vm_step1()
 * (used by the JIT as a fallback for opcodes it does not yet inline)
 * dispatch through here, so backend semantics never drift between the
 * interpreter and the JIT.
 * ---------------------------------------------------------------------- */
static void step_body(RelicVM *vm, u32 instr) {
    IRBuffer *ir = vm->ir;
    u8  op    = IR_OP(instr);
    u8  dst   = IR_DST(instr);
    u8  src_a = IR_SRCA(instr);
    u8  src_b = IR_SRCB(instr);
    i32 imm12 = IR_IMM12(instr);

    i32 a = reg_r(vm, src_a);
    i32 b = reg_r(vm, src_b);

    switch(op) {
            case OP_NOP:  break;
            case OP_HALT:
                /* src_a != 0: exit code comes from r[src_a] (@exit stages
                 * its arg in r1). Otherwise imm12 (hand-written `HALT n`
                 * assembly). */
                vm->exit_code = src_a ? a : imm12;
                vm->running   = false;
                break;

            /* --- Load / Store --- */
            case OP_LDC: {
                /* imm20: bits [19:0] = upper 20 bits of instruction (dst+srca+srcb+imm12) */
                i32 imm20 = IR_IMM20(instr);
                reg_w(vm, dst, imm20);
                break;
            }
            case OP_LDC32:
                /* next word is the literal */
                if(vm->pc < (u32)ir->len)
                    reg_w(vm, dst, (i32)IRCODE(ir, vm->pc++));
                break;
            case OP_LD8:
                reg_w(vm, dst, (i32)(u32)mem_read8(vm,  (u32)(a + imm12)));
                break;
            case OP_LD16:
                reg_w(vm, dst, (i32)(u32)mem_read16(vm, (u32)(a + imm12)));
                break;
            case OP_LD32:
                reg_w(vm, dst, mem_read32(vm, (u32)(a + imm12)));
                break;
            case OP_ST8:
                mem_write8(vm,  (u32)(reg_r(vm,dst) + imm12), (u8)a);
                break;
            case OP_ST16:
                mem_write16(vm, (u32)(reg_r(vm,dst) + imm12), (u16)a);
                break;
            case OP_ST32:
                mem_write32(vm, (u32)(reg_r(vm,dst) + imm12), a);
                break;
            case OP_LEA:
                reg_w(vm, dst, a + imm12);
                break;
            case OP_MOV:
                reg_w(vm, dst, a);
                break;

            /* --- Arithmetic --- */
            case OP_ADD:
                reg_w(vm, dst, a + b);
                break;
            case OP_ADDI: reg_w(vm, dst, a + imm12);        break;
            case OP_SUB:  reg_w(vm, dst, a - b);            break;
            case OP_SUBI: reg_w(vm, dst, a - imm12);        break;
            case OP_MUL:  reg_w(vm, dst, a * b);            break;
            case OP_DIV:  reg_w(vm, dst, b ? a / b : 0);   break;
            case OP_MOD:  reg_w(vm, dst, b ? a % b : 0);   break;
            case OP_NEG:  reg_w(vm, dst, -a);               break;

            /* --- Floating point (single precision) ---
             * All float values live in the i32 register file as their raw
             * IEEE-754 bit pattern. AS_F/AS_I bit-cast without conversion.
             * On non-FPU targets these helpers must be redirected to a
             * soft-float / ROM implementation in a platform fpu.h. */
            #define AS_F(x) (((RelicFBits){ .i = (x) }).f)
            #define AS_I(x) (((RelicFBits){ .f = (x) }).i)
            case OP_FADD: reg_w(vm, dst, AS_I(AS_F(a) + AS_F(b))); break;
            case OP_FSUB: reg_w(vm, dst, AS_I(AS_F(a) - AS_F(b))); break;
            case OP_FMUL: reg_w(vm, dst, AS_I(AS_F(a) * AS_F(b))); break;
            case OP_FDIV: reg_w(vm, dst, AS_I(AS_F(a) / AS_F(b))); break;
            case OP_FNEG: reg_w(vm, dst, AS_I(-AS_F(a))); break;
            case OP_FCEQ: reg_w(vm, dst, AS_F(a) == AS_F(b) ? 1 : 0); break;
            case OP_FCLT: reg_w(vm, dst, AS_F(a) <  AS_F(b) ? 1 : 0); break;
#ifdef RELIC_RISCOS_FPA
            /* GCC's FPA <= lowers to cmfe+movls, and the FPEmulator's
             * unordered flag pattern makes 'ls' TRUE for NaN operands.
             * Guard with explicit NaN tests (x == x is false for NaN);
             * the guard can't be folded back into a single compare. */
            case OP_FCLE: {
                float fa_ = AS_F(a), fb_ = AS_F(b);
                reg_w(vm, dst, (fa_ == fa_ && fb_ == fb_ && fa_ <= fb_) ? 1 : 0);
            } break;
#else
            case OP_FCLE: reg_w(vm, dst, AS_F(a) <= AS_F(b) ? 1 : 0); break;
#endif
            case OP_I2F:  reg_w(vm, dst, AS_I((float)a));   break;
            case OP_F2I:  reg_w(vm, dst, (i32)AS_F(a));     break;

            /* --- Floating point (double precision, d bank) ---
             * The same 4-bit operand fields index vm->d[] instead of
             * vm->r[]. `a`/`b` are used only where a source operand is
             * genuinely an r register: the base address for DLD/DST and
             * the int/f32 source for the conversions. Compares write an
             * r-bank 0/1, matching CEQ/FCEQ. */
            case OP_DADD: vm->d[dst] = vm->d[src_a] + vm->d[src_b]; break;
            case OP_DSUB: vm->d[dst] = vm->d[src_a] - vm->d[src_b]; break;
            case OP_DMUL: vm->d[dst] = vm->d[src_a] * vm->d[src_b]; break;
            case OP_DDIV: vm->d[dst] = vm->d[src_a] / vm->d[src_b]; break;
            case OP_DNEG: vm->d[dst] = -vm->d[src_a];               break;
            case OP_DMOV: vm->d[dst] =  vm->d[src_a];               break;
            case OP_DCEQ: reg_w(vm, dst, vm->d[src_a] == vm->d[src_b] ? 1 : 0); break;
            case OP_DCLT: reg_w(vm, dst, vm->d[src_a] <  vm->d[src_b] ? 1 : 0); break;
#ifdef RELIC_RISCOS_FPA
            case OP_DCLE: {             /* same cmfe+movls NaN trap as FCLE */
                double da_ = vm->d[src_a], db_ = vm->d[src_b];
                reg_w(vm, dst, (da_ == da_ && db_ == db_ && da_ <= db_) ? 1 : 0);
            } break;
#else
            case OP_DCLE: reg_w(vm, dst, vm->d[src_a] <= vm->d[src_b] ? 1 : 0); break;
#endif
            case OP_DLDC: {
                /* 64-bit literal as two payload words, lo then hi. A u64
                 * is memcpy'd into the double, so the in-memory byte
                 * order of the bit pattern follows the host either way -
                 * the code stream itself is just two u32 words. */
                u64 bits = 0;
                if(vm->pc < (u32)ir->len)
                    bits  = (u64)(u32)IRCODE(ir, vm->pc++);
                if(vm->pc < (u32)ir->len)
                    bits |= (u64)(u32)IRCODE(ir, vm->pc++) << 32;
#if defined(RELIC_RISCOS_FPA)
                /* FPA doubles are stored "mixed-endian": the high 32-bit
                 * word sits at the lower address (bytes within each word
                 * little-endian). A canonical u64 bit pattern must be
                 * word-swapped before it lands in a C double. */
                bits = (bits >> 32) | (bits << 32);
#endif
                memcpy(&vm->d[dst], &bits, 8);
                break;
            }
            case OP_DLD:  vm->d[dst] = mem_read64(vm, (u32)(a + imm12)); break;
            case OP_DST:  mem_write64(vm, (u32)(reg_r(vm, dst) + imm12),
                                      vm->d[src_a]);                    break;
            case OP_I2D:  vm->d[dst] = (double)a;              break;
            case OP_U2D:  vm->d[dst] = (double)(u32)a;         break;
            case OP_D2I:  reg_w(vm, dst, (i32)vm->d[src_a]);   break;
            case OP_D2U:  reg_w(vm, dst, (i32)(u32)vm->d[src_a]); break;
            case OP_F2D:  vm->d[dst] = (double)AS_F(a);        break;
            case OP_D2F:  reg_w(vm, dst, AS_I((float)vm->d[src_a])); break;
            case OP_DSQRT: vm->d[dst] = sqrt (vm->d[src_a]);   break;
            case OP_DSIN:  vm->d[dst] = sin  (vm->d[src_a]);   break;
            case OP_DCOS:  vm->d[dst] = cos  (vm->d[src_a]);   break;
            case OP_DTAN:  vm->d[dst] = tan  (vm->d[src_a]);   break;
            case OP_DASIN: vm->d[dst] = asin (vm->d[src_a]);   break;
            case OP_DACOS: vm->d[dst] = acos (vm->d[src_a]);   break;
            case OP_DATAN: vm->d[dst] = atan (vm->d[src_a]);   break;
            case OP_DATAN2:vm->d[dst] = atan2(vm->d[src_a], vm->d[src_b]); break;
            case OP_DEXP:  vm->d[dst] = exp  (vm->d[src_a]);   break;
            case OP_DLOG:  vm->d[dst] = log  (vm->d[src_a]);   break;
            case OP_DLOG10:vm->d[dst] = log10(vm->d[src_a]);   break;
            case OP_DPOW:  vm->d[dst] = pow  (vm->d[src_a], vm->d[src_b]); break;
            case OP_DFLOOR:vm->d[dst] = floor(vm->d[src_a]);   break;
            case OP_DCEIL: vm->d[dst] = ceil (vm->d[src_a]);   break;
            case OP_DROUND:vm->d[dst] = relic_dround_half_away(vm->d[src_a]); break;
            case OP_DFMOD: vm->d[dst] = fmod (vm->d[src_a], vm->d[src_b]); break;
            case OP_DFABS: vm->d[dst] = fabs (vm->d[src_a]);   break;
            case OP_DSINH: vm->d[dst] = sinh (vm->d[src_a]);   break;
            case OP_DCOSH: vm->d[dst] = cosh (vm->d[src_a]);   break;
            case OP_DTANH: vm->d[dst] = tanh (vm->d[src_a]);   break;
            #undef AS_F
            #undef AS_I

            /* --- Bitwise --- */
            case OP_AND:  reg_w(vm, dst, a & b);            break;
            case OP_OR:   reg_w(vm, dst, a | b);            break;
            case OP_XOR:  reg_w(vm, dst, a ^ b);            break;
            case OP_NOT:  reg_w(vm, dst, ~a);               break;
            case OP_SHL:  reg_w(vm, dst, a << (b & 31));    break;
            case OP_SHR:  reg_w(vm, dst, (i32)((u32)a >> (b & 31))); break;
            case OP_SAR:  reg_w(vm, dst, a >> (b & 31));    break;
            case OP_ANDI: reg_w(vm, dst, a & imm12);        break;
            case OP_ORI:  reg_w(vm, dst, a | imm12);        break;

            /* --- Comparison --- */
            case OP_CEQ:  reg_w(vm, dst, a == b ? 1 : 0);  break;
            case OP_CNE:  reg_w(vm, dst, a != b ? 1 : 0);  break;
            case OP_CLT:  reg_w(vm, dst, a <  b ? 1 : 0);  break;
            case OP_CLE:  reg_w(vm, dst, a <= b ? 1 : 0);  break;
            case OP_CGT:  reg_w(vm, dst, a >  b ? 1 : 0);  break;
            case OP_CGE:  reg_w(vm, dst, a >= b ? 1 : 0);  break;
            case OP_CLTU: reg_w(vm, dst, (u32)a <  (u32)b ? 1 : 0); break;
            case OP_CLEU: reg_w(vm, dst, (u32)a <= (u32)b ? 1 : 0); break;

            /* --- Branches / Jumps --- */
            /* PC-relative: vm->pc already points past the jump, which is
             * exactly the displacement base (see IR_JTARGET). */
            case OP_JMP:
                vm->pc = (u32)((i32)vm->pc + IR_JDISP(instr));
                break;
            case OP_JT:
                if(reg_r(vm, src_a) != 0) vm->pc = (u32)((i32)vm->pc + IR_JDISP(instr));
                break;
            case OP_JF:
                if(reg_r(vm, src_a) == 0) vm->pc = (u32)((i32)vm->pc + IR_JDISP(instr));
                break;
            case OP_JMPR:
                vm->pc = (u32)reg_r(vm, dst);
                break;

            /* --- Calls / Return --- */
            case OP_CALL: {
                u32 fn_id = instr & 0x00FFFFFF;
                if(fn_id < (u32)ir->nfuncs) {
                    stack_push(vm, (i32)vm->pc);
                    stack_push(vm, (i32)vm->fp);
                    vm->fp = vm->sp;
                    vm->pc = (u32)IRFNOFF(ir, fn_id);
                }
                break;
            }
            case OP_CALLR: {
                u32 fn_id = (u32)reg_r(vm, dst);
                if(fn_id < (u32)ir->nfuncs) {
                    stack_push(vm, (i32)vm->pc);
                    stack_push(vm, (i32)vm->fp);
                    vm->fp = vm->sp;
                    vm->pc = (u32)IRFNOFF(ir, fn_id);
                }
                break;
            }
            case OP_CALLP: {
                u32 fn_id = (u32)reg_r(vm, dst);
                if(fn_id < (u32)ir->nfuncs) {
                    stack_push(vm, (i32)vm->pc);
                    stack_push(vm, (i32)vm->fp);
                    vm->fp = vm->sp;
                    vm->pc = (u32)IRFNOFF(ir, fn_id);
                }
                break;
            }
            case OP_RET: {
                vm->sp = vm->fp;
                vm->fp = (u32)stack_pop(vm);
                u32 ret_pc = (u32)stack_pop(vm);
                vm->pc = ret_pc;
                /* Returning from the outermost (main) frame: the sentinel
                 * call frame pushed ir->len as the return PC. Treat it as a
                 * clean halt so the VM stops with running=false, matching
                 * OP_HALT / @exit. Without this, relic_vm_run's loop exits
                 * via pc>=len but running stays true, tripping the
                 * "ran past end of code" trace dump. Only the sentinel
                 * setups push ir->len; real OP_CALL return PCs are always
                 * pc+1 inside the code buffer, so this can't false-fire. */
                if(ret_pc >= (u32)ir->len) {
                    vm->running = false;
                }
                break;
            }
            case OP_ENTER: {
                /* Allocate frame_words * 4 bytes on stack. The frame size
                 * is a 20-bit unsigned immediate in bits [19:0] (the
                 * dst/srca/srcb fields are repurposed, per the imm20
                 * convention in docs/opcodes.md) so large local arrays
                 * don't truncate the way a 12-bit imm would. */
                i32 nwords = IR_UIMM20(instr);
                u32 need = (u32)(nwords * 4);
                /* Guard against stack overflow. The stack grows down from
                 * mem_sz and the heap grows up from heap_base; if a
                 * function's cumulative frame usage (locals plus the
                 * anonymous struct-by-value staging slots the compiler
                 * allocates per call and never reclaims within a
                 * function) pushes sp below heap_top, the two regions
                 * collide and every subsequent local write silently
                 * corrupts live heap objects - manifesting later as an
                 * unrelated crash far from the real cause. Trap here
                 * instead, at the point of overflow. */
                if(need > vm->sp || vm->sp - need < vm->heap_top) {
                    u32 at = (vm->pc > 0) ? vm->pc - 1 : vm->pc;
                    vm_stack_overflow_at(vm, at);
                    break;
                }
                vm->sp -= need;
                break;
            }
            case OP_LEAVE: {
                vm->sp = vm->fp;
                break;
            }

            /* --- Type Conversion --- */
            case OP_SX8:    reg_w(vm, dst, (i32)(i8) (a & 0xFF));   break;
            case OP_SX16:   reg_w(vm, dst, (i32)(i16)(a & 0xFFFF)); break;
            case OP_ZX8:    reg_w(vm, dst, (i32)(u32)(a & 0xFF));   break;
            case OP_ZX16:   reg_w(vm, dst, (i32)(u32)(a & 0xFFFF)); break;
            case OP_TRUNC8: reg_w(vm, dst, a & 0xFF);               break;
            case OP_TRUNC16:reg_w(vm, dst, a & 0xFFFF);             break;

            /* --- Stack / Misc --- */
            case OP_PUSH: stack_push(vm, a);        break;
            case OP_POP:  reg_w(vm, dst, stack_pop(vm)); break;

            /* --- Graphics builtins --- */
            case OP_GFX_CLEAR:    relic_op_gfx_clear(vm);    break;
            case OP_GFX_SPRITE:   relic_op_gfx_sprite(vm);   break;
            case OP_GFX_PLOT:     relic_op_gfx_plot(vm);     break;
            case OP_GFX_FLIP:     relic_op_gfx_flip(vm);     break;
            case OP_GFX_LINE:     relic_op_gfx_line(vm);     break;
            case OP_GFX_WINDOW:   relic_op_gfx_window(vm);   break;
            case OP_GFX_POLYGON:  relic_op_gfx_polygon(vm);  break;
            case OP_GFX_POLYLINE: relic_op_gfx_polyline(vm); break;
            case OP_GFX_ELLIPSE:        relic_op_gfx_ellipse(vm);        break;
            case OP_GFX_ELLIPSE_FILLED: relic_op_gfx_ellipse_filled(vm); break;
            case OP_GFX_ROUNDED_RECT:        relic_op_gfx_rounded_rect(vm);        break;
            case OP_GFX_ROUNDED_RECT_FILLED: relic_op_gfx_rounded_rect_filled(vm); break;
            case OP_GFX_CAMERA:   relic_op_gfx_camera(vm);   break;
            case OP_GFX_TEXTURE:  relic_op_gfx_texture(vm);  break;
            case OP_GFX_LIGHT:    relic_op_gfx_light(vm);    break;
            case OP_GFX_MESH:     relic_op_gfx_mesh(vm);     break;
            case OP_GFX_FOG:      relic_op_gfx_fog(vm);      break;
            case OP_GFX_TEXT:     relic_op_gfx_text(vm);     break;
            case OP_GFX_VIEWPORT: relic_op_gfx_viewport(vm); break;
            case OP_GFX_TEXT_MEASURE: relic_op_gfx_text_measure(vm); break;
            case OP_GFX_CLIP:     relic_op_gfx_clip(vm);     break;
case OP_GFX_VIEW:     relic_op_gfx_view(vm);     break;
case OP_GFX_VIEW_CAPTURE: relic_op_gfx_view_capture(vm); break;
case OP_GFX_VIEW_PRESENT: relic_op_gfx_view_present(vm); break;
            case OP_GFX_DRAW_LIST: relic_op_gfx_draw_list(vm); break;
            case OP_GFX_DRAW_MODE: relic_op_gfx_draw_mode(vm); break;
            case OP_GFX_LINES3D: relic_op_gfx_lines3d(vm); break;
            case OP_GFX_GRID3D:  relic_op_gfx_grid3d(vm);  break;
            case OP_GFX_PALETTE_UPLOAD: relic_op_gfx_palette_upload(vm); break;
            case OP_WR_PALETTE:   relic_op_wr_palette(vm);   break;
            case OP_WR_TEXTURE:   relic_op_wr_texture(vm);   break;
            case OP_WR_FLAT:      relic_op_wr_flat(vm);      break;
            case OP_WR_TEST:      relic_op_wr_test(vm);      break;
            case OP_WR_MAP:       relic_op_wr_map(vm);       break;
            case OP_WR_RENDER:    relic_op_wr_render(vm);    break;
            case OP_WR_TEXMAP:    relic_op_wr_texmap(vm);    break;
            case OP_WR_SKY:       relic_op_wr_sky(vm);       break;
            case OP_WR_FLATSHADE: relic_op_wr_flatshade(vm); break;
            case OP_WR_PATCH:     relic_op_wr_patch(vm);     break;
            case OP_WR_SPRITE:    relic_op_wr_sprite(vm);    break;
            case OP_WR_BLOCKMAP:  relic_op_wr_blockmap(vm);  break;
            case OP_WR_BLOCKED:   relic_op_wr_blocked(vm);   break;
            case OP_WR_BLOCKED_LINE: relic_op_wr_blocked_line(vm); break;

            /* --- Input builtins --- */
            case OP_INP_KEY:      relic_op_inp_key(vm);      break;
            case OP_INP_KEYHELD:  relic_op_inp_keyheld(vm);  break;
            case OP_INP_MOUSE:    relic_op_inp_mouse(vm);    break;
            case OP_INP_GAMEPAD_COUNT: relic_op_inp_gamepad_count(vm); break;
            case OP_INP_GAMEPAD:       relic_op_inp_gamepad(vm);       break;

            /* --- System builtins --- */
            case OP_SYS_RAND:     relic_op_sys_rand(vm);     break;
            case OP_SYS_MILLIS:   relic_op_sys_millis(vm);   break;
            case OP_SYS_CONSOLE:  relic_op_sys_console(vm);  break;
            case OP_SYS_SNPRINT:  relic_op_sys_snprint(vm);  break;
            case OP_SYS_ASSERT:   relic_op_sys_assert(vm);   break;
            case OP_SYS_ALLOC:    relic_op_sys_alloc(vm);    break;
            case OP_SYS_FREE:     relic_op_sys_free(vm);     break;
            case OP_SYS_MEMCPY:   relic_op_sys_memcpy(vm);   break;
            case OP_SYS_MEMSET:   relic_op_sys_memset(vm);   break;
            case OP_SYS_FOPEN:    relic_op_sys_fopen(vm);    break;
            case OP_SYS_FREAD:    relic_op_sys_fread(vm);    break;
            case OP_SYS_FCLOSE:   relic_op_sys_fclose(vm);   break;
            case OP_SYS_FWRITE:   relic_op_sys_fwrite(vm);   break;
            case OP_SYS_FSEEK:    relic_op_sys_fseek(vm);    break;
            case OP_SYS_FLEN:     relic_op_sys_flen(vm);     break;
            case OP_SYS_FDIR:     relic_op_sys_fdir(vm);     break;
            case OP_SYS_FPATH:    relic_op_sys_fpath(vm);    break;
            case OP_SYS_LOAD_PNG: relic_op_sys_load_png(vm); break;
            case OP_SYS_FDELETE:  relic_op_sys_fdelete(vm);  break;
            case OP_SYS_FMKDIR:   relic_op_sys_fmkdir(vm);   break;
            case OP_SYS_CLIP_SET: relic_op_sys_clip_set(vm); break;
            case OP_SYS_CLIP_GET: relic_op_sys_clip_get(vm); break;
            case OP_SYS_ARGS:     relic_op_sys_args(vm);     break;
            case OP_SYS_ISNUM:    relic_op_sys_isnum(vm);    break;

            /* --- Math (single precision) ---
             * Args: r1 = f32 bit pattern. Return: r1 = f32 bit pattern.
             * Platforms without an FPU should override these via the backend
             * to a CORDIC / ROM implementation. */
            case OP_SYS_SIN:      relic_op_sys_sin(vm);      break;
            case OP_SYS_COS:      relic_op_sys_cos(vm);      break;
            case OP_SYS_SQRT:     relic_op_sys_sqrt(vm);     break;
            case OP_SYS_EXP:      relic_op_sys_exp(vm);      break;
            case OP_SYS_LOG:      relic_op_sys_log(vm);      break;
            case OP_SYS_LOG10:    relic_op_sys_log10(vm);    break;
            case OP_SYS_POW:      relic_op_sys_pow(vm);      break;
            case OP_SYS_ATAN:     relic_op_sys_atan(vm);     break;
            case OP_SYS_ATAN2:    relic_op_sys_atan2(vm);    break;
            case OP_SYS_TAN:      relic_op_sys_tan(vm);      break;
            case OP_SYS_ASIN:     relic_op_sys_asin(vm);     break;
            case OP_SYS_ACOS:     relic_op_sys_acos(vm);     break;
            case OP_SYS_FLOOR:    relic_op_sys_floor(vm);    break;
            case OP_SYS_CEIL:     relic_op_sys_ceil(vm);     break;
            case OP_SYS_ROUND:    relic_op_sys_round(vm);    break;
            case OP_SYS_FMOD:     relic_op_sys_fmod(vm);     break;
            case OP_SYS_FABS:     relic_op_sys_fabs(vm);     break;
            case OP_SYS_SINH:     relic_op_sys_sinh(vm);     break;
            case OP_SYS_COSH:     relic_op_sys_cosh(vm);     break;
            case OP_SYS_TANH:     relic_op_sys_tanh(vm);     break;
            case OP_SYS_PLATFORM: relic_op_sys_platform(vm); break;
            case OP_SYS_OS:       relic_op_sys_os(vm, instr); break;

            /* --- Web / HTTP builtins --- */
            case OP_SYS_HTTP_REQ:    relic_op_sys_http_request(vm); break;
            case OP_SYS_HTTP_POLL:   relic_op_sys_http_poll(vm);    break;
            case OP_SYS_HTTP_STATUS: relic_op_sys_http_status(vm);  break;
            case OP_SYS_HTTP_READ:   relic_op_sys_http_read(vm);    break;
            case OP_SYS_HTTP_CLOSE:  relic_op_sys_http_close(vm);   break;

            /* --- Tiny socket server (experimental) --- */
            case OP_SYS_SERVE:       relic_op_sys_serve(vm);       break;
            case OP_SYS_SERVE_REPLY: relic_op_sys_serve_reply(vm); break;

            /* --- Subprocess management builtins --- */
            case OP_SYS_PROC_SPAWN:     relic_op_sys_proc_spawn(vm);    break;
            case OP_SYS_PROC_POLL:      relic_op_sys_proc_poll(vm);     break;
            case OP_SYS_PROC_EXIT_CODE: relic_op_sys_proc_exit_code(vm); break;
            case OP_SYS_PROC_READ_OUT:  relic_op_sys_proc_read_out(vm);  break;
            case OP_SYS_PROC_READ_ERR:  relic_op_sys_proc_read_err(vm);  break;
            case OP_SYS_PROC_KILL:      relic_op_sys_proc_kill(vm);     break;
            case OP_SYS_PROC_CLOSE:     relic_op_sys_proc_close(vm);    break;

            /* --- ZIP extraction --- */
            case OP_SYS_UNZIP:       relic_op_sys_unzip(vm);       break;

            /* --- Platform registration --- */
            case OP_SYS_REGISTER:    relic_op_sys_register(vm);    break;

            /* --- Audio builtins --- */
            case OP_SND_TONE_FREQ: relic_op_snd_tone_freq(vm); break;
            case OP_SND_TONE_ID:   relic_op_snd_tone_id(vm);   break;
            case OP_SND_PLAY:      relic_op_snd_play(vm);      break;
            case OP_SND_MUSIC:     relic_op_snd_music(vm);     break;
            case OP_SND_MIXER:     relic_op_snd_mixer(vm);     break;

            default: {
                extern void vm_trace_dump(RelicVM *vm);
                u32 at = vm->pc - 1;
                fprintf(stderr, "vm: Unknown opcode 0x%02X at pc=%u\n", op, at);
                vm_trace_dump(vm);

                vm->trap.kind    = RELIC_TRAP_UNKNOWN_OPCODE;
                vm->trap.pc      = at;
                vm->trap.line    = (vm->ir && at < (u32)vm->ir->len) ? IRLINE_RD(vm->ir, at) : 0;
                vm->trap.file_id = (vm->ir && at < (u32)vm->ir->len) ? IRFILEID_RD(vm->ir, at) : -1;
                snprintf(vm->trap.message, sizeof(vm->trap.message), "Unknown opcode 0x%02X", op);

                vm->running = false;
                vm->exit_code = -1;
                break;
            }
    }
}

/* -------------------------------------------------------------------------
 * Single-step entry point used by the JIT as a safety-net fallback for
 * opcodes it does not yet inline. Fetches one instruction at vm->pc,
 * advances pc, and dispatches through step_body. Identical semantics to
 * one iteration of the relic_vm_run() loop, minus the debugger hook
 * (the JIT and the debugger are mutually exclusive).
 * ---------------------------------------------------------------------- */
void relic_vm_step1(RelicVM *vm) {
    if(!vm->running) return;
    if(vm->pc >= (u32)vm->ir->len) { vm->running = false; return; }
    u32 instr = IRCODE(vm->ir, vm->pc++);
    step_body(vm, instr);
}

/* -------------------------------------------------------------------------
 * AOT slow-path helpers. The ARM32 native emitter (src/native/arm32-
 * riscos) calls these when its inline bounds/parity checks fail; they
 * carry the same OOB semantics as the jit_arm32 C helpers - a failed
 * read returns 0, a failed write is dropped. The divide helpers match
 * the interpreter's b ? a/b : 0 rule (ARMv2-v5 have no divide insn).
 * ---------------------------------------------------------------------- */
i32 relic_aot_ld8u(RelicVM *vm, i32 a) {
    u32 ua = (u32)a;
    if(ua >= vm->mem_sz) return 0;
    return (i32)(u32)vm->mem[ua];
}
i32 relic_aot_ld16u(RelicVM *vm, i32 a) {
    u32 ua = (u32)a;
    if(ua + 2 > vm->mem_sz) return 0;
    return (i32)(u32)((u32)vm->mem[ua] | ((u32)vm->mem[ua + 1] << 8));
}
i32 relic_aot_ld32u(RelicVM *vm, i32 a) {
    u32 ua = (u32)a;
    if(ua + 4 > vm->mem_sz) return 0;
    return (i32)(((u32)vm->mem[ua]) | ((u32)vm->mem[ua + 1] << 8)
        | ((u32)vm->mem[ua + 2] << 16) | ((u32)vm->mem[ua + 3] << 24));
}
void relic_aot_st8u(RelicVM *vm, i32 a, i32 v) {
    u32 ua = (u32)a;
    if(ua >= vm->mem_sz) return;
    vm->mem[ua] = (u8)v;
}
void relic_aot_st16u(RelicVM *vm, i32 a, i32 v) {
    u32 ua = (u32)a;
    if(ua + 2 > vm->mem_sz) return;
    vm->mem[ua]     = (u8)v;
    vm->mem[ua + 1] = (u8)(v >> 8);
}
void relic_aot_st32u(RelicVM *vm, i32 a, i32 v) {
    u32 ua = (u32)a;
    if(ua + 4 > vm->mem_sz) return;
    vm->mem[ua]     = (u8)v;
    vm->mem[ua + 1] = (u8)(v >> 8);
    vm->mem[ua + 2] = (u8)(v >> 16);
    vm->mem[ua + 3] = (u8)(v >> 24);
}
i32 relic_aot_div32(i32 a, i32 b) { return b ? a / b : 0; }
i32 relic_aot_mod32(i32 a, i32 b) { return b ? a % b : 0; }

/* -------------------------------------------------------------------------
 * Main interpreter loop
 * ---------------------------------------------------------------------- */
static u32 vm_trace_ring[256];
static u32 vm_trace_n = 0;
void vm_trace_dump(RelicVM *vm) {
    IRBuffer *ir = vm->ir;
    u32 n = vm_trace_n < 64 ? vm_trace_n : 64;
    fprintf(stderr, "vm: trace of last %u pcs (oldest first):\n", n);
    for(u32 i = 0; i < n; i++) {
        u32 pc = vm_trace_ring[(vm_trace_n - n + i) & 255];
        int fn = relic_pc_to_fn(ir, pc);
        int line = (pc < (u32)ir->len) ? IRLINE_RD(ir, pc) : -1;
        u8 op = (pc < (u32)ir->len) ? (u8)(IRCODE(ir, pc) >> 24) : 0xFF;
        char fnbuf[RELIC_MAX_IDENT];
        const char *nm = "?";
        if(fn >= 0 && fn < ir->nfuncs) {
            relic_ir_fn_name(ir, fn, fnbuf, sizeof fnbuf);
            nm = fnbuf;
        }
        fprintf(stderr, "  pc=%u fn=%d (%s) line=%d op=%s\n", pc, fn,
                nm, line, relic_opcode_name(op));
    }
    fflush(stderr);
}
void relic_vm_run(RelicVM *vm) {
    IRBuffer *ir = vm->ir;

    while(vm->running && vm->pc < (u32)ir->len) {
        /* Embedded-host preemption: one byte test per instruction.
         * The hook may switch fibers/tasks and return much later. */
        if(vm->yield_req) {
            vm->yield_req = 0;
            if(vm->yield_hook) vm->yield_hook(vm);
        }
        /* Debugger hook: fires before fetching the next instruction.
         * Cost when no debugger is attached is a single null-pointer test.
         * When attached, we additionally check single-step / breakpoint
         * bitmap, gated by step_fp_floor for step-over and step-out. */
        if(vm->dbg) {
            RelicDebugger *dbg = vm->dbg;
            bool fp_ok = (dbg->step_fp_floor == 0) || (vm->fp >= dbg->step_fp_floor);
            bool hit = false;
            if(fp_ok && dbg->single_step) hit = true;
            if(!hit && dbg->bp_bitmap) {
                u32 pc = vm->pc;
                int byte = (int)(pc >> 3);
                if(byte < dbg->bp_bitmap_bytes &&
                   (dbg->bp_bitmap[byte] & (1u << (pc & 7)))) {
                    hit = true;
                }
            }
            if(hit && dbg->on_break) {
                dbg->on_break(vm, dbg->user);
                if(!vm->running) break;
            }
        }
        vm_trace_ring[vm_trace_n++ & 255] = vm->pc;
        u32 instr = IRCODE(ir, vm->pc++);
        step_body(vm, instr);
    }
    if(vm->running && vm->pc >= (u32)ir->len) {
        fprintf(stderr, "vm: pc %u ran past end of code (%d words)\n",
                vm->pc, ir->len);
        vm_trace_dump(vm);
    }
}
