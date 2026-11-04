/*
 * main.c
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
 * main.c - Relic interpreter / bytecode compiler entry point (relic-ri)
 *
 * Usage:
 *   relic-ri --input <file>                           -- compile/run based on extension
 *   relic-ri --input <file> --output-bytecode <f.tlx> -- save bytecode, no run
 *   relic-ri --input <file> --output-assembly [f.tla] -- save assembly listing, no run
 *   relic-ri --version                                -- print version
 *   relic-ri --verbose --input <file>                 -- show relic: progress messages
 *   relic-ri --debug --input <file>                   -- run under interactive debugger
 *
 * Input file extensions:
 *   .tp    Relic source          - lex/parse/typecheck/emit then run or save
 *   .tgx   Relic extended source - preprocess first, then as .tp
 *   .tla  Relic assembly        - assemble then run or save
 *   .tlx  Relic bytecode        - load and run directly (no compile)
 *
 * NOTE: End users should use `relic run / bind` rather than invoking relic-ri directly.
 */
#define _GNU_SOURCE
#include "tigryx.h"
#include "tigryx_pragma.h"
#include "tigryx_pp.h"
#include "tigryx_tier.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#define RELIC_COPYRIGHT "Copyright (c) 2026 Scintillae Limited. All rights reserved."
#ifndef RELIC_PLATFORM_ID
#  define RELIC_PLATFORM_ID "unknown"
#endif
#ifndef RELIC_BACKEND_ID
#  define RELIC_BACKEND_ID "unknown"
#endif

#if !defined(_WIN32)
#include <unistd.h>
#include <sys/stat.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#elif defined(RELIC_DOS)
#include <dos.h>
#include <process.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

/* Provided by whichever backend library is linked into this build
 * (see src/backends/<name>/). Every backend exports exactly one symbol
 * with this name so the core does not need to know about specific ports. */
extern RelicBackend relic_backend;

/* Provided by whichever JIT library is linked into this build
 * (see src/jits/<name>/). The default "null" JIT's compile() returns false,
 * so --jit silently falls back to the interpreter when no real JIT is built. */
extern RelicJit relic_jit;

static int g_verbose = 0;
#ifdef RELIC_IA16
/* DOS paths are short (8.3) - keep the argv0 copy small for DGROUP. */
static char g_argv0[128] = {0};
#else
static char g_argv0[1024] = {0};
#endif

static void fatal(const char *title, const char *msg) {
    /* A fatal error must be visible even if the console was hidden as an
     * Explorer launch (see tigryx.h). */
    relic_console_ensure();
    fprintf(stderr, "%s: %s\n", title, msg);
    fflush(stderr);
}

