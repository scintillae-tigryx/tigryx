/*
 * tigryx.h
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
 * tigryx.h - Shared types, limits, and opcode constants for the Relic compiler/VM
 */
#ifndef TIGRYX_H
#define TIGRYX_H

#include <stdint.h>
#include <stddef.h>
/* Use compatibility header for MSVC 2012 which doesn't have stdbool.h */
#if defined(_MSC_VER) && _MSC_VER < 1800
#include "stdbool_compat.h"
#else
#include <stdbool.h>
#endif
#include <stdlib.h>
#include <stdio.h>   /* for FILE used in relic_bytecode_write */

/* Portable "unused function/variable" attribute.
 * GCC/Clang support __attribute__((unused)); MSVC does not, so we
 * expand to nothing there (MSVC's /W3 doesn't warn about unused
 * static functions the way GCC -Wall does). */
#if defined(__GNUC__) || defined(__clang__)
#define RELIC_UNUSED __attribute__((unused))
#else
#define RELIC_UNUSED
#endif

#define RELIC_VERSION "0.1.0"

/* Parse RELIC_VERSION into major, minor, patch integers at compile time.
 * Assumes format "MAJOR.MINOR.PATCH" where each is a single digit 0-9.
 * For runtime version checking of bytecode files. */
#define __RELIC_VERSION_MAJOR_CHAR (RELIC_VERSION[0] - '0')
#define __RELIC_VERSION_MINOR_CHAR (RELIC_VERSION[2] - '0')
#define __RELIC_VERSION_PATCH_CHAR (RELIC_VERSION[4] - '0')
#define RELIC_VERSION_MAJOR ((u32)__RELIC_VERSION_MAJOR_CHAR)
#define RELIC_VERSION_MINOR ((u32)__RELIC_VERSION_MINOR_CHAR)
#define RELIC_VERSION_PATCH ((u32)__RELIC_VERSION_PATCH_CHAR)

/* -------------------------------------------------------------------------
 * Primitive integer aliases
 * ---------------------------------------------------------------------- */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

/* -------------------------------------------------------------------------
 * 16-bit DOS (gcc-ia16) portability layer
 *
 * gcc-ia16 has no large/huge memory model: medium model gives far code
 * pointers but *near* (16-bit) data pointers, and __far pointer
 * arithmetic wraps at a 64K segment boundary - there is no huge-pointer
 * normalization.  Large objects therefore cannot be plain arrays.
 *
 * The approach used here: objects that can exceed 64K (guest VM memory,
 * IR code array, string pool) are allocated contiguously in real-mode
 * memory via _dos_allocmem and addressed through a RELIC_FAR pointer,
 * with every access normalizing the linear address back into seg:off
 * (relic_fp_add below).  Accessing fp[i] for i >= 64K would silently
 * wrap, so the access macros (VMMEM8/IRCODE/IRSTR) are mandatory on
 * 16-bit builds and no-ops elsewhere.
 * ---------------------------------------------------------------------- */
#if defined(__ia16__)
#  define RELIC_IA16 1
#endif

/* Both DOS toolchains share the same conventions - argv[0] carries the
 * exe path, 8.3 filenames, AUTOEXEC.BAT PATH edits, spawnlp for child
 * processes.  Shared core code branches on RELIC_DOS; toolchain-specific
 * differences (DJGPP's DPMI vs ia16 real mode) keep using __DJGPP__ /
 * RELIC_IA16 directly. */
#if defined(__DJGPP__) || defined(RELIC_IA16)
#  define RELIC_DOS 1
#endif

#ifdef RELIC_IA16
#  include <i86.h>
#  include <dos.h>
#  define RELIC_FAR __far
#else
#  define RELIC_FAR
#endif

/* Add a byte offset to a (possibly far) pointer with huge-pointer
 * semantics.  On ia16 this re-segments so any offset is reachable; on
 * flat-addressing hosts it is ordinary pointer arithmetic. */
static inline u8 RELIC_FAR *relic_fp_add(u8 RELIC_FAR *p, u32 off) {
#ifdef RELIC_IA16
    u32 lin = (u32)FP_OFF(p) + off;
    return MK_FP((u16)(FP_SEG(p) + (u16)(lin >> 4)), (u16)(lin & 15));
#else
    return p + off;
#endif
}
static inline const u8 RELIC_FAR *relic_cfp_add(const u8 RELIC_FAR *p, u32 off) {
#ifdef RELIC_IA16
    u32 lin = (u32)FP_OFF(p) + off;
    return MK_FP((u16)(FP_SEG(p) + (u16)(lin >> 4)), (u16)(lin & 15));
#else
    return p + off;
#endif
}

/* Guest-memory / IR accessors - lvalues on both paths. */
#define VMMEM8(vm, a)  (*relic_fp_add((u8 RELIC_FAR *)(vm)->mem, (u32)(a)))
#define IRCODE(ir, i)  (*(u32 RELIC_FAR *)relic_fp_add((u8 RELIC_FAR *)(ir)->code, (u32)(i) * 4u))
#define IRSTR(ir, o)   (*(char RELIC_FAR *)relic_fp_add((u8 RELIC_FAR *)(ir)->strings, (u32)(o)))
#define IRLINE(ir, i)  (*(i32 RELIC_FAR *)relic_fp_add((u8 RELIC_FAR *)(ir)->line_for_pc, (u32)(i) * 4u))
#define IRFILEID(ir,i) (*(i32 RELIC_FAR *)relic_fp_add((u8 RELIC_FAR *)(ir)->file_for_pc, (u32)(i) * 4u))
/* Read-side: line_for_pc/file_for_pc may be NULL on ia16 (lazy debug
 * tables).  IRLINE -> 0, IRFILEID -> -1, the same "unknown" sentinels. */
#define IRLINE_RD(ir, i)   ((ir)->line_for_pc ? IRLINE(ir, i)   : 0)
#define IRFILEID_RD(ir, i) ((ir)->file_for_pc ? IRFILEID(ir, i) : -1)

/* Far C-string accessors for the parallel function/source-file name
 * tables (evaluate to 'char RELIC_FAR *'). */
#define IRFNNAME(ir, i)  ((char RELIC_FAR *)relic_fp_add((u8 RELIC_FAR *)(ir)->fn_names, (u32)(i) * RELIC_MAX_IDENT))
#define IRSRCFILE(ir, i) ((char RELIC_FAR *)relic_fp_add((u8 RELIC_FAR *)(ir)->source_files, (u32)(i) * RELIC_MAX_SOURCE_PATH))
#define IRSTRP(ir, o)    ((char RELIC_FAR *)relic_fp_add((u8 RELIC_FAR *)(ir)->strings, (u32)(o)))

/* Element lvalues for the function tables (ia16: far-allocated, so plain
 * array indexing would wrap at a 64K boundary; flat hosts: identical to
 * ir->fn_offsets[i] / ir->fn_argmask[i]). */
#define IRFNOFF(ir, i)   (*(i32 RELIC_FAR *)relic_fp_add((u8 RELIC_FAR *)(ir)->fn_offsets, (u32)(i) * 4u))
#define IRFNAMASK(ir, i) (*(u16 RELIC_FAR *)relic_fp_add((u8 RELIC_FAR *)(ir)->fn_argmask, (u32)(i) * 2u))

/* Lexer file table entry (ia16: far-allocated like IRBuffer::source_files). */
#define LXFILE(lx, i)    ((char RELIC_FAR *)relic_fp_add((u8 RELIC_FAR *)(lx)->files, (u32)(i) * RELIC_MAX_SOURCE_PATH))

/* Far heap helpers (DOS allocmem on ia16, malloc elsewhere). */
void RELIC_FAR *relic_far_alloc(u32 bytes);
void  relic_far_free(void RELIC_FAR *p);
/* Largest single block allocatable via relic_far_alloc right now
 * (ia16: largest DOS memory block; flat hosts: 0xFFFFFFFF). */
u32   relic_far_avail(void);
/* block copy with 64K segment-boundary safety.  On ia16 the second
 * operand may be near (implicitly widened via DS) or far; elsewhere
 * these are plain memcpy. */
void  relic_far_write(u8 RELIC_FAR *dst, u32 off, const void RELIC_FAR *src, u32 n);
void  relic_far_read(const u8 RELIC_FAR *src, u32 off, void RELIC_FAR *dst, u32 n);
/* memmove where both operands are byte offsets inside one far object. */
void  relic_far_move(u8 RELIC_FAR *base, u32 dst, u32 src, u32 n);
/* memset into a (possibly far) object at a byte offset. */
void  relic_far_set(u8 RELIC_FAR *dst, u32 off, int c, u32 n);
/* fread/fwrite into/from a far object through a near bounce buffer -
 * libc stdio only takes near pointers on ia16. */
size_t relic_fread_far(FILE *f, u8 RELIC_FAR *dst, u32 n);
size_t relic_fwrite_far(FILE *f, const u8 RELIC_FAR *src, u32 n);
/* Copy a (possibly far) NUL-terminated string into a near buffer -
 * libc string functions cannot take far pointers on ia16. */
size_t relic_far_str(char *dst, const char RELIC_FAR *src, size_t cap);

/* -------------------------------------------------------------------------
 * Little-endian file-format helpers
 *
 * All on-disk binary formats (.tlx bytecode, the packed-asset trailer and
 * the bind footer) are little-endian regardless of host byte order. These
 * helpers assemble/disassemble values a byte at a time, so they are correct
 * on both little- and big-endian hosts (x86/ARM and SPARC/PPC) with no
 * compile-time switching - a decent compiler folds them into a single
 * load/store on little-endian anyway.
 * ---------------------------------------------------------------------- */
static inline u16 relic_rd16le(const void *p) {
    const u8 *b = (const u8 *)p;
    return (u16)((u16)b[0] | ((u16)b[1] << 8));
}
static inline u32 relic_rd32le(const void *p) {
    const u8 *b = (const u8 *)p;
    return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
}
static inline u64 relic_rd64le(const void *p) {
    const u8 *b = (const u8 *)p;
    return (u64)relic_rd32le(b) | ((u64)relic_rd32le(b + 4) << 32);
}
static inline void relic_wr16le(void *p, u16 v) {
    u8 *b = (u8 *)p;
    b[0] = (u8)(v & 0xFFu); b[1] = (u8)(v >> 8);
}
static inline void relic_wr32le(void *p, u32 v) {
    u8 *b = (u8 *)p;
    b[0] = (u8)(v & 0xFFu);        b[1] = (u8)((v >> 8)  & 0xFFu);
    b[2] = (u8)((v >> 16) & 0xFFu); b[3] = (u8)((v >> 24) & 0xFFu);
}
static inline void relic_wr64le(void *p, u64 v) {
    u8 *b = (u8 *)p;
    relic_wr32le(b, (u32)v);
    relic_wr32le(b + 4, (u32)(v >> 32));
}

#if defined(__sun)
#include <unistd.h>
#include <string.h>
/* Solaris has no /proc/self/exe. /proc/self/path/a.out is the equivalent
 * absolute-path symlink but exists only on Solaris 8+; on Solaris 7 use
 * getexecname() (libc since SunOS 5.7), which can return a relative or
 * PATH-resolved name, so run it through realpath() when possible.
 * Returns 1 and a NUL-terminated path on success, 0 otherwise. */
static inline int relic_sun_exepath(char *out, size_t out_sz) {
    if(!out || out_sz == 0) return 0;
    ssize_t n = readlink("/proc/self/path/a.out", out, out_sz - 1);
    if(n > 0) { out[n] = '\0'; return 1; }
    const char *e = getexecname();
    if(!e || !*e) return 0;
    if(!realpath(e, out)) {
        size_t l = strlen(e);
        if(l >= out_sz) l = out_sz - 1;
        memcpy(out, e, l); out[l] = '\0';
    }
    return out[0] != '\0';
}
#endif

/* -------------------------------------------------------------------------
 * tigryxcc target ISA level (--cpu)
 *
 * Unlike .tlx bytecode (portable, interpreted anywhere), a tigryxcc .exe
 * is machine-bound, so the ISA level is a legitimate compile-time knob.
 * SSE2 is the x64 architectural floor; SSE4.1 adds pmulld/ptest/pblendvb
 * (32-bit packed multiply for i32 vector lanes); AVX2 gives 8-wide lanes
 * and gathers.
 * ---------------------------------------------------------------------- */
#define RELIC_AOT_CPU_SSE2   0   /* default: x64 baseline */
#define RELIC_AOT_CPU_SSE41  1   /* pmulld, ptest, pblendvb, ... */
#define RELIC_AOT_CPU_AVX2   2   /* 8-wide ymm ops, gathers      */

/* -------------------------------------------------------------------------
 * Limits
 * ---------------------------------------------------------------------- */
#define RELIC_MAX_IDENT     64

/* RELIC_MAX_TOKENS / RELIC_MAX_NODES / RELIC_MAX_IR / RELIC_MAX_TYPES are
 * vestigial — the lexer, parser, IR buffer, and type table all grow
 * dynamically via realloc. They are kept here only for reference and any
 * external code that might still reference them. */
#define RELIC_MAX_TOKENS    65536
#define RELIC_MAX_NODES     65536
#define RELIC_MAX_TYPES     1024
#define RELIC_MAX_IR        131072

/* Static-array limits for the compiler's SymTable and IRBuffer. These
 * arrays are baked into the structs and allocated in full on every
 * compilation, so their size is a fixed memory cost.
 *
 * RELIC_LOW_MEMORY is defined by CMake for memory-constrained platforms
 * (currently DOS). It keeps the arrays at their original sizes so the
 * compiler tables fit in limited memory. Modern platforms (Windows,
 * Linux, macOS) use larger defaults so large programs compile without
 * hitting the caps.
 *
 * Each limit can still be overridden individually via -D on the command
 * line if a platform needs a value between the two defaults. */
#ifdef RELIC_IA16
   /* 16-bit DOS: arrays must stay <64K each (one segment), and the big
    * IRBuffer pools are far-allocated pointers instead of inline arrays,
    * so DGROUP pressure stays small. */
#  ifndef RELIC_MAX_SYMBOLS
#    define RELIC_MAX_SYMBOLS   192       /* SymTable ~17K; TypeChecker
                                         * total must stay <32K */
#  endif
#  ifndef RELIC_MAX_STRUCTS
#    define RELIC_MAX_STRUCTS   64
#  endif
#  ifndef RELIC_MAX_FUNCS
#    define RELIC_MAX_FUNCS     512
#  endif
#  ifndef RELIC_MAX_STRINGS
#    define RELIC_MAX_STRINGS   16384     /* string pool cap: 16 KB far */
#  endif
#  ifndef RELIC_MAX_SOURCE_FILES
#    define RELIC_MAX_SOURCE_FILES 16
#  endif
   /* entries are inline in RelicAssets (near heap on ia16): 16 * 264B
    * keeps the table ~4K instead of ~17K. */
#  define RELIC_MAX_ASSETS     16
#elif defined(RELIC_LOW_MEMORY)
#  ifndef RELIC_MAX_SYMBOLS
#    define RELIC_MAX_SYMBOLS   4096      /* SymTable: ~384 KB */
#  endif
#  ifndef RELIC_MAX_FUNCS
     /* DJGPP builds run under a DPMI host with megabytes available, so
      * the function table matches the desktop default: large programs
      * (TIDE ~1800 fns) must at least LOAD as .tlx on DOS. Symbols and
      * structs stay small - full on-device compiles are not the goal. */
#    define RELIC_MAX_FUNCS     8192      /* fn_offsets+fn_names: ~544 KB */
#  endif
#  ifndef RELIC_MAX_STRINGS
#    define RELIC_MAX_STRINGS   65536     /* string pool: 64 KB */
#  endif
#  ifndef RELIC_MAX_SOURCE_FILES
#    define RELIC_MAX_SOURCE_FILES 64     /* source_files: 32 KB */
#  endif
#  ifndef RELIC_MAX_STRUCTS
#    define RELIC_MAX_STRUCTS   256
#  endif
#else
#  ifndef RELIC_MAX_SYMBOLS
#    define RELIC_MAX_SYMBOLS   16384     /* SymTable: ~1.5 MB */
#  endif
#  ifndef RELIC_MAX_FUNCS
#    define RELIC_MAX_FUNCS     8192      /* fn_offsets+fn_names: ~544 KB */
#  endif
#  ifndef RELIC_MAX_STRINGS
#    define RELIC_MAX_STRINGS   262144    /* string pool: 256 KB */
#  endif
#  ifndef RELIC_MAX_SOURCE_FILES
#    define RELIC_MAX_SOURCE_FILES 512    /* source_files: 256 KB */
#  endif
#  ifndef RELIC_MAX_STRUCTS
#    define RELIC_MAX_STRUCTS   512
#  endif
#endif

#ifndef RELIC_MEM_SIZE
#ifdef RELIC_IA16
#define RELIC_MEM_SIZE      (320uL * 1024)   /* real mode: fits in ~640K */
#else
#define RELIC_MEM_SIZE      (16 * 1024 * 1024)  /* default VM memory: 16 MB */
#endif
#endif
/* Default VM stack: 256 KB. Was 32 KB, which programs with several
 * struct-by-value locals (e.g. a handful of ~1-2 KB structs plus their
 * anonymous call-result staging slots - frame_alloc never reclaims those
 * within a function) could exceed without any error, silently corrupting
 * the heap that sits just below the stack (see the OP_ENTER guard in
 * vm.c, which now catches this and traps cleanly instead). 256 KB gives
 * realistic TIDE-style model/test code headroom while remaining a small
 * fraction of the default 16 MB VM memory. */
#ifdef RELIC_IA16
/* 320 KB default guest memory cannot afford a 256 KB stack reserve. */
#define RELIC_STACK_SIZE    (32uL * 1024)
#elif !defined(RELIC_STACK_SIZE)
#define RELIC_STACK_SIZE    (256uL * 1024)
#endif

/* Staging regions the transpiled-C runtime shim carves out of vm->mem.
 * Declared here (rather than only in tigryx_rc_shim.c) so the minimum
 * viable heap can be computed without the two copies drifting apart. */
#define RELIC_PLATFORM_STR_SIZE (1024u)          /* sys_platform scratch  */
#define RELIC_RC_SCRATCH_SIZE   (4096u)          /* arg marshalling       */
#define RELIC_RC_BLOB_SIZE      (1024u * 1024u)  /* bulk I/O bounce buffer*/

