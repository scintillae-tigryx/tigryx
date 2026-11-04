/*
 * ir.c
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
 * ir.c - Relic AST -> IR emitter
 */
#include "tigryx.h"
#include "tigryx_tier.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#if !defined(_WIN32)
#  include <strings.h>
#endif

/* -------------------------------------------------------------------------
 * IR buffer helpers
 * ---------------------------------------------------------------------- */
bool relic_ir_ensure_aux(IRBuffer *ir) {
#ifdef RELIC_IA16
    /* Compile-time aux tables, far-allocated on demand so the .tlx run
     * path never pays for them. */
    if(!ir->fn_names) {
        ir->fn_names = relic_far_alloc((u32)RELIC_MAX_FUNCS * RELIC_MAX_IDENT);
        if(!ir->fn_names) return false;
        relic_far_set((u8 RELIC_FAR *)ir->fn_names, 0, 0,
                      (u32)RELIC_MAX_FUNCS * RELIC_MAX_IDENT);
    }
    if(!ir->fn_argmask) {
        ir->fn_argmask = (u16 RELIC_FAR *)relic_far_alloc((u32)RELIC_MAX_FUNCS * 2u);
        if(!ir->fn_argmask) return false;
        relic_far_set((u8 RELIC_FAR *)ir->fn_argmask, 0, 0, (u32)RELIC_MAX_FUNCS * 2u);
    }
    if(!ir->source_files) {
        ir->source_files = (char RELIC_FAR (*)[RELIC_MAX_SOURCE_PATH])
            relic_far_alloc((u32)RELIC_MAX_SOURCE_FILES * RELIC_MAX_SOURCE_PATH);
        if(!ir->source_files) return false;
        relic_far_set((u8 RELIC_FAR *)ir->source_files, 0, 0,
                      (u32)RELIC_MAX_SOURCE_FILES * RELIC_MAX_SOURCE_PATH);
    }
#else
    (void)ir;
#endif
    return true;
}

void relic_ir_fn_name(const IRBuffer *ir, int fn, char *dst, size_t cap) {
    if(!dst || cap == 0) return;
    dst[0] = 0;
    if(!ir || fn < 0 || fn >= ir->nfuncs) return;
#ifdef RELIC_IA16
    if(!ir->fn_names) return;
#endif
    relic_far_str(dst, IRFNNAME(ir, fn), cap);
}

bool relic_ir_reserve(IRBuffer *ir, int need) {
#ifdef RELIC_IA16
    /* Runtime-needed far tables, allocated once before the grow early-
     * return: bytecode load and the assembler write fn_offsets/strings
     * directly rather than through emit(). strings is eagerly sized to
     * the cap (the emitter interning doesn't track a budget ahead). */
    if(!ir->strings) {
        ir->strings = relic_far_alloc(RELIC_MAX_STRINGS);
        if(!ir->strings) return false;
    }
    if(!ir->fn_offsets) {
        ir->fn_offsets = (i32 RELIC_FAR *)relic_far_alloc((u32)RELIC_MAX_FUNCS * 4u);
        if(!ir->fn_offsets) return false;
        relic_far_set((u8 RELIC_FAR *)ir->fn_offsets, 0, 0, (u32)RELIC_MAX_FUNCS * 4u);
    }
#endif
    if(need <= ir->cap) return true;
#ifdef RELIC_IA16
    /* First alloc is exact-fit when it is already large: bytecode loads
     * call this once with the final instruction count, and pow2 rounding
     * would waste tens of KB of conventional memory (a 64K block per
     * table on >8192-instr programs).  Small starts still get 512. */
    int new_cap = ir->cap ? ir->cap : (need > 512 ? need : 512);
#else
    int new_cap = ir->cap ? ir->cap : 4096;
#endif
    while(new_cap < need) {
        /* Doubling, with an overflow guard (int may be only 16 bits). */
        if(new_cap > INT_MAX / 2) { new_cap = need; break; }
        new_cap *= 2;
    }
#ifdef RELIC_IA16
    /* Far allocations: code grows past 64K, and real-mode realloc cannot
     * resize a DOS block - allocate the new block, copy, free the old.
     * The line/file debug tables only exist when want_dbg is set (emit
     * path, or a .tlx with a debug section) - skipping them saves
     * 2*cap*4 bytes on the common load path. */
    u32 RELIC_FAR *new_code = relic_far_alloc((u32)new_cap * 4u);
    if(!new_code) return false;
    i32 RELIC_FAR *new_lines = NULL, *new_files = NULL;
    if(ir->want_dbg || ir->line_for_pc) {
        new_lines = relic_far_alloc((u32)new_cap * 4u);
        if(!new_lines) { relic_far_free(new_code); return false; }
        new_files = relic_far_alloc((u32)new_cap * 4u);
        if(!new_files) { relic_far_free(new_code); relic_far_free(new_lines); return false; }
    }
    if(ir->code) {
        relic_far_write((u8 RELIC_FAR *)new_code, 0, ir->code, (u32)ir->len * 4u);
        relic_far_free(ir->code);
        if(ir->line_for_pc) {
            relic_far_write((u8 RELIC_FAR *)new_lines, 0, ir->line_for_pc, (u32)ir->len * 4u);
            relic_far_free(ir->line_for_pc);
            relic_far_write((u8 RELIC_FAR *)new_files, 0, ir->file_for_pc, (u32)ir->len * 4u);
            relic_far_free(ir->file_for_pc);
        }
    }
    if(new_lines) {
        /* 0 for unwritten lines, -1 ("unknown") for file ids - the far
         * pointer needs normalized addressing past 16K elements. */
        u32 i;
        for(i = (u32)ir->cap; i < (u32)new_cap; i++) {
            *(i32 RELIC_FAR *)relic_fp_add((u8 RELIC_FAR *)new_lines, i * 4u) = 0;
            *(i32 RELIC_FAR *)relic_fp_add((u8 RELIC_FAR *)new_files, i * 4u) = -1;
        }
    }
    ir->code = new_code;
    ir->line_for_pc = new_lines;
    ir->file_for_pc = new_files;
    ir->cap = new_cap;
    return true;
#else
    u32 *new_code = realloc(ir->code, (size_t)new_cap * sizeof(u32));
    if(!new_code) return false;
    i32 *new_lines = realloc(ir->line_for_pc, (size_t)new_cap * sizeof(i32));
    if(!new_lines) { ir->code = new_code; return false; }
    /* Zero the freshly-grown tail so unwritten line_for_pc entries stay 0. */
    memset(new_lines + ir->cap, 0, (size_t)(new_cap - ir->cap) * sizeof(i32));
    i32 *new_files = realloc(ir->file_for_pc, (size_t)new_cap * sizeof(i32));
    if(!new_files) { ir->code = new_code; ir->line_for_pc = new_lines; return false; }
    /* -1 ("unknown"), not 0, is the "none" sentinel for file ids. */
    for(int i = ir->cap; i < new_cap; i++) new_files[i] = -1;
    ir->code = new_code;
    ir->line_for_pc = new_lines;
    ir->file_for_pc = new_files;
    ir->cap = new_cap;
    return true;
#endif
}

void relic_ir_free(IRBuffer *ir) {
#ifdef RELIC_IA16
    relic_far_free(ir->code);         ir->code = NULL;
    relic_far_free(ir->line_for_pc);  ir->line_for_pc = NULL;
    relic_far_free(ir->file_for_pc);  ir->file_for_pc = NULL;
    relic_far_free(ir->strings);      ir->strings = NULL;
    relic_far_free(ir->fn_names);     ir->fn_names = NULL;
    relic_far_free(ir->fn_offsets);   ir->fn_offsets = NULL;
    relic_far_free(ir->fn_argmask);   ir->fn_argmask = NULL;
    relic_far_free(ir->source_files); ir->source_files = NULL;
    free(ir->dbg_syms);               ir->dbg_syms = NULL;
#else
    free(ir->code);         ir->code = NULL;
    free(ir->line_for_pc);  ir->line_for_pc = NULL;
    free(ir->file_for_pc);  ir->file_for_pc = NULL;
    free(ir->dbg_syms);     ir->dbg_syms = NULL;
#endif
    free(ir->warnings);     ir->warnings = NULL;
    ir->nwarnings = 0;
    ir->warnings_dropped = 0;
    ir->dbg_syms_len = 0;
    ir->dbg_syms_cap = 0;
    ir->len = 0;
    ir->cap = 0;
}

static int emit(IREmitter *em, u32 instr) {
    em->ir->want_dbg = 1;   /* emit() writes line/file tables below */
    if(em->ir->len >= em->ir->cap) {
        if(!relic_ir_reserve(em->ir, em->ir->len + 1)) {
            snprintf(em->err, sizeof(em->err), "out of memory growing IR buffer");
            return -1;
        }
    }
    int pc = em->ir->len++;
    IRCODE(em->ir, pc) = instr;
    /* Record the source line/file for this word. cur_line/cur_file_id are
     * updated by emit_stmt/emit_expr at every AST node entry; LDC32's
     * trailing data word (and any other helper continuations) inherit
     * them. */
    IRLINE(em->ir, pc) = em->cur_line;
    IRFILEID(em->ir, pc) = em->cur_file_id;
    return pc;
}

static int emit3(IREmitter *em, u8 op, u8 dst, u8 a, u8 b) {
    return emit(em, IR_ENCODE(op, dst, a, b, 0));
}

static int emit_imm(IREmitter *em, u8 op, u8 dst, u8 a, i32 imm12) {
    /* The immediate field is 12-bit signed (range -2048..+2047). Silently
     * truncating an out-of-range value miscompiles the program (e.g. an
     * FP-relative store goes to the wrong stack slot). Surface it as a
     * compile error so the bug is caught instead of producing a binary
     * that quietly behaves incorrectly. Callers that need larger offsets
     * must materialise the address in a register (see emit_load_off /
     * emit_store_off helpers below). */
    if(imm12 < -2048 || imm12 > 2047) {
        if(!em->err[0]) {
            snprintf(em->err, sizeof(em->err),
                     "line %d: op 0x%02X immediate %d out of 12-bit range "
                     "(frame too large for direct addressing)",
                     em->cur_line, op, imm12);
        }
        /* Still emit the (masked) instruction so the emitter can keep
         * going and report further errors; the err flag will fail the
         * overall compilation. */
    }
    return emit(em, IR_ENCODE(op, dst, a, 0, imm12));
}

static int cur_pc(IREmitter *em) { return em->ir->len; }

/* -------------------------------------------------------------------------
 * Compute byte size of a type, including structs (via TC struct registry).
 * Falls back to 4 if no TypeChecker is available or the struct is unknown.
 * ---------------------------------------------------------------------- */
static int emit_type_size(TypeChecker *tc, Type *t) {
    if(!t) return 4;
    switch(t->kind) {
        case TY_BOOL: case TY_I8: case TY_U8: return 1;
        case TY_I16: case TY_U16: return 2;
        case TY_I32: case TY_U32: case TY_F32:
        case TY_PTR: case TY_REF: case TY_FNPTR: return 4;
        case TY_F64: return 8;
        case TY_ARRAY: return emit_type_size(tc, t->inner) * (int)t->count;
        case TY_STRUCT: {
            if(!tc) return 4;
            for(int i=0;i<tc->nstructs;i++) {
                if(strcmp(tc->structs[i].name, t->name)!=0) continue;
                Node *decl = tc->structs[i].decl;
                int sz = 0;
                for(int j=0;j<decl->nchildren;j++)
                    sz += emit_type_size(tc, decl->children[j]->type);
                return sz;
            }
            return 4;
        }
        default: return 4;
    }
}

/* -------------------------------------------------------------------------
 * Register allocator (trivial linear scan over r1..r13)
 * r0=zero  r14=FP  r15=reserved
 * ---------------------------------------------------------------------- */
#define REG_ZERO  0
#define REG_FP   14
#define REG_SP   15
#define REG_FIRST 1
#define REG_LAST 13

/* Hard cap on call-site arity. The first MAX_REG_ARGS args (after any
 * hidden retbuf pointer) travel in registers; everything beyond that is
 * pushed on the VM stack and read by the callee from positive offsets
 * above the saved frame-pointer slot. The cap is generous to avoid the
 * old silent-truncation footgun, but large enough to keep call sequences
 * compact and to fit comfortably within MAX_LOCALS spill slots. */
#define MAX_TOTAL_ARGS 255

typedef struct {
    bool used[16];
    bool dused[16];     /* d-bank temporaries (f64); d0 is reserved for
                         * the eventual f64 call-ABI return register, so
                         * rd_alloc hands out d1..d15 only. */
    int  frame_size;    /* bytes allocated in current frame */
} RegAlloc;

static u8 ra_alloc(RegAlloc *ra, IREmitter *em) {
    for(int i=REG_FIRST;i<=REG_LAST;i++) {
        if(!ra->used[i]) { ra->used[i]=true; return (u8)i; }
    }
    snprintf(em->err, sizeof(em->err),
             "line %d: out of registers", em->cur_line);
    return 0xFF;
}

static void ra_free(RegAlloc *ra, u8 r) {
    if(r >= REG_FIRST && r <= REG_LAST) ra->used[r] = false;
}

/* -------------------------------------------------------------------------
 * Label / patch system
 * ---------------------------------------------------------------------- */
static int new_label(IREmitter *em) {
    int lbl = em->next_label++;
    if(lbl >= em->label_cap) {
        int new_cap = em->label_cap ? em->label_cap * 2 : 4096;
        while(new_cap <= lbl) new_cap *= 2;
        int *new_labels = realloc(em->labels, (size_t)new_cap * sizeof(int));
        if(!new_labels) {
            if(!em->err[0]) snprintf(em->err, sizeof(em->err),
                "out of memory growing label table");
            return lbl;
        }
        /* Zero the freshly-grown tail so unbound labels read as 0. */
        memset(new_labels + em->label_cap, 0,
               (size_t)(new_cap - em->label_cap) * sizeof(int));
        em->labels = new_labels;
        em->label_cap = new_cap;
    }
    return lbl;
}

static void bind_label(IREmitter *em, int lbl) {
    if(lbl >= 0 && lbl < em->label_cap) em->labels[lbl] = cur_pc(em);
}

static int emit_jmp(IREmitter *em, u8 op, u8 src, int lbl) {
    int pc = emit(em, IR_ENCODE(op, 0, src, 0, 0)); /* placeholder */
    if(em->npatches >= em->patch_cap) {
        int new_cap = em->patch_cap ? em->patch_cap * 2 : 4096;
        void *new_patches = realloc(em->patches,
            (size_t)new_cap * sizeof(*em->patches));
        if(!new_patches) {
            if(!em->err[0]) snprintf(em->err, sizeof(em->err),
                "out of memory growing patch table");
            return pc;
        }
        em->patches = new_patches;
        em->patch_cap = new_cap;
    }
    em->patches[em->npatches].patch_pc     = pc;
    em->patches[em->npatches].target_label = lbl;
    em->npatches++;
    return pc;
}

static void patch_jumps(IREmitter *em) {
    for(int i=0;i<em->npatches;i++) {
        int pc  = em->patches[i].patch_pc;
        int lbl = em->patches[i].target_label;
        int tgt = (lbl >= 0 && lbl < em->label_cap) ? em->labels[lbl] : 0;
        u32 instr = IRCODE(em->ir, pc);
        /* PC-relative 20-bit displacement - see IR_JTARGET in tigryx.h.
         * Jumps stay inside one function, so only a single function
         * larger than 512K IR words can overflow. */
        if(!IR_JFITS(pc, tgt)) {
            if(!em->err[0]) {
                snprintf(em->err, sizeof(em->err),
                         "jump from %d to %d exceeds the 20-bit displacement "
                         "(function too large)", pc, tgt);
            }
            continue;
        }
        IRCODE(em->ir, pc) = IR_JSET(instr, pc, tgt);
    }
}

/* -------------------------------------------------------------------------
 * Stack frame: local variable offset tracking
 * ---------------------------------------------------------------------- */
typedef struct {
    char  name[RELIC_MAX_IDENT];
    int   offset;    /* byte offset from FP */
    Type *type;
    int   live;      /* 0 once its lexical scope has closed - the typechecker
                      * pops symbols at scope end; without this, frame_find
                      * would resurrect a dead same-named slot and resolve a
                      * fresh `for` var or a global to the wrong storage. */
} LocalVar;

#define MAX_LOCALS 256

typedef struct {
    LocalVar locals[MAX_LOCALS];
    int      nlocals;
    int      frame_bytes;
} Frame;

static int frame_alloc_sz(Frame *f, const char *name, Type *t, int sz);

static int frame_alloc(Frame *f, const char *name, Type *t) {
    /* Default: 4 bytes (sizes that need more must call frame_alloc_sz). */
    return frame_alloc_sz(f, name, t, 4);
}

static int frame_alloc_sz(Frame *f, const char *name, Type *t, int sz) {
    /* Locals live at NEGATIVE offsets from FP, growing down toward SP.
     * FP+0 is the saved frame pointer (pushed by CALL) and FP+4 is the
     * return PC; locals must not collide with those slots. */
    if(sz < 4) sz = 4;
    sz = (sz + 3) & ~3;          /* round up to 4-byte alignment */
    f->frame_bytes += sz;
    int off = -f->frame_bytes;   /* first local at -4, next at -(4+sz), ... */
    /* A NULL name marks an anonymous scratch slot (e.g. call-argument
     * staging in emit_call_with_dest). Such slots are addressed only by the
     * offset returned here and are never resolved by name, so they must NOT
     * consume a locals[] entry. Recording them would let a function with many
     * call arguments exhaust MAX_LOCALS, after which real variables declared
     * later get valid offsets but go unrecorded and are mis-resolved as
     * globals — corrupting them and, e.g., hanging loops on a bad counter. */
    if(name && f->nlocals < MAX_LOCALS) {
        snprintf(f->locals[f->nlocals].name, RELIC_MAX_IDENT, "%s", name);
        f->locals[f->nlocals].offset = off;
        f->locals[f->nlocals].type   = t;
        f->locals[f->nlocals].live   = 1;
        f->nlocals++;
    }
    return off;
}

/* Kill every local allocated since `mark` - the lexical scope (block or
 * for-header) that owns them has closed. The slot bytes stay reserved
 * (frame_bytes never shrinks); only name resolution is affected. Mirrors
 * sym_pop_scope in the typechecker, which pops the same names at the same
 * points - keep the two in step or big-endian hosts will see width-mixed
 * accesses when a dead i32 slot gets resurrected for a fresh i16 for-var. */
static void frame_close_scope(Frame *f, int mark) {
    for(int i=mark;i<f->nlocals;i++) f->locals[i].live = 0;
}

/* Sentinel returned by frame_find when the name is not a local.
 * Distinct from any valid (negative) local offset. */
#define FRAME_NOT_FOUND  0x7FFFFFFFL

static int frame_find(Frame *f, const char *name) {
    /* Scan from the most recent allocation backwards so that re-used names
     * (e.g. a second `for i in ...` loop in the same function) shadow the
     * earlier slot rather than aliasing it. */
    for(int i=f->nlocals-1;i>=0;i--)
        if(f->locals[i].live && strcmp(f->locals[i].name, name)==0)
            return f->locals[i].offset;
    return FRAME_NOT_FOUND;
}

/* Type recorded for an existing local, or NULL when the name is not a
 * local (or was allocated without a type, as `for` induction slots are). */
static Type *frame_find_type(Frame *f, const char *name) {
    for(int i=f->nlocals-1;i>=0;i--)
        if(f->locals[i].live && strcmp(f->locals[i].name, name)==0)
            return f->locals[i].type;
    return NULL;
}

/* -------------------------------------------------------------------------
 * Pick the load / store opcode (and optional sign-extension) appropriate
 * for an in-memory value of type t. Sub-word types (bool, i8/u8, i16/u16)
 * are stored as 1 or 2 bytes; everything else uses 4-byte access.
 * For signed sub-word types, callers should emit `sx_op` on the loaded
 * register to sign-extend to 32 bits.
 * ---------------------------------------------------------------------- */
static void pick_mem_ops(Type *t, u8 *ld_op, u8 *st_op, u8 *sx_op) {
    *sx_op = 0;
    if(!t) { *ld_op = OP_LD32; *st_op = OP_ST32; return; }
    switch(t->kind) {
        case TY_BOOL: case TY_U8:
            *ld_op = OP_LD8;  *st_op = OP_ST8;  return;
        case TY_I8:
            *ld_op = OP_LD8;  *st_op = OP_ST8;  *sx_op = OP_SX8;  return;
        case TY_U16:
            *ld_op = OP_LD16; *st_op = OP_ST16; return;
        case TY_I16:
            *ld_op = OP_LD16; *st_op = OP_ST16; *sx_op = OP_SX16; return;
        default:
            *ld_op = OP_LD32; *st_op = OP_ST32; return;
    }
}

/* -------------------------------------------------------------------------
 * Debug symbols blob: records collected during emit and appended to the
 * .tlx debug section by relic_bytecode_write (under a "LS\0\x01" magic so
 * loaders can tell it from an asset trailer).
 * Record layout (little-endian, unaligned):
 *   u8  kind    0 = function, 1 = local, 2 = global, 3 = param
 *   u16 nlen + name bytes
 *   i32 a       fn: start pc / local+param: fp offset / global: mem offset
 *   i32 b       fn: frame bytes; else 0
 *   u8  tkind   TypeKind (255 = none)
 *   i32 tsize   total byte size
 *   i32 ecount  array element count (0 = not an array)
 *   i32 esize   array element byte size
 *   u16 tnlen + type-name bytes (struct/typedef name or "")
 * ---------------------------------------------------------------------- */
static void dbg_put(IRBuffer *ir, const void *p, int n) {
    if(ir->dbg_syms_len + n > ir->dbg_syms_cap) {
        int nc = ir->dbg_syms_cap ? ir->dbg_syms_cap * 2 : 4096;
        while(nc < ir->dbg_syms_len + n) nc *= 2;
        u8 *nb = realloc(ir->dbg_syms, (size_t)nc);
        if(!nb) return;
        ir->dbg_syms = nb;
        ir->dbg_syms_cap = nc;
    }
    memcpy(ir->dbg_syms + ir->dbg_syms_len, p, (size_t)n);
    ir->dbg_syms_len += n;
}
static void dbg_u8 (IRBuffer *ir, u8  v) { dbg_put(ir, &v, 1); }
static void dbg_u16(IRBuffer *ir, u16 v) { u8 b[2]; relic_wr16le(b, v); dbg_put(ir, b, 2); }
static void dbg_i32(IRBuffer *ir, i32 v) { u8 b[4]; relic_wr32le(b, (u32)v); dbg_put(ir, b, 4); }
static void dbg_str(IRBuffer *ir, const char *s) {
    u16 n = s ? (u16)strlen(s) : 0;
    dbg_u16(ir, n);
    if(n) dbg_put(ir, s, n);
}
static void dbg_sym(IREmitter *em, u8 kind, const char *name,
                    i32 a, i32 b, Type *t) {
    IRBuffer *ir = em->ir;
    dbg_u8(ir, kind);
    dbg_str(ir, name ? name : "");
    dbg_i32(ir, a);
    dbg_i32(ir, b);
    dbg_u8(ir, t ? (u8)t->kind : 255);
    dbg_i32(ir, t ? emit_type_size(em->tc, t) : 0);
    dbg_i32(ir, (t && t->kind == TY_ARRAY) ? (i32)t->count : 0);
    dbg_i32(ir, (t && t->kind == TY_ARRAY && t->inner)
                ? emit_type_size(em->tc, t->inner) : 0);
    dbg_str(ir, (t && t->name[0]) ? t->name : "");
}

/* -------------------------------------------------------------------------
 * String interning
 * ---------------------------------------------------------------------- */
static int RELIC_UNUSED intern_string(IRBuffer *ir, const char *s, int len) {
    u32 off = ir->strings_len;
    if(off + len + 1 >= RELIC_MAX_STRINGS) return -1;
    relic_far_write((u8 RELIC_FAR *)ir->strings, off, s, (u32)len);
    IRSTR(ir, off+len) = 0;
    ir->strings_len += len+1;
    return (int)off;
}

