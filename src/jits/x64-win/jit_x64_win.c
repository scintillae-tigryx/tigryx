/*
 * jit_x64_win.c
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
 * jit_x64_win.c - Whole-program JIT for Relic targeting x86-64 / Windows (MSVC).
 *
 * Layout produced by compile():
 *
 *   +-----------------------------+   <- code base (PAGE_EXECUTE_READ)
 *   | prologue (set up r12-r15)   |
 *   +-----------------------------+
 *   | jump to dispatch[entry_pc]  |
 *   +-----------------------------+
 *   | per-IR-word emitted code    |
 *   +-----------------------------+
 *   | epilogue (restore, ret)     |
 *   +-----------------------------+
 *
 * Register conventions inside the JIT'd body (Win64 fastcall):
 *
 *      r12 = RelicVM*              (callee-saved)
 *      r13 = &vm->r[0]             (callee-saved)
 *      r14 = vm->mem               (callee-saved)
 *      r15 = vm->fp (32-bit value) (callee-saved)
 *      rbx = vm->sp (32-bit value) (callee-saved) [pinned to avoid memory churn]
 *      rbp = saved host rsp at top of body (used to unwind on HALT / !running)
 *
 *      rax, rcx, rdx, r8-r11 = scratch
 *
 * Stack frame after prologue:
 *      [rsp+0..32)  = shadow space for C calls
 *      [rsp+32..40) = alignment padding (8 bytes)
 *      Total frame: 8 pushes (64) + 40 = 104 bytes
 *      rsp on entry was 8 mod 16 (caller's CALL pushed 8-byte ret addr).
 *      8-64-40 = -96 = 0 mod 16. Aligned for further CALLs.
 *
 * Per-IR-function frame (one mini-prologue/epilogue per JIT'd function):
 *      entry: sub rsp, 40                ; 32 shadow + 8 align (rsp arrives mod16=8)
 *      RET:   add rsp, 40 ; ret
 * This lets OP_CALL be a real x86-64 `call rel32` (so the CPU return
 * address stack predicts the matching `ret`), giving a large speedup for
 * call-heavy code (recursive Fibonacci, matrix multiply, sorting).
 *
 * Logical-register access:
 *      r0      always 0 - reads emit `xor reg, reg`, writes are dropped.
 *      r1..r13 live in [r13 + 4*N] as 32-bit slots in vm->r[].
 *      r14     aliases vm->fp - reads/writes go through r15d.
 *      r15     aliases vm->sp - reads/writes go through ebx.
 *
 * Build guard: real codegen is only enabled on a 64-bit Windows build.
 */
#include "tigryx.h"
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#if defined(_WIN64)
#  define X64_WIN_HAVE_TARGET 1
#endif

#if X64_WIN_HAVE_TARGET
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

struct RelicJitCode {
    void   *code;
    size_t  code_sz;
    void  **dispatch;
    size_t  dispatch_len;
    int    (*entry)(RelicVM *vm);
};

#if !X64_WIN_HAVE_TARGET
static bool x64_win_compile(IRBuffer *ir, RelicJitCode **out_code,
                            char *err, int err_sz) {
    (void)ir; (void)out_code;
    const char *msg = "x64-win JIT requires a 64-bit Windows build";
    if(err && err_sz > 0) {
        int n = (int)strlen(msg);
        if(n >= err_sz) n = err_sz - 1;
        memcpy(err, msg, n); err[n] = 0;
    }
    return false;
}
static int  x64_win_run(RelicJitCode *c, RelicVM *vm){ (void)c;(void)vm; return 1; }
static void x64_win_free(RelicJitCode *c)            { (void)c; }

#else /* X64_WIN_HAVE_TARGET */

/* =========================================================================
 *                            EMITTER
 * ========================================================================= */

typedef enum {
    RELOC_REL32_AT_PC,
} RelocKind;

typedef struct {
    uint32_t  jit_off;
    int32_t   target_pc;
    RelocKind kind;
} Reloc;

typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    int      oom;

    int32_t *pc_to_jit;
    size_t   pc_count;

    Reloc   *relocs;
    size_t   relocs_len;
    size_t   relocs_cap;

    uintptr_t addr_step1;
    uintptr_t addr_dispatch_table;
    int32_t   epilogue_off;

    /* ---- BB-local logical-register cache. ----
     * Within a basic block, logical regs r1..r13 (the 13 GP slots in
     * vm->r[]) can be kept "live" in a small set of physical registers
     * instead of being spilled to memory after every op. The cache is
     * flushed at every BB boundary (branch targets, terminators, step1
     * fallback) so the rest of the JIT (and step1, and external
     * dispatch) sees a consistent vm->r[] in memory.
     *
     * Slots are 5 of the callee-saved / scratch registers that the JIT
     * does NOT otherwise touch: rdi, rsi, r8, r9, r10. r8/r9/r10 are
     * Win64 volatile but every C call (step1) is preceded by a full
     * cache spill, so their volatility is harmless.
     */
    /* A logical reg lives in at most ONE cache (GP or XMM) at a time, or
     * in memory. rc_dirty[L]==1 means whichever cache currently holds L
     * has a newer value than vm->r[L]. Float ops cache via XMM, int ops
     * via GP; cross-domain reads movd between them. */
    int8_t  rc_log_at[5];        /* GP slot i  -> logical L (or -1 free)     */
    int8_t  rc_slot_of_log[16];  /* logical L -> GP slot   (or -1)           */
    int8_t  rc_xmm_log_at[4];    /* XMM slot j -> logical L (or -1 free)     */
    int8_t  rc_xmm_slot_of_log[16]; /* logical L -> XMM slot (or -1)         */
    uint8_t rc_dirty[16];        /* 1 if cached value newer than memory      */
    uint8_t rc_next_slot;        /* GP round-robin eviction pointer          */
    uint8_t rc_next_xmm_slot;    /* XMM round-robin eviction pointer         */
} Emitter;

/* GP physical regs used as cache slots. Indexed by rc_log_at[]. */
#define RC_CACHE_SLOTS 5
static const uint8_t RC_CACHE_PHYS[RC_CACHE_SLOTS] = {
    7,  /* RDI */
    6,  /* RSI */
    8,  /* R8  */
    9,  /* R9  */
    10, /* R10 */
};

/* XMM cache slots. xmm0/xmm1 are reserved as op-scratch (every
 * FP emitter writes to them); xmm2..xmm5 are volatile on Win64 so the
 * prologue does not need to preserve them, and every C call (step1) is
 * preceded by a full cache spill so their volatility is harmless. */
#define RC_XMM_SLOTS 4
static const uint8_t RC_XMM_PHYS[RC_XMM_SLOTS] = { 2, 3, 4, 5 };

static int em_init(Emitter *e, size_t pc_count) {
    memset(e, 0, sizeof(*e));
    e->cap       = 1024;
    e->buf       = (uint8_t*)malloc(e->cap);
    e->pc_count  = pc_count;
    e->pc_to_jit = (int32_t*)malloc(sizeof(int32_t) * (pc_count + 1));
    e->relocs_cap = 64;
    e->relocs    = (Reloc*)malloc(sizeof(Reloc) * e->relocs_cap);
    if(!e->buf || !e->pc_to_jit || !e->relocs) { e->oom = 1; return 0; }
    for(size_t i = 0; i < pc_count + 1; i++) e->pc_to_jit[i] = -1;
    e->epilogue_off = -1;
    for(int i = 0; i < RC_CACHE_SLOTS; i++) e->rc_log_at[i] = -1;
    for(int i = 0; i < RC_XMM_SLOTS;   i++) e->rc_xmm_log_at[i] = -1;
    for(int i = 0; i < 16; i++) {
        e->rc_slot_of_log[i]     = -1;
        e->rc_xmm_slot_of_log[i] = -1;
        e->rc_dirty[i]           = 0;
    }
    e->rc_next_slot     = 0;
    e->rc_next_xmm_slot = 0;
    return 1;
}

static void em_free(Emitter *e) {
    free(e->buf);       e->buf = NULL;
    free(e->pc_to_jit); e->pc_to_jit = NULL;
    free(e->relocs);    e->relocs = NULL;
}

static int em_reserve(Emitter *e, size_t extra) {
    if(e->oom) return 0;
    if(e->len + extra <= e->cap) return 1;
    size_t nc = e->cap ? e->cap * 2 : 1024;
    while(nc < e->len + extra) nc *= 2;
    uint8_t *nb = (uint8_t*)realloc(e->buf, nc);
    if(!nb) { e->oom = 1; return 0; }
    e->buf = nb;
    e->cap = nc;
    return 1;
}

static void em_u8 (Emitter *e, uint8_t  v) {
    if(em_reserve(e, 1)) e->buf[e->len++] = v;
}
static void em_u32(Emitter *e, uint32_t v) {
    if(em_reserve(e, 4)) {
        memcpy(e->buf + e->len, &v, 4);
        e->len += 4;
    }
}
static void em_u64(Emitter *e, uint64_t v) {
    if(em_reserve(e, 8)) {
        memcpy(e->buf + e->len, &v, 8);
        e->len += 8;
    }
}

static void em_record_pc(Emitter *e, int32_t pc) {
    if(pc >= 0 && (size_t)pc < e->pc_count) e->pc_to_jit[pc] = (int32_t)e->len;
}

static void em_add_reloc(Emitter *e, uint32_t at_off, int32_t target_pc, RelocKind k) {
    if(e->oom) return;
    if(e->relocs_len == e->relocs_cap) {
        size_t nc = e->relocs_cap * 2;
        Reloc *nb = (Reloc*)realloc(e->relocs, sizeof(Reloc) * nc);
        if(!nb) { e->oom = 1; return; }
        e->relocs = nb; e->relocs_cap = nc;
    }
    e->relocs[e->relocs_len].jit_off   = at_off;
    e->relocs[e->relocs_len].target_pc = target_pc;
    e->relocs[e->relocs_len].kind      = k;
    e->relocs_len++;
}

/* =========================================================================
 *                       REX & ModR/M HELPERS
 *
 * REX prefix bits:
 *   REX_W (0x48) - 64-bit operand size
 *   REX_R (0x44) - extends ModR/M.reg to access r8-r15
 *   REX_X (0x42) - extends SIB.index to access r8-r15
 *   REX_B (0x41) - extends ModR/M.r/m or SIB.base to access r8-r15
 * ========================================================================= */
#define REX_W 0x48
#define REX_R 0x44
#define REX_X 0x42
#define REX_B 0x41

#define R_RAX 0
#define R_RCX 1
#define R_RDX 2
#define R_RBX 3
#define R_RSP 4
#define R_RBP 5
#define R_RSI 6
#define R_RDI 7
#define R_R8  8
#define R_R9  9
#define R_R10 10
#define R_R11 11
#define R_R12 12
#define R_R13 13
#define R_R14 14
#define R_R15 15

/* Build ModR/M byte: mod(2) | reg(3) | r/m(3) - using LOW 3 bits only. */
static uint8_t modrm(uint8_t mod, uint8_t reg, uint8_t rm) {
    return (uint8_t)(((mod & 3) << 6) | ((reg & 7) << 3) | (rm & 7));
}
/* Build SIB byte: scale(2) | index(3) | base(3) */
static uint8_t sib(uint8_t scale, uint8_t index, uint8_t base) {
    return (uint8_t)(((scale & 3) << 6) | ((index & 7) << 3) | (base & 7));
}

/* =========================================================================
 *                       MEMORY ADDRESSING HELPERS
 *
 * These emit complete instructions for common addressing patterns,
 * eliminating the bug-prone manual byte sequences.
 * ========================================================================= */

/* Emit `mov reg32, [r12 + disp32]` (r12 base requires SIB).
 * reg can be in r8-r15 (we add REX.R). */
static void emit_mov_r32_r12disp(Emitter *e, uint8_t reg, int32_t disp) {
    uint8_t rex = REX_B;
    if(reg >= 8) rex |= REX_R;
    em_u8(e, rex);
    em_u8(e, 0x8B);
    em_u8(e, modrm(2, reg, 4));   /* mod=10 (disp32), r/m=100 (SIB) */
    em_u8(e, sib(0, 4, 4));       /* index=100 (none), base=100 (r12 with REX.B) */
    em_u32(e, (uint32_t)disp);
}

/* Emit `mov [r12 + disp32], reg32` (r12 base requires SIB). */
static void emit_mov_r12disp_r32(Emitter *e, uint8_t reg, int32_t disp) {
    uint8_t rex = REX_B;
    if(reg >= 8) rex |= REX_R;
    em_u8(e, rex);
    em_u8(e, 0x89);
    em_u8(e, modrm(2, reg, 4));
    em_u8(e, sib(0, 4, 4));
    em_u32(e, (uint32_t)disp);
}

/* Emit `mov dword ptr [r12 + disp32], imm32`. */
static void emit_mov_r12disp_imm32(Emitter *e, int32_t disp, uint32_t imm) {
    em_u8(e, REX_B);
    em_u8(e, 0xC7);
    em_u8(e, modrm(2, 0, 4));
    em_u8(e, sib(0, 4, 4));
    em_u32(e, (uint32_t)disp);
    em_u32(e, imm);
}

/* Emit `mov byte ptr [r12 + disp32], imm8`. */
static void emit_mov_r12disp_imm8(Emitter *e, int32_t disp, uint8_t imm) {
    em_u8(e, REX_B);
    em_u8(e, 0xC6);
    em_u8(e, modrm(2, 0, 4));
    em_u8(e, sib(0, 4, 4));
    em_u32(e, (uint32_t)disp);
    em_u8(e, imm);
}

/* Emit `cmp byte ptr [r12 + disp32], imm8`. */
static void emit_cmp_byte_r12disp_imm8(Emitter *e, int32_t disp, uint8_t imm) {
    em_u8(e, REX_B);
    em_u8(e, 0x80);
    em_u8(e, modrm(2, 7, 4));     /* /7 = CMP */
    em_u8(e, sib(0, 4, 4));
    em_u32(e, (uint32_t)disp);
    em_u8(e, imm);
}

/* Emit `lea reg64, [r12 + disp32]`. */
static void emit_lea_r64_r12disp(Emitter *e, uint8_t reg, int32_t disp) {
    uint8_t rex = REX_W | REX_B;
    if(reg >= 8) rex |= REX_R;
    em_u8(e, rex);
    em_u8(e, 0x8D);
    em_u8(e, modrm(2, reg, 4));
    em_u8(e, sib(0, 4, 4));
    em_u32(e, (uint32_t)disp);
}

/* Emit `mov reg64, [r12 + disp32]` (64-bit load, e.g. for vm->mem pointer). */
static void emit_mov_r64_r12disp(Emitter *e, uint8_t reg, int32_t disp) {
    uint8_t rex = REX_W | REX_B;
    if(reg >= 8) rex |= REX_R;
    em_u8(e, rex);
    em_u8(e, 0x8B);
    em_u8(e, modrm(2, reg, 4));
    em_u8(e, sib(0, 4, 4));
    em_u32(e, (uint32_t)disp);
}

/* Emit `mov reg32, [r13 + disp8]` (vm->r[N] access). */
static void emit_mov_r32_r13disp8(Emitter *e, uint8_t reg, int8_t disp) {
    uint8_t rex = REX_B;
    if(reg >= 8) rex |= REX_R;
    em_u8(e, rex);
    em_u8(e, 0x8B);
    em_u8(e, modrm(1, reg, 5));   /* mod=01, r/m=101 (r13 with REX.B) */
    em_u8(e, (uint8_t)disp);
}

/* Emit `mov [r13 + disp8], reg32`. */
static void emit_mov_r13disp8_r32(Emitter *e, uint8_t reg, int8_t disp) {
    uint8_t rex = REX_B;
    if(reg >= 8) rex |= REX_R;
    em_u8(e, rex);
    em_u8(e, 0x89);
    em_u8(e, modrm(1, reg, 5));
    em_u8(e, (uint8_t)disp);
}