/* Headroom left for @alloc when the minimum heap is requested
 * (`--heap 0` / `#pragma arg heap 0`). A true minimum would make the
 * first @alloc fail, so keep a small usable margin. */
#define RELIC_HEAP_AUTO_MARGIN  (256u * 1024u)

/* Minimum viable VM memory for the transpiled-C (tigryxrc) path, where
 * globals are native C statics and @alloc is native malloc, so only the
 * staging regions and the stack have to fit. ~1.3 MB. */
#define RELIC_RC_MIN_MEM_SIZE   (RELIC_PLATFORM_STR_SIZE + \
                                 RELIC_RC_SCRATCH_SIZE   + \
                                 RELIC_RC_BLOB_SIZE      + \
                                 (u32)RELIC_STACK_SIZE   + \
                                 RELIC_HEAP_AUTO_MARGIN)
#define RELIC_MAX_ERRORS    32

/* Per-compile file table (lexer's `#import`-discovered files, and the
 * matching table on IRBuffer for PC->file attribution - see tigryx.h's
 * Lexer/IRBuffer below). A handful of #import targets is typical; this is
 * a soft cap, not a hard error - lexer_intern_file() saturates rather than
 * failing the compile if it's ever exceeded. Defined in the platform-split
 * block above (64 on RELIC_LOW_MEMORY, 512 otherwise). */
#define RELIC_MAX_SOURCE_PATH  512

/* Bound-executable footer format.
 * Layout: [ runtime exe ][ payload (.b + assets) ][ footer ]
 * Footer (32 bytes):
 *      char     magic[8] = "RLCBIND\x02"
 *      uint64_t payload_offset    (LE)
 *      uint64_t payload_size      (LE)
 *      uint32_t heap_bytes        (LE, 0 = use compiled-in default)
 *      uint32_t flags             (LE, bit 0 = RELIC_BIND_FLAG_NO_CONSOLE)
 * The magic's last byte is a format version tag. */
#define RELIC_BIND_FLAG_NO_CONSOLE 0x1u
#define RELIC_BIND_MAGIC       "RLCBIND\x02"
#define RELIC_BIND_MAGIC_SIZE  8
#define RELIC_BIND_FOOTER_SIZE 32

/* Sentinel returned by relic_parse_heap_size() for an explicit size of 0,
 * meaning "use the smallest heap this program can run in". The caller
 * resolves it, because the true minimum differs per execution path. */
#define RELIC_HEAP_AUTO  0xFFFFFFFFu

/* Parse a human-readable memory size string.  Accepts an integer followed
 * by an optional unit: k/K (kilobytes), m/M (megabytes), g/G (gigabytes).
 * If no unit is given, megabytes are assumed.  Returns RELIC_MEM_SIZE
 * on any parse error so callers always get a usable value. */
static inline u32 relic_parse_mem_size(const char *s) {
    char *end = NULL;
    unsigned long val = strtoul(s, &end, 10);
    if (end == s || val == 0) return RELIC_MEM_SIZE;

    u64 mult = 1;
    switch (*end) {
        case 'k': case 'K': mult = 1024ULL; break;
        case 'm': case 'M': mult = 1024ULL * 1024; break;
        case 'g': case 'G': mult = 1024ULL * 1024 * 1024; break;
        case '\0':          mult = 1024ULL * 1024; break; /* default M */
        default: return RELIC_MEM_SIZE;
    }

    if (val > UINT32_MAX / mult) return UINT32_MAX;
    return (u32)(val * mult);
}

/* Parse a heap size exactly like relic_parse_mem_size(), except that an
 * explicit "0" yields RELIC_HEAP_AUTO instead of the default, so callers
 * can distinguish "smallest that works" from "unset". Callers that cannot
 * resolve a minimum should fall back to RELIC_MEM_SIZE. */
static inline u32 relic_parse_heap_size(const char *s) {
    char *end = NULL;
    unsigned long val = strtoul(s, &end, 10);
    if (end != s && val == 0) return RELIC_HEAP_AUTO;
    return relic_parse_mem_size(s);
}

#if defined(RELIC_DOS)
/* DOS-only: optional runtime override for the backend screen colour depth.
 * Defined in src/core/vm.c. Set by mains when --colour-depth N is given
 * on the command line. Both DOS backends (allegro-dos and allegro-tinygl-dos)
 * honour this flag. The custom rasteriser in allegro-dos is optimized for
 * 8 and 16 bpp; other depths work via Allegro's converters. Zero means
 * "use default ladder". Gated to DJGPP so non-DOS backends are not polluted.
 * See src/backends/allegro-dos/ and allegro-tinygl-dos/. */
extern int g_relic_preferred_color_depth;

/* DOS-only: --res <WxH> picks the physical graphics mode when a program
 * opts into WINFLAG_STRETCH (the logical buffer is stretched onto it).
 * Ignored without WINFLAG_STRETCH: a non-stretch window requires its
 * exact snapped mode and exits if the card cannot set it. Zero means
 * "use the request-snapped ladder". Gated to DJGPP like --colour-depth. */
extern int g_relic_stretch_mode_w;
extern int g_relic_stretch_mode_h;

/* Parse a "<W>x<H>" resolution string ("640x480", case-insensitive x).
 * Returns 1 and fills w/h on success, 0 otherwise. */
static inline int relic_parse_mode_res(const char *s, int *w, int *h) {
    if (!s || !w || !h) return 0;
    int tw = 0, th = 0;
    const char *p = s;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') { tw = tw * 10 + (*p - '0'); p++; }
    if (*p != 'x' && *p != 'X') return 0;
    p++;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') { th = th * 10 + (*p - '0'); p++; }
    if (*p != '\0' || tw <= 0 || th <= 0) return 0;
    *w = tw; *h = th;
    return 1;
}

/* DOS-only: parse and validate a --colour-depth argument. Accepts "8",
 * "16", "24", or "32"; anything else returns 0. */
static inline int relic_parse_colour_depth(const char *s) {
    if (!s) return 0;
    if (s[0] == '8'  && s[1] == '\0') return 8;
    if (s[0] == '1'  && s[1] == '6' && s[2] == '\0') return 16;
    if (s[0] == '2'  && s[1] == '4' && s[2] == '\0') return 24;
    if (s[0] == '3'  && s[1] == '2' && s[2] == '\0') return 32;
    return 0;
}

/* DOS-only: opt-in to z-buffered polytype rendering in allegro-dos. See
 * src/core/vm.c for the definition and the comment above gfx_mesh in
 * src/backends/allegro-dos/backend_allegro_dos.c for the rationale. */
extern int g_relic_allegro_zbuf;

/* DOS-only: opt-in to per-frame stats logging for the --allegro-zbuf
 * path. When set, allegro_gfx_flip writes one line per second to
 * relic.log summarising tris submitted, near-plane rejects, and
 * scene_polygon3d_f overflow fallbacks. Diagnostic only. */
extern int g_relic_allegro_zbuf_stats;

/* DOS-only: opt-in to skipping the per-frame colour-buffer clear in
 * the allegro-tinygl-dos and allegro-dos backends. Set by `--skip-clear`.
 * Saves one full-screen memset (~960 KB at 800x600x16) per frame for
 * programs whose 3D scene fully covers the framebuffer (e.g. tdoom).
 * The depth clear is always preserved; only the colour clear is skipped.
 * See src/core/vm.c for the definition. */
extern int g_relic_skip_clear;

/* DOS-only: opt-in 3D rendering profiler. Set by `--gfx-prof`.
 * When set, the allegro-dos backend accumulates per-frame counters
 * (mesh calls, vertices transformed, triangles considered/rasterised,
 * scanlines, span pixels, pixels written) and dumps them to TIGRYX.LOG
 * at shutdown.  Zero overhead when the flag is absent.
 * See src/core/vm.c for the definition. */
extern int g_relic_gfx_prof;

/* DOS-only: opt-in to wireframe rendering in allegro-tinygl-dos. Set by
 * `--wireframe`. When enabled, glPolygonMode(GL_FRONT_AND_BACK, GL_LINE)
 * is called after glInit, so all @mesh calls render as outlines instead
 * of filled triangles. The mesh base colour is applied to the wireframe
 * lines. See src/core/vm.c for the definition. */
extern int g_relic_wireframe;

/* DOS-only: opt-in to back-face culling in allegro-tinygl-dos. Set by
 * `--cull-backface`. When enabled, glEnable(GL_CULL_FACE) + glCullFace(GL_BACK)
 * is called after glInit. This can nearly double performance for closed
 * geometry (sector-style geometry) by rejecting back-facing triangles before
 * the rasteriser. If the mesh winding is wrong (CW vs CCW), the scene
 * will render incorrectly — use this flag only after verifying geometry.
 * See src/core/vm.c for the definition. */
extern int g_relic_cull_backface;
#endif

#if defined(RELIC_RISCOS)
/* RISC OS-only: force every @window call into the fullscreen games-mode
 * path. Set by `--fullscreen`; consumed by the vidc-riscos backend, which
 * ORs it into the window flags so test programs written for windowed use
 * can also be exercised fullscreen without editing. Defined in
 * src/core/vm.c. */
extern int g_relic_fullscreen;

/* Clear the FPA trap-enable bits (IOE DZE OFE UFE IXE, FPSR bits 20:16)
 * so IEEE ops like 1.0/0.0 return Inf/NaN instead of killing the task
 * with "Floating point exception". No-op on non-FPA builds. Call once at
 * main() entry, before any floating point executes.
 * platforms/riscos-vidc-gccsdk-arm3/riscos_fpe.c */
void relic_fpe_untrap(void);
#endif

/* -------------------------------------------------------------------------
 * IR Opcodes
 * ---------------------------------------------------------------------- */