static int hex_digit_val(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

/* Decode a single escape character (must match lexer's decode_escape) */
static int emit_decode_escape(char c) {
    switch(c) {
        case 'n':  return '\n';
        case 'r':  return '\r';
        case 't':  return '\t';
        case '0':  return 0;
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case 'a':  return '\a';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'v':  return '\v';
        default:   return c;
    }
}

/* Intern a string literal token (text includes surrounding quotes),
 * decoding escape sequences. Returns offset in ir->strings (or -1). */
static int intern_string_lit(IRBuffer *ir, const char *quoted, int qlen, int *out_len) {
    if(qlen < 2) return -1;
    const char *s = quoted + 1;        /* skip opening " */
    const char *e = quoted + qlen - 1; /* skip closing " */
    u32 off = ir->strings_len;
    int len = 0;
    while(s < e) {
        if(off + len + 1 >= RELIC_MAX_STRINGS) return -1;
        char ch;
        if(*s == '\\' && s + 1 < e) {
            if(s[1] == 'x' && s + 3 < e) {
                ch = (char)(hex_digit_val(s[2]) * 16 + hex_digit_val(s[3]));
                s += 4;
            } else {
                ch = (char)emit_decode_escape(s[1]);
                s += 2;
            }
        } else {
            ch = *s++;
        }
        IRSTR(ir, off + len++) = ch;
    }
    if(off + len + 1 >= RELIC_MAX_STRINGS) return -1;
    IRSTR(ir, off + len) = 0;
    ir->strings_len += len + 1;
    if(out_len) *out_len = len;
    return (int)off;
}

/* Intern an ND_STRING_LIT node (or fetch its cached offset). On success
 * fills *out_off and *out_len and returns true. */
static bool intern_node_string(IREmitter *em, Node *n, int *out_off, int *out_len) {
    if(!n || n->kind != ND_STRING_LIT) return false;
    if(n->op & 0x40000000) {
        if(out_off) *out_off = n->op & 0x3FFFFFFF;
        if(out_len) *out_len = n->ival;
        return true;
    }
    if(!em->tokens) return false;
    Token *tok = &em->tokens[n->soff];
    int slen = 0;
    int soff = intern_string_lit(em->ir, tok->start, tok->len, &slen);
    if(soff < 0) return false;
    n->op   = 0x40000000 | soff;
    n->ival = slen;
    if(out_off) *out_off = soff;
    if(out_len) *out_len = slen;
    return true;
}

/* True iff the var-decl initialiser is the `[N]u8 = "..."` syntax sugar. */
static bool is_str_array_init(Node *d) {
    return d && d->left && d->type
        && d->type->kind == TY_ARRAY
        && d->type->inner && d->type->inner->kind == TY_U8
        && d->left->kind == ND_STRING_LIT;
}

/* -------------------------------------------------------------------------
 * Emitter context per-function
 * ---------------------------------------------------------------------- */
typedef struct {
    IREmitter *em;
    Frame      frame;
    RegAlloc   ra;
    SymTable  *syms;
    /* Hidden return-buffer pointer slot (FP+offset). Used when the current
     * function returns an aggregate (struct) by hidden reference. */
    int        retbuf_off;
    bool       has_retbuf;
} EmCtx;

/* Forward declarations */
static u8 emit_expr(EmCtx *ctx, Node *n);
static bool emit_stmt(EmCtx *ctx, Node *n);
static u8 emit_struct_addr(EmCtx *ctx, Node *n);
static u8 emit_call_with_dest(EmCtx *ctx, Node *n, u8 dest_base, int dest_off);

/* True for value types that don't fit in a single 32-bit register and must
 * be passed/returned by hidden pointer. Currently: structs only. (Arrays in
 * this codebase already decay to pointers at use sites.) */
static bool is_aggregate(Type *t) {
    return t && t->kind == TY_STRUCT;
}

/* True if the subtree emits a CALL or BUILTIN — both of which stage their
 * arguments into r1..rN via unconditional MOVs, which can clobber live
 * values currently held in those registers by an enclosing expression
 * (e.g. the left operand of a BINOP that is sitting in r1). Callers that
 * hold a value in a register across such a subtree must spill it first. */
static bool expr_has_call(Node *n) {
    if(!n) return false;
    if(n->kind == ND_CALL || n->kind == ND_BUILTIN_CALL) return true;
    if(expr_has_call(n->left))  return true;
    if(expr_has_call(n->right)) return true;
    if(expr_has_call(n->cond))  return true;
    for(int i=0;i<n->nchildren;i++)
        if(expr_has_call(n->children[i])) return true;
    return false;
}

/* -------------------------------------------------------------------------
 * Memory load/store with large offset support.
 *
 * The IR's load/store instructions encode the offset in a 12-bit signed
 * immediate (range -2048..+2047). When a frame contains arrays large
 * enough to push a local past that range, we materialise the offset in
 * a scratch register: tmp = LDC32 off; tmp = base + tmp; op via tmp+0.
 *
 * emit_load_off:  reg = mem[base + off]   (base goes in src_a field)
 * emit_store_off: mem[base + off] = reg   (base goes in dst    field)
 * ---------------------------------------------------------------------- */
static bool offset_fits_imm12(int off) {
    return off >= -2048 && off <= 2047;
}

static void emit_load_off(EmCtx *ctx, u8 op, u8 dst, u8 base, int off) {
    IREmitter *em = ctx->em;
    if(offset_fits_imm12(off)) {
        emit_imm(em, op, dst, base, off);
        return;
    }
    u8 tmp = ra_alloc(&ctx->ra, em);
    if(tmp == 0xFF) return;
    emit_imm(em, OP_LDC32, tmp, 0, 0);
    emit(em, (u32)off);
    emit3(em, OP_ADD, tmp, base, tmp);
    emit_imm(em, op, dst, tmp, 0);
    ra_free(&ctx->ra, tmp);
}

static void emit_store_off(EmCtx *ctx, u8 op, u8 base, u8 src, int off) {
    IREmitter *em = ctx->em;
    if(offset_fits_imm12(off)) {
        emit_imm(em, op, base, src, off);
        return;
    }
    u8 tmp = ra_alloc(&ctx->ra, em);
    if(tmp == 0xFF) return;
    emit_imm(em, OP_LDC32, tmp, 0, 0);
    emit(em, (u32)off);
    emit3(em, OP_ADD, tmp, base, tmp);
    emit_imm(em, op, tmp, src, 0);
    ra_free(&ctx->ra, tmp);
}

/* Compute dst = base + off, handling offsets that exceed the 12-bit
 * signed immediate range (-2048..2047) by materialising the offset in
 * a scratch register.  Mirrors emit_load_off / emit_store_off. */
static void emit_lea_off(EmCtx *ctx, u8 dst, u8 base, int off) {
    IREmitter *em = ctx->em;
    if(offset_fits_imm12(off)) {
        emit_imm(em, OP_LEA, dst, base, off);
        return;
    }
    u8 tmp = ra_alloc(&ctx->ra, em);
    if(tmp == 0xFF) return;
    emit_imm(em, OP_LDC32, tmp, 0, 0);
    emit(em, (u32)off);
    emit3(em, OP_ADD, dst, base, tmp);
    ra_free(&ctx->ra, tmp);
}

/* Emit a byte-accurate copy of `size` bytes from [src_reg+src_off] to
 * [dst_reg+dst_off]. Uses 4-byte loads/stores where possible, falling back
 * to 1-byte ops for any tail. Allocates and frees a single scratch reg. */
static void emit_memcpy(EmCtx *ctx, u8 dst_reg, int dst_off,
                        u8 src_reg, int src_off, int size) {
    u8 tmp = ra_alloc(&ctx->ra, ctx->em);
    if(tmp == 0xFF) return;
    int o = 0;
    while(size - o >= 4) {
        emit_load_off (ctx, OP_LD32, tmp, src_reg, src_off + o);
        emit_store_off(ctx, OP_ST32, dst_reg, tmp, dst_off + o);
        o += 4;
    }
    while(size - o >= 1) {
        emit_load_off (ctx, OP_LD8, tmp, src_reg, src_off + o);
        emit_store_off(ctx, OP_ST8, dst_reg, tmp, dst_off + o);
        o += 1;
    }
    ra_free(&ctx->ra, tmp);
}

/* -------------------------------------------------------------------------
 * Load a local/global variable address into a register
 * Returns register holding the value (caller must ra_free it)
 * ---------------------------------------------------------------------- */
static u8 emit_lvalue_addr(EmCtx *ctx, Node *n, int *out_offset) {
    IREmitter *em = ctx->em;
    if(n->kind == ND_IDENT) {
        int off = frame_find(&ctx->frame, n->sval);
        if(off != FRAME_NOT_FOUND) {
            *out_offset = off;
            return REG_FP;   /* addr = FP + offset (offset may be negative) */
        }
            /* global variable: emit absolute address into a fresh register */
        Symbol *sym = sym_lookup(ctx->syms, n->sval);
        if(sym && sym->kind == SYM_CONST) {
            /* Aggregate-typed module-level consts are given storage in the
             * data segment (see Pass 1b in relic_emit) and initialised at
             * program start. Emit the absolute address of that storage so
             * struct field access / by-pointer passing works correctly. */
            if(is_aggregate(sym->type)) {
                u8 ar = ra_alloc(&ctx->ra, ctx->em); if(ar==0xFF) return 0xFF;
                i32 addr = (i32)sym->offset;
                if(addr >= -2048 && addr <= 2047)
                    emit_imm(ctx->em, OP_LDC, ar, 0, addr);
                else { emit_imm(ctx->em, OP_LDC32, ar, 0, 0); emit(ctx->em, (u32)addr); }
                *out_offset = 0;
                return ar;
            }
            *out_offset = -1;
            return 0xFF;
        }
        { u8 ar = ra_alloc(&ctx->ra, ctx->em); if(ar==0xFF) return 0xFF;
          i32 addr = sym ? (i32)sym->offset : 0;
          if(addr >= -2048 && addr <= 2047)
              emit_imm(ctx->em, OP_LDC, ar, 0, addr);
          else { emit_imm(ctx->em, OP_LDC32, ar, 0, 0); emit(ctx->em, (u32)addr); }
          *out_offset = 0;
          return ar; }
    }
    if(n->kind == ND_UNOP && n->op == TK_STAR) {
        /* *ptr — load ptr value into reg, offset=0 */
        u8 r = emit_expr(ctx, n->left);
        *out_offset = 0;
        return r;
    }
    if(n->kind == ND_INDEX) {
        int base_off = 0;
        u8  base;
        /* If n->left is already a pointer (e.g. an array parameter that
         * decayed to *T, or an explicit pointer), load the pointer VALUE
         * rather than the address of the pointer-variable. Otherwise take
         * the address of the array-like lvalue. */
        Type *lt = n->left->type;
        if(lt && (lt->kind == TY_PTR || lt->kind == TY_REF)) {
            base = emit_expr(ctx, n->left);
        } else {
            base = emit_lvalue_addr(ctx, n->left, &base_off);
        }
        u8 idx  = emit_expr(ctx, n->right);
        u8 dst  = ra_alloc(&ctx->ra, em);
        /* dst = base + idx * elem_size  (elem type set by type checker) */
        int elem_size = emit_type_size(em->tc, n->type);
        if(elem_size < 1) elem_size = 4;
        u8 scaled = ra_alloc(&ctx->ra, em);
        /* fast path: power-of-2 size -> SHL; otherwise -> MUL */
        int shift = 0, e = elem_size;
        while(e > 1 && (e & 1) == 0) { e >>= 1; shift++; }
        if(e == 1) {
            if(shift == 0) {
                emit3(em, OP_MOV, scaled, idx, 0);
            } else {
                emit_imm(em, OP_LDC, scaled, 0, shift);
                emit3(em, OP_SHL, scaled, idx, scaled);
            }
        } else {
            /* LDC only carries a 12-bit immediate - indexing an array of
             * large structs (e.g. [8]Document, ~7.9 KB/element) needs the
             * LDC32+payload form or the scale factor silently truncates. */
            if(elem_size >= 0 && elem_size <= 2047) {
                emit_imm(em, OP_LDC, scaled, 0, elem_size);
            } else {
                emit_imm(em, OP_LDC32, scaled, 0, 0);
                emit(em, (u32)elem_size);
            }
            emit3(em, OP_MUL, scaled, idx, scaled);
        }
        emit3(em, OP_ADD, dst, base, scaled);
        if(base_off != 0) {
            /* base has an offset (e.g., FP+off), add it to dst */
            if(offset_fits_imm12(base_off)) {
                emit_imm(em, OP_ADDI, dst, dst, base_off);
            } else {
                u8 tmp = ra_alloc(&ctx->ra, em);
                if(tmp != 0xFF) {
                    emit_imm(em, OP_LDC32, tmp, 0, 0);
                    emit(em, (u32)base_off);
                    emit3(em, OP_ADD, dst, dst, tmp);
                    ra_free(&ctx->ra, tmp);
                }
            }
        }
        ra_free(&ctx->ra, scaled);
        ra_free(&ctx->ra, idx);
        if(base != REG_FP && base != REG_ZERO) ra_free(&ctx->ra, base);
        *out_offset = 0;
        return dst;
    }
    if(n->kind == ND_FIELD_ACCESS) {
        /* If left is a pointer-to-struct (or ref-to-struct), load the
         * pointer value as base. Otherwise (local struct, array element,
         * etc.) take its address. */
        Type *lt = n->left->type;
        if(lt && (lt->kind == TY_PTR || lt->kind == TY_REF)) {
            u8 base = emit_expr(ctx, n->left);
            *out_offset = n->ival;
            return base;
        }
        int base_off = 0;
        u8 base = emit_lvalue_addr(ctx, n->left, &base_off);
        *out_offset = base_off + n->ival;
        return base;
    }
    *out_offset = 0;
    return REG_ZERO;
}

/* -------------------------------------------------------------------------
 * f64 (d-bank) expression emission
 *
 * emit_expr always returns an r-bank register; f64 values live in the
 * separate d bank, so f64-typed subtrees route through emit_expr_d,
 * which returns a d-register. The boundary is explicit: a parent checks
 * the child node's type and calls the right entry point, and the places
 * where a value crosses back to r-land (compares producing bool, casts
 * from f64) are handled at those sites.
 *
 * No expr_has_call-style spilling is needed in d-land: calls cannot yet
 * return f64 (emit_fn rejects f64 signatures), and staging call args
 * into r1..rN never touches the d bank anyway.
 * ---------------------------------------------------------------------- */
static u8 emit_expr_d(EmCtx *ctx, Node *n);   /* fwd: mutual recursion
                                              * via emit_expr */

static u8 rd_alloc(RegAlloc *ra, IREmitter *em) {
    /* d0 is reserved for the future f64 call-ABI return register. */
    for(int i=1;i<16;i++) {
        if(!ra->dused[i]) { ra->dused[i]=true; return (u8)i; }
    }
    snprintf(em->err, sizeof(em->err),
             "line %d: out of f64 registers", em->cur_line);
    return 0xFF;
}
static void rd_free(RegAlloc *ra, u8 d) {
    if(d >= 1 && d < 16) ra->dused[d] = false;
}

/* f64 load/store with the same imm12 addressing and large-offset escape
 * as emit_load_off/emit_store_off. DLD: d_dst = mem[r_base + off].
 * DST: mem[r_base + off] = d_srca. */
static void emit_dload_off(EmCtx *ctx, u8 ddst, u8 rbase, int off) {
    IREmitter *em = ctx->em;
    if(offset_fits_imm12(off)) {
        emit_imm(em, OP_DLD, ddst, rbase, off);
        return;
    }
    u8 tmp = ra_alloc(&ctx->ra, em);
    if(tmp == 0xFF) return;
    emit_imm(em, OP_LDC32, tmp, 0, 0);
    emit(em, (u32)off);
    emit3(em, OP_ADD, tmp, rbase, tmp);
    emit_imm(em, OP_DLD, ddst, tmp, 0);
    ra_free(&ctx->ra, tmp);
}
static void emit_dstore_off(EmCtx *ctx, u8 rbase, u8 dsrc, int off) {
    IREmitter *em = ctx->em;
    if(offset_fits_imm12(off)) {
        emit_imm(em, OP_DST, rbase, dsrc, off);
        return;
    }
    u8 tmp = ra_alloc(&ctx->ra, em);
    if(tmp == 0xFF) return;
    emit_imm(em, OP_LDC32, tmp, 0, 0);
    emit(em, (u32)off);
    emit3(em, OP_ADD, tmp, rbase, tmp);
    emit_imm(em, OP_DST, tmp, dsrc, 0);
    ra_free(&ctx->ra, tmp);
}

/* Store an f64 d-register to an lvalue (ident, index, field, deref). */
static bool emit_store_d(EmCtx *ctx, Node *lval, u8 dsrc) {
    int off = 0;
    u8 addr = emit_lvalue_addr(ctx, lval, &off);
    if(addr == 0xFF) return false;
    emit_dstore_off(ctx, addr, dsrc, off);
    if(addr != REG_FP && addr != REG_ZERO) ra_free(&ctx->ra, addr);
    return true;
}

/* Emit a condition operand for JF/JT. An f64 condition is truthy when
 * nonzero: emitted as !(d == 0.0), so NaN counts as true (matching C's
 * d != 0 semantics) and no new opcode is needed. */
static u8 emit_cond_reg(EmCtx *ctx, Node *cond) {
    IREmitter *em = ctx->em;
    RegAlloc  *ra = &ctx->ra;
    if(cond && cond->type && cond->type->kind == TY_F64) {
        u8 dc = emit_expr_d(ctx, cond);
        if(dc == 0xFF) return 0xFF;
        u8 dz = rd_alloc(ra, em);
        if(dz == 0xFF) { rd_free(ra, dc); return 0xFF; }
        union { double f; u64 u; } z; z.f = 0.0;
        emit_imm(em, OP_DLDC, dz, 0, 0);
        emit(em, (u32)(z.u & 0xFFFFFFFFu));
        emit(em, (u32)(z.u >> 32));
        u8 r = ra_alloc(ra, em);
        if(r == 0xFF) { rd_free(ra, dc); rd_free(ra, dz); return 0xFF; }
        emit3(em, OP_DCEQ, r, dc, dz);        /* 1 when d == 0.0 */
        emit3(em, OP_CEQ,  r, r, REG_ZERO);   /* invert -> truthy */
        rd_free(ra, dz); rd_free(ra, dc);
        return r;
    }
    return emit_expr(ctx, cond);
}

/* Map a math builtin name to its f64 opcode (0 = no f64 overload). The
 * typechecker has already validated arity and argument widths. */
static u8 f64_builtin_op(const char *name) {
    static const struct { const char *name; u8 op; } map[] = {
        {"sin",  OP_DSIN},  {"cos",   OP_DCOS},   {"sqrt",  OP_DSQRT},
        {"tan",  OP_DTAN},  {"asin",  OP_DASIN},  {"acos",  OP_DACOS},
        {"atan", OP_DATAN}, {"atan2", OP_DATAN2},
        {"exp",  OP_DEXP},  {"log",   OP_DLOG},   {"log10", OP_DLOG10},
        {"pow",  OP_DPOW},  {"floor", OP_DFLOOR}, {"ceil",  OP_DCEIL},
        {"round",OP_DROUND},{"fmod",  OP_DFMOD},  {"fabs",  OP_DFABS},
        {"sinh", OP_DSINH}, {"cosh",  OP_DCOSH},  {"tanh",  OP_DTANH},
        {NULL, 0}
    };
    for(int i=0; map[i].name; i++)
        if(strcmp(map[i].name, name) == 0) return map[i].op;
    return 0;
}

/* Emit an f64-typed expression, returning a d register (caller rd_frees).
 * Only reached for nodes whose type is TY_F64. */
static u8 emit_expr_d(EmCtx *ctx, Node *n) {
    IREmitter *em = ctx->em;
    RegAlloc  *ra = &ctx->ra;
    if(!n) return 0xFF;
    if(n->line > 0) { em->cur_line = n->line; em->cur_file_id = n->file_id; }

    switch(n->kind) {

        case ND_FLOAT_LIT: {
            /* Full double precision: DLDC carries two payload words,
             * lo then hi (the instruction stream is word-based, so this
             * is endian-clean; the .tlx writer handles each u32 word). */
            u8 d = rd_alloc(ra, em); if(d==0xFF) return 0xFF;
            union { double f; u64 u; } cv;
            cv.f = n->fval;
            emit_imm(em, OP_DLDC, d, 0, 0);
            emit(em, (u32)(cv.u & 0xFFFFFFFFu));
            emit(em, (u32)(cv.u >> 32));
            return d;
        }

        case ND_IDENT: {
            /* An f64 ident is always a variable or a non-folded const -
             * both have storage. (const f64 can never constant-fold: the
             * fold channel is i32, so has_const_val stays false.) */
            Symbol *sym = sym_lookup(ctx->syms, n->sval);
            int off = frame_find(&ctx->frame, n->sval);
            u8 d = rd_alloc(ra, em); if(d==0xFF) return 0xFF;
            if(off != FRAME_NOT_FOUND) {
                emit_dload_off(ctx, d, REG_FP, off);
            } else {
                u8 ar = ra_alloc(ra, em);
                if(ar==0xFF) { rd_free(ra, d); return 0xFF; }
                i32 addr = sym ? (i32)sym->offset : 0;
                if(addr >= -2048 && addr <= 2047)
                    emit_imm(em, OP_LDC, ar, 0, addr);
                else { emit_imm(em, OP_LDC32, ar, 0, 0); emit(em, (u32)addr); }
                emit_dload_off(ctx, d, ar, 0);
                ra_free(ra, ar);
            }
            return d;
        }

        case ND_INDEX: case ND_FIELD_ACCESS: {
            /* Element/field load: all the address arithmetic (index *
             * elem_size, base+offset) is ordinary r-bank code shared via
             * emit_lvalue_addr; only the final load is DLD. */
            int off = 0;
            u8 base = emit_lvalue_addr(ctx, n, &off);
            if(base == 0xFF) return 0xFF;
            u8 d = rd_alloc(ra, em);
            if(d==0xFF) {
                if(base != REG_FP && base != REG_ZERO) ra_free(ra, base);
                return 0xFF;
            }
            emit_dload_off(ctx, d, base, off);
            if(base != REG_FP && base != REG_ZERO) ra_free(ra, base);
            return d;
        }

        case ND_UNOP: {
            if(n->op == TK_STAR) {
                /* *ptr where ptr: *f64 - emit_lvalue_addr loads the
                 * pointer value as the base. */
                int off = 0;
                u8 base = emit_lvalue_addr(ctx, n, &off);
                if(base == 0xFF) return 0xFF;
                u8 d = rd_alloc(ra, em);
                if(d==0xFF) {
                    if(base != REG_FP && base != REG_ZERO) ra_free(ra, base);
                    return 0xFF;
                }
                emit_dload_off(ctx, d, base, off);
                if(base != REG_FP && base != REG_ZERO) ra_free(ra, base);
                return d;
            }
            if(n->op == TK_MINUS) {
                u8 a = emit_expr_d(ctx, n->left);
                if(a==0xFF) return 0xFF;
                emit3(em, OP_DNEG, a, a, 0);
                return a;
            }
            snprintf(em->err, sizeof(em->err),
                     "f64 unary operator not supported");
            return 0xFF;
        }

        case ND_BINOP: {
            /* Only arithmetic reaches here - f64 comparisons produce a
             * bool and are emitted in emit_expr's ND_BINOP. */
            u8 op;
            switch(n->op) {
                case TK_PLUS:  op = OP_DADD; break;
                case TK_MINUS: op = OP_DSUB; break;
                case TK_STAR:  op = OP_DMUL; break;
                case TK_SLASH: op = OP_DDIV; break;
                default:
                    snprintf(em->err, sizeof(em->err),
                             "f64 binary operator not supported");
                    return 0xFF;
            }
            u8 a = emit_expr_d(ctx, n->left);
            if(a==0xFF) return 0xFF;
            u8 b = emit_expr_d(ctx, n->right);
            if(b==0xFF) { rd_free(ra, a); return 0xFF; }
            u8 d = rd_alloc(ra, em);
            if(d==0xFF) { rd_free(ra, a); rd_free(ra, b); return 0xFF; }
            emit3(em, op, d, a, b);
            rd_free(ra, b);
            rd_free(ra, a);
            return d;
        }

        case ND_CAST: {
            /* Result is f64. cast(f64, 0.1) converts the *f32 value* and
             * is deliberately inexact - the 'd' suffix is the exact
             * spelling. Pointers convert by address value (I2D). */
            Type *src = n->left->type;
            u8 d = rd_alloc(ra, em); if(d==0xFF) return 0xFF;
            if(src && src->kind == TY_F64) {
                u8 a = emit_expr_d(ctx, n->left);
                if(a==0xFF) { rd_free(ra, d); return 0xFF; }
                emit3(em, OP_DMOV, d, a, 0);
                rd_free(ra, a);
            } else if(src && src->kind == TY_F32) {
                u8 a = emit_expr(ctx, n->left);
                if(a==0xFF) { rd_free(ra, d); return 0xFF; }
                emit3(em, OP_F2D, d, a, 0);
                ra_free(ra, a);
            } else if(src && (src->kind==TY_U8 || src->kind==TY_U16 ||
                              src->kind==TY_U32)) {
                u8 a = emit_expr(ctx, n->left);
                if(a==0xFF) { rd_free(ra, d); return 0xFF; }
                emit3(em, OP_U2D, d, a, 0);
                ra_free(ra, a);
            } else {
                u8 a = emit_expr(ctx, n->left);
                if(a==0xFF) { rd_free(ra, d); return 0xFF; }
                emit3(em, OP_I2D, d, a, 0);
                ra_free(ra, a);
            }
            return d;
        }

        case ND_CALL: {
            /* f64-returning call: emit_call_with_dest sees the f64 node
             * type, stages f64 args through the d bank and materialises
             * the d1 return value into a fresh d register. */
            return emit_call_with_dest(ctx, n, REG_ZERO, 0);
        }

        case ND_BUILTIN_CALL: {
            u8 dop = f64_builtin_op(n->sval);
            if(!dop) {
                snprintf(em->err, sizeof(em->err),
                         "@%s has no f64 overload", n->sval);
                return 0xFF;
            }
            u8 a = emit_expr_d(ctx, n->children[0]);
            if(a==0xFF) return 0xFF;
            u8 b = 0;
            if(n->nchildren >= 2) {
                b = emit_expr_d(ctx, n->children[1]);
                if(b==0xFF) { rd_free(ra, a); return 0xFF; }
            }
            u8 d = rd_alloc(ra, em);
            if(d==0xFF) { rd_free(ra, a); if(b) rd_free(ra, b); return 0xFF; }
            emit3(em, dop, d, a, b);
            if(b) rd_free(ra, b);
            rd_free(ra, a);
            return d;
        }

        default:
            snprintf(em->err, sizeof(em->err),
                     "f64 codegen: unsupported expression kind %d", n->kind);
            return 0xFF;
    }
}

/* -------------------------------------------------------------------------
 * Aggregate-by-pointer helpers
 *
 * Calling convention for struct-typed values:
 *   - A struct *argument* is passed by placing the address of the source
 *     struct in the argument register; the callee copies it into its own
 *     frame slot (preserving by-value semantics).
 *   - A function returning a struct receives a hidden destination pointer
 *     in r1; the user-visible arguments occupy r2..rN. On return the
 *     callee memcpys its result into *r1 and leaves r1 as the dest addr.
 *
 * `emit_struct_addr` produces a register holding the absolute address of
 * a struct-typed expression's value (allocating a temp frame slot for
 * call-result rvalues). The caller must ra_free the returned register.
 * ---------------------------------------------------------------------- */
static u8 emit_struct_addr(EmCtx *ctx, Node *n) {
    IREmitter *em = ctx->em;
    if(n->kind == ND_CALL) {
        int sz = emit_type_size(em->tc, n->type);
        if(sz < 4) sz = 4;
        int off = frame_alloc_sz(&ctx->frame, NULL, n->type, sz);
        return emit_call_with_dest(ctx, n, REG_FP, off);
    }
    int off = 0;
    u8 base = emit_lvalue_addr(ctx, n, &off);
    u8 r = ra_alloc(&ctx->ra, em);
    if(r == 0xFF) return 0xFF;
    emit_lea_off(ctx, r, base, off);
    if(base != REG_FP && base != REG_ZERO) ra_free(&ctx->ra, base);
    return r;
}

/* Emit a function call. If the callee returns an aggregate, the call
 * result is written into the caller-provided buffer at [dest_base+dest_off]
 * and the returned register holds that buffer's address. Otherwise dest_*
 * are ignored and the returned register holds the scalar return value. */
static u8 emit_call_with_dest(EmCtx *ctx, Node *n, u8 dest_base, int dest_off) {
    IREmitter *em = ctx->em;
    RegAlloc  *ra = &ctx->ra;
    bool ret_struct = is_aggregate(n->type);
    int  arg_base  = ret_struct ? 2 : 1;
    /* The first `max_reg_args` args travel in r(arg_base)..r(REG_LAST).
     * The allocator only manages r1..r(REG_LAST), so anything beyond that
     * has to be pushed on the VM stack; the callee reads them from
     * positive offsets above its saved-FP slot. */
    int  max_reg_args = REG_LAST - arg_base + 1;
    if(n->nchildren > MAX_TOTAL_ARGS) {
        snprintf(em->err, sizeof(em->err),
                 "line %d: too many arguments in call (%d, max %d)",
                 n->line, n->nchildren, MAX_TOTAL_ARGS);
        return 0xFF;
    }
    int  nargs       = n->nchildren;
    int  nreg_args   = nargs < max_reg_args ? nargs : max_reg_args;
    int  nstack_args = nargs - nreg_args;

    /* When dest_base is a dynamic register (not FP/ZERO), spill the resolved
     * destination address to a frame slot before arg evaluation.  The allocator
     * may otherwise reclaim dest_base for a temporary, corrupting the address
     * we need to re-load into r1 before the CALL and to materialise the result
     * afterwards.  FP-relative destinations are always stable and need no spill. */
    int dest_spill_off = 0;
    bool dest_spilled  = false;
    if(ret_struct && dest_base != REG_FP && dest_base != REG_ZERO) {
        dest_spill_off = frame_alloc_sz(&ctx->frame, NULL, NULL, 4);
        /* LEA tmp = dest_base + dest_off, then ST32 into spill slot. */
        u8 tmp = ra_alloc(ra, em);
        if(tmp == 0xFF) return 0xFF;
        emit_lea_off(ctx, tmp, dest_base, dest_off);
        emit_store_off(ctx, OP_ST32, REG_FP, tmp, dest_spill_off);
        ra_free(ra, tmp);
        ra_free(ra, dest_base);   /* dest_base is no longer needed as a reg */
        dest_base  = REG_FP;
        dest_off   = dest_spill_off;
        dest_spilled = true;
    }

    /* For aggregate return: place the destination address into r1 and
     * reserve it so subsequent arg evaluation doesn't pick it. */
    if(ret_struct) {
        if(dest_spilled) {
            /* Reload the spilled address into r1. */
            emit_load_off(ctx, OP_LD32, 1, REG_FP, dest_off);
        } else {
            emit_lea_off(ctx, 1, dest_base, dest_off);
        }
        ra->used[1] = true;
    }

    /* Stack-passed args are spilled into frame slots first so we evaluate
     * them in left-to-right order (matching script semantics for any
     * side-effecting expressions), then later replay them onto the VM
     * stack via OP_PUSH after the register args are in place. */
    int stack_arg_spill[MAX_TOTAL_ARGS];
    for(int i=nreg_args; i<nargs; i++) {
        Node *child = n->children[i];
        u8 ar;
        if(child->type && child->type->kind == TY_F64) {
            /* Stack args are 4-byte slots; an f64 there would need an
             * 8-byte push and a matching callee layout. Rare enough to
             * gate for now. */
            snprintf(em->err, sizeof(em->err),
                     "line %d: f64 argument %d beyond register-passed "
                     "limit not yet implemented", n->line, i);
            return 0xFF;
        }
        if(is_aggregate(child->type)) {
            ar = emit_struct_addr(ctx, child);
        } else if(child->type && child->type->kind == TY_ARRAY) {
            int off = 0;
            u8 base = emit_lvalue_addr(ctx, child, &off);
            ar = ra_alloc(&ctx->ra, em);
            if(ar == 0xFF) return 0xFF;
            emit_lea_off(ctx, ar, base, off);
            if(base != REG_FP && base != REG_ZERO) ra_free(&ctx->ra, base);
        } else {
            ar = emit_expr(ctx, child);
        }
        if(ar == 0xFF) return 0xFF;
        stack_arg_spill[i] = frame_alloc_sz(&ctx->frame, NULL, NULL, 4);
        emit_store_off(ctx, OP_ST32, REG_FP, ar, stack_arg_spill[i]);
        ra_free(ra, ar);
    }

    /* Evaluate the register args into frame spill slots (left-to-right)
     * BEFORE touching the VM stack or any target register.  A nested
     * aggregate-returning call inside a later argument's expression uses r1
     * as its hidden return-destination and stages its own args through
     * r1..rN, which would otherwise clobber an earlier argument already
     * placed in a target register.  Spilling every argument first and only
     * loading the target registers immediately before the CALL makes
     * argument evaluation order-independent and clobber-safe. */
    /* f64 args travel in the d bank at the same argument index
     * (d(arg_base+i) for arg i), spilled to an 8-byte frame slot like
     * their r-bank counterparts. reg_arg_spill tracks the slot either
     * way; the child's type says which bank to use when reloading. */
    int reg_arg_spill[MAX_TOTAL_ARGS];
    for(int i=0;i<nreg_args;i++) {
        Node *child = n->children[i];
        u8 ar;
        if(child->type && child->type->kind == TY_F64) {
            u8 dr = emit_expr_d(ctx, child);
            if(dr == 0xFF) return 0xFF;
            reg_arg_spill[i] = frame_alloc_sz(&ctx->frame, NULL, NULL, 8);
            emit_dstore_off(ctx, REG_FP, dr, reg_arg_spill[i]);
            rd_free(ra, dr);
            continue;
        }
        if(is_aggregate(child->type)) {
            ar = emit_struct_addr(ctx, child);
        } else if(child->type && child->type->kind == TY_ARRAY) {
            /* Arrays decay to pointers at call boundaries: pass address. */
            int off = 0;
            u8 base = emit_lvalue_addr(ctx, child, &off);
            ar = ra_alloc(&ctx->ra, em);
            if(ar == 0xFF) return 0xFF;
            emit_lea_off(ctx, ar, base, off);
            if(base != REG_FP && base != REG_ZERO) ra_free(&ctx->ra, base);
        } else {
            ar = emit_expr(ctx, child);
        }
        if(ar == 0xFF) return 0xFF;
        reg_arg_spill[i] = frame_alloc_sz(&ctx->frame, NULL, NULL, 4);
        emit_store_off(ctx, OP_ST32, REG_FP, ar, reg_arg_spill[i]);
        ra_free(ra, ar);
    }

    /* Push stack args in REVERSE order so that the first stack arg
     * (child[nreg_args]) ends up at the lowest address - immediately above
     * the saved frame-pointer slot - giving the callee a stable layout
     * independent of nargs:  arg[nreg_args + j]  lives at FP + 8 + 4*j. */
    if(nstack_args > 0) {
        u8 tmp = ra_alloc(ra, em);
        if(tmp == 0xFF) return 0xFF;
        for(int i=nargs-1; i>=nreg_args; i--) {
            emit_load_off(ctx, OP_LD32, tmp, REG_FP, stack_arg_spill[i]);
            emit3(em, OP_PUSH, 0, tmp, 0);
        }
        ra_free(ra, tmp);
    }

    /* Load each staged register arg into its target register.  No CALL is
     * emitted between here and the CALL below, so these values are stable.
     * f64 args load into the d bank at the same index. */
    for(int i=0;i<nreg_args;i++) {
        u8 tgt = (u8)(arg_base + i);
        Node *child = n->children[i];
        if(child->type && child->type->kind == TY_F64) {
            emit_dload_off(ctx, tgt, REG_FP, reg_arg_spill[i]);
            ra->dused[tgt] = true;
        } else {
            emit_load_off(ctx, OP_LD32, tgt, REG_FP, reg_arg_spill[i]);
            ra->used[tgt] = true;
        }
    }

    /* Re-load the hidden destination pointer into r1 last: arg staging above
     * used r2..rN, but the dest pointer must be live in r1 at the CALL. */
    if(ret_struct) {
        if(dest_spilled) {
            emit_load_off(ctx, OP_LD32, 1, REG_FP, dest_off);
        } else {
            emit_lea_off(ctx, 1, dest_base, dest_off);
        }
    }

    /* Emit CALL */
    if(n->left->kind == ND_IDENT) {
        Symbol *sym = sym_lookup_global(ctx->syms, n->left->sval);
        if(!sym) sym = sym_lookup(ctx->syms, n->left->sval);
        if(sym && sym->kind == SYM_FN) {
            int fn_id = (int)sym->fn_id;
            u32 instr = IR_ENCODE(OP_CALL, 0, ((u32)fn_id>>16)&0xF, ((u32)fn_id>>12)&0xF, fn_id&0xFFF);
            emit(em, instr);
        } else {
            /* Local variable or parameter holding a fn-ptr: load its value
             * and dispatch via CALLP.  Falling into the direct-CALL path would
             * emit CALL fn0 (sym->fn_id is 0 for vars/params), which is wrong. */
            u8 fp = emit_expr(ctx, n->left);
            emit3(em, OP_CALLP, fp, 0, 0);
            ra_free(ra, fp);
        }
    } else {
        u8 fp = emit_expr(ctx, n->left);
        emit3(em, OP_CALLP, fp, 0, 0);
        ra_free(ra, fp);
    }

    /* Reclaim the stack space used by spilled args. The callee's OP_RET
     * restores SP to its own FP and pops the saved FP / return-PC, leaving
     * SP exactly where it was after our pushes. We bump SP back over them
     * here using an LEA on r15 (the SP register). */
    if(nstack_args > 0) {
        emit_imm(em, OP_LEA, REG_SP, REG_SP, 4 * nstack_args);
    }

    /* Release argument registers (and r1 if it held the dest pointer). */
    for(int i=0;i<nreg_args;i++) {
        Node *child = n->children[i];
        if(child->type && child->type->kind == TY_F64)
            rd_free(ra, (u8)(arg_base + i));
        else
            ra_free(ra, (u8)(arg_base + i));
    }
    if(ret_struct) ra_free(ra, 1);

    /* Void-returning calls produce no result; return REG_ZERO so callers
     * (e.g. ND_EXPR_STMT) can ra_free() it safely (ra_free guards REG_ZERO).
     * Without this, we would allocate r1 as the "result" and emit a spurious
     * MOV r1, r1 (no-op) or MOV rN, r1, which bleeds the last argument value
     * through as a fake return and corrupts register allocation for subsequent
     * fn-ptr calls. */
    if(!ret_struct && n->type && n->type->kind == TY_VOID) return REG_ZERO;

    /* Materialize the result. An f64 call returns its value in d1, the
     * mirror of r1 for scalar calls. */
    if(n->type && n->type->kind == TY_F64) {
        u8 d = rd_alloc(ra, em);
        if(d == 0xFF) return 0xFF;
        if(d != 1) emit3(em, OP_DMOV, d, 1, 0);
        return d;
    }
    u8 r = ra_alloc(ra, em);
    if(r == 0xFF) return 0xFF;
    if(ret_struct) {
        if(dest_spilled) {
            emit_load_off(ctx, OP_LD32, r, REG_FP, dest_off);
        } else {
            emit_lea_off(ctx, r, dest_base, dest_off);
        }
    } else {
        if(r != 1) emit3(em, OP_MOV, r, 1, 0);
    }
    return r;
}

/* -------------------------------------------------------------------------
 * Emit expression, returns register with result (caller must ra_free)
 * ---------------------------------------------------------------------- */
static u8 emit_expr(EmCtx *ctx, Node *n) {
    IREmitter *em  = ctx->em;
    RegAlloc  *ra  = &ctx->ra;
    if(n && n->line > 0) { em->cur_line = n->line; em->cur_file_id = n->file_id; }

    /* Struct-typed expressions are produced as addresses (by-pointer).
     * The four contexts that consume struct rvalues — call args, return,
     * var-decl init, and assign — handle the address explicitly. */
    if(is_aggregate(n->type)) {
        return emit_struct_addr(ctx, n);
    }

    /* f64 lives in the d bank and is emitted by emit_expr_d, dispatched
     * to by the parents that know operand width (binop compares, casts,
     * var/const/global init, assignment, expr-stmt, conditions). An
     * f64-typed node reaching here means a call site that doesn't
     * dispatch yet - fail loudly rather than emit a 32-bit load/store
     * or r-bank arithmetic and miscompile. */
    if(n->type && n->type->kind == TY_F64) {
        if(!em->err[0])
            snprintf(em->err, sizeof(em->err),
                     "f64 codegen not yet implemented for expression "
                     "kind %d", n->kind);
        return 0xFF;
    }

    switch(n->kind) {

        case ND_INT_LIT: case ND_CHAR_LIT: {
            u8 r = ra_alloc(ra, em); if(r==0xFF) return 0xFF;
            /* OP_LDC carries a 20-bit signed immediate (see IR_IMM20 in
             * vm.c and decode_imm20 in aot_x64_win.c + the JITs), but
             * emit_imm only encodes 12 bits and leaves the srcA/srcB
             * nibbles zero. For non-negative values the unused upper
             * 8 bits are zero either way, so the round-trip is exact.
             * For negative values the 12-bit two's-complement pattern
             * (e.g. -1 -> 0xFFF) is read back as a positive 20-bit
             * number (4095) because there is no sign extension at bit
             * 11. Route negative literals through OP_LDC32 to carry
             * the full 32-bit value safely. */
            if(n->ival >= 0 && n->ival <= 2047) {
                emit_imm(em, OP_LDC, r, 0, n->ival);
            } else {
                emit_imm(em, OP_LDC32, r, 0, 0);
                emit(em, (u32)n->ival);
            }
            return r;
        }

        case ND_FLOAT_LIT: {
            /* Emit raw IEEE-754 bit pattern as a 32-bit LDC32 immediate.
             * n->fval is a double (so f64 literals keep full precision);
             * narrowing to float here is the f32 path and is deliberate. */
            u8 r = ra_alloc(ra, em); if(r==0xFF) return 0xFF;
            union { float f; u32 u; } cv;
            cv.f = (float)n->fval;
            emit_imm(em, OP_LDC32, r, 0, 0);
            emit(em, cv.u);
            return r;
        }

        case ND_BOOL_LIT: {
            u8 r = ra_alloc(ra, em); if(r==0xFF) return 0xFF;
            emit_imm(em, OP_LDC, r, 0, n->bval ? 1 : 0);
            return r;
        }

        case ND_STRING_LIT: {
            /* String literal: load address of the string in vm memory.
             * Strings are placed in vm->mem at offset = ir->globals_size
             * (the VM copies ir->strings there at init time). The decoded
             * byte length is cached on the node (n->ival). To avoid
             * re-interning when the same node is visited twice, we mark
             * interned nodes by stashing the string-table offset in n->op
             * (an unused field for ND_STRING_LIT) plus a sentinel bit. */
            u8 r = ra_alloc(ra, em); if(r==0xFF) return 0xFF;
            int str_off;
            int str_len;
            if(n->op & 0x40000000) {
                /* Already interned on a previous visit */
                str_off = n->op & 0x3FFFFFFF;
                str_len = n->ival;
            } else if(em->tokens) {
                Token *tok = &em->tokens[n->soff];
                str_off = intern_string_lit(em->ir, tok->start, tok->len, &str_len);
                if(str_off < 0) {
                    snprintf(em->err, sizeof(em->err),
                             "line %d: string table overflow (limit %d bytes). Raise RELIC_MAX_STRINGS in include/tigryx.h and rebuild.",
                             n->line, RELIC_MAX_STRINGS);
                    return 0xFF;
                }
                n->op   = 0x40000000 | str_off; /* cache offset + sentinel */
                n->ival = str_len;              /* cache decoded length */
            } else {
                snprintf(em->err, sizeof(em->err),
                         "line %d: emitter has no token table for strings", n->line);
                return 0xFF;
            }
            /* Address in vm->mem = globals_size + str_off */
            i32 addr = (i32)em->ir->globals_size + str_off;
            emit_imm(em, OP_LDC32, r, 0, 0);
            emit(em, (u32)addr);
            return r;
        }

        case ND_IDENT: {
            Symbol *sym = sym_lookup(ctx->syms, n->sval);
            if(sym && sym->kind == SYM_CONST && sym->has_const_val) {
                u8 r = ra_alloc(ra, em); if(r==0xFF) return 0xFF;
                /* f32 consts need the full 32-bit IEEE-754 bit pattern.
                 * For ints, OP_LDC's immediate is written by emit_imm as 12
                 * bits but read back by the VM as a *signed 20-bit* field
                 * (IR_IMM20), so a negative 12-bit pattern such as -1 (0xFFF)
                 * decodes as +4095 -- there is no sign extension at bit 11.
                 * Mirror the ND_INT_LIT guard above and only take the OP_LDC
                 * fast path for non-negative values that round-trip exactly;
                 * everything else goes through OP_LDC32. */
                bool is_float = sym->type && sym->type->kind == TY_F32;
                if(!is_float && sym->const_val >= 0 && sym->const_val <= 2047) {
                    emit_imm(em, OP_LDC, r, 0, sym->const_val);
                } else {
                    emit_imm(em, OP_LDC32, r, 0, 0);
                    emit(em, (u32)sym->const_val);
                }
                return r;
            }
            if(sym && sym->kind == SYM_FN) {
                u8 r = ra_alloc(ra, em); if(r==0xFF) return 0xFF;
                /* fn_id exceeds the 12-bit LDC field in programs with
                 * >2047 functions - use LDC32+payload like big literals. */
                if(sym->fn_id >= 0 && sym->fn_id <= 2047) {
                    emit_imm(em, OP_LDC, r, 0, sym->fn_id);
                } else {
                    emit_imm(em, OP_LDC32, r, 0, 0);
                    emit(em, (u32)sym->fn_id);
                }
                return r;
            }
            int off = frame_find(&ctx->frame, n->sval);
            u8 r = ra_alloc(ra, em); if(r==0xFF) return 0xFF;
            /* Choose a load op matching the variable's declared type so that
             * sub-word locals (i8/u8/i16/u16/bool) don't read garbage from
             * the high bytes of their 4-byte stack slot. */
            u8 ld_op, st_op, sx_op;
            pick_mem_ops(n->type, &ld_op, &st_op, &sx_op);
            if(off != FRAME_NOT_FOUND) {
                /* local variable (offset relative to FP, typically negative) */
                emit_load_off(ctx, ld_op, r, REG_FP, off);
                if(sx_op) emit3(em, sx_op, r, r, 0);
            } else {
                /* global variable: load from absolute address */
                i32 addr = sym ? (i32)sym->offset : 0;
                u8 ar = ra_alloc(ra, em);
                if(addr >= -2048 && addr <= 2047)
                    emit_imm(em, OP_LDC, ar, 0, addr);
                else { emit_imm(em, OP_LDC32, ar, 0, 0); emit(em, (u32)addr); }
                emit_imm(em, ld_op, r, ar, 0);
                if(sx_op) emit3(em, sx_op, r, r, 0);
                ra_free(ra, ar);
            }
            return r;
        }

        case ND_UNOP: {
            bool is_float = n->left->type && n->left->type->kind == TY_F32;
            (void)is_float;
            if(n->op == TK_AMPERSAND) {
                /* address-of */
                int off = 0;
                u8 base = emit_lvalue_addr(ctx, n->left, &off);
                u8 r = ra_alloc(ra, em); if(r==0xFF) return 0xFF;
                emit_lea_off(ctx, r, base, off);
                if(base != REG_FP && base != REG_ZERO) ra_free(ra, base);
                return r;
            }
            if(n->op == TK_STAR) {
                u8 ptr = emit_expr(ctx, n->left);
                u8 r = ra_alloc(ra, em); if(r==0xFF) return 0xFF;
                /* Load at the pointee width: a 32-bit load reads the
                 * addressed byte in the wrong lane on big-endian hosts
                 * (and SX8/SX16 keep signed narrows correct). */
                u8 ld_op, st_op, sx_op;
                pick_mem_ops(n->type, &ld_op, &st_op, &sx_op);
                emit_imm(em, ld_op, r, ptr, 0);
                if(sx_op) emit3(em, sx_op, r, r, 0);
                ra_free(ra, ptr);
                return r;
            }
            /* Unary ops on a single operand can run in-place on `a`'s
             * register: this avoids ever holding two registers live at
             * once for the operand, which matters in tight max-arity
             * call sites where every register is already pinned for
             * positional argument staging. TK_BANG still needs a second
             * register to materialise the constant 0 to compare against. */
            u8 a = emit_expr(ctx, n->left);
            if(a == 0xFF) return 0xFF;
            switch(n->op) {
                case TK_MINUS: emit3(em, is_float ? OP_FNEG : OP_NEG, a, a, 0); return a;
                case TK_TILDE: emit3(em, OP_NOT,  a, a, 0); return a;
                case TK_BANG: {
                    u8 r = ra_alloc(ra, em); if(r==0xFF) { ra_free(ra, a); return 0xFF; }
                    emit_imm(em, OP_LDC, r, 0, 0);
                    emit3(em, OP_CEQ, r, a, r);
                    ra_free(ra, a);
                    return r;
                }
                default: break;
            }
            ra_free(ra, a);
            return a;
        }

        case ND_BINOP: {
            if(n->left->type && n->left->type->kind == TY_F64) {
                /* f64 operands: only comparisons reach here (the node's
                 * own type is bool). f64 *arithmetic* nodes are f64-typed
                 * and are emitted wholesale by emit_expr_d. GT/GE swap
                 * operands, mirroring the FCLT/FCLE syntheses below. */
                u8 da = emit_expr_d(ctx, n->left);
                if(da==0xFF) return 0xFF;
                u8 db = emit_expr_d(ctx, n->right);
                if(db==0xFF) { rd_free(ra, da); return 0xFF; }
                u8 r = ra_alloc(ra, em);
                if(r==0xFF) { rd_free(ra, da); rd_free(ra, db); return 0xFF; }
                bool okc = true;
                switch(n->op) {
                    case TK_EQ:  emit3(em, OP_DCEQ, r, da, db); break;
                    case TK_NEQ: emit3(em, OP_DCEQ, r, da, db);
                                 emit3(em, OP_CEQ,  r, r, REG_ZERO); break;
                    case TK_LT:  emit3(em, OP_DCLT, r, da, db); break;
                    case TK_LE:  emit3(em, OP_DCLE, r, da, db); break;
                    case TK_GT:  emit3(em, OP_DCLT, r, db, da); break;
                    case TK_GE:  emit3(em, OP_DCLE, r, db, da); break;
                    default: okc = false; break;
                }
                rd_free(ra, db); rd_free(ra, da);
                if(!okc) {
                    snprintf(em->err, sizeof(em->err),
                             "f64 operator not supported");
                    ra_free(ra, r);
                    return 0xFF;
                }
                return r;
            }
            u8 a = emit_expr(ctx, n->left);
            u8 r = ra_alloc(ra, em); if(r==0xFF) return 0xFF;
            /* Copy left side into the result register so that emitting the
             * right side (which may issue CALL/BUILTIN and clobber r1..rN)
             * cannot destroy our left operand. Free the original `a` once
             * its value has been moved — otherwise it leaks permanently. */
            if(a != r) {
                emit3(em, OP_MOV, r, a, 0);
                ra_free(ra, a);
                a = r;
            }
            /* If the right operand contains a CALL/BUILTIN, staging its
             * args into r1..rN will MOV over whatever is currently there —
             * including our saved left operand. Spill `a` to a frame slot
             * across the right-hand emit, then reload into a fresh reg.
             * This mirrors the manual workaround of extracting the call
             * result to a local variable. */
            int spill_off = 0;
            bool spilled = expr_has_call(n->right);
            if(spilled) {
                spill_off = frame_alloc(&ctx->frame, NULL, NULL);
                emit_store_off(ctx, OP_ST32, REG_FP, a, spill_off);
                ra_free(ra, a);
            }
            u8 b = emit_expr(ctx, n->right);
            if(spilled) {
                a = ra_alloc(ra, em); if(a==0xFF) return 0xFF;
                emit_load_off(ctx, OP_LD32, a, REG_FP, spill_off);
                r = a;
            }
            /* Float / unsigned dispatch keyed on operand type. The type
             * checker promotes both operands to a common type, so a
             * single side suffices. Unsigned integer compares need the
             * OP_CLTU / OP_CLEU forms; without that, u32 values with
             * bit 31 set would be treated as negative i32 by OP_CLT /
             * OP_CGT and produce wrong results (e.g.
             * `0u < 0xFFFFFFFFu` would be evaluated as `0 < -1`). */
            bool is_float = n->left->type && n->left->type->kind == TY_F32;
            bool is_unsigned = n->left->type && (n->left->type->kind == TY_U8
                                              || n->left->type->kind == TY_U16
                                              || n->left->type->kind == TY_U32);
            switch(n->op) {
                case TK_PLUS:      emit3(em, is_float?OP_FADD:OP_ADD, r, a, b); break;
                case TK_MINUS:     emit3(em, is_float?OP_FSUB:OP_SUB, r, a, b); break;
                case TK_STAR:      emit3(em, is_float?OP_FMUL:OP_MUL, r, a, b); break;
                case TK_SLASH:     emit3(em, is_float?OP_FDIV:OP_DIV, r, a, b); break;
                case TK_PERCENT:   emit3(em, OP_MOD,  r, a, b); break;
                case TK_AMPERSAND: emit3(em, OP_AND,  r, a, b); break;
                case TK_PIPE:      emit3(em, OP_OR,   r, a, b); break;
                case TK_CARET:     emit3(em, OP_XOR,  r, a, b); break;
                case TK_LSHIFT:    emit3(em, OP_SHL,  r, a, b); break;
                case TK_RSHIFT:    emit3(em, OP_SHR,  r, a, b); break;
                case TK_EQ:
                    if(is_float) emit3(em, OP_FCEQ, r, a, b);
                    else         emit3(em, OP_CEQ,  r, a, b);
                    break;
                case TK_NEQ:
                    if(is_float) {
                        emit3(em, OP_FCEQ, r, a, b);
                        emit3(em, OP_CEQ,  r, r, REG_ZERO); /* logical not */
                    } else emit3(em, OP_CNE, r, a, b);
                    break;
                case TK_LT:
                    if(is_float)         emit3(em, OP_FCLT, r, a, b);
                    else if(is_unsigned) emit3(em, OP_CLTU, r, a, b);
                    else                 emit3(em, OP_CLT,  r, a, b);
                    break;
                case TK_LE:
                    if(is_float)         emit3(em, OP_FCLE, r, a, b);
                    else if(is_unsigned) emit3(em, OP_CLEU, r, a, b);
                    else                 emit3(em, OP_CLE,  r, a, b);
                    break;
                case TK_GT:
                    /* a > b == b < a. No OP_CGTU exists; synthesise via
                     * OP_CLTU with operands swapped, mirroring how the
                     * float path synthesises a > b as FCLT(b, a). */
                    if(is_float)         emit3(em, OP_FCLT, r, b, a);
                    else if(is_unsigned) emit3(em, OP_CLTU, r, b, a);
                    else                 emit3(em, OP_CGT,  r, a, b);
                    break;
                case TK_GE:
                    /* a >= b == b <= a. No OP_CGEU exists; same trick. */
                    if(is_float)         emit3(em, OP_FCLE, r, b, a);
                    else if(is_unsigned) emit3(em, OP_CLEU, r, b, a);
                    else                 emit3(em, OP_CGE,  r, a, b);
                    break;
                case TK_AND: {
                    /* short-circuit: r = a != 0 && b != 0 */
                    u8 tmp = ra_alloc(ra, em);
                    emit_imm(em, OP_LDC, tmp, 0, 0);
                    emit3(em, OP_CNE, r,   a, tmp);
                    emit3(em, OP_CNE, tmp, b, tmp);
                    emit3(em, OP_AND, r,   r, tmp);
                    ra_free(ra, tmp);
                    break;
                }
                case TK_OR: {
                    u8 tmp = ra_alloc(ra, em);
                    emit_imm(em, OP_LDC, tmp, 0, 0);
                    emit3(em, OP_CNE, r,   a, tmp);
                    emit3(em, OP_CNE, tmp, b, tmp);
                    emit3(em, OP_OR,  r,   r, tmp);
                    ra_free(ra, tmp);
                    break;
                }
                default: break;
            }
            /* `a` was aliased to `r` above; freeing it here would release
             * the very register we're returning to the caller. Only `b`
             * needs to be released. */
            ra_free(ra, b);
            return r;
        }

        case ND_CAST: {
            /* f64 source: the value arrives in a d register and converts
             * to the destination width. D2I/D2U truncate toward zero,
             * matching the F2I convention. */
            Type *src = n->left->type;
            if(src && src->kind == TY_F64) {
                u8 da = emit_expr_d(ctx, n->left);
                if(da==0xFF) return 0xFF;
                u8 r = ra_alloc(ra, em);
                if(r==0xFF) { rd_free(ra, da); return 0xFF; }
                switch(n->type->kind) {
                    case TY_F32: emit3(em, OP_D2F, r, da, 0); break;
                    case TY_U32:
                        emit3(em, OP_D2U, r, da, 0); break;
                    case TY_U16:
                        emit3(em, OP_D2U, r, da, 0);
                        emit3(em, OP_ZX16, r, r, 0); break;
                    case TY_U8:
                        emit3(em, OP_D2U, r, da, 0);
                        emit3(em, OP_ZX8, r, r, 0); break;
                    case TY_I8:
                        emit3(em, OP_D2I, r, da, 0);
                        emit3(em, OP_TRUNC8, r, r, 0);
                        emit3(em, OP_SX8,   r, r, 0); break;
                    case TY_I16:
                        emit3(em, OP_D2I, r, da, 0);
                        emit3(em, OP_TRUNC16, r, r, 0);
                        emit3(em, OP_SX16,  r, r, 0); break;
                    default:
                        emit3(em, OP_D2I, r, da, 0); break;
                }
                rd_free(ra, da);
                return r;
            }
            u8 a = emit_expr(ctx, n->left);
            u8 r = ra_alloc(ra, em); if(r==0xFF) return 0xFF;
            bool dst_f = n->type->kind == TY_F32;
            bool src_f = src && src->kind == TY_F32;
            if(dst_f && !src_f) {
                /* int -> f32 */
                emit3(em, OP_I2F, r, a, 0);
            } else if(!dst_f && src_f) {
                /* f32 -> int (truncate toward zero, then narrow if needed) */
                emit3(em, OP_F2I, r, a, 0);
                switch(n->type->kind) {
                    case TY_I8:  emit3(em, OP_TRUNC8,  r, r, 0); emit3(em, OP_SX8,  r, r, 0); break;
                    case TY_U8:  emit3(em, OP_ZX8,     r, r, 0); break;
                    case TY_I16: emit3(em, OP_TRUNC16, r, r, 0); emit3(em, OP_SX16, r, r, 0); break;
                    case TY_U16: emit3(em, OP_ZX16,    r, r, 0); break;
                    default: break;
                }
            } else {
                switch(n->type->kind) {
                    case TY_I8:  emit3(em, OP_TRUNC8,  r, a, 0); emit3(em, OP_SX8,  r, r, 0); break;
                    case TY_U8:  emit3(em, OP_ZX8,   r, a, 0); break;
                    case TY_I16: emit3(em, OP_TRUNC16, r, a, 0); emit3(em, OP_SX16, r, r, 0); break;
                    case TY_U16: emit3(em, OP_ZX16,  r, a, 0); break;
                    default:     emit3(em, OP_MOV,   r, a, 0); break;
                }
            }
            ra_free(ra, a);
            return r;
        }

        case ND_INDEX: {
            int off = 0;
            u8 addr = emit_lvalue_addr(ctx, n, &off);
            u8 r = ra_alloc(ra, em); if(r==0xFF) return 0xFF;
            u8 ld_op, st_op, sx_op;
            pick_mem_ops(n->type, &ld_op, &st_op, &sx_op);
            emit_load_off(ctx, ld_op, r, addr, off);
            if(sx_op) emit3(em, sx_op, r, r, 0);
            if(addr != REG_FP && addr != REG_ZERO) ra_free(ra, addr);
            return r;
        }

        case ND_FIELD_ACCESS: {
            int off = 0;
            u8 base = emit_lvalue_addr(ctx, n, &off);
            u8 r = ra_alloc(ra, em); if(r==0xFF) return 0xFF;
            u8 ld_op, st_op, sx_op;
            pick_mem_ops(n->type, &ld_op, &st_op, &sx_op);
            emit_load_off(ctx, ld_op, r, base, off);
            if(sx_op) emit3(em, sx_op, r, r, 0);
            if(base != REG_FP && base != REG_ZERO) ra_free(ra, base);
            return r;
        }

        case ND_CALL: {
            /* Aggregate-returning calls are intercepted by the early
             * is_aggregate guard in emit_expr; this path handles only
             * scalar-returning calls. The dest_* args are unused. */
            return emit_call_with_dest(ctx, n, REG_ZERO, 0);
        }

        case ND_BUILTIN_CALL: {
            /* map builtin name to opcode, args in r1..rN */
            static const struct { const char *name; u8 op; } btbl[] = {
                {"clear",        OP_GFX_CLEAR},
                {"sprite",       OP_GFX_SPRITE},
                {"plot",         OP_GFX_PLOT},   {"flip",       OP_GFX_FLIP},
                {"line",         OP_GFX_LINE},   {"window",     OP_GFX_WINDOW},
                {"polygon",      OP_GFX_POLYGON},{"polyline",   OP_GFX_POLYLINE},
                {"ellipse",       OP_GFX_ELLIPSE},{"ellipse_filled", OP_GFX_ELLIPSE_FILLED},
                {"rounded_rectangle", OP_GFX_ROUNDED_RECT},{"rounded_rectangle_filled", OP_GFX_ROUNDED_RECT_FILLED},
                {"camera",       OP_GFX_CAMERA}, {"texture",    OP_GFX_TEXTURE},
                {"palette_upload", OP_GFX_PALETTE_UPLOAD},
                {"wr_palette",   OP_WR_PALETTE}, {"wr_texture", OP_WR_TEXTURE},
                {"wr_flat",      OP_WR_FLAT},    {"wr_test",    OP_WR_TEST},
                {"wr_map",       OP_WR_MAP},      {"wr_render", OP_WR_RENDER},
                {"wr_texmap",    OP_WR_TEXMAP}, {"wr_sky",     OP_WR_SKY},
                {"wr_flatshade", OP_WR_FLATSHADE},
                {"wr_patch",     OP_WR_PATCH},
                {"wr_sprite",    OP_WR_SPRITE},
                {"wr_blockmap",  OP_WR_BLOCKMAP},
                {"wr_blocked",   OP_WR_BLOCKED},
                {"wr_blocked_line", OP_WR_BLOCKED_LINE},
                {"light",        OP_GFX_LIGHT},  {"mesh",       OP_GFX_MESH},
                {"fog",          OP_GFX_FOG},
                {"text",         OP_GFX_TEXT},   {"viewport",   OP_GFX_VIEWPORT},
                {"text_measure", OP_GFX_TEXT_MEASURE}, {"clip",   OP_GFX_CLIP},
                {"view",         OP_GFX_VIEW},
                {"view_capture", OP_GFX_VIEW_CAPTURE},
                {"view_present", OP_GFX_VIEW_PRESENT},
                {"draw_list",    OP_GFX_DRAW_LIST},
                {"draw_mode",    OP_GFX_DRAW_MODE},
                {"lines3d",      OP_GFX_LINES3D},
                {"grid3d",       OP_GFX_GRID3D},
                {"key_pressed",  OP_INP_KEY},    {"key_held",   OP_INP_KEYHELD},
                {"mouse",        OP_INP_MOUSE},
                {"gamepad_count",OP_INP_GAMEPAD_COUNT}, {"gamepad", OP_INP_GAMEPAD},
                {"rand",         OP_SYS_RAND},   {"millis",     OP_SYS_MILLIS},
                {"console",      OP_SYS_CONSOLE},
                {"snprint",     OP_SYS_SNPRINT},
                {"assert",       OP_SYS_ASSERT},
                {"exit",         OP_HALT},
                {"alloc",        OP_SYS_ALLOC},  {"free",       OP_SYS_FREE},
                {"memcpy",       OP_SYS_MEMCPY}, {"memset",     OP_SYS_MEMSET},
                {"file_open",    OP_SYS_FOPEN},  {"file_read",  OP_SYS_FREAD},
                {"file_write",   OP_SYS_FWRITE}, {"file_close", OP_SYS_FCLOSE},
                {"file_seek",    OP_SYS_FSEEK},  {"file_length",OP_SYS_FLEN},
                {"file_directory",OP_SYS_FDIR}, {"file_path",  OP_SYS_FPATH},
                {"file_load_png",OP_SYS_LOAD_PNG},
                {"file_delete",  OP_SYS_FDELETE},
                {"file_mkdir",   OP_SYS_FMKDIR},
                {"clipboard_set",OP_SYS_CLIP_SET},{"clipboard_get",OP_SYS_CLIP_GET},
                {"args",         OP_SYS_ARGS},   {"is_number",  OP_SYS_ISNUM},
                {"sin",          OP_SYS_SIN},    {"cos",        OP_SYS_COS},
                {"sqrt",         OP_SYS_SQRT},
                {"exp",          OP_SYS_EXP},    {"log",        OP_SYS_LOG},
                {"log10",        OP_SYS_LOG10},  {"pow",        OP_SYS_POW},
                {"atan",         OP_SYS_ATAN},   {"atan2",      OP_SYS_ATAN2},
                {"tan",          OP_SYS_TAN},    {"asin",       OP_SYS_ASIN},
                {"acos",         OP_SYS_ACOS},   {"floor",      OP_SYS_FLOOR},
                {"ceil",         OP_SYS_CEIL},   {"round",      OP_SYS_ROUND},
                {"fmod",         OP_SYS_FMOD},   {"fabs",       OP_SYS_FABS},
                {"sinh",         OP_SYS_SINH},   {"cosh",       OP_SYS_COSH},
                {"tanh",         OP_SYS_TANH},
                {"http_request", OP_SYS_HTTP_REQ},   {"http_poll",  OP_SYS_HTTP_POLL},
                {"http_status",  OP_SYS_HTTP_STATUS},{"http_read",  OP_SYS_HTTP_READ},
                {"http_close",   OP_SYS_HTTP_CLOSE},
                {"unzip",        OP_SYS_UNZIP},
                {"register",     OP_SYS_REGISTER},
                {"process_spawn",      OP_SYS_PROC_SPAWN},
                {"process_poll",       OP_SYS_PROC_POLL},
                {"process_exit_code",  OP_SYS_PROC_EXIT_CODE},
                {"process_read_stdout",OP_SYS_PROC_READ_OUT},
                {"process_read_stderr",OP_SYS_PROC_READ_ERR},
                {"process_kill",       OP_SYS_PROC_KILL},
                {"process_close",      OP_SYS_PROC_CLOSE},
                {"serve",              OP_SYS_SERVE},
                {"serve_reply",        OP_SYS_SERVE_REPLY},
                /* @os_* embedded-host syscalls - all share OP_SYS_OS;
                 * the sub-op is looked up again below and encoded into
                 * the instruction's SRCA field. */
                {"os_spawn",   OP_SYS_OS}, {"os_kill",    OP_SYS_OS},
                {"os_status",  OP_SYS_OS}, {"os_focus",   OP_SYS_OS},
                {"os_count",   OP_SYS_OS}, {"os_handle",  OP_SYS_OS},
                {"os_set_rect",OP_SYS_OS}, {"os_get_rect",OP_SYS_OS},
                {"os_name",    OP_SYS_OS}, {"os_iconify", OP_SYS_OS},
                {"os_dock",    OP_SYS_OS},
                {NULL, 0}
            };
            u8 op = 0;
            bool found = false;
            for(int i=0; btbl[i].name; i++)
                if(strcmp(btbl[i].name, n->sval)==0) { op=btbl[i].op; found=true; break; }
            
            // Handle @tone overload
            if(strcmp(n->sval, "tone")==0) {
                if(n->nchildren == 1) {
                    op = OP_SND_TONE_ID;  // @tone(sound_id: u8) - will add default volume
                } else if(n->nchildren == 2) {
                    // Check if first child is a small integer literal (sound id) vs frequency
                    Node *first = n->children[0];
                    if(first->kind == ND_INT_LIT && first->ival >= 0 && first->ival < 256) {
                        op = OP_SND_TONE_ID;  // @tone(sound_id: u8, volume: u8)
                    } else {
                        op = OP_SND_TONE_FREQ;  // @tone(frequency: u16, duration_ms: u16) - will add default volume
                    }
                } else {
                    // 3-arg: same heuristic — small literal first arg → preset id, otherwise frequency
                    Node *first = n->children[0];
                    if(first->kind == ND_INT_LIT && first->ival >= 0 && first->ival < 256) {
                        op = OP_SND_TONE_ID;  // @tone(sound_id: u8, duration_ms: u16, volume: u8)
                    } else {
                        op = OP_SND_TONE_FREQ;  // @tone(frequency: u16, duration_ms: u16, volume: u8)
                    }
                }
                found = true;
            }
            // Handle @play
            if(strcmp(n->sval, "play")==0) {
                op = OP_SND_PLAY;
                found = true;
            }
            // Handle @music
            if(strcmp(n->sval, "music")==0) {
                op = OP_SND_MUSIC;
                found = true;
            }
            // Handle @mixer
            if(strcmp(n->sval, "mixer")==0) {
                op = OP_SND_MIXER;
                found = true;
            }
            // Handle @platform
            if(strcmp(n->sval, "platform")==0) {
                op = OP_SYS_PLATFORM;
                found = true;
            }
            
            if(!found) {
                snprintf(em->err, sizeof(em->err),
                         "line %d: unknown builtin '@%s'", n->line, n->sval);
                return 0xFF;
            }

            /* Builtins pass args in r1..r(nargs). The allocator pool is
             * r1..r13 (r0=ZERO, r14=FP, r15=reserved), so 13 is the hard
             * upper bound. Args beyond this would need stack-overflow
             * handling like user-defined CALLs; no current builtin needs it. */
            #define MAX_BUILTIN_ARGS 13
            int nargs = n->nchildren < MAX_BUILTIN_ARGS ? n->nchildren : MAX_BUILTIN_ARGS;
            /* Evaluate args left-to-right and spill each result to a frame
             * slot first.  A subsequent arg's expression may itself contain
             * a CALL whose ABI writes directly into r1..r(N) — that would
             * clobber any args we'd already loaded into target registers.
             * After all args are computed, reload them from their slots into
             * r1..r(nargs) immediately before the builtin op. */
            int arg_spill[MAX_BUILTIN_ARGS];
            u8 arg_f64[MAX_BUILTIN_ARGS];
            for(int i=0;i<nargs;i++) {
                Node *child = n->children[i];
                u8 ar;
                if(child->type && child->type->kind == TY_F64) {
                    /* f64 builtin args travel in the d bank at the same
                     * argument index - the convention CALL already uses.
                     * Only @snprint consumes them (%lf/%le/%lg). */
                    if(op != OP_SYS_SNPRINT) {
                        snprintf(em->err, sizeof(em->err),
                                 "line %d: f64 argument to @%s not supported",
                                 n->line, n->sval);
                        return 0xFF;
                    }
                    u8 dr = emit_expr_d(ctx, child);
                    if(dr == 0xFF) return 0xFF;
                    arg_spill[i] = frame_alloc_sz(&ctx->frame, NULL, NULL, 8);
                    emit_dstore_off(ctx, REG_FP, dr, arg_spill[i]);
                    rd_free(ra, dr);
                    arg_f64[i] = 1;
                    continue;
                }
                arg_f64[i] = 0;
                if(child->type && child->type->kind == TY_ARRAY) {
                    /* Arrays decay to pointers at builtin boundaries: pass
                     * address, not the first word of the array's storage. */
                    int off = 0;
                    u8 base = emit_lvalue_addr(ctx, child, &off);
                    if(base == 0xFF) return 0xFF;
                    ar = ra_alloc(ra, em);
                    if(ar == 0xFF) return 0xFF;
                    emit_lea_off(ctx, ar, base, off);
                    if(base != REG_FP && base != REG_ZERO) ra_free(ra, base);
                } else {
                    ar = emit_expr(ctx, child);
                }
                if(ar == 0xFF) return 0xFF;
                arg_spill[i] = frame_alloc_sz(&ctx->frame, NULL, NULL, 4);
                emit_store_off(ctx, OP_ST32, REG_FP, ar, arg_spill[i]);
                ra_free(ra, ar);
            }
            for(int i=0;i<nargs;i++) {
                u8 tgt = (u8)(1+i);
                if(arg_f64[i]) {
                    emit_dload_off(ctx, tgt, REG_FP, arg_spill[i]);
                    ra->dused[tgt] = true;
                } else {
                    emit_load_off(ctx, OP_LD32, tgt, REG_FP, arg_spill[i]);
                    ra->used[tgt] = true;
                }
            }

            /* Add default volume parameter for sound functions if not provided */
            if(op == OP_SND_TONE_FREQ && nargs == 2) {
                /* @tone(freq, duration) -> add volume=255 */
                emit_imm(em, OP_LDC, 3, 0, 255);
                ra->used[3] = true;
                nargs = 3;
            } else if(op == OP_SND_TONE_ID && nargs == 1) {
                /* @tone(id) -> add volume=255 */
                emit_imm(em, OP_LDC, 2, 0, 255);
                ra->used[2] = true;
                nargs = 2;
            } else if(op == OP_SND_PLAY && nargs == 2) {
                /* @play(ptr, length) -> add volume=255 */
                emit_imm(em, OP_LDC, 3, 0, 255);
                ra->used[3] = true;
                nargs = 3;
            } else if(op == OP_GFX_WINDOW && nargs == 4) {
                /* @window(x,y,w,h) with no flags -> default flags=0 in r5
                 * and default title=0 (NULL -> "Tigryx") in r6.
                 * Backends read r[5] as the flag word; without an explicit
                 * zero it would hold a stale register value (garbage flags,
                 * e.g. spuriously enabling stretch mode / disabling MSAA). */
                emit_imm(em, OP_LDC, 5, 0, 0);
                ra->used[5] = true;
                emit_imm(em, OP_LDC, 6, 0, 0);
                ra->used[6] = true;
                nargs = 6;
            } else if(op == OP_GFX_WINDOW && nargs == 5) {
                /* @window(x,y,w,h,flags) with no title -> default title=0
                 * (NULL -> "Tigryx") in r6. */
                emit_imm(em, OP_LDC, 6, 0, 0);
                ra->used[6] = true;
                nargs = 6;
            }
            /* @file_open passes args via r1=ptr, r2=length, r3=mode.
             * The user's `mode` argument is already loaded into r2 by the
             * generic arg-reload above; relocate it to r3 first, then
             * overwrite r2 with the literal string length. */
            bool needs_r2_len = (op == OP_SYS_FOPEN);
            if(needs_r2_len && nargs >= 2) {
                /* Move mode (currently in r2) to r3. */
                emit3(em, OP_MOV, 3, 2, 0);
                ra->used[3] = true;
                if(n->children[0]->kind == ND_STRING_LIT) {
                    i32 slen = n->children[0]->ival;
                    if(slen >= -2048 && slen <= 2047) {
                        emit_imm(em, OP_LDC, 2, 0, slen);
                    } else {
                        emit_imm(em, OP_LDC32, 2, 0, 0);
                        emit(em, (u32)slen);
                    }
                } else {
                    /* Non-literal (dynamic) path: length is unknown at
                     * compile time, so pass 0. The fopen backends treat a
                     * 0 length as "derive from the NUL terminator", which
                     * makes runtime-built filenames (e.g. from @args) work. */
                    emit_imm(em, OP_LDC, 2, 0, 0);
                }
                ra->used[2] = true;
            }
            /* @exit(code): the code arg is staged into r1 like every
             * builtin, so emit HALT with src_a=1 - the VM reads the exit
             * code from r[src_a] when src_a != 0, falling back to imm12.
             * (Historically imm12 was always 0 here, so @exit's argument
             * was silently dropped and every program exited with code 0.) */
            if(op == OP_HALT && nargs >= 1) {
                emit(em, IR_ENCODE(op, 0, 1, 0, 0));
            } else {
                u8 subop = 0;
                if(op == OP_SYS_OS) {
                    /* @os_* -> RELIC_OS_* sub-op in the SRCA field, so
                     * call args keep their normal r1..rN positions. */
                    static const struct { const char *name; u8 sub; }
                    ostbl[] = {
                        {"os_spawn",  RELIC_OS_SPAWN},
                        {"os_kill",   RELIC_OS_KILL},
                        {"os_status", RELIC_OS_STATUS},
                        {"os_focus",  RELIC_OS_FOCUS},
                        {"os_count",  RELIC_OS_COUNT},
                        {"os_handle", RELIC_OS_HANDLE},
                        {"os_set_rect", RELIC_OS_SET_RECT},
                        {"os_get_rect", RELIC_OS_GET_RECT},
                        {"os_name",   RELIC_OS_NAME},
                        {"os_iconify", RELIC_OS_ICONIFY},
                        {"os_dock",   RELIC_OS_DOCK},
                        {NULL, 0}
                    };
                    for(int i=0; ostbl[i].name; i++)
                        if(strcmp(ostbl[i].name, n->sval) == 0)
                            { subop = ostbl[i].sub; break; }
                }
                emit(em, IR_ENCODE(op, 0, subop, 0, 0));
            }
            /* release argument registers */
            for(int i=0;i<nargs;i++) ra_free(ra, (u8)(1+i));
            if(needs_r2_len && nargs >= 2) {
                ra_free(ra, 2);
                ra_free(ra, 3);
            }
            u8 r = ra_alloc(ra, em); if(r==0xFF) return 0xFF;
            if(r != 1) emit3(em, OP_MOV, r, 1, 0);
            return r;
        }

        default: {
            snprintf(em->err, sizeof(em->err), "line %d: unhandled expr node %d", n->line, n->kind);
            return 0xFF;
        }
    }
}

/* -------------------------------------------------------------------------
 * Emit store to lvalue
 * ---------------------------------------------------------------------- */
static bool emit_store(EmCtx *ctx, Node *lval, u8 src) {
    int off = 0;
    u8 addr = emit_lvalue_addr(ctx, lval, &off);
    if(addr == 0xFF) return false;
    u8 ld_op, st_op, sx_op;
    pick_mem_ops(lval->type, &ld_op, &st_op, &sx_op);
    emit_store_off(ctx, st_op, addr, src, off);
    if(addr != REG_FP && addr != REG_ZERO) ra_free(&ctx->ra, addr);
    (void)ld_op;
    return true;
}

/* -------------------------------------------------------------------------
 * match on integer constants: binary decision tree
 *
 * The default lowering below evaluates arms as a linear CEQ/JT chain -
 * O(N) per evaluation, fine for a few arms but hot in a dispatch loop
 * (relvm's vm_run does a ~150-way match per guest instruction). When
 * every pattern folds to an integer constant we instead sort the
 * (value -> arm) pairs and emit a binary search: each internal node is
 * `if val <= K goto left` (CLE/CLEU + JT) and each leaf does a short
 * CEQ/JT chain over <= MATCH_TREE_LEAF candidates before jumping to the
 * default (the wildcard arm, or the end of the match).
 *
 * Semantics are preserved exactly: the subject is still evaluated once
 * into `val`, the first-listed arm wins on duplicate values, a wildcard
 * anywhere becomes the default, and patterns in arms after a wildcard
 * are unreachable in both schemes. Only used when the subject has an
 * integer/bool/pointer type and every pattern constant-folds; otherwise
 * the caller runs the linear path (which also handles f32 subjects and
 * pattern expressions with side effects).
 *
 * Emits only ordinary LDC/LDC32/CLE/CLEU/CEQ/JT/JF/JMP ops, so no
 * interpreter, JIT, AOT or codegen backend needs to know about it.
 * ---------------------------------------------------------------------- */
#define MATCH_TREE_MIN  8   /* constant patterns required to bother    */
#define MATCH_TREE_LEAF 4   /* candidates checked linearly per leaf    */

typedef struct { i32 v; int arm; } MatchPair;

/* Load a full i32 constant into a scratch register (LDC for the common
 * small-positive case, LDC32+word otherwise). Caller ra_frees it. */
static u8 emit_const_reg(EmCtx *ctx, i32 v) {
    IREmitter *em = ctx->em;
    u8 r = ra_alloc(&ctx->ra, em);
    if(r == 0xFF) return 0xFF;
    if(v >= 0 && v <= 2047) {
        emit_imm(em, OP_LDC, r, 0, v);
    } else {
        emit_imm(em, OP_LDC32, r, 0, 0);
        emit(em, (u32)v);
    }
    return r;
}

static int match_pair_cmp(const void *a, const void *b) {
    i32 d = ((const MatchPair *)a)->v - ((const MatchPair *)b)->v;
    if(d < 0) return -1;
    if(d > 0) return 1;
    return ((const MatchPair *)a)->arm - ((const MatchPair *)b)->arm;
}

/* Recursive splitter: `if val <= pairs[mid].v goto left`, right subtree
 * emitted inline so it is the fall-through path. Leaf ranges run a short
 * CEQ/JT chain then jump to the default label. */
static bool emit_match_bst(EmCtx *ctx, u8 val, MatchPair *pairs,
                           int lo, int hi, int *body_lbl, int default_lbl,
                           bool unsigned_cmp) {
    IREmitter *em = ctx->em;
    int span = hi - lo + 1;
    if(span <= MATCH_TREE_LEAF) {
        for(int k = lo; k <= hi; k++) {
            u8 preg = emit_const_reg(ctx, pairs[k].v);
            if(preg == 0xFF) return false;
            u8 eq = ra_alloc(&ctx->ra, em);
            if(eq == 0xFF) { ra_free(&ctx->ra, preg); return false; }
            emit3(em, OP_CEQ, eq, val, preg);
            ra_free(&ctx->ra, preg);
            emit_jmp(em, OP_JT, eq, body_lbl[pairs[k].arm]);
            ra_free(&ctx->ra, eq);
        }
        emit_jmp(em, OP_JMP, 0, default_lbl);
        return true;
    }
    int mid = lo + span / 2;
    int l_lo = new_label(em);
    u8 kreg = emit_const_reg(ctx, pairs[mid].v);
    if(kreg == 0xFF) return false;
    u8 le = ra_alloc(&ctx->ra, em);
    if(le == 0xFF) { ra_free(&ctx->ra, kreg); return false; }
    emit3(em, unsigned_cmp ? OP_CLEU : OP_CLE, le, val, kreg);
    ra_free(&ctx->ra, kreg);
    emit_jmp(em, OP_JT, le, l_lo);
    ra_free(&ctx->ra, le);
    if(!emit_match_bst(ctx, val, pairs, mid + 1, hi,
                       body_lbl, default_lbl, unsigned_cmp)) return false;
    bind_label(em, l_lo);
    return emit_match_bst(ctx, val, pairs, lo, mid,
                          body_lbl, default_lbl, unsigned_cmp);
}

/* Try to emit `match` as a binary decision tree. Returns:
 *    1 - emitted (caller binds end_lbl and is done)
 *    0 - hard emission error (caller must bail)
 *   -1 - not eligible; no code emitted, caller runs the linear path   */
static int emit_match_tree(EmCtx *ctx, Node *n, u8 val, int end_lbl) {
    IREmitter *em = ctx->em;

    /* Integer/bool/pointer subjects only. f32 needs FCEQ semantics and
     * aggregates can't be CEQ'd at all; both keep the linear path. */
    bool unsigned_cmp = false;
    if(n->cond->type) {
        switch(n->cond->type->kind) {
            case TY_BOOL: case TY_I8: case TY_I16: case TY_I32:
                break;
            case TY_U8: case TY_U16: case TY_U32: case TY_PTR:
                unsigned_cmp = true;
                break;
            default:
                return -1;
        }
    }

    /* Collect (value -> arm) pairs from constant patterns, stopping at
     * the first wildcard - arms after it are unreachable in both
     * lowerings, so their patterns must not enter the map. */
    int narms = n->nchildren;
    int wild_arm = -1;
    int capacity = 0;
    for(int i = 0; i < narms; i++) {
        Node *arm = n->children[i];
        if(arm->bval) { wild_arm = i; break; }
        capacity += 1 + arm->nchildren;
    }
    if(capacity < MATCH_TREE_MIN) return -1;

    MatchPair *pairs = (MatchPair *)malloc((size_t)capacity * sizeof(*pairs));
    if(!pairs) {
        snprintf(em->err, sizeof(em->err), "out of memory for match lowering");
        return 0;
    }
    int npairs = 0;
    for(int i = 0; i < narms; i++) {
        Node *arm = n->children[i];
        if(arm->bval) break;
        for(int j = -1; j < arm->nchildren; j++) {
            Node *pnode = (j < 0) ? arm->cond : arm->children[j];
            i32 v;
            if(!relic_fold_const(em->tc, pnode, &v)) {
                free(pairs);
                return -1;   /* non-constant pattern: linear path */
            }
            pairs[npairs].v   = v;
            pairs[npairs].arm = i;
            npairs++;
        }
    }
    if(npairs < MATCH_TREE_MIN) { free(pairs); return -1; }

    /* Sort by (value, arm index); first-listed arm wins on duplicates. */
    qsort(pairs, (size_t)npairs, sizeof(*pairs), match_pair_cmp);
    int m = 0;
    for(int i = 0; i < npairs; i++) {
        if(m > 0 && pairs[m - 1].v == pairs[i].v) continue;
        pairs[m++] = pairs[i];
    }
    npairs = m;

    int *body_lbl = (int *)malloc((size_t)narms * sizeof(int));
    if(!body_lbl) {
        free(pairs);
        snprintf(em->err, sizeof(em->err), "out of memory for match lowering");
        return 0;
    }
    for(int i = 0; i < narms; i++) body_lbl[i] = -1;
    for(int i = 0; i < npairs; i++)
        if(body_lbl[pairs[i].arm] < 0) body_lbl[pairs[i].arm] = new_label(em);

    int wild_lbl    = (wild_arm >= 0) ? new_label(em) : -1;
    int default_lbl = (wild_arm >= 0) ? wild_lbl : end_lbl;

    if(!emit_match_bst(ctx, val, pairs, 0, npairs - 1,
                       body_lbl, default_lbl, unsigned_cmp)) {
        free(pairs); free(body_lbl);
        return 0;
    }

    /* Arm bodies in source order. Arms with no surviving pattern (all
     * duplicates folded away, or unreachable after the wildcard) have no
     * label and are skipped - dead in both schemes. */
    for(int i = 0; i < narms; i++) {
        Node *arm = n->children[i];
        if(arm->bval) break;
        if(body_lbl[i] < 0) continue;
        bind_label(em, body_lbl[i]);
        if(!emit_stmt(ctx, arm->left)) {
            free(pairs); free(body_lbl);
            return 0;
        }
        emit_jmp(em, OP_JMP, 0, end_lbl);
    }
    if(wild_arm >= 0) {
        bind_label(em, wild_lbl);
        if(!emit_stmt(ctx, n->children[wild_arm]->left)) {
            free(pairs); free(body_lbl);
            return 0;
        }
        /* falls through to end_lbl */
    }

    free(pairs);
    free(body_lbl);
    return 1;
}

/* -------------------------------------------------------------------------
 * Emit statement
 * ---------------------------------------------------------------------- */
static bool emit_stmt(EmCtx *ctx, Node *n) {
    IREmitter *em = ctx->em;
    if(n && n->line > 0) { em->cur_line = n->line; em->cur_file_id = n->file_id; }

    switch(n->kind) {

        case ND_BLOCK: {
            /* Locals declared inside the block die with it - the
             * typechecker pops its scope at the same point. */
            int mark = ctx->frame.nlocals;
            for(int i=0;i<n->nchildren;i++)
                if(!emit_stmt(ctx, n->children[i])) return false;
            frame_close_scope(&ctx->frame, mark);
            return true;
        }

        case ND_STMT_SEQ:
            /* Scopeless sequence from array-literal desugaring (no
             * scope close - the typechecker pushed none either). */
            for(int i=0;i<n->nchildren;i++)
                if(!emit_stmt(ctx, n->children[i])) return false;
            return true;

        case ND_VAR_DECL: {
            int sz = emit_type_size(em->tc, n->type);
            int off = frame_alloc_sz(&ctx->frame, n->sval, n->type, sz);
            if(n->left) {
                if(is_str_array_init(n)) {
                    /* `var buf: [N]u8 = "..."` syntax sugar: copy
                     * up to N bytes (string + NUL) from the IR string
                     * table into the local. */
                    int str_off = 0, str_len = 0;
                    if(!intern_node_string(em, n->left, &str_off, &str_len)) {
                        snprintf(em->err, sizeof(em->err),
                                 "line %d: failed to intern string literal", n->line);
                        return false;
                    }
                    int n_arr = (int)n->type->count;
                    int copy_n = (str_len + 1 < n_arr) ? str_len + 1 : n_arr;
                    u8 src = ra_alloc(&ctx->ra, em);
                    if(src == 0xFF) return false;
                    i32 addr = (i32)em->ir->globals_size + str_off;
                    emit_imm(em, OP_LDC32, src, 0, 0);
                    emit(em, (u32)addr);
                    emit_memcpy(ctx, REG_FP, off, src, 0, copy_n);
                    ra_free(&ctx->ra, src);
                } else if(is_aggregate(n->type)) {
                    /* If RHS is a struct-returning call, have it write
                     * directly into the new local — no intermediate copy. */
                    if(n->left->kind == ND_CALL && is_aggregate(n->left->type)) {
                        u8 r = emit_call_with_dest(ctx, n->left, REG_FP, off);
                        if(r==0xFF) return false;
                        ra_free(&ctx->ra, r);
                    } else {
                        u8 src = emit_struct_addr(ctx, n->left);
                        if(src==0xFF) return false;
                        emit_memcpy(ctx, REG_FP, off, src, 0, sz);
                        ra_free(&ctx->ra, src);
                    }
                } else if(n->type && n->type->kind == TY_F64) {
                    u8 d = emit_expr_d(ctx, n->left);
                    if(d==0xFF) return false;
                    emit_dstore_off(ctx, REG_FP, d, off);
                    rd_free(&ctx->ra, d);
                } else {
                    u8 r = emit_expr(ctx, n->left);
                    if(r==0xFF) return false;
                    /* Store at the declared width so a later narrow load
                     * reads the same bytes on big-endian hosts. */
                    u8 ld_op, st_op, sx_op;
                    pick_mem_ops(n->type, &ld_op, &st_op, &sx_op);
                    emit_store_off(ctx, st_op, REG_FP, r, off);
                    ra_free(&ctx->ra, r);
                }
            }
            return true;
        }

        case ND_CONST_DECL: {
            int sz = emit_type_size(em->tc, n->type);
            int off = frame_alloc_sz(&ctx->frame, n->sval, n->type, sz);
            if(n->left) {
                if(is_aggregate(n->type)) {
                    /* Mirror ND_VAR_DECL aggregate handling: copy the RHS
                     * struct into the new local, or have a struct-returning
                     * call write directly into it. */
                    if(n->left->kind == ND_CALL && is_aggregate(n->left->type)) {
                        u8 r = emit_call_with_dest(ctx, n->left, REG_FP, off);
                        if(r==0xFF) return false;
                        ra_free(&ctx->ra, r);
                    } else {
                        u8 src = emit_struct_addr(ctx, n->left);
                        if(src==0xFF) return false;
                        emit_memcpy(ctx, REG_FP, off, src, 0, sz);
                        ra_free(&ctx->ra, src);
                    }
                } else if(n->type && n->type->kind == TY_F64) {
                    u8 d = emit_expr_d(ctx, n->left);
                    if(d==0xFF) return false;
                    emit_dstore_off(ctx, REG_FP, d, off);
                    rd_free(&ctx->ra, d);
                } else {
                    u8 r = emit_expr(ctx, n->left);
                    if(r==0xFF) return false;
                    u8 ld_op, st_op, sx_op;
                    pick_mem_ops(n->type, &ld_op, &st_op, &sx_op);
                    emit_store_off(ctx, st_op, REG_FP, r, off);
                    ra_free(&ctx->ra, r);
                }
            }
            return true;
        }

        case ND_ASSIGN: {
            /* Aggregate assignment: memcpy RHS struct to LHS slot, or have
             * a struct-returning call write directly into the LHS. */
            if(n->op == TK_ASSIGN && n->left->type && is_aggregate(n->left->type)) {
                int sz = emit_type_size(em->tc, n->left->type);
                int lhs_off = 0;
                u8 lhs_base = emit_lvalue_addr(ctx, n->left, &lhs_off);
                if(lhs_base == 0xFF) return false;
                if(n->right->kind == ND_CALL && is_aggregate(n->right->type)) {
                    u8 r = emit_call_with_dest(ctx, n->right, lhs_base, lhs_off);
                    if(r==0xFF) return false;
                    ra_free(&ctx->ra, r);
                } else {
                    u8 src = emit_struct_addr(ctx, n->right);
                    if(src==0xFF) return false;
                    emit_memcpy(ctx, lhs_base, lhs_off, src, 0, sz);
                    ra_free(&ctx->ra, src);
                }
                if(lhs_base != REG_FP && lhs_base != REG_ZERO)
                    ra_free(&ctx->ra, lhs_base);
                return true;
            }
            /* f64 assignment: RHS arrives in a d register and stores via
             * OP_DST. Compound ops load the lhs through emit_expr_d too
             * (its lvalue kinds all support loads). */
            if(n->right->type && n->right->type->kind == TY_F64) {
                u8 dr = emit_expr_d(ctx, n->right);
                if(dr==0xFF) return false;
                if(n->op != TK_ASSIGN) {
                    u8 dl = emit_expr_d(ctx, n->left);
                    if(dl==0xFF) { rd_free(&ctx->ra, dr); return false; }
                    u8 dop = 0;
                    switch(n->op) {
                        case TK_PLUS_ASSIGN:  dop = OP_DADD; break;
                        case TK_MINUS_ASSIGN: dop = OP_DSUB; break;
                        case TK_STAR_ASSIGN:  dop = OP_DMUL; break;
                        case TK_SLASH_ASSIGN: dop = OP_DDIV; break;
                        default: break;
                    }
                    if(!dop) {
                        snprintf(em->err, sizeof(em->err),
                                 "f64 compound-assign operator not supported");
                        rd_free(&ctx->ra, dl); rd_free(&ctx->ra, dr);
                        return false;
                    }
                    u8 dd = rd_alloc(&ctx->ra, em);
                    if(dd==0xFF) { rd_free(&ctx->ra, dl); rd_free(&ctx->ra, dr); return false; }
                    emit3(em, dop, dd, dl, dr);
                    rd_free(&ctx->ra, dl);
                    rd_free(&ctx->ra, dr);
                    dr = dd;
                }
                if(!emit_store_d(ctx, n->left, dr)) { rd_free(&ctx->ra, dr); return false; }
                rd_free(&ctx->ra, dr);
                return true;
            }
            u8 rhs = emit_expr(ctx, n->right);
            if(rhs==0xFF) return false;
            if(n->op != TK_ASSIGN) {
                /* compound assignment: load lhs, apply op, store */
                u8 lhs = emit_expr(ctx, n->left);
                if(lhs==0xFF) { ra_free(&ctx->ra, rhs); return false; }
                u8 r = ra_alloc(&ctx->ra, em);
                switch(n->op) {
                    case TK_PLUS_ASSIGN:  emit3(em,OP_ADD,r,lhs,rhs); break;
                    case TK_MINUS_ASSIGN: emit3(em,OP_SUB,r,lhs,rhs); break;
                    case TK_STAR_ASSIGN:  emit3(em,OP_MUL,r,lhs,rhs); break;
                    case TK_SLASH_ASSIGN: emit3(em,OP_DIV,r,lhs,rhs); break;
                    case TK_PERCENT_ASSIGN: emit3(em,OP_MOD,r,lhs,rhs); break;
                    case TK_AMP_ASSIGN:   emit3(em,OP_AND,r,lhs,rhs); break;
                    case TK_PIPE_ASSIGN:  emit3(em,OP_OR, r,lhs,rhs); break;
                    case TK_CARET_ASSIGN: emit3(em,OP_XOR,r,lhs,rhs); break;
                    case TK_LSHIFT_ASSIGN:emit3(em,OP_SHL,r,lhs,rhs); break;
                    case TK_RSHIFT_ASSIGN:emit3(em,OP_SHR,r,lhs,rhs); break;
                    default: break;
                }
                ra_free(&ctx->ra, lhs);
                ra_free(&ctx->ra, rhs);
                rhs = r;
            }
            if(!emit_store(ctx, n->left, rhs)) { ra_free(&ctx->ra, rhs); return false; }
            ra_free(&ctx->ra, rhs);
            return true;
        }

        case ND_EXPR_STMT: {
            if(n->left->type && n->left->type->kind == TY_F64) {
                u8 d = emit_expr_d(ctx, n->left);
                if(d==0xFF) return false;
                rd_free(&ctx->ra, d);
                return true;
            }
            u8 r = emit_expr(ctx, n->left);
            if(r==0xFF) return false;
            ra_free(&ctx->ra, r);
            return true;
        }

        case ND_IF: {
            u8 cond = emit_cond_reg(ctx, n->cond);
            if(cond==0xFF) return false;
            int else_lbl = new_label(em);
            int end_lbl  = new_label(em);
            emit_jmp(em, OP_JF, cond, else_lbl);
            ra_free(&ctx->ra, cond);
            if(!emit_stmt(ctx, n->left)) return false;
            if(n->right) emit_jmp(em, OP_JMP, 0, end_lbl);
            bind_label(em, else_lbl);
            if(n->right) {
                if(!emit_stmt(ctx, n->right)) return false;
                bind_label(em, end_lbl);
            }
            return true;
        }

        case ND_WHILE: {
            int top_lbl  = new_label(em);
            int end_lbl  = new_label(em);
            em->break_label[em->loop_depth]    = end_lbl;
            em->continue_label[em->loop_depth] = top_lbl;
            em->loop_depth++;
            bind_label(em, top_lbl);
            u8 cond = emit_cond_reg(ctx, n->cond);
            if(cond==0xFF) return false;
            emit_jmp(em, OP_JF, cond, end_lbl);
            ra_free(&ctx->ra, cond);
            if(!emit_stmt(ctx, n->left)) return false;
            emit_jmp(em, OP_JMP, 0, top_lbl);
            bind_label(em, end_lbl);
            em->loop_depth--;
            return true;
        }

        case ND_FOR: {
            /* for i in lo..hi { } */
            Node *range = n->cond;
            /* `for j in ..` over a name that is already a local reuses
             * that variable's slot AND its width, so the value is still
             * there (and readable) after the loop - which is what
             * tests/language/BUG5 asserts. The typechecker agrees: it
             * reuses an enclosing declaration's type rather than
             * shadowing it with an i16 (see ND_FOR in typecheck.c).
             * Width matters because the loop stores and the later read
             * must touch the same bytes: an i16 store into an i32 slot
             * lands in the half a 32-bit load ignores on big-endian
             * hosts (little-endian happens to read it back).
             *
             * With no enclosing declaration the induction variable is
             * its own i16 local, so the body's LD16 and the loop's ST16
             * agree; do not "simplify" either to 32-bit. */
            /* The for-var (and anything the body declares) is scoped to the
             * loop; a dead same-named local from an earlier closed block must
             * NOT be resurrected here - its width could differ from the i16
             * the typechecker gave this var, and the loop's ST32 would then
             * write the half the body's LD16 can't see on big-endian hosts. */
            int fmark = ctx->frame.nlocals;
            Type *outer_t = frame_find_type(&ctx->frame, n->sval);
            /* n->type is the typechecker's resolved induction type: a live
             * outer local's type, a visible GLOBAL's type (the for-var is a
             * loop-scoped shadow of the same width, not the global itself),
             * or i16 when the name was undeclared. Width must match the
             * body's `j` accesses, which are emitted at this type - a 2-byte
             * slot read as LD32 returns `j<<16|junk` on big-endian hosts. */
            Type *vt = outer_t ? outer_t : n->type;
            Type i16t;
            if(!vt) { memset(&i16t, 0, sizeof(i16t)); i16t.kind = TY_I16; vt = &i16t; }
            int var_off = (outer_t ? frame_find(&ctx->frame, n->sval)
                                   : frame_alloc(&ctx->frame, n->sval, vt));
            u8 i_ld, i_st, i_sx;
            pick_mem_ops(vt, &i_ld, &i_st, &i_sx);
            /* init: i = lo */
            u8 lo = emit_expr(ctx, range->left);
            if(lo==0xFF) return false;
            emit_store_off(ctx, i_st, REG_FP, lo, var_off);
            ra_free(&ctx->ra, lo);

            int top_lbl  = new_label(em);
            int cont_lbl = new_label(em);   /* `continue` lands here -- *before* the increment, so the
                                             * induction variable still advances on each iteration.
                                             * Landing on top_lbl would skip the increment and
                                             * infinite-loop. */
            int end_lbl  = new_label(em);
            em->break_label[em->loop_depth]    = end_lbl;
            em->continue_label[em->loop_depth] = cont_lbl;
            em->loop_depth++;

            bind_label(em, top_lbl);
            /* cond: i < hi */
            u8 i_r  = ra_alloc(&ctx->ra, em);
            emit_load_off(ctx, i_ld, i_r, REG_FP, var_off);
            if(i_sx) emit3(em, i_sx, i_r, i_r, 0);
            u8 hi = emit_expr(ctx, range->right);
            if(hi==0xFF) return false;
            u8 cmp = ra_alloc(&ctx->ra, em);
            emit3(em, OP_CLT, cmp, i_r, hi);
            emit_jmp(em, OP_JF, cmp, end_lbl);
            ra_free(&ctx->ra, cmp); ra_free(&ctx->ra, hi); ra_free(&ctx->ra, i_r);

            if(!emit_stmt(ctx, n->left)) return false;

            /* i++  (continue target so a `continue` still advances i). */
            bind_label(em, cont_lbl);
            u8 iv = ra_alloc(&ctx->ra, em);
            emit_load_off(ctx, i_ld, iv, REG_FP, var_off);
            if(i_sx) emit3(em, i_sx, iv, iv, 0);
            emit_imm(em, OP_ADDI, iv, iv, 1);
            emit_store_off(ctx, i_st, REG_FP, iv, var_off);
            ra_free(&ctx->ra, iv);

            emit_jmp(em, OP_JMP, 0, top_lbl);
            bind_label(em, end_lbl);
            frame_close_scope(&ctx->frame, fmark);
            em->loop_depth--;
            return true;
        }

        case ND_MATCH: {
            u8 val = emit_expr(ctx, n->cond);
            if(val==0xFF) return false;
            int end_lbl = new_label(em);
            /* Constant-pattern matches with enough arms emit as a binary
             * decision tree instead of the linear chain below. */
            int mt = emit_match_tree(ctx, n, val, end_lbl);
            if(mt < 0) {
            for(int i=0;i<n->nchildren;i++) {
                Node *arm = n->children[i];
                if(arm->bval) {
                    /* wildcard _ : always execute */
                    if(!emit_stmt(ctx, arm->left)) { ra_free(&ctx->ra, val); return false; }
                    emit_jmp(em, OP_JMP, 0, end_lbl);
                } else {
                    /* One or more patterns (`A | B | C => body`). Each
                     * comparison that succeeds jumps straight to the body;
                     * only if every one fails do we fall through to the
                     * next arm. */
                    int body_lbl = (arm->nchildren > 0) ? new_label(em) : -1;
                    int next_lbl = new_label(em);
                    for(int j = -1; j < arm->nchildren; j++) {
                        Node *pnode = (j < 0) ? arm->cond : arm->children[j];
                        u8 pat = emit_expr(ctx, pnode);
                        if(pat==0xFF) { ra_free(&ctx->ra, val); return false; }
                        u8 eq = ra_alloc(&ctx->ra, em);
                        emit3(em, OP_CEQ, eq, val, pat);
                        ra_free(&ctx->ra, pat);
                        if(j == arm->nchildren - 1) {
                            /* last alternative: failure means next arm */
                            emit_jmp(em, OP_JF, eq, next_lbl);
                        } else {
                            /* earlier alternative: success means run body */
                            emit_jmp(em, OP_JT, eq, body_lbl);
                        }
                        ra_free(&ctx->ra, eq);
                    }
                    if(body_lbl >= 0) bind_label(em, body_lbl);
                    if(!emit_stmt(ctx, arm->left)) { ra_free(&ctx->ra, val); return false; }
                    emit_jmp(em, OP_JMP, 0, end_lbl);
                    bind_label(em, next_lbl);
                }
            }
            } else if(mt == 0) {
                ra_free(&ctx->ra, val);
                return false;
            }
            bind_label(em, end_lbl);
            ra_free(&ctx->ra, val);
            return true;
        }

        case ND_RETURN: {
            if(n->left) {
                if(ctx->has_retbuf && is_aggregate(n->left->type)) {
                    /* Aggregate return: copy source struct into the
                     * caller-provided destination buffer (its address was
                     * stashed in ctx->retbuf_off at function entry). */
                    int sz = emit_type_size(em->tc, n->left->type);
                    u8 src = emit_struct_addr(ctx, n->left);
                    if(src==0xFF) return false;
                    u8 dst = ra_alloc(&ctx->ra, em);
                    if(dst==0xFF) { ra_free(&ctx->ra, src); return false; }
                    emit_load_off(ctx, OP_LD32, dst, REG_FP, ctx->retbuf_off);
                    emit_memcpy(ctx, dst, 0, src, 0, sz);
                    /* By convention, r1 holds the dest address on return. */
                    emit3(em, OP_MOV, 1, dst, 0);
                    ra_free(&ctx->ra, src);
                    ra_free(&ctx->ra, dst);
                } else if(n->left->type && n->left->type->kind == TY_F64) {
                    u8 dr = emit_expr_d(ctx, n->left);
                    if(dr==0xFF) return false;
                    emit3(em, OP_DMOV, 1, dr, 0); /* return value in d1 */
                    rd_free(&ctx->ra, dr);
                } else {
                    u8 r = emit_expr(ctx, n->left);
                    if(r==0xFF) return false;
                    emit3(em, OP_MOV, 1, r, 0); /* return value in r1 */
                    ra_free(&ctx->ra, r);
                }
            }
            emit(em, IR_ENCODE(OP_LEAVE, 0,0,0,0));
            emit(em, IR_ENCODE(OP_RET,   0,0,0,0));
            return true;
        }

        case ND_BREAK:
            if(em->loop_depth == 0) {
                snprintf(em->err, sizeof(em->err), "line %d: break outside loop", n->line);
                return false;
            }
            emit_jmp(em, OP_JMP, 0, em->break_label[em->loop_depth-1]);
            return true;

        case ND_CONTINUE:
            if(em->loop_depth == 0) {
                snprintf(em->err, sizeof(em->err), "line %d: continue outside loop", n->line);
                return false;
            }
            emit_jmp(em, OP_JMP, 0, em->continue_label[em->loop_depth-1]);
            return true;

        default:
            snprintf(em->err, sizeof(em->err), "line %d: unhandled stmt node %d", n->line, n->kind);
            return false;
    }
}

/* -------------------------------------------------------------------------
 * Emit a function declaration
 * ---------------------------------------------------------------------- */
static bool emit_fn(IREmitter *em, Node *n, SymTable *syms, Node *root) {
    Symbol *sym = sym_lookup_global(syms, n->sval);
    if(!sym) sym = sym_lookup(syms, n->sval);
    /* fn_id was pre-assigned in relic_emit's pass 1 so that forward
     * references (a function calling one declared later) resolve to the
     * correct id. Fall back to sequential assignment only if missing. */
    int fn_id = (sym && sym->fn_id >= 0) ? sym->fn_id : em->ir->nfuncs++;
    if(sym) sym->fn_id = fn_id;
    if(fn_id < 0 || fn_id >= RELIC_MAX_FUNCS) {
        fprintf(stderr, "tigryx: too many functions (limit %d) while emitting '%s'.\n"
                        "  Raise RELIC_MAX_FUNCS in include/tigryx.h and rebuild the toolchain.\n",
                RELIC_MAX_FUNCS, n->sval);
        return false;
    }
    if(!relic_ir_ensure_aux(em->ir)) return false;
    IRFNOFF(em->ir, fn_id) = cur_pc(em);
    /* fn_names is a far array on ia16 - snprintf to a near buffer first. */
    { char nm[RELIC_MAX_IDENT];
      snprintf(nm, RELIC_MAX_IDENT, "%s", n->sval);
      relic_far_write((u8 RELIC_FAR *)em->ir->fn_names,
                      (u32)fn_id * RELIC_MAX_IDENT, nm, (u32)strlen(nm) + 1); }
    /* Update cur_line so the function-prologue OP_ENTER carries this fn's
     * declaration line, not the previous function's last expression. */
    if(n->line > 0) { em->cur_line = n->line; em->cur_file_id = n->file_id; }

    EmCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.em   = em;
    ctx.syms = syms;

    /* Determine return-by-pointer convention. n->type holds the return
     * type for ND_FN_DECL (set by parser). When returning a struct, the
     * caller passes a hidden destination pointer in r1 and the user-
     * visible arguments shift to r2..rN. */
    bool ret_struct = is_aggregate(n->type);
    int  arg_base   = ret_struct ? 2 : 1;

    /* f64 params/returns use the d-bank calling convention:
     * parameter i travels in d(arg_base+i) when f64-typed,
     * the return value comes back in d1 - the positional mirror of the
     * r-bank convention, so fn_argmask and the spill-first staging logic
     * apply unchanged. */
    if(ret_struct) {
        /* Reserve a 4-byte slot to spill the hidden return pointer so it
         * survives across nested calls within this function. */
        ctx.retbuf_off = frame_alloc_sz(&ctx.frame, NULL, NULL, 4);
        ctx.has_retbuf = true;
    }

    /* Allocate param slots in frame first, sized correctly for aggregates. */
    for(int i=0;i<n->nchildren;i++) {
        Node *param = n->children[i];
        int psz = emit_type_size(em->tc, param->type);
        frame_alloc_sz(&ctx.frame, param->sval, param->type, psz);
    }

    /* Emit ENTER as a placeholder - its immediate (frame size in words) is
     * patched once the body has been emitted and the full frame_bytes is
     * known. We can't know the size up front because var-decls and
     * for-loop induction variables inside the body also call frame_alloc. */
    int enter_pc = em->ir->len;
    emit(em, IR_ENCODE(OP_ENTER, 0, 0, 0, 0));

    /* Spill hidden retbuf pointer (r1) before any other use of r1..rN. */
    if(ret_struct) {
        emit_store_off(&ctx, OP_ST32, REG_FP, 1, ctx.retbuf_off);
    }

    /* The first `max_reg_params` args arrive in r(arg_base)..r(REG_LAST);
     * any extras live on the VM stack at positive FP offsets, written by
     * the caller via OP_PUSH (see emit_call_with_dest). */
    int max_reg_params = REG_LAST - arg_base + 1;
    int nreg_params = n->nchildren < max_reg_params
                    ? n->nchildren : max_reg_params;

    /* Record the exact register-arg mask for native backends: the hidden
     * dest pointer (r1) plus each staged reg arg r(arg_base)..r(arg_base+
     * nreg_params-1). Stack-passed params arrive via FP+8, not regs. */
    {
        u16 amask = ret_struct ? (u16)(1u << 1) : 0;
        for(int i=0;i<nreg_params;i++) amask |= (u16)(1u << (arg_base + i));
        IRFNAMASK(em->ir, fn_id) = amask;
    }

    /* Reserve arg registers so emit_memcpy's scratch alloc doesn't
     * collide with values still live in r(arg_base)..r(arg_base+N-1). */
    int max_used_reg = arg_base + nreg_params - 1;
    if(max_used_reg > REG_LAST) max_used_reg = REG_LAST;
    for(int i=1; i<=max_used_reg; i++) ctx.ra.used[i] = true;

    /* Pass 1: store register-passed params into their frame slots
     * (negative offsets from FP). Aggregate params arrive as pointers and
     * are memcpy'd into the callee's local slot to preserve by-value
     * semantics. The reg-arg pin (above) keeps emit_memcpy's scratch
     * allocator from clobbering them. */
    for(int i=0;i<nreg_params;i++) {
        Node *param = n->children[i];
        int off = frame_find(&ctx.frame, param->sval);
        u8 src = (u8)(arg_base + i);
        if(is_aggregate(param->type)) {
            int psz = emit_type_size(em->tc, param->type);
            emit_memcpy(&ctx, REG_FP, off, src, 0, psz);
        } else if(param->type && param->type->kind == TY_F64) {
            emit_dstore_off(&ctx, REG_FP, src, off);
        } else {
            /* Store at the declared width, not always 32 bits: on a
             * big-endian host a 4-byte store followed by a 16-bit load
             * reads the wrong half of the word. */
            u8 ld_op, st_op, sx_op;
            pick_mem_ops(param->type, &ld_op, &st_op, &sx_op);
            emit_store_off(&ctx, st_op, REG_FP, src, off);
        }
    }

    /* Release reg-arg pin; body (and pass 2 below) can now alloc freely. */
    for(int i=1; i<=max_used_reg; i++) ctx.ra.used[i] = false;

    /* Pass 2: load stack-passed params (i >= nreg_params) from positive FP
     * offsets into their local frame slots.
     *   FP+0 = saved FP, FP+4 = return PC.
     *   First stack arg lives at FP+8 (lowest pushed slot, see caller).
     *   arg j (0-based among stack args) lives at FP + 8 + 4*j. */
    for(int i=nreg_params;i<n->nchildren;i++) {
        Node *param = n->children[i];
        int off = frame_find(&ctx.frame, param->sval);
        int stk_off = 8 + 4 * (i - nreg_params);
        if(param->type && param->type->kind == TY_F64) {
            /* f64 stack params need an 8-byte slot layout on both sides;
             * the caller already refuses to emit one, so this is a guard. */
            snprintf(em->err, sizeof(em->err),
                     "f64 parameter '%s' beyond register-passed limit not "
                     "yet implemented (fn '%s')", param->sval, n->sval);
            return false;
        }
        u8 tmp = ra_alloc(&ctx.ra, em);
        if(tmp == 0xFF) return false;
        emit_load_off(&ctx, OP_LD32, tmp, REG_FP, stk_off);
        if(is_aggregate(param->type)) {
            int psz = emit_type_size(em->tc, param->type);
            emit_memcpy(&ctx, REG_FP, off, tmp, 0, psz);
        } else {
            u8 ld_op, st_op, sx_op;
            pick_mem_ops(param->type, &ld_op, &st_op, &sx_op);
            emit_store_off(&ctx, st_op, REG_FP, tmp, off);
        }
        ra_free(&ctx.ra, tmp);
    }

    /* If this is the main function, emit global variable initializers.
     * This covers module-level `var`s, aggregate (struct) consts, and scalar
     * consts whose initializer did not constant-fold (has_const_val == false),
     * which all have global storage allocated by Pass 1b. Scalar consts that
     * DID fold are inlined as immediates and have no storage, so they are
     * skipped here. */
    if(sym && strcmp(sym->name, "main") == 0 && root) {
        for(int i=0;i<root->nchildren;i++) {
            Node *d = root->children[i];
            bool is_var   = (d->kind == ND_VAR_DECL);
            bool is_const = (d->kind == ND_CONST_DECL);
            if(!(is_var || is_const) || !d->left) continue;
            Symbol *gvar_sym = sym_lookup(syms, d->sval);
            if(!gvar_sym) continue;
            bool is_aggr = (d->type && d->type->kind == TY_STRUCT);
            /* Skip scalar consts that were constant-folded into const_val:
             * they have no global slot and are inlined as immediates. */
            if(is_const && !is_aggr && gvar_sym->has_const_val) continue;

            i32 addr = gvar_sym->offset;

            if(is_var && is_str_array_init(d)) {
                /* Global `var buf: [N]u8 = "..."`: copy bytes from the
                 * string table to the global. */
                int str_off = 0, str_len = 0;
                if(!intern_node_string(em, d->left, &str_off, &str_len)) {
                    snprintf(em->err, sizeof(em->err),
                             "line %d: failed to intern string literal", d->line);
                    return false;
                }
                int n_arr = (int)d->type->count;
                int copy_n = (str_len + 1 < n_arr) ? str_len + 1 : n_arr;
                u8 dst = ra_alloc(&ctx.ra, em);
                if(dst == 0xFF) return false;
                if(addr >= -2048 && addr <= 2047)
                    emit_imm(em, OP_LDC, dst, 0, addr);
                else { emit_imm(em, OP_LDC32, dst, 0, 0); emit(em, (u32)addr); }
                u8 src = ra_alloc(&ctx.ra, em);
                if(src == 0xFF) { ra_free(&ctx.ra, dst); return false; }
                i32 saddr = (i32)em->ir->globals_size + str_off;
                emit_imm(em, OP_LDC32, src, 0, 0);
                emit(em, (u32)saddr);
                emit_memcpy(&ctx, dst, 0, src, 0, copy_n);
                ra_free(&ctx.ra, src);
                ra_free(&ctx.ra, dst);
            } else if(is_aggr) {
                /* Aggregate global init: load the absolute destination
                 * address, then either have a struct-returning call write
                 * directly into it, or memcpy from the source struct. */
                int sz = emit_type_size(em->tc, d->type);
                u8 dst = ra_alloc(&ctx.ra, em);
                if(dst == 0xFF) return false;
                if(addr >= -2048 && addr <= 2047)
                    emit_imm(em, OP_LDC, dst, 0, addr);
                else { emit_imm(em, OP_LDC32, dst, 0, 0); emit(em, (u32)addr); }
                if(d->left->kind == ND_CALL && is_aggregate(d->left->type)) {
                    u8 r = emit_call_with_dest(&ctx, d->left, dst, 0);
                    if(r == 0xFF) { ra_free(&ctx.ra, dst); return false; }
                    ra_free(&ctx.ra, r);
                } else {
                    u8 src = emit_struct_addr(&ctx, d->left);
                    if(src == 0xFF) { ra_free(&ctx.ra, dst); return false; }
                    emit_memcpy(&ctx, dst, 0, src, 0, sz);
                    ra_free(&ctx.ra, src);
                }
                ra_free(&ctx.ra, dst);
            } else if(d->left->type && d->left->type->kind == TY_F64) {
                /* f64 global init: evaluate in the d bank, store 8 bytes. */
                u8 dv = emit_expr_d(&ctx, d->left);
                if(dv == 0xFF) return false;
                u8 ar = ra_alloc(&ctx.ra, em);
                if(ar == 0xFF) { rd_free(&ctx.ra, dv); return false; }
                if(addr >= -2048 && addr <= 2047)
                    emit_imm(em, OP_LDC, ar, 0, addr);
                else { emit_imm(em, OP_LDC32, ar, 0, 0); emit(em, (u32)addr); }
                emit_dstore_off(&ctx, ar, dv, 0);
                ra_free(&ctx.ra, ar);
                rd_free(&ctx.ra, dv);
            } else {
                /* Scalar global init (var or non-folded const): evaluate the
                 * initializer expression at runtime and store to the global. */
                u8 r = emit_expr(&ctx, d->left);
                if(r == 0xFF) return false;

                u8 ar = ra_alloc(&ctx.ra, em);
                if(ar == 0xFF) { ra_free(&ctx.ra, r); return false; }

                if(addr >= -2048 && addr <= 2047)
                    emit_imm(em, OP_LDC, ar, 0, addr);
                else { emit_imm(em, OP_LDC32, ar, 0, 0); emit(em, (u32)addr); }

                /* Store at the declared width: narrow globals are read
                 * back with LD8/LD16, which hit the wrong half of a 32-bit
                 * word on big-endian hosts. */
                u8 ld_op, st_op, sx_op;
                pick_mem_ops(d->type, &ld_op, &st_op, &sx_op);
                emit_imm(em, st_op, ar, r, 0);
                ra_free(&ctx.ra, ar);
                ra_free(&ctx.ra, r);
            }
        }
    }

    /* Emit body (may grow ctx.frame.frame_bytes via additional frame_alloc) */
    if(!emit_stmt(&ctx, n->left)) return false;

    /* Patch ENTER with the final frame size. ENTER carries a 20-bit
     * unsigned frame size in bits [19:0] - the dst/srca/srcb fields are
     * unused, matching the imm20 convention documented for OP_LDC and the
     * jump instructions in docs/opcodes.md. This raises the per-function
     * frame limit from 2047 words (8 KB) to 1048575 words (4 MB), far
     * beyond the VM stack; anything larger is a compile error rather than
     * a silently truncated allocation. */
    i32 frame_words = (ctx.frame.frame_bytes + 3) / 4;
    if(frame_words < 0 || frame_words > 0xFFFFF) {
        if(!em->err[0]) {
            snprintf(em->err, sizeof(em->err),
                     "line %d: function frame needs %d bytes (max %u supported)",
                     n->line > 0 ? n->line : em->cur_line,
                     ctx.frame.frame_bytes, 0xFFFFFu * 4u);
        }
        frame_words = 0xFFFFF;
    }
    IRCODE(em->ir, enter_pc) = ((u32)OP_ENTER << 24) | ((u32)frame_words & 0xFFFFFu);

    /* Record the function and its named frame slots for the debugger's
     * variables view. Params are the first frame entries (allocated
     * before the body ran, so locals[0..nchildren) are params); the rest
     * are locals. Anonymous scratch slots aren't recorded in
     * frame.locals and so never appear here. */
    dbg_sym(em, 0, n->sval, IRFNOFF(em->ir, fn_id),
            ctx.frame.frame_bytes, n->type);
    for(int li = 0; li < ctx.frame.nlocals; li++) {
        LocalVar *lv = &ctx.frame.locals[li];
        dbg_sym(em, li < n->nchildren ? 3 : 1,
                lv->name, lv->offset, 0, lv->type);
    }

    /* Implicit return at end of void function */
    emit(em, IR_ENCODE(OP_LEAVE, 0,0,0,0));
    emit(em, IR_ENCODE(OP_RET, 0,0,0,0));

    return true;
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */
bool relic_emit(IREmitter *em, Node *root, IRBuffer *ir, SymTable *syms, TypeChecker *tc, Token *tokens) {
    memset(em, 0, sizeof(*em));
    em->ir          = ir;
    em->syms        = syms;
    em->tc          = tc;
    em->tokens      = tokens;
    em->cur_file_id = -1;

    /* Reserve data-segment address 0 as a null sentinel.  String literals
     * are placed at globals_size + str_off, so starting the string table at
     * offset 1 guarantees the smallest possible string address is 1 (when
     * there are no globals).  This prevents the first string literal from
     * being indistinguishable from a null pointer in backend code that
     * guards reads with `if (ptr)`. */
    if(ir->strings_len == 0) {
        IRSTR(ir, 0) = '\0';
        ir->strings_len = 1;
    }

    /* Pass 1: pre-assign fn_id to every function declaration so that
     * forward references (a function body calling another function that
     * is declared later in the source) can encode the correct id. */
    for(int i=0;i<root->nchildren;i++) {
        Node *d = root->children[i];
        if(d->kind == ND_FN_DECL) {
            Symbol *fs = sym_lookup_global(syms, d->sval);
            if(!fs) fs = sym_lookup(syms, d->sval);
            if(fs && fs->fn_id < 0) {
                if(ir->nfuncs >= RELIC_MAX_FUNCS) {
                    fprintf(stderr, "tigryx: too many functions (limit %d) while pre-assigning '%s'.\n"
                                    "  Raise RELIC_MAX_FUNCS in include/tigryx.h and rebuild the toolchain.\n",
                            RELIC_MAX_FUNCS, d->sval);
                    return false;
                }
                fs->fn_id = ir->nfuncs++;
            }
        }
    }

    /* Pass 1b: assign global variable offsets in the data segment.
     * Module-level `var`s always get global storage. Module-level `const`s
     * with aggregate (struct) type also need global storage so their
     * runtime-computed initialiser (e.g. `const C : RGBA = make_rgba(...)`)
     * has a well-defined home that struct field access / by-pointer
     * arg passing can reference. Scalar consts whose initializer was
     * constant-folded (has_const_val) use the const_val immediate fast path
     * and need no storage; scalar consts with a non-foldable runtime
     * initializer (e.g. `const N : i32 = compute_n();`) get a global slot
     * that the main() global-init loop populates. */
    int globals_offset = 0;
    for(int i=0;i<root->nchildren;i++) {
        Node *d = root->children[i];
        bool is_var   = (d->kind == ND_VAR_DECL);
        bool is_const = (d->kind == ND_CONST_DECL);
        bool is_aggr  = (d->type && d->type->kind == TY_STRUCT);
        Symbol *sym   = (is_var || is_const) ? sym_lookup(syms, d->sval) : NULL;
        /* Aggregate consts always need storage; scalar consts need storage
         * only when their initializer did not fold into const_val. */
        bool needs_storage = is_var ||
                             (is_const && is_aggr) ||
                             (is_const && !is_aggr && sym && !sym->has_const_val);
        if(needs_storage) {
            int sz = 4;
            if(d->type) {
                sz = emit_type_size(em->tc, d->type);
                if(sz < 4) sz = 4; /* promote scalars to 4 for alignment */
            }
            if(sym) {
                sym->offset = globals_offset;
                globals_offset += sz;
                globals_offset = (globals_offset + 3) & ~3; /* keep 4-byte aligned */
                /* Record for the debugger's globals view. */
                dbg_sym(em, 2, sym->name, sym->offset, 0, d->type);
            }
        }
    }
    ir->globals_size = globals_offset;

    /* Emit all function declarations */
    for(int i=0;i<root->nchildren;i++) {
        Node *d = root->children[i];
        if(d->kind == ND_FN_DECL) {
            if(!emit_fn(em, d, syms, root)) return false;
        }
    }

    /* Patch all forward jump targets. A target past the 20-bit encoding
     * leaves em->err set - propagate it or the unpatched placeholder
     * (target 0) silently detours control flow at run time. */
    patch_jumps(em);
    if(em->err[0]) return false;
    return true;
}

/* -------------------------------------------------------------------------
 * Disassembler (debug utility)
 * ---------------------------------------------------------------------- */
const char *relic_opcode_name(u8 op) {
    switch(op) {
        case OP_LDC:   return "LDC";    case OP_LDC32: return "LDC32";
        case OP_LD8:   return "LD8";    case OP_LD16:  return "LD16";
        case OP_LD32:  return "LD32";   case OP_ST8:   return "ST8";
        case OP_ST16:  return "ST16";   case OP_ST32:  return "ST32";
        case OP_LEA:   return "LEA";    case OP_MOV:   return "MOV";
        case OP_ADD:   return "ADD";    case OP_ADDI:  return "ADDI";
        case OP_SUB:   return "SUB";    case OP_SUBI:  return "SUBI";
        case OP_MUL:   return "MUL";    case OP_DIV:   return "DIV";
        case OP_MOD:   return "MOD";    case OP_NEG:   return "NEG";
        case OP_FADD:  return "FADD";   case OP_FSUB:  return "FSUB";
        case OP_FMUL:  return "FMUL";   case OP_FDIV:  return "FDIV";
        case OP_FNEG:  return "FNEG";
        case OP_AND:   return "AND";    case OP_OR:    return "OR";
        case OP_XOR:   return "XOR";    case OP_NOT:   return "NOT";
        case OP_SHL:   return "SHL";    case OP_SHR:   return "SHR";
        case OP_SAR:   return "SAR";    case OP_ANDI:  return "ANDI";
        case OP_ORI:   return "ORI";
        case OP_CEQ:   return "CEQ";    case OP_CNE:   return "CNE";
        case OP_CLT:   return "CLT";    case OP_CLE:   return "CLE";
        case OP_CGT:   return "CGT";    case OP_CGE:   return "CGE";
        case OP_CLTU:  return "CLTU";   case OP_CLEU:  return "CLEU";
        case OP_FCEQ:  return "FCEQ";   case OP_FCLT:  return "FCLT";
        case OP_FCLE:  return "FCLE";
        case OP_JMP:   return "JMP";    case OP_JT:    return "JT";
        case OP_JF:    return "JF";     case OP_JMPR:  return "JMPR";
        case OP_CALL:  return "CALL";   case OP_CALLR: return "CALLR";
        case OP_RET:   return "RET";    case OP_ENTER: return "ENTER";
        case OP_LEAVE: return "LEAVE";  case OP_CALLP: return "CALLP";
        case OP_SX8:   return "SX8";    case OP_SX16:  return "SX16";
        case OP_ZX8:   return "ZX8";    case OP_ZX16:  return "ZX16";
        case OP_TRUNC8:return "TRUNC8"; case OP_TRUNC16:return "TRUNC16";
        case OP_I2F:   return "I2F";    case OP_F2I:   return "F2I";
        case OP_PUSH:  return "PUSH";   case OP_POP:   return "POP";
        case OP_NOP:   return "NOP";    case OP_HALT:  return "HALT";
        case OP_GFX_CLEAR:  return "GFX_CLEAR";
        case OP_GFX_SPRITE: return "GFX_SPRITE";
        case OP_GFX_PLOT:   return "GFX_PLOT";   case OP_GFX_FLIP:   return "GFX_FLIP";
        case OP_GFX_LINE:   return "GFX_LINE";   case OP_GFX_WINDOW: return "GFX_WINDOW";
        case OP_GFX_POLYGON:return "GFX_POLYGON";case OP_GFX_POLYLINE:return "GFX_POLYLINE";
        case OP_GFX_CAMERA: return "GFX_CAMERA"; case OP_GFX_TEXTURE:return "GFX_TEXTURE";
        case OP_GFX_LIGHT:  return "GFX_LIGHT";  case OP_GFX_MESH:   return "GFX_MESH";
        case OP_GFX_TEXT:   return "GFX_TEXT";   case OP_GFX_VIEWPORT:return "GFX_VIEWPORT";
        case OP_GFX_TEXT_MEASURE: return "GFX_TEXT_MEASURE"; case OP_GFX_CLIP: return "GFX_CLIP";
        case OP_GFX_VIEW: return "GFX_VIEW";
        case OP_GFX_VIEW_CAPTURE: return "GFX_VIEW_CAPTURE";
        case OP_GFX_VIEW_PRESENT: return "GFX_VIEW_PRESENT";
        case OP_GFX_ELLIPSE: return "GFX_ELLIPSE"; case OP_GFX_ELLIPSE_FILLED: return "GFX_ELLIPSE_FILLED";
        case OP_GFX_ROUNDED_RECT: return "GFX_ROUNDED_RECT"; case OP_GFX_ROUNDED_RECT_FILLED: return "GFX_ROUNDED_RECT_FILLED";
        case OP_GFX_PALETTE_UPLOAD: return "GFX_PALETTE_UPLOAD";
        case OP_INP_KEY:    return "INP_KEY";     case OP_INP_KEYHELD:return "INP_KEYHELD";
        case OP_INP_GAMEPAD_COUNT: return "INP_GAMEPAD_COUNT"; case OP_INP_GAMEPAD: return "INP_GAMEPAD";
        case OP_SYS_RAND:   return "SYS_RAND";   case OP_SYS_MILLIS: return "SYS_MILLIS";
        case OP_SYS_CONSOLE: return "SYS_CONSOLE"; case OP_SYS_SNPRINT: return "SYS_SNPRINT";
        case OP_SYS_ASSERT: return "SYS_ASSERT";
        case OP_SYS_ALLOC:  return "SYS_ALLOC";  case OP_SYS_FREE:   return "SYS_FREE";
        case OP_SYS_MEMCPY: return "SYS_MEMCPY"; case OP_SYS_MEMSET: return "SYS_MEMSET";
        case OP_SYS_FOPEN:  return "SYS_FOPEN";  case OP_SYS_FREAD:  return "SYS_FREAD";
        case OP_SYS_FCLOSE: return "SYS_FCLOSE"; case OP_SYS_FWRITE: return "SYS_FWRITE";
        case OP_SYS_FSEEK:  return "SYS_FSEEK";  case OP_SYS_FLEN:   return "SYS_FLEN";
        case OP_SYS_FDIR:   return "SYS_FDIR";   case OP_SYS_FPATH:  return "SYS_FPATH";
        case OP_SYS_ARGS:   return "SYS_ARGS";   case OP_SYS_ISNUM:  return "SYS_ISNUM";
        case OP_SYS_SIN:    return "SYS_SIN";    case OP_SYS_COS:    return "SYS_COS";
        case OP_SYS_SQRT:   return "SYS_SQRT";
        case OP_SYS_EXP:    return "SYS_EXP";    case OP_SYS_LOG:    return "SYS_LOG";
        case OP_SYS_LOG10:  return "SYS_LOG10";  case OP_SYS_POW:    return "SYS_POW";
        case OP_SYS_ATAN:   return "SYS_ATAN";   case OP_SYS_ATAN2:  return "SYS_ATAN2";
        case OP_SYS_TAN:    return "SYS_TAN";    case OP_SYS_ASIN:   return "SYS_ASIN";
        case OP_SYS_ACOS:   return "SYS_ACOS";   case OP_SYS_FLOOR:  return "SYS_FLOOR";
        case OP_SYS_CEIL:   return "SYS_CEIL";   case OP_SYS_ROUND:  return "SYS_ROUND";
        case OP_SYS_FMOD:   return "SYS_FMOD";   case OP_SYS_FABS:   return "SYS_FABS";
        case OP_SYS_SINH:   return "SYS_SINH";   case OP_SYS_COSH:   return "SYS_COSH";
        case OP_SYS_TANH:   return "SYS_TANH";
        case OP_SYS_HTTP_REQ:    return "SYS_HTTP_REQ";
        case OP_SYS_HTTP_POLL:   return "SYS_HTTP_POLL";
        case OP_SYS_HTTP_STATUS: return "SYS_HTTP_STATUS";
        case OP_SYS_HTTP_READ:   return "SYS_HTTP_READ";
        case OP_SYS_HTTP_CLOSE:  return "SYS_HTTP_CLOSE";
        case OP_SYS_UNZIP:       return "SYS_UNZIP";
        case OP_SYS_REGISTER:    return "SYS_REGISTER";
        case OP_SYS_FDELETE:     return "SYS_FDELETE";
        case OP_SYS_FMKDIR:      return "SYS_FMKDIR";
        case OP_SYS_CLIP_SET:    return "SYS_CLIP_SET";
        case OP_SYS_CLIP_GET:    return "SYS_CLIP_GET";
        case OP_SYS_PROC_SPAWN:      return "SYS_PROC_SPAWN";
        case OP_SYS_PROC_POLL:       return "SYS_PROC_POLL";
        case OP_SYS_PROC_EXIT_CODE:  return "SYS_PROC_EXIT_CODE";
        case OP_SYS_PROC_READ_OUT:   return "SYS_PROC_READ_OUT";
        case OP_SYS_PROC_READ_ERR:   return "SYS_PROC_READ_ERR";
        case OP_SYS_PROC_KILL:       return "SYS_PROC_KILL";
        case OP_SYS_PROC_CLOSE:      return "SYS_PROC_CLOSE";
        case OP_SYS_SERVE:           return "SYS_SERVE";
        case OP_SYS_SERVE_REPLY:     return "SYS_SERVE_REPLY";
        case OP_SYS_OS:              return "SYS_OS";
        case OP_DCEQ:  return "DCEQ";   case OP_DCLT:  return "DCLT";
        case OP_DCLE:  return "DCLE";
        case OP_DADD:  return "DADD";   case OP_DSUB:  return "DSUB";
        case OP_DMUL:  return "DMUL";   case OP_DDIV:  return "DDIV";
        case OP_DNEG:  return "DNEG";   case OP_DMOV:  return "DMOV";
        case OP_DLD:   return "DLD";    case OP_DST:   return "DST";
        case OP_DLDC:  return "DLDC";
        case OP_DBITS_LO: return "DBITS_LO"; case OP_DBITS_HI: return "DBITS_HI";
        case OP_DFROMBITS: return "DFROMBITS";
        case OP_DSQRT: return "DSQRT";  case OP_DSIN:  return "DSIN";
        case OP_DCOS:  return "DCOS";   case OP_DTAN:  return "DTAN";
        case OP_DASIN: return "DASIN";  case OP_DACOS: return "DACOS";
        case OP_DATAN: return "DATAN";  case OP_DATAN2:return "DATAN2";
        case OP_DEXP:  return "DEXP";   case OP_DLOG:  return "DLOG";
        case OP_DLOG10:return "DLOG10"; case OP_DPOW:  return "DPOW";
        case OP_DFLOOR:return "DFLOOR"; case OP_DCEIL: return "DCEIL";
        case OP_DROUND:return "DROUND"; case OP_DFMOD: return "DFMOD";
        case OP_DFABS: return "DFABS";  case OP_DSINH: return "DSINH";
        case OP_DCOSH: return "DCOSH";  case OP_DTANH: return "DTANH";
        case OP_I2D:   return "I2D";    case OP_D2I:   return "D2I";
        case OP_F2D:   return "F2D";    case OP_D2F:   return "D2F";
        case OP_U2D:   return "U2D";    case OP_D2U:   return "D2U";
        default: return "???";
    }
}

/* Which register operands of an f64 op are d-bank (dN) rather than
 * r-bank (rN): bit0 = dst, bit1 = srca, bit2 = srcb. The encoding's
 * 4-bit register fields are shared between banks; this table is how the
 * disassemblers print the right prefix. Shared by relic_ir_disasm and
 * main.c's write_assembly. */
unsigned relic_op_dreg_mask(u8 op) {
    switch(op) {
        /* compare: d,d -> r (bool result) */
        case OP_DCEQ: case OP_DCLT: case OP_DCLE:  return 0x6;
        /* 3-operand d,d,d */
        case OP_DADD: case OP_DSUB: case OP_DMUL: case OP_DDIV:
        case OP_DATAN2: case OP_DPOW: case OP_DFMOD: return 0x7;
        /* unary d -> d */
        case OP_DNEG: case OP_DMOV:
        case OP_DSQRT: case OP_DSIN: case OP_DCOS: case OP_DTAN:
        case OP_DASIN: case OP_DACOS: case OP_DATAN: case OP_DEXP:
        case OP_DLOG: case OP_DLOG10: case OP_DFLOOR: case OP_DCEIL:
        case OP_DROUND: case OP_DFABS: case OP_DSINH: case OP_DCOSH:
        case OP_DTANH: return 0x3;
        /* d <- r-base load/const/bits materialise */
        case OP_DLD: case OP_DLDC: case OP_DFROMBITS:
        case OP_I2D: case OP_U2D: case OP_F2D:     return 0x1;
        /* d -> r conversions; DST stores d via r-base */
        case OP_DST: case OP_D2I: case OP_D2U: case OP_D2F:
        case OP_DBITS_LO: case OP_DBITS_HI:        return 0x2;
        default: return 0;
    }
}

void relic_ir_disasm(IRBuffer *ir, int from, int to) {
    if(to < 0 || to > ir->len) to = ir->len;
    for(int i=from;i<to;i++) {
        u32 instr = IRCODE(ir, i);
        u8  op    = IR_OP(instr);
        int line  = IRLINE_RD(ir, i);
        /* OP_ENTER carries its frame size as a 20-bit immediate in bits
         * [19:0]; the register nibbles are repurposed, so print the wide
         * value rather than the misleading imm12/reg triple. OP_LDC32's
         * data word is shown as a raw .imm32 line. */
        i32 disp_imm = (op == OP_ENTER) ? (i32)(instr & 0xFFFFFu)
                                        : IR_IMM12(instr);
        unsigned dm = relic_op_dreg_mask(op);
        char rd = (dm & 1) ? 'd' : 'r';
        char ra = (dm & 2) ? 'd' : 'r';
        char rb = (dm & 4) ? 'd' : 'r';
        if(op == OP_LDC32 && i + 1 < to) {
            if(line > 0)
                printf("%04d  L%-4d  %-12s  %c%d, %c%d, %c%d  imm=%d\n",
                       i, line, relic_opcode_name(op),
                       rd, IR_DST(instr), ra, IR_SRCA(instr), rb, IR_SRCB(instr),
                       disp_imm);
            else
                printf("%04d         %-12s  %c%d, %c%d, %c%d  imm=%d\n",
                       i, relic_opcode_name(op),
                       rd, IR_DST(instr), ra, IR_SRCA(instr), rb, IR_SRCB(instr),
                       disp_imm);
            i++;
            printf("%04d         .imm32        %u\n", i, IRCODE(ir, i));
        } else if(op == OP_DLDC && i + 2 < to) {
            /* DLDC carries a 64-bit literal as two payload words. */
            if(line > 0)
                printf("%04d  L%-4d  %-12s  %c%d, %c%d, %c%d  imm=%d\n",
                       i, line, relic_opcode_name(op),
                       rd, IR_DST(instr), ra, IR_SRCA(instr), rb, IR_SRCB(instr),
                       disp_imm);
            else
                printf("%04d         %-12s  %c%d, %c%d, %c%d  imm=%d\n",
                       i, relic_opcode_name(op),
                       rd, IR_DST(instr), ra, IR_SRCA(instr), rb, IR_SRCB(instr),
                       disp_imm);
            /* Two bare .imm32 lines (lo then hi) so the output
             * re-assembles through the same path as LDC32's payload. */
            i++;
            printf("%04d         .imm32        %u\n", i, IRCODE(ir, i));
            i++;
            printf("%04d         .imm32        %u\n", i, IRCODE(ir, i));
        } else if(line > 0) {
            printf("%04d  L%-4d  %-12s  %c%d, %c%d, %c%d  imm=%d\n",
                   i, line, relic_opcode_name(op),
                   rd, IR_DST(instr), ra, IR_SRCA(instr), rb, IR_SRCB(instr),
                   disp_imm);
        } else {
            printf("%04d         %-12s  %c%d, %c%d, %c%d  imm=%d\n",
                   i, relic_opcode_name(op),
                   rd, IR_DST(instr), ra, IR_SRCA(instr), rb, IR_SRCB(instr),
                   disp_imm);
        }
    }
}

/* -------------------------------------------------------------------------
 * Debug helpers
 * ---------------------------------------------------------------------- */
int relic_pc_to_fn(IRBuffer *ir, u32 pc) {
    /* Linear scan; nfuncs is small in practice. Returns the fn_id whose
     * fn_offsets[id] <= pc and is the largest such id, else -1. */
    int best = -1;
    int best_off = -1;
    for(int i=0;i<ir->nfuncs;i++) {
        int off = IRFNOFF(ir, i);
        if(off <= (int)pc && off > best_off) {
            best_off = off;
            best = i;
        }
    }
    return best;
}

/* Resolve `path` to an index into ir->source_files[], or -1 if `ir` has no
 * file table (e.g. loaded bytecode - see relic_bytecode_load) or `path`
 * doesn't match any entry. Tries an exact match first, then a basename
 * match, since a `#line "path"` directive's spelling (relative, absolute,
 * or artifact-resolved) doesn't necessarily match how the caller names the
 * same file (e.g. the debugger's default is IRBuffer::source_path, which
 * predates multi-file support and may be spelled differently). */
static int find_source_file_id(IRBuffer *ir, const char *path) {
    if(!path || !*path || ir->nsource_files == 0) return -1;
    /* source_files may be far (ia16): bounce each entry into a near buffer
     * for strcmp/strrchr. */
    for(int i=0; i<ir->nsource_files; i++) {
        char pb[RELIC_MAX_SOURCE_PATH];
        relic_far_str(pb, IRSRCFILE(ir, i), sizeof pb);
        if(strcmp(pb, path) == 0) return i;
    }
    const char *base = strrchr(path, '/');
    const char *base2 = strrchr(path, '\\');
    if(base2 && (!base || base2 > base)) base = base2;
    base = base ? base + 1 : path;
    for(int i=0; i<ir->nsource_files; i++) {
        char pb[RELIC_MAX_SOURCE_PATH];
        relic_far_str(pb, IRSRCFILE(ir, i), sizeof pb);
        const char *b2 = strrchr(pb, '/');
        const char *b3 = strrchr(pb, '\\');
        if(b3 && (!b2 || b3 > b2)) b2 = b3;
        b2 = b2 ? b2 + 1 : pb;
        if(strcmp(b2, base) == 0) return i;
    }
    return -1;
}

int relic_line_to_pc(IRBuffer *ir, const char *path, int line) {
    if(line <= 0) return -1;
    /* Multi-file model: if `path` resolves to a known file, only consider
     * PCs attributed to that file. Falls back to the old single-source-file
     * behaviour (match by line alone) when there's no file table to
     * consult, or `path` doesn't name a file we recognise - this keeps
     * every existing single-file debugging session working unchanged. */
    int file_id = find_source_file_id(ir, path);
    /* First try exact match. */
    for(int i=0;i<ir->len;i++) {
        if(IRLINE_RD(ir, i) == line && (file_id < 0 || IRFILEID_RD(ir, i) == file_id)) return i;
    }
    /* Fall back: the next executable line at or after `line`. */
    int best = -1;
    int best_line = 0;
    for(int i=0;i<ir->len;i++) {
        if(file_id >= 0 && IRFILEID_RD(ir, i) != file_id) continue;
        int l = IRLINE_RD(ir, i);
        if(l >= line && (best < 0 || l < best_line)) {
            best = i;
            best_line = l;
        }
    }
    return best;
}

/* -------------------------------------------------------------------------
 * Bytecode serialization
 * Format:
 *   Magic: "REL\0" (4 bytes)
 *   Format version: 5 (1 byte)
 *   Flags: bit 0 = has_debug (see below); bit 1 = tier stamp present in
 *     bits 2-3; other bits reserved, must be 0 (1 byte)
 *   Relic version major (4 bytes)
 *   Relic version minor (4 bytes)
 *   Relic version patch (4 bytes)
 *   Instruction count (4 bytes)
 *   Function count (4 bytes)
 *   String table length (4 bytes)
 *   Globals size (4 bytes)
 *   Main function ID (4 bytes)
 *   Heap bytes (4 bytes; v5+ only, 0 = no preference)
 *   Instructions (instruction_count * 4 bytes)
 *   Function offsets (function_count * 4 bytes)
 *   String table (strings_len bytes)
 *   -- if flags & RELIC_BYTECODE_FLAG_DEBUG --
 *   Source file count (4 bytes)
 *   For each source file: path length (4 bytes), path bytes (no NUL)
 *   line_for_pc, RLE-compressed: run count (4 bytes), then per run:
 *     run length (4 bytes), line number (4 bytes signed)
 *   file_for_pc, RLE-compressed: same shape, with a source-file index
 *     (-1 = unknown) instead of a line number
 *
 * The debug section is opt-in (tigryxri's --debug-info; see
 * relic_bytecode_write's `include_debug` parameter) so ordinary .tlx
 * output doesn't grow just from compiling with file/line tracking always
 * on. It sits before wherever an asset trailer would be appended (that
 * trailer is located by scanning *backwards* from EOF for its own magic
 * word - see relic_assets_load - so an extra section here doesn't disturb
 * it either way). RLE compresses well: line/file rarely change more than
 * once per source statement, which is many IR words.
 * ---------------------------------------------------------------------- */
#define RELIC_BYTECODE_MAGIC 0x004C4552  /* "REL\0" little-endian */
#define RELIC_BYTECODE_VERSION 2
/* Version 3 is written ONLY when the code stream contains f64 opcodes
 * (see ir_uses_f64_op). Programs that never touch f64 stay version 2 and
 * keep loading on every VM built before f64 existed - including all the
 * committed .tlx game binaries and the shipped web runtimes. An f64
 * program on a pre-f64 VM fails at load with the version error instead
 * of "unknown opcode" mid-execution. Both loaders accept 2 or 3. */
#define RELIC_BYTECODE_VERSION_F64 3
/* Version 4: JMP/JT/JF carry PC-relative displacements (IR_JTARGET)
 * instead of 20-bit absolute targets, so program size is no longer capped
 * at 1M IR words. Always written. v2/v3 files still load: their absolute
 * targets are rewritten to displacements in ir_upgrade_legacy_jumps. */
#define RELIC_BYTECODE_VERSION_RELJMP 4
/* Version 5: adds a u32 heap_bytes field right after main_fn_id, carrying
 * the program's `#pragma arg heap` (or the --heap given at bc time);
 * 0 means "no preference". Always written: v5 is the only emitted
 * format, so every consumer sees exactly one header layout. */
#define RELIC_BYTECODE_VERSION_HEAP 5
#define RELIC_BYTECODE_FLAG_DEBUG 0x01u
/* bit1: Adaptive Build tier stamp present in bits 2-3 (RELIC_TIER_*
 * ordinal, see tigryx_tier.h). Lives in the flags byte rather than a
 * header field so old readers — which only test bit0 — load a stamped
 * file unchanged, and a new reader faced with an unstamped file just
 * sees tier == -1. The stamp records the tier the program was
 * *preprocessed/compiled* for (from `#pragma arg tier` in the .tp, or
 * the detected tier when the source had no opinion). */
#define RELIC_BYTECODE_FLAG_TIER  0x02u
#define RELIC_BYTECODE_TIER_SHIFT 2
#define RELIC_BYTECODE_TIER_MASK  0x0Cu

static bool bytecode_version_ok(u8 version) {
    return version == RELIC_BYTECODE_VERSION ||
           version == RELIC_BYTECODE_VERSION_F64 ||
           version == RELIC_BYTECODE_VERSION_RELJMP ||
           version == RELIC_BYTECODE_VERSION_HEAP;
}

/* v2/v3 files encode JMP/JT/JF targets as 20-bit absolute PCs in the same
 * bit positions v4 uses for the displacement. Rewrite them in place so
 * every consumer only ever sees v4 code. Payload words (LDC32: 1,
 * DLDC: 2) are skipped so a constant can't be misread as a jump. */
static void ir_upgrade_legacy_jumps(IRBuffer *ir, u8 version) {
    if(version >= RELIC_BYTECODE_VERSION_RELJMP) return;
    for(int pc = 0; pc < ir->len; pc++) {
        u32 w  = IRCODE(ir, pc);
        u8  op = IR_OP(w);
        if(op == OP_JMP || op == OP_JT || op == OP_JF) {
            i32 tgt = (i32)((((w >> 4) & 0xF0000u) | (w & 0xFFFFu)));
            IRCODE(ir, pc) = IR_JSET(w, pc, tgt);
        } else if(op == OP_DLDC) {
            pc += 2;
        } else if(op == OP_LDC32) {
            pc += 1;
        }
    }
}

/* -------------------------------------------------------------------------
 * On-disk read/write wrappers. The .tlx format is little-endian on every
 * host (see relic_rd*le/relic_wr*le in tigryx.h), so a bytecode file or
 * bound executable produced on any platform loads everywhere - including
 * big-endian targets such as SPARC.
 * ---------------------------------------------------------------------- */
static bool wr_u8le(FILE *f, u8 v) {
    return fwrite(&v, 1, 1, f) == 1;
}
static bool wr_u32le_f(FILE *f, u32 v) {
    u8 b[4];
    relic_wr32le(b, v);
    return fwrite(b, 1, 4, f) == 4;
}
static bool rd_u8le(FILE *f, u8 *out) {
    return fread(out, 1, 1, f) == 1;
}
static bool rd_u32le_f(FILE *f, u32 *out) {
    u8 b[4];
    if(fread(b, 1, 4, f) != 4) return false;
    *out = relic_rd32le(b);
    return true;
}
/* Write `count` u32 array elements as LE through a stack staging buffer.
 * `arr` may be far (ia16): elements are fetched via normalized far
 * addressing, so arrays larger than 64K are safe. */
static bool wr_u32arr_le_f(FILE *f, const u32 RELIC_FAR *arr, u32 count) {
#ifdef RELIC_IA16
    u8 buf[1024];   /* real-mode stack is small */
#else
    u8 buf[4096];
#endif
    u32 n = 0;
    for(u32 i = 0; i < count; i++) {
        if(n + 4 > sizeof buf) {
            if(fwrite(buf, 1, n, f) != n) return false;
            n = 0;
        }
        u32 v = *(const u32 RELIC_FAR *)relic_cfp_add((const u8 RELIC_FAR *)arr, i * 4u);
        relic_wr32le(buf + n, v);
        n += 4;
    }
    return fwrite(buf, 1, n, f) == n;
}
/* Bulk-read `count` LE u32s into a host-order array (near). */
static bool rd_u32arr_le_f(FILE *f, u32 *arr, u32 count) {
    if(fread(arr, 4, count, f) != count) return false;
    for(u32 i = 0; i < count; i++) arr[i] = relic_rd32le(&arr[i]);
    return true;
}
/* fread(*,1,n) for RELIC_RISCOS: Arculator's HostFS never implements
 * OS_GBPB (empty stub), and SCL fread switches to GBPB for bulk reads and
 * spins. Byte reads go via OS_BGet which works - read through fgetc. */
#ifdef RELIC_RISCOS
static size_t rd_bytes(FILE *f, u8 *dst, u32 n) {
    u32 i;
    for(i = 0; i < n; i++) {
        int c = fgetc(f);
        if(c == EOF) break;
        dst[i] = (u8)c;
    }
    return (size_t)i;
}
#define rd_bytes_f(f, d, n) rd_bytes((f), (u8*)(d), (n))
#else
#define rd_bytes_f(f, d, n) fread((d), 1, (n), (f))
#endif

/* Far-array counterpart of rd_u32arr_le_f: reads through a near staging
 * buffer, then copies out with normalized far addressing (ia16). */
static bool rd_u32arr_far_f(FILE *f, u32 RELIC_FAR *arr, u32 count) {
#ifdef RELIC_IA16
    u32 buf[256];   /* real-mode stack is small */
#elif defined(RELIC_RISCOS)
    /* static: Wimp-task stacks on RO3.x are small and SCL's stack-limit
     * check aborts silently on a big local array */
    static u32 buf[1024];
#else
    u32 buf[1024];
#endif
    while(count) {
        u32 n = count > (u32)(sizeof buf / sizeof buf[0]) ? (u32)(sizeof buf / sizeof buf[0]) : count;
        if(rd_bytes_f(f, buf, n * 4u) != n * 4u) return false;
        for(u32 i = 0; i < n; i++) buf[i] = relic_rd32le(&buf[i]);
        relic_far_write((u8 RELIC_FAR *)arr, 0, buf, n * 4u);
        arr = (u32 RELIC_FAR *)relic_fp_add((u8 RELIC_FAR *)arr, n * 4u);
        count -= n;
    }
    return true;
}
/* fread/fwrite into far memory - shared bounce-buffer helpers (far16.c). */
static bool fread_far(FILE *f, u8 RELIC_FAR *dst, u32 n) {
    return relic_fread_far(f, dst, n) == n;
}
static bool fwrite_far(FILE *f, const u8 RELIC_FAR *src, u32 n) {
    return relic_fwrite_far(f, src, n) == n;
}

/* Write `arr[0..count)` as a run-length-encoded stream: a u32 run count,
 * then that many (u32 run_length, i32 value) pairs. Cheap two-pass (count
 * runs, then write them) rather than buffering - `count` is at most
 * RELIC_MAX_IR (131072), so this is negligible next to the rest of the
 * write. */
static void write_rle_i32(FILE *f, const i32 RELIC_FAR *arr, int count) {
    /* Element reads go through normalized far addressing so a >64K map is
     * safe on ia16 (plain far indexing would wrap at the segment). */
    u32 nruns = 0;
    for(int i = 0; i < count; ) {
        int j = i + 1;
        while(j < count &&
              *(const i32 RELIC_FAR *)relic_cfp_add((const u8 RELIC_FAR *)arr, (u32)j*4u) ==
              *(const i32 RELIC_FAR *)relic_cfp_add((const u8 RELIC_FAR *)arr, (u32)i*4u)) j++;
        nruns++;
        i = j;
    }
    wr_u32le_f(f, nruns);
    for(int i = 0; i < count; ) {
        int j = i + 1;
        while(j < count &&
              *(const i32 RELIC_FAR *)relic_cfp_add((const u8 RELIC_FAR *)arr, (u32)j*4u) ==
              *(const i32 RELIC_FAR *)relic_cfp_add((const u8 RELIC_FAR *)arr, (u32)i*4u)) j++;
        wr_u32le_f(f, (u32)(j - i));
        wr_u32le_f(f, (u32)*(const i32 RELIC_FAR *)relic_cfp_add((const u8 RELIC_FAR *)arr, (u32)i*4u));
        i = j;
    }
}

/* Inverse of write_rle_i32, reading from an already-open FILE*. Fills
 * exactly `count` entries of `out_arr` or fails if the stream is short,
 * malformed (a run overruns `count`), or doesn't cover every entry. */
static bool read_rle_i32_file(FILE *f, i32 RELIC_FAR *out_arr, int count) {
    u32 nruns;
    if(!rd_u32le_f(f, &nruns)) return false;
    int written = 0;
    for(u32 r = 0; r < nruns; r++) {
        u32 run_len; i32 val;
        if(!rd_u32le_f(f, &run_len)) return false;
        if(!rd_u32le_f(f, (u32*)&val)) return false;
        if((i64)written + (i64)run_len > (i64)count) return false;
        for(u32 k = 0; k < run_len; k++)
            *(i32 RELIC_FAR *)relic_fp_add((u8 RELIC_FAR *)out_arr, (u32)written++ * 4u) = val;
    }
    return written == count;
}

/* Mem-cursor counterpart of read_rle_i32_file, for relic_bytecode_load_mem.
 * `p` may be far (ia16); `remaining` tracks bytes left so the cursor never
 * needs a second pointer (far-pointer subtraction isn't normalized). */
static bool read_rle_i32_mem(const u8 RELIC_FAR **p, u32 *remaining,
                             i32 RELIC_FAR *out_arr, int count) {
    u8 b[4];
    if(*remaining < 4) return false;
    relic_far_read(*p, 0, b, 4);
    u32 nruns = relic_rd32le(b);
    *p = relic_cfp_add(*p, 4); *remaining -= 4;
    int written = 0;
    for(u32 r = 0; r < nruns; r++) {
        if(*remaining < 8) return false;
        u8 b8[8];
        relic_far_read(*p, 0, b8, 8);
        *p = relic_cfp_add(*p, 8); *remaining -= 8;
        u32 run_len = relic_rd32le(b8);
        i32 val     = (i32)relic_rd32le(b8 + 4);
        if((i64)written + (i64)run_len > (i64)count) return false;
        for(u32 k = 0; k < run_len; k++)
            *(i32 RELIC_FAR *)relic_fp_add((u8 RELIC_FAR *)out_arr, (u32)written++ * 4u) = val;
    }
    return written == count;
}

/* Stream the bytecode payload to an already-open FILE*. Does NOT close f.
 * Used by both relic_bytecode_save (path-based) and by --output-executable
 * to append the payload mid-stream into a bound runtime executable. */
bool relic_bytecode_write(IRBuffer *ir, FILE *f, int main_fn_id, bool include_debug) {
    if(!f) return false;

    /* A debug section needs a file table to be meaningful - an IRBuffer
     * built by the assembler or loaded from bytecode has none, so silently
     * downgrade to no-debug rather than writing an empty-but-flagged
     * section. */
    /* ia16: the line/file tables are lazily allocated, so an IR that was
     * loaded without them can only write a non-debug .tlx. */
    bool want_debug = include_debug && ir->nsource_files > 0 && ir->line_for_pc != NULL;

    u8  version        = RELIC_BYTECODE_VERSION_HEAP;
    u8  flags          = want_debug ? RELIC_BYTECODE_FLAG_DEBUG : 0;
    flags |= RELIC_BYTECODE_FLAG_TIER
           | (u8)((relic_tier_detect() & 3) << RELIC_BYTECODE_TIER_SHIFT);
    u32 instr_count    = (u32)ir->len;
    u32 func_count     = (u32)ir->nfuncs;
    u32 strings_len    = (u32)ir->strings_len;

    wr_u32le_f(f, RELIC_BYTECODE_MAGIC);
    wr_u8le(f, version);
    wr_u8le(f, flags);
    wr_u32le_f(f, RELIC_VERSION_MAJOR);
    wr_u32le_f(f, RELIC_VERSION_MINOR);
    wr_u32le_f(f, RELIC_VERSION_PATCH);
    wr_u32le_f(f, instr_count);
    wr_u32le_f(f, func_count);
    wr_u32le_f(f, strings_len);
    wr_u32le_f(f, (u32)ir->globals_size);
    wr_u32le_f(f, (u32)main_fn_id);
    wr_u32le_f(f, ir->heap_size);
    wr_u32arr_le_f(f, ir->code, instr_count);
    wr_u32arr_le_f(f, (const u32 RELIC_FAR *)ir->fn_offsets, func_count);
    if(!fwrite_far(f, (const u8 RELIC_FAR *)ir->strings, strings_len))
        return false;

    if(want_debug) {
        u32 nfiles = (u32)ir->nsource_files;
        wr_u32le_f(f, nfiles);
        for(u32 i = 0; i < nfiles; i++) {
            char pb[RELIC_MAX_SOURCE_PATH];
            u32 plen = (u32)relic_far_str(pb, IRSRCFILE(ir, i), sizeof pb);
            wr_u32le_f(f, plen);
            fwrite(pb, 1, plen, f);
        }
        write_rle_i32(f, ir->line_for_pc, (int)instr_count);
        write_rle_i32(f, ir->file_for_pc, (int)instr_count);
        /* Symbols blob: "LS\0\x01" magic + u32 length + records (the
         * layout documented at dbg_sym). Old loaders stop at the maps;
         * new ones pick up names/types/offsets for the variables view. */
        wr_u32le_f(f, 0x0100534Cu);   /* bytes: 'L','S',0,1 */
        wr_u32le_f(f, (u32)ir->dbg_syms_len);
        if(ir->dbg_syms_len) fwrite(ir->dbg_syms, 1, (size_t)ir->dbg_syms_len, f);
    }
    return true;
}

bool relic_bytecode_save(IRBuffer *ir, const char *path, int main_fn_id, bool include_debug) {
    FILE *f = fopen(path, "wb");
    if(!f) return false;
    bool ok = relic_bytecode_write(ir, f, main_fn_id, include_debug);
    fclose(f);
    return ok;
}

/* In-memory variant of relic_bytecode_load. Walks a byte cursor over `data`
 * exactly mirroring the file-based reader. Used by the bound-executable path
 * in tigryx.c, where bytecode lives at the tail of the running .exe.
 * `data` may be far (ia16: bound payloads live in real-mode far memory -
 * a near malloc cannot hold a large .tlx); reads go through near staging. */
bool relic_bytecode_load_mem(IRBuffer *ir, const void RELIC_FAR *data, size_t len,
                             int *out_main_fn_id) {
    if(!data) return false;
    const u8 RELIC_FAR *p = (const u8 RELIC_FAR *)data;
    u32 remaining = (u32)len;

    #define RB_TAKE(dst, n) do {                                              \
        if(remaining < (u32)(n)) return false;                                \
        relic_far_read(p, 0, (void RELIC_FAR *)(dst), (u32)(n));              \
        p = relic_cfp_add(p, (u32)(n)); remaining -= (u32)(n);                \
    } while(0)
    #define RB_TAKE32(dst) do {                                               \
        u32 rb_v;                                                             \
        RB_TAKE(&rb_v, 4);                                                    \
        (dst) = relic_rd32le(&rb_v);                                          \
    } while(0)

    u32 magic;
    u8  version, flags;
    u32 ver_major, ver_minor, ver_patch;
    u32 instr_count, func_count, strings_len, globals_size, main_fn_id_u32;

    RB_TAKE32(magic);
    if(magic != RELIC_BYTECODE_MAGIC) return false;
    RB_TAKE(&version, 1);
    if(!bytecode_version_ok(version)) return false;
    RB_TAKE(&flags, 1);
    ir->tier = (flags & RELIC_BYTECODE_FLAG_TIER)
             ? (int)((flags & RELIC_BYTECODE_TIER_MASK) >> RELIC_BYTECODE_TIER_SHIFT)
             : -1;
    RB_TAKE32(ver_major);
    RB_TAKE32(ver_minor);
    RB_TAKE32(ver_patch);
    RB_TAKE32(instr_count);
    RB_TAKE32(func_count);
    RB_TAKE32(strings_len);
    RB_TAKE32(globals_size);
    RB_TAKE32(main_fn_id_u32);
    /* v5 adds a u32 heap_bytes field; earlier versions have none. */
    u32 heap_bytes = 0;
    if(version >= RELIC_BYTECODE_VERSION_HEAP) {
        RB_TAKE32(heap_bytes);
    }
    ir->heap_size = heap_bytes;
    if(ver_major > RELIC_VERSION_MAJOR ||
       (ver_major == RELIC_VERSION_MAJOR && ver_minor > RELIC_VERSION_MINOR)) {
        fprintf(stderr,
            "Warning: TLX file was created with Relic %u.%u.%u, "
            "but current version is %s. May not be fully compatible.\n",
            ver_major, ver_minor, ver_patch, RELIC_VERSION);
    }

    if(func_count > RELIC_MAX_FUNCS || strings_len > RELIC_MAX_STRINGS) return false;
    /* ia16: ir->len is a 16-bit int so instr_count > INT_MAX can't be
     * represented, but globals_size is i32 - let it load and fail later
     * at vm_init with a proper "memory too small" diagnostic. */
    if(instr_count > (u32)INT_MAX || globals_size > 0x7FFFFFFFu) return false;
    if(flags & RELIC_BYTECODE_FLAG_DEBUG) ir->want_dbg = 1;
    if(!relic_ir_reserve(ir, (int)instr_count)) return false;

    #define RB_TAKE_FAR(dst, n) do {                                          \
        if(remaining < (u32)(n)) return false;                                \
        relic_far_write((u8 RELIC_FAR *)(dst), 0, p, (u32)(n));               \
        p = relic_cfp_add(p, (u32)(n)); remaining -= (u32)(n);                \
    } while(0)

    RB_TAKE_FAR(ir->code,   4u * instr_count);
    RB_TAKE_FAR(ir->fn_offsets, 4u * func_count);
    RB_TAKE_FAR(ir->strings, strings_len);
    #undef RB_TAKE_FAR

    /* The file stream is little-endian; convert the u32 views in place.
     * (A no-op byte-for-byte on little-endian hosts.) */
    for(u32 i = 0; i < instr_count; i++) {
        u32 w = IRCODE(ir, i);
        IRCODE(ir, i) = relic_rd32le(&w);
    }
    for(u32 i = 0; i < func_count; i++) {
        i32 t = IRFNOFF(ir, i);
        IRFNOFF(ir, i) = (i32)relic_rd32le(&t);
    }

    ir->len          = (int)instr_count;
    ir->nfuncs       = (int)func_count;
    ir->strings_len  = strings_len;
    ir->globals_size = (i32)globals_size;
    ir->nsource_files = 0;
    if(out_main_fn_id) *out_main_fn_id = (int)main_fn_id_u32;
    ir_upgrade_legacy_jumps(ir, version);

    bool has_debug = (flags & RELIC_BYTECODE_FLAG_DEBUG) != 0;
    u32  nfiles = 0;
    if(has_debug) {
        RB_TAKE32(nfiles);
        if(nfiles > RELIC_MAX_SOURCE_FILES) return false;
        if(nfiles && !relic_ir_ensure_aux(ir)) return false;
        for(u32 i = 0; i < nfiles; i++) {
            u32 plen;
            char pb[RELIC_MAX_SOURCE_PATH];
            RB_TAKE32(plen);
            if(plen >= RELIC_MAX_SOURCE_PATH) return false;
            RB_TAKE(pb, (size_t)plen);
            pb[plen] = '\0';
            relic_far_write((u8 RELIC_FAR *)ir->source_files,
                            i * (u32)RELIC_MAX_SOURCE_PATH, pb, plen + 1);
        }
    }

    #undef RB_TAKE32
    #undef RB_TAKE

    if(has_debug) {
        ir->nsource_files = (int)nfiles;
        if(!read_rle_i32_mem(&p, &remaining, ir->line_for_pc, (int)instr_count)) return false;
        if(!read_rle_i32_mem(&p, &remaining, ir->file_for_pc, (int)instr_count)) return false;
    }
    return true;
}