/* ---- Same three ops but indexed by rbx (pinned vm->sp) instead of rcx. ---- */
/* Emit `mov reg32, [r14 + rbx]` - top-of-stack read via pinned sp. */
static void emit_mov_r32_r14rbx(Emitter *e, uint8_t reg) {
    uint8_t rex = REX_B;
    if(reg >= 8) rex |= REX_R;
    em_u8(e, rex);
    em_u8(e, 0x8B);
    em_u8(e, modrm(0, reg, 4));
    em_u8(e, sib(0, R_RBX, R_R14 & 7));
}
/* Emit `mov [r14 + rbx], reg32`. */
static void emit_mov_r14rbx_r32(Emitter *e, uint8_t reg) {
    uint8_t rex = REX_B;
    if(reg >= 8) rex |= REX_R;
    em_u8(e, rex);
    em_u8(e, 0x89);
    em_u8(e, modrm(0, reg, 4));
    em_u8(e, sib(0, R_RBX, R_R14 & 7));
}
/* Emit `mov dword ptr [r14 + rbx], imm32`. */
static void emit_mov_r14rbx_imm32(Emitter *e, uint32_t imm) {
    em_u8(e, REX_B);
    em_u8(e, 0xC7);
    em_u8(e, modrm(0, 0, 4));
    em_u8(e, sib(0, R_RBX, R_R14 & 7));
    em_u32(e, imm);
}

/* Emit `add/sub ebx, imm8` (sign-extended). subop=0 ADD, 5 SUB. */
static void emit_addsub_ebx_imm8(Emitter *e, uint8_t subop, int8_t imm) {
    em_u8(e, 0x83);
    em_u8(e, modrm(3, subop, R_RBX));
    em_u8(e, (uint8_t)imm);
}
/* Emit `add/sub ebx, imm32`. */
static void emit_addsub_ebx_imm32(Emitter *e, uint8_t subop, int32_t imm) {
    em_u8(e, 0x81);
    em_u8(e, modrm(3, subop, R_RBX));
    em_u32(e, (uint32_t)imm);
}
/* Emit `add/sub r15d, imm8/imm32`. subop=0 ADD, 5 SUB. r15 needs REX.B. */
static void emit_addsub_r15d_imm(Emitter *e, uint8_t subop, int32_t imm) {
    em_u8(e, REX_B);
    if(imm >= -128 && imm <= 127) {
        em_u8(e, 0x83);
        em_u8(e, modrm(3, subop, R_R15 & 7));
        em_u8(e, (uint8_t)(int8_t)imm);
    } else {
        em_u8(e, 0x81);
        em_u8(e, modrm(3, subop, R_R15 & 7));
        em_u32(e, (uint32_t)imm);
    }
}
/* Emit `and/or ebx, imm32` and `and/or r15d, imm32`. subop=4 AND, 1 OR. */
static void emit_andor_ebx_imm32(Emitter *e, uint8_t subop, int32_t imm) {
    em_u8(e, 0x81);
    em_u8(e, modrm(3, subop, R_RBX));
    em_u32(e, (uint32_t)imm);
}
static void emit_andor_r15d_imm32(Emitter *e, uint8_t subop, int32_t imm) {
    em_u8(e, REX_B);
    em_u8(e, 0x81);
    em_u8(e, modrm(3, subop, R_R15 & 7));
    em_u32(e, (uint32_t)imm);
}

/* Emit `op r32, imm` (ADD/SUB/AND/OR) on a register. subop encoding:
 * 0=ADD, 5=SUB, 4=AND, 1=OR. Used to mutate cache slots in place when the
 * dst==a peephole hits a cached register. */
static void emit_op_r32_imm(Emitter *e, uint8_t subop, uint8_t reg, int32_t imm) {
    if(reg >= 8) em_u8(e, REX_B);
    if(imm >= -128 && imm <= 127) {
        em_u8(e, 0x83);
        em_u8(e, modrm(3, subop, reg & 7));
        em_u8(e, (uint8_t)(int8_t)imm);
    } else {
        em_u8(e, 0x81);
        em_u8(e, modrm(3, subop, reg & 7));
        em_u32(e, (uint32_t)imm);
    }
}

/* Emit RMW `op dword ptr [r13 + disp8], imm` for ADD/SUB/AND/OR.
 * subop: 0=ADD, 5=SUB, 4=AND, 1=OR. Used by emit_bin_imm dst==a peephole. */
static void emit_rmw_r13disp8_imm(Emitter *e, uint8_t subop,
                                  int8_t disp, int32_t imm) {
    em_u8(e, REX_B);   /* r13 base needs REX.B */
    if(imm >= -128 && imm <= 127) {
        em_u8(e, 0x83);
        em_u8(e, modrm(1, subop, 5));
        em_u8(e, (uint8_t)disp);
        em_u8(e, (uint8_t)(int8_t)imm);
    } else {
        em_u8(e, 0x81);
        em_u8(e, modrm(1, subop, 5));
        em_u8(e, (uint8_t)disp);
        em_u32(e, (uint32_t)imm);
    }
}

/* Emit `sub/add rsp, imm8` (REX.W). subop=5 SUB, 0 ADD. */
static void emit_addsub_rsp_imm8(Emitter *e, uint8_t subop, int8_t imm) {
    em_u8(e, REX_W);
    em_u8(e, 0x83);
    em_u8(e, modrm(3, subop, R_RSP));
    em_u8(e, (uint8_t)imm);
}
/* Emit `mov rsp, rbp` (REX.W). */
static void emit_mov_rsp_rbp(Emitter *e) {
    em_u8(e, REX_W);
    em_u8(e, 0x89);
    em_u8(e, modrm(3, R_RBP, R_RSP));   /* MOV r/m, r : r/m=rsp, r=rbp */
}
/* Emit `mov rbp, rsp` (REX.W). */
static void emit_mov_rbp_rsp(Emitter *e) {
    em_u8(e, REX_W);
    em_u8(e, 0x89);
    em_u8(e, modrm(3, R_RSP, R_RBP));
}
/* Emit `ret`. */
static void emit_ret(Emitter *e) { em_u8(e, 0xC3); }

/* Emit `mov reg32, [r14 + rcx + disp32]` - memory load. */
static void emit_mov_r32_r14rcx_disp(Emitter *e, uint8_t reg, int32_t disp) {
    uint8_t rex = REX_B;
    if(reg >= 8) rex |= REX_R;
    em_u8(e, rex);
    em_u8(e, 0x8B);
    em_u8(e, modrm(2, reg, 4));
    em_u8(e, sib(0, R_RCX, R_R14 & 7));
    em_u32(e, (uint32_t)disp);
}

/* Emit `mov [r14 + rcx + disp32], reg32`. */
static void emit_mov_r14rcx_disp_r32(Emitter *e, uint8_t reg, int32_t disp) {
    uint8_t rex = REX_B;
    if(reg >= 8) rex |= REX_R;
    em_u8(e, rex);
    em_u8(e, 0x89);
    em_u8(e, modrm(2, reg, 4));
    em_u8(e, sib(0, R_RCX, R_R14 & 7));
    em_u32(e, (uint32_t)disp);
}

/* Emit `movzx reg32, byte ptr [r14 + rcx + disp32]`. */
static void emit_movzx8_r32_r14rcx_disp(Emitter *e, uint8_t reg, int32_t disp) {
    uint8_t rex = REX_B;
    if(reg >= 8) rex |= REX_R;
    em_u8(e, rex);
    em_u8(e, 0x0F); em_u8(e, 0xB6);
    em_u8(e, modrm(2, reg, 4));
    em_u8(e, sib(0, R_RCX, R_R14 & 7));
    em_u32(e, (uint32_t)disp);
}

/* Emit `movzx reg32, word ptr [r14 + rcx + disp32]`. */
static void emit_movzx16_r32_r14rcx_disp(Emitter *e, uint8_t reg, int32_t disp) {
    uint8_t rex = REX_B;
    if(reg >= 8) rex |= REX_R;
    em_u8(e, rex);
    em_u8(e, 0x0F); em_u8(e, 0xB7);
    em_u8(e, modrm(2, reg, 4));
    em_u8(e, sib(0, R_RCX, R_R14 & 7));
    em_u32(e, (uint32_t)disp);
}

/* Emit `mov word ptr [r14 + rcx + disp32], reg16`. */
static void emit_mov16_r14rcx_disp_r32(Emitter *e, uint8_t reg, int32_t disp) {
    em_u8(e, 0x66);    /* operand-size prefix for 16-bit */
    uint8_t rex = REX_B;
    if(reg >= 8) rex |= REX_R;
    em_u8(e, rex);
    em_u8(e, 0x89);
    em_u8(e, modrm(2, reg, 4));
    em_u8(e, sib(0, R_RCX, R_R14 & 7));
    em_u32(e, (uint32_t)disp);
}

/* Emit `mov byte ptr [r14 + rcx + disp32], reg8`. Only AL/CL/DL/BL safe. */
static void emit_mov8_r14rcx_disp_r32(Emitter *e, uint8_t reg, int32_t disp) {
    /* For low 4 regs we don't need REX. We always need REX.B for r14 base.
     * Adding REX automatically maps al/cl/dl/bl correctly (no spl/bpl issue
     * because we only use AL/CL here). */
    uint8_t rex = REX_B;
    if(reg >= 8) rex |= REX_R;
    em_u8(e, rex);
    em_u8(e, 0x88);
    em_u8(e, modrm(2, reg, 4));
    em_u8(e, sib(0, R_RCX, R_R14 & 7));
    em_u32(e, (uint32_t)disp);
}

/* =========================================================================
 *                LOGICAL <-> PHYSICAL REGISTER HELPERS
 *
 * `phys` is the physical register encoding (R_RAX..R_R15).
 * For logical r0: emits `xor reg, reg`.
 * For r1..r13: [r13 + 4*log] (32-bit slot).
 * For r14 (fp): aliases r15 (pinned).
 * For r15 (sp): aliases [r12 + offsetof(sp)].
 * ========================================================================= */
/* Generic 32-bit reg-to-reg move: `mov dst, src` (handles REX for r8..r15). */
static void emit_mov_r32_r32(Emitter *e, uint8_t dst, uint8_t src) {
    if(dst == src) return;
    uint8_t rex = 0;
    if(src >= 8) rex |= REX_R;
    if(dst >= 8) rex |= REX_B;
    if(rex) em_u8(e, rex);
    em_u8(e, 0x89);
    em_u8(e, modrm(3, src & 7, dst & 7));
}

/* ---- SSE / XMM memory helpers. ----
 * All assume xmm regs in 0..7 (no REX.R) and use [r13 + disp8] for
 * vm->r[N] access (REX.B for r13 base + rm=101 with mod=01). */

/* movss xmm, [r13 + disp8]      ;  F3 41 0F 10 /r disp8 */
static void emit_movss_xmm_r13disp8(Emitter *e, uint8_t xmm, int8_t disp) {
    em_u8(e, 0xF3);
    em_u8(e, REX_B);
    em_u8(e, 0x0F); em_u8(e, 0x10);
    em_u8(e, modrm(1, xmm & 7, 5));
    em_u8(e, (uint8_t)disp);
}

/* movss [r13 + disp8], xmm      ;  F3 41 0F 11 /r disp8 */
static void emit_movss_r13disp8_xmm(Emitter *e, int8_t disp, uint8_t xmm) {
    em_u8(e, 0xF3);
    em_u8(e, REX_B);
    em_u8(e, 0x0F); em_u8(e, 0x11);
    em_u8(e, modrm(1, xmm & 7, 5));
    em_u8(e, (uint8_t)disp);
}

/* ---- f64 (d-bank) memory helpers. ----
 * The d bank lives in vm->d[]; r13 points at vm->r[0], so a d slot is
 * [r13 + (offsetof(d) - offsetof(r)) + 8*n]. That offset exceeds disp8,
 * so all d access uses mod=10 disp32 (r/m=101 is [r13+disp32] with
 * mod=10; only mod=00 means RIP-relative). The d bank is NOT cached:
 * every op is load-op-store through scratch xmm0/xmm1, which keeps the
 * rc_* invalidation machinery untouched. d0 is a real register (the
 * d-bank has no zero reg), so no early-out on dst==0 for d writes. */

#define D_BASE_OFF \
    ((int32_t)offsetof(RelicVM, d) - (int32_t)offsetof(RelicVM, r))
static int32_t d_disp(uint8_t n) { return D_BASE_OFF + 8 * (int32_t)n; }

/* movsd xmm, [r13 + disp32]     ;  F2 41 0F 10 /r disp32 */
static void emit_movsd_xmm_r13disp32(Emitter *e, uint8_t xmm, int32_t disp) {
    em_u8(e, 0xF2);
    em_u8(e, REX_B);
    em_u8(e, 0x0F); em_u8(e, 0x10);
    em_u8(e, modrm(2, xmm & 7, 5));
    em_u32(e, (uint32_t)disp);
}

/* movsd [r13 + disp32], xmm     ;  F2 41 0F 11 /r disp32 */
static void emit_movsd_r13disp32_xmm(Emitter *e, int32_t disp, uint8_t xmm) {
    em_u8(e, 0xF2);
    em_u8(e, REX_B);
    em_u8(e, 0x0F); em_u8(e, 0x11);
    em_u8(e, modrm(2, xmm & 7, 5));
    em_u32(e, (uint32_t)disp);
}

/* mov reg32, [r13 + disp32] / mov [r13 + disp32], reg32 — d[] halves,
 * used by DBITS/DFROMBITS and the DLDC literal store. */
static void emit_mov_r32_r13disp32(Emitter *e, uint8_t reg, int32_t disp) {
    em_u8(e, REX_B);
    em_u8(e, 0x8B);
    em_u8(e, modrm(2, reg & 7, 5));
    em_u32(e, (uint32_t)disp);
}
static void emit_mov_r13disp32_r32(Emitter *e, int32_t disp, uint8_t reg) {
    em_u8(e, REX_B);
    em_u8(e, 0x89);
    em_u8(e, modrm(2, reg & 7, 5));
    em_u32(e, (uint32_t)disp);
}
/* mov dword [r13 + disp32], imm32   ;  C7 /0 disp32 imm32 */
static void emit_mov_r13disp32_imm32(Emitter *e, int32_t disp, uint32_t imm) {
    em_u8(e, REX_B);
    em_u8(e, 0xC7);
    em_u8(e, modrm(2, 0, 5));
    em_u32(e, (uint32_t)disp);
    em_u32(e, imm);
}
/* mov r64, [r13 + disp32] / mov [r13 + disp32], r64 — DMOV whole-word copy */
static void emit_mov_r64_r13disp32(Emitter *e, uint8_t reg, int32_t disp) {
    em_u8(e, REX_W | REX_B);
    em_u8(e, 0x8B);
    em_u8(e, modrm(2, reg & 7, 5));
    em_u32(e, (uint32_t)disp);
}
static void emit_mov_r13disp32_r64(Emitter *e, int32_t disp, uint8_t reg) {
    em_u8(e, REX_W | REX_B);
    em_u8(e, 0x89);
    em_u8(e, modrm(2, reg & 7, 5));
    em_u32(e, (uint32_t)disp);
}
/* RMW dword [r13 + disp32], imm32   ;  81 /subop disp32 imm32
 * subop: 4 = AND (DFABS), 6 = XOR (DNEG). */
static void emit_rmw_r13disp32_imm32(Emitter *e, uint8_t subop,
                                     int32_t disp, uint32_t imm) {
    em_u8(e, REX_B);
    em_u8(e, 0x81);
    em_u8(e, modrm(2, subop, 5));
    em_u32(e, (uint32_t)disp);
    em_u32(e, imm);
}

/* movsd xmm, [r14 + rcx + disp32] / movsd [r14 + rcx + disp32], xmm —
 * DLD/DST guest-memory access, same addressing as LD32/ST32 (rcx holds
 * the zero-extended address reg). */