typedef enum {
    /* Load / Store */
    OP_LDC    = 0x01,
    OP_LDC32  = 0x02,
    OP_LD8    = 0x03,
    OP_LD16   = 0x04,
    OP_LD32   = 0x05,
    OP_ST8    = 0x06,
    OP_ST16   = 0x07,
    OP_ST32   = 0x08,
    OP_LEA    = 0x09,
    OP_MOV    = 0x0A,

    /* Arithmetic */
    OP_ADD    = 0x10,
    OP_ADDI   = 0x11,
    OP_SUB    = 0x12,
    OP_SUBI   = 0x13,
    OP_MUL    = 0x14,
    OP_DIV    = 0x15,
    OP_MOD    = 0x16,
    OP_NEG    = 0x17,

    /* Floating point (single precision) */
    OP_FADD   = 0x18,
    OP_FSUB   = 0x19,
    OP_FMUL   = 0x1A,
    OP_FDIV   = 0x1B,
    OP_FNEG   = 0x1C,
    /* f64 arithmetic is at 0x44-0x48 - see "f64 opcode map" below. It could
     * not live at 0x1D-0x1F because only three slots are free here. */

    /* Bitwise / Shift */
    OP_AND    = 0x20,
    OP_OR     = 0x21,
    OP_XOR    = 0x22,
    OP_NOT    = 0x23,
    OP_SHL    = 0x24,
    OP_SHR    = 0x25,
    OP_SAR    = 0x26,
    OP_ANDI   = 0x27,
    OP_ORI    = 0x28,

    /* Comparison */
    OP_CEQ    = 0x30,
    OP_CNE    = 0x31,
    OP_CLT    = 0x32,
    OP_CLE    = 0x33,
    OP_CGT    = 0x34,
    OP_CGE    = 0x35,
    OP_CLTU   = 0x36,
    OP_CLEU   = 0x37,

    /* Floating point comparisons (only ==, <, <= are primitive;
     * !=, >, >= are synthesised by the emitter) */
    OP_FCEQ   = 0x38,
    OP_FCLT   = 0x39,
    OP_FCLE   = 0x3A,

    /* f64 comparisons. Operands are d-bank registers; the result is 0/1
     * written to an *i32* register, exactly like the f32 forms above.
     * !=, >, >= are synthesised by the emitter. */
    OP_DCEQ   = 0x3B,
    OP_DCLT   = 0x3C,
    OP_DCLE   = 0x3D,

    /* Branches / Jumps */
    OP_JMP    = 0x40,
    OP_JT     = 0x41,
    OP_JF     = 0x42,
    OP_JMPR   = 0x43,

    /* =====================================================================
     * f64 opcode map
     * ---------------------------------------------------------------------
     * f64 values live in a SEPARATE 16-register bank d0..d15 (RelicVM.d),
     * not in the i32 register file. The 4-bit dst/a/b instruction fields
     * address that bank, so the instruction word format is unchanged.
     *
     * The set is split to keep each group adjacent to its f32 counterpart,
     * following this file's existing organisation. Nothing in a backend
     * needs the groups to be contiguous.
     *
     *   0x3B-0x3D  compare       (beside OP_FCEQ..OP_FCLE)
     *   0x44-0x4F  core          arithmetic, move, memory, bit access
     *   0x56-0x5F  math part 1   (beside the OP_SYS_* math block in spirit)
     *   0x68-0x6D  convert       (beside OP_I2F / OP_F2I)
     *   0x74-0x7D  math part 2
     *
     * 41 opcodes total; 45 of the original 86 free slots remain.
     * ===================================================================== */

    /* --- f64 core: arithmetic (mirrors OP_FADD..OP_FNEG) --- */
    OP_DADD   = 0x44,
    OP_DSUB   = 0x45,
    OP_DMUL   = 0x46,
    OP_DDIV   = 0x47,
    OP_DNEG   = 0x48,

    /* --- f64 core: move and memory --- */
    OP_DMOV   = 0x49,       /* d[dst] = d[a] */
    OP_DLD    = 0x4A,       /* d[dst] = f64 at mem[r[a] + imm12] */
    OP_DST    = 0x4B,       /* f64 at mem[r[a] + imm12] = d[dst] */
    /* OP_DLDC: load a 64-bit constant. The literal follows the instruction
     * as TWO payload words, low half first (little-endian on disk, like
     * every other multi-byte .tlx field - never memcpy a double, SPARC is
     * big-endian). Decode advances pc by 3 words total.
     *
     * This mirrors OP_LDC32 (0x02), which already consumes one trailing
     * word, so the precedent for a variable-length instruction exists.
     * Consequence to respect in EVERY instruction walker - backends,
     * disassembler, and the RLE line/file debug maps: the two payload
     * words are data, not instructions, and must be skipped. */
    OP_DLDC   = 0x4C,

    /* --- f64 bit access: RESERVED, implementation DEFERRED ---
     * The language has no i64, so a double's bit pattern must move through
     * two 32-bit halves. These slots are reserved so the numbering never
     * has to shift, but they are deliberately NOT implemented yet.
     *
     * Reason: Tigryx already expresses bit reinterpretation as @memcpy over
     * addresses.
     * Taking &x of an f64 local forces it to memory, so OP_DLD / OP_DST plus
     * @memcpy already covers serialisation, hashing and .mat I/O with no new
     * opcodes. Implementing these would cost a case in the interpreter and
     * all seven AOT backends to buy only the saving of a store/load pair.
     *
     * Implement only if profiling shows the memory round-trip matters. */
    OP_DBITS_LO  = 0x4D,    /* r[dst] = low  32 bits of d[a] */
    OP_DBITS_HI  = 0x4E,    /* r[dst] = high 32 bits of d[a] */
    OP_DFROMBITS = 0x4F,    /* d[dst] = bits(hi = r[a], lo = r[b]) */

    /* Calls / Return */
    OP_CALL   = 0x50,
    OP_CALLR  = 0x51,
    OP_RET    = 0x52,
    OP_ENTER  = 0x53,       /* sp -= uimm20 * 4 (frame size in words) */
    OP_LEAVE  = 0x54,
    OP_CALLP  = 0x55,

    /* --- f64 math, part 1 (mirrors the f32 builtins one-for-one) ---
     * Unlike the f32 SYS_* math ops these are NOT in op_sym_name and use
     * no backend hook: they are plain register ops implemented inline in
     * step_body(), so JITs and AOTs take the relic_vm_step1 fallback
     * until they grow native lowering. */
    OP_DSQRT  = 0x56,
    OP_DSIN   = 0x57,
    OP_DCOS   = 0x58,
    OP_DTAN   = 0x59,
    OP_DASIN  = 0x5A,
    OP_DACOS  = 0x5B,
    OP_DATAN  = 0x5C,
    OP_DATAN2 = 0x5D,       /* d[dst] = atan2(d[a], d[b]) */
    OP_DEXP   = 0x5E,
    OP_DLOG   = 0x5F,

    /* Type Conversion */
    OP_SX8    = 0x60,
    OP_SX16   = 0x61,
    OP_ZX8    = 0x62,
    OP_ZX16   = 0x63,
    OP_TRUNC8 = 0x64,
    OP_TRUNC16= 0x65,
    OP_I2F    = 0x66,       /* dst = (float)(i32)a */
    OP_F2I    = 0x67,       /* dst = (i32)(float)a (truncate toward zero) */

    /* --- f64 conversion (mirrors OP_I2F / OP_F2I) ---
     * Cross-bank: the i32 forms read/write r[], the f32 forms read/write
     * r[] (f32 is a bit pattern in an i32 register), and d[] is the other
     * side in every case. */
    OP_I2D    = 0x68,       /* d[dst] = (double)(i32)r[a] */
    OP_D2I    = 0x69,       /* r[dst] = (i32)d[a]  (truncate toward zero) */
    OP_F2D    = 0x6A,       /* d[dst] = (double)asfloat(r[a])  - always exact */
    OP_D2F    = 0x6B,       /* r[dst] = asbits((float)d[a])  - nearest, ties even */
    OP_U2D    = 0x6C,       /* d[dst] = (double)(u32)r[a] */
    OP_D2U    = 0x6D,       /* r[dst] = (u32)d[a]  (truncate toward zero) */

    /* Stack / Misc */
    OP_PUSH   = 0x70,
    OP_POP    = 0x71,
    OP_NOP    = 0x72,
    OP_HALT   = 0x73,

    /* --- f64 math, part 2 (continues the 0x56-0x5F block) --- */
    OP_DLOG10 = 0x74,
    OP_DPOW   = 0x75,       /* d[dst] = pow(d[a], d[b]) */
    OP_DFLOOR = 0x76,
    OP_DCEIL  = 0x77,
    OP_DROUND = 0x78,       /* round-half-away, matching OP_SYS_ROUND */
    OP_DFMOD  = 0x79,       /* d[dst] = fmod(d[a], d[b]) */
    OP_DFABS  = 0x7A,
    OP_DSINH  = 0x7B,
    OP_DCOSH  = 0x7C,
    OP_DTANH  = 0x7D,

    /* Graphics */
    OP_GFX_CLEAR  = 0x81,
    /* @draw_mode(mode) -> i32 - primitive blend mode for subsequent
     * 2D draws: 0 = copy (normal), 1 = xor (invert). Returns 1 if the
     * backend honoured the mode, 0 if it has no draw-mode support
     * (the vtable slot is NULL), so a program can fall back to
     * clear-and-redraw. Backend list it affects is backend-defined,
     * but @line/@plot are the minimum. XOR exists for 8-bit/limited
     * targets: drawing a line twice in XOR mode erases it, so a moving
     * wireframe redraws without clearing the screen. NULL -> op
     * returns 0 and is otherwise a no-op. */
    OP_GFX_DRAW_MODE = 0x82,
    /* @sprite(x, y, tex, src_x, src_y, src_w, src_h, dst_w, dst_h, angle,
     * &rgba) - draws a textured, rotatable, tinted quad.
     * (src_x,src_y,src_w,src_h) crop the source texture in its own pixel
     * space (src_w/src_h <= 0 means "whole texture"). (dst_w,dst_h) is the
     * on-screen quad size in pixels, independent of the source crop size
     * (dst_w/dst_h <= 0 means "same as src_w/src_h", i.e. 1:1 / stretched-
     * to-src-rect like a plain blit). This decoupling lets a texture be
     * sampled at its native resolution while being drawn at a different
     * logical size - e.g. an HD-upscaled sprite can be drawn at the same
     * on-screen size as its original low-res version without the source
     * crop rect being wrong for the higher-resolution texture. */
    OP_GFX_SPRITE = 0x83,
    OP_GFX_PLOT   = 0x84,
    OP_GFX_FLIP   = 0x85,
    OP_GFX_LINE   = 0x86,
    OP_GFX_WINDOW = 0x87,
    OP_GFX_POLYGON = 0x88,
    OP_GFX_POLYLINE = 0x89,
    OP_GFX_CAMERA  = 0x8A,
    OP_GFX_TEXTURE = 0x8B,
    OP_GFX_LIGHT   = 0x8C,
    OP_GFX_MESH    = 0x8D,
    OP_GFX_TEXT    = 0x8E,
    OP_GFX_VIEWPORT = 0x8F,
    OP_GFX_TEXT_MEASURE = 0x90, /* @text_measure(text,size,weight,font,out: *TextMetrics) */
    OP_GFX_CLIP    = 0x91,      /* @clip(x,y,w,h) - 2D scissor; w<=0||h<=0 disables */
    OP_GFX_ELLIPSE        = 0x92, /* @ellipse(x,y,w,h,stroke_width,stroke_col,start,finish) */
    OP_GFX_ELLIPSE_FILLED = 0x93, /* @ellipse_filled(x,y,w,h,stroke_width,stroke_col,start,finish,fill_col) */
    OP_GFX_ROUNDED_RECT        = 0x94, /* @rounded_rectangle(x1,y1,x2,y2,stroke_width,stroke_col,radius) */
    OP_GFX_ROUNDED_RECT_FILLED = 0x95, /* @rounded_rectangle_filled(x1,y1,x2,y2,stroke_width,stroke_col,radius,fill_col,elevation) */
    /* @palette_upload(ptr_rgb888_or_0, count) - 8bpp-only palette selection.
     * count==0 -> flat/general-use RGB palette (ptr ignored); count>0 ->
     * exact RGB888 palette of `count` entries (<=256) copied from ptr.
     * Not calling this at all keeps the default: auto texture-sampled
     * median-cut palette. No-op on true-colour backends / non-8bpp modes. */
    OP_GFX_PALETTE_UPLOAD      = 0x96,

    /* -----------------------------------------------------------------
     * Sector-based world renderer (allegro-fixed-dos backend only).
     *
     * Unlike @mesh/@wall/@flat, these are NOT general-purpose primitives
     * with a portable fallback: they render a sector-map world (BSP map,
     * column-major wall textures, 64x64 flats, colormap lighting) the way
     * the classic engine does - no z-buffer, no perspective divide, no per-pixel
     * float math anywhere - which only makes sense as a single backend
     * owning the whole frame. A script using them is inherently tied to
     * that backend, the same way a script using Metal-specific calls would
     * be tied to iOS. Every op below is null-checked and silently a no-op
     * on any other backend, same as other backend-specific calls. */
    OP_WR_PALETTE = 0x97,   /* @wr_palette(playpal_rgb888, colormap, num_colormaps) */
    OP_WR_TEXTURE = 0x98,   /* @wr_texture(id, width, height, column_major_data) */
    OP_WR_FLAT    = 0x99,   /* @wr_flat(id, data_64x64_row_major) */
    OP_WR_TEST    = 0x9A,   /* @wr_test(tex_id, flat_id, light_level) - diagnostic:
                             * draws one DrawColumn strip + one DrawSpan row with no
                             * map/BSP involved, to prove the pixel loops and
                             * palette/colormap lighting before wiring up the BSP walk. */
    /* @wr_map(header_ptr) - hands the backend the whole BSP map in one
     * call. header_ptr points at a tightly-packed 56-byte header (7 x
     * {u32 array_ptr, i32 count}, in this order: vertices, linedefs,
     * sidedefs, sectors, segs, subsectors, nodes) so the call fits in one
     * register regardless of how many arrays a WAD map has (a flat
     * ptr+count per array would need 14 registers, over the 13-register
     * builtin-call limit). Each array must already be in the raw WAD
     * on-disk layout (Vertex/Linedef/Sidedef/Sector/Seg/Subsector/Node,
     * as e.g. samples/adoom's wad.tgx already loads them) - the backend
     * copies all of it into its own memory, so the caller's buffers can
     * be freed/reused immediately after the call returns. Replaces any
     * previously uploaded map. */
    OP_WR_MAP     = 0x9B,
    /* @wr_render(x, y, z, angle, fov) - renders the whole visible world
     * from this viewpoint into the backbuffer: BSP front-to-back with a
     * solid-segment clip list (so every column is written at most once -
     * no z-buffer), colormap-lit wall columns via @wr_texture and
     * floor/ceiling spans via @wr_flat. angle/fov are radians (matching
     * @camera's convention); this is the only place in the renderer a
     * float crosses the API boundary - everything downstream is 16.16
     * fixed-point. Requires @wr_map and @wr_palette to have been called
     * first. y is height (matching @camera's eye convention), x/z are the
     * ground-plane position. */
    OP_WR_RENDER  = 0x9C,
    /* @wr_texmap(sidedef_tex_ptr, n_sidedefs, sector_tex_ptr, n_sectors) -
     * tells the backend which @wr_texture/@wr_flat IDs to use for each
     * sidedef and sector during @wr_render. sidedef_tex_ptr points at
     * n_sidedefs * 3 i32s (mid_tex_id, upper_tex_id, lower_tex_id per
     * sidedef); sector_tex_ptr points at n_sectors * 2 i32s (floor_tex_id,
     * ceil_tex_id per sector). A tex_id of 0 means "no texture" (skip
     * drawing that section). Must be called after @wr_map and before
     * @wr_render; replaces any previously uploaded texmap. */
    OP_WR_TEXMAP  = 0x9D,
    /* @wr_sky(tex_id) - sets the @wr_texture ID to use for sky rendering:
     * any sector whose floor/ceiling flat is the game's F_SKY1 (which the
     * script maps to tex_id 0 in its @wr_texmap sector table, so the
     * normal flat renderer skips it) gets its open ceiling filled with
     * this texture instead, scrolled horizontally with view angle the
     * way the classic sky renderer does - no clear needed. tex_id 0
     * disables sky rendering (those columns are left untouched again).
     * Must be called after @wr_texture uploads the sky texture (the game's
     * IWADs define it as a normal composite texture named SKY1/SKY2/
     * SKY3, not a flat) and before @wr_render. */
    OP_WR_SKY     = 0x9E,

    /* @fog(mode, near, far, color) - per-pixel depth fog.
     * mode: 0=off (disable fog), 1=linear (near..far), 2=exp, 3=exp2.
     * near/far: fog start/end distances in world units (for linear mode).
     *   For exp/exp2 modes, `near` is the fog density (GL_FOG_DENSITY);
     *   `far` is unused.
     * color: *RGBA fog colour (typically = clear colour so distant geometry
     * dissolves into the background rather than fading to an arbitrary grey).
     * OpenGL backends use GL_FOG (hardware per-pixel, essentially free);
     * DOS backends are NULL (no-op) — the classic sector renderer has its own
     * colormap-based distance diminishing. */
    OP_GFX_FOG    = 0x9F,

    /* Input */
    OP_INP_KEY    = 0xA0,
    OP_INP_KEYHELD= 0xA1,
    OP_INP_MOUSE  = 0xA2,  /* @mouse(out: ref Mouse) - fill struct via pointer */
    OP_INP_GAMEPAD_COUNT = 0xA3, /* @gamepad_count() -> i32 */
    OP_INP_GAMEPAD       = 0xA4, /* @gamepad(idx: i32, out: ref Gamepad) -> i32 (1=connected) */

    /* @wr_flatshade(enable) - performance/style mode for the sector-based
     * renderer: when enable != 0, every wall column and floor/ceiling span
     * is filled with a single representative colour (sampled once from
     * each texture/flat and cached) instead of being texture-mapped
     * pixel-by-pixel. This trades texture detail for speed - floor/ceiling
     * spans become a straight memset-class fill (no per-pixel table
     * lookups at all) and wall columns drop from two dependent memory
     * reads per pixel to one, which matters a lot on cacheless CPUs like
     * a 486SX where every table lookup is a real bus cycle. Sector
     * lighting still applies (the cached colour is remapped through the
     * sector colormap same as before), so shading/distance darkening is
     * preserved - only the texture pattern itself is dropped. Must be
     * called after @wr_texture/@wr_flat uploads (the colour cache is
     * built lazily on first use per tex/flat id) and before @wr_render.
     * enable == 0 restores normal texture-mapped rendering.
     * The r1 argument packs flags: bit0 = flatshade enable; bit1 =
     * "stripe-shade" enable - wall columns fill with the average colour
     * of the texture column being sampled (cached per column per
     * texture) instead of one colour per texture, so vertical banding,
     * panel edges and door frames survive at memset-class fill cost;
     * floors/ceilings still take the solid flat average, and masked
     * midtextures stay skipped as under flatshade. Ignored while bit0
     * is also set (full flatshade wins). bits 8-9 =
     * pixel scale mode (0 = full res, 1 = "low detail" half-width
     * pixels, 2 = quarter-res 2x2 blocks). The render/projection works
     * in logical pixels and each pixel is stored as an sx*sy block, so
     * scaled modes halve/quarter the column count, clip work and
     * dependent texture loads - the classic low-detail lever. Guests
     * compiled before the field existed pass 0/1 = full res.
     * DOS backends only; no-op elsewhere. */
    OP_WR_FLATSHADE = 0xA5,
    /* @view(x,y,w,h) - redirects the render surface: the 3D projection
     * (glViewport) maps into this logical rect instead of the whole
     * window. Used by embedding hosts (TIDE) so a guest's @window-size
     * scene lands inside a panel. w<=0||h<=0 resets to the full window. */
    OP_GFX_VIEW = 0xA6,
    /* @view_capture(x,y,w,h) - snapshot that framebuffer rect into a
     * retained texture (an embedded guest's completed frame, taken at
     * its @flip; typically the visible part of the @view rect).
     * @view_present(x,y,w,h) - draw that retained frame scaled into the
     * given rect; no-op until the first capture. */
    OP_GFX_VIEW_CAPTURE = 0xA7,
    OP_GFX_VIEW_PRESENT = 0xA8,
    OP_GFX_DRAW_LIST = 0xA9,      /* @draw_list(buf: *u8, len: i32) - retained
                                   primitive buffer; see
                                   backends/common/drawlist/drawlist.h */
    /* @lines3d(walls, nw, cam_x, cam_z, gs, gc, eye, out, cap, mode)
     * -> i32 - batch corridor-renderer projection: walks `nw` wall
     * records (x1 z1 x2 z2 ybot ytop flags colour, i16 each) in world
     * units, rotates by the caller-supplied sin/cos (8.8 fixed) and
     * projects to screen-space records at `out`, at most `cap` records.
     * Wall flags: bit0/bit1 emit an end post at vertex 1/2, bit2 marks
     * a two-sided portal slab (never back-face culled), bit3 open sky
     * above. Same shape as the WR_* block above: a backend-native
     * primitive for targets where interpreting the per-wall projection
     * loop costs more than the drawing itself. Camera is yaw-only,
     * world is xz + height - the "corridor renderer" subset,
     * deliberately not a general matrix pipeline.
     *
     * mode bits:
     *   bit0  colour: output records are FIVE i32s (x1 y1 x2 y2 colour)
     *         instead of four
     *   bit2  diff-draw (BBC): emit only records changed since the
     *         last call, for XOR erase/redraw
     *   bit3  hidden-line removal: back-face cull, front-to-back sort,
     *         per-column visibility window (wireframe solidsegs)
     *   bit4  depth-band colour: +2 mid / +4 far on nearer-end depth
     *   bit5  indexed wall blob: {i16 nv, nv{x,z}, nw{v1,v2,ybot,ytop,
     *         flags,col}} + optional room-PVS / wall-BSP trailer
     *   bit6  overlay partition (BBC)
     *   bit7  span fill: per open 16-px clip column emit filled spans
     *         (solid wall [ceiling|face|floor], portal [lintel|step],
     *         open remainder void) instead of edge lines
     * Fill mode packs extra fields: mode bits 8-12/13-17/18-22/23-27
     * carry the ceiling/floor/void/sky colour words (palette index |
     * bit4 halftone), bits 28-30 the filled view height in 32-row
     * bands (0 = scrh). cap bits 0-15 stay the record cap; bits 16-23
     * carry the camera room id, bit24 a centre-column inset, bit25
     * textured faces, bit26 the hires renderer, bits 27-28 texture
     * style, bit29 a deeper inset, bit30 half-x column pairs (native
     * renderers only; the shared record path ignores them). Span
     * colour words are palette|bit4 checker|bit5 sparse|0x08000000.
     *
     * Returns: n >= 0 records written; n | 0x40000000 when the backend
     * already drew the whole frame (mask with 0x3FFFFFFF); -1 on
     * backends that do not implement it (vtable NULL) so callers keep
     * a portable fallback. Unimplemented mode/cap bits are ignored, so
     * one mode word serves every tier. */
    OP_GFX_LINES3D = 0xAA,

    /* @grid3d(map, mw, mh, cam_x, cam_z, gs, gc, out, cap, mode)
     * -> i32 - Wolfenstein-style grid raycaster: `map` is a row-major
     * u8 grid (mw x mh cells, one cell = 256 world units, camera args
     * 8.8 fixed as in @lines3d). Cell 0 is open; 1..63 is a wall whose
     * value IS the span colour word (palette index | bit4 checker |
     * bit5 sparse - same packing as the fill-mode span colours).
     * Walls with their face on the ray's x-axis get the colour as-is;
     * z-axis faces OR in bit5 (sparse dither = same hue, darker) so
     * the two faces read differently. A ray that leaves the grid or
     * runs past the distance cap leaves its clip column open, where
     * the fill machinery paints the void colour.
     *
     * Wall height equals cell size (the classic unit-cube look); the
     * projection constant follows the same camera-plane geometry as a
     * ~64-degree horizontal FOV, columns are the 16-px clip columns of
     * the fill renderer (cap bits 24/26/29 pick the same insets as
     * @lines3d). mode packs exactly like the @lines3d fill mode:
     * bits 8-12/13-17/18-22/23-27 carry the ceiling/floor/void/sky
     * colour words, bits 28-30 the filled view height in 32-row
     * bands (0 = scrh); bit 7 marks the span-fill contract the BBC
     * backend requires. `out`/`cap` follow the lines3d contract for
     * future portable-record use; the BBC path draws the whole frame
     * and ignores them.
     *
     * Returns: n | 0x40000000 when the backend drew the whole frame;
     * -1 when unimplemented (vtable NULL) so callers fall back to
     * computing columns and drawing spans themselves. */
    OP_GFX_GRID3D = 0xAB,

    /* System */
    OP_SYS_RAND   = 0xB0,
    OP_SYS_MILLIS = 0xB1,
    OP_SYS_CONSOLE = 0xB2, /* @console(s: *u8) - print null-terminated string */
    OP_SYS_SNPRINT = 0xB3, /* @snprint(n: u32, s: *u8, fmt: *u8, args...) -> *u8 */
    OP_SYS_ASSERT = 0xB4,
    OP_SYS_ALLOC  = 0xB5,
    OP_SYS_FREE   = 0xB6,
    OP_SYS_FOPEN  = 0xB7,
    OP_SYS_FREAD  = 0xB8,
    OP_SYS_FCLOSE = 0xB9,

    /* Math (single precision) */
    OP_SYS_SIN    = 0xBA,
    OP_SYS_COS    = 0xBB,
    OP_SYS_PLATFORM = 0xBC,
    OP_SYS_SQRT   = 0xBD,

    /* More system / file I/O */
    OP_SYS_FWRITE = 0xBE,  /* @file_write(fd: i32, buf: *u8, n: i32) -> u32 */
    OP_SYS_ARGS   = 0xBF,  /* @args(idx: i32, buf: *u8) -> i32  (returns argc) */
    OP_SYS_ISNUM  = 0xCA,  /* @is_number(v: f32) -> i32  (0=NaN, 1=+Inf, 2=-Inf, 3=Normal) */

    /* More file I/O */
    OP_SYS_FSEEK  = 0xCB,  /* @file_seek(fd, offset, whence) -> i32 */
    OP_SYS_FLEN   = 0xCC,  /* @file_length(fd) -> i32 */
    OP_SYS_FDIR   = 0xCD,  /* @file_directory(path, buf, max_entries) -> i32 */
    OP_SYS_FPATH  = 0xCE,  /* @file_path(buf, buf_size) -> i32 */
    OP_SYS_LOAD_PNG = 0xCF, /* @file_load_png(path, buf, buf_size, out_wh) -> i32 */

    /* Web / HTTP (poll-based, non-blocking — see docs/web.md) */
    OP_SYS_HTTP_REQ    = 0xD0, /* @http_request(method,url,headers,body,body_len) -> i32 handle */
    OP_SYS_HTTP_POLL   = 0xD1, /* @http_poll(handle) -> i32 (0=pending,1=ready,<0=error) */
    OP_SYS_HTTP_STATUS = 0xD2, /* @http_status(handle) -> i32 HTTP status code */
    OP_SYS_HTTP_READ   = 0xD3, /* @http_read(handle, buf, cap) -> i32 bytes copied (0=EOF) */
    OP_SYS_HTTP_CLOSE  = 0xD4, /* @http_close(handle) */

    OP_SYS_UNZIP       = 0xD5, /* @unzip(zip_path: *u8, dest_path: *u8) -> i32 */
    OP_SYS_REGISTER    = 0xD6, /* @register(action: i32, key: *u8, value: *u8) -> i32 */
    OP_SYS_FDELETE     = 0xD7, /* @file_delete(path: *u8) -> i32 (unlinks a file, or rmdir's an empty directory) */
    OP_SYS_FMKDIR      = 0xD8, /* @file_mkdir(path: *u8) -> i32 (creates a directory; 0=ok, -1=error) */

    /* System clipboard. NULL backend hook -> vm.c returns failure (0 for
     * set, -1 for get), so scripts degrade gracefully on platforms without
     * a clipboard (DOS) or where it isn't implemented yet (web). */
    OP_SYS_CLIP_SET    = 0xDA, /* @clipboard_set(buf: *u8, len: i32) -> i32 (1=ok, 0=fail) */
    OP_SYS_CLIP_GET    = 0xDB, /* @clipboard_get(buf: *u8, cap: i32) -> i32 (bytes written, NUL-terminated; -1=fail) */

    /* Subprocess management (poll-based, non-blocking — same pattern as
     * the HTTP ops above). NULL backend hook -> vm.c returns -1 for all
     * calls and close is a no-op, so scripts degrade gracefully on
     * platforms without process spawning (DOS, web). Used by TIDE to
     * launch `tigryx run <file>.tgx` and capture stdout/stderr. */
    OP_SYS_PROC_SPAWN      = 0xDC, /* @process_spawn(cmd: *u8, cwd: *u8) -> i32 handle (>=0) or -1 */
    OP_SYS_PROC_POLL       = 0xDD, /* @process_poll(handle: i32) -> i32 (0=running, 1=exited, -1=invalid) */
    OP_SYS_PROC_EXIT_CODE  = 0xDE, /* @process_exit_code(handle: i32) -> i32 exit code (valid once exited) */
    OP_SYS_PROC_READ_OUT   = 0xDF, /* @process_read_stdout(handle, buf: *u8, cap: i32) -> i32 bytes (0=none) */
    OP_SYS_PROC_READ_ERR   = 0xE0, /* @process_read_stderr(handle, buf: *u8, cap: i32) -> i32 bytes (0=none) */
    OP_SYS_PROC_KILL       = 0xE1, /* @process_kill(handle: i32) -> i32 (1=killed, 0=already exited) */
    OP_SYS_PROC_CLOSE      = 0xE2, /* @process_close(handle: i32) -> i32 (1=ok, releases slot; kills if running) */

    /* Bulk memory operations. Both clamp to the VM's memory bounds and
     * return the number of bytes actually written, so a caller can detect
     * a truncated operation without a separate bounds check. @memcpy
     * tolerates overlapping regions (it behaves like memmove). */
    OP_SYS_MEMCPY          = 0xE3, /* @memcpy(dst: *u8, src: *u8, n: i32) -> i32 bytes copied */
    OP_SYS_MEMSET          = 0xE4, /* @memset(dst: *u8, val: i32, n: i32) -> i32 bytes set */

    /* Extended math (single precision). Unary ops take one float arg in
     * r1; the two-arg ops take floats in r1 and r2. All return a float
     * bit pattern in r1. These are the libm single-precision
     * counterparts — platforms without an FPU may override through the
     * backend hooks (as with @sin/@cos/@sqrt). */
    OP_SYS_EXP     = 0xE5,  /* @exp(x: f32) -> f32 */
    OP_SYS_LOG     = 0xE6,  /* @log(x: f32) -> f32   (natural log) */
    OP_SYS_LOG10   = 0xE7,  /* @log10(x: f32) -> f32 */
    OP_SYS_POW     = 0xE8,  /* @pow(x: f32, y: f32) -> f32 */
    OP_SYS_ATAN    = 0xE9,  /* @atan(x: f32) -> f32 */
    OP_SYS_ATAN2   = 0xEA,  /* @atan2(y: f32, x: f32) -> f32 */
    OP_SYS_TAN     = 0xEB,  /* @tan(x: f32) -> f32 */
    OP_SYS_ASIN    = 0xEC,  /* @asin(x: f32) -> f32 */
    OP_SYS_ACOS    = 0xED,  /* @acos(x: f32) -> f32 */
    OP_SYS_FLOOR   = 0xEE,  /* @floor(x: f32) -> f32 */
    OP_SYS_CEIL    = 0xEF,  /* @ceil(x: f32) -> f32 */
    OP_SYS_ROUND   = 0xF0,  /* @round(x: f32) -> f32 (round-half-away) */
    OP_SYS_FMOD    = 0xF1,  /* @fmod(x: f32, y: f32) -> f32 */
    OP_SYS_FABS    = 0xF2,  /* @fabs(x: f32) -> f32 */
    OP_SYS_SINH    = 0xF3,  /* @sinh(x: f32) -> f32 */
    OP_SYS_COSH    = 0xF4,  /* @cosh(x: f32) -> f32 */
    OP_SYS_TANH    = 0xF5,  /* @tanh(x: f32) -> f32 */

    /* Embedded-host syscalls (TigryxOS): one multiplexed opcode for the
     * whole @os_* family - the sub-operation lives in the instruction's
     * SRCA field (see RELIC_OS_* below), call args stay in r1..rN as
     * usual, result comes back in r1. Dispatched by relic_op_sys_os to
     * the host-provided RelicOSOps vtable on RelicVM; no vtable means
     * the environment isn't an OS host and every call returns -1. */
    OP_SYS_OS      = 0xFC,

    /* @wr_patch(x, y, dst_w, dst_h, src_w, src_h, pal_data, light_level) -
     * draw a palette-indexed 2D patch directly to the WR backbuffer, stretched from (src_w,src_h) to (dst_w,dst_h)
     * via fixed-point nearest-neighbour sampling (no per-pixel divide - a
     * single 16.16 step is computed once per axis, then accumulated, the
     * same technique wr_draw_column uses for texture-mapped walls).
     * pal_data is row-major, one byte per texel (palette index). Index 0
     * is transparent (skipped). light_level selects a colormap row (0 =
     * full bright, e.g. HUD/weapon; higher = darker). This bypasses the
     * RGBA roundtrip of @texture+@sprite entirely: no bestfit_color, no
     * POT padding, no Allegro BITMAP allocation. DOS WR backend only;
     * no-op elsewhere. Must be called after @wr_palette. */
    OP_WR_PATCH   = 0xD9,

    /* @wr_sprite(items: *u8, count: i32) - draw `count` masked world-space
     * billboards (WAD "things") into the WR backbuffer, sorted
     * back-to-front by depth. Must be called after @wr_render in the same
     * frame: the op reuses that frame's view transform and the per-column
     * clip records the wall pass left behind, so a sprite standing behind
     * a wall is hidden, one seen through a doorway is clipped to the
     * doorway opening, and one in front of it draws unclipped.
     *
     * `items` is a packed array of 44-byte records (11 x i32, matching a
     * fully-packed tgx struct). f32 fields pass their raw bit pattern:
     *
     *   +0  x      f32 - thing centre, map units (same space as @wr_render)
     *   +4  z      f32 - thing centre, map units
     *   +8  hl     f32 - map units the patch extends LEFT of the centre
     *                    on screen (= the patch's leftoffset)
     *   +12 hr     f32 - map units it extends RIGHT (= src_w - leftoffset)
     *   +16 yb     f32 - world height of the billboard bottom, map units
     *   +20 yt     f32 - world height of the billboard top, map units
     *   +24 light  i32 - sector light level 0..255, mapped to a colormap
     *                    row exactly like wall columns (255 = brightest)
     *   +28 src_w  i32 - patch width in texels (<= 512)
     *   +32 src_h  i32 - patch height in texels (<= 512)
     *   +36 flags  i32 - bit0: mirror the patch horizontally
     *                      bit1: fuzzy (classic spectre) - covered pixels remap
     *                      the framebuffer through colormap row 6 instead of
     *                      writing patch texels
     *   +40 data   ptr - row-major u8 palette indices, w*h bytes; index 0
     *                    is transparent (skipped), same as @wr_patch.
     *                    Last field so the native (tigryxrc) mirror
     *                    struct needs no interior padding.
     *
     * The billboard's vertical screen extent comes from (yb,yt), not
     * src_h, so the caller decides the world-space height (patch texels
     * map 1:1 to map units for WAD-authored sprites). Requires
     * @wr_palette and a @wr_map; no-op without them. WR backends only. */
    OP_WR_SPRITE  = 0xF6,

    /* @wr_blockmap(data_ptr, size, w, h, xorg, yorg) - upload the raw
     * WAD BLOCKMAP lump (word-aligned: i16 xorg/yorg header, u16 cell
     * table of word offsets, u16 lists terminated by 0xFFFF) so
     * @wr_blocked can do the whole cell-walk + linedef probe natively.
     * Call after @wr_map; a 0 ptr or invalid dims disables the native
     * probe (callers then use their own fallback). WR backends only. */
    OP_WR_BLOCKMAP = 0xF9,
    /* @wr_blocked(pmx, pmz, rm, from_sector) -> i32: 1 = the map-unit
     * circle at (pmx,pmz) radius rm collides with geometry, 0 = clear,
     * -1 = no blockmap/map uploaded (guest should fall back). The
     * blocking rule mirrors the classic line-blocking rule: one-sided lines block,
     * two-sided lines block when impassable, when the step up exceeds 24
     * map units, or when the floor/ceiling opening is < 56. WR backends
     * only. */
    OP_WR_BLOCKED  = 0xFA,
    /* @wr_blocked_line(axm, azm, bxm, bzm, li) -> i32: 1 if the movement
     * segment a->b (map units, same scale as @wr_blocked) crosses linedef
     * li, 0 if not, -1 = no map uploaded (guest falls back). Used for
     * walk-over trigger checks where the guest only needs the boolean;
     * integer cross products in i64, no division. WR backends only. */
    OP_WR_BLOCKED_LINE = 0xFB,

    /* Tiny socket server (experimental; TIGRYX_EXPERIMENTAL_SERVER builds
     * only wire backend hooks — on all other builds every call degrades
     * to -1 / no-op). The host implements a
     * generic minimal HTTP listener; it knows nothing about NSNF itself —
     * protocol, shares and auth are all handled in Tigryx.
     *
     * @serve(cmd, h, p, n) multiplexes on cmd in r1:
     *   0 OPEN   r2=port (0=OS picks), r3=bind addr *u8 or 0, r4=flags
     *            -> server handle >=0; -1 unsupported, -2 bind failed
     *   1 CLOSE  r2=srv -> 0
     *   2 POLL   r2=srv, r3=*ServeRequest, r4=sizeof -> req handle >=0
     *            and fills the request struct; -1 nothing pending
     *   3 READ   r2=req, r3=buf, r4=cap -> request-body bytes (0=EOF)
     *   4 DROP   r2=req -> abandon the request without replying
     *   5 STAT   r2=srv, r3=*ServeStat, r4=sizeof -> fills stats
     *   6 XFER   r2=srv, r3=*ServeXfer buf, r4=sizeof buf -> in-flight
     *            transfer records for the UI
     * @serve_reply(req, status, headers, body, body_len):
     *   body_len >= 0: body is that many bytes in VM memory.
     *   body_len == -1: body is a NUL-terminated HOST FILESYSTEM PATH and
     *   the backend streams the file asynchronously (sets Content-Length
     *   itself, honours a Range: header parsed into the request struct),
     *   returning immediately — the VM never holds the file. */
    OP_SYS_SERVE       = 0xF7,
    OP_SYS_SERVE_REPLY = 0xF8,

    /* Audio */
    OP_SND_TONE_FREQ = 0xC0,
    OP_SND_TONE_ID   = 0xC1,
    OP_SND_PLAY      = 0xC2,
    OP_SND_MUSIC     = 0xC3,  /* @music(buf: *u8, len: i32, cmd: i32) */
    OP_SND_MIXER     = 0xC4,  /* @mixer(cmd: u8, value: u8) */
} RelicOpcode;