bool relic_bytecode_load(IRBuffer *ir, const char *path, int *out_main_fn_id) {
    FILE *f = fopen(path, "rb");
    if(!f) return false;

    u32 magic;
    u8 version, flags;
    u32 ver_major, ver_minor, ver_patch;
    u32 instr_count, func_count, strings_len, globals_size, main_fn_id_u32;

    if(!rd_u32le_f(f, &magic) || magic != RELIC_BYTECODE_MAGIC) {
        fclose(f);
        return false;
    }
    if(!rd_u8le(f, &version) || !bytecode_version_ok(version)) {
        fclose(f);
        return false;
    }
    if(!rd_u8le(f, &flags)) {
        fclose(f);
        return false;
    }
    ir->tier = (flags & RELIC_BYTECODE_FLAG_TIER)
             ? (int)((flags & RELIC_BYTECODE_TIER_MASK) >> RELIC_BYTECODE_TIER_SHIFT)
             : -1;
    if(!rd_u32le_f(f, &ver_major)) {
        fclose(f);
        return false;
    }
    if(!rd_u32le_f(f, &ver_minor)) {
        fclose(f);
        return false;
    }
    if(!rd_u32le_f(f, &ver_patch)) {
        fclose(f);
        return false;
    }

    /* Check version compatibility: warn if file was created with newer major.minor */
    if(ver_major > RELIC_VERSION_MAJOR ||
       (ver_major == RELIC_VERSION_MAJOR && ver_minor > RELIC_VERSION_MINOR)) {
        fprintf(stderr,
            "Warning: TLX file was created with Relic %u.%u.%u, "
            "but current version is %s. May not be fully compatible.\n",
            ver_major, ver_minor, ver_patch, RELIC_VERSION);
    }

    if(!rd_u32le_f(f, &instr_count)) {
        fclose(f);
        return false;
    }
    if(!rd_u32le_f(f, &func_count)) {
        fclose(f);
        return false;
    }
    if(!rd_u32le_f(f, &strings_len)) {
        fclose(f);
        return false;
    }
    if(!rd_u32le_f(f, &globals_size)) {
        fclose(f);
        return false;
    }
    if(!rd_u32le_f(f, &main_fn_id_u32)) {
        fclose(f);
        return false;
    }

    /* v5 adds a u32 heap_bytes field; earlier versions have none. */
    ir->heap_size = 0;
    if(version >= RELIC_BYTECODE_VERSION_HEAP) {
        if(!rd_u32le_f(f, &ir->heap_size)) {
            fclose(f);
            return false;
        }
    }

    if(func_count > RELIC_MAX_FUNCS || strings_len > RELIC_MAX_STRINGS) {
        fclose(f);
        return false;
    }
    if(instr_count > (u32)INT_MAX || globals_size > 0x7FFFFFFFu) {
        fclose(f);
        return false;
    }
    if(flags & RELIC_BYTECODE_FLAG_DEBUG) ir->want_dbg = 1;
    if(!relic_ir_reserve(ir, (int)instr_count)) {
        fclose(f);
        return false;
    }

    if(!rd_u32arr_far_f(f, ir->code, instr_count)) {
        fclose(f);
        return false;
    }
    if(!rd_u32arr_far_f(f, (u32 RELIC_FAR *)ir->fn_offsets, func_count)) {
        fclose(f);
        return false;
    }
    if(!fread_far(f, (u8 RELIC_FAR *)ir->strings, strings_len)) {
        fclose(f);
        return false;
    }


    ir->len = (int)instr_count;
    ir->nfuncs = (int)func_count;
    ir->strings_len = strings_len;
    ir->globals_size = (i32)globals_size;
    ir->nsource_files = 0;
    if(out_main_fn_id) *out_main_fn_id = (int)main_fn_id_u32;
    ir_upgrade_legacy_jumps(ir, version);

    if(flags & RELIC_BYTECODE_FLAG_DEBUG) {
        u32 nfiles;
        if(!rd_u32le_f(f, &nfiles) || nfiles > RELIC_MAX_SOURCE_FILES) {
            fclose(f);
        return false;
        }
        if(nfiles && !relic_ir_ensure_aux(ir)) {
            fclose(f);
        return false;
        }
        for(u32 i = 0; i < nfiles; i++) {
            u32 plen;
            char pb[RELIC_MAX_SOURCE_PATH];
            if(!rd_u32le_f(f, &plen) || plen >= RELIC_MAX_SOURCE_PATH) {
                fclose(f);
        return false;
            }
            if(fread(pb, 1, plen, f) != plen) {
                fclose(f);
        return false;
            }
            pb[plen] = '\0';
            relic_far_write((u8 RELIC_FAR *)ir->source_files,
                            i * (u32)RELIC_MAX_SOURCE_PATH, pb, plen + 1);
        }
        ir->nsource_files = (int)nfiles;
        if(!read_rle_i32_file(f, ir->line_for_pc, (int)instr_count) ||
           !read_rle_i32_file(f, ir->file_for_pc, (int)instr_count)) {
            fclose(f);
        return false;
        }
    }

    fclose(f);
    return true;
}