static void emit_movsd_xmm_r14rcx(Emitter *e, uint8_t xmm, int32_t disp) {
    em_u8(e, 0xF2);
    em_u8(e, REX_B);
    em_u8(e, 0x0F); em_u8(e, 0x10);
    em_u8(e, modrm(2, xmm, 4));
    em_u8(e, sib(0, R_RCX, R_R14 & 7));
    em_u32(e, (uint32_t)disp);
}
static void emit_movsd_r14rcx_xmm(Emitter *e, uint8_t xmm, int32_t disp) {
    em_u8(e, 0xF2);
    em_u8(e, REX_B);
    em_u8(e, 0x0F); em_u8(e, 0x11);
    em_u8(e, modrm(2, xmm, 4));
    em_u8(e, sib(0, R_RCX, R_R14 & 7));
    em_u32(e, (uint32_t)disp);
}

/* movaps xmm_dst, xmm_src       ;  0F 28 /r (reg-form)  (xmm 0..7 only) */
static void emit_movaps_xmm_xmm(Emitter *e, uint8_t dst, uint8_t src) {
    if(dst == src) return;
    em_u8(e, 0x0F); em_u8(e, 0x28);
    em_u8(e, modrm(3, dst & 7, src & 7));
}

/* Forward decls for cross-domain (GP <-> XMM) movd. The implementations
 * live further down with the other SSE emitters; we only need the
 * prototypes here so the cache helpers can spill XMM slots and so
 * emit_load/store_logical can bridge between caches. */
static void emit_movd_xmm_r32(Emitter *e, uint8_t xmm_dst, uint8_t r32_src);
static void emit_movd_r32_xmm(Emitter *e, uint8_t r32_dst, uint8_t xmm_src);

/* ---- Register-cache helpers (BB-local allocator). ---- */

/* Spill GP cache slot `slot` (writing back to memory if dirty) and clear
 * its mapping. After this the slot is free. */
static void rc_spill_slot(Emitter *e, int slot) {
    int8_t log = e->rc_log_at[slot];
    if(log < 0) return;
    if(e->rc_dirty[log]) {
        emit_mov_r13disp8_r32(e, RC_CACHE_PHYS[slot], (int8_t)(4 * log));
        e->rc_dirty[log] = 0;
    }
    e->rc_slot_of_log[log] = -1;
    e->rc_log_at[slot]     = -1;
}

/* Spill XMM cache slot `slot` and clear its mapping. */
static void rc_spill_xmm_slot(Emitter *e, int slot) {
    int8_t log = e->rc_xmm_log_at[slot];
    if(log < 0) return;
    if(e->rc_dirty[log]) {
        emit_movss_r13disp8_xmm(e, (int8_t)(4 * log), RC_XMM_PHYS[slot]);
        e->rc_dirty[log] = 0;
    }
    e->rc_xmm_slot_of_log[log] = -1;
    e->rc_xmm_log_at[slot]     = -1;
}

/* Flush ALL cached logicals (GP and XMM) to memory and clear the cache.
 * Called at every BB boundary (branch target, terminator op, step1
 * fallback). */
static void rc_spill_all(Emitter *e) {
    for(int i = 0; i < RC_CACHE_SLOTS; i++) rc_spill_slot(e, i);
    for(int i = 0; i < RC_XMM_SLOTS;   i++) rc_spill_xmm_slot(e, i);
    e->rc_next_slot     = 0;
    e->rc_next_xmm_slot = 0;
}

/* Spill+invalidate just logical `log` from whichever cache holds it (if
 * any). Used by the RMW peephole, which mutates [r13+4*log] directly and
 * would otherwise leave a stale cache entry shadowing the new memory
 * value. Also used to break the GP/XMM invariant before installing a
 * new value in the other cache. */
static void rc_evict_log(Emitter *e, uint8_t log) {
    int8_t slot = e->rc_slot_of_log[log];
    if(slot >= 0) { rc_spill_slot(e, slot); return; }
    int8_t xslot = e->rc_xmm_slot_of_log[log];
    if(xslot >= 0) rc_spill_xmm_slot(e, xslot);
}

/* Drop any cache entry for `log` WITHOUT spilling. Caller is about to
 * overwrite `log` with a new value so the old cached value is dead. */
static void rc_drop_log_nospill(Emitter *e, uint8_t log) {
    int8_t slot = e->rc_slot_of_log[log];
    if(slot >= 0) {
        e->rc_log_at[slot]     = -1;
        e->rc_slot_of_log[log] = -1;
    }
    int8_t xslot = e->rc_xmm_slot_of_log[log];
    if(xslot >= 0) {
        e->rc_xmm_log_at[xslot]    = -1;
        e->rc_xmm_slot_of_log[log] = -1;
    }
    e->rc_dirty[log] = 0;
}

/* Allocate a GP cache slot, or return -1 if all are in use. We deliberately
 * do NOT evict on full: eviction costs (spill victim + install new) =
 * 2 ops, while writing the new value straight to memory is only 1 op,
 * so falling back to memory is strictly better when the cache is
 * saturated. Hot inner-loop variables that are written first will keep
 * their slots; later "transient" values just go to memory. */
static int rc_alloc_slot(Emitter *e) {
    for(int i = 0; i < RC_CACHE_SLOTS; i++) {
        if(e->rc_log_at[i] < 0) return i;
    }
    return -1;
}

/* Same policy for XMM: return -1 on full and let caller write to memory. */
static int rc_alloc_xmm_slot(Emitter *e) {
    for(int i = 0; i < RC_XMM_SLOTS; i++) {
        if(e->rc_xmm_log_at[i] < 0) return i;
    }
    return -1;
}

/* =========================================================================
 *                       LOGICAL REGISTER LOAD / STORE
 *
 * These are the public surface for inlined op emitters. They route
 * through the BB-local register cache:
 *   - r0  : compile-time constant 0 (xor)
 *   - r14 : pinned in r15d (fp)
 *   - r15 : pinned in rbx (sp)
 *   - 1..13: vm->r[1..13]; cached in RC_CACHE_PHYS when possible.
 * ========================================================================= */
static void emit_load_logical(Emitter *e, uint8_t phys, uint8_t log) {
    if(log == 0) {
        /* xor reg32, reg32 */
        uint8_t rex = 0;
        if(phys >= 8) rex |= REX_R | REX_B;
        if(rex) em_u8(e, rex);
        em_u8(e, 0x31);
        em_u8(e, modrm(3, phys & 7, phys & 7));
        return;
    }
    if(log == 14) {
        emit_mov_r32_r32(e, phys, R_R15);     /* fp lives in r15d */
        return;
    }
    if(log == 15) {
        emit_mov_r32_r32(e, phys, R_RBX);     /* sp lives in ebx */
        return;
    }
    /* GP cache hit -> reg-to-reg copy from the slot's physical reg. */
    int8_t slot = e->rc_slot_of_log[log];
    if(slot >= 0) {
        emit_mov_r32_r32(e, phys, RC_CACHE_PHYS[slot]);
        return;
    }
    /* XMM cache hit -> the live value lives in an xmm slot (e.g. after an
     * FP op). Bridge it back into a GP via movd. */
    int8_t xslot = e->rc_xmm_slot_of_log[log];
    if(xslot >= 0) {
        emit_movd_r32_xmm(e, phys, RC_XMM_PHYS[xslot]);
        return;
    }
    /* Miss -> load from vm->r[log] in memory. We deliberately do NOT
     * auto-fill the cache on read: caching a read-only value would cost
     * an extra mov compared to a pure load, and most reads are one-shot
     * within a BB. The cache fills on writes (which dominate hot loops). */
    emit_mov_r32_r13disp8(e, phys, (int8_t)(4 * log));
}

/* Cache-aware store. The new value is placed into a cache slot and marked
 * dirty; the spill to memory is deferred until BB end (or eviction). */
static void emit_store_logical(Emitter *e, uint8_t log, uint8_t phys) {
    if(log == 0) return;
    if(log == 14) {
        emit_mov_r32_r32(e, R_R15, phys);
        return;
    }
    if(log == 15) {
        emit_mov_r32_r32(e, R_RBX, phys);
        return;
    }
    /* Drop any previous cache entry for `log` (GP or XMM) without spilling:
     * about to be overwritten. */
    rc_drop_log_nospill(e, log);
    /* Try to install in a GP cache slot. If full, write straight to memory
     * (cheaper than evicting an existing slot). */
    int slot = rc_alloc_slot(e);
    if(slot < 0) {
        emit_mov_r13disp8_r32(e, phys, (int8_t)(4 * log));
        return;
    }
    emit_mov_r32_r32(e, RC_CACHE_PHYS[slot], phys);
    e->rc_log_at[slot]     = (int8_t)log;
    e->rc_slot_of_log[log] = (int8_t)slot;
    e->rc_dirty[log]       = 1;
}

/* Cache-bypassing store: writes phys directly to vm->r[log] in memory and
 * invalidates any stale cache entry. Used by emit_cmp_branch_fused which
 * writes `d` between rc_spill_all and the terminator jump (so a cached
 * write would never get flushed before the jump leaves the BB). */
static void emit_store_logical_to_mem(Emitter *e, uint8_t log, uint8_t phys) {
    if(log == 0) return;
    if(log == 14) { emit_mov_r32_r32(e, R_R15, phys); return; }
    if(log == 15) { emit_mov_r32_r32(e, R_RBX, phys); return; }
    rc_drop_log_nospill(e, log);
    emit_mov_r13disp8_r32(e, phys, (int8_t)(4 * log));
}

/* =========================================================================
 *                       FLOAT (XMM) LOAD / STORE
 *
 * These mirror emit_load_logical/emit_store_logical but route through the
 * XMM cache. The op's scratch xmm reg (xmm0/xmm1) is the consumer/producer.
 *
 * Cross-domain interplay:
 *   - load: if log is in GP cache, movd it into the xmm phys; if in XMM
 *           cache, movaps from the slot; else movss from [r13+4*log].
 *   - store: drop any existing cache entry (GP or XMM) for log, install
 *            in a fresh XMM slot. No memory write until BB end / spill.
 *
 * Loads of r14 (fp) and r15 (sp) into xmm are not meaningful (those are
 * integer-pinned values), so we assert log in 0..13 by routing the
 * exceptional cases through movd from their integer home.
 * ========================================================================= */
static void emit_fp_load_logical(Emitter *e, uint8_t xmm, uint8_t log) {
    if(log == 0) {
        /* xorps xmm, xmm   ;  0F 57 /r  (clears all bits = +0.0f) */
        em_u8(e, 0x0F); em_u8(e, 0x57);
        em_u8(e, modrm(3, xmm & 7, xmm & 7));
        return;
    }
    if(log == 14) { emit_movd_xmm_r32(e, xmm, R_R15); return; }
    if(log == 15) { emit_movd_xmm_r32(e, xmm, R_RBX); return; }
    /* XMM cache hit -> reg-to-reg xmm copy. */
    int8_t xslot = e->rc_xmm_slot_of_log[log];
    if(xslot >= 0) {
        emit_movaps_xmm_xmm(e, xmm, RC_XMM_PHYS[xslot]);
        return;
    }
    /* GP cache hit -> bridge across domains via movd. */
    int8_t gslot = e->rc_slot_of_log[log];
    if(gslot >= 0) {
        emit_movd_xmm_r32(e, xmm, RC_CACHE_PHYS[gslot]);
        return;
    }
    /* Miss -> movss xmm, [r13+4*log]. Direct memory load, no gpr stopover. */
    emit_movss_xmm_r13disp8(e, xmm, (int8_t)(4 * log));
}

static void emit_fp_store_logical(Emitter *e, uint8_t log, uint8_t xmm) {
    if(log == 0) return;
    if(log == 14) { emit_movd_r32_xmm(e, R_R15, xmm); return; }
    if(log == 15) { emit_movd_r32_xmm(e, R_RBX, xmm); return; }
    rc_drop_log_nospill(e, log);
    int slot = rc_alloc_xmm_slot(e);
    if(slot < 0) {
        /* Cache full: write straight to memory rather than pay an eviction. */
        emit_movss_r13disp8_xmm(e, (int8_t)(4 * log), xmm);
        return;
    }
    emit_movaps_xmm_xmm(e, RC_XMM_PHYS[slot], xmm);
    e->rc_xmm_log_at[slot]     = (int8_t)log;
    e->rc_xmm_slot_of_log[log] = (int8_t)slot;
    e->rc_dirty[log]           = 1;
}

/* =========================================================================
 *                    PROLOGUE / EPILOGUE / DISPATCH
 * ========================================================================= */
static void emit_prologue(Emitter *e) {
    /* Win64 callee-saved registers we use: rbp, rbx, r12-r15. We also push
     * rdi/rsi because we are required to preserve them even if we do not
     * use them (clobber safety; cheap). */
    em_u8(e, 0x55);                       /* push rbp */
    em_u8(e, 0x53);                       /* push rbx */
    em_u8(e, 0x57);                       /* push rdi */
    em_u8(e, 0x56);                       /* push rsi */
    em_u8(e, 0x41); em_u8(e, 0x54);       /* push r12 */
    em_u8(e, 0x41); em_u8(e, 0x55);       /* push r13 */
    em_u8(e, 0x41); em_u8(e, 0x56);       /* push r14 */
    em_u8(e, 0x41); em_u8(e, 0x57);       /* push r15 */

    /* sub rsp, 40 - 32 bytes shadow space + 8 bytes alignment.
     * On entry rsp was 8 mod 16 (CALL pushed return addr). 8 pushes + 40
     * keeps rsp at 0 mod 16, ready for further CALLs. */
    em_u8(e, REX_W); em_u8(e, 0x83); em_u8(e, 0xEC); em_u8(e, 0x28);

    /* mov r12, rcx  ; rcx = vm (Win64 1st arg) */
    em_u8(e, REX_W | REX_B); em_u8(e, 0x89); em_u8(e, modrm(3, R_RCX, R_R12 & 7));

    /* lea r13, [r12 + offsetof(r)] */
    emit_lea_r64_r12disp(e, R_R13, (int32_t)offsetof(RelicVM, r));

    /* mov r14, [r12 + offsetof(mem)]  ; 64-bit pointer */
    emit_mov_r64_r12disp(e, R_R14, (int32_t)offsetof(RelicVM, mem));

    /* mov r15d, [r12 + offsetof(fp)]  ; pinned fp */
    emit_mov_r32_r12disp(e, R_R15, (int32_t)offsetof(RelicVM, fp));

    /* mov ebx, [r12 + offsetof(sp)]   ; pinned sp */
    {
        /* mov ebx, [r12 + disp32] - rbx is low 4, r12 base needs SIB + REX.B */
        em_u8(e, REX_B);
        em_u8(e, 0x8B);
        em_u8(e, modrm(2, R_RBX, 4));
        em_u8(e, sib(0, 4, 4));
        em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    }

    /* mov rbp, rsp   ; remember host rsp so HALT / !running can unwind
     * cleanly out of any nested native call frames. */
    emit_mov_rbp_rsp(e);

    /* Entry: native CALL into the JIT'd image at dispatch_table[vm->pc].
     * Using `call` (not `jmp`) so main's OP_RET (a host `ret`) returns
     * here. We then jump to the epilogue (we cannot fall through, since
     * the next bytes are the body of the first IR PC). */
    emit_mov_r32_r12disp(e, R_RCX, (int32_t)offsetof(RelicVM, pc));
    em_u8(e, REX_W | REX_B); em_u8(e, 0xB8 | (R_R11 & 7));
    em_u64(e, (uint64_t)e->addr_dispatch_table);
    /* call qword ptr [r11 + 8*rcx] */
    em_u8(e, REX_B);
    em_u8(e, 0xFF);
    em_u8(e, modrm(0, 2, 4));     /* /2 = CALL */
    em_u8(e, sib(3, R_RCX, R_R11 & 7));
    /* jmp rel32 epilogue (reloc target = e->pc_count, which the reloc
     * pass resolves to e->epilogue_off) */
    em_u8(e, 0xE9);
    em_add_reloc(e, (uint32_t)e->len, (int32_t)e->pc_count, RELOC_REL32_AT_PC);
    em_u32(e, 0);
}