/* -------------------------------------------------------------------------
 * @register(action, key, value) -- platform integration for installers
 *
 * A single string-based builtin covers every OS-integration step an
 * installer needs (file associations, icons, PATH, shortcuts). Actions are
 * selected numerically so new ones can be added without burning opcodes.
 *
 * Not every action is meaningful everywhere: DOS has no registry, and a
 * headless VM cannot create shell shortcuts. Rather than have scripts
 * discover this through failures, RELIC_REG_CAPS returns a bitmask of the
 * actions the running platform actually supports:
 *
 *     if (@register(RELIC_REG_CAPS, "", "") & (1 << RELIC_REG_SHORTCUT)) { ... }
 *
 * Both string arguments are always passed (use "" for unused ones) so the
 * calling convention stays uniform.
 * ---------------------------------------------------------------------- */
typedef enum {
    RELIC_REG_CAPS       = 0,  /* key="", value=""            -> bitmask of supported actions */
    RELIC_REG_PROGID     = 1,  /* key=ProgID, value=description                               */
    RELIC_REG_ASSOC      = 2,  /* key=".ext", value=ProGID                                    */
    RELIC_REG_COMMAND    = 3,  /* key=ProgID, value=command line ("%1" = the file)            */
    RELIC_REG_ICON       = 4,  /* key=ProgID, value="path\to\exe,index" or "path\to\file.ico" */
    RELIC_REG_PATH       = 5,  /* key=directory to add to PATH, value=""                      */
    RELIC_REG_SHORTCUT   = 6,  /* key=target exe path, value=shortcut .lnk path               */
    RELIC_REG_APPPATH    = 7,  /* key="app.exe", value=full path to the executable            */
    RELIC_REG_UNREGISTER = 8,  /* key=ProgID or ".ext", value=""                              */
    RELIC_REG_COMMIT     = 9,  /* key="", value="" -- flush changes / notify the shell        */
    RELIC_REG_PATH_REMOVE = 10, /* key=directory to remove from PATH, value=""                */
    RELIC_REG_STARTMENU   = 11, /* key=target exe path, value="Name.lnk" (Start Menu shortcut) */

    /* key=output buffer (*u8, NOT read as a string - the raw address is
     * written into), value=app name, e.g. "Tigryx". Writes a suggested
     * per-user install directory (no admin rights needed to write there) and
     * returns its length, like @file_path - or RELIC_REG_ERR_UNSUP if the
     * platform has no such notion. The caller can always fall back to
     * @file_path() if this fails. */
    RELIC_REG_SUGGEST_PATH = 12,

    /* key=app key (a short stable name, e.g. "Tigryx" - used again with
     * RELIC_REG_UNREGISTER to remove this entry), value=six '\n'-separated
     * fields in this exact order:
     *   DisplayName, DisplayVersion, Publisher, UninstallString,
     *   DisplayIcon, InstallLocation
     * Registers the app so it shows up in Windows' "Apps & Features" /
     * Control Panel "Programs and Features", with an Uninstall button that
     * runs UninstallString. NoModify/NoRepair are set automatically since
     * tigryx installers don't support either. */
    RELIC_REG_UNINSTALL = 13,

    /* key=path to the currently-running uninstaller exe, value=path to the
     * install directory. Spawns a detached helper (cmd.exe on Windows) that
     * waits briefly for the caller to exit, then deletes the exe and removes
     * the now-empty directory. This solves the classic "running exe cannot
     * delete itself" problem. The caller should call this LAST and then exit
     * immediately. Returns RELIC_REG_OK on success (the helper was launched),
     * RELIC_REG_ERR_UNSUP on platforms with no such notion. */
    RELIC_REG_SELFDELETE = 14,

    /* key=output buffer (*u8, NOT read as a string - the raw address is
     * written into), value="" (unused). Writes the full path of the
     * currently-running executable and returns its length, like
     * @file_path but for the exe location rather than the CWD. This is
     * needed because @file_path returns the current working directory,
     * which may differ from the executable's directory (e.g. when
     * Windows launches an uninstaller from "Add/Remove Programs" with
     * CWD = C:\Windows\System32). */
    RELIC_REG_EXE_PATH = 15,

    /* key="", value="" -> 1 if running on Windows 95/98/ME (the "9x"
     * family: no NTFS-style per-user registry merging, no Program Files
     * convention was ever the norm here), 0 on Windows NT+ and every other
     * platform. Installers use this to pick an install location: 9x users
     * expect a top-level folder like C:\Tigryx rather than a deep per-user
     * profile path. RELIC_REG_ERR_UNSUP is never returned - platforms that
     * don't have the concept simply answer 0. */
    RELIC_REG_IS_LEGACY_9X = 16,

    /* key="", value="" -> enables system-wide grayscale font smoothing via
     * SystemParametersInfo(SPI_SETFONTSMOOTHING). Only effective on Windows
     * 98/ME (Win95 lacks the API; XP+ defaults to ClearType). Returns
     * RELIC_REG_OK on success, RELIC_REG_ERR_UNSUP on platforms without the
     * notion. The change is system-wide and persists across reboots; the
     * installer does NOT undo it on uninstall (it is a user-visible quality
     * improvement, like adding a PATH entry). */
    RELIC_REG_FONTSMOOTHING = 17,

    /* key=output buffer (*u8, NOT read as a string - written into), value="".
     * Queries the OS-wide light/dark app theme and accent (highlight) colour.
     * Returns 0 if the OS is in dark mode, 1 if in light mode, or
     * RELIC_REG_ERR_UNSUP if the platform has no such notion.
     *
     * The accent colour (if available) is written as a 6-char hex string
     * "RRGGBB" (no '#', NUL-terminated) through the key buffer. An empty
     * string is written if the platform does not expose an accent colour.
     *
     * Windows: reads AppsUseLightTheme from the registry (dark/light) and
     * the UI accent colour from Explorer\Accent\AccentColorMenu (the colour
     * shown in Settings > Personalization > Colors, used on buttons/toggles/
     * selection highlights). Falls back to StartColorMenu, then DWM\
     * ColorizationColor, then GetSysColor(COLOR_HIGHLIGHT) on older Windows.
     * Win95 always reports light mode (the registry key does not exist).
     *
     * macOS: reads AppleInterfaceStyle (dark/light) and the accent colour
     * index from defaults(1). Other platforms return RELIC_REG_ERR_UNSUP. */
    RELIC_REG_SYSTEM_THEME = 18,

    /* key=output buffer (*u8, NOT read as a string - written into), value="".
     * Writes the current user's Desktop folder path (e.g.
     * "C:\Users\ian\Desktop") and returns its length, like
     * RELIC_REG_EXE_PATH. Use with RELIC_REG_SHORTCUT to place a .lnk on the
     * desktop. Returns RELIC_REG_ERR_UNSUP on platforms without the notion.
     * Windows: SHGetFolderPathA(CSIDL_DESKTOPDIRECTORY) - per-user desktop,
     * correct for per-user installs. */
    RELIC_REG_DESKTOPDIR = 19,

    /* key=output buffer (*u8, NOT read as a string - written into), value="".
     * Writes the OS's classic UI colours as eleven ';'-separated 6-char hex
     * strings "RRGGBB;RRGGBB;..." (no '#', NUL-terminated) and returns the
     * length. Field order:
     *   1  face      - button face / dialog background (COLOR_3DFACE)
     *   2  hilight   - bright bevel edge (COLOR_3DHILIGHT)
     *   3  light     - secondary bevel edge (COLOR_3DLIGHT)
     *   4  shadow    - bevel shadow (COLOR_3DSHADOW)
     *   5  dkshadow  - darkest bevel edge (COLOR_3DDKSHADOW)
     *   6  window    - well/field background (COLOR_WINDOW)
     *   7  wintext   - text inside wells (COLOR_WINDOWTEXT)
     *   8  btntext   - text on buttons (COLOR_BTNTEXT)
     *   9  highlight - selection background (COLOR_HIGHLIGHT)
     *   10 hightext  - selected text (COLOR_HIGHLIGHTTEXT)
     *   11 graytext  - disabled text (COLOR_GRAYTEXT)
     * These are exactly the colours a classic Windows appearance scheme
     * (Display Properties > Appearance) customises, so a "Windows Standard"
     * theme can track the host scheme. The buffer must hold at least 96
     * bytes. Returns RELIC_REG_ERR_UNSUP on platforms without the notion. */
    RELIC_REG_SYSTEM_COLORS = 20,

    /* key=output buffer (*u8, NOT read as a string - written into), value="".
     * Writes the OS's default UI font face name (NUL-terminated, max 31
     * chars + NUL) and returns its length. Windows: the stock
     * DEFAULT_GUI_FONT face - "MS Sans Serif" on Windows 9x, its mapped
     * substitute (typically Tahoma / Microsoft Sans Serif) on NT+. The
     * buffer must hold at least 32 bytes. RELIC_REG_ERR_UNSUP elsewhere. */
    RELIC_REG_SYSTEM_FONT = 21,

    /* ---- process environment ------------------------------------------
     * The next five actions expose the host process environment to guest
     * code (a shell/console component being the motivating user). Because
     * @process_spawn children inherit the host environment block,
     * RELIC_REG_ENV_SET is enough to make a shell's `export` reach the
     * programs it launches - no change to the spawn API.
     *
     * Unlike the registry actions, `key` stays an INPUT (the variable
     * name or a decimal index); string results are written THROUGH
     * `value`, which must be a writable guest buffer of >= 1024 bytes.
     * On platforms with no environment notion every action returns
     * RELIC_REG_ERR_UNSUP. */

    /* key=variable name (input), value=output buffer. Writes the value
     * NUL-terminated and returns its length; 0 means unset or empty. */
    RELIC_REG_ENV_GET = 22,

    /* key=variable name, value=variable value (both inputs). Returns
     * RELIC_REG_OK, RELIC_REG_ERR_ARGS for an empty/'='-bearing name, or
     * RELIC_REG_ERR_OS on failure. An empty value sets an empty variable;
     * use RELIC_REG_ENV_UNSET to remove one. */
    RELIC_REG_ENV_SET = 23,

    /* key=variable name, value="". Removes the variable. Always returns
     * RELIC_REG_OK where environments exist (removing an absent name is
     * not an error). */
    RELIC_REG_ENV_UNSET = 24,

    /* key="", value="". Returns the number of enumerable "NAME=VALUE"
     * entries, >= 0. */
    RELIC_REG_ENV_COUNT = 25,

    /* key=decimal index as a string ("0", "1", ...), value=output buffer.
     * Writes the idx-th environment entry as "NAME=VALUE", returns its
     * length, or RELIC_REG_ERR_ARGS when the index is out of range or not
     * a number. Entries that carry no '=' (e.g. Windows' hidden "=C:"
     * drive-cwd records) are skipped so index space stays consistent
     * with RELIC_REG_ENV_COUNT. */
    RELIC_REG_ENV_NAME = 26,

    /* ---- window chrome controls -------------------------------------
     * Scripts running under WINFLAG_FULLCONTENT draw their own title bar
     * and, on platforms where the native buttons vanish with the caption
     * (Win32, Linux), their own minimise/maximise buttons in the trailing
     * WINDOW_BUTTONS_W of the drag strip. These actions route to the
     * backend's win_ctl hook, which owns the window handle; key and value
     * are unused (""). Platforms without a hook return
     * RELIC_REG_ERR_UNSUP - the script should then hide its buttons. */

    /* key="", value="". Iconify/minimise the window. -> RELIC_REG_OK */
    RELIC_REG_WIN_MINIMIZE = 27,

    /* key="", value="". Toggle maximise/zoom (restore when already
     * maximised). -> RELIC_REG_OK */
    RELIC_REG_WIN_MAXIMIZE = 28,

    /* key="", value="". Query state -> 0 normal, 1 maximised/zoomed.
     * Scripts redraw their maximise glyph from this after
     * RELIC_REG_WIN_MAXIMIZE. */
    RELIC_REG_WIN_STATE = 29,
} RelicRegisterAction;