/* -------------------------------------------------------------------------
 * Asset table serialization
 *
 * Trailer format (appended after the bytecode payload):
 *   [all blob bytes, concatenated]
 *   for each entry:
 *     u32  name_len
 *     u8[] name  (name_len bytes, no NUL)
 *     u32  blob_offset  (from start of blob region)
 *     u32  blob_size
 *   u32  directory_offset  (offset from file start where directory begins)
 *   u32  asset_count
 *   u32  RELIC_ASSET_MAGIC
 *
 * The last 12 bytes are always (directory_offset, asset_count, magic), so a loader
 * can detect the trailer by seeking to EOF-4 and checking the magic word, then read
 * the directory_offset to jump directly to the directory.
 * ---------------------------------------------------------------------- */

bool relic_assets_write(RelicAssets *assets, FILE *f) {
    if(!assets || !f || assets->count == 0) return true;

    /* Write all blobs */
    if(assets->data_size > 0) {
        if(!fwrite_far(f, assets->data, assets->data_size))
            return false;
    }

    /* Capture file position now that blobs are written: this is the start of
     * the directory, which is what the loader expects in the footer. */
    long dir_offset = ftell(f);

    /* Write directory entries */
    for(int i = 0; i < assets->count; i++) {
        RelicAssetEntry *e = &assets->entries[i];
        u32 name_len = (u32)strlen(e->name);
        if(!wr_u32le_f(f, name_len)) return false;
        if(fwrite(e->name,         1, name_len, f) != name_len) return false;
        if(!wr_u32le_f(f, e->blob_offset)) return false;
        if(!wr_u32le_f(f, e->blob_size))   return false;
    }

    /* Footer */
    if(!wr_u32le_f(f, (u32)dir_offset))  return false;
    if(!wr_u32le_f(f, (u32)assets->count)) return false;
    if(!wr_u32le_f(f, RELIC_ASSET_MAGIC)) return false;
    return true;
}