static void emit_epilogue(Emitter *e) {
    e->epilogue_off = (int32_t)e->len;

    /* Unwind any leftover native call frames from a HALT or !running exit.
     * On a normal exit (main returned via OP_RET = host ret), rsp == rbp
     * already and this is a no-op. */
    emit_mov_rsp_rbp(e);

    /* spill r15d back to vm->fp */
    emit_mov_r12disp_r32(e, R_R15, (int32_t)offsetof(RelicVM, fp));
    /* spill ebx back to vm->sp */
    {
        em_u8(e, REX_B);
        em_u8(e, 0x89);
        em_u8(e, modrm(2, R_RBX, 4));
        em_u8(e, sib(0, 4, 4));
        em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    }
    /* mov eax, [r12 + offsetof(exit_code)] */
    emit_mov_r32_r12disp(e, R_RAX, (int32_t)offsetof(RelicVM, exit_code));

    /* add rsp, 40 */
    em_u8(e, REX_W); em_u8(e, 0x83); em_u8(e, 0xC4); em_u8(e, 0x28);

    em_u8(e, 0x41); em_u8(e, 0x5F);       /* pop r15 */
    em_u8(e, 0x41); em_u8(e, 0x5E);       /* pop r14 */
    em_u8(e, 0x41); em_u8(e, 0x5D);       /* pop r13 */
    em_u8(e, 0x41); em_u8(e, 0x5C);       /* pop r12 */
    em_u8(e, 0x5E);                       /* pop rsi */
    em_u8(e, 0x5F);                       /* pop rdi */
    em_u8(e, 0x5B);                       /* pop rbx */
    em_u8(e, 0x5D);                       /* pop rbp */
    em_u8(e, 0xC3);                       /* ret */
}

/* Emit indirect dispatch via vm->pc. is_call: true => host CALL (push
 * return address, used inside emit_op_callr); false => host JMP (no push,
 * used by emit_step1_fallback when step1 advanced pc and we resume inside
 * the same JIT function).
 *
 *   mov ecx, [r12 + offsetof(pc)]
 *   mov r11, dispatch_table
 *   jmp/call qword ptr [r11 + 8*rcx]
 */
static void emit_dispatch_via_pc_op(Emitter *e, int is_call) {
    emit_mov_r32_r12disp(e, R_RCX, (int32_t)offsetof(RelicVM, pc));
    em_u8(e, REX_W | REX_B); em_u8(e, 0xB8 | (R_R11 & 7));
    em_u64(e, (uint64_t)e->addr_dispatch_table);
    em_u8(e, REX_B);
    em_u8(e, 0xFF);
    em_u8(e, modrm(0, is_call ? 2 : 4, 4));
    em_u8(e, sib(3, R_RCX, R_R11 & 7));
}
static void emit_dispatch_via_pc(Emitter *e) { emit_dispatch_via_pc_op(e, 0); }

/* =========================================================================
 *                       STEP-1 FALLBACK
 * ========================================================================= */
static void emit_step1_fallback(Emitter *e, int32_t ir_pc) {
    /* mov dword ptr [r12+pc], ir_pc */
    emit_mov_r12disp_imm32(e, (int32_t)offsetof(RelicVM, pc), (uint32_t)ir_pc);

    /* spill pinned regs to vm-> so step1 sees the live values. */
    emit_mov_r12disp_r32(e, R_R15, (int32_t)offsetof(RelicVM, fp));
    {
        /* mov [r12+sp], ebx */
        em_u8(e, REX_B);
        em_u8(e, 0x89);
        em_u8(e, modrm(2, R_RBX, 4));
        em_u8(e, sib(0, 4, 4));
        em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    }

    /* mov rcx, r12  ; first argument (RelicVM*) */
    em_u8(e, REX_W | REX_R);
    em_u8(e, 0x89);
    em_u8(e, modrm(3, R_R12 & 7, R_RCX));

    /* mov rax, addr_step1 ; call rax */
    em_u8(e, REX_W); em_u8(e, 0xB8 | R_RAX);
    em_u64(e, (uint64_t)e->addr_step1);
    em_u8(e, 0xFF); em_u8(e, modrm(3, 2, R_RAX));   /* call rax */

    /* reload pinned regs (step1 may have changed vm->fp / vm->sp). */
    emit_mov_r32_r12disp(e, R_R15, (int32_t)offsetof(RelicVM, fp));
    {
        /* mov ebx, [r12+sp] */
        em_u8(e, REX_B);
        em_u8(e, 0x8B);
        em_u8(e, modrm(2, R_RBX, 4));
        em_u8(e, sib(0, 4, 4));
        em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    }

    /* cmp byte ptr [r12+running], 0 */
    emit_cmp_byte_r12disp_imm8(e, (int32_t)offsetof(RelicVM, running), 0);

    /* jne short over_unwind */
    em_u8(e, 0x75);
    size_t jne_off = e->len;
    em_u8(e, 0);                                  /* placeholder rel8 */
    size_t unwind_start = e->len;
    /* !running: unwind any nested native call frames and jump to epilogue. */
    emit_mov_rsp_rbp(e);
    em_u8(e, 0xE9);
    em_add_reloc(e, (uint32_t)e->len, (int32_t)e->pc_count, RELOC_REL32_AT_PC);
    em_u32(e, 0);
    e->buf[jne_off] = (uint8_t)(e->len - unwind_start);

    /* over_unwind: dispatch on vm->pc (jump, not call - we are still
     * inside the current JIT'd function's native frame). */
    emit_dispatch_via_pc(e);
}

/* =========================================================================
 *                       INLINED OP EMITTERS
 * ========================================================================= */

static void RELIC_UNUSED emit_op_nop(Emitter *e) { (void)e; }

/* OP_HALT: src_a != 0 -> exit code from r[src_a] (@exit stages its arg
 * in r1); else imm12 (hand-written `HALT n` assembly). */
static void emit_op_halt(Emitter *e, uint8_t src_a, int32_t imm12) {
    if(src_a) {
        emit_load_logical(e, R_RAX, src_a);
        emit_mov_r12disp_r32(e, R_RAX, (int32_t)offsetof(RelicVM, exit_code));
    } else {
        emit_mov_r12disp_imm32(e, (int32_t)offsetof(RelicVM, exit_code), (uint32_t)imm12);
    }
    emit_mov_r12disp_imm8(e, (int32_t)offsetof(RelicVM, running), 0);
    /* Unwind any nested native call frames (we may be N levels deep), then
     * jump to the epilogue. */
    emit_mov_rsp_rbp(e);
    em_u8(e, 0xE9);
    em_add_reloc(e, (uint32_t)e->len, (int32_t)e->pc_count, RELOC_REL32_AT_PC);
    em_u32(e, 0);
}

/* OP_LDC dst, imm20 */
static void emit_op_ldc(Emitter *e, uint8_t dst, int32_t imm20) {
    if(dst == 0) return;
    em_u8(e, 0xB8 | R_RAX); em_u32(e, (uint32_t)imm20);   /* mov eax, imm32 */
    emit_store_logical(e, dst, R_RAX);
}

/* OP_LDC32 dst, imm32 */
static void emit_op_ldc32(Emitter *e, uint8_t dst, uint32_t imm32) {
    if(dst == 0) return;
    em_u8(e, 0xB8 | R_RAX); em_u32(e, imm32);
    emit_store_logical(e, dst, R_RAX);
}

/* OP_MOV dst, src */
static void emit_op_mov(Emitter *e, uint8_t dst, uint8_t src_a) {
    if(dst == 0) return;
    emit_load_logical(e, R_RAX, src_a);
    emit_store_logical(e, dst, R_RAX);
}

/* OP_LEA dst, srcA, imm12 : reg_w(dst, a + imm12) */
static void emit_op_lea(Emitter *e, uint8_t dst, uint8_t src_a, int32_t imm12) {
    if(dst == 0) return;
    emit_load_logical(e, R_RAX, src_a);
    em_u8(e, 0x05); em_u32(e, (uint32_t)imm12);   /* add eax, imm32 */
    emit_store_logical(e, dst, R_RAX);
}

/* Helper: binary "op eax, ecx" reg-reg (32-bit, no REX needed for low regs). */
static void emit_op_eax_ecx(Emitter *e, uint8_t op_byte) {
    em_u8(e, op_byte);
    em_u8(e, modrm(3, R_RAX, R_RCX));
}

/* Emit `op reg_dst, reg_src` (32-bit) where op_byte is the standard
 * `op reg, r/m` encoding (ADD=03, SUB=2B, AND=23, OR=0B, XOR=33). */
static void emit_op_r32_r32(Emitter *e, uint8_t op_byte,
                            uint8_t reg_dst, uint8_t reg_src) {
    uint8_t rex = 0;
    if(reg_dst >= 8) rex |= REX_R;
    if(reg_src >= 8) rex |= REX_B;
    if(rex) em_u8(e, rex);
    em_u8(e, op_byte);
    em_u8(e, modrm(3, reg_dst & 7, reg_src & 7));
}

/* Binary op dst, a, b: load a->eax, b->ecx, op eax,ecx, store eax->dst.
 *
 * Cache peephole: when dst == a and dst is in a GP cache slot, mutate the
 * slot register in place: `op slot_reg, b_reg`. We still need b in a real
 * register, but we save the load-of-a, the temp eax, and the store-back. */
static void emit_bin(Emitter *e, uint8_t op_byte,
                     uint8_t dst, uint8_t a, uint8_t b) {
    if(dst == 0) return;
    if(dst == a && dst >= 1 && dst <= 13) {
        int8_t slot = e->rc_slot_of_log[dst];
        if(slot >= 0) {
            emit_load_logical(e, R_RCX, b);
            emit_op_r32_r32(e, op_byte, RC_CACHE_PHYS[slot], R_RCX);
            e->rc_dirty[dst] = 1;
            return;
        }
    }
    emit_load_logical(e, R_RAX, a);
    emit_load_logical(e, R_RCX, b);
    emit_op_eax_ecx(e, op_byte);
    emit_store_logical(e, dst, R_RAX);
}

/* Map op-eax-imm32 byte to ModR/M /digit (subop) for RMW form. */
static uint8_t subop_for_op_eax_imm32(uint8_t op_eax_imm32) {
    switch(op_eax_imm32) {
        case 0x05: return 0;   /* ADD */
        case 0x2D: return 5;   /* SUB */
        case 0x25: return 4;   /* AND */
        case 0x0D: return 1;   /* OR  */
        default:   return 0;
    }
}

/* Binary op dst, a, imm: load a->eax, op eax,imm32, store eax->dst.
 * op_eax_imm32 is the "op eax, imm32" form: ADDI=0x05, SUBI=0x2D,
 * ANDI=0x25, ORI=0x0D.
 *
 * Peephole: when dst == a, emit a direct read-modify-write on the
 * register's home slot, avoiding the load/op/store round-trip entirely.
 * This is the common case for induction variables (`i = i + 1`), bitmasks,
 * and similar tight-loop patterns. */
static void emit_bin_imm(Emitter *e, uint8_t op_eax_imm32,
                         uint8_t dst, uint8_t a, int32_t imm) {
    if(dst == 0) return;
    if(dst == a) {
        uint8_t subop = subop_for_op_eax_imm32(op_eax_imm32);
        if(dst >= 1 && dst <= 13) {
            /* If dst lives in a GP cache slot, mutate the slot register
             * directly: 0 memory ops vs 2 (spill + RMW-mem). The cache
             * entry remains valid and dirty. */
            int8_t slot = e->rc_slot_of_log[dst];
            if(slot >= 0) {
                emit_op_r32_imm(e, subop, RC_CACHE_PHYS[slot], imm);
                e->rc_dirty[dst] = 1;
                return;
            }
            /* If dst happens to be in the XMM cache (rare for integer
             * RMW), fall back to spill+RMW-mem: bridging through movd
             * would cost more than the spill+memory-RMW pair. */
            rc_evict_log(e, dst);
            emit_rmw_r13disp8_imm(e, subop, (int8_t)(4 * dst), imm);
            return;
        }
        if(dst == 14) {
            /* RMW on r15d (pinned fp) */
            if(subop == 0 || subop == 5) {
                emit_addsub_r15d_imm(e, subop, imm);
            } else {
                emit_andor_r15d_imm32(e, subop, imm);
            }
            return;
        }
        if(dst == 15) {
            /* RMW on ebx (pinned sp) */
            if(subop == 0 || subop == 5) {
                if(imm >= -128 && imm <= 127)
                    emit_addsub_ebx_imm8(e, subop, (int8_t)imm);
                else
                    emit_addsub_ebx_imm32(e, subop, imm);
            } else {
                emit_andor_ebx_imm32(e, subop, imm);
            }
            return;
        }
    }
    emit_load_logical(e, R_RAX, a);
    em_u8(e, op_eax_imm32); em_u32(e, (uint32_t)imm);
    emit_store_logical(e, dst, R_RAX);
}

static void emit_op_add (Emitter *e, uint8_t d, uint8_t a, uint8_t b) { emit_bin(e, 0x03, d, a, b); }
static void emit_op_sub (Emitter *e, uint8_t d, uint8_t a, uint8_t b) { emit_bin(e, 0x2B, d, a, b); }
static void emit_op_and (Emitter *e, uint8_t d, uint8_t a, uint8_t b) { emit_bin(e, 0x23, d, a, b); }
static void emit_op_or  (Emitter *e, uint8_t d, uint8_t a, uint8_t b) { emit_bin(e, 0x0B, d, a, b); }
static void emit_op_xor (Emitter *e, uint8_t d, uint8_t a, uint8_t b) { emit_bin(e, 0x33, d, a, b); }
static void emit_op_addi(Emitter *e, uint8_t d, uint8_t a, int32_t i) { emit_bin_imm(e, 0x05, d, a, i); }
static void emit_op_subi(Emitter *e, uint8_t d, uint8_t a, int32_t i) { emit_bin_imm(e, 0x2D, d, a, i); }
static void emit_op_andi(Emitter *e, uint8_t d, uint8_t a, int32_t i) { emit_bin_imm(e, 0x25, d, a, i); }
static void emit_op_ori (Emitter *e, uint8_t d, uint8_t a, int32_t i) { emit_bin_imm(e, 0x0D, d, a, i); }

/* OP_MUL : imul eax, ecx */
static void emit_op_mul(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    emit_load_logical(e, R_RCX, b);
    em_u8(e, 0x0F); em_u8(e, 0xAF);
    em_u8(e, modrm(3, R_RAX, R_RCX));
    emit_store_logical(e, d, R_RAX);
}

/* OP_NEG */
static void emit_op_neg(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    em_u8(e, 0xF7); em_u8(e, modrm(3, 3, R_RAX));     /* neg eax */
    emit_store_logical(e, d, R_RAX);
}

/* OP_NOT */
static void emit_op_not(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    em_u8(e, 0xF7); em_u8(e, modrm(3, 2, R_RAX));     /* not eax */
    emit_store_logical(e, d, R_RAX);
}

/* Compare + setcc:
 *   mov eax, [a]; mov ecx, [b]
 *   cmp eax, ecx
 *   setcc al
 *   movzx eax, al
 *   mov [d], eax
 * setcc opcodes: 0x94=E, 0x95=NE, 0x9C=L, 0x9E=LE, 0x9F=G, 0x9D=GE,
 *                0x92=B (CLTU), 0x96=BE (CLEU). */
static void emit_cmp_setcc(Emitter *e, uint8_t setcc_op,
                           uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    emit_load_logical(e, R_RCX, b);
    em_u8(e, 0x3B); em_u8(e, modrm(3, R_RAX, R_RCX));   /* cmp eax, ecx */
    em_u8(e, 0x0F); em_u8(e, setcc_op); em_u8(e, modrm(3, 0, R_RAX)); /* setcc al */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, modrm(3, R_RAX, R_RAX)); /* movzx eax, al */
    emit_store_logical(e, d, R_RAX);
}