/* @register return codes. Zero and positive mean success. */
#define RELIC_REG_OK            0   /* completed                                        */
#define RELIC_REG_OK_REBOOT     1   /* completed, but a reboot is required to take effect */
#define RELIC_REG_ERR_UNSUP   (-1)  /* action not supported on this platform            */
#define RELIC_REG_ERR_ARGS    (-2)  /* missing or malformed key/value                   */
#define RELIC_REG_ERR_OS      (-3)  /* the OS rejected the change (permissions, etc.)   */

/* -------------------------------------------------------------------------
 * IR instruction encoding helpers
 *
 * [31:24] opcode  [23:20] dst  [19:16] src_a  [15:12] src_b  [11:0] imm12
 *
 * Opcodes that need a wider immediate repurpose the register nibbles and
 * read a 20-bit field from bits [19:0] (OP_LDC's constant, OP_ENTER's
 * frame size). IR_UIMM20 is the unsigned variant used by OP_ENTER - frame
 * sizes are never negative, so the sign bit is payload rather than a sign.
 *
 * OP_JMP/JT/JF carry a signed 20-bit PC-RELATIVE displacement, measured
 * from the instruction after the jump (bytecode format v4+):
 *   [31:24] op  [23:20] disp bits 19:16  [19:16] src_a (condition)
 *   [15: 0] disp bits 15:0
 *   target = jump_pc + 1 + disp
 * Jumps never leave their function, so the reach (+/-512K IR words) bounds
 * a single function, not the program. Every decoder must use IR_JTARGET -
 * never re-derive the field layout by hand.
 * ---------------------------------------------------------------------- */
#define IR_ENCODE(op, dst, a, b, imm) \
    ( ((u32)(op)  << 24) | ((u32)((dst) & 0xF) << 20) | \
      ((u32)((a)  & 0xF) << 16) | ((u32)((b) & 0xF) << 12) | \
      ((u32)((imm)& 0xFFF)) )

#define IR_OP(instr)   ((u8) (((instr) >> 24) & 0xFF))
#define IR_DST(instr)  ((u8) (((instr) >> 20) & 0x0F))
#define IR_SRCA(instr) ((u8) (((instr) >> 16) & 0x0F))
#define IR_SRCB(instr) ((u8) (((instr) >> 12) & 0x0F))
#define IR_IMM12(instr) ((i32)(((instr) & 0xFFF) | (((instr) & 0x800) ? 0xFFFFF000 : 0)))
#define IR_IMM20(instr) ((i32)(((instr) & 0xFFFFF) | (((instr) & 0x80000) ? 0xFFF00000 : 0)))
#define IR_UIMM20(instr) ((i32)((instr) & 0xFFFFFu))

#define IR_JDISP_MIN (-0x80000)
#define IR_JDISP_MAX 0x7FFFF
/* Raw 20-bit jump field (bits [23:20] and [15:0]), sign-extended. */
#define IR_JDISP(instr) \
    ((i32)(((((u32)(instr) >> 4) & 0xF0000u) | ((u32)(instr) & 0xFFFFu)) ^ 0x80000u) - 0x80000)
/* Absolute target of the jump at `pc`. */
#define IR_JTARGET(instr, pc) ((i32)(pc) + 1 + IR_JDISP(instr))
/* Re-encode `instr` (op + src_a kept) to jump from `pc` to `tgt`. Caller
 * checks the displacement fits IR_JDISP_MIN..IR_JDISP_MAX. */
#define IR_JSET(instr, pc, tgt) \
    ( ((u32)(instr) & 0xFF0F0000u) | \
      ((((u32)((i32)(tgt) - (i32)(pc) - 1)) & 0xF0000u) << 4) | \
      (((u32)((i32)(tgt) - (i32)(pc) - 1)) & 0xFFFFu) )
#define IR_JFITS(pc, tgt) \
    ((i32)(tgt) - (i32)(pc) - 1 >= IR_JDISP_MIN && (i32)(tgt) - (i32)(pc) - 1 <= IR_JDISP_MAX)

/* -------------------------------------------------------------------------
 * Token types
 * ---------------------------------------------------------------------- */
typedef enum {
    TK_EOF = 0,

    /* Literals */
    TK_INT, TK_CHAR, TK_STRING, TK_FLOAT,

    /* Identifiers and keywords */
    TK_IDENT,
    TK_BOOL, TK_BREAK, TK_CAST, TK_CLASS, TK_CONST, TK_CONTINUE,
    TK_ELSE, TK_FALSE, TK_FN, TK_FOR, TK_IF,
    TK_F32, TK_F64, TK_I8, TK_I16, TK_I32, TK_IN,
    TK_MATCH, TK_REF, TK_RETURN, TK_STRUCT, TK_TRUE,
    TK_U8, TK_U16, TK_U32, TK_VAR, TK_WHILE,

    /* Punctuation */
    TK_LPAREN, TK_RPAREN,       /* ( ) */
    TK_LBRACE, TK_RBRACE,       /* { } */
    TK_LBRACKET, TK_RBRACKET,   /* [ ] */
    TK_COMMA, TK_SEMICOLON, TK_COLON, TK_DOT,
    TK_ARROW,                   /* -> */
    TK_DOTDOT,                  /* .. */
    TK_AT,                      /* @ */
    TK_AMPERSAND,               /* & (unary address-of or bitwise AND) */
    TK_STAR,                    /* * */
    TK_FAT_ARROW,               /* => */

    /* Operators */
    TK_PLUS, TK_MINUS, TK_SLASH, TK_PERCENT,
    TK_BANG,                    /* ! */
    TK_TILDE,                   /* ~ */
    TK_PIPE,                    /* | */
    TK_CARET,                   /* ^ */
    TK_LSHIFT, TK_RSHIFT,       /* << >> */

    /* Comparison */
    TK_EQ, TK_NEQ,              /* == != */
    TK_LT, TK_LE,               /* < <= */
    TK_GT, TK_GE,               /* > >= */

    /* Logical */
    TK_AND, TK_OR,              /* && || */

    /* Assignment */
    TK_ASSIGN,                  /* = */
    TK_PLUS_ASSIGN,             /* += */
    TK_MINUS_ASSIGN,            /* -= */
    TK_STAR_ASSIGN,             /* *= */
    TK_SLASH_ASSIGN,            /* /= */
    TK_PERCENT_ASSIGN,          /* %= */
    TK_AMP_ASSIGN,              /* &= */
    TK_PIPE_ASSIGN,             /* |= */
    TK_CARET_ASSIGN,            /* ^= */
    TK_LSHIFT_ASSIGN,           /* <<= */
    TK_RSHIFT_ASSIGN,           /* >>= */

    TK_COUNT
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *start;  /* pointer into source buffer */
    int         len;
    int         line;
    int         file_id;  /* index into Lexer::files[] / IRBuffer::source_files[];
                           * -1 = unknown (no #line "path" / initial path seen yet) */
    int         col;      /* 1-based column of `start` within its line; -1 = unknown */
    int         end_col;  /* 1-based, exclusive (col + len); -1 = unknown */
    u8          f64lit;   /* TK_FLOAT only: written with a 'd' suffix (1.5d),
                           * so it is a double literal rather than f32. */
    union {
        i32    ival;    /* TK_INT, TK_CHAR */
        int    soff;    /* TK_STRING: offset into string table */
        /* TK_FLOAT. Deliberately `double`, not `float`, even though f32 was
         * the only float type when this was written: the literal must keep
         * full precision until the emitter knows the target type. Narrowing
         * here would make `var x: f64 = 0.1` yield f32(0.1) widened
         * (0x3FB99999A0000000) instead of the nearest double
         * (0x3FB999999999999A). The f32 path narrows at emission, so its
         * output is unchanged. See tests/f64/t_f64_literal.tgx. */
        double fval;    /* TK_FLOAT */
    } val;
} Token;

/* -------------------------------------------------------------------------
 * AST node types
 * ---------------------------------------------------------------------- */
typedef enum {
    /* Declarations */
    ND_PROGRAM,
    ND_STRUCT_DECL,
    ND_FN_DECL,
    ND_CONST_DECL,
    ND_VAR_DECL,
    ND_PARAM,
    ND_FIELD,

    /* Statements */
    ND_BLOCK,
    ND_ASSIGN,
    ND_IF,
    ND_WHILE,
    ND_FOR,
    ND_MATCH,
    ND_MATCH_ARM,
    ND_RETURN,
    ND_BREAK,
    ND_CONTINUE,
    ND_EXPR_STMT,

    /* Expressions */
    ND_BINOP,
    ND_UNOP,
    ND_CALL,
    ND_BUILTIN_CALL,
    ND_INDEX,
    ND_FIELD_ACCESS,
    ND_CAST,
    ND_IDENT,
    ND_INT_LIT,
    ND_FLOAT_LIT,
    ND_CHAR_LIT,
    ND_STRING_LIT,
    ND_BOOL_LIT,
    ND_RANGE,       /* expr '..' expr (for loop) */

    /* Native class support. Appended at the end so
     * existing kind codes are unchanged. */
    ND_CLASS_DECL,  /* class Name [implicit] { fields }; the desugared
                     * method ND_FN_DECLs live in ->members AND as
                     * top-level program children immediately after
                     * this node (fields first, then methods in source
                     * order). */
    ND_NEW,         /* new Class(args) - the type checker rewrites this
                     * in place to an ordinary ND_CALL on the class's
                     * desugared "Class__new" constructor fn */
    ND_STMT_SEQ,    /* scopeless statement sequence produced by array
                     * literal desugaring ([e0,e1,...] -> recv[i]=ei);
                     * parse_block splices its children in place so a
                     * `var` member lands in the enclosing scope */

    ND_COUNT
} NodeKind;

/* -------------------------------------------------------------------------
 * Type representation
 * ---------------------------------------------------------------------- */
typedef enum {
    TY_VOID,
    TY_BOOL,
    TY_I8, TY_U8,
    TY_I16, TY_U16,
    TY_I32, TY_U32,
    TY_F32,         /* IEEE-754 single precision */
    TY_PTR,         /* .inner = pointee type (nullable, arithmetic OK) */
    TY_REF,         /* .inner = referent type (non-null at type level,
                     * no pointer arithmetic; same runtime repr as TY_PTR) */
    TY_ARRAY,       /* .inner = element type, .count = size */
    TY_STRUCT,      /* .name = struct name */
    TY_FNPTR,       /* .inner = return type, .params / .nparams */
    /* IEEE-754 double precision. APPENDED here rather than placed beside
     * TY_F32, deliberately: dbg_sym() in ir.c serialises (u8)t->kind into
     * the .tlx debug-symbol blob, so inserting a value mid-enum would
     * renumber TY_PTR/TY_ARRAY/TY_STRUCT and make every previously built
     * .tlx misreport its variable types in the debugger. Appending keeps
     * all existing type codes stable. */
    TY_F64,
    TY_COUNT
} TypeKind;

typedef struct Type Type;
struct Type {
    TypeKind  kind;
    Type     *inner;        /* pointee / element / return type */
    u32       count;        /* array element count */
    char      name[RELIC_MAX_IDENT]; /* struct or typedef name */
    Type    **params;       /* fn ptr param types */
    int       nparams;
};

/* -------------------------------------------------------------------------
 * AST node
 * ---------------------------------------------------------------------- */
/* A non-fatal diagnostic collected during compilation (see
 * IRBuffer::warnings). message is fixed-size so the array stays POD. */
typedef struct {
    int  file_id;
    int  line;
    int  col;
    char message[224];
} RelicWarning;

/* Cap on stored warnings per compile - enough to see a real problem,
 * small enough that a pathological file can't flood memory. */