/* Little-endian u32 read at a byte offset from a (possibly far) base. */
static u32 relic_mem_rd32le(const u8 RELIC_FAR *base, u32 off) {
    u8 b[4];
    relic_far_read(base, off, b, 4);
    return relic_rd32le(b);
}

/* Parse the asset directory from a raw memory block that is the full
 * content of a .tlx file, or the embedded payload from a bound executable.
 * In the latter case, the payload may be followed by a 24-byte bind footer,
 * so we search backwards from the end to find the asset magic.
 * `data` may be far (ia16: bound payloads are loaded into far memory), so
 * all cursor reads go through near staging buffers. */
static RelicAssets *assets_load_mem(const void RELIC_FAR *data, size_t len, int borrow);

RelicAssets *relic_assets_load_mem(const void RELIC_FAR *data, size_t len) {
    return assets_load_mem(data, len, 0);
}

RelicAssets *relic_assets_load_mem_ref(const void RELIC_FAR *data, size_t len) {
    return assets_load_mem(data, len, 1);
}

static RelicAssets *assets_load_mem(const void RELIC_FAR *data, size_t len, int borrow) {
    if(!data || len < 12) return NULL;
    const u8 RELIC_FAR *base = (const u8 RELIC_FAR*)data;

    /* Search backwards from the end for the asset magic.
     * This handles both normal .tlx files (magic at EOF) and bound executables
     * (magic followed by a 24-byte bind footer). */
    u32 count, magic, dir_offset;
    u32 search_end = (u32)len;
    u32 search_start = (len > 1024) ? (u32)len - 1024 : 0; /* Search last 1KB max */

    bool found = false;
    for(u32 pos = (u32)len - 4; pos >= search_start && pos + 4 <= (u32)len; pos--) {
        magic = relic_mem_rd32le(base, pos);
        if(magic == RELIC_ASSET_MAGIC) {
            /* Found it - read the footer fields */
            if(pos < 8) break;
            count = relic_mem_rd32le(base, pos - 4);
            if(pos < 12) break;
            dir_offset = relic_mem_rd32le(base, pos - 8);
            search_end = pos + 4; /* Asset magic is at this position */
            found = true;
            break;
        }
        if(pos == 0) break;   /* u32 loop: don't wrap */
    }

    if(!found) return NULL;
    if(count == 0 || count > RELIC_MAX_ASSETS) return NULL;
    if(dir_offset >= search_end) return NULL;

    /* File layout is: [bytecode][blob_data][directory][footer] */
    u32 dir_start = dir_offset;
    u32 dir_end   = search_end - 12;

    /* Parse directory entries forward from dir_start to dir_end */
    /* Entry format: name_len(4), name(var), blob_offset(4), blob_size(4) */
    RelicAssetEntry *tmp = calloc(count, sizeof(RelicAssetEntry));
    if(!tmp) return NULL;

    u32 p = dir_start;
    for(u32 i = 0; i < count; i++) {
        if(p + 4 > dir_end) { free(tmp); return NULL; }
        u32 nlen = relic_mem_rd32le(base, p);
        p += 4;
        if(nlen == 0 || nlen >= RELIC_MAX_ASSET_NAME) { free(tmp); return NULL; }
        if(p + nlen + 8 > dir_end) { free(tmp); return NULL; }
        relic_far_read(base, p, tmp[i].name, nlen);
        tmp[i].name[nlen] = '\0';
        p += nlen;
        tmp[i].blob_offset = relic_mem_rd32le(base, p);
        p += 4;
        tmp[i].blob_size = relic_mem_rd32le(base, p);
        p += 4;
    }

    /* Blob data comes BEFORE the directory (layout: [bytecode][blob_data][directory][footer]) */
    u32 data_size = 0;
    for(u32 i = 0; i < count; i++) data_size += tmp[i].blob_size;
    if(data_size > dir_start) {
        free(tmp);
        return NULL;
    }
    u32 blob_start = dir_start - data_size;

    RelicAssets *assets = (RelicAssets*)calloc(1, sizeof(RelicAssets));
    if(!assets) { free(tmp); return NULL; }

    if(data_size > 0 && borrow) {
        assets->data = (u8 RELIC_FAR *)relic_cfp_add(base, blob_start);
        assets->borrowed = 1;
    } else if(data_size > 0) {
        assets->data = relic_far_alloc(data_size);
        if(!assets->data) { free(tmp); free(assets); return NULL; }
        relic_far_write(assets->data, 0, relic_cfp_add(base, blob_start), data_size);
    }
    assets->data_size = data_size;
    assets->count     = (int)count;
    memcpy(assets->entries, tmp, sizeof(RelicAssetEntry) * count);
    free(tmp);

    return assets;
}