static void vlog(const char *fmt, ...) {
    if(!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* -------------------------------------------------------------------------
 * File utilities
 * ---------------------------------------------------------------------- */
static char *read_file(const char *path, int *out_len) {
    FILE *f = fopen(path, "rb");
    if(!f) { fprintf(stderr, "error: cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if(!buf) { fclose(f); return NULL; }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    buf[nread] = 0;
    fclose(f);
    if(out_len) *out_len = (int)nread;
    return buf;
}

static const char *file_ext(const char *path) {
    const char *dot = strrchr(path, '.');
#ifdef RELIC_RISCOS
    /* On RISC OS '.' is the directory separator; leafnames carry the
     * extension as a '/ext' suffix (host "foo.tgx" -> leaf "foo/tgx").
     * A '/' after the last '.' is therefore the real extension marker. */
    {
        const char *sl = strrchr(path, '/');
        if(sl && sl > (dot ? dot : path)) {
            static char extbuf[300];
            size_t n = strlen(sl + 1);
            if(n < sizeof(extbuf) - 2) {
                extbuf[0] = '.';
                memcpy(extbuf + 1, sl + 1, n + 1);
                return extbuf;
            }
        }
    }
#endif
    return dot ? dot : "";
}

/* Extension equality. DOS (DJGPP) always uppercases filenames, and users
 * on other platforms may type uppercase extensions too, so we compare
 * case-insensitively on ALL platforms. */
static int ext_ieq(const char *ext, const char *want) {
    while(*ext && *want) {
        char a = *ext++, b = *want++;
        if(a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if(b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if(a != b) return 0;
    }
    return *ext == 0 && *want == 0;
}

/* -------------------------------------------------------------------------
 * Bound executable output (--output-executable)
 *
 * Layout: [ runtime .exe bytes ][ .b bytecode bytes ][ 32-byte footer ]
 * Footer at the very end of the file:
 *      char     magic[8] = "RLCBIND\x02"
 *      uint64_t payload_offset    (LE)
 *      uint64_t payload_size      (LE)
 *      uint32_t heap_bytes        (LE, 0 = use compiled-in default)
 *      uint32_t flags             (LE, bit 0 = RELIC_BIND_FLAG_NO_CONSOLE)
 *
 * The runtime (`tigryxvm.exe`) detects this footer in its own image at startup
 * and loads the embedded bytecode automatically. See try_load_embedded() in
 * src/core/tigryx.c.
 * ---------------------------------------------------------------------- */
/* RELIC_BIND_MAGIC, _MAGIC_SIZE, _FOOTER_SIZE are defined in tigryx.h */

/* Locate a sibling executable next to argv[0]. Writes the full path into
 * `out` and returns 1 on success. Used to find `tigryxvm[.exe]` from the
 * directory containing `tigryxri[.exe]`. */
static int sibling_exe_path(const char *argv0, const char *sibling,
                            char *out, size_t out_sz) {
    char self[1024] = {0};
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, self, (DWORD)sizeof self);
    if(n == 0 || n >= sizeof self) {
        snprintf(self, sizeof self, "%s", argv0);
    }
#elif defined(RELIC_DOS)
    /* DJGPP/DOS: sibling path resolution not supported */
    (void)argv0; (void)sibling; (void)out; (void)out_sz;
    return 0;
#elif defined(__APPLE__)
    uint32_t exe_sz = (uint32_t)(sizeof self - 1);
    if(_NSGetExecutablePath(self, &exe_sz) != 0)
        snprintf(self, sizeof self, "%s", argv0);
#else
#  if defined(__sun)
    /* Solaris: no /proc/self/exe; path/a.out needs 8+, getexecname on 7. */
    if(!relic_sun_exepath(self, sizeof self))
        snprintf(self, sizeof self, "%s", argv0);
#  else
    ssize_t r = readlink("/proc/self/exe", self, sizeof self - 1);
    if(r <= 0) snprintf(self, sizeof self, "%s", argv0);
    else self[r] = '\0';
#  endif
#endif

#if defined(_WIN32)
    const char *sep = "\\";
    char *slash = strrchr(self, '\\');
    if(!slash) slash = strrchr(self, '/');
#else
    const char *sep = "/";
    char *slash = strrchr(self, '/');
#endif
    if(slash) *(slash + 1) = '\0'; else self[0] = '\0';

#if defined(_WIN32)
    snprintf(out, out_sz, "%s%s%s.exe", self, slash ? "" : sep, sibling);
#else
    snprintf(out, out_sz, "%s%s%s", self, slash ? "" : sep, sibling);
#endif
    return 1;
}

/* Write a self-contained executable that runs `ir` when launched.
 * `runtime_path` must point at a `relic[.exe]` binary (typically a sibling
 * of relicc). Returns false on any IO failure. */
static bool write_bound_executable(IRBuffer *ir, int main_fn_id,
                                   RelicAssets *assets,
                                   const char *runtime_path,
                                   const char *out_path,
                                   u32 heap_bytes,
                                   u32 flags) {
    /* 1. Slurp the runtime executable. read_file() malloc's an extra NUL
     *    byte; we only write the first rt_size bytes so it's binary-safe. */
    int rt_size = 0;
    char *rt = read_file(runtime_path, &rt_size);
    if(!rt || rt_size <= 0) {
        fatal("Error", "Failed to read runtime executable for --output-executable");
        free(rt);
        return false;
    }

    /* 2. Open output and copy the runtime verbatim. */
    FILE *out = fopen(out_path, "wb");
    if(!out) { fatal("Error", "Failed to open output executable"); free(rt); return false; }
    if(fwrite(rt, 1, (size_t)rt_size, out) != (size_t)rt_size) {
        fclose(out); free(rt); fatal("Error", "Short write to output executable"); return false;
    }
    free(rt);

    /* 3. Build the payload (bytecode + optional asset trailer) in a temp
     *    file first.  relic_assets_write stores dir_offset = ftell(f) as an
     *    absolute file position; the runtime's relic_assets_load_mem treats
     *    it as relative to the payload start.  Writing to a fresh temp file
     *    keeps those semantics correct. */
#if defined(RELIC_IA16)
    /* tmpfile() pulls tmpnam->_sprintf_r.float_impl->svfprintf->dtoa -
     * newlib's float printf chain (~15K) - into DGROUP-adjacent .text and
     * statics.  A named temp + remove() avoids it entirely. */
    char tmp_path[1024];
    FILE *tmp = NULL;
    if(relic_pp_tmp_path(out_path, tmp_path, sizeof tmp_path))
        tmp = fopen(tmp_path, "w+b");
#define TMP_CLOSE() do { fclose(tmp); remove(tmp_path); } while(0)
#else
    FILE *tmp = tmpfile();
#define TMP_CLOSE() fclose(tmp)
#endif
    if(!tmp) { fclose(out); fatal("Error", "Failed to create temp file for payload"); return false; }

    /* Bound executables never embed debug info - they're a "shipped"
     * artifact just like a plain --output-bytecode .tlx without
     * --debug-info. */
    if(!relic_bytecode_write(ir, tmp, main_fn_id, false)) {
        fclose(out); TMP_CLOSE(); fatal("Error", "Failed to write bytecode payload"); return false;
    }

    if(assets && assets->count > 0) {
        if(!relic_assets_write(assets, tmp)) {
            fclose(out); TMP_CLOSE(); fatal("Error", "Failed to write asset trailer"); return false;
        }
    }

    /* 4. Copy the payload into the bound executable. */
    long payload_off = ftell(out);
    if(payload_off < 0) { fclose(out); TMP_CLOSE(); fatal("Error", "ftell failed"); return false; }

    fseek(tmp, 0, SEEK_END);
    long payload_size = ftell(tmp);
    if(payload_size < 0) { fclose(out); TMP_CLOSE(); fatal("Error", "ftell failed"); return false; }
    rewind(tmp);

    char buf[4096];
    size_t remaining = (size_t)payload_size;
    while(remaining > 0) {
        size_t chunk = remaining < sizeof buf ? remaining : sizeof buf;
        if(fread(buf, 1, chunk, tmp) != chunk) {
            fclose(out); TMP_CLOSE(); fatal("Error", "Failed to read payload temp file"); return false;
        }
        if(fwrite(buf, 1, chunk, out) != chunk) {
            fclose(out); TMP_CLOSE(); fatal("Error", "Short write to output executable"); return false;
        }
        remaining -= chunk;
    }
    TMP_CLOSE();

    /* 5. Footer (32 bytes). All scalar fields are little-endian on disk. */
    {
        uint8_t fbuf[24];
        relic_wr64le(fbuf + 0,  (uint64_t)payload_off);
        relic_wr64le(fbuf + 8,  (uint64_t)payload_size);
        relic_wr32le(fbuf + 16, heap_bytes);
        relic_wr32le(fbuf + 20, flags);
        fwrite(RELIC_BIND_MAGIC, 1, RELIC_BIND_MAGIC_SIZE, out);
        fwrite(fbuf, 1, sizeof fbuf, out);
    }

    if(fclose(out) != 0) { fatal("Error", "Failed to close output executable"); return false; }

#if !defined(_WIN32) && !defined(RELIC_DOS)
    /* Make it executable for the owner+group+others (rwxr-xr-x). */
    chmod(out_path, 0755);
#endif
#undef TMP_CLOSE
    return true;
}

/* -------------------------------------------------------------------------
 * Assembly output (shared by --output-assembly)
 * ---------------------------------------------------------------------- */
static void write_assembly(IRBuffer *ir, int main_fn_id, FILE *out, const char *source_path) {
    fprintf(out, "; Relic assembly listing\n");
    fprintf(out, "; Source: %s\n", source_path);
    fprintf(out, "; Instructions: %d\n", ir->len);
    fprintf(out, "; Functions: %d\n", ir->nfuncs);
    fprintf(out, "; Main function ID: %d\n", main_fn_id);
    fprintf(out, "; String table size: %lu bytes\n", (unsigned long)ir->strings_len);
    fprintf(out, "; Globals size: %ld bytes\n", (long)ir->globals_size);
    fprintf(out, "\n");

    fprintf(out, "; Function table\n");
    for(int i=0; i<ir->nfuncs; i++) {
        fprintf(out, "; fn%d: offset = %ld", i, (long)IRFNOFF(ir, i));
        if(i == main_fn_id) fprintf(out, " (main)");
        fprintf(out, "\n");
    }
    fprintf(out, "\n");

    fprintf(out, "; Code\n");
    for(int i=0; i<ir->len; i++) {
        u32 instr = IRCODE(ir, i);
        u8  op    = IR_OP(instr);

        for(int fn=0; fn<ir->nfuncs; fn++) {
            if(IRFNOFF(ir, fn) == i) {
                fprintf(out, "\n; --- fn%d%s ---\n", fn, (fn == main_fn_id) ? " (main)" : "");
                break;
            }
        }

        unsigned dm = relic_op_dreg_mask(op);
        char rd = (dm & 1) ? 'd' : 'r';
        char ra = (dm & 2) ? 'd' : 'r';
        char rb = (dm & 4) ? 'd' : 'r';
        if(op == OP_LDC32) {
            fprintf(out, "%04d  %-12s  %c%d, %c%d, %c%d  imm=%ld\n",
                    i, relic_opcode_name(op),
                    rd, IR_DST(instr), ra, IR_SRCA(instr), rb, IR_SRCB(instr),
                    (long)IR_IMM12(instr));
            i++;
            if(i < ir->len)
                fprintf(out, "%04d  .imm32        %lu\n", i, (unsigned long)IRCODE(ir, i));
        } else if(op == OP_DLDC) {
            /* DLDC carries a 64-bit literal as TWO payload words, lo then
             * hi. Both print as bare .imm32 so the output re-assembles
             * through the same path as LDC32's payload. */
            fprintf(out, "%04d  %-12s  %c%d, %c%d, %c%d  imm=%ld\n",
                    i, relic_opcode_name(op),
                    rd, IR_DST(instr), ra, IR_SRCA(instr), rb, IR_SRCB(instr),
                    (long)IR_IMM12(instr));
            for(int w = 0; w < 2; w++) {
                i++;
                if(i < ir->len)
                    fprintf(out, "%04d  .imm32        %lu\n", i, (unsigned long)IRCODE(ir, i));
            }
        } else if(op == OP_JMP || op == OP_JT || op == OP_JF) {
            /* PC-relative displacement: shown as the absolute target */
            i32 tgt = IR_JTARGET(instr, i);
            u8 src = IR_SRCA(instr);
            fprintf(out, "%04d  %-12s  r0, %c%d, r0  tgt=%ld\n",
                    i, relic_opcode_name(op), ra, src, (long)tgt);
        } else if(op == OP_ENTER) {
            /* Frame size is a 20-bit unsigned field in bits [19:0]; the
             * register nibbles are repurposed, so print the wide value. */
            fprintf(out, "%04d  %-12s  r0, r0, r0  imm=%lu\n",
                    i, relic_opcode_name(op), (unsigned long)(instr & 0xFFFFFu));
        } else {
            fprintf(out, "%04d  %-12s  %c%d, %c%d, %c%d  imm=%ld\n",
                    i, relic_opcode_name(op),
                    rd, IR_DST(instr), ra, IR_SRCA(instr), rb, IR_SRCB(instr),
                    (long)IR_IMM12(instr));
        }
    }

    if(ir->strings_len > 0) {
        fprintf(out, "\n; String table\n");
        int pos = 0;
        char sbuf[512];
        while(pos < (int)ir->strings_len) {
            relic_far_str(sbuf, IRSTRP(ir, pos), sizeof sbuf);
            int len = (int)strlen(sbuf);
            if(len == 0) { pos++; continue; } /* skip null sentinel slot */
            fprintf(out, "; [%04d] \"%s\"\n", pos, sbuf);
            pos += len + 1;
        }
    }
}

/* -------------------------------------------------------------------------
 * Assembler (.a -> IRBuffer)
 * ---------------------------------------------------------------------- */
#if defined(RELIC_IA16)
/* ia16: the text assembler needs sscanf, which drags in newlib's wide-char
 * scanner chain (svfiscanf -> iswspace -> _jp2uc table = ~14K of DGROUP)
 * plus locale/mbtowc. With DGROUP at 64K total that leaves the near heap
 * empty and every malloc/fopen fails. .tla assembly is a developer path
 * unused on this target (the harness feeds pre-assembled .tlx), so stub
 * it out and keep the heap for the interpreter. */
static int assemble_file(const char *asm_path, IRBuffer *ir, int *out_main_fn_id) {
    (void)ir; (void)out_main_fn_id;
    fprintf(stderr, "error: .tla assembly is not supported on this build "
                    "('%s'): compile to .tlx on a host build instead\n",
            asm_path);
    return 0;
}
#else
static u8 parse_opcode(const char *name) {
    static const struct { const char *name; u8 op; } op_table[] = {
        {"LDC", OP_LDC}, {"LDC32", OP_LDC32},
        {"LD8", OP_LD8}, {"LD16", OP_LD16}, {"LD32", OP_LD32},
        {"ST8", OP_ST8}, {"ST16", OP_ST16}, {"ST32", OP_ST32},
        {"LEA", OP_LEA}, {"MOV", OP_MOV},
        {"ADD", OP_ADD}, {"ADDI", OP_ADDI},
        {"SUB", OP_SUB}, {"SUBI", OP_SUBI},
        {"MUL", OP_MUL}, {"DIV", OP_DIV}, {"MOD", OP_MOD}, {"NEG", OP_NEG},
        {"FADD", OP_FADD}, {"FSUB", OP_FSUB}, {"FMUL", OP_FMUL}, {"FDIV", OP_FDIV}, {"FNEG", OP_FNEG},
        {"FCEQ", OP_FCEQ}, {"FCLT", OP_FCLT}, {"FCLE", OP_FCLE},
        {"DCEQ", OP_DCEQ}, {"DCLT", OP_DCLT}, {"DCLE", OP_DCLE},
        {"DADD", OP_DADD}, {"DSUB", OP_DSUB}, {"DMUL", OP_DMUL},
        {"DDIV", OP_DDIV}, {"DNEG", OP_DNEG}, {"DMOV", OP_DMOV},
        {"DLD", OP_DLD}, {"DST", OP_DST}, {"DLDC", OP_DLDC},
        {"DSQRT", OP_DSQRT}, {"DSIN", OP_DSIN}, {"DCOS", OP_DCOS},
        {"DTAN", OP_DTAN}, {"DASIN", OP_DASIN}, {"DACOS", OP_DACOS},
        {"DATAN", OP_DATAN}, {"DATAN2", OP_DATAN2}, {"DEXP", OP_DEXP},
        {"DLOG", OP_DLOG}, {"DLOG10", OP_DLOG10}, {"DPOW", OP_DPOW},
        {"DFLOOR", OP_DFLOOR}, {"DCEIL", OP_DCEIL}, {"DROUND", OP_DROUND},
        {"DFMOD", OP_DFMOD}, {"DFABS", OP_DFABS}, {"DSINH", OP_DSINH},
        {"DCOSH", OP_DCOSH}, {"DTANH", OP_DTANH},
        {"I2F", OP_I2F}, {"F2I", OP_F2I},
        {"I2D", OP_I2D}, {"D2I", OP_D2I}, {"F2D", OP_F2D},
        {"D2F", OP_D2F}, {"U2D", OP_U2D}, {"D2U", OP_D2U},
        {"AND", OP_AND}, {"OR", OP_OR}, {"XOR", OP_XOR}, {"NOT", OP_NOT},
        {"SHL", OP_SHL}, {"SHR", OP_SHR}, {"SAR", OP_SAR},
        {"ANDI", OP_ANDI}, {"ORI", OP_ORI},
        {"CEQ", OP_CEQ}, {"CNE", OP_CNE},
        {"CLT", OP_CLT}, {"CLE", OP_CLE},
        {"CGT", OP_CGT}, {"CGE", OP_CGE},
        {"CLTU", OP_CLTU}, {"CLEU", OP_CLEU},
        {"JMP", OP_JMP}, {"JT", OP_JT}, {"JF", OP_JF}, {"JMPR", OP_JMPR},
        {"CALL", OP_CALL}, {"CALLR", OP_CALLR}, {"RET", OP_RET},
        {"ENTER", OP_ENTER}, {"LEAVE", OP_LEAVE}, {"CALLP", OP_CALLP},
        {"SX8", OP_SX8}, {"SX16", OP_SX16},
        {"ZX8", OP_ZX8}, {"ZX16", OP_ZX16},
        {"TRUNC8", OP_TRUNC8}, {"TRUNC16", OP_TRUNC16},
        {"PUSH", OP_PUSH}, {"POP", OP_POP}, {"NOP", OP_NOP}, {"HALT", OP_HALT},
        {"GFX_CLEAR", OP_GFX_CLEAR},
        {"GFX_SPRITE", OP_GFX_SPRITE},
        {"GFX_PLOT", OP_GFX_PLOT}, {"GFX_FLIP", OP_GFX_FLIP}, {"GFX_LINE", OP_GFX_LINE},
        {"GFX_WINDOW", OP_GFX_WINDOW}, {"GFX_POLYGON", OP_GFX_POLYGON}, {"GFX_POLYLINE", OP_GFX_POLYLINE},
        {"GFX_ELLIPSE", OP_GFX_ELLIPSE}, {"GFX_ELLIPSE_FILLED", OP_GFX_ELLIPSE_FILLED},
        {"GFX_ROUNDED_RECT", OP_GFX_ROUNDED_RECT}, {"GFX_ROUNDED_RECT_FILLED", OP_GFX_ROUNDED_RECT_FILLED},
        {"GFX_CAMERA", OP_GFX_CAMERA}, {"GFX_TEXTURE", OP_GFX_TEXTURE},
        {"GFX_LIGHT",  OP_GFX_LIGHT},  {"GFX_MESH",    OP_GFX_MESH},
        {"GFX_VIEWPORT", OP_GFX_VIEWPORT},
        {"GFX_TEXT_MEASURE", OP_GFX_TEXT_MEASURE}, {"GFX_CLIP", OP_GFX_CLIP},
        {"GFX_VIEW", OP_GFX_VIEW},
        {"GFX_VIEW_CAPTURE", OP_GFX_VIEW_CAPTURE},
        {"GFX_VIEW_PRESENT", OP_GFX_VIEW_PRESENT},
        {"GFX_PALETTE_UPLOAD", OP_GFX_PALETTE_UPLOAD},
        {"GFX_DRAW_MODE", OP_GFX_DRAW_MODE},
        {"GFX_LINES3D", OP_GFX_LINES3D},
        {"GFX_GRID3D", OP_GFX_GRID3D},
        {"INP_KEY", OP_INP_KEY}, {"INP_KEYHELD", OP_INP_KEYHELD},
        {"INP_GAMEPAD_COUNT", OP_INP_GAMEPAD_COUNT}, {"INP_GAMEPAD", OP_INP_GAMEPAD},
        {"SYS_RAND", OP_SYS_RAND}, {"SYS_MILLIS", OP_SYS_MILLIS},
        {"SYS_CONSOLE", OP_SYS_CONSOLE}, {"SYS_SNPRINT", OP_SYS_SNPRINT},
        {"SYS_ASSERT", OP_SYS_ASSERT},
        {"SYS_ALLOC", OP_SYS_ALLOC}, {"SYS_FREE", OP_SYS_FREE},
        {"SYS_MEMCPY", OP_SYS_MEMCPY}, {"SYS_MEMSET", OP_SYS_MEMSET},
        {"SYS_FOPEN", OP_SYS_FOPEN}, {"SYS_FREAD", OP_SYS_FREAD}, {"SYS_FCLOSE", OP_SYS_FCLOSE},
        {"SYS_FWRITE", OP_SYS_FWRITE}, {"SYS_ARGS", OP_SYS_ARGS}, {"SYS_ISNUM", OP_SYS_ISNUM},
        {"SYS_SIN", OP_SYS_SIN}, {"SYS_COS", OP_SYS_COS}, {"SYS_SQRT", OP_SYS_SQRT}, {"SYS_PLATFORM", OP_SYS_PLATFORM},
        {"SYS_EXP", OP_SYS_EXP}, {"SYS_LOG", OP_SYS_LOG}, {"SYS_LOG10", OP_SYS_LOG10},
        {"SYS_POW", OP_SYS_POW}, {"SYS_ATAN", OP_SYS_ATAN}, {"SYS_ATAN2", OP_SYS_ATAN2},
        {"SYS_TAN", OP_SYS_TAN}, {"SYS_ASIN", OP_SYS_ASIN}, {"SYS_ACOS", OP_SYS_ACOS},
        {"SYS_FLOOR", OP_SYS_FLOOR}, {"SYS_CEIL", OP_SYS_CEIL}, {"SYS_ROUND", OP_SYS_ROUND},
        {"SYS_FMOD", OP_SYS_FMOD}, {"SYS_FABS", OP_SYS_FABS},
        {"SYS_SINH", OP_SYS_SINH}, {"SYS_COSH", OP_SYS_COSH}, {"SYS_TANH", OP_SYS_TANH},
        {"SYS_UNZIP", OP_SYS_UNZIP}, {"SYS_REGISTER", OP_SYS_REGISTER},
        {"SYS_FDELETE", OP_SYS_FDELETE}, {"SYS_FMKDIR", OP_SYS_FMKDIR},
        {"SYS_CLIP_SET", OP_SYS_CLIP_SET}, {"SYS_CLIP_GET", OP_SYS_CLIP_GET},
        {"SND_TONE_FREQ", OP_SND_TONE_FREQ}, {"SND_TONE_ID", OP_SND_TONE_ID}, {"SND_PLAY", OP_SND_PLAY},
        {"SND_MUSIC", OP_SND_MUSIC}, {"SND_MIXER", OP_SND_MIXER},
        {NULL, 0}
    };
    for(int i=0; op_table[i].name; i++)
        if(strcmp(name, op_table[i].name) == 0) return op_table[i].op;
    return 0xFF;
}

static u8 parse_register(const char *s) {
    /* 'dN' is accepted too: f64 ops print d-bank operands, and the 4-bit
     * field holds the same index either way - the bank lives in the op. */
    if((s[0] == 'r' || s[0] == 'd') && isdigit((unsigned char)s[1])) {
        int reg = atoi(s + 1);
        if(reg >= 0 && reg <= 15) return (u8)reg;
    }
    return 0xFF;
}

static char *trim_str(char *s) {
    char *end;
    while(isspace((unsigned char)*s)) s++;
    if(*s == 0) return s;
    end = s + strlen(s) - 1;
    while(end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

static int assemble_file(const char *asm_path, IRBuffer *ir, int *out_main_fn_id) {
    FILE *f = fopen(asm_path, "r");
    if(!f) { fprintf(stderr, "error: cannot open '%s'\n", asm_path); return 0; }
    /* fn_offsets[] may be written (--- fnN ---) before any instruction
     * grows the buffer, so force the (ia16: far) tables to exist now. */
    if(!relic_ir_reserve(ir, 1)) {
        fprintf(stderr, "error: out of memory\n");
        fclose(f); return 0;
    }

    int main_fn_id = 0;
    char line[1024];
    int line_num = 0;

    while(fgets(line, sizeof(line), f)) {
        line_num++;
        char *trimmed = trim_str(line);

        if(strstr(trimmed, "--- fn") && strstr(trimmed, "---")) {
            char *fn_start = strstr(trimmed, "fn");
            if(fn_start) {
                int fn_id = atoi(fn_start + 2);
                if(fn_id >= 0 && fn_id < RELIC_MAX_FUNCS) {
                    IRFNOFF(ir, fn_id) = ir->len;
                    if(fn_id >= ir->nfuncs) ir->nfuncs = fn_id + 1;
                }
            }
            continue;
        }

        if(strstr(trimmed, "Main function ID:")) {
            char *id_start = strstr(trimmed, "ID:");
            if(id_start) main_fn_id = atoi(id_start + 3);
            continue;
        }
        if(strstr(trimmed, "Globals size:")) {
            char *p = strstr(trimmed, "Globals size:");
            ir->globals_size = atoi(p + (int)strlen("Globals size:"));
            continue;
        }

        if(trimmed[0] == '\0' || trimmed[0] == ';') continue;

        for(char *p = trimmed; *p; p++) if(*p == ',') *p = ' ';

        {
            char addr_buf[16], dir[16];
            unsigned long raw_l;   /* %u is 16-bit on ia16 - parse wide */
            if(sscanf(trimmed, "%15s %15s %lu", addr_buf, dir, &raw_l) == 3 &&
               strcmp(dir, ".imm32") == 0) {
                u32 raw = (u32)raw_l;
                if(!relic_ir_reserve(ir, ir->len + 1)) {
                    fprintf(stderr, "%s:%d: out of memory growing IR buffer\n", asm_path, line_num);
                    fclose(f); return 0;
                }
                IRCODE(ir, ir->len++) = raw;
                continue;
            }
        }

        char addr_str[16], opcode_str[32], dst_str[8], srca_str[8], srcb_str[8], field_name[8], field_val[32];
        /* First try to parse with tgt= (for JMP/JT/JF) */
        int parsed = sscanf(trimmed, "%15s %31s %7s %7s %7s %7[^=]=%31s",
                            addr_str, opcode_str, dst_str, srca_str, srcb_str, field_name, field_val);
        if(parsed >= 2) {
            u8 op = parse_opcode(opcode_str);
            if(op == 0xFF) {
                fprintf(stderr, "%s:%d: unknown opcode '%s'\n", asm_path, line_num, opcode_str);
                fclose(f); return 0;
            }
            u8 dst  = (parsed >= 3) ? parse_register(dst_str)  : 0;
            u8 srca = (parsed >= 4) ? parse_register(srca_str) : 0;
            u8 srcb = (parsed >= 5) ? parse_register(srcb_str) : 0;

            if(!relic_ir_reserve(ir, ir->len + 1)) {
                fprintf(stderr, "%s:%d: out of memory growing IR buffer\n", asm_path, line_num);
                fclose(f); return 0;
            }

            if(parsed >= 6 && strcmp(field_name, "tgt") == 0) {
                /* Jump: absolute tgt= in the text, PC-relative in the IR */
                if(op != OP_JMP && op != OP_JT && op != OP_JF) {
                    fprintf(stderr, "%s:%d: tgt= only valid for JMP/JT/JF\n", asm_path, line_num);
                    fclose(f); return 0;
                }
                long tgt = atol(field_val);
                int  pc  = ir->len;
                if(!IR_JFITS(pc, tgt)) {
                    fprintf(stderr, "%s:%d: jump from %d to %ld exceeds the 20-bit displacement\n",
                            asm_path, line_num, pc, tgt);
                    fclose(f); return 0;
                }
                IRCODE(ir, ir->len++) = IR_JSET(IR_ENCODE(op, 0, srca, 0, 0), pc, tgt);
            } else if(parsed >= 6 && strcmp(field_name, "imm") == 0) {
                /* Regular instruction with imm field. OP_ENTER is
                 * special: its frame size occupies the full 20-bit field
                 * [19:0], not the 12-bit immediate. */
                i32 imm = atoi(field_val);
                if(op == OP_ENTER) {
                    IRCODE(ir, ir->len++) = ((u32)op << 24) | ((u32)imm & 0xFFFFFu);
                } else {
                    IRCODE(ir, ir->len++) = IR_ENCODE(op, dst, srca, srcb, imm);
                }
            } else {
                /* Fallback: no field name, try old format with imm= */
                char imm_str[32];
                int parsed2 = sscanf(trimmed, "%15s %31s %7s %7s %7s imm=%31s",
                                    addr_str, opcode_str, dst_str, srca_str, srcb_str, imm_str);
                if(parsed2 >= 6) {
                    i32 imm = atoi(imm_str);
                    if(op == OP_ENTER) {
                        IRCODE(ir, ir->len++) = ((u32)op << 24) | ((u32)imm & 0xFFFFFu);
                    } else {
                        IRCODE(ir, ir->len++) = IR_ENCODE(op, dst, srca, srcb, imm);
                    }
                } else {
                    /* No imm field at all, use 0 */
                    IRCODE(ir, ir->len++) = IR_ENCODE(op, dst, srca, srcb, 0);
                }
            }
        }
    }

    fclose(f);
    if(out_main_fn_id) *out_main_fn_id = main_fn_id;
    return 1;
}
#endif /* !RELIC_IA16 */

/* -------------------------------------------------------------------------
 * VM runner (shared)
 * ---------------------------------------------------------------------- */
static int run_ir(IRBuffer *ir, int main_fn_id, int debug_mode, int jit_mode, u32 mem_sz,
                  RelicAssets *assets, int cli_argc, char **cli_argv,
                  const char *proxy_url) {
    if(main_fn_id < 0 || main_fn_id >= ir->nfuncs) {
        fatal("Error", "No main() found"); return 1;
    }
    vlog("[tigryxri] main fn_id=%d, starting vm at pc=%d\n",
         main_fn_id, IRFNOFF(ir, main_fn_id));

    vlog("[tigryxri] vm_init mem_sz=%lu\n", (unsigned long)mem_sz);
    RelicVM vm = {0};
    if(!relic_vm_init(&vm, ir, &relic_backend, mem_sz)) {
        fatal("Error", "VM init failed"); return 1;
    }
    vm.verbose = g_verbose;
    vlog("[tigryxri] vm_init ok\n");
    vm.assets = assets;
    vm.cli_argc = cli_argc;
    vm.cli_argv = cli_argv;
    /* user args exclude argv[0]; RELIC_REG_EXE_PATH needs the real one */
    vm.exe_path = g_argv0[0] ? g_argv0 : NULL;
    vm.jit_name = relic_jit.name;
    vm.jit_enabled = jit_mode;
    vm.http_proxy = proxy_url;

    /* Embedded multi-VM host: installs the @os_* vtable and wraps
     * gfx_flip so spawned children get a slice per frame. No-op where
     * fibers/VEH aren't available. */
    relic_mvm_attach(&vm, &relic_jit);

    relic_splash_banner(&vm);

    { i32 v = (i32)ir->len; vm.sp -= 4; relic_far_write(vm.mem, vm.sp, &v, 4); }
    { i32 v = (i32)vm.fp;   vm.sp -= 4; relic_far_write(vm.mem, vm.sp, &v, 4); }
    vm.fp = vm.sp;
    vm.pc = (u32)IRFNOFF(ir, main_fn_id);

    int code;
    if(debug_mode) {
        code = relic_debug_run(&vm, ir, main_fn_id);
    } else if(jit_mode) {
        RelicJitCode *jcode = NULL;
        char jerr[256] = {0};
        int compiled = (relic_jit.compile && relic_jit.compile(ir, &jcode, jerr, sizeof jerr));
#if defined(RELIC_DOS)
        /* DOS: vlog goes to text-mode video which is invisible while
         * Allegro holds a VESA mode. Mirror JIT status into TIGRYX.LOG so
         * it survives the run (the Allegro backend already opens this
         * file for @console output; we append independently here). */
        {
            /* Open with "w" to start a fresh log per run; the Allegro
             * backend opens TIGRYX.LOG with "a" later so its banner and
             * @console output append after this [jit] line. */
            FILE *jl = fopen("TIGRYX.LOG", "w");
            if(jl) {
                if(compiled) {
                    fprintf(jl, "[jit] '%s' compiled OK, running JIT\n",
                            relic_jit.name);
                } else {
                    fprintf(jl, "[jit] '%s' compile FAILED (%s); using interpreter\n",
                            relic_jit.name, jerr[0] ? jerr : "no reason given");
                }
                fflush(jl);
                fclose(jl);
            }
        }
#endif
        if(compiled) {
            vlog("[tigryxri] jit '%s' compiled, running\n", relic_jit.name);
            code = relic_jit.run(jcode, &vm);
            relic_jit.free(jcode);
        } else {
            vlog("[tigryxri] jit '%s' unavailable (%s); using interpreter\n",
                 relic_jit.name, jerr[0] ? jerr : "no reason given");
            relic_vm_run(&vm);
            code = vm.exit_code;
        }
    } else {
        relic_vm_run(&vm);
        code = vm.exit_code;
    }

    vlog("[tigryxri] vm exited code=%d\n", code);
    relic_mvm_shutdown();
    relic_vm_free(&vm);
    return code;
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */
static void print_usage(void) {
    relic_console_ensure();
    fprintf(stderr,
        "\n"
        RELIC_COPYRIGHT "\n"
        "\n"
        "BETA SOFTWARE - this is a beta release. There will be bugs, and the\n"
        "API and behaviour may change between releases without notice.\n"
        "\n"
        "Version:  tigryxri " RELIC_VERSION " BETA\n"
        "Platform: " RELIC_PLATFORM_ID "\n"
        "Backend:  " RELIC_BACKEND_ID "\n"
        "\n"
        "Usage:\n"
        "  tigryxri --input <file>                          compile/run based on extension\n"
        "  tigryxri --input <file> --output-bytecode <f>    save bytecode, no run\n"
        "  tigryxri --input <file> --output-assembly [f]    save assembly listing, no run\n"
        "  tigryxri --input <file> --output-executable <f>  produce standalone exe, no run\n"
        "  tigryxri <file>                                  (file as first positional)\n"
        "  tigryxri --version                               print version\n"
        "\n"
        "Options:\n"
        "  --input <file>              input file (.tp source, .a assembly, .b bytecode)\n"
        "  --output-bytecode <file.b>  save bytecode to file, do not run\n"
        "  --debug-info                with --output-bytecode: embed a source\n"
        "                              file/line map so the .tlx can be debugged/\n"
        "                              inspected without the original source\n"
        "  --output-assembly [file.a]  save assembly listing (stdout if no file), do not run\n"
        "  --output-executable <file>  produce a standalone exe that bundles the tigryxvm\n"
        "                              runtime + this program's bytecode, do not run\n"
        "  --runtime <file>            override the runtime used by\n"
        "                              --output-executable (default: sibling 'tigryxvm[.exe]')\n"
        "  --heap <size>               VM heap size with optional unit suffix\n"
        "                              (k/K=KB, m/M=MB, g/G=GB; default is MB)\n"
        "                              0 = smallest heap the program can run in\n"
        "                              may also be set in source: #pragma arg heap <size>\n"
        "                              (an explicit --heap overrides the pragma)\n"
        "  --no-console                with --output-executable: hide the console\n"
        "                              when the produced .exe is launched from Explorer\n"
        "  --verbose                   show relic: progress messages\n"
        "  --debug                     run under interactive debugger\n"
        "  --jit                       force enable JIT (auto-enabled if available)\n"
        "  --no-jit                    disable JIT, use interpreter\n"
        "  --tier low|medium|high|super\n"
        "                              pin the Adaptive Build tier (default:\n"
        "                              auto-detect; TGX_TIER env also works).\n"
        "                              Stamps --output-bytecode's .tlx, and sets\n"
        "                              the reference for the tier-mismatch warning\n"
        "  --platform <id>               Adaptive Build: target platform id for\n"
        "                              cross-compiles (ceiling/caps/stamp identity)\n"
        "  --backend <id>                Adaptive Build: target backend id\n"
#if !defined(_WIN32)
        "  --proxy <url>               HTTP proxy for plain-HTTP backends (DOS; non-curl POSIX)\n"
#endif
        "  --version                   print version and exit\n"
        "\n"
        "  <file> may also be supplied as the first positional argument.\n"
    );
}

int main(int argc, char **argv) {
    /* tigryxri is always spawned by tigryx (never a file-association target
     * itself), so if it's ever launched directly and got a console of its
     * own, hide it - see tigryx.h. fatal()/print_usage()/@console un-hide it. */
    relic_console_attach(1);

#if defined(RELIC_RISCOS)
    /* Disarm FPA trap enables before any guest float op runs: SCL's
     * default FP context would otherwise kill the task on 1.0/0.0. */
    relic_fpe_untrap();
#endif

    /* Capture argv[0] for DJGPP executable path resolution */
    if(argc > 0 && argv[0]) {
        snprintf(g_argv0, sizeof g_argv0, "%s", argv[0]);
    }

    vlog("[tigryxri] started argv[0]=%s\n", argv[0] ? argv[0] : "(null)");
    for(int i = 1; i < argc; i++) vlog("[tigryxri]   arg[%d]=%s\n", i, argv[i]);

    if(argc < 2) { print_usage(); return 1; }

    const char *input_path       = NULL;
    const char *bytecode_out     = NULL;
    const char *assembly_out     = NULL;
    const char *executable_out   = NULL;
    const char *runtime_path_arg = NULL;
    int         output_assembly  = 0;
    int         debug_mode       = 0;
    int         debug_info       = 0;   /* --debug-info: embed source file/line
                                         * map in --output-bytecode's .tlx
                                         * (see relic_bytecode_write) */
    int         jit_mode         = (strcmp(relic_jit.name, "null") != 0);  /* Auto-enable if available */
    int         no_jit           = 0;
    u32         mem_sz           = RELIC_MEM_SIZE;
    int         heap_given       = 0;
    int         no_console       = 0;
    int         tier_cli_given   = 0;
    const char *tier_cli         = NULL;
    const char *platform_cli     = NULL;
    const char *backend_cli      = NULL;
    const char *proxy_url        = NULL;
    int         native_class     = 0;   /* --native-class: pass class syntax
                                         * through tigryxpp for the
                                         * compiler */

    /* User passthrough args: everything after a "--" separator. Exposed to
     * the program via @args (0-indexed, no program name, no relic flags).
     * The `relic` dispatcher always appends "--" before them. */
    char      **user_argv        = NULL;
    int         user_argc        = 0;

    for(int i=1; i<argc; i++) {
        if(strcmp(argv[i], "--") == 0) {
            user_argv = &argv[i + 1];
            user_argc = argc - (i + 1);
            break;
        }
        if(strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if(strcmp(argv[i], "--debug") == 0) {
            debug_mode = 1;
        } else if(strcmp(argv[i], "--debug-info") == 0) {
            debug_info = 1;
        } else if(strcmp(argv[i], "--jit") == 0) {
            jit_mode = 1;
        } else if(strcmp(argv[i], "--no-jit") == 0) {
            jit_mode = 0;
            no_jit = 1;
        } else if(strcmp(argv[i], "--tier") == 0) {
            /* Adaptive Build override for this process: stamps
             * --output-bytecode .tlx and is the reference tier for the
             * bytecode-mismatch warning. A `#pragma arg tier` in the .tp
             * does NOT override an explicit --tier (flag wins, matching
             * the --heap convention). Deferred to after the loop so a
             * later --platform already set the ceiling it validates
             * against. */
            if(i+1 < argc) { tier_cli = argv[++i]; tier_cli_given = 1; }
            else { fprintf(stderr, "error: --tier requires a value (low|medium|high|super)\n"); return 1; }
        } else if(strcmp(argv[i], "--platform") == 0) {
            /* Adaptive Build target identity for cross-compilation:
             * overrides the compiled-in platform id for ceiling/caps and
             * the .tlx stamp's notion of "the platform". e.g. building
             * BBC bytecode on a Windows host. */
            if(i+1 < argc) { platform_cli = argv[++i];
                           relic_tier_set_platform(platform_cli, relic_tier_backend()); }
            else { fprintf(stderr, "error: --platform requires a platform id\n"); return 1; }
        } else if(strcmp(argv[i], "--backend") == 0) {
            if(i+1 < argc) { backend_cli = argv[++i];
                           relic_tier_set_platform(relic_tier_platform(), backend_cli); }
            else { fprintf(stderr, "error: --backend requires a backend id\n"); return 1; }
        } else if(strcmp(argv[i], "--native-class") == 0) {
            /* Experimental native class support: forwarded to tigryxpp
             * below so class syntax reaches the compiler untouched. */
            native_class = 1;
        } else if(strcmp(argv[i], "--version") == 0) {
            relic_console_ensure();
            printf("relic-ri %s BETA (%s, backend-%s, jit=%s) built %s %s\n",
                   RELIC_VERSION, RELIC_PLATFORM_ID, RELIC_BACKEND_ID, relic_jit.name, __DATE__, __TIME__);
            printf("%s\n", RELIC_COPYRIGHT);
            return 0;
        } else if(strcmp(argv[i], "--input") == 0) {
            if(i+1 < argc) { input_path = argv[++i]; }
            else { fprintf(stderr, "error: --input requires a filename\n"); return 1; }
        } else if(strcmp(argv[i], "--output-bytecode") == 0) {
            if(i+1 < argc) { bytecode_out = argv[++i]; }
            else { fprintf(stderr, "error: --output-bytecode requires a filename\n"); return 1; }
        } else if(strcmp(argv[i], "--output-assembly") == 0) {
            output_assembly = 1;
            /* optional next argument: output path (only if it doesn't start with --) */
            if(i+1 < argc && argv[i+1][0] != '-') { assembly_out = argv[++i]; }
        } else if(strcmp(argv[i], "--output-executable") == 0) {
            if(i+1 < argc) { executable_out = argv[++i]; }
            else { fprintf(stderr, "error: --output-executable requires a filename\n"); return 1; }
        } else if(strcmp(argv[i], "--runtime") == 0) {
            if(i+1 < argc) { runtime_path_arg = argv[++i]; }
            else { fprintf(stderr, "error: --runtime requires a filename\n"); return 1; }
        } else if(strcmp(argv[i], "--heap") == 0) {
            if(i+1 < argc) { mem_sz = relic_parse_heap_size(argv[++i]); heap_given = 1; }
            else { fprintf(stderr, "error: --heap requires a size argument\n"); return 1; }
        } else if(strcmp(argv[i], "--no-console") == 0) {
            /* With --output-executable: baked into the produced .exe's footer
             * (see tigryx.h) rather than affecting tigryxri's own console.
             * When running, it means "this is an Explorer / file-association
             * launch": no console may be shown, so any child process we spawn
             * has to be a GUI-subsystem twin (tigryxppw, see below). The win95
             * associations point at tigryxriw.exe, the GUI twin of this
             * binary, and pass this flag. */
            no_console = 1;
#if defined(RELIC_DOS)
        } else if(strcmp(argv[i], "--colour-depth") == 0 ||
                  strcmp(argv[i], "--color-depth")  == 0) {
            /* DOS+TinyGL only: pin screen colour depth to 8/16/24/32.
             * DJGPP-gated so non-DOS builds never see the flag. */
            if(i+1 < argc) {
                int d = relic_parse_colour_depth(argv[++i]);
                if(d == 0) {
                    fprintf(stderr, "error: --colour-depth must be 8, 16, 24, or 32\n");
                    return 1;
                }
                g_relic_preferred_color_depth = d;
            } else {
                fprintf(stderr, "error: --colour-depth requires a value (8|16|24|32)\n");
                return 1;
            }
        } else if(strcmp(argv[i], "--res") == 0) {
            /* DOS only: physical mode for WINFLAG_STRETCH windows; ignored
             * otherwise. DJGPP-gated. */
            if(i+1 < argc) {
                int rw = 0, rh = 0;
                if(!relic_parse_mode_res(argv[++i], &rw, &rh)) {
                    fprintf(stderr, "error: --res must be <W>x<H> (e.g. 640x480)\n");
                    return 1;
                }
                g_relic_stretch_mode_w = rw;
                g_relic_stretch_mode_h = rh;
            } else {
                fprintf(stderr, "error: --res requires a value (<W>x<H>)\n");
                return 1;
            }
        } else if(strcmp(argv[i], "--skip-clear") == 0) {
            /* DOS only: opt out of the per-frame colour-buffer clear for
             * programs whose 3D scene fully covers the screen. Works on both
             * allegro-dos and allegro-tinygl-dos. See src/core/vm.c for details.
             * DJGPP-gated. */
            g_relic_skip_clear = 1;
        } else if(strcmp(argv[i], "--gfx-prof") == 0) {
            /* DOS only: 3D rendering profiler. Accumulates mesh/vert/tri/
             * pixel counters and dumps them to TIGRYX.LOG at shutdown.
             * DJGPP-gated. */
            g_relic_gfx_prof = 1;
#endif
#if !defined(_WIN32)
        } else if(strcmp(argv[i], "--proxy") == 0) {
            /* HTTP proxy URL for backends that speak plain HTTP only (DOS
             * lwIP, or POSIX socket fallback when built without libcurl).
             * A TLS-terminating proxy bridges the HTTPS gap. */
            if(i+1 < argc) { proxy_url = argv[++i]; }
            else { fprintf(stderr, "error: --proxy requires a URL argument\n"); return 1; }
#endif
        } else if(argv[i][0] != '-') {
            /* positional fallback */
            if(!input_path) input_path = argv[i];
            else { fprintf(stderr, "error: unexpected argument '%s'\n", argv[i]); return 1; }
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            print_usage(); return 1;
        }
    }

    /* Deferred --tier application: validated against the ceiling of the
     * --platform already seen (if any), so argument order doesn't matter. */
    if(tier_cli) {
        char err[160];
        if(relic_tier_set_override_name(tier_cli, err, sizeof err) != 0) {
            fprintf(stderr, "error: --tier: %s\n", err);
            return 1;
        }
    }

    if(!input_path) {
        fatal("Error", "No input file specified");
        print_usage(); return 1;
    }
    vlog("[tigryxri] input_path=%s jit_mode=%d no_jit=%d\n", input_path, jit_mode, no_jit);

    /* ------------------------------------------------------------------ */
    /* .tgx: run tigryxpp to produce a temporary .tp file, then compile that */
    /* ------------------------------------------------------------------ */
    char rx_tmp_path[1024] = {0};
    const char *ext = file_ext(input_path);
    if(ext_ieq(ext, ".tgx")) {
#if defined(__EMSCRIPTEN__) || defined(RELIC_RISCOS)
        /* No process model to spawn tigryxpp into under Emscripten (and no
         * meaningful executable-path resolution in MEMFS either) - run the
         * preprocessor in-process instead. See include/tigryx_pp.h.
         * RISC OS uses the same path: OS_CLI-spawning a second absolute
         * from inside a TaskWindow/Wimp context is fragile (child slot
         * sizing, escape handling, stderr routing), and tier/platform/
         * backend CLI flags were already applied to this process's tier
         * globals at arg-parse, so they propagate automatically. */
        if(!relic_pp_tmp_path(input_path, rx_tmp_path, sizeof(rx_tmp_path))) {
            fprintf(stderr, "error: cannot build temp path for '%s'\n", input_path);
            return 1;
        }
        if(tigryx_pp_run(input_path, rx_tmp_path) != 0) {
            fprintf(stderr, "error: tigryxpp failed\n");
            remove(rx_tmp_path);
            return 1;
        }
        input_path = rx_tmp_path;
        ext = ".tp";
#else
        /* Build path to tigryxpp: same directory as this executable. */
        char tigryxpp_path[1024] = {0};
#if defined(_WIN32)
        GetModuleFileNameA(NULL, tigryxpp_path, (DWORD)sizeof(tigryxpp_path)-1);
        char *bslash = strrchr(tigryxpp_path, '\\');
        if(bslash) *(bslash+1) = '\0'; else tigryxpp_path[0] = '\0';
        /* With --no-console (an Explorer / file-association launch) prefer the
         * GUI-subsystem twin tigryxppw.exe. Windows creates a console for a
         * console-subsystem child whenever the parent has none, and _spawnvp
         * can't pass the creation flags that suppress it - flags Win9x ignores
         * anyway. The child's PE subsystem is the only reliable guarantee.
         * Falls back to tigryxpp.exe where the twin isn't built/installed. */
        size_t pp_dir_len = strlen(tigryxpp_path);
        int pp_used_w = 0;
        if(no_console) {
            strncat(tigryxpp_path, "tigryxppw.exe",
                    sizeof(tigryxpp_path) - pp_dir_len - 1);
            FILE *pp_probe = fopen(tigryxpp_path, "rb");
            if(pp_probe) { fclose(pp_probe); pp_used_w = 1; }
            else tigryxpp_path[pp_dir_len] = '\0';
        }
        if(!pp_used_w)
            strncat(tigryxpp_path, "tigryxpp.exe",
                    sizeof(tigryxpp_path) - strlen(tigryxpp_path) - 1);
#elif defined(RELIC_DOS)
        /* DJGPP/DOS: use argv[0] captured in main() */
        if(g_argv0[0] == '\0') {
            fprintf(stderr, "error: cannot determine executable path for tigryxpp\n");
            return 1;
        }
        snprintf(tigryxpp_path, sizeof(tigryxpp_path), "%s", g_argv0);
        char *slash = strrchr(tigryxpp_path, '/');
        char *bslash = strrchr(tigryxpp_path, '\\');
        if(bslash && (!slash || bslash > slash)) slash = bslash;
        if(slash) *(slash+1) = '\0'; else tigryxpp_path[0] = '\0';
        strncat(tigryxpp_path, "tigryxpp.exe",
                sizeof(tigryxpp_path) - strlen(tigryxpp_path) - 1);
#else
        /* Find path to this executable, then replace basename with tigryxpp. */
#ifdef __APPLE__
        uint32_t exe_sz = (uint32_t)(sizeof(tigryxpp_path) - 1);
        if(_NSGetExecutablePath(tigryxpp_path, &exe_sz) != 0)
            snprintf(tigryxpp_path, sizeof(tigryxpp_path), "%s", argv[0]);
#else
#if defined(__sun)
        /* Solaris 8+ has /proc/self/path/a.out; getexecname covers 7. */
        if(!relic_sun_exepath(tigryxpp_path, sizeof(tigryxpp_path)))
            snprintf(tigryxpp_path, sizeof(tigryxpp_path), "%s", argv[0]);
#else
        ssize_t len = readlink("/proc/self/exe", tigryxpp_path, sizeof(tigryxpp_path)-1);
        if(len <= 0)
            snprintf(tigryxpp_path, sizeof(tigryxpp_path), "%s", argv[0]);
        else
            tigryxpp_path[len] = '\0';
#endif
#endif
        /* Replace basename with 'tigryxpp' */
        char *slash = strrchr(tigryxpp_path, '/');
        if(slash) *(slash+1) = '\0'; else tigryxpp_path[0] = '\0';
        strncat(tigryxpp_path, "tigryxpp",
                sizeof(tigryxpp_path) - strlen(tigryxpp_path) - 1);
#endif
        /* Build temp output path in the OS temp dir (not next to the input,
         * which could clobber a user's real .tp or leave stray files on crash).
         * rx_tmp_path is function-scoped so it remains valid after this block
         * (input_path will be set to it below). */
        if(!relic_pp_tmp_path(input_path, rx_tmp_path, sizeof(rx_tmp_path))) {
            fprintf(stderr, "error: cannot build temp path for '%s'\n", input_path);
            return 1;
        }

        /* Convert input to an absolute path for reliable process invocation.
         * The temp path from relic_pp_tmp_path is already absolute in the
         * normal case (GetTempPathA / /tmp), but resolve it on Windows too
         * in case the fallback used a relative input directory. */
        char abs_input_path[1024] = {0};
#ifdef _WIN32
        GetFullPathNameA(input_path, sizeof(abs_input_path), abs_input_path, NULL);
        char abs_tmp_path[1024] = {0};
        GetFullPathNameA(rx_tmp_path, sizeof(abs_tmp_path), abs_tmp_path, NULL);
        snprintf(rx_tmp_path, sizeof(rx_tmp_path), "%s", abs_tmp_path);
#elif defined(RELIC_DOS)
        /* DJGPP: use paths as-is, DJGPP handles path normalization internally */
        snprintf(abs_input_path, sizeof(abs_input_path), "%s", input_path);
#else
        /* realpath() requires the file to exist. Input does, output (tmp) doesn't
         * yet, so resolve input only.
         * NOTE: glibc's realpath with a non-NULL buffer requires the buffer to
         * be at least PATH_MAX (4096) bytes; we only have 1024. Passing NULL
         * makes realpath malloc the result, which we then copy and free.
         * Solaris' realpath() does not accept NULL - use a fixed buffer there. */
#if defined(__sun)
        {
            char rbuf[4096];
            if(realpath(input_path, rbuf)) {
                snprintf(abs_input_path, sizeof(abs_input_path), "%s", rbuf);
            } else {
                snprintf(abs_input_path, sizeof(abs_input_path), "%s", input_path);
            }
        }
#else
        char *resolved = realpath(input_path, NULL);
        if(resolved) {
            snprintf(abs_input_path, sizeof(abs_input_path), "%s", resolved);
            free(resolved);
        } else {
            snprintf(abs_input_path, sizeof(abs_input_path), "%s", input_path);
        }
#endif
#endif

        /* Invoke: tigryxpp [tier flags] <input.tgx> <tmp.r>
         * Adaptive Build: forward --tier/--platform/--backend so the
         * child emits the same TGX_* defines this process resolves to -
         * without this a `.tgx --tier low` compile gets host-tier
         * preprocessor defines but a low-tier .tlx stamp. */
        const char *pp_argv[12];
        int pp_argc = 0;
        pp_argv[pp_argc++] = tigryxpp_path;
        if(tier_cli)     { pp_argv[pp_argc++] = "--tier";     pp_argv[pp_argc++] = tier_cli; }
        if(platform_cli) { pp_argv[pp_argc++] = "--platform"; pp_argv[pp_argc++] = platform_cli; }
        if(backend_cli)  { pp_argv[pp_argc++] = "--backend";  pp_argv[pp_argc++] = backend_cli; }
        if(native_class) { pp_argv[pp_argc++] = "--native-class"; }
        pp_argv[pp_argc++] = abs_input_path;
        pp_argv[pp_argc++] = rx_tmp_path;
        pp_argv[pp_argc]   = NULL;
        int pp_ret;
#if defined(_WIN32)
        /* Use _spawnvp on Windows for reliable process execution */
        pp_ret = _spawnvp(_P_WAIT, tigryxpp_path, pp_argv);
#elif defined(RELIC_DOS)
        /* Use spawnlp on DOS for reliable process execution
         * (mode 0 = wait for child). gcc-ia16's libc only exposes the
         * underscore spelling (_spawnlp); DJGPP has both. */
#ifdef RELIC_IA16
        /* The fixed-signature spawnlp can't take the optional tier
         * flags - acceptable: cross-target compiles run on full hosts,
         * and the flags still apply to this process's own stamp. */
        if(tier_cli || platform_cli || backend_cli || native_class)
            fprintf(stderr, "warning: --tier/--platform/--backend/--native-class not "
                            "forwarded to tigryxpp on this build\n");
        pp_ret = _spawnlp(0, tigryxpp_path, tigryxpp_path, abs_input_path, rx_tmp_path, NULL);
#else
        pp_ret = spawnvp(0, tigryxpp_path, pp_argv);
#endif
#else
        char cmd[2048];
        char *w = cmd;
        size_t rem = sizeof(cmd);
        int wn = snprintf(w, rem, "\"%s\"", tigryxpp_path);
        w += wn; rem -= (size_t)wn;
        if(tier_cli)     { wn = snprintf(w, rem, " --tier \"%s\"", tier_cli);         w += wn; rem -= (size_t)wn; }
        if(platform_cli) { wn = snprintf(w, rem, " --platform \"%s\"", platform_cli); w += wn; rem -= (size_t)wn; }
        if(backend_cli)  { wn = snprintf(w, rem, " --backend \"%s\"", backend_cli);   w += wn; rem -= (size_t)wn; }
        if(native_class) { wn = snprintf(w, rem, " --native-class");                  w += wn; rem -= (size_t)wn; }
        snprintf(w, rem, " \"%s\" \"%s\"", abs_input_path, rx_tmp_path);
        vlog("tigryx: preprocessing: %s\n", cmd);
        pp_ret = system(cmd);
#endif

        if(pp_ret != 0) {
            fprintf(stderr, "error: tigryxpp failed (exit %d)\n", pp_ret);
            remove(rx_tmp_path);
            return 1;
        }
        input_path = rx_tmp_path;
        ext = ".tp";
#endif /* __EMSCRIPTEN__ */
    }

    /* Source-level tool arguments (`#pragma arg ...`). tigryxpp copies these
     * into the .tp; an explicit command-line flag always wins over them. */
    {
        RelicPragmaArgs pragma;
        relic_pragma_scan(input_path, &pragma);
        if(pragma.has_heap && !heap_given) {
            mem_sz = relic_parse_heap_size(pragma.heap);
            heap_given = 1;
            vlog("[tigryxri] #pragma arg heap %s\n", pragma.heap);
        }
        if(pragma.has_no_console && !no_console) no_console = 1;
#if defined(RELIC_DOS)
        if(pragma.has_colour_depth && g_relic_preferred_color_depth == 0)
            g_relic_preferred_color_depth = pragma.colour_depth;
#endif
        /* Adaptive Build: tigryxpp always emits one `#pragma arg tier`
         * naming the tier the source was preprocessed for. Adopt it as
         * this process's tier (unless --tier already pinned one) so the
         * .tlx stamp records the source's tier, and a .tp written by a
         * cross-tier -D/--tier build still carries its real tier through
         * to the stamp. */
        if(pragma.has_tier && pragma.tier >= 0 && !tier_cli_given) {
            relic_tier_set_override(pragma.tier);
            vlog("[tigryxri] #pragma arg tier %s\n",
                 relic_tier_name(pragma.tier));
        }
    }

    IRBuffer *ir = calloc(1, sizeof(IRBuffer));
    if(!ir) {
        /* Print to stdout too: DOS stderr can't be redirected, so under
         * DOSBox `prog > OUT` would show nothing at all. */
        printf("error: out of memory allocating IRBuffer (%lu bytes)\n",
               (unsigned long)sizeof(IRBuffer));
        fatal("Error", "Out of memory"); return 1;
    }

    int main_fn_id = -1;

    /* ------------------------------------------------------------------ */
    /* Load / compile the input                                            */
    /* ------------------------------------------------------------------ */
    RelicAssets *assets = NULL;
    if(ext_ieq(ext, ".tlx") || ext_ieq(ext, ".b")) {
        /* Binary bytecode: load directly */
        vlog("[tigryxri] loading %s\n", input_path);
        if(!relic_bytecode_load(ir, input_path, &main_fn_id)) {
            fatal("Error", "Failed to load bytecode file"); free(ir); return 1;
        }
        vlog("[tigryxri] loaded %d words, %d fns, %d bytes strings\n",
             ir->len, ir->nfuncs, ir->strings_len);
        /* Adaptive Build: the .tlx flags byte carries the tier the program
         * was compiled for. Warn — not fail — when it differs from this
         * machine's tier: running a HIGH build on LOW hardware is exactly
         * the accident the stamp exists to catch, but a LOW build on a
         * SUPER desktop is legitimate (the game just picks its simplest
         * assets), so a warning is the right severity. */
        {
            int host = relic_tier_detect();
            if(ir->tier >= 0 && ir->tier != host)
                fprintf(stderr,
                    "warning: '%s' was compiled for tier '%s' but this "
                    "platform is tier '%s' (platform %s); it may run slowly "
                    "or use features the program did not expect\n",
                    input_path, relic_tier_name(ir->tier),
                    relic_tier_name(host), RELIC_PLATFORM_ID);
        }
        /* Bytecode format v5 may carry the program's requested heap
         * (its `#pragma arg heap`, or a --heap given at bc time). The
         * command line still wins: only apply the embedded value when no
         * --heap was passed. Setting heap_given also lets a subsequent
         * bind bake the same value into the bound exe's footer. */
        if(ir->heap_size && !heap_given) {
            mem_sz = ir->heap_size;
            heap_given = 1;
            vlog("[tigryxri] .tlx embedded heap %lu bytes\n",
                 (unsigned long)ir->heap_size);
        }
        assets = relic_assets_load(input_path);
        if(assets) vlog("[tigryxri] %d embedded asset(s)\n", assets->count);

    } else if(ext_ieq(ext, ".tla") || ext_ieq(ext, ".a")) {
        /* Assembly: parse and assemble */
        vlog("[tigryxri] assembling %s\n", input_path);
        if(!assemble_file(input_path, ir, &main_fn_id)) {
            free(ir); return 1;
        }
        vlog("[tigryxri] assembled %d words, %d fns\n", ir->len, ir->nfuncs);

    } else {
        /* Relic source (.tp or anything else): lex/parse/typecheck/emit,
         * via relic_compile_source() (src/core/compile.c) - shared with
         * anything else that needs to compile already-preprocessed Relic
         * source (the future in-browser tigryxwc compiler). */
        vlog("[tigryxri] reading %s\n", input_path);
        int src_len = 0;
        char *src = read_file(input_path, &src_len);
        /* Remove temp .tp immediately after reading - prevents stray files on crash */
        if(rx_tmp_path[0]) { remove(rx_tmp_path); rx_tmp_path[0] = '\0'; }
        if(!src) { free(ir); return 1; }
        vlog("[tigryxri] %d bytes\n", src_len);

        RelicDiag diag;
        vlog("[tigryxri] compiling...\n");
        if(!relic_compile_source(src, src_len, input_path, ir, &main_fn_id, &diag)) {
            static const char *stage_title[] = {
                "Error", "Error", "Lex Error", "Parse Error", "Type Error", "Emit Error",
            };
            int ti = (diag.stage >= 0 && (size_t)diag.stage < sizeof(stage_title)/sizeof(stage_title[0]))
                     ? diag.stage : 0;
            /* Report the original source file:line[:col] (recovered via the
             * #line directives tigryxpp emits for #import) in the usual
             * compiler "file:line:col: error:" shape so it's clear which
             * source file failed when the .tp flattens several together. */
            char located[RELIC_MAX_SOURCE_PATH + 64];
            const char *m = diag.message;
            if(diag.file_path[0] && diag.line > 0) {
                if(diag.col > 0)
                    snprintf(located, sizeof(located), "%s:%d:%d",
                             diag.file_path, diag.line, diag.col);
                else
                    snprintf(located, sizeof(located), "%s:%d",
                             diag.file_path, diag.line);
                /* Stage messages still embed a "line N:" prefix - redundant
                 * now that the location is printed up front. */
                if(strncmp(m, "line ", 5) == 0) {
                    const char *q = m + 5;
                    while(*q >= '0' && *q <= '9') q++;
                    if(q[0] == ':' && q[1] == ' ') m = q + 2;
                }
            } else if(diag.file_path[0]) {
                snprintf(located, sizeof(located), "%s", diag.file_path);
            } else {
                located[0] = 0;
            }
            if(located[0]) {
                /* file:line[:col]: Parse Error: message — location first so
                 * editors/IDEs can match the gcc-style prefix for jump-to-
                 * error. fatal() puts its title first, so print directly. */
                char buf[sizeof(located) + 32 + sizeof(diag.message)];
                snprintf(buf, sizeof(buf), "%s: %s: %s", located, stage_title[ti], m);
                relic_console_ensure();
                fprintf(stderr, "%s\n", buf);
                fflush(stderr);
            } else {
                fatal(stage_title[ti], diag.message);
            }
            free(src); free(ir);
            return 1;
        }
        vlog("[tigryxri] compile ok - %d words, %d fns, main_fn_id=%d\n", ir->len, ir->nfuncs, main_fn_id);
        free(src);
    }

    /* Resolve `--heap 0` / `#pragma arg heap 0` now that the data segment
     * size is known: globals + strings + platform scratch + stack, plus a
     * margin so @alloc still has somewhere to go. */
    if(mem_sz == RELIC_HEAP_AUTO) {
        u32 data_end = (u32)ir->globals_size + (u32)ir->strings_len;
        data_end = (data_end + 3u) & ~3u;
        data_end += RELIC_PLATFORM_STR_SIZE;
        u32 need = data_end + (u32)RELIC_STACK_SIZE + RELIC_HEAP_AUTO_MARGIN;
        need = (need + 4095u) & ~4095u;   /* round to a page */
        mem_sz = need;
        vlog("[tigryxri] heap auto -> %u bytes (globals %u, strings %u)\n",
             mem_sz, (u32)ir->globals_size, (u32)ir->strings_len);
    }

    /* ------------------------------------------------------------------ */
    /* Output modes                                                        */
    /* ------------------------------------------------------------------ */
    if(bytecode_out) {
        if(main_fn_id < 0) { fatal("Error", "No main() found for bytecode output"); free(ir); return 1; }
        /* Embed the effective heap (from `#pragma arg heap` or a --heap
         * given to this compile) in the .tlx. mem_sz is already resolved:
         * `--heap 0` (auto) baked the computed minimum above. When no heap
         * was given, any heap_size already on ir survives a re-save. */
        if(heap_given) ir->heap_size = mem_sz;
        if(!relic_bytecode_save(ir, bytecode_out, main_fn_id, debug_info)) {
            fatal("Error", "Failed to save bytecode file"); free(ir); return 1;
        }
        vlog("[tigryxri] bytecode saved to %s\n", bytecode_out);
        free(ir);
        return 0;
    }

    if(executable_out) {
        if(main_fn_id < 0) { fatal("Error", "No main() found for --output-executable"); free(ir); return 1; }
        char rt_path[1024];
        if(runtime_path_arg) {
            snprintf(rt_path, sizeof rt_path, "%s", runtime_path_arg);
        } else {
            /* Pick the VM stub whose PE subsystem matches what the bound
             * program should do on an Explorer launch. A CONSOLE-subsystem
             * binary always gets a console window created by Windows before
             * main() runs - that's the flash --no-console must avoid - so
             * --no-console binds against the GUI-subsystem twin instead.
             * On non-Windows both names resolve to the same single binary
             * (tigryxvmw isn't built there), so fall back if absent. */
            const char *stub = no_console ? "tigryxvmw" : "tigryxvm";
            sibling_exe_path(argv[0], stub, rt_path, sizeof rt_path);
#if defined(_WIN32)
            if(no_console) {
                FILE *probe = fopen(rt_path, "rb");
                if(probe) fclose(probe);
                else sibling_exe_path(argv[0], "tigryxvm", rt_path, sizeof rt_path);
            }
#endif
        }
        vlog("[tigryxri] binding runtime '%s' + bytecode -> %s\n", rt_path, executable_out);
        if(heap_given)
            vlog("[tigryxri] baking heap size %lu MB into executable\n",
                 (unsigned long)(mem_sz / (1024uL * 1024uL)));
        if(no_console)
            vlog("[tigryxri] baking --no-console into executable\n");
        u32 bind_flags = no_console ? RELIC_BIND_FLAG_NO_CONSOLE : 0;
        /* Carry the same heap request inside the bound payload too (v5)
         * so the image is self-describing even if the footer is lost. */
        if(heap_given) ir->heap_size = mem_sz;
        if(!write_bound_executable(ir, main_fn_id, assets, rt_path, executable_out,
                                   heap_given ? mem_sz : 0, bind_flags)) {
            free(ir); return 1;
        }
        vlog("[tigryxri] executable written to %s\n", executable_out);
        free(ir);
        return 0;
    }

    if(output_assembly) {
        FILE *out = stdout;
        if(assembly_out) {
            out = fopen(assembly_out, "w");
            if(!out) { fatal("Error", "Failed to open assembly output file"); free(ir); return 1; }
            vlog("[tigryxri] writing assembly to %s\n", assembly_out);
        }
        write_assembly(ir, main_fn_id, out, input_path);
        if(assembly_out) { fclose(out); vlog("[tigryxri] done\n"); }
        free(ir);
        return 0;
    }

    /* ------------------------------------------------------------------ */
    /* Run                                                                 */
    /* ------------------------------------------------------------------ */
    /* Record source path on the IR so the debugger can show source. */
    snprintf(ir->source_path, sizeof(ir->source_path), "%s", input_path);

    if(debug_mode && jit_mode && !no_jit) {
        fatal("Error", "--jit and --debug are mutually exclusive");
        free(ir); return 1;
    }

    vlog("[tigryxri] run_ir main_fn_id=%d jit=%d\n", main_fn_id, jit_mode);
    /* Expose only the user passthrough args (after "--") to the program via
     * @args - never relic-ri's own argv (program name / --input / flags). */
    int exit_code = run_ir(ir, main_fn_id, debug_mode, jit_mode, mem_sz, assets,
                           user_argc, user_argv, proxy_url);
    vlog("[tigryxri] run_ir returned exit_code=%d\n", exit_code);
    relic_ir_free(ir);
    free(ir);
    relic_assets_free(assets);
    return exit_code;
}