#define RELIC_MAX_WARNINGS 64

typedef struct Node Node;
struct Node {
    NodeKind  kind;
    int       line;
    int       file_id;      /* copied from the token at alloc_node() time;
                              * -1 = unknown (see Token::file_id) */
    int       col;          /* ditto, from Token::col; -1 = unknown */
    Type     *type;         /* filled by type checker */

    /* Children — small fixed-size set covers all node kinds */
    Node     *left;
    Node     *right;
    Node     *cond;
    Node    **children;
    int       nchildren;

    /* Payload */
    char      sval[RELIC_MAX_IDENT];  /* identifier name, field name */
    i32       ival;                   /* int/char literal */
    /* Float literal. `double` so an f64 literal keeps full precision; the
     * f32 emit path narrows at use. See Token::val.fval above. */
    double    fval;                   /* float literal (ND_FLOAT_LIT) */
    int       soff;                   /* string literal offset */
    int       op;                     /* binary/unary operator token kind */
    bool      bval;                   /* bool literal / class `implicit` flag */

    /* Native class support. members[] is populated on
     * ND_CLASS_DECL only and holds the desugared method ND_FN_DECL nodes
     * (each of which has ->owner pointing back at the class). ->mflags
     * carries ND_MF_* bits on those method nodes. All three are NULL/0
     * on every other node kind. */
    Node    **members;
    int       nmembers;
    Node     *owner;
    int       mflags;
};

/* Node::mflags bits - set on the ND_FN_DECL a class method desugars to. */
#define ND_MF_METHOD 0x1   /* function is a class method (self injected) */
#define ND_MF_CTOR   0x2   /* method is a `new` constructor (returns Class) */
#define ND_MF_PROTO  0x4   /* declaration-only method (out-of-class def) */
#define ND_MF_OUTCLASS 0x8 /* `fn Class.method(...)` - defined outside the
                          * class body; ->owner is resolved in the type
                          * checker once all class decls are known */

/* -------------------------------------------------------------------------
 * Symbol table entry
 * ---------------------------------------------------------------------- */
typedef enum {
    SYM_VAR,
    SYM_CONST,
    SYM_FN,
    SYM_STRUCT,
    SYM_PARAM,
} SymKind;

typedef struct Symbol Symbol;
struct Symbol {
    char    name[RELIC_MAX_IDENT];
    SymKind kind;
    Type   *type;
    int     scope;
    i32     offset;     /* stack frame byte offset for locals/params */
    i32     fn_id;      /* function id for SYM_FN */
    i32     const_val;  /* value for SYM_CONST (valid iff has_const_val) */
    bool    has_const_val; /* true once const_val has been constant-folded;
                            * scalar consts with a non-foldable initializer
                            * stay false and get runtime storage instead */
};

/* -------------------------------------------------------------------------
 * Lexer state
 * ---------------------------------------------------------------------- */
typedef struct {
    const char *src;
    int         pos;
    int         len;
    int         line;
    int         line_start;    /* byte offset of the start of `line`, for
                                * computing Token::col/end_col */
    Token      *tokens;
    int         ntokens;
    int         capacity;      /* allocated capacity for tokens array */
    char        err[256];
    int         err_line;      /* source line the error is attributed to - NOT
                              * the scanning position: on failure scanning has
                              * already run past the fault (an unterminated
                              * comment/string consumes to EOF). 0 = unknown */
    int         err_file_id;   /* file table index at the error site; -1 =
                              * unknown/irrelevant */
    int         err_col;       /* column of the error site; -1 = unknown */

    /* File table for #line "path" attribution (see lexer.c). Populated by
     * an optional initial path passed to relic_lex(), and by `#line N
     * "path"` directives tigryxpp emits at #import boundaries. A bare
     * `#line N` (no filename) leaves cur_file unchanged. */
#ifdef RELIC_IA16
    /* ia16: far-allocated lazily by lexer_intern_file (8K inline would
     * blow the near heap). Read entries with LXFILE()/relic_far_str(). */
    char RELIC_FAR (*files)[RELIC_MAX_SOURCE_PATH];
#else
    char        files[RELIC_MAX_SOURCE_FILES][RELIC_MAX_SOURCE_PATH];
#endif
    int         nfiles;
    int         cur_file;      /* -1 = no file identity known yet */
} Lexer;

/* -------------------------------------------------------------------------
 * Symbol / scope table
 * ---------------------------------------------------------------------- */
typedef struct {
    Symbol  syms[RELIC_MAX_SYMBOLS];
    int     nsyms;
    int     scope;          /* current scope depth */
    char    err[256];
} SymTable;

/* -------------------------------------------------------------------------
 * Asset table
 *
 * An asset is a named binary blob packed into a .tlx file after the normal
 * bytecode payload.  At runtime the VM keeps a pointer to a RelicAssets
 * table; @file_open checks this table first (by name) before falling back
 * to the host filesystem, giving a seamless development/release workflow.
 *
 * File layout of the asset trailer (appended after the bytecode stream):
 *   [blob_0 bytes] ... [blob_N-1 bytes]          -- raw data, back-to-back
 *   For each entry (N entries, in order):
 *     u32  name_len      -- byte length of name string (not NUL-terminated)
 *     u8[] name          -- path-preserving name, e.g. "mydata/foo.dat"
 *     u32  blob_offset   -- byte offset from start of trailer blob region
 *     u32  blob_size     -- byte length of blob
 *   u32  asset_count     -- number of entries
 *   u32  magic           -- RELIC_ASSET_MAGIC ("RLA\0")
 *
 * Reading 8 bytes from EOF identifies whether a trailer is present.
 * Old loaders that don't know about assets just ignore the trailer.
 * ---------------------------------------------------------------------- */
#define RELIC_ASSET_MAGIC    0x00414C52u  /* "RLA\0" little-endian */
#define RELIC_MAX_ASSET_NAME 256
#ifndef RELIC_MAX_ASSETS
#define RELIC_MAX_ASSETS     256
#endif

typedef struct {
    char   name[RELIC_MAX_ASSET_NAME];
    u32    blob_offset;   /* byte offset within RelicAssets->data */
    u32    blob_size;
} RelicAssetEntry;

typedef struct {
    int              count;
    RelicAssetEntry  entries[RELIC_MAX_ASSETS];
    u8 RELIC_FAR    *data;       /* all blobs concatenated; far alloc'd */
    u32              data_size;
    int              borrowed;   /* data points into caller memory; not freed */
} RelicAssets;

/* -------------------------------------------------------------------------
 * IR buffer
 * ---------------------------------------------------------------------- */
typedef struct {
    /* Instruction stream. Grows dynamically (see relic_ir_reserve in ir.c).
     * `cap` is the current allocated word count for `code`, `line_for_pc`
     * and `file_for_pc`; `len` is how many words have been emitted. */
    u32 RELIC_FAR *code;
    /* parallel to code: source line per word (0 = none) and index into
     * source_files[] per word (-1 = unknown).  i32 (not int): these are
     * serialised as 4-byte LE words, so they must be 32-bit on 16-bit
     * hosts too. */
    i32 RELIC_FAR *line_for_pc;
    i32 RELIC_FAR *file_for_pc;
    int  len;
    int  cap;
    /* ia16: set before relic_ir_reserve() to force allocation of the
     * line_for_pc/file_for_pc debug tables.  Clear on the common bytecode-
     * load path (debug section absent), where leaving them NULL saves
     * 2*cap*4 bytes of scarce real-mode memory.  Emit sets it because
     * emit() writes both tables unconditionally. */
    int  want_dbg;

    /* Multi-file table, populated by relic_compile_source() from the
     * lexer's file table once compilation succeeds (see compile.c). Empty
     * (nsource_files == 0) for IRBuffers built any other way (assembler,
     * bytecode load) - file_for_pc entries are meaningless without it.
     * ia16: far-allocated (16*512B inline would exceed the near heap).
     * Read entries with IRSRCFILE()/relic_far_str(). */
#ifdef RELIC_IA16
    char RELIC_FAR (*source_files)[RELIC_MAX_SOURCE_PATH];
#else
    char source_files[RELIC_MAX_SOURCE_FILES][RELIC_MAX_SOURCE_PATH];
#endif
    int  nsource_files;

    /* Function table: fn_offsets[fn_id] = word index of first instruction.
     * i32 (not int): serialised as 4-byte LE words.
     * ia16: far-allocated; read/write entries with IRFNOFF(). */
#ifdef RELIC_IA16
    i32 RELIC_FAR *fn_offsets;
#else
    i32  fn_offsets[RELIC_MAX_FUNCS];
#endif
    int  nfuncs;
    /* Argument registers per function, parallel to fn_offsets: bit L set
     * means the caller stages logical rL (r1 = hidden dest ptr for
     * struct-returning fns; user args start at arg_base). Compile-time
     * metadata for native backends; not serialised, not read at runtime.
     * Zero for IR built without it (assembler, bytecode load).
     * ia16: far-allocated; read/write entries with IRFNAMASK(). */
#ifdef RELIC_IA16
    u16 RELIC_FAR *fn_argmask;
#else
    u16  fn_argmask[RELIC_MAX_FUNCS];
#endif
    /* Optional: function names parallel to fn_offsets, for nicer backtraces.
     * Filled by the emitter; empty string if unknown.
     * ia16: far-allocated (RELIC_MAX_FUNCS*64 would exceed the 32K member-
     * array limit). Use IRFNNAME()/relic_far_str() to read entries. */
#ifdef RELIC_IA16
    char RELIC_FAR (*fn_names)[RELIC_MAX_IDENT];
#else
    char fn_names[RELIC_MAX_FUNCS][RELIC_MAX_IDENT];
#endif

    /* String table */
#ifdef RELIC_IA16
    char RELIC_FAR *strings;
#else
    char strings[RELIC_MAX_STRINGS];
#endif
    u32  strings_len;

    /* Global data segment: globals placed at vm->mem[0..globals_size)
     * (i32 not int: on ia16 a plain int can't hold data segments >32K). */
    i32  globals_size;

    /* Debug symbols blob (function/local/global records for the debugger's
     * variables view). Collected by the emitter; written into the .tlx
     * only when the caller saves with include_debug. Record layout is
     * documented at the writer (relic_bytecode_write). */
    u8  *dbg_syms;
    int  dbg_syms_len;
    int  dbg_syms_cap;

    /* Adaptive Build tier stamped by the .tlx writer (relic_tier_detect()
     * at save time) and recovered by the bytecode loaders — -1 when the
     * file predates the stamp or the IR came from source/assembly. See
     * tigryx_tier.h; serialised in the .tlx flags byte, not a field, so
     * the format stays backward-compatible. */
    int  tier;

    /* Requested VM heap in bytes, from `#pragma arg heap` / `--heap`.
     * Serialised as a u32 field in bytecode-format v5 (written only when
     * nonzero, so heap-less programs stay on v4 and load everywhere).
     * 0 = no embedded preference -> RELIC_MEM_SIZE default applies. A
     * command-line --heap always wins over this value at run time. */
    u32  heap_size;

    /* Compile-time warnings collected while the IR was being built
     * (e.g. uninitialised class fields from the typechecker). file_id
     * indexes source_files[] once that table is populated; -1 when the
     * producing diagnostic had no file context. The array is heap-
     * allocated, capped at RELIC_MAX_WARNINGS; further warnings are
     * counted in warnings_dropped instead of stored. */
    RelicWarning *warnings;
    int  nwarnings;
    int  warnings_dropped;

    char source_path[512];
} IRBuffer;

/* Ensure ir->cap >= need words. Returns false on OOM. Safe to call before
 * any direct write to ir->code[]. */
bool relic_ir_reserve(IRBuffer *ir, int need);
/* ia16: lazily allocate the compile-time aux tables (fn_names, fn_argmask,
 * source_files) so the bytecode-load/run path doesn't pay for them.
 * Always true on flat hosts (the tables are inline there). */
bool relic_ir_ensure_aux(IRBuffer *ir);
/* Copy function `fn`'s name (if any) into dst; writes "" when unknown
 * (ia16: fn_names may be unallocated on the run path). */
void relic_ir_fn_name(const IRBuffer *ir, int fn, char *dst, size_t cap);
/* Release dynamically-allocated IR buffers (code, line_for_pc). */
void relic_ir_free(IRBuffer *ir);

/* -------------------------------------------------------------------------
 * VM state
 * ---------------------------------------------------------------------- */
typedef struct RelicVM RelicVM;

/* Per-VM open file handles: each slot is either an asset cursor
 * (is_asset=true, fp=NULL) or a real host FILE*.  The guest sees an
 * opaque i32 index.  The table lives inside RelicVM so several VMs in
 * one process (the TigryxOS multi-VM host) cannot share, exhaust or
 * close each other's handles. */
/* -------------------------------------------------------------------------
 * Embedded-host OS syscall vtable (see OP_SYS_OS).
 *
 * A host that embeds RelicVMs as first-class processes (TigryxOS) hangs a
 * table of these off RelicVM.os_ops. Guests reach it through the @os_*
 * builtins; the sub-op selector rides in the instruction's SRCA field so
 * call args keep their normal r1..rN positions.
 *
 * Sub-op codes (SRCA field; max 15):
 *   SPAWN    r1 = path ptr          -> handle >=0, or -1 on failure
 *   KILL     r1 = handle            -> 1 ok / 0 invalid
 *   STATUS   r1 = handle            -> RELIC_OS_ST_* flags, -1 invalid
 *   FOCUS    r1 = handle            -> 1 ok / 0 invalid
 *   COUNT    (no args)              -> number of live app slots
 *   HANDLE   r1 = ordinal 0..count-1-> handle of the n-th app, -1 oob
 *   SET_RECT r1 = handle, r2..r5 = x,y,w,h (desktop px) -> 1 ok / 0
 *   GET_RECT r1 = handle, r2 = out ptr ([4]i32 frame rect)  -> 1 ok / 0
 *   NAME     r1 = handle, r2 = buf, r3 = cap -> chars written, -1 inv
 *   ICONIFY  r1 = handle, r2 = 1 iconify / 0 restore -> 1 ok / 0 inv
 *   DOCK     r1 = handle, r2..r5 = x,y,w,h in the CALLER's content
 *            coords -> 1 ok / 0 invalid. The window becomes chromeless,
 *            pinned just above the caller in z, clipped to the caller's
 *            content rect, and transparent to clicks (they land on the
 *            caller). w<=0 or h<=0 undocks it back to a normal window.
 *            A docked window dies with its owner.
 * ---------------------------------------------------------------------- */
#define RELIC_OS_SPAWN    0
#define RELIC_OS_KILL     1
#define RELIC_OS_STATUS   2
#define RELIC_OS_FOCUS    3
#define RELIC_OS_COUNT    4
#define RELIC_OS_HANDLE   5
#define RELIC_OS_SET_RECT 6
#define RELIC_OS_GET_RECT 7
#define RELIC_OS_NAME     8
#define RELIC_OS_ICONIFY  9
#define RELIC_OS_DOCK     10

/* STATUS flag bits returned by RELIC_OS_STATUS. */
#define RELIC_OS_ST_RUNNING  1  /* vm scheduled + running */
#define RELIC_OS_ST_DEAD     2  /* halted/crashed - window may linger */
#define RELIC_OS_ST_ICON     4  /* iconified (hidden to taskbar) */
#define RELIC_OS_ST_FOCUS    8  /* currently the focused window */
#define RELIC_OS_ST_PINNED   16 /* pinned desktop shell (VM 0) */
#define RELIC_OS_ST_WINDOW   32 /* guest opened a @window surface */

struct RelicVM;
typedef struct RelicOSOps {
    int (*spawn)(struct RelicVM *caller, const char *path);
    int (*kill)(struct RelicVM *caller, int handle);
    int (*status)(struct RelicVM *caller, int handle);
    int (*focus)(struct RelicVM *caller, int handle);
    int (*count)(struct RelicVM *caller);
    int (*handle_at)(struct RelicVM *caller, int idx);
    int (*set_rect)(struct RelicVM *caller, int handle,
                    int x, int y, int w, int h);
    int (*get_rect)(struct RelicVM *caller, int handle, i32 out[4]);
    /* -> chars written (host may write fewer than the name's length if
     * cap is small; the result is always NUL-terminated when >= 0). */
    int (*name)(struct RelicVM *caller, int handle,
                char *buf, int cap);
    /* iconify=1 hides the window to the taskbar; 0 restores it. */
    int (*iconify)(struct RelicVM *caller, int handle, int iconify);
    /* Docks handle's window inside the caller's content rect (chrome-
     * less, z-locked above the caller, click-transparent). w<=0/h<=0
     * undocks. */
    int (*dock)(struct RelicVM *caller, int handle,
                int x, int y, int w, int h);
} RelicOSOps;

/* -------------------------------------------------------------------------
 * Embedded multi-VM host (src/core/mvm.c).
 *
 * Standalone runtimes (tigryxri, tigryxvm, tigryxcc-compiled exes) call
 * relic_mvm_attach() on the host VM after relic_vm_init(): it installs
 * the RelicOSOps vtable so @os_* builtins work, wraps the host's
 * gfx_flip so every parent frame first runs each spawned child one
 * slice, and provides fiber/VEH plumbing for child VMs rendering into
 * rects of the parent's window via gfx_guest_begin/end. `jit` may be
 * NULL (children then run interpreted). Platforms without support are
 * no-ops: os_ops stays NULL and @os_* returns -1 as usual.
 * relic_mvm_shutdown() frees all children; optional at process exit.
 * ---------------------------------------------------------------------- */
struct RelicJit;
void  relic_mvm_attach(RelicVM *vm, const struct RelicJit *jit);
void  relic_mvm_shutdown(void);

#define VM_MAX_FILES 16
typedef struct {
    bool   is_asset;
    FILE  *fp;           /* real file, or NULL for asset */
    const u8 RELIC_FAR *asset_data;   /* far: asset blobs can exceed 64K */
    u32    asset_size;
    u32    asset_pos;
} VMFileHandle;