/* OP_JMP */
static void emit_op_jmp(Emitter *e, int32_t target_pc) {
    em_u8(e, 0xE9);
    em_add_reloc(e, (uint32_t)e->len, target_pc, RELOC_REL32_AT_PC);
    em_u32(e, 0);
}

/* OP_JT/OP_JF */
static void emit_op_jcond(Emitter *e, uint8_t cc_byte,
                          uint8_t src_a, int32_t target_pc) {
    emit_load_logical(e, R_RAX, src_a);
    em_u8(e, 0x85); em_u8(e, modrm(3, R_RAX, R_RAX));   /* test eax, eax */
    em_u8(e, 0x0F); em_u8(e, cc_byte);
    em_add_reloc(e, (uint32_t)e->len, target_pc, RELOC_REL32_AT_PC);
    em_u32(e, 0);
}

/* Fused CMP+JT/JF (peephole). */
static void emit_cmp_branch_fused(Emitter *e,
                                  uint8_t setcc_op,
                                  uint8_t d, uint8_t a, uint8_t b,
                                  int jt_or_jf, int32_t target_pc) {
    emit_load_logical(e, R_RAX, a);
    emit_load_logical(e, R_RCX, b);
    em_u8(e, 0x3B); em_u8(e, modrm(3, R_RAX, R_RCX));   /* cmp eax, ecx */
    if(d != 0) {
        em_u8(e, 0x0F); em_u8(e, setcc_op); em_u8(e, modrm(3, 0, R_RAX));
        em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, modrm(3, R_RAX, R_RAX));
        /* BB-terminator op: caller pre-spilled the cache. We must NOT
         * route this write through the cache because the conditional
         * jump below would leave before any post-spill could flush it.
         * Write straight to vm->r[d]. */
        emit_store_logical_to_mem(e, d, R_RAX);
    }
    uint8_t jcc_cc = (uint8_t)(setcc_op - 0x10);
    if(jt_or_jf == 0) jcc_cc ^= 1;
    em_u8(e, 0x0F); em_u8(e, jcc_cc);
    em_add_reloc(e, (uint32_t)e->len, target_pc, RELOC_REL32_AT_PC);
    em_u32(e, 0);
}

/* OP_JMPR : vm->pc = src; dispatch */
static void emit_op_jmpr(Emitter *e, uint8_t src) {
    emit_load_logical(e, R_RAX, src);
    emit_mov_r12disp_r32(e, R_RAX, (int32_t)offsetof(RelicVM, pc));
    emit_dispatch_via_pc(e);
}

/* Stack helpers using the pinned vm->sp register (rbx). */

/* Push imm32: sub ebx,4 ; mov [r14+rbx], imm32 */
static void emit_stack_push_imm32(Emitter *e, uint32_t imm) {
    emit_addsub_ebx_imm8(e, 5, 4);
    emit_mov_r14rbx_imm32(e, imm);
}

/* Push reg32 (rbx must not be the source - we update rbx FIRST). Callers
 * pass r15 (fp) and rax which are both safe. */
static void emit_stack_push_r32(Emitter *e, uint8_t reg) {
    emit_addsub_ebx_imm8(e, 5, 4);
    emit_mov_r14rbx_r32(e, reg);
}

/* Pop into reg32. */
static void emit_stack_pop_r32(Emitter *e, uint8_t reg) {
    emit_mov_r32_r14rbx(e, reg);
    emit_addsub_ebx_imm8(e, 0, 4);
}

/* OP_CALL fn_id : push(retpc) ; push(old fp) ; fp=sp ; native call.
 *
 * The VM-stack layout MUST match the IR convention (see ir.c line 224):
 *   FP+0 = saved old fp
 *   FP+4 = return PC slot
 *   FP+8.. = stack-passed args
 * so we still push retpc as the value at FP+4, even though the actual
 * return is via the host call/ret pair, not via reading that slot. */
static void emit_op_call(Emitter *e, int32_t target_pc, int32_t return_pc) {
    emit_stack_push_imm32(e, (uint32_t)return_pc);    /* VM-push retpc */
    emit_stack_push_r32(e, R_R15);                     /* VM-push old fp */
    /* fp = sp : mov r15d, ebx */
    em_u8(e, REX_B);
    em_u8(e, 0x89);
    em_u8(e, modrm(3, R_RBX, R_R15 & 7));
    /* call rel32 (target_pc) */
    em_u8(e, 0xE8);
    em_add_reloc(e, (uint32_t)e->len, target_pc, RELOC_REL32_AT_PC);
    em_u32(e, 0);
}

/* OP_CALLR/OP_CALLP : indirect call via fn_offsets[reg], then
 * dispatch_table[ir_pc] to the JIT'd address. */
static void emit_op_callr(Emitter *e, uint8_t src_reg, int32_t return_pc,
                          int32_t nfuncs, uintptr_t fn_offsets_addr) {
    emit_load_logical(e, R_RAX, src_reg);
    /* cmp eax, nfuncs */
    em_u8(e, 0x3D); em_u32(e, (uint32_t)nfuncs);

    /* jae +rel8 - patched after the block. */
    em_u8(e, 0x73);
    size_t jae_off = e->len;
    em_u8(e, 0);    /* placeholder */
    size_t block_start = e->len;

    /* VM-push retpc and old fp. */
    emit_stack_push_imm32(e, (uint32_t)return_pc);
    emit_stack_push_r32(e, R_R15);

    /* fp = sp : mov r15d, ebx */
    em_u8(e, REX_B);
    em_u8(e, 0x89);
    em_u8(e, modrm(3, R_RBX, R_R15 & 7));

    /* mov r11, fn_offsets_addr ; mov ecx, [r11 + 4*rax]  (ecx = ir_pc) */
    em_u8(e, REX_W | REX_B); em_u8(e, 0xB8 | (R_R11 & 7));
    em_u64(e, (uint64_t)fn_offsets_addr);
    em_u8(e, REX_B);
    em_u8(e, 0x8B);
    em_u8(e, modrm(0, R_RCX, 4));
    em_u8(e, sib(2, R_RAX, R_R11 & 7));

    /* mov r11, dispatch_table ; call qword [r11 + 8*rcx] */
    em_u8(e, REX_W | REX_B); em_u8(e, 0xB8 | (R_R11 & 7));
    em_u64(e, (uint64_t)e->addr_dispatch_table);
    em_u8(e, REX_B);
    em_u8(e, 0xFF);
    em_u8(e, modrm(0, 2, 4));     /* /2 = CALL */
    em_u8(e, sib(3, R_RCX, R_R11 & 7));

    /* Patch jae rel8 to skip the entire block. */
    size_t block_end = e->len;
    int32_t block_size = (int32_t)(block_end - block_start);
    if(block_size < 0 || block_size > 127) {
        e->oom = 1;     /* internal error: block too large for rel8 */
    } else {
        e->buf[jae_off] = (uint8_t)block_size;
    }
}

/* OP_RET : sp=fp ; pop old fp ; discard retpc slot ; host ret.
 * Also tears down the per-function 40-byte host frame allocated at fn
 * entry by emit_fn_entry_stub(). */
static void emit_op_ret(Emitter *e) {
    /* sp = fp : mov ebx, r15d */
    em_u8(e, REX_B);   /* r/m = r15 needs REX.B; reg = rbx (low 3) */
    em_u8(e, 0x8B);
    em_u8(e, modrm(3, R_RBX, R_R15 & 7));
    /* pop old fp into r15d */
    emit_stack_pop_r32(e, R_R15);
    /* discard retpc slot (we used the host stack for the actual return) */
    emit_addsub_ebx_imm8(e, 0, 4);
    /* tear down per-function host frame, then host ret */
    emit_addsub_rsp_imm8(e, 0, 40);
    emit_ret(e);
}

/* OP_ENTER uimm20 : sub ebx, frame_words*4. */
static void emit_op_enter(Emitter *e, int32_t frame_words) {
    int32_t bytes = frame_words * 4;
    if(bytes == 0) return;
    if(bytes >= -128 && bytes <= 127) {
        emit_addsub_ebx_imm8(e, 5, (int8_t)bytes);
    } else {
        emit_addsub_ebx_imm32(e, 5, bytes);
    }
}

/* OP_LEAVE : sp = fp (mov ebx, r15d). */
static void emit_op_leave(Emitter *e) {
    em_u8(e, REX_B);
    em_u8(e, 0x8B);
    em_u8(e, modrm(3, R_RBX, R_R15 & 7));
}

/* Per-function host-frame setup. Emitted at every fn_offsets[i] pc.
 * Reserves 32 bytes of shadow space + 8 bytes of alignment, matching the
 * outermost prologue. After this, rsp is 16-byte aligned so the body can
 * safely emit `call rax` to step1 without further fixups. */
static void emit_fn_entry_stub(Emitter *e) {
    /* sub rsp, 40 */
    emit_addsub_rsp_imm8(e, 5, 40);
}

/* Memory ops: load/store via [r14 + rcx + imm12] where rcx = address reg. */
static void emit_op_ld32(Emitter *e, uint8_t dst, uint8_t a, int32_t imm12) {
    if(dst == 0) return;
    emit_load_logical(e, R_RCX, a);
    emit_mov_r32_r14rcx_disp(e, R_RAX, imm12);
    emit_store_logical(e, dst, R_RAX);
}
static void emit_op_ld16(Emitter *e, uint8_t dst, uint8_t a, int32_t imm12) {
    if(dst == 0) return;
    emit_load_logical(e, R_RCX, a);
    emit_movzx16_r32_r14rcx_disp(e, R_RAX, imm12);
    emit_store_logical(e, dst, R_RAX);
}
static void emit_op_ld8(Emitter *e, uint8_t dst, uint8_t a, int32_t imm12) {
    if(dst == 0) return;
    emit_load_logical(e, R_RCX, a);
    emit_movzx8_r32_r14rcx_disp(e, R_RAX, imm12);
    emit_store_logical(e, dst, R_RAX);
}
static void emit_op_st32(Emitter *e, uint8_t addr_reg, uint8_t val_reg, int32_t imm12) {
    emit_load_logical(e, R_RCX, addr_reg);
    emit_load_logical(e, R_RAX, val_reg);
    emit_mov_r14rcx_disp_r32(e, R_RAX, imm12);
}
static void emit_op_st16(Emitter *e, uint8_t addr_reg, uint8_t val_reg, int32_t imm12) {
    emit_load_logical(e, R_RCX, addr_reg);
    emit_load_logical(e, R_RAX, val_reg);
    emit_mov16_r14rcx_disp_r32(e, R_RAX, imm12);
}
static void emit_op_st8(Emitter *e, uint8_t addr_reg, uint8_t val_reg, int32_t imm12) {
    emit_load_logical(e, R_RCX, addr_reg);
    emit_load_logical(e, R_RAX, val_reg);
    emit_mov8_r14rcx_disp_r32(e, R_RAX, imm12);
}

/* OP_DIV/OP_MOD : signed 32-bit division with /0 -> 0 guard. */
static void emit_op_div(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    emit_load_logical(e, R_RCX, b);
    em_u8(e, 0x85); em_u8(e, modrm(3, R_RCX, R_RCX));   /* test ecx, ecx */
    em_u8(e, 0x74); em_u8(e, 0x05);                     /* jz +5 */
    em_u8(e, 0x99);                                     /* cdq */
    em_u8(e, 0xF7); em_u8(e, modrm(3, 7, R_RCX));       /* idiv ecx */
    em_u8(e, 0xEB); em_u8(e, 0x02);                     /* jmp +2 */
    em_u8(e, 0x31); em_u8(e, modrm(3, R_RAX, R_RAX));   /* xor eax, eax */
    emit_store_logical(e, d, R_RAX);
}
static void emit_op_mod(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    emit_load_logical(e, R_RCX, b);
    em_u8(e, 0x85); em_u8(e, modrm(3, R_RCX, R_RCX));   /* test ecx, ecx */
    em_u8(e, 0x74); em_u8(e, 0x07);                     /* jz +7 */
    em_u8(e, 0x99);                                     /* cdq */
    em_u8(e, 0xF7); em_u8(e, modrm(3, 7, R_RCX));       /* idiv ecx */
    em_u8(e, 0x89); em_u8(e, modrm(3, R_RDX, R_RAX));   /* mov eax, edx */
    em_u8(e, 0xEB); em_u8(e, 0x02);                     /* jmp +2 */
    em_u8(e, 0x31); em_u8(e, modrm(3, R_RAX, R_RAX));   /* xor eax, eax */
    emit_store_logical(e, d, R_RAX);
}

/* Variable shifts via CL: D3 /N where N is subop. */
static void emit_op_shift(Emitter *e, uint8_t subop,
                          uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    emit_load_logical(e, R_RCX, b);
    em_u8(e, 0xD3); em_u8(e, modrm(3, subop, R_RAX));
    emit_store_logical(e, d, R_RAX);
}
static void emit_op_shl(Emitter *e, uint8_t d, uint8_t a, uint8_t b) { emit_op_shift(e, 4, d, a, b); }
static void emit_op_shr(Emitter *e, uint8_t d, uint8_t a, uint8_t b) { emit_op_shift(e, 5, d, a, b); }
static void emit_op_sar(Emitter *e, uint8_t d, uint8_t a, uint8_t b) { emit_op_shift(e, 7, d, a, b); }

/* Type conversions: each loads via emit_load_logical, applies the convert,
 * and stores via emit_store_logical. All operate on EAX. */
static void emit_op_sx8(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    em_u8(e, 0x0F); em_u8(e, 0xBE); em_u8(e, modrm(3, R_RAX, R_RAX)); /* movsx eax, al */
    emit_store_logical(e, d, R_RAX);
}
static void emit_op_sx16(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    em_u8(e, 0x0F); em_u8(e, 0xBF); em_u8(e, modrm(3, R_RAX, R_RAX)); /* movsx eax, ax */
    emit_store_logical(e, d, R_RAX);
}
static void emit_op_zx8(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, modrm(3, R_RAX, R_RAX));
    emit_store_logical(e, d, R_RAX);
}
static void emit_op_zx16(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    em_u8(e, 0x0F); em_u8(e, 0xB7); em_u8(e, modrm(3, R_RAX, R_RAX));
    emit_store_logical(e, d, R_RAX);
}
static void emit_op_trunc8(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    em_u8(e, 0x25); em_u32(e, 0x000000FF);              /* and eax, 0xFF */
    emit_store_logical(e, d, R_RAX);
}
static void emit_op_trunc16(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    em_u8(e, 0x25); em_u32(e, 0x0000FFFF);
    emit_store_logical(e, d, R_RAX);
}

/* OP_PUSH/OP_POP via the pinned-sp stack helpers. */
static void emit_op_push(Emitter *e, uint8_t src) {
    /* Load src into eax FIRST. If src == r15 (sp) this captures the
     * pre-decrement value, which is the correct semantics. */
    emit_load_logical(e, R_RAX, src);
    emit_addsub_ebx_imm8(e, 5, 4);          /* sub ebx, 4 */
    emit_mov_r14rbx_r32(e, R_RAX);
}
static void emit_op_pop(Emitter *e, uint8_t dst) {
    emit_mov_r32_r14rbx(e, R_RAX);
    emit_addsub_ebx_imm8(e, 0, 4);          /* add ebx, 4 */
    emit_store_logical(e, dst, R_RAX);
}

/* =========================================================================
 *                       SSE / FLOATING-POINT HELPERS
 *
 * Float values live in the i32 register file as raw IEEE-754 bit patterns
 * (see vm.c AS_F/AS_I), so we just bit-cast through MOVD and use scalar
 * single-precision SSE ops. Only xmm0/xmm1 are used; no REX needed.
 * ========================================================================= */

/* movd xmm<dst>, r32<src>   ;  66 [REX] 0F 6E /r
 * xmm in reg field (REX.R if xmm>=8); r32 in r/m field (REX.B if r32>=8). */