RelicAssets *relic_assets_load(const char *path) {
#ifdef RELIC_IA16
    /* Streaming variant: size_t is 16-bit on ia16, so the whole-file
     * malloc+load_mem approach used by the generic path cannot read a
     * >64K .tlx.  We only need the footer, directory and blob region -
     * each addressable independently by file offset. */
    FILE *f = fopen(path, "rb");
    if(!f) return NULL;
    if(fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long fsz = ftell(f);
    if(fsz < 12) { fclose(f); return NULL; }

    /* Scan the last <=1K backwards for the asset magic (a bound
     * executable may carry a 24-byte footer after it). */
    u32 tail = fsz > 1024 ? 1024u : (u32)fsz;
    u8  tbuf[1024];
    if(fseek(f, fsz - (long)tail, SEEK_SET) != 0 ||
       fread(tbuf, 1, tail, f) != tail) { fclose(f); return NULL; }
    u32 count = 0, dir_offset = 0;
    long magic_end = 0;
    bool found = false;
    for(long pos = (long)tail - 4; pos >= 0; pos--) {
        if(relic_rd32le(tbuf + pos) == RELIC_ASSET_MAGIC) {
            if(pos < 8) break;
            count      = relic_rd32le(tbuf + pos - 4);
            dir_offset = relic_rd32le(tbuf + pos - 8);
            magic_end  = fsz - (long)tail + pos + 4;
            found = true;
            break;
        }
    }
    if(!found || count == 0 || count > RELIC_MAX_ASSETS ||
       (long)dir_offset >= magic_end) { fclose(f); return NULL; }

    RelicAssets *assets = (RelicAssets*)calloc(1, sizeof(RelicAssets));
    if(!assets) { fclose(f); return NULL; }
    if(fseek(f, (long)dir_offset, SEEK_SET) != 0) goto fail;
    for(u32 i = 0; i < count; i++) {
        RelicAssetEntry *e = &assets->entries[i];
        u32 nlen;
        if(!rd_u32le_f(f, &nlen)) goto fail;
        if(nlen == 0 || nlen >= RELIC_MAX_ASSET_NAME) goto fail;
        if(fread(e->name, 1, nlen, f) != nlen) goto fail;
        e->name[nlen] = '\0';
        if(!rd_u32le_f(f, &e->blob_offset)) goto fail;
        if(!rd_u32le_f(f, &e->blob_size))   goto fail;
    }
    u32 data_size = 0;
    for(u32 i = 0; i < count; i++) data_size += assets->entries[i].blob_size;
    if(data_size > 0) {
        if((long)dir_offset < (long)data_size ||
           fseek(f, (long)dir_offset - (long)data_size, SEEK_SET) != 0)
            goto fail;
        assets->data = relic_far_alloc(data_size);
        if(!assets->data) goto fail;
        if(!fread_far(f, assets->data, data_size)) goto fail;
    }
    assets->data_size = data_size;
    assets->count     = (int)count;
    fclose(f);
    return assets;
fail:
    fclose(f);
    relic_assets_free(assets);
    return NULL;
#else
    FILE *f = fopen(path, "rb");
    if(!f) { return NULL; }

    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fclose(f);
    if(fsz < 12) { return NULL; }

    /* Load entire file into memory then delegate to load_mem */
    f = fopen(path, "rb");
    if(!f) { return NULL; }
    fseek(f, fsz - 4, SEEK_SET);
    u32 magic = 0;
    {
        u8 mb[4];
        if(fread(mb, 1, 4, f) != 4) {
            fclose(f); return NULL;
        }
        magic = relic_rd32le(mb);
    }
    if(magic != RELIC_ASSET_MAGIC) {
        fclose(f); return NULL;
    }

    /* Load entire file into memory then delegate to load_mem */
    rewind(f);
    u8 *buf = (u8*)malloc((size_t)fsz);
    if(!buf) { fclose(f); return NULL; }
    if((long)rd_bytes_f(f, buf, (u32)fsz) != fsz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);

    RelicAssets *assets = relic_assets_load_mem(buf, (size_t)fsz);
    free(buf);
    return assets;
#endif
}

void relic_assets_free(RelicAssets *assets) {
    if(!assets) return;
    if(!assets->borrowed) relic_far_free(assets->data);
    free(assets);
}

static int asset_strcasecmp(const char *a, const char *b) {
#if defined(_WIN32)
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

static const char *asset_basename(const char *path) {
    const char *s = path;
    for(const char *p = path; *p; p++)
        if(*p == '/' || *p == '\\') s = p + 1;
    return s;
}

const u8 RELIC_FAR *relic_assets_find(RelicAssets *assets, const char *name, u32 *out_size) {
    if(!assets || !name) return NULL;
    const char *name_base = asset_basename(name);

    /* Pass 1: exact full-path match */
    for(int i = 0; i < assets->count; i++) {
        if(strcmp(assets->entries[i].name, name) == 0) {
            if(out_size) *out_size = assets->entries[i].blob_size;
            return relic_cfp_add(assets->data, assets->entries[i].blob_offset);
        }
    }
    /* Pass 2: case-insensitive full-path match */
    for(int i = 0; i < assets->count; i++) {
        if(asset_strcasecmp(assets->entries[i].name, name) == 0) {
            if(out_size) *out_size = assets->entries[i].blob_size;
            return relic_cfp_add(assets->data, assets->entries[i].blob_offset);
        }
    }
    /* Pass 3: case-insensitive basename match (e.g. open("GAME.WAD") finds "tdoom/game.wad") */
    for(int i = 0; i < assets->count; i++) {
        if(asset_strcasecmp(asset_basename(assets->entries[i].name), name_base) == 0) {
            if(out_size) *out_size = assets->entries[i].blob_size;
            return relic_cfp_add(assets->data, assets->entries[i].blob_offset);
        }
    }
    return NULL;
}

/* Find an existing entry by name (case-insensitive). Returns index or -1. */
static int assets_find_index(RelicAssets *assets, const char *name) {
    for(int i = 0; i < assets->count; i++)
        if(asset_strcasecmp(assets->entries[i].name, name) == 0)
            return i;
    return -1;
}

/* Remove the entry at idx, compacting the contiguous blob region and fixing up
 * the offsets of any entries whose blob followed the removed one. The blob
 * region must stay gap-free because the loader reconstructs it as a single
 * contiguous span sized as the sum of all entry blob sizes (see
 * relic_assets_load_mem). */
static void assets_remove_at(RelicAssets *assets, int idx) {
    if(idx < 0 || idx >= assets->count) return;
    u32 off = assets->entries[idx].blob_offset;
    u32 sz  = assets->entries[idx].blob_size;
    if(sz > 0 && assets->data) {
        relic_far_move(assets->data, off, off + sz,
                       assets->data_size - off - sz);
        assets->data_size -= sz;
        for(int i = 0; i < assets->count; i++)
            if(assets->entries[i].blob_offset > off)
                assets->entries[i].blob_offset -= sz;
    }
    for(int i = idx; i < assets->count - 1; i++)
        assets->entries[i] = assets->entries[i + 1];
    assets->count--;
}

bool relic_assets_add_file(RelicAssets *assets, const char *host_path,
                           const char *asset_name) {
    if(!assets || !host_path || !asset_name) return false;
    if(strlen(asset_name) >= RELIC_MAX_ASSET_NAME) return false;

    /* If an asset with this name already exists, drop it first so re-packing
     * updates the asset in place rather than creating a duplicate entry. */
    int existing = assets_find_index(assets, asset_name);
    if(existing >= 0) assets_remove_at(assets, existing);

    if(assets->count >= RELIC_MAX_ASSETS) return false;

    FILE *f = fopen(host_path, "rb");
    if(!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if(sz < 0) { fclose(f); return false; }

    u32 new_offset = assets->data_size;
    u32 new_size   = (u32)sz;

    if(new_size > 0) {
#ifdef RELIC_IA16
        /* No realloc for DOS blocks: alloc a new one, copy, free old. */
        u8 RELIC_FAR *expanded =
            relic_far_alloc(assets->data_size + new_size);
        if(!expanded) { fclose(f); return false; }
        if(assets->data) {
            relic_far_write(expanded, 0, assets->data, assets->data_size);
            relic_far_free(assets->data);
        }
        assets->data = expanded;
        if(!fread_far(f, relic_fp_add(assets->data, new_offset), new_size)) {
            fclose(f); return false;
        }
        assets->data_size += new_size;
#else
        u8 *expanded = (u8*)realloc(assets->data,
                                    (size_t)(assets->data_size + new_size));
        if(!expanded) { fclose(f); return false; }
        assets->data = expanded;
        if(fread(assets->data + new_offset, 1, new_size, f) != new_size) {
            fclose(f); return false;
        }
        assets->data_size += new_size;
#endif
    }
    fclose(f);

    RelicAssetEntry *e = &assets->entries[assets->count++];
    strncpy(e->name, asset_name, RELIC_MAX_ASSET_NAME - 1);
    e->name[RELIC_MAX_ASSET_NAME - 1] = '\0';
    e->blob_offset = new_offset;
    e->blob_size   = new_size;
    return true;
}

/* Glob expansion: cross-platform recursive directory walk.
 * On POSIX we use opendir/readdir; on Windows we use FindFirstFile. */

#if !defined(RELIC_DOS)
/* Case-insensitive glob match supporting '*' (any run) and '?' (one char).
 * Used to filter directory entries by the filename part of a pattern. */
static int wildcard_match(const char *pat, const char *str) {
    while(*pat) {
        if(*pat == '*') {
            while(*pat == '*') pat++;
            if(!*pat) return 1;                 /* trailing '*' matches rest */
            for(; *str; str++)
                if(wildcard_match(pat, str)) return 1;
            return 0;
        } else if(*pat == '?') {
            if(!*str) return 0;
            pat++; str++;
        } else {
            if(tolower((unsigned char)*pat) != tolower((unsigned char)*str))
                return 0;
            pat++; str++;
        }
    }
    return *str == '\0';
}

/* True if s contains glob metacharacters ('*' or '?'). */
static int has_wildcard(const char *s) {
    for(; *s; s++) if(*s == '*' || *s == '?') return 1;
    return 0;
}
#endif /* !RELIC_DOS */

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

static int assets_add_dir(RelicAssets *assets,
                          const char *host_dir,    /* full host path of dir */
                          const char *name_prefix, /* virtual path prefix   */
                          const char *glob,        /* filename pattern      */
                          int recurse)             /* descend into subdirs  */
{
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*", host_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if(h == INVALID_HANDLE_VALUE) return 0;
    int added = 0;
    do {
        if(strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        char host_child[1024], name_child[1024];
        snprintf(host_child, sizeof host_child, "%s\\%s", host_dir, fd.cFileName);
        if(name_prefix[0])
            snprintf(name_child, sizeof name_child, "%s/%s", name_prefix, fd.cFileName);
        else
            snprintf(name_child, sizeof name_child, "%s", fd.cFileName);
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if(recurse)
                added += assets_add_dir(assets, host_child, name_child, glob, recurse);
        } else if(wildcard_match(glob, fd.cFileName)) {
            if(relic_assets_add_file(assets, host_child, name_child)) added++;
        }
    } while(FindNextFileA(h, &fd));
    FindClose(h);
    return added;
}

int relic_assets_add_glob(RelicAssets *assets, const char *pattern) {
    if(!assets || !pattern) return -1;

    /* Split pattern into directory prefix and filename/glob part.
     * e.g. "mydata/\*"      -> dir="mydata"  glob="*"
     *      "mydata/\*.rgba" -> dir="mydata"  glob="*.rgba"
     *      "foo.dat"        -> single file with no directory prefix */
    char dir[1024] = {0};
    const char *slash = NULL;
    for(const char *p = pattern; *p; p++)
        if(*p == '/' || *p == '\\') slash = p;

    const char *glob_part = slash ? slash + 1 : pattern;
    char name_prefix[1024] = {0};
    if(slash) {
        size_t dlen = (size_t)(slash - pattern);
        if(dlen >= sizeof dir) return -1;
        memcpy(dir, pattern, dlen);
        dir[dlen] = '\0';
        memcpy(name_prefix, pattern, dlen < sizeof name_prefix ? dlen : sizeof name_prefix - 1);
        for(char *p = name_prefix; *p; p++) if(*p == '\\') *p = '/';
    } else {
        dir[0] = '.'; dir[1] = '\0';
    }

    /* A wildcard filename part expands by matching filenames in the directory.
     * "*" / "*.rgba" match only the immediate directory; "**" matches every
     * file recursively through subdirectories. */
    if(has_wildcard(glob_part)) {
        int recurse = (strcmp(glob_part, "**") == 0);
        const char *match = recurse ? "*" : glob_part;
        return assets_add_dir(assets, dir, name_prefix, match, recurse);
    }

    /* Single named file: use pattern as host path, basename as asset name. */
    return relic_assets_add_file(assets, pattern, glob_part) ? 1 : -1;
}

#elif defined(RELIC_DOS) || defined(RELIC_IA16)
/* DJGPP/DOS + ia16/DOS: Asset globbing not supported - DOS filesystem
 * APIs are limited */
int relic_assets_add_glob(RelicAssets *assets, const char *pattern) {
    if(!assets || !pattern) return -1;
    /* DJGPP/DOS: only support single named files, not glob patterns */
    return relic_assets_add_file(assets, pattern, pattern) ? 1 : -1;
}

#else /* POSIX */
#  include <dirent.h>
#  include <sys/stat.h>

static int assets_add_dir(RelicAssets *assets,
                          const char *host_dir,
                          const char *name_prefix,
                          const char *glob,
                          int recurse)
{
    DIR *d = opendir(host_dir);
    if(!d) return 0;
    struct dirent *de;
    int added = 0;
    while((de = readdir(d)) != NULL) {
        if(strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char host_child[1024], name_child[1024];
        snprintf(host_child, sizeof host_child, "%s/%s", host_dir, de->d_name);
        if(name_prefix[0])
            snprintf(name_child, sizeof name_child, "%s/%s", name_prefix, de->d_name);
        else
            snprintf(name_child, sizeof name_child, "%s", de->d_name);
        struct stat st;
        if(stat(host_child, &st) != 0) continue;
        if(S_ISDIR(st.st_mode)) {
            if(recurse)
                added += assets_add_dir(assets, host_child, name_child, glob, recurse);
        } else if(S_ISREG(st.st_mode) && wildcard_match(glob, de->d_name)) {
            if(relic_assets_add_file(assets, host_child, name_child)) added++;
        }
    }
    closedir(d);
    return added;
}

int relic_assets_add_glob(RelicAssets *assets, const char *pattern) {
    if(!assets || !pattern) return -1;

    /* Split into dir prefix and filename/glob part */
    char dir[1024] = {0};
    const char *slash = strrchr(pattern, '/');
    const char *glob_part = slash ? slash + 1 : pattern;

    char name_prefix[1024] = {0};
    if(slash) {
        size_t dlen = (size_t)(slash - pattern);
        if(dlen >= sizeof dir) return -1;
        memcpy(dir, pattern, dlen);
        dir[dlen] = '\0';
        memcpy(name_prefix, pattern, dlen < sizeof name_prefix ? dlen : sizeof name_prefix - 1);
    } else {
        dir[0] = '.'; dir[1] = '\0';
    }

    /* A wildcard filename part expands by matching filenames in the directory.
     * "*" / "*.rgba" match only the immediate directory; "**" matches every
     * file recursively through subdirectories. */
    if(has_wildcard(glob_part)) {
        int recurse = (strcmp(glob_part, "**") == 0);
        const char *match = recurse ? "*" : glob_part;
        return assets_add_dir(assets, dir, name_prefix, match, recurse);
    }

    /* Single named file: use the pattern itself as the host path, and
     * just the basename as the asset name so that absolute paths and
     * paths with directory prefixes all result in a short, portable name
     * (e.g. "/abs/path/to/game.wad" -> asset name "game.wad"). */
    return relic_assets_add_file(assets, pattern, glob_part) ? 1 : -1;
}
#endif /* POSIX */