typedef struct RelicBackend {
    void (*init)(RelicVM *vm);
    void (*shutdown)(RelicVM *vm);
    void (*gfx_clear)(RelicVM *vm);
    void (*gfx_sprite)(RelicVM *vm);
    void (*gfx_plot)(RelicVM *vm);
    void (*gfx_flip)(RelicVM *vm);
    void (*gfx_line)(RelicVM *vm);
    void (*gfx_window)(RelicVM *vm);
    void (*gfx_polygon)(RelicVM *vm);
    void (*gfx_polyline)(RelicVM *vm);
    void (*gfx_camera)(RelicVM *vm);
    void (*gfx_texture)(RelicVM *vm);
    void (*gfx_light)(RelicVM *vm);
    void (*gfx_mesh)(RelicVM *vm);
    void (*gfx_fog)(RelicVM *vm);
    void (*gfx_text)(RelicVM *vm);
    void (*gfx_viewport)(RelicVM *vm);
    void (*gfx_text_measure)(RelicVM *vm);
    void (*gfx_clip)(RelicVM *vm);
    void (*gfx_ellipse)(RelicVM *vm);
    void (*gfx_ellipse_filled)(RelicVM *vm);
    void (*gfx_rounded_rect)(RelicVM *vm);
    void (*gfx_rounded_rect_filled)(RelicVM *vm);
    /* @palette_upload(ptr, count) - 8bpp palette mode select/upload. NULL ->
     * no-op (true-colour backends, or DOS backends before this is wired up). */
    void (*gfx_palette_upload)(RelicVM *vm);
    /* @view(x,y,w,h) - render-surface rect for embedded guests (see
     * OP_GFX_VIEW). NULL -> no-op, which simply renders full-window. */
    void (*gfx_view)(RelicVM *vm);
    /* @view_capture / @view_present - retained guest frame (see
     * OP_GFX_VIEW_CAPTURE/PRESENT). NULL -> no-op. */
    void (*gfx_view_capture)(RelicVM *vm);
    void (*gfx_view_present)(RelicVM *vm);
    /* @draw_list(buf, len) - retained primitive buffer (see
     * OP_GFX_DRAW_LIST). NULL -> no-op. */
    void (*gfx_draw_list)(RelicVM *vm);
    /* @draw_mode(mode) - 2D blend mode, 0=copy 1=xor (see
     * OP_GFX_DRAW_MODE). NULL -> the op reports 0 and does nothing. */
    void (*gfx_draw_mode)(RelicVM *vm);
    /* @lines3d(...) - batch wireframe projection (see OP_GFX_LINES3D).
     * NULL -> the op returns -1 so callers use their bytecode fallback. */
    void (*gfx_lines3d)(RelicVM *vm);
    /* @grid3d(...) - grid-cell DDA raycaster (see OP_GFX_GRID3D).
     * NULL -> the op returns -1 so callers use their bytecode fallback. */
    void (*gfx_grid3d)(RelicVM *vm);
    /* Sector-based world renderer (see OP_WR_* above). NULL on every
     * backend except allegro-fixed-dos; the vm.c dispatcher no-ops safely
     * when unset, same treatment as any other backend-specific call. */
    void (*wr_palette)(RelicVM *vm);
    void (*wr_texture)(RelicVM *vm);
    void (*wr_flat)(RelicVM *vm);
    void (*wr_test)(RelicVM *vm);
    void (*wr_map)(RelicVM *vm);
    void (*wr_render)(RelicVM *vm);
    void (*wr_texmap)(RelicVM *vm);
    void (*wr_sky)(RelicVM *vm);
    void (*wr_flatshade)(RelicVM *vm);
    void (*wr_patch)(RelicVM *vm);
    void (*wr_sprite)(RelicVM *vm);
    /* @wr_blockmap/@wr_blocked - native BLOCKMAP upload and collision
     * probe (see OP_WR_BLOCKMAP/OP_WR_BLOCKED). NULL -> wr_blocked
     * reports -1 so guests fall back to their own path. */
    void (*wr_blockmap)(RelicVM *vm);
    void (*wr_blocked)(RelicVM *vm);
    void (*wr_blocked_line)(RelicVM *vm);
    void (*inp_key)(RelicVM *vm);
    void (*inp_keyheld)(RelicVM *vm);
    void (*inp_mouse)(RelicVM *vm);
    void (*inp_gamepad_count)(RelicVM *vm);
    void (*inp_gamepad)(RelicVM *vm);
    void (*sys_rand)(RelicVM *vm);
    void (*sys_millis)(RelicVM *vm);
    void (*sys_console)(RelicVM *vm);
    void (*sys_alloc)(RelicVM *vm);
    void (*sys_free)(RelicVM *vm);
    void (*sys_fopen)(RelicVM *vm);
    void (*sys_fread)(RelicVM *vm);
    void (*sys_fclose)(RelicVM *vm);
    void (*sys_fwrite)(RelicVM *vm);
    void (*sys_fseek)(RelicVM *vm);
    void (*sys_flen)(RelicVM *vm);
    void (*sys_fdir)(RelicVM *vm);
    void (*sys_fpath)(RelicVM *vm);
    void (*sys_args)(RelicVM *vm);
    void (*sys_isnum)(RelicVM *vm);
    void (*sys_sin)(RelicVM *vm);
    void (*sys_cos)(RelicVM *vm);
    void (*sys_sqrt)(RelicVM *vm);
    /* Extended math. NULL -> vm.c calls the libm single-precision
     * function. Two-arg ops read r1/r2; all write the float result to
     * r1. */
    void (*sys_exp)(RelicVM *vm);
    void (*sys_log)(RelicVM *vm);
    void (*sys_log10)(RelicVM *vm);
    void (*sys_pow)(RelicVM *vm);
    void (*sys_atan)(RelicVM *vm);
    void (*sys_atan2)(RelicVM *vm);
    void (*sys_tan)(RelicVM *vm);
    void (*sys_asin)(RelicVM *vm);
    void (*sys_acos)(RelicVM *vm);
    void (*sys_floor)(RelicVM *vm);
    void (*sys_ceil)(RelicVM *vm);
    void (*sys_round)(RelicVM *vm);
    void (*sys_fmod)(RelicVM *vm);
    void (*sys_fabs)(RelicVM *vm);
    void (*sys_sinh)(RelicVM *vm);
    void (*sys_cosh)(RelicVM *vm);
    void (*sys_tanh)(RelicVM *vm);
    void (*sys_platform)(RelicVM *vm);
    /* Web / HTTP (poll-based). NULL -> vm.c returns "not supported" (-1). */
    void (*sys_http_request)(RelicVM *vm);
    void (*sys_http_poll)(RelicVM *vm);
    void (*sys_http_status)(RelicVM *vm);
    void (*sys_http_read)(RelicVM *vm);
    void (*sys_http_close)(RelicVM *vm);
    /* ZIP extraction. NULL -> vm.c uses portable miniz implementation. */
    void (*sys_unzip)(RelicVM *vm);
    /* Delete a file or empty directory (@file_delete). NULL -> vm.c uses a
     * portable remove() implementation. */
    void (*sys_fdelete)(RelicVM *vm);
    /* Create a directory (@file_mkdir). NULL -> vm.c uses a portable
     * _mkdir()/mkdir() implementation. */
    void (*sys_fmkdir)(RelicVM *vm);
    /* Platform registration (@register). NULL -> vm.c uses the per-OS
     * implementation, which is what every current backend wants; this hook
     * exists for backends that need to intercept or sandbox installation. */
    void (*sys_register)(RelicVM *vm);
    /* Window chrome control for @register RELIC_REG_WIN_* actions:
     * minimise / maximise / state query for WINFLAG_FULLCONTENT scripts
     * that draw their own title-bar buttons. vm->r[1] in = action,
     * out = result (RELIC_REG_OK / state / RELIC_REG_ERR_UNSUP).
     * NULL -> the actions report unsupported. */
    void (*win_ctl)(RelicVM *vm);
    /* System clipboard (@clipboard_set / @clipboard_get). NULL -> vm.c
     * returns failure; there is no portable fallback. */
    void (*sys_clip_set)(RelicVM *vm);
    void (*sys_clip_get)(RelicVM *vm);
    /* Subprocess management (@process_spawn/poll/exit_code/read_stdout/
     * read_stderr/kill/close). NULL -> vm.c returns -1 / no-op, so
     * scripts degrade gracefully on platforms without process spawning
     * (DOS, web). Used by TIDE to launch `tigryx run <file>.tgx`. */
    void (*sys_proc_spawn)(RelicVM *vm);
    void (*sys_proc_poll)(RelicVM *vm);
    void (*sys_proc_exit_code)(RelicVM *vm);
    void (*sys_proc_read_out)(RelicVM *vm);
    void (*sys_proc_read_err)(RelicVM *vm);
    void (*sys_proc_kill)(RelicVM *vm);
    void (*sys_proc_close)(RelicVM *vm);
    /* Tiny socket server (@serve / @serve_reply). NULL -> vm.c returns
     * -1 / no-op so scripts degrade gracefully. Only wired under
     * TIGRYX_EXPERIMENTAL_SERVER builds. */
    void (*sys_serve)(RelicVM *vm);
    void (*sys_serve_reply)(RelicVM *vm);
    void (*snd_tone_freq)(RelicVM *vm);
    void (*snd_tone_id)(RelicVM *vm);
    void (*snd_play)(RelicVM *vm);
    void (*snd_music)(RelicVM *vm);
    void (*snd_mixer)(RelicVM *vm);
    /* Embedded-host guest isolation (tigryxos). A host that runs several
     * VMs on ONE shared backend_data (one GL context, one window) calls
     * begin() before each guest's run slice and end() after. x,y,w,h is
     * the guest's content rect in host logical px (top-left origin);
     * lw,lh is the guest's own logical surface size. begin() must save
     * any per-surface render state the backend keeps in backend_data
     * (3D mode, view/viewport rects, lights) and install the guest's
     * transform + clip; end() restores it. NULL -> the backend keeps no
     * such state (2D or per-VM backends). */
    void (*gfx_guest_begin)(RelicVM *vm, int x, int y, int w, int h,
                            int lw, int lh);
    void (*gfx_guest_end)(RelicVM *vm);
    /* Optional: wipe the shared depth buffer inside the slice clip the
     * current guest_begin installed. Hosts that share one depth buffer
     * across slices call this once per guest frame (before resuming a
     * guest that just finished a frame), so a lower guest's 3D pass
     * can't depth-shadow the frame a sibling draws on top. NULL ->
     * the backend keeps no shared depth state. */
    void (*gfx_guest_depth_clear)(RelicVM *vm);
    /* Optional retained guest frames. snap_capture copies the active
     * slice rect (as of guest_begin) into a backend-owned snapshot -
     * call it when a guest completes a frame; pass the previous snapshot
     * (or NULL) and keep the returned one. snap_present redraws a
     * snapshot into the active slice rect - for host frames in which
     * the guest finished no frame (pacing waits, preempted slices), so
     * its pane shows the last complete frame instead of flickering.
     * snap_free releases it. All NULL -> no retention. */
    void *(*gfx_guest_snap_capture)(RelicVM *vm, void *snap);
    void  (*gfx_guest_snap_present)(RelicVM *vm, void *snap);
    void  (*gfx_guest_snap_free)(void *snap);
    /* Optional: WINFLAG_CLOSE_REQUEST poll for embedded hosts (mvm).
     * Returns 1 once after the window close button was clicked, without
     * touching the physical key state. mvm routes this to the host VM
     * only, so a focused child's @key_held(27) poll can neither steal
     * the host's close request nor observe a phantom Escape. */
    int (*inp_close_requested)(RelicVM *vm);
} RelicBackend;

/* -------------------------------------------------------------------------
 * Debugger hook (optional)
 *
 * If vm->dbg is non-NULL, the VM consults it before executing each
 * instruction. The hot path stays a single pointer test + (when active)
 * a bitmap bit-test + comparison, so disabled debugging has near-zero cost.
 * ---------------------------------------------------------------------- */
typedef struct RelicDebugger {
    /* Called synchronously before executing the instruction at vm->pc.
     * Implementation typically runs an interactive REPL until the user
     * resumes (sets single_step / clears it / asks to continue). */
    void  (*on_break)(RelicVM *vm, void *user);
    void   *user;

    /* Per-PC breakpoint bitmap; length == ir->len bits (rounded up to bytes).
     * NULL disables breakpoints. */
    u8     *bp_bitmap;
    int     bp_bitmap_bytes;

    /* If true, on_break is called before every instruction. */
    bool    single_step;

    /* Step-over / step-out support. When >0, the VM only fires on_break
     * once vm->fp >= step_fp_floor (i.e. we are back at, or shallower than,
     * the recorded frame). Set by the REPL; cleared on each break. */
    u32     step_fp_floor;
} RelicDebugger;

/* -------------------------------------------------------------------------
 * Runtime trap (optional structured failure record)
 *
 * Populated whenever the VM halts itself abnormally (an @assert failure,
 * or a corrupt/impossible opcode - both already set vm->running = false
 * and a non-zero exit_code; this just gives a caller something better to
 * inspect than stderr text). kind == RELIC_TRAP_NONE (the zero value, so
 * a fresh/relic_vm_init'd VM starts with no trap) means "the program
 * hasn't hit one of these" - it says nothing about whether the program is
 * still running or exited normally via @exit/RET from main.
 * ---------------------------------------------------------------------- */
typedef enum {
    RELIC_TRAP_NONE = 0,
    RELIC_TRAP_ASSERT,
    RELIC_TRAP_UNKNOWN_OPCODE,
    RELIC_TRAP_STACK_OVERFLOW,
} RelicTrapKind;

typedef struct {
    RelicTrapKind kind;
    u32           pc;
    int           line;      /* 0  = unknown (see IRBuffer::line_for_pc) */
    int           file_id;   /* -1 = unknown (see IRBuffer::source_files) */
    char          message[256];
} RelicTrap;

struct RelicVM {
    /* Guest memory.  RELIC_FAR on ia16: real-mode DOS allocation
     * (_dos_allocmem) addressed with segment normalization via VMMEM8 -
     * guest objects may span 64K boundaries correctly. */
    u8 RELIC_FAR *mem;
    u32           mem_sz;
    i32           r[16];        /* register file r0..r15 */
    u32           pc;           /* word index into ir */
    u32           sp;           /* byte offset stack pointer */
    u32           fp;           /* byte offset frame pointer */
    bool          running;
    /* Cooperative preemption for embedded hosts (TigryxOS): when
     * nonzero, JIT-compiled loop back-edges and the interpreter call
     * yield_hook so a guest that has stopped @flip-ing can be suspended
     * instead of starving the host. Occupies padding after `running`
     * (offset 89) - no hot field moves. Cleared by the host. */
    u8            yield_req;
    IRBuffer     *ir;
    RelicBackend *backend;
    void         *backend_data; /* platform-specific window/context */
    int           exit_code;
    /* See RelicTrap above. Zero-initialised (RELIC_TRAP_NONE) by
     * relic_vm_init's memset; set by relic_op_sys_assert and the VM's
     * unknown-opcode safety net. */
    RelicTrap     trap;
    /* In-VM heap (for @alloc / @free). Pointers are offsets into vm->mem,
     * not host addresses, so they're compatible with OP_LOAD/OP_STORE,
     * @print, fread, etc. */
    u32           heap_base;    /* first byte of heap region (after data) */
    u32           heap_top;     /* one past last byte currently committed   */

    /* Backend-managed scratch slice within vm->mem for fixed strings
     * returned by @platform (and similar). Backends may write
     * NUL-terminated bytes here and store the resulting offsets in
     * caller-provided struct fields of type *u8. */
    u32           platform_strings_base; /* first byte in vm->mem */
    u32           platform_strings_size; /* total bytes reserved   */

    /* Optional debugger hook. NULL = run at full speed (default). */
    RelicDebugger *dbg;

    /* Asset table loaded from the .tlx trailer (or NULL if no assets).
     * Owned externally; the VM does not free it. */
    RelicAssets   *assets;

    /* Command-line arguments captured at process start. Used by the
     * @args builtin under the interpreter. The pointer array is owned
     * by the host process (argv from main()), the VM does not free it. */
    int            cli_argc;
    char         **cli_argv;

    /* Host argv[0] - the process's own executable path. cli_argv holds
     * only user passthrough args (argv[0] is stripped), so this is the
     * only reliable exe path for RELIC_REG_EXE_PATH on hosts where the
     * OS can't report it (DOS). Owned by the host; not freed. */
    const char    *exe_path;

    /* JIT compiler name (from the linked RelicJit), or "null" if none.
     * Set by the runner after relic_vm_init so backends can report it
     * via @platform. NULL means "not set" (treat as not available). */
    const char    *jit_name;

    /* 1 if JIT is active this run, 0 if --no-jit or no JIT available.
     * Set by the runner so backends can distinguish "disabled by user"
     * from "not available" when reporting via @platform. */
    int           jit_enabled;

    /* Optional HTTP proxy URL (e.g. "http://192.168.1.50:8080").
     * When set, DOS backends rewrite http:// URLs to route through the proxy
     * so plain-HTTP-only DOS builds can reach HTTPS servers via a TLS-terminating
     * proxy. NULL = no proxy (direct connections). */
    const char    *http_proxy;

    /* 1 if --verbose was passed on the command line, 0 otherwise.
     * Backends use this to gate diagnostic fprintf output. */
    int           verbose;

    /* AOT unwind anchor: generated x86 code saves the host esp of the
     * r_main entry frame here; the epilogue restores it so a HALT /
     * !running exit can unwind arbitrarily deep native call frames.
     * (x64 pins rbp for this; on i386 ebp is the register-file base, so
     * the anchor lives in the VM instead.)  Written only by generated
     * code; the interpreter and JITs never touch it. */
    u32           aot_sp_anchor;

    /* ---------------------------------------------------------------------
     * f64 register bank d0..d15.
     *
     * DELIBERATELY LAST. Do not move it next to r[16], however natural that
     * looks. The hot scalars sit at low offsets:
     *
     *     r @ 12   pc @ 76   sp @ 80   fp @ 84   running @ 88
     *
     * all below 128, so JIT-generated accesses encode with a ONE-byte
     * displacement. The x86 JIT aliases r14 -> vm->fp and r15 -> vm->sp via
     * [ebx + offsetof(...)] (jits/x86-win/jit_x86_win.c), which are among
     * the hottest memory references in generated code. Inserting these 128
     * bytes ahead of them pushes pc/sp/fp past 127 and forces a FOUR-byte
     * displacement on every such access - 3 extra bytes each, in every JIT,
     * penalising integer code that never touches a double.
     *
     * Keeping d[] here leaves the first 128 bytes of the struct unchanged.
     * New trailing fields may follow it; only the head is frozen.
     * ------------------------------------------------------------------- */
    double        d[16];