static void emit_movd_xmm_r32(Emitter *e, uint8_t xmm_dst, uint8_t r32_src) {
    em_u8(e, 0x66);
    uint8_t rex = 0;
    if(xmm_dst >= 8) rex |= REX_R;
    if(r32_src >= 8) rex |= REX_B;
    if(rex) em_u8(e, rex);
    em_u8(e, 0x0F); em_u8(e, 0x6E);
    em_u8(e, modrm(3, xmm_dst & 7, r32_src & 7));
}

/* movd r32<dst>, xmm<src>   ;  66 [REX] 0F 7E /r
 * xmm in reg field (REX.R if xmm>=8); r32 in r/m field (REX.B if r32>=8). */
static void emit_movd_r32_xmm(Emitter *e, uint8_t r32_dst, uint8_t xmm_src) {
    em_u8(e, 0x66);
    uint8_t rex = 0;
    if(xmm_src >= 8) rex |= REX_R;
    if(r32_dst >= 8) rex |= REX_B;
    if(rex) em_u8(e, rex);
    em_u8(e, 0x0F); em_u8(e, 0x7E);
    em_u8(e, modrm(3, xmm_src & 7, r32_dst & 7));
}

/* SSE scalar-single binary op, fixed on xmm0,xmm1 :  F3 0F <op> C1 */
static void emit_sse_xmm0_xmm1(Emitter *e, uint8_t op) {
    em_u8(e, 0xF3);
    em_u8(e, 0x0F); em_u8(e, op);
    em_u8(e, modrm(3, 0, 1));
}

/* ucomiss xmm0, xmm1   ;  0F 2E C1  (no F3 prefix) */
static void emit_ucomiss_xmm0_xmm1(Emitter *e) {
    em_u8(e, 0x0F); em_u8(e, 0x2E);
    em_u8(e, modrm(3, 0, 1));
}

/* cvtsi2ss xmm0, eax   ;  F3 0F 2A C0 */
static void emit_cvtsi2ss_xmm0_eax(Emitter *e) {
    em_u8(e, 0xF3);
    em_u8(e, 0x0F); em_u8(e, 0x2A);
    em_u8(e, modrm(3, 0, R_RAX));
}

/* cvttss2si eax, xmm0  ;  F3 0F 2C C0  (truncating, matches (i32)f) */
static void emit_cvttss2si_eax_xmm0(Emitter *e) {
    em_u8(e, 0xF3);
    em_u8(e, 0x0F); em_u8(e, 0x2C);
    em_u8(e, modrm(3, R_RAX, 0));
}

/* ---- SSE2 scalar-double helpers (f64). ---- */

/* SSE2 scalar-double binary op, fixed on xmm0,xmm1 :  F2 0F <op> C1 */
static void emit_sse_sd_xmm0_xmm1(Emitter *e, uint8_t op) {
    em_u8(e, 0xF2);
    em_u8(e, 0x0F); em_u8(e, op);
    em_u8(e, modrm(3, 0, 1));
}

/* ucomisd xmm0, xmm1   ;  66 0F 2E C1  (66 prefix, not F2) */
static void emit_ucomisd_xmm0_xmm1(Emitter *e) {
    em_u8(e, 0x66);
    em_u8(e, 0x0F); em_u8(e, 0x2E);
    em_u8(e, modrm(3, 0, 1));
}
/* ucomisd xmm1, xmm0   ;  66 0F 2E C8 */
static void emit_ucomisd_xmm1_xmm0(Emitter *e) {
    em_u8(e, 0x66);
    em_u8(e, 0x0F); em_u8(e, 0x2E);
    em_u8(e, modrm(3, 1, 0));
}

/* cvtsi2sd xmm0, eax   ;  F2 0F 2A C0  (signed i32 -> f64) */
static void emit_cvtsi2sd_xmm0_eax(Emitter *e) {
    em_u8(e, 0xF2);
    em_u8(e, 0x0F); em_u8(e, 0x2A);
    em_u8(e, modrm(3, 0, R_RAX));
}
/* cvtsi2sd xmm0, rax   ;  F2 48 0F 2A C0  (signed i64 -> f64; a
 * zero-extended u32 in rax gives the unsigned conversion) */
static void emit_cvtsi2sd_xmm0_rax(Emitter *e) {
    em_u8(e, 0xF2);
    em_u8(e, REX_W);
    em_u8(e, 0x0F); em_u8(e, 0x2A);
    em_u8(e, modrm(3, 0, R_RAX));
}
/* cvttsd2si eax, xmm0  ;  F2 0F 2C C0  (truncating, matches (i32)d) */
static void emit_cvttsd2si_eax_xmm0(Emitter *e) {
    em_u8(e, 0xF2);
    em_u8(e, 0x0F); em_u8(e, 0x2C);
    em_u8(e, modrm(3, R_RAX, 0));
}
/* cvttsd2si rax, xmm0  ;  F2 48 0F 2C C0  (i64 truncation; low dword
 * of the result is exactly (u32)d for in-range values) */
static void emit_cvttsd2si_rax_xmm0(Emitter *e) {
    em_u8(e, 0xF2);
    em_u8(e, REX_W);
    em_u8(e, 0x0F); em_u8(e, 0x2C);
    em_u8(e, modrm(3, R_RAX, 0));
}
/* cvtsd2ss xmm0, xmm0  ;  F2 0F 5A C0  (f64 -> f32, round-nearest-even) */
static void emit_cvtsd2ss_xmm0_xmm0(Emitter *e) {
    em_u8(e, 0xF2);
    em_u8(e, 0x0F); em_u8(e, 0x5A);
    em_u8(e, modrm(3, 0, 0));
}
/* cvtss2sd xmm0, xmm0  ;  F3 0F 5A C0  (f32 -> f64, exact) */
static void emit_cvtss2sd_xmm0_xmm0(Emitter *e) {
    em_u8(e, 0xF3);
    em_u8(e, 0x0F); em_u8(e, 0x5A);
    em_u8(e, modrm(3, 0, 0));
}

/* Common preamble for binary FP ops: load a->xmm0, b->xmm1 directly,
 * routing through the XMM cache when possible, so FP ops never touch the
 * GPR domain unless the operand happens to be GP-cached. */
static void emit_fp_load_ab(Emitter *e, uint8_t a, uint8_t b) {
    emit_fp_load_logical(e, 0, a);
    emit_fp_load_logical(e, 1, b);
}

/* Common tail: store xmm0 into the XMM cache as the new value of dst. */
static void emit_fp_store_xmm0(Emitter *e, uint8_t dst) {
    emit_fp_store_logical(e, dst, 0);
}

static void emit_op_fadd(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_fp_load_ab(e, a, b);
    emit_sse_xmm0_xmm1(e, 0x58);   /* addss */
    emit_fp_store_xmm0(e, d);
}
static void emit_op_fsub(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_fp_load_ab(e, a, b);
    emit_sse_xmm0_xmm1(e, 0x5C);   /* subss */
    emit_fp_store_xmm0(e, d);
}
static void emit_op_fmul(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_fp_load_ab(e, a, b);
    emit_sse_xmm0_xmm1(e, 0x59);   /* mulss */
    emit_fp_store_xmm0(e, d);
}

/* OP_FDIV : raw IEEE 754 division (Inf/NaN propagate). */
static void emit_op_fdiv(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    emit_load_logical(e, R_RCX, b);
    emit_movd_xmm_r32(e, 0, R_RAX);               /* movd xmm0, eax  (4) */
    emit_movd_xmm_r32(e, 1, R_RCX);               /* movd xmm1, ecx  (4) */
    emit_sse_xmm0_xmm1(e, 0x5E);                  /* divss xmm0,xmm1 (4) */
    emit_movd_r32_xmm(e, R_RAX, 0);               /* movd eax, xmm0  (4) */
    emit_store_logical(e, d, R_RAX);
}

/* OP_FNEG : flip sign bit in integer domain. */
static void emit_op_fneg(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    /* xor eax, 0x80000000  ;  35 00 00 00 80 */
    em_u8(e, 0x35); em_u32(e, 0x80000000u);
    emit_store_logical(e, d, R_RAX);
}

/* OP_FCEQ : ordered equality (NaN -> false), matches C `==`.
 *   ucomiss xmm0, xmm1
 *   setnp  al    ; al = !PF (1 if ordered)
 *   setz   cl    ; cl = ZF  (1 if equal-or-unordered)
 *   and    al, cl
 *   movzx  eax, al
 */
static void emit_op_fceq(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_fp_load_ab(e, a, b);
    emit_ucomiss_xmm0_xmm1(e);
    em_u8(e, 0x0F); em_u8(e, 0x9B); em_u8(e, modrm(3, 0, R_RAX));   /* setnp al */
    em_u8(e, 0x0F); em_u8(e, 0x94); em_u8(e, modrm(3, 0, R_RCX));   /* setz  cl */
    em_u8(e, 0x20); em_u8(e, modrm(3, R_RCX, R_RAX));               /* and al, cl */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, modrm(3, R_RAX, R_RAX));/* movzx eax,al */
    emit_store_logical(e, d, R_RAX);
}

/* OP_FCLT : a < b (NaN -> false).
 * Trick: ucomiss xmm1, xmm0 ; seta al  (CF=0 && ZF=0 means xmm1 > xmm0 ordered).
 * On unordered, CF=1 so seta=0 => false. Matches C semantics. */
static void emit_op_fclt(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_fp_load_ab(e, a, b);
    /* ucomiss xmm1, xmm0  ;  0F 2E C8 */
    em_u8(e, 0x0F); em_u8(e, 0x2E); em_u8(e, modrm(3, 1, 0));
    em_u8(e, 0x0F); em_u8(e, 0x97); em_u8(e, modrm(3, 0, R_RAX));   /* seta al */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, modrm(3, R_RAX, R_RAX));
    emit_store_logical(e, d, R_RAX);
}

/* OP_FCLE : a <= b (NaN -> false).
 * Trick: ucomiss xmm1, xmm0 ; setae al  (CF=0 means xmm1 >= xmm0 ordered).
 * On unordered, CF=1 so setae=0 => false. */
static void emit_op_fcle(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_fp_load_ab(e, a, b);
    em_u8(e, 0x0F); em_u8(e, 0x2E); em_u8(e, modrm(3, 1, 0));       /* ucomiss xmm1,xmm0 */
    em_u8(e, 0x0F); em_u8(e, 0x93); em_u8(e, modrm(3, 0, R_RAX));   /* setae al */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, modrm(3, R_RAX, R_RAX));
    emit_store_logical(e, d, R_RAX);
}

/* OP_I2F : (float)(i32)a */
static void emit_op_i2f(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    emit_cvtsi2ss_xmm0_eax(e);
    emit_fp_store_xmm0(e, d);
}

/* OP_F2I : (i32)truncate(AS_F(a)) */
static void emit_op_f2i(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_RAX, a);
    emit_movd_xmm_r32(e, 0, R_RAX);
    emit_cvttss2si_eax_xmm0(e);
    emit_store_logical(e, d, R_RAX);
}

/* =========================================================================
 *                          f64 (d-bank) OPERATIONS
 * ----------------------------------------------------------------------------
 * Same model as the v2 JIT and the AOT backends: vm->d[] is authoritative,
 * ops are load-op-store through scratch xmm0/xmm1, and d operands never
 * enter the rc_* logical-register caches. d0 is a real register — no
 * zero-pinning, so d-writes never early-out on dst==0.
 * ========================================================================= */

static void emit_d_load(Emitter *e, uint8_t xmm, uint8_t log) {
    emit_movsd_xmm_r13disp32(e, xmm, d_disp(log));
}
static void emit_d_store(Emitter *e, uint8_t log, uint8_t xmm) {
    emit_movsd_r13disp32_xmm(e, d_disp(log), xmm);
}
static void emit_d_load_ab(Emitter *e, uint8_t a, uint8_t b) {
    emit_d_load(e, 0, a);
    emit_d_load(e, 1, b);
}

/* OP_DLDC : d[dst] = bits(lo,hi) — two payload words follow in the IR
 * stream. Two imm32 stores (little-endian: low dword first). */
static void emit_op_dldc(Emitter *e, uint8_t d, uint32_t lo, uint32_t hi) {
    emit_mov_r13disp32_imm32(e, d_disp(d),     lo);
    emit_mov_r13disp32_imm32(e, d_disp(d) + 4, hi);
}

/* OP_DMOV : d[dst] = d[a] — 64-bit copy through rax. */
static void emit_op_dmov(Emitter *e, uint8_t d, uint8_t a) {
    if(d == a) return;
    emit_mov_r64_r13disp32(e, R_RAX, d_disp(a));
    emit_mov_r13disp32_r64(e, d_disp(d), R_RAX);
}

/* OP_DNEG : d[dst] = -d[a] — copy, then flip the high-dword sign bit. */
static void emit_op_dneg(Emitter *e, uint8_t d, uint8_t a) {
    emit_op_dmov(e, d, a);
    emit_rmw_r13disp32_imm32(e, 6, d_disp(d) + 4, 0x80000000u);
}

/* OP_DFABS : d[dst] = fabs(d[a]) — copy, clear the sign bit. */
static void emit_op_dfabs(Emitter *e, uint8_t d, uint8_t a) {
    emit_op_dmov(e, d, a);
    emit_rmw_r13disp32_imm32(e, 4, d_disp(d) + 4, 0x7FFFFFFFu);
}

/* OP_DADD/DSUB/DMUL/DDIV : scalar SSE2 double arithmetic. */
static void emit_op_darith(Emitter *e, uint8_t sd_op,
                           uint8_t d, uint8_t a, uint8_t b) {
    emit_d_load_ab(e, a, b);
    emit_sse_sd_xmm0_xmm1(e, sd_op);
    emit_d_store(e, d, 0);
}
static void emit_op_dadd(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    emit_op_darith(e, 0x58, d, a, b);
}
static void emit_op_dsub(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    emit_op_darith(e, 0x5C, d, a, b);
}
static void emit_op_dmul(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    emit_op_darith(e, 0x59, d, a, b);
}
static void emit_op_ddiv(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    emit_op_darith(e, 0x5E, d, a, b);
}

/* OP_DSQRT : d[dst] = sqrt(d[a]) — inline sqrtsd, no libm call. */
static void emit_op_dsqrt(Emitter *e, uint8_t d, uint8_t a) {
    emit_d_load(e, 0, a);
    /* sqrtsd xmm0, xmm0  ;  F2 0F 51 C0 */
    em_u8(e, 0xF2); em_u8(e, 0x0F); em_u8(e, 0x51);
    em_u8(e, modrm(3, 0, 0));
    emit_d_store(e, d, 0);
}

/* OP_DCEQ : ordered equality (NaN -> false). Same flag decode as f32. */
static void emit_op_dceq(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_d_load_ab(e, a, b);
    emit_ucomisd_xmm0_xmm1(e);
    em_u8(e, 0x0F); em_u8(e, 0x9B); em_u8(e, modrm(3, 0, R_RAX));   /* setnp al */
    em_u8(e, 0x0F); em_u8(e, 0x94); em_u8(e, modrm(3, 0, R_RCX));   /* setz  cl */
    em_u8(e, 0x20); em_u8(e, modrm(3, R_RCX, R_RAX));               /* and al, cl */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, modrm(3, R_RAX, R_RAX));/* movzx eax,al */
    emit_store_logical(e, d, R_RAX);
}
/* OP_DCLT : a < b (NaN -> false). ucomisd xmm1, xmm0 ; seta al. */
static void emit_op_dclt(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_d_load_ab(e, a, b);
    emit_ucomisd_xmm1_xmm0(e);
    em_u8(e, 0x0F); em_u8(e, 0x97); em_u8(e, modrm(3, 0, R_RAX));   /* seta al */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, modrm(3, R_RAX, R_RAX));
    emit_store_logical(e, d, R_RAX);
}
/* OP_DCLE : a <= b (NaN -> false). ucomisd xmm1, xmm0 ; setae al. */
static void emit_op_dcle(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_d_load_ab(e, a, b);
    emit_ucomisd_xmm1_xmm0(e);
    em_u8(e, 0x0F); em_u8(e, 0x93); em_u8(e, modrm(3, 0, R_RAX));   /* setae al */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, modrm(3, R_RAX, R_RAX));
    emit_store_logical(e, d, R_RAX);
}