    /* Per-VM file handle table (see VMFileHandle above). Trailing field:
     * zeroed by relic_vm_init's memset, drained by relic_vm_free. */
    VMFileHandle  files[VM_MAX_FILES];

    /* Preemption hook, called when yield_req is set at a back-edge or
     * in the interpreter loop. NULL = no preemption (standalone runs).
     * For JIT'd guests the hook is called like a normal C function from
     * generated code - it may switch fibers and return later. Trailing
     * field; standalone callers never set it. */
    void        (*yield_hook)(RelicVM *vm);

    /* Embedded-host OS syscalls (see RelicOSOps / OP_SYS_OS). NULL = the
     * @os_* builtins return -1, which standalone programs can probe to
     * detect "not running inside an OS host". Trailing field. */
    const RelicOSOps *os_ops;
};

/* -------------------------------------------------------------------------
 * Parser state
 * ---------------------------------------------------------------------- */
typedef struct {
    Token  *tokens;
    int     ntokens;
    int     pos;
    Node   **pool;          /* array of pointers to individually-allocated nodes */
    int     npool;
    int     capacity;      /* allocated capacity for node pool */
    char    err[256];
    int     err_line;
} Parser;

/* -------------------------------------------------------------------------
 * Type checker state
 * ---------------------------------------------------------------------- */
typedef struct {
    SymTable   syms;
    IRBuffer  *ir;
    char       err[1024];
    int        err_line;
    int        err_file;   /* -1 = unknown; see TC_ERR/TC_ERR_NULL in typecheck.c */
    int        err_col;    /* -1 = unknown; ditto */
    char       cur_fn[RELIC_MAX_IDENT];
    Type      *cur_ret_type;
    /* While a class method's body is being checked: the ND_CLASS_DECL
     * the method belongs to (->bval is the class's `implicit` flag).
     * NULL inside ordinary functions. Used for implicit member lookup
     * and `new` resolution. */
    Node      *cur_class;
    /* struct registry (ia16: small - the whole TypeChecker must stay
     * under the 32K-per-struct limit) */
    struct { char name[RELIC_MAX_IDENT]; Node *decl; } structs[RELIC_MAX_STRUCTS];
    int nstructs;
} TypeChecker;

/* -------------------------------------------------------------------------
 * IR emitter state
 * ---------------------------------------------------------------------- */
typedef struct {
    IRBuffer  *ir;
    SymTable  *syms;
    TypeChecker *tc;
    Token     *tokens;     /* token array, for string-literal interning */
    char       err[256];
    int        err_line;
    /* Patch lists for forward jumps. Grown dynamically so that large
     * programs with many branches cannot silently drop a patch (which
     * would leave a JF/JMP target as 0, causing a jump to pc=0 and
     * eventual walk off the end of code). */
    struct { int patch_pc; int target_label; } *patches;
    int npatches;
    int patch_cap;
    int *labels;
    int nlabels;
    int label_cap;
    int next_label;
    /* Loop context for break/continue */
    int break_label[64];
    int continue_label[64];
    int loop_depth;

    /* Current source line/file (set by emit_stmt/emit_expr at each AST
     * node). Recorded into IRBuffer.line_for_pc/file_for_pc for every
     * emitted word. */
    int cur_line;
    int cur_file_id;
} IREmitter;

/* -------------------------------------------------------------------------
 * Top-level API
 * ---------------------------------------------------------------------- */
Symbol *sym_lookup(SymTable *st, const char *name);
Symbol *sym_lookup_global(SymTable *st, const char *name);

/* `initial_path`, if non-NULL/non-empty, seeds file id 0 so tokens lexed
 * before any `#line "path"` directive (i.e. the whole file, for a program
 * with no #import) still carry correct file attribution. NULL is fine for
 * callers that don't need file attribution (file_id stays -1 throughout). */
bool  relic_lex(Lexer *lx, const char *src, int len, const char *initial_path);
Node *relic_parse(Parser *p, Token *tokens, int ntokens);
bool  relic_typecheck(TypeChecker *tc, Node *root, IRBuffer *ir);
/* Evaluate `n` as a compile-time constant expression (literals, foldable
 * const idents, arithmetic/bitwise/shift/cast over them). Returns true and
 * writes the i32 bit-pattern to *out on success; false for anything
 * runtime-dependent. Must be called after tc_expr has populated n->type. */
bool  relic_fold_const(TypeChecker *tc, Node *n, i32 *out);
bool  relic_emit(IREmitter *em, Node *root, IRBuffer *ir, SymTable *syms, TypeChecker *tc, Token *tokens);

/* -------------------------------------------------------------------------
 * Structured compile diagnostics
 *
 * Each stage (lexer/parser/typechecker/emitter) still records its own
 * `char err[]` + `err_line` on failure - that plumbing is unchanged. This
 * struct is the stage-agnostic shape relic_compile_file() reports failures
 * in, so a caller (tigryxri's fatal() today; the web compiler's JSON
 * diagnostics later) has one thing to consume regardless of which stage
 * failed.
 *
 * `file_id`/`col`/`end_col` are populated where the underlying stage data
 * is actually reliable (see the per-stage comments in compile.c); where it
 * isn't, they stay -1 and callers should treat that as "whole line, file
 * unknown" rather than guessing.
 *
 * `file_id` alone isn't resolvable to a path once compilation has failed
 * (there's no surviving IRBuffer::source_files[] to index into - that
 * table is only populated on success), so `file_path` carries the actual
 * string too, resolved from the lexer's own file table while it's still
 * in scope inside relic_compile_source(). Empty string = unknown, same
 * meaning as file_id == -1. */
typedef enum {
    RELIC_DIAG_NONE = 0,
    RELIC_DIAG_READ,        /* could not read the input file */
    RELIC_DIAG_LEX,
    RELIC_DIAG_PARSE,
    RELIC_DIAG_TYPECHECK,
    RELIC_DIAG_EMIT,
} RelicDiagStage;

typedef struct {
    RelicDiagStage stage;
    char           message[256];
    int            file_id;                       /* -1 = unknown */
    char           file_path[RELIC_MAX_SOURCE_PATH]; /* "" = unknown */
    int            line;      /* 0   = unknown */
    int            col;       /* -1 = unknown */
    int            end_col;   /* -1 = unknown */
} RelicDiag;

/* Compile already-preprocessed Relic source held in memory (NOT .tgx -
 * callers resolve #import via tigryx_pp_run() or a tigryxpp
 * subprocess first) through lex -> parse -> typecheck -> emit.
 *
 * `display_path` is used only for diagnostics/tooling (e.g. IRBuffer's
 * source_path); it need not name a file that still exists - the caller may
 * have already deleted a temporary .tp by the time this returns.
 *
 * On success, returns true, fills `ir`, and sets `*out_main_fn_id` to the
 * function id of `main()` (or -1 if the program has none). On failure,
 * returns false and, if `out_diag` is non-NULL, fills it with the first
 * error encountered. `ir` is always safe to pass to relic_ir_free()
 * afterwards either way. */
bool  relic_compile_source(const char *src, int src_len, const char *display_path,
                           IRBuffer *ir, int *out_main_fn_id, RelicDiag *out_diag);

/* Convenience wrapper: reads `input_path` from disk and calls
 * relic_compile_source(). Prefer this unless the caller already has the
 * source in memory (e.g. tigryxri deletes a preprocessor temp file
 * immediately after reading it, before compiling - see main.c). */
bool  relic_compile_file(const char *input_path, IRBuffer *ir,
                         int *out_main_fn_id, RelicDiag *out_diag);
bool  relic_vm_init(RelicVM *vm, IRBuffer *ir, RelicBackend *backend, u32 mem_sz);
void  relic_vm_run(RelicVM *vm);
/* Execute exactly one instruction at vm->pc, advancing pc. No-op if the VM
 * is halted or pc is past the end of code. Used by the JIT as a fallback
 * for opcodes it doesn't yet emit natively. */
void  relic_vm_step1(RelicVM *vm);
/* Report a VM stack overflow exactly as the interpreter's OP_ENTER guard
 * does (message + trap fields + running=false). vm->pc must hold the
 * address one past the faulting ENTER. Called directly by AOT-compiled
 * ENTER traps, which cannot re-run the instruction through step1 when
 * the embedded blob omits the instruction stream. */
void  relic_vm_stack_overflow(RelicVM *vm);
void  relic_vm_free(RelicVM *vm);

/* Pre-production splash banner. Opens a temporary window, draws the
 * "Made with Tigryx" / "Preproduction Alpha" text, waits for space,
 * then closes the window so the program's own @window works normally.
 * No-op on headless/web backends. */
void  relic_splash_banner(RelicVM *vm);

const char *relic_token_name(TokenKind k);
const char *relic_opcode_name(u8 op);
unsigned    relic_op_dreg_mask(u8 op);   /* bit0/1/2: dst/srca/srcb is d-bank */
void        relic_ir_disasm(IRBuffer *ir, int from, int to);

/* Bytecode serialization.
 *
 * `include_debug` (write) embeds an optional debug section - the source
 * file table plus RLE-compressed line_for_pc/file_for_pc - after the
 * string table. It's opt-in (see tigryxri's --debug-info flag) so ordinary
 * shipped .tlx files don't grow; false reproduces the exact pre-existing
 * on-disk format. Readers always understand both forms (see the `flags`
 * byte in ir.c) - an old reader given a new file, or vice versa, degrades
 * to "no debug info" rather than failing to load. */
bool        relic_bytecode_save(IRBuffer *ir, const char *path, int main_fn_id, bool include_debug);
bool        relic_bytecode_write(IRBuffer *ir, FILE *f, int main_fn_id, bool include_debug);
bool        relic_bytecode_load(IRBuffer *ir, const char *path, int *out_main_fn_id);
bool        relic_bytecode_load_mem(IRBuffer *ir, const void RELIC_FAR *data, size_t len,
                                    int *out_main_fn_id);

/* Asset table API */
/* Write asset trailer to an already-open FILE* (after the bytecode payload). */
bool        relic_assets_write(RelicAssets *assets, FILE *f);
/* Load asset trailer from a .tlx file (file-based). Returns NULL if no trailer. */
RelicAssets *relic_assets_load(const char *path);
/* Load asset trailer from an in-memory blob (same format as relic_bytecode_load_mem). */
RelicAssets *relic_assets_load_mem(const void RELIC_FAR *data, size_t len);
/* Same, but assets->data points INTO `data` instead of copying the blobs.
 * `data` must outlive the table (e.g. a compiled program's static blob) -
 * avoids doubling the footprint of large packed assets on DOS. */
RelicAssets *relic_assets_load_mem_ref(const void RELIC_FAR *data, size_t len);
/* Free a RelicAssets table returned by relic_assets_load / relic_assets_load_mem. */
void        relic_assets_free(RelicAssets *assets);
/* Look up an asset by name. Returns pointer into assets->data and sets *out_size,
 * or returns NULL if not found. */
const u8 RELIC_FAR *relic_assets_find(RelicAssets *assets, const char *name, u32 *out_size);
/* Append a single file from the host filesystem into a RelicAssets being built.
 * asset_name is the name under which it will be accessible at runtime.
 * Returns false on I/O error or if the table is full. */
bool        relic_assets_add_file(RelicAssets *assets, const char *host_path,
                                  const char *asset_name);
/* Append all files matching a glob pattern into assets.
 * E.g. pattern "mydata/[star]" packs every file directly under mydata/.
 * Preserves directory structure: mydata/sub/foo.dat is stored as that name.
 * Returns number of files added, or -1 on error. */
int         relic_assets_add_glob(RelicAssets *assets, const char *pattern);

/* Debugger: interactive line-level debugger over a compiled IRBuffer.
 * Spins up a TTY REPL, then runs `vm` with a RelicDebugger attached.
 * Returns the program's exit code. */
int         relic_debug_run(RelicVM *vm, IRBuffer *ir, int main_fn_id);

/* -------------------------------------------------------------------------
 * JIT compiler interface (optional, pluggable per platform)
 *
 * Like RelicBackend, exactly one RelicJit is linked into the executable.
 * Each implementation lives in src/jits/<arch-os>/ and exports a single
 * `RelicJit relic_jit = { ... };` value. The "null" JIT is the default and
 * its compile() returns false, so the --jit flag silently falls back to the
 * interpreter on platforms that don't yet have a real code generator.
 *
 * Usage in main: after IR is built and the VM is initialised (pc/sp/fp set
 * up exactly as for relic_vm_run), call relic_jit.compile(ir,...) once. On
 * success, relic_jit.run(code, vm) replaces relic_vm_run(vm). On failure,
 * fall through to the interpreter.
 * ---------------------------------------------------------------------- */
typedef struct RelicJitCode RelicJitCode; /* opaque, owned by the JIT impl */

typedef struct RelicJit {
    const char *name;   /* e.g. "null", "x86-win" - for --version / logging */

    /* Compile the entire IR. On success returns true and writes *out_code.
     * On failure returns false; if err is non-NULL, writes a short reason
     * (NUL-terminated, truncated to err_sz). out_code may be left untouched. */
    bool (*compile)(IRBuffer *ir, RelicJitCode **out_code, char *err, int err_sz);

    /* Run the compiled blob against vm. vm must be in the same state the
     * interpreter expects (pc/sp/fp set, running=true). Returns vm->exit_code. */
    int  (*run)    (RelicJitCode *code, RelicVM *vm);

    /* Release any memory / executable pages owned by `code`. */
    void (*free)   (RelicJitCode *code);
} RelicJit;

/* Helpers used by the debugger and assembly listing. */
int         relic_pc_to_fn(IRBuffer *ir, u32 pc);
int         relic_line_to_pc(IRBuffer *ir, const char *path, int line);

/* -------------------------------------------------------------------------
 * Windows console visibility
 *
 * tigryx.exe / tigryxri.exe / tigryxvm.exe / tigryxpp.exe are ordinary
 * CONSOLE-subsystem binaries, so typing `tigryx ...` at a prompt behaves
 * exactly like any other CLI tool: the shell waits for it, shares its
 * console, stdout and exit codes just work. The trade-off is that Windows
 * pops a brand-new console window whenever one of these programs is
 * launched with no console already attached - i.e. double-clicked from
 * Explorer, a Start Menu shortcut, or a file association.
 *
 * Two mechanisms suppress that window when it isn't wanted:
 *
 *   - `tigryx run <file> --no-console`: the file associations and Start Menu
 *     shortcut install.tgx registers for .tp/.tgx/.tlx/.tla pass this flag,
 *     so double-clicking one never shows a window. Typing `tigryx run` at a
 *     prompt without the flag behaves like a normal CLI command.
 *
 *   - `tigryx bind <file> --no-console`: bakes RELIC_BIND_FLAG_NO_CONSOLE
 *     into the bound executable's footer (see above), so the game/program
 *     it produces never shows a console when double-clicked either. Without
 *     the flag, the bound exe shows a console like any other CLI tool.
 *
 * On Windows 95/98/ME the runtime console-hiding above is not available
 * (no AttachConsole, no GetConsoleWindow; CREATE_NO_WINDOW is ignored and
 * DETACHED_PROCESS is documented-buggy), so the win95 targets build an
 * extra set of GUI-subsystem twins - tigryxw, tigryxriw, tigryxvmw,
 * tigryxppw - and the Win9x installer registers file associations against
 * tigryxriw.exe directly (`tigryxriw --no-console --input "%1"`). Going
 * straight to the interpreter also drops the dispatcher hop: a double-
 * clicked .tgx is one process (tigryxriw, which spawns tigryxppw itself
 * for preprocessing) instead of three. On NT-family Windows the original
 * `tigryxw run --no-console "%1"` association is still used because the
 * runtime hiding works there and the dispatcher hop is cheap.
 *
 * Runtime support is three functions:
 *
 *   relic_console_init() - call at the very top of main(), before any I/O.
 *     On GUI-subsystem builds (tigryxw, tigryxvmw), tries AttachConsole to
 *     grab the parent shell's console so CLI use works. On Explorer launches
 *     this fails silently — no console exists and none is shown. On
 *     CONSOLE-subsystem builds (tigryx, tigryxvm) this is a no-op.
 *
 *   relic_console_attach(no_console) - call once `no_console` is known.
 *     When true, sets a global that makes relic_console_ensure() a complete
 *     no-op: no console is EVER shown under any circumstance. Also hides any
 *     existing console that is exclusively ours (CONSOLE-subsystem children).
 *
 *   relic_console_ensure() - call right before writing anything a user
 *     should see (a fatal error, @console, --version, ...). If --no-console
 *     was set, this is a no-op — the console is NEVER shown. Otherwise,
 *     un-hides or allocates a console as needed.
 *
 * All three are cheap to call repeatedly (idempotent) and are safe on every
 * platform: elsewhere they compile to no-ops. */
void relic_console_init(void);
void relic_console_attach(int no_console);
void relic_console_ensure(void);

/* -------------------------------------------------------------------------
 * Preprocessor temp-file helper.
 *
 * tigryx / tigryxri / tigryxrc all spawn tigryxpp as a separate process to
 * lower .tgx to .tp.  Writing the .tp next to the input risks clobbering a
 * user's real .tp and leaves stray files on crash.  This helper builds a
 * path in the OS temp dir instead, tagged with the PID so concurrent
 * invocations don't collide.
 *
 *   input_rx_path - the .tgx file being preprocessed (used for the basename)
 *   out           - receives the temp .tp path (e.g. "%TEMP%/tigryx-foo-1234.tp")
 *   out_sz        - size of out; should be >= 1024
 *
 * Returns 1 on success, 0 on failure (out left empty).  The temp dir is
 * obtained via GetTempPathA (Windows), $TMPDIR (Unix), $TEMP/$TMP (DOS),
 * with a fallback to the input's own directory if none is available.
 * The path is always absolute on Windows/Linux/macOS; on DOS it follows
 * whatever $TEMP points at. */
int relic_pp_tmp_path(const char *input_rx_path, char *out, size_t out_sz);

#endif /* TIGRYX_H */