/* OP_DLD : d[dst] = f64 at mem[r[a] + imm12] */
static void emit_op_dld(Emitter *e, uint8_t d, uint8_t a, int32_t imm12) {
    emit_load_logical(e, R_RCX, a);
    emit_movsd_xmm_r14rcx(e, 0, imm12);
    emit_d_store(e, d, 0);
}
/* OP_DST : f64 at mem[r[dst] + imm12] = d[a]   (dst is the ADDRESS reg) */
static void emit_op_dst(Emitter *e, uint8_t addr_reg, uint8_t a, int32_t imm12) {
    emit_load_logical(e, R_RCX, addr_reg);
    emit_d_load(e, 0, a);
    emit_movsd_r14rcx_xmm(e, 0, imm12);
}

/* OP_DBITS_LO / OP_DBITS_HI : r[dst] = low/high dword of d[a]. */
static void emit_op_dbits(Emitter *e, uint8_t d, uint8_t a, int hi) {
    if(d == 0) return;
    emit_mov_r32_r13disp32(e, R_RAX, d_disp(a) + (hi ? 4 : 0));
    emit_store_logical(e, d, R_RAX);
}
/* OP_DFROMBITS : d[dst] = bits(hi = r[a], lo = r[b]). */
static void emit_op_dfrombits(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    emit_load_logical(e, R_RAX, a);              /* hi */
    emit_load_logical(e, R_RCX, b);              /* lo */
    emit_mov_r13disp32_r32(e, d_disp(d) + 4, R_RAX);
    emit_mov_r13disp32_r32(e, d_disp(d),     R_RCX);
}

/* OP_I2D : d[dst] = (double)(i32)r[a] */
static void emit_op_i2d(Emitter *e, uint8_t d, uint8_t a) {
    emit_load_logical(e, R_RAX, a);
    emit_cvtsi2sd_xmm0_eax(e);
    emit_d_store(e, d, 0);
}
/* OP_U2D : d[dst] = (double)(u32)r[a] — the r32 load zero-extends into
 * rax, then cvtsi2sd r64 sees the positive i64. */
static void emit_op_u2d(Emitter *e, uint8_t d, uint8_t a) {
    emit_load_logical(e, R_RAX, a);
    emit_cvtsi2sd_xmm0_rax(e);
    emit_d_store(e, d, 0);
}
/* OP_D2I : r[dst] = (i32)truncate(d[a]) */
static void emit_op_d2i(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_d_load(e, 0, a);
    emit_cvttsd2si_eax_xmm0(e);
    emit_store_logical(e, d, R_RAX);
}
/* OP_D2U : r[dst] = (i32)(u32)d[a] — 64-bit truncate, keep low dword. */
static void emit_op_d2u(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_d_load(e, 0, a);
    emit_cvttsd2si_rax_xmm0(e);
    emit_store_logical(e, d, R_RAX);
}
/* OP_F2D : d[dst] = (double)asfloat(r[a]) — always exact. */
static void emit_op_f2d(Emitter *e, uint8_t d, uint8_t a) {
    emit_fp_load_logical(e, 0, a);
    emit_cvtss2sd_xmm0_xmm0(e);
    emit_d_store(e, d, 0);
}
/* OP_D2F : r[dst] = asbits((float)d[a]) — round-nearest-even. */
static void emit_op_d2f(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_d_load(e, 0, a);
    emit_cvtsd2ss_xmm0_xmm0(e);
    emit_movd_r32_xmm(e, R_RAX, 0);
    emit_store_logical(e, d, R_RAX);
}

/* ---- f64 libm calls ----
 * The double-precision counterparts of libm calls, reached through a
 * lazily-initialised pointer table: MSVC declares libm functions as
 * __declspec(dllimport), whose addresses are not constant initialisers,
 * so the array is filled on first use rather than at file scope. The
 * pointers are the *same* functions the interpreter calls, so results
 * are bit-identical to --no-jit by construction.
 * Win64: double args in xmm0/xmm1, result xmm0. */
typedef double (*jit_dp_unary_fn)(double);
typedef double (*jit_dp_binary_fn)(double, double);

/* round-half-away, matching relic_dround_half_away / OP_SYS_ROUND. */
static double jit_dround(double x) {
    return x < 0.0 ? -floor(-x + 0.5) : floor(x + 0.5);
}

#define JIT_D1_COUNT 16
static jit_dp_unary_fn  g_d1_fn[JIT_D1_COUNT];
#define JIT_D2_COUNT 3
static jit_dp_binary_fn g_d2_fn[JIT_D2_COUNT];

static void jit_d_resolve(void) {
    if(g_d1_fn[0]) return;
    jit_dp_unary_fn d1[JIT_D1_COUNT] = {
        sin,   cos,   tan,   asin,  acos,  atan,
        exp,   log,   log10, floor, ceil,  jit_dround,
        fabs,  sinh,  cosh,  tanh };
    jit_dp_binary_fn d2[JIT_D2_COUNT] = { pow, atan2, fmod };
    memcpy(g_d1_fn, d1, sizeof d1);
    memcpy(g_d2_fn, d2, sizeof d2);
}

#define D1_SIN   0
#define D1_COS   1
#define D1_TAN   2
#define D1_ASIN  3
#define D1_ACOS  4
#define D1_ATAN  5
#define D1_EXP   6
#define D1_LOG   7
#define D1_LOG10 8
#define D1_FLOOR 9
#define D1_CEIL  10
#define D1_ROUND 11
/* index 12 (fabs) is resolved but unused — DFABS emits a bit-op. */
#define D1_SINH  13
#define D1_COSH  14
#define D1_TANH  15
#define D2_POW   0
#define D2_ATAN2 1
#define D2_FMOD  2

static void emit_op_dmath1(Emitter *e, int idx, uint8_t d, uint8_t a) {
    jit_d_resolve();
    rc_spill_all(e);
    emit_d_load(e, 0, a);
    /* movabs rax, g_d1_fn[idx] ; call rax */
    em_u8(e, REX_W); em_u8(e, 0xB8 | R_RAX);
    em_u64(e, (uint64_t)(uintptr_t)g_d1_fn[idx]);
    em_u8(e, 0xFF); em_u8(e, modrm(3, 2, R_RAX));
    emit_d_store(e, d, 0);
}
static void emit_op_dmath2(Emitter *e, int idx,
                           uint8_t d, uint8_t a, uint8_t b) {
    jit_d_resolve();
    rc_spill_all(e);
    emit_d_load(e, 0, a);
    emit_d_load(e, 1, b);
    em_u8(e, REX_W); em_u8(e, 0xB8 | R_RAX);
    em_u64(e, (uint64_t)(uintptr_t)g_d2_fn[idx]);
    em_u8(e, 0xFF); em_u8(e, modrm(3, 2, R_RAX));
    emit_d_store(e, d, 0);
}

/* =========================================================================
 *                          MAIN COMPILE PASS
 * ========================================================================= */

/* Is `op` a basic-block terminator? Terminators end a BB: control either
 * leaves (jmp/halt/ret) or transfers to a different scope (call). The
 * register cache must be fully spilled before emitting one of these. */
static int op_is_terminator(uint8_t op) {
    switch(op) {
        case OP_HALT:
        case OP_JMP: case OP_JT: case OP_JF: case OP_JMPR:
        case OP_CALL: case OP_CALLR: case OP_CALLP: case OP_RET:
            return 1;
        default:
            return 0;
    }
}

static int op_is_inlined(uint8_t op) {
    switch(op) {
        case OP_NOP:  case OP_HALT:
        case OP_LDC:  case OP_LDC32:  case OP_MOV:  case OP_LEA:
        case OP_ADD:  case OP_ADDI:   case OP_SUB:  case OP_SUBI:
        case OP_MUL:  case OP_DIV:    case OP_MOD:
        case OP_NEG:  case OP_NOT:
        case OP_AND:  case OP_OR:     case OP_XOR:
        case OP_ANDI: case OP_ORI:
        case OP_SHL:  case OP_SHR:    case OP_SAR:
        case OP_CEQ:  case OP_CNE:
        case OP_CLT:  case OP_CLE:
        case OP_CGT:  case OP_CGE:
        case OP_CLTU: case OP_CLEU:
        case OP_LD8:  case OP_LD16:   case OP_LD32:
        case OP_ST8:  case OP_ST16:   case OP_ST32:
        case OP_SX8:  case OP_SX16:
        case OP_ZX8:  case OP_ZX16:
        case OP_TRUNC8: case OP_TRUNC16:
        case OP_FADD: case OP_FSUB:   case OP_FMUL: case OP_FDIV:
        case OP_FNEG:
        case OP_FCEQ: case OP_FCLT:   case OP_FCLE:
        case OP_I2F:  case OP_F2I:
        case OP_DMOV: case OP_DNEG:   case OP_DFABS:
        case OP_DADD: case OP_DSUB:   case OP_DMUL: case OP_DDIV:
        case OP_DSQRT:
        case OP_DCEQ: case OP_DCLT:   case OP_DCLE:
        case OP_DLD:  case OP_DST:
        case OP_DLDC:
        case OP_DBITS_LO: case OP_DBITS_HI: case OP_DFROMBITS:
        case OP_I2D:  case OP_U2D:    case OP_D2I:  case OP_D2U:
        case OP_F2D:  case OP_D2F:
        case OP_DSIN: case OP_DCOS:   case OP_DTAN:
        case OP_DASIN: case OP_DACOS: case OP_DATAN: case OP_DATAN2:
        case OP_DEXP: case OP_DLOG:   case OP_DLOG10:
        case OP_DPOW: case OP_DFLOOR: case OP_DCEIL:
        case OP_DROUND: case OP_DFMOD:
        case OP_DSINH: case OP_DCOSH: case OP_DTANH:
        case OP_JMP:  case OP_JT:     case OP_JF:   case OP_JMPR:
        case OP_CALL: case OP_CALLR:  case OP_CALLP: case OP_RET:
        case OP_ENTER:case OP_LEAVE:
        case OP_PUSH: case OP_POP:
            return 1;
        default:
            return 0;
    }
}

static uint8_t setcc_for_cmp(uint8_t op) {
    switch(op) {
        case OP_CEQ:  return 0x94;
        case OP_CNE:  return 0x95;
        case OP_CLT:  return 0x9C;
        case OP_CLE:  return 0x9E;
        case OP_CGT:  return 0x9F;
        case OP_CGE:  return 0x9D;
        case OP_CLTU: return 0x92;
        case OP_CLEU: return 0x96;
        default:      return 0x00;
    }
}

static int32_t decode_jmp_target(uint32_t instr, int32_t pc) {
    return (int32_t)IR_JTARGET(instr, pc);
}

static bool x64_win_compile(IRBuffer *ir, RelicJitCode **out_code,
                            char *err, int err_sz) {
    if(!ir || ir->len <= 0) {
        if(err && err_sz > 0) {
            const char *m = "empty IR";
            int n = (int)strlen(m);
            if(n >= err_sz) n = err_sz - 1;
            memcpy(err, m, n); err[n] = 0;
        }
        return false;
    }

    void **dispatch = (void**)calloc((size_t)ir->len + 1, sizeof(void*));
    if(!dispatch) {
        if(err && err_sz > 0) {
            const char *m = "out of memory (dispatch table)";
            int n = (int)strlen(m); if(n >= err_sz) n = err_sz - 1;
            memcpy(err, m, n); err[n] = 0;
        }
        return false;
    }

    Emitter e;
    if(!em_init(&e, (size_t)ir->len)) {
        em_free(&e); free(dispatch);
        if(err && err_sz > 0) {
            const char *m = "out of memory (emitter)";
            int n = (int)strlen(m); if(n >= err_sz) n = err_sz - 1;
            memcpy(err, m, n); err[n] = 0;
        }
        return false;
    }
    e.addr_step1          = (uintptr_t)&relic_vm_step1;
    e.addr_dispatch_table = (uintptr_t)dispatch;

    uint8_t *is_branch_target = (uint8_t*)calloc((size_t)ir->len + 1, 1);
    uint8_t *is_fn_entry      = (uint8_t*)calloc((size_t)ir->len + 1, 1);
    if(!is_branch_target || !is_fn_entry) {
        free(is_branch_target); free(is_fn_entry);
        em_free(&e); free(dispatch);
        if(err && err_sz > 0) {
            const char *m = "out of memory (branch-target bitmap)";
            int n = (int)strlen(m); if(n >= err_sz) n = err_sz - 1;
            memcpy(err, m, n); err[n] = 0;
        }
        return false;
    }
    for(int32_t p = 0; p < ir->len; p++) {
        uint32_t w = ir->code[p];
        uint8_t  o = (uint8_t)((w >> 24) & 0xFF);
        if(o == OP_JMP || o == OP_JT || o == OP_JF) {
            int32_t t = decode_jmp_target(w, p);
            if(t >= 0 && t < ir->len) is_branch_target[t] = 1;
        } else if(o == OP_LDC32) {
            p++;
        } else if(o == OP_DLDC) {
            /* DLDC carries TWO payload words: skip both or a literal
             * half could decode as OP_JMP and mark a bogus target. */
            p += 2;
        }
    }
    for(int i = 0; i < ir->nfuncs; i++) {
        int32_t off = IRFNOFF(ir, i);
        if(off >= 0 && off < ir->len) {
            is_branch_target[off] = 1;
            is_fn_entry[off]      = 1;
        }
    }

    emit_prologue(&e);

    int32_t pc = 0;
    while(pc < ir->len) {
        /* BB entry: if this PC is reachable via external dispatch (a
         * branch target or fn entry), flush the cache BEFORE recording
         * the dispatch position. Two reasons:
         *   1. External jumpers arrive with empty cache state; the
         *      JIT code at dispatch[pc] must therefore start with the
         *      compile-time cache state also empty, otherwise loads
         *      from cache slots would return garbage.
         *   2. Putting the spill before em_record_pc means external
         *      entry skips the spill code (which only the fall-through
         *      path needs to execute). */
        if(is_branch_target[pc]) {
            rc_spill_all(&e);
        }
        em_record_pc(&e, pc);
        /* At every IR function entry, emit the per-function host-frame
         * setup (`sub rsp, 40`). The dispatch-table entry for this PC
         * points at the sub, so both direct OP_CALL (host `call rel32`)
         * and indirect CALLR/CALLP (host `call [dispatch]`) hit the
         * alignment-correct entry. */
        if(is_fn_entry[pc]) {
            emit_fn_entry_stub(&e);
        }
        uint32_t instr = ir->code[pc];
        uint8_t  op    = (uint8_t)((instr >> 24) & 0xFF);
        uint8_t  dst   = (uint8_t)((instr >> 20) & 0x0F);
        uint8_t  srca  = (uint8_t)((instr >> 16) & 0x0F);
        uint8_t  srcb  = (uint8_t)((instr >> 12) & 0x0F);
        int32_t  imm12 = (int32_t)((instr & 0xFFF) | ((instr & 0x800) ? 0xFFFFF000 : 0));

        if(!op_is_inlined(op)) {
            /* Step1 is a C call that reads/writes vm->r[] through memory.
             * Spill all cached logicals so memory is authoritative, then
             * dispatch. After step1 the cache is already empty. */
            rc_spill_all(&e);
            emit_step1_fallback(&e, pc);
            pc++;
            /* DLDC carries TWO literal payload words: step1 consumes them
             * itself, but the pc map must still skip both or the stream
             * desyncs and the payload gets decoded as code. */
            if(op == OP_DLDC) {
                if(pc < ir->len) e.pc_to_jit[pc] = e.pc_to_jit[pc - 1];
                pc++;
                if(pc < ir->len) e.pc_to_jit[pc] = e.pc_to_jit[pc - 1];
                pc++;
            }
            continue;
        }

        /* Fuse CMP + JT/JF */
        {
            uint8_t setcc_op = setcc_for_cmp(op);
            if(setcc_op && pc + 1 < ir->len && !is_branch_target[pc + 1]) {
                uint32_t nxt   = ir->code[pc + 1];
                uint8_t  nxt_o = (uint8_t)((nxt >> 24) & 0xFF);
                uint8_t  nxt_a = (uint8_t)((nxt >> 16) & 0x0F);
                if((nxt_o == OP_JT || nxt_o == OP_JF) && nxt_a == dst) {
                    int32_t target_pc = decode_jmp_target(nxt, pc + 1);
                    /* Fused cmp+jcc is a BB terminator. Spill before emit;
                     * the fused emitter uses emit_store_logical_to_mem for
                     * `dst` so the cache stays empty. */
                    rc_spill_all(&e);
                    emit_cmp_branch_fused(&e, setcc_op, dst, srca, srcb,
                                          (nxt_o == OP_JT) ? 1 : 0, target_pc);
                    pc++;
                    if(pc < ir->len) e.pc_to_jit[pc] = (int32_t)e.len;
                    pc++;
                    continue;
                }
            }
        }

        /* For BB terminators, flush the cache before emitting the op so
         * memory is consistent when control leaves this BB. */
        if(op_is_terminator(op)) {
            rc_spill_all(&e);
        }

        switch(op) {
            case OP_NOP:                                          break;
            case OP_HALT:  emit_op_halt (&e, srca, imm12);        break;
            case OP_LDC: {
                int32_t imm20 = (int32_t)((instr & 0xFFFFF) |
                                          ((instr & 0x80000) ? 0xFFF00000 : 0));
                emit_op_ldc(&e, dst, imm20);
                break;
            }
            case OP_LDC32: {
                uint32_t lit = (pc + 1 < ir->len) ? ir->code[pc + 1] : 0;
                emit_op_ldc32(&e, dst, lit);
                pc++;
                if(pc < ir->len) e.pc_to_jit[pc] = e.pc_to_jit[pc - 1];
                break;
            }
            case OP_MOV:   emit_op_mov  (&e, dst, srca);          break;
            case OP_LEA:   emit_op_lea  (&e, dst, srca, imm12);   break;

            case OP_ADD:   emit_op_add  (&e, dst, srca, srcb);    break;
            case OP_ADDI:  emit_op_addi (&e, dst, srca, imm12);   break;
            case OP_SUB:   emit_op_sub  (&e, dst, srca, srcb);    break;
            case OP_SUBI:  emit_op_subi (&e, dst, srca, imm12);   break;
            case OP_MUL:   emit_op_mul  (&e, dst, srca, srcb);    break;
            case OP_DIV:   emit_op_div  (&e, dst, srca, srcb);    break;
            case OP_MOD:   emit_op_mod  (&e, dst, srca, srcb);    break;
            case OP_NEG:   emit_op_neg  (&e, dst, srca);          break;
            case OP_NOT:   emit_op_not  (&e, dst, srca);          break;

            case OP_AND:   emit_op_and  (&e, dst, srca, srcb);    break;
            case OP_OR:    emit_op_or   (&e, dst, srca, srcb);    break;
            case OP_XOR:   emit_op_xor  (&e, dst, srca, srcb);    break;
            case OP_ANDI:  emit_op_andi (&e, dst, srca, imm12);   break;
            case OP_ORI:   emit_op_ori  (&e, dst, srca, imm12);   break;
            case OP_SHL:   emit_op_shl  (&e, dst, srca, srcb);    break;
            case OP_SHR:   emit_op_shr  (&e, dst, srca, srcb);    break;
            case OP_SAR:   emit_op_sar  (&e, dst, srca, srcb);    break;

            case OP_SX8:   emit_op_sx8  (&e, dst, srca);          break;
            case OP_SX16:  emit_op_sx16 (&e, dst, srca);          break;
            case OP_ZX8:   emit_op_zx8  (&e, dst, srca);          break;
            case OP_ZX16:  emit_op_zx16 (&e, dst, srca);          break;
            case OP_TRUNC8: emit_op_trunc8 (&e, dst, srca);       break;
            case OP_TRUNC16:emit_op_trunc16(&e, dst, srca);       break;

            case OP_FADD:  emit_op_fadd (&e, dst, srca, srcb);    break;
            case OP_FSUB:  emit_op_fsub (&e, dst, srca, srcb);    break;
            case OP_FMUL:  emit_op_fmul (&e, dst, srca, srcb);    break;
            case OP_FDIV:  emit_op_fdiv (&e, dst, srca, srcb);    break;
            case OP_FNEG:  emit_op_fneg (&e, dst, srca);          break;
            case OP_FCEQ:  emit_op_fceq (&e, dst, srca, srcb);    break;
            case OP_FCLT:  emit_op_fclt (&e, dst, srca, srcb);    break;
            case OP_FCLE:  emit_op_fcle (&e, dst, srca, srcb);    break;
            case OP_I2F:   emit_op_i2f  (&e, dst, srca);          break;
            case OP_F2I:   emit_op_f2i  (&e, dst, srca);          break;

            /* ---- f64 (d-bank) ops: dst/srca/srcb are d-register indices
             * for the D* ops (they index vm->d[], not the r-bank).
             * Exceptions: DBITS_LO/HI write an r-reg dst; DFROMBITS and
             * I2D/U2D/D2I/D2U/F2D read r-regs; DLD's srca and DST's dst
             * are r-bank address regs. */
            case OP_DMOV:  emit_op_dmov  (&e, dst, srca);         break;
            case OP_DNEG:  emit_op_dneg  (&e, dst, srca);         break;
            case OP_DFABS: emit_op_dfabs (&e, dst, srca);         break;
            case OP_DADD:  emit_op_dadd  (&e, dst, srca, srcb);   break;
            case OP_DSUB:  emit_op_dsub  (&e, dst, srca, srcb);   break;
            case OP_DMUL:  emit_op_dmul  (&e, dst, srca, srcb);   break;
            case OP_DDIV:  emit_op_ddiv  (&e, dst, srca, srcb);   break;
            case OP_DSQRT: emit_op_dsqrt (&e, dst, srca);         break;
            case OP_DCEQ:  emit_op_dceq  (&e, dst, srca, srcb);   break;
            case OP_DCLT:  emit_op_dclt  (&e, dst, srca, srcb);   break;
            case OP_DCLE:  emit_op_dcle  (&e, dst, srca, srcb);   break;
            case OP_DLD:   emit_op_dld   (&e, dst, srca, imm12);  break;
            case OP_DST:   emit_op_dst   (&e, dst, srca, imm12);  break;
            case OP_DLDC: {
                uint32_t lo = (pc + 1 < ir->len) ? ir->code[pc + 1] : 0;
                uint32_t hi = (pc + 2 < ir->len) ? ir->code[pc + 2] : 0;
                emit_op_dldc(&e, dst, lo, hi);
                pc += 2;
                if(pc - 1 < ir->len)
                    e.pc_to_jit[pc - 1] = e.pc_to_jit[pc - 2];
                if(pc < ir->len)
                    e.pc_to_jit[pc]     = e.pc_to_jit[pc - 1];
                break;
            }
            case OP_DBITS_LO: emit_op_dbits(&e, dst, srca, 0);    break;
            case OP_DBITS_HI: emit_op_dbits(&e, dst, srca, 1);    break;
            case OP_DFROMBITS:
                emit_op_dfrombits(&e, dst, srca, srcb);           break;
            case OP_I2D:   emit_op_i2d   (&e, dst, srca);         break;
            case OP_U2D:   emit_op_u2d   (&e, dst, srca);         break;
            case OP_D2I:   emit_op_d2i   (&e, dst, srca);         break;
            case OP_D2U:   emit_op_d2u   (&e, dst, srca);         break;
            case OP_F2D:   emit_op_f2d   (&e, dst, srca);         break;
            case OP_D2F:   emit_op_d2f   (&e, dst, srca);         break;
            case OP_DSIN:   emit_op_dmath1(&e, D1_SIN,   dst, srca); break;
            case OP_DCOS:   emit_op_dmath1(&e, D1_COS,   dst, srca); break;
            case OP_DTAN:   emit_op_dmath1(&e, D1_TAN,   dst, srca); break;
            case OP_DASIN:  emit_op_dmath1(&e, D1_ASIN,  dst, srca); break;
            case OP_DACOS:  emit_op_dmath1(&e, D1_ACOS,  dst, srca); break;
            case OP_DATAN:  emit_op_dmath1(&e, D1_ATAN,  dst, srca); break;
            case OP_DATAN2: emit_op_dmath2(&e, D2_ATAN2, dst, srca, srcb); break;
            case OP_DEXP:   emit_op_dmath1(&e, D1_EXP,   dst, srca); break;
            case OP_DLOG:   emit_op_dmath1(&e, D1_LOG,   dst, srca); break;
            case OP_DLOG10: emit_op_dmath1(&e, D1_LOG10, dst, srca); break;
            case OP_DPOW:   emit_op_dmath2(&e, D2_POW,   dst, srca, srcb); break;
            case OP_DFLOOR: emit_op_dmath1(&e, D1_FLOOR, dst, srca); break;
            case OP_DCEIL:  emit_op_dmath1(&e, D1_CEIL,  dst, srca); break;
            case OP_DROUND: emit_op_dmath1(&e, D1_ROUND, dst, srca); break;
            case OP_DFMOD:  emit_op_dmath2(&e, D2_FMOD,  dst, srca, srcb); break;
            case OP_DSINH:  emit_op_dmath1(&e, D1_SINH,  dst, srca); break;
            case OP_DCOSH:  emit_op_dmath1(&e, D1_COSH,  dst, srca); break;
            case OP_DTANH:  emit_op_dmath1(&e, D1_TANH,  dst, srca); break;

            case OP_PUSH:  emit_op_push (&e, srca);               break;
            case OP_POP:   emit_op_pop  (&e, dst);                break;

            case OP_CEQ:   emit_cmp_setcc(&e, 0x94, dst, srca, srcb); break;
            case OP_CNE:   emit_cmp_setcc(&e, 0x95, dst, srca, srcb); break;
            case OP_CLT:   emit_cmp_setcc(&e, 0x9C, dst, srca, srcb); break;
            case OP_CLE:   emit_cmp_setcc(&e, 0x9E, dst, srca, srcb); break;
            case OP_CGT:   emit_cmp_setcc(&e, 0x9F, dst, srca, srcb); break;
            case OP_CGE:   emit_cmp_setcc(&e, 0x9D, dst, srca, srcb); break;
            case OP_CLTU:  emit_cmp_setcc(&e, 0x92, dst, srca, srcb); break;
            case OP_CLEU:  emit_cmp_setcc(&e, 0x96, dst, srca, srcb); break;

            case OP_LD8:   emit_op_ld8 (&e, dst, srca, imm12);    break;
            case OP_LD16:  emit_op_ld16(&e, dst, srca, imm12);    break;
            case OP_LD32:  emit_op_ld32(&e, dst, srca, imm12);    break;
            case OP_ST8:   emit_op_st8 (&e, dst, srca, imm12);    break;
            case OP_ST16:  emit_op_st16(&e, dst, srca, imm12);    break;
            case OP_ST32:  emit_op_st32(&e, dst, srca, imm12);    break;

            case OP_JMP:   emit_op_jmp  (&e, decode_jmp_target(instr, pc));        break;
            case OP_JT:    emit_op_jcond(&e, 0x85, srca, decode_jmp_target(instr, pc)); break;
            case OP_JF:    emit_op_jcond(&e, 0x84, srca, decode_jmp_target(instr, pc)); break;
            case OP_JMPR:  emit_op_jmpr (&e, dst);                break;

            case OP_CALL: {
                uint32_t fn_id = instr & 0x00FFFFFFu;
                int32_t target = (fn_id < (uint32_t)ir->nfuncs)
                                 ? (int32_t)IRFNOFF(ir, fn_id)
                                 : pc + 1;
                emit_op_call(&e, target, pc + 1);
                break;
            }
            case OP_CALLR:
            case OP_CALLP:
                emit_op_callr(&e, dst, pc + 1, ir->nfuncs,
                              (uintptr_t)&IRFNOFF(ir, 0));
                break;
            case OP_RET:   emit_op_ret  (&e);                     break;
            case OP_ENTER: emit_op_enter(&e, (int32_t)(instr & 0xFFFFFu)); break;
            case OP_LEAVE: emit_op_leave(&e);                     break;

            default:
                emit_step1_fallback(&e, pc);
                break;
        }

        pc++;
    }

    emit_epilogue(&e);

    if(e.oom) goto oom;

    SIZE_T page_sz = e.len;
    void *page = VirtualAlloc(NULL, page_sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if(!page) {
        if(err && err_sz > 0) {
            _snprintf_s(err, err_sz, _TRUNCATE,
                        "VirtualAlloc failed (%lu)", (unsigned long)GetLastError());
        }
        em_free(&e); free(dispatch); free(is_branch_target);
        return false;
    }
    memcpy(page, e.buf, e.len);

    for(size_t i = 0; i < e.relocs_len; i++) {
        Reloc *r = &e.relocs[i];
        int32_t target_off;
        if(r->target_pc == (int32_t)e.pc_count) {
            target_off = e.epilogue_off;
        } else if(r->target_pc < 0 || r->target_pc >= (int32_t)e.pc_count) {
            target_off = e.epilogue_off;
        } else {
            target_off = e.pc_to_jit[r->target_pc];
            if(target_off < 0) target_off = e.epilogue_off;
        }
        int32_t disp = (int32_t)target_off - (int32_t)(r->jit_off + 4);
        memcpy((uint8_t*)page + r->jit_off, &disp, 4);
    }

    for(size_t i = 0; i < e.pc_count; i++) {
        int32_t off = e.pc_to_jit[i];
        if(off < 0) {
            dispatch[i] = (uint8_t*)page + e.epilogue_off;
        } else {
            dispatch[i] = (uint8_t*)page + off;
        }
    }
    dispatch[e.pc_count] = (uint8_t*)page + e.epilogue_off;

    em_free(&e);
    free(is_branch_target);
    free(is_fn_entry);

    DWORD old_prot = 0;
    if(!VirtualProtect(page, page_sz, PAGE_EXECUTE_READ, &old_prot)) {
        if(err && err_sz > 0) {
            _snprintf_s(err, err_sz, _TRUNCATE,
                        "VirtualProtect failed (%lu)", (unsigned long)GetLastError());
        }
        VirtualFree(page, 0, MEM_RELEASE);
        free(dispatch);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), page, page_sz);

    RelicJitCode *jc = (RelicJitCode*)calloc(1, sizeof(*jc));
    if(!jc) {
        VirtualFree(page, 0, MEM_RELEASE);
        free(dispatch);
        goto oom_post;
    }
    jc->code         = page;
    jc->code_sz      = page_sz;
    jc->dispatch     = dispatch;
    jc->dispatch_len = (size_t)ir->len + 1;
    jc->entry        = (int (*)(RelicVM*))page;
    *out_code = jc;
    return true;

oom:
    em_free(&e);
    free(dispatch);
    free(is_branch_target);
    free(is_fn_entry);
oom_post:
    if(err && err_sz > 0) {
        const char *m = "out of memory while assembling JIT code";
        int n = (int)strlen(m); if(n >= err_sz) n = err_sz - 1;
        memcpy(err, m, n); err[n] = 0;
    }
    return false;
}

static int x64_win_run(RelicJitCode *code, RelicVM *vm) {
    return code->entry(vm);
}

static void x64_win_free(RelicJitCode *code) {
    if(!code) return;
    if(code->code) VirtualFree(code->code, 0, MEM_RELEASE);
    free(code->dispatch);
    free(code);
}

#endif /* X64_WIN_HAVE_TARGET */

RelicJit relic_jit = {
    .name    = "x64-win",
    .compile = x64_win_compile,
    .run     = x64_win_run,
    .free    = x64_win_free,
};
