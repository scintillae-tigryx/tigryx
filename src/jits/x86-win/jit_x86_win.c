/*
 * jit_x86_win.c
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
 * jit_x86_win.c - Whole-program JIT for Relic targeting i386 / Windows.
 *
 * Per-IR-word emission with safety-net fallback.
 *
 * Layout produced by compile():
 *
 *   +-----------------------------+   <- code base (PAGE_EXECUTE_READ)
 *   | prologue (set up ebx/ebp/   |
 *   |          esi/edi)           |
 *   +-----------------------------+
 *   | jump to dispatch[entry_pc]  |   <- after prologue, hand off to first IR-word
 *   +-----------------------------+
 *   | per-IR-word emitted code    |
 *   |   pc=0 ...                  |
 *   |   pc=1 ...                  |
 *   |   ...                       |
 *   +-----------------------------+
 *   | epilogue (restore, ret)     |
 *   +-----------------------------+
 *
 *   And, separately, on the C heap:
 *
 *   dispatch[ir->len + 1] : void* table indexed by IR-word, used by
 *                           OP_RET / OP_CALLR / OP_JMPR for indirect
 *                           dispatch and by step1-fallback to resume at
 *                           wherever the interpreter left vm->pc. Slot
 *                           [ir->len] points at the epilogue (sentinel
 *                           used when main() returns).
 *
 * Register conventions inside the JIT'd body (Win32 cdecl, all callee-saved):
 *
 *      ebx = RelicVM*
 *      ebp = &vm->r[0]
 *      esi = vm->mem
 *      edi = vm->backend
 *
 * Logical-register access:
 *
 *      r0      always 0 - reads emit `xor reg, reg`, writes are dropped.
 *      r1..r13 live in [ebp + 4*N] as 32-bit slots in vm->r[].
 *      r14     aliases vm->fp - reads/writes go through [ebx + offsetof(fp)].
 *      r15     aliases vm->sp - reads/writes go through [ebx + offsetof(sp)].
 *
 * For correctness, any opcode the JIT does not yet inline natively is
 * compiled to a "step1 fallback": store current pc, call relic_vm_step1,
 * check vm->running, then indirect-jump through `dispatch[vm->pc]`. The
 * fallback covers semantics 1:1 with the interpreter at the cost of one
 * function call per non-inlined op. As the inline set grows, the
 * fallback path naturally shrinks.
 *
 * Build guard: real codegen is only enabled on a 32-bit Windows build.
 * On any other configuration compile() returns false and the runner falls
 * back to the interpreter (same behaviour as the null JIT).
 */
#include "tigryx.h"
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#if defined(_WIN32) && !defined(_WIN64)
#  define X86_WIN_HAVE_TARGET 1
#endif

#if X86_WIN_HAVE_TARGET
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

/* -------------------------------------------------------------------------
 * RelicJitCode: opaque to the rest of relic.
 * ---------------------------------------------------------------------- */
struct RelicJitCode {
    void   *code;            /* VirtualAlloc'd PAGE_EXECUTE_READ buffer    */
    size_t  code_sz;         /* byte length of `code`                       */
    void  **dispatch;        /* heap: IR-word -> absolute JIT addr; len+1   */
    size_t  dispatch_len;    /* number of slots (= ir->len + 1)             */
    int    (__cdecl *entry)(RelicVM *vm);   /* typed entry pointer          */
};

#if !X86_WIN_HAVE_TARGET
/* -------------------------------------------------------------------------
 * Fallback stubs for non-Win32 builds. Keeps the symbol table happy so the
 * x86-win JIT folder can still be selected on a wrong host without a link
 * error - it just declines to compile at runtime.
 * ---------------------------------------------------------------------- */
static bool x86_win_compile(IRBuffer *ir, RelicJitCode **out_code,
                            char *err, int err_sz) {
    (void)ir; (void)out_code;
    const char *msg = "x86-win JIT requires a 32-bit Windows build";
    if(err && err_sz > 0) {
        int n = (int)strlen(msg);
        if(n >= err_sz) n = err_sz - 1;
        memcpy(err, msg, n); err[n] = 0;
    }
    return false;
}
static int  x86_win_run(RelicJitCode *c, RelicVM *vm){ (void)c;(void)vm; return 1; }
static void x86_win_free(RelicJitCode *c)            { (void)c; }

#else /* X86_WIN_HAVE_TARGET --------------------------------------------- */

/* =========================================================================
 *                            EMITTER
 * ========================================================================= */

typedef enum {
    RELOC_REL32_AT_PC,   /* rel32 displacement to dispatch[target_pc] for
                            direct branches (JMP/JT/JF/CALL). The 4-byte
                            slot to patch sits at jit_off..+4. The reloc
                            rewrites it to (pc_to_jit[target_pc] - (jit_off+4)). */
} RelocKind;

typedef struct {
    uint32_t  jit_off;     /* offset of the 4-byte rel32 slot in code buffer  */
    int32_t   target_pc;   /* IR-word index of branch target                  */
    RelocKind kind;
} Reloc;

typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    int      oom;          /* sticky: any allocation failure latches here */

    /* pc_to_jit[i] = byte offset in `buf` where IR word `i` was emitted.
     * Filled in as we walk the IR. -1 = not (yet) emitted (only happens
     * after-the-fact for words we skipped, e.g. the literal word that
     * follows OP_LDC32 - we set it to its parent's offset). */
    int32_t *pc_to_jit;
    size_t   pc_count;     /* = ir->len */

    Reloc   *relocs;
    size_t   relocs_len;
    size_t   relocs_cap;

    /* Pre-resolved baked addresses (filled in compile()). */
    uintptr_t addr_step1;
    uintptr_t addr_dispatch_table;   /* address of `dispatch[0]` (the heap array) */
    int32_t   epilogue_off;          /* byte offset of epilogue label in buf  */
} Emitter;

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
 *                LOGICAL <-> PHYSICAL REGISTER HELPERS
 *
 * These are the only places that know about the r0/r14/r15 special cases.
 * `phys` is the x86 register encoding (0=eax, 1=ecx, 2=edx, 3=ebx, etc.).
 * ========================================================================= */

#define R_EAX 0
#define R_ECX 1
#define R_EDX 2
#define R_EBX 3
#define R_ESP 4   /* unused as ModR/M reg */
#define R_EBP 5
#define R_ESI 6
#define R_EDI 7

/* Pin logical r14 (frame pointer) to physical edi.
 * The prologue loads edi from vm->fp; the epilogue writes it back.
 * Any path that exits the JIT to C (step1 fallback) must spill+reload
 * edi around the call - see emit_step1_fallback. */
#define R_FP_PIN R_EDI

/* Emit `mov phys_reg, [ebp + 4*log_idx]` for log in 1..13.
 * For r0: emits `xor reg, reg`.
 * For r14: emits `mov reg, edi` (pinned).
 * For r15: emits `mov reg, [ebx + offsetof(sp)]`. */
static void emit_load_logical(Emitter *e, uint8_t phys, uint8_t log) {
    if(log == 0) {
        /* xor reg32, reg32   ->   31 c0 (modrm = 11 reg reg) */
        em_u8(e, 0x31);
        em_u8(e, (uint8_t)(0xC0 | (phys << 3) | phys));
        return;
    }
    if(log == 14) {
        /* mov phys, edi   ->   8B /r mod=11 reg=phys r/m=edi */
        em_u8(e, 0x8B);
        em_u8(e, (uint8_t)(0xC0 | (phys << 3) | R_FP_PIN));
        return;
    }
    if(log == 15) {
        em_u8(e, 0x8B);
        em_u8(e, (uint8_t)(0x80 | (phys << 3) | R_EBX));
        em_u32(e, (uint32_t)offsetof(RelicVM, sp));
        return;
    }
    /* mov reg, [ebp + 4*log]   ->   8B /r disp8 */
    em_u8(e, 0x8B);
    em_u8(e, (uint8_t)(0x40 | (phys << 3) | R_EBP));       /* mod=01 r/m=ebp */
    em_u8(e, (uint8_t)(4 * log));
}

/* Emit `mov [ebp + 4*log_idx], phys_reg` for log in 1..13.
 * For r0: emits nothing (writes are dropped).
 * For r14: writes go to pinned edi.
 * For r15: writes go to vm->sp memory. */
static void emit_store_logical(Emitter *e, uint8_t log, uint8_t phys) {
    if(log == 0) return;
    if(log == 14) {
        /* mov edi, phys   ->   89 /r mod=11 reg=phys r/m=edi */
        em_u8(e, 0x89);
        em_u8(e, (uint8_t)(0xC0 | (phys << 3) | R_FP_PIN));
        return;
    }
    if(log == 15) {
        em_u8(e, 0x89);
        em_u8(e, (uint8_t)(0x80 | (phys << 3) | R_EBX));
        em_u32(e, (uint32_t)offsetof(RelicVM, sp));
        return;
    }
    em_u8(e, 0x89);
    em_u8(e, (uint8_t)(0x40 | (phys << 3) | R_EBP));
    em_u8(e, (uint8_t)(4 * log));
}

/* Forward decl: the x87 control-word normalize lives with the f64 ops
 * but must also run at JIT entry and after every C-call boundary. */
#if defined(RELIC_WIN95) && !defined(RELIC_SOFTFP)
static void emit_fcw_dbl(Emitter *e);
#endif

/* =========================================================================
 *                    PROLOGUE / EPILOGUE / DISPATCH
 * ========================================================================= */

static void emit_prologue(Emitter *e) {
    em_u8(e, 0x55);   /* push ebp */
    em_u8(e, 0x53);   /* push ebx */
    em_u8(e, 0x56);   /* push esi */
    em_u8(e, 0x57);   /* push edi */

    /* mov ebx, [esp + 20]    ; ebx = vm (the cdecl arg) */
    em_u8(e, 0x8B); em_u8(e, 0x5C); em_u8(e, 0x24); em_u8(e, 0x14);

    /* lea ebp, [ebx + offsetof(r)]   ; ebp = &vm->r[0] */
    em_u8(e, 0x8D); em_u8(e, 0xAB); em_u32(e, (uint32_t)offsetof(RelicVM, r));
    /* mov esi, [ebx + offsetof(mem)] */
    em_u8(e, 0x8B); em_u8(e, 0xB3); em_u32(e, (uint32_t)offsetof(RelicVM, mem));
    /* mov edi, [ebx + offsetof(fp)]  ; load pinned fp */
    em_u8(e, 0x8B); em_u8(e, 0xBB); em_u32(e, (uint32_t)offsetof(RelicVM, fp));
#if defined(RELIC_WIN95) && !defined(RELIC_SOFTFP)
    emit_fcw_dbl(e);   /* x87: 53-bit significand for f64 parity */
#endif
}

/* Mark the current emitter offset as the epilogue's entry point and emit it.
 * Inline jumps to the epilogue (e.g. from OP_HALT, fallback bail-out, the
 * end-of-program sentinel) all jmp to this label. */
static void emit_epilogue(Emitter *e) {
    e->epilogue_off = (int32_t)e->len;

    /* Spill pinned fp (edi) back to vm->fp before returning so the
     * interpreter observes our final frame pointer. */
    em_u8(e, 0x89); em_u8(e, 0xBB); em_u32(e, (uint32_t)offsetof(RelicVM, fp));

    /* mov eax, [ebx + offsetof(exit_code)] */
    em_u8(e, 0x8B); em_u8(e, 0x83); em_u32(e, (uint32_t)offsetof(RelicVM, exit_code));

    em_u8(e, 0x5F);   /* pop edi */
    em_u8(e, 0x5E);   /* pop esi */
    em_u8(e, 0x5B);   /* pop ebx */
    em_u8(e, 0x5D);   /* pop ebp */
    em_u8(e, 0xC3);   /* ret */
}

/* Emit indirect dispatch through the heap-resident `dispatch[]` table:
 *      mov ecx, [ebx + offsetof(pc)]
 *      jmp dword ptr [dispatch_table + 4*ecx]
 * Used by RET/CALLR/JMPR and by the fallback after step1.
 * Caller must have already arranged for vm->pc to be the IR-word index
 * of the resume target (or, for RET, popped from vm-stack into a phys reg). */
static void emit_dispatch_via_pc(Emitter *e) {
    /* mov ecx, [ebx + offsetof(pc)] */
    em_u8(e, 0x8B); em_u8(e, 0x8B); em_u32(e, (uint32_t)offsetof(RelicVM, pc));
    /* jmp dword ptr [imm32 + 4*ecx]   ->   FF /4 mod=00 r/m=100 sib */
    em_u8(e, 0xFF); em_u8(e, 0x24); em_u8(e, 0x8D);
    em_u32(e, (uint32_t)e->addr_dispatch_table);
}

/* =========================================================================
 *                       STEP-1 FALLBACK
 *
 * Used for any opcode the JIT does not yet emit natively. Emits roughly:
 *
 *      mov dword ptr [ebx + offsetof(pc)], <ir_pc>
 *      push ebx
 *      mov  eax, &relic_vm_step1
 *      call eax
 *      add  esp, 4
 *      cmp  byte ptr [ebx + offsetof(running)], 0
 *      je   epilogue
 *      mov  ecx, [ebx + offsetof(pc)]
 *      jmp  dword ptr [dispatch + 4*ecx]
 *
 * step1 advances vm->pc internally, so after the call vm->pc points at
 * whatever the next instruction should be. We dispatch there.
 * ========================================================================= */
static void emit_step1_fallback(Emitter *e, int32_t ir_pc) {
    /* mov dword ptr [ebx + offsetof(pc)], imm32(ir_pc) */
    em_u8(e, 0xC7); em_u8(e, 0x83);
    em_u32(e, (uint32_t)offsetof(RelicVM, pc));
    em_u32(e, (uint32_t)ir_pc);

    /* Spill pinned fp (edi) to memory so step1 sees the current
     * frame pointer. OP_CALLR/OP_CALLP run through step1 and modify fp. */
    em_u8(e, 0x89); em_u8(e, 0xBB); em_u32(e, (uint32_t)offsetof(RelicVM, fp));

    /* push ebx ; mov eax, &step1 ; call eax ; add esp, 4 */
    em_u8(e, 0x53);
    em_u8(e, 0xB8); em_u32(e, (uint32_t)e->addr_step1);
    em_u8(e, 0xFF); em_u8(e, 0xD0);
    em_u8(e, 0x83); em_u8(e, 0xC4); em_u8(e, 0x04);

    /* Reload pinned fp - step1 may have modified vm->fp via CALLR/CALLP. */
    em_u8(e, 0x8B); em_u8(e, 0xBB); em_u32(e, (uint32_t)offsetof(RelicVM, fp));

#if defined(RELIC_WIN95) && !defined(RELIC_SOFTFP)
    emit_fcw_dbl(e);   /* step1 is a C call - it may leave FCW dirty */
#endif

    /* cmp byte ptr [ebx + offsetof(running)], 0   ->   80 /7 disp32 ib */
    em_u8(e, 0x80); em_u8(e, 0xBB);
    em_u32(e, (uint32_t)offsetof(RelicVM, running));
    em_u8(e, 0x00);

    /* jne next  (5 bytes); je would be simpler but we jump-on-eq to epilogue.
     * Use: je rel32 to epilogue. We patch the rel32 with a "self-relative" by
     * recording an internal reloc kind... but simpler: emit an unconditional
     * jmp-to-epilogue placeholder behind a short jcc.
     *
     * Plan:
     *      jne +5         ; skip the jmp epilogue if running
     *      jmp epilogue   ; rel32 (epilogue_reloc handled below)
     */
    /* jne short +5  ->  75 05 */
    em_u8(e, 0x75); em_u8(e, 0x05);

    /* jmp rel32 to epilogue (resolved in pass 2 against epilogue_off via
     * a "target_pc = pc_count" sentinel reloc - dispatch[pc_count] points
     * at the epilogue). */
    em_u8(e, 0xE9);
    em_add_reloc(e, (uint32_t)e->len, (int32_t)e->pc_count, RELOC_REL32_AT_PC);
    em_u32(e, 0);

    /* Otherwise fall through and dispatch on vm->pc */
    emit_dispatch_via_pc(e);
}

/* =========================================================================
 *                       INLINED OP EMITTERS
 *
 * Each emitter writes the x86 sequence equivalent to one IR-word's switch
 * case in vm.c. They use [ebp + 4*N] for r1..r13 and special-case r0/14/15
 * via the helpers above.
 * ========================================================================= */

/* OP_NOP : no code at all (fall through). */
static void RELIC_UNUSED emit_op_nop(Emitter *e) { (void)e; }

/* OP_HALT : src_a != 0 -> exit code from r[src_a] (@exit stages its arg
 * in r1); else imm12 (hand-written `HALT n`). running = 0 ; jmp epilogue */
static void emit_op_halt(Emitter *e, uint8_t src_a, int32_t imm12) {
    if(src_a) {
        emit_load_logical(e, R_EAX, src_a);
        /* mov dword ptr [ebx + offsetof(exit_code)], eax */
        em_u8(e, 0x89); em_u8(e, 0x83);
        em_u32(e, (uint32_t)offsetof(RelicVM, exit_code));
    } else {
        /* mov dword ptr [ebx + offsetof(exit_code)], imm32(imm12) */
        em_u8(e, 0xC7); em_u8(e, 0x83);
        em_u32(e, (uint32_t)offsetof(RelicVM, exit_code));
        em_u32(e, (uint32_t)imm12);
    }

    /* mov byte ptr [ebx + offsetof(running)], 0 */
    em_u8(e, 0xC6); em_u8(e, 0x83);
    em_u32(e, (uint32_t)offsetof(RelicVM, running));
    em_u8(e, 0x00);

    /* jmp rel32 to epilogue (target_pc = pc_count sentinel) */
    em_u8(e, 0xE9);
    em_add_reloc(e, (uint32_t)e->len, (int32_t)e->pc_count, RELOC_REL32_AT_PC);
    em_u32(e, 0);
}

/* OP_LDC dst, imm20 : reg_w(dst, imm20) */
static void emit_op_ldc(Emitter *e, uint8_t dst, int32_t imm20) {
    if(dst == 0) return;
    /* mov eax, imm32 */
    em_u8(e, 0xB8); em_u32(e, (uint32_t)imm20);
    emit_store_logical(e, dst, R_EAX);
}

/* OP_LDC32 dst, imm32 : reg_w(dst, imm32) - imm32 is the next IR word. */
static void emit_op_ldc32(Emitter *e, uint8_t dst, uint32_t imm32) {
    if(dst == 0) return;
    em_u8(e, 0xB8); em_u32(e, imm32);
    emit_store_logical(e, dst, R_EAX);
}

/* OP_MOV dst, src */
static void emit_op_mov(Emitter *e, uint8_t dst, uint8_t src_a) {
    if(dst == 0) return;
    emit_load_logical(e, R_EAX, src_a);
    emit_store_logical(e, dst, R_EAX);
}

/* OP_LEA dst, srcA, imm12 : reg_w(dst, a + imm12) */
static void emit_op_lea(Emitter *e, uint8_t dst, uint8_t src_a, int32_t imm12) {
    if(dst == 0) return;
    emit_load_logical(e, R_EAX, src_a);
    /* add eax, imm32 */
    em_u8(e, 0x05); em_u32(e, (uint32_t)imm12);
    emit_store_logical(e, dst, R_EAX);
}

/* Helper for the binary "op dst, a, b" pattern using:
 *   mov eax, [a]
 *   <op> eax, [b]
 *   mov [dst], eax
 * `op_byte` is the opcode for "op eax, r/m32" (e.g. 0x03 for ADD, 0x2B SUB,
 * 0x23 AND, 0x0B OR, 0x33 XOR). */
static void emit_bin_rm32(Emitter *e, uint8_t op_byte,
                          uint8_t dst, uint8_t a, uint8_t b) {
    if(dst == 0) {
        /* still potentially has side effects? No - pure data ops. Skip. */
        return;
    }
    emit_load_logical(e, R_EAX, a);
    /* <op_byte> eax, <src_b> */
    if(b == 0) {
        /* xor eax,0 etc. - use immediate variant: more correct to reload b=0 */
        em_u8(e, 0x31); em_u8(e, 0xC9); /* xor ecx,ecx */
        em_u8(e, op_byte); em_u8(e, 0xC1); /* op_byte eax, ecx (modrm 11/eax/ecx) */
    } else if(b == 14 || b == 15) {
        emit_load_logical(e, R_ECX, b);
        em_u8(e, op_byte); em_u8(e, 0xC1); /* op eax, ecx */
    } else {
        /* op eax, [ebp + 4*b]   ->   <op_byte> /r mod=01 r/m=ebp disp8 */
        em_u8(e, op_byte);
        em_u8(e, (uint8_t)(0x40 | (R_EAX << 3) | R_EBP));
        em_u8(e, (uint8_t)(4 * b));
    }
    emit_store_logical(e, dst, R_EAX);
}

/* op_byte_immediate variant: "op eax, imm32"
 *   ADDI: 0x05, SUBI: 0x2D, ANDI: 0x25, ORI: 0x0D
 * (these are the "EAX, imm32" specific opcodes). */
static void emit_bin_imm32(Emitter *e, uint8_t op_byte_eax_imm32,
                           uint8_t dst, uint8_t a, int32_t imm) {
    if(dst == 0) return;
    emit_load_logical(e, R_EAX, a);
    em_u8(e, op_byte_eax_imm32); em_u32(e, (uint32_t)imm);
    emit_store_logical(e, dst, R_EAX);
}

static void emit_op_add (Emitter *e, uint8_t d, uint8_t a, uint8_t b) { emit_bin_rm32(e, 0x03, d, a, b); }
static void emit_op_sub (Emitter *e, uint8_t d, uint8_t a, uint8_t b) { emit_bin_rm32(e, 0x2B, d, a, b); }
static void emit_op_and (Emitter *e, uint8_t d, uint8_t a, uint8_t b) { emit_bin_rm32(e, 0x23, d, a, b); }
static void emit_op_or  (Emitter *e, uint8_t d, uint8_t a, uint8_t b) { emit_bin_rm32(e, 0x0B, d, a, b); }
static void emit_op_xor (Emitter *e, uint8_t d, uint8_t a, uint8_t b) { emit_bin_rm32(e, 0x33, d, a, b); }
static void emit_op_addi(Emitter *e, uint8_t d, uint8_t a, int32_t i) { emit_bin_imm32(e, 0x05, d, a, i); }
static void emit_op_subi(Emitter *e, uint8_t d, uint8_t a, int32_t i) { emit_bin_imm32(e, 0x2D, d, a, i); }
static void emit_op_andi(Emitter *e, uint8_t d, uint8_t a, int32_t i) { emit_bin_imm32(e, 0x25, d, a, i); }
static void emit_op_ori (Emitter *e, uint8_t d, uint8_t a, int32_t i) { emit_bin_imm32(e, 0x0D, d, a, i); }

/* OP_MUL dst, a, b : signed 32-bit, result in eax. Use IMUL r32, r/m32. */
static void emit_op_mul(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    emit_load_logical(e, R_ECX, b);
    /* imul eax, ecx   ->   0F AF /r */
    em_u8(e, 0x0F); em_u8(e, 0xAF);
    em_u8(e, (uint8_t)(0xC0 | (R_EAX << 3) | R_ECX));
    emit_store_logical(e, d, R_EAX);
}

/* OP_NEG dst, a */
static void emit_op_neg(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    /* neg eax   ->   F7 /3 mod=11 reg=011 r/m=eax (0xD8) */
    em_u8(e, 0xF7); em_u8(e, 0xD8);
    emit_store_logical(e, d, R_EAX);
}

/* OP_NOT dst, a : bitwise NOT */
static void emit_op_not(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    em_u8(e, 0xF7); em_u8(e, 0xD0);   /* not eax */
    emit_store_logical(e, d, R_EAX);
}

/* Comparison helper: emits
 *      mov  eax, [a]
 *      <load b into ecx (or xor ecx for r0)>
 *      cmp  eax, ecx
 *      set<cc> al
 *      movzx eax, al
 *      mov  [dst], eax
 * setcc opcode: 0x94 SETE, 0x95 SETNE, 0x9C SETL, 0x9E SETLE,
 *               0x9F SETG, 0x9D SETGE, 0x92 SETB (CLTU), 0x96 SETBE (CLEU). */
static void emit_cmp_setcc(Emitter *e, uint8_t setcc_op,
                           uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    emit_load_logical(e, R_ECX, b);
    /* cmp eax, ecx   ->   3B /r mod=11 reg=eax r/m=ecx */
    em_u8(e, 0x3B); em_u8(e, (uint8_t)(0xC0 | (R_EAX << 3) | R_ECX));
    /* set<cc> al     ->   0F <setcc_op> /0 mod=11 r/m=al */
    em_u8(e, 0x0F); em_u8(e, setcc_op); em_u8(e, 0xC0);
    /* movzx eax, al  ->   0F B6 /r mod=11 reg=eax r/m=al */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, 0xC0);
    emit_store_logical(e, d, R_EAX);
}

/* Branches: target_pc is the IR-word index of the branch target. */
static void emit_op_jmp(Emitter *e, int32_t target_pc) {
    /* jmp rel32   ->   E9 rel32 */
    em_u8(e, 0xE9);
    em_add_reloc(e, (uint32_t)e->len, target_pc, RELOC_REL32_AT_PC);
    em_u32(e, 0);
}

/* JT/JF: branch if reg src_a is non-zero / zero.
 *      mov  eax, [src_a]
 *      test eax, eax
 *      jcc  rel32_to_target   ; cc=NZ for JT, Z for JF */
static void emit_op_jcond(Emitter *e, uint8_t cc_byte,
                          uint8_t src_a, int32_t target_pc) {
    emit_load_logical(e, R_EAX, src_a);
    /* test eax, eax   ->   85 /r modrm 11/eax/eax */
    em_u8(e, 0x85); em_u8(e, 0xC0);
    /* jcc rel32   ->   0F <cc_byte> rel32 */
    em_u8(e, 0x0F); em_u8(e, cc_byte);
    em_add_reloc(e, (uint32_t)e->len, target_pc, RELOC_REL32_AT_PC);
    em_u32(e, 0);
}

/* ---- Peephole: fused compare+branch -----------------------------------
 * When the IR emits CMP immediately followed by JT/JF reading the CMP's dst,
 * we can skip the JT/JF's separate `mov eax, [d]; test eax, eax` because the
 * `cmp` already set EFLAGS appropriately. The intermediate `setcc/movzx/mov`
 * sequence preserves EFLAGS (none of those instructions touch flags), so the
 * final `jcc` reads the original cmp's result.
 *
 * Saves ~5 bytes and one memory round-trip per branch.
 *
 *      mov   eax, [a]
 *      mov   ecx, [b]            (or xor ecx,ecx for r0)
 *      cmp   eax, ecx            ; sets EFLAGS
 *      setcc al                  ; \
 *      movzx eax, al             ;  | flags preserved
 *      mov   [d], eax            ; /  (skipped if d == r0)
 *      jcc   rel32 -> target_pc  ; reads original cmp flags
 *
 * `setcc_op` is the SETcc opcode byte (0x94..0x9F, 0x92, 0x96).
 * `jt_or_jf` is 1 for JT (jump-if-true), 0 for JF (jump-if-false). */
static void emit_cmp_branch_fused(Emitter *e,
                                  uint8_t setcc_op,
                                  uint8_t d, uint8_t a, uint8_t b,
                                  int jt_or_jf, int32_t target_pc) {
    emit_load_logical(e, R_EAX, a);
    emit_load_logical(e, R_ECX, b);
    em_u8(e, 0x3B); em_u8(e, (uint8_t)(0xC0 | (R_EAX << 3) | R_ECX)); /* cmp eax, ecx */
    if(d != 0) {
        em_u8(e, 0x0F); em_u8(e, setcc_op); em_u8(e, 0xC0);   /* setcc al */
        em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, 0xC0);       /* movzx eax, al */
        emit_store_logical(e, d, R_EAX);                      /* mov [d], eax */
    }
    /* Jcc rel32 opcode = setcc_op - 0x10. Invert low bit for JF. */
    uint8_t jcc_cc = (uint8_t)(setcc_op - 0x10);
    if(jt_or_jf == 0) jcc_cc ^= 1;
    em_u8(e, 0x0F); em_u8(e, jcc_cc);
    em_add_reloc(e, (uint32_t)e->len, target_pc, RELOC_REL32_AT_PC);
    em_u32(e, 0);
}

/* OP_JMPR dst : jump to address in dst (IR-word index). */
static void emit_op_jmpr(Emitter *e, uint8_t src) {
    /* mov [ebx + pc], <src>   ; then dispatch. */
    emit_load_logical(e, R_EAX, src);
    em_u8(e, 0x89); em_u8(e, 0x83); em_u32(e, (uint32_t)offsetof(RelicVM, pc));
    emit_dispatch_via_pc(e);
}

/* OP_CALLR / OP_CALLP dst : indirect call. dst holds the fn_id at runtime.
 * If fn_id < nfuncs:
 *      push return_pc; push old fp; fp = sp; pc = fn_offsets[fn_id]; dispatch.
 * Otherwise: fall through (matches the interpreter's `if(fn_id < ir->nfuncs)`
 * gate).
 *
 * Block layout (sizes must stay in sync with the `jae rel8` displacement):
 *      mov   eax, [src]                  ; load fn_id
 *      cmp   eax, nfuncs_imm             ; 3D imm32 (5 bytes for eax form)
 *      jae   skip                        ; rel8 over the call block
 *   --------- start of 62-byte call block -----------
 *      sub   [ebx+sp], 4
 *      mov   ecx, [ebx+sp]
 *      mov   [esi+ecx], imm32(return_pc) ; push return pc
 *      sub   [ebx+sp], 4
 *      mov   ecx, [ebx+sp]
 *      mov   [esi+ecx], edi              ; push old fp
 *      mov   edi, [ebx+sp]               ; fp = sp
 *      mov   ecx, [fn_offsets_addr + 4*eax]   ; ecx = target_pc
 *      mov   [ebx+pc], ecx               ; vm->pc = target_pc
 *      jmp   dword ptr [dispatch + 4*ecx]
 *   --------- end of 62-byte call block --------------
 *   skip:                                ; fall through to next IR word
 */
static void emit_op_callr(Emitter *e, uint8_t src_reg, int32_t return_pc,
                          int32_t nfuncs, uintptr_t fn_offsets_addr) {
    emit_load_logical(e, R_EAX, src_reg);
    /* cmp eax, imm32   ->   3D imm32 */
    em_u8(e, 0x3D); em_u32(e, (uint32_t)nfuncs);
    /* jae rel8 +62 (size of the call block below) */
    em_u8(e, 0x73); em_u8(e, 62);

    /* sub [ebx+sp], 4 */
    em_u8(e, 0x83); em_u8(e, 0xAB); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    em_u8(e, 0x04);
    /* mov ecx, [ebx+sp] */
    em_u8(e, 0x8B); em_u8(e, 0x8B); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    /* mov [esi+ecx], imm32(return_pc) */
    em_u8(e, 0xC7); em_u8(e, 0x04); em_u8(e, 0x0E);
    em_u32(e, (uint32_t)return_pc);

    /* sub [ebx+sp], 4 */
    em_u8(e, 0x83); em_u8(e, 0xAB); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    em_u8(e, 0x04);
    /* mov ecx, [ebx+sp] */
    em_u8(e, 0x8B); em_u8(e, 0x8B); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    /* mov [esi+ecx], edi  (push pinned fp) */
    em_u8(e, 0x89); em_u8(e, 0x3C); em_u8(e, 0x0E);

    /* mov edi, [ebx+sp]   (fp = sp) */
    em_u8(e, 0x8B); em_u8(e, 0xBB); em_u32(e, (uint32_t)offsetof(RelicVM, sp));

    /* mov ecx, dword ptr [fn_offsets_addr + 4*eax]
     *      ->   8B /r SIB scale=2 index=eax base=none disp32
     *           mod=00 reg=ecx r/m=100 sib(ss=10 idx=000 base=101) disp32 */
    em_u8(e, 0x8B); em_u8(e, 0x0C); em_u8(e, 0x85); em_u32(e, (uint32_t)fn_offsets_addr);

    /* mov [ebx+pc], ecx */
    em_u8(e, 0x89); em_u8(e, 0x8B); em_u32(e, (uint32_t)offsetof(RelicVM, pc));

    /* jmp dword ptr [dispatch_table + 4*ecx]
     *      ->   FF /4 SIB scale=2 index=ecx base=none disp32 */
    em_u8(e, 0xFF); em_u8(e, 0x24); em_u8(e, 0x8D);
    em_u32(e, (uint32_t)e->addr_dispatch_table);
    /* skip: fall through */
}

/* OP_CALL fn_id : push (current_pc+1) and old fp onto vm-stack, fp=sp,
 *                 jmp rel32 to fn_offsets[fn_id].
 * fp lives in edi - no memory round-trips for fp manipulation. */
static void emit_op_call(Emitter *e, int32_t target_pc, int32_t return_pc) {
    /* sub dword ptr [ebx+sp], 4 */
    em_u8(e, 0x83); em_u8(e, 0xAB); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    em_u8(e, 0x04);
    /* mov ecx, [ebx+sp] */
    em_u8(e, 0x8B); em_u8(e, 0x8B); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    /* mov dword ptr [esi + ecx], imm32(return_pc) */
    em_u8(e, 0xC7); em_u8(e, 0x04); em_u8(e, 0x0E);
    em_u32(e, (uint32_t)return_pc);

    /* sub dword ptr [ebx+sp], 4 */
    em_u8(e, 0x83); em_u8(e, 0xAB); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    em_u8(e, 0x04);
    /* mov ecx, [ebx+sp] */
    em_u8(e, 0x8B); em_u8(e, 0x8B); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    /* mov [esi + ecx], edi    (push old fp from pinned reg) */
    em_u8(e, 0x89); em_u8(e, 0x3C); em_u8(e, 0x0E);

    /* fp = sp:  mov edi, [ebx+sp] */
    em_u8(e, 0x8B); em_u8(e, 0xBB); em_u32(e, (uint32_t)offsetof(RelicVM, sp));

    /* jmp rel32 to target_pc */
    em_u8(e, 0xE9);
    em_add_reloc(e, (uint32_t)e->len, target_pc, RELOC_REL32_AT_PC);
    em_u32(e, 0);
}

/* OP_RET : sp=fp; pop fp; pop pc; dispatch. fp is in edi. */
static void emit_op_ret(Emitter *e) {
    /* sp = fp:  mov [ebx+sp], edi */
    em_u8(e, 0x89); em_u8(e, 0xBB); em_u32(e, (uint32_t)offsetof(RelicVM, sp));

    /* pop fp: ecx=sp; edi=[esi+ecx]; sp+=4 */
    em_u8(e, 0x8B); em_u8(e, 0x8B); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    em_u8(e, 0x8B); em_u8(e, 0x3C); em_u8(e, 0x0E);  /* mov edi,[esi+ecx] */
    em_u8(e, 0x83); em_u8(e, 0x83); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    em_u8(e, 0x04); /* add [ebx+sp], 4 */

    /* pop pc: ecx=sp; eax=[esi+ecx]; sp+=4; [ebx+pc]=eax */
    em_u8(e, 0x8B); em_u8(e, 0x8B); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    em_u8(e, 0x8B); em_u8(e, 0x04); em_u8(e, 0x0E); /* mov eax,[esi+ecx] */
    em_u8(e, 0x83); em_u8(e, 0x83); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    em_u8(e, 0x04);
    em_u8(e, 0x89); em_u8(e, 0x83); em_u32(e, (uint32_t)offsetof(RelicVM, pc));

    emit_dispatch_via_pc(e);
}

/* OP_ENTER uimm20 : sub dword ptr [ebx+sp], (frame_words * 4) */
static void emit_op_enter(Emitter *e, int32_t frame_words) {
    int32_t bytes = frame_words * 4;
    em_u8(e, 0x81); em_u8(e, 0xAB); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    em_u32(e, (uint32_t)bytes);   /* sub [ebx+sp], imm32 */
}

/* OP_LEAVE : sp = fp. fp is in edi. */
static void emit_op_leave(Emitter *e) {
    /* mov [ebx+sp], edi */
    em_u8(e, 0x89); em_u8(e, 0xBB); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
}

/* Memory ops. The IR carries the address register in the `dst` field for
 * stores and in the `srcA` field for loads (see vm.c). All loads zero-extend
 * (interpreter casts mem_read{8,16} through u32 -> i32). No bounds check -
 * the typed front-end is responsible for that. */
static void emit_op_ld32(Emitter *e, uint8_t dst, uint8_t a, int32_t imm12) {
    if(dst == 0) return;
    emit_load_logical(e, R_ECX, a);
    /* mov eax, [esi + ecx + imm32]   ->   8B /r mod=10 r/m=100 sib */
    em_u8(e, 0x8B); em_u8(e, 0x84); em_u8(e, 0x0E); em_u32(e, (uint32_t)imm12);
    emit_store_logical(e, dst, R_EAX);
}
static void emit_op_ld16(Emitter *e, uint8_t dst, uint8_t a, int32_t imm12) {
    if(dst == 0) return;
    emit_load_logical(e, R_ECX, a);
    /* movzx eax, word ptr [esi+ecx+imm32]   ->   0F B7 /r mod=10 r/m=100 sib */
    em_u8(e, 0x0F); em_u8(e, 0xB7); em_u8(e, 0x84); em_u8(e, 0x0E);
    em_u32(e, (uint32_t)imm12);
    emit_store_logical(e, dst, R_EAX);
}
static void emit_op_ld8(Emitter *e, uint8_t dst, uint8_t a, int32_t imm12) {
    if(dst == 0) return;
    emit_load_logical(e, R_ECX, a);
    /* movzx eax, byte ptr [esi+ecx+imm32]   ->   0F B6 /r mod=10 r/m=100 sib */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, 0x84); em_u8(e, 0x0E);
    em_u32(e, (uint32_t)imm12);
    emit_store_logical(e, dst, R_EAX);
}

static void emit_op_st32(Emitter *e, uint8_t addr_reg, uint8_t val_reg, int32_t imm12) {
    emit_load_logical(e, R_ECX, addr_reg);
    emit_load_logical(e, R_EAX, val_reg);
    /* mov [esi + ecx + imm32], eax */
    em_u8(e, 0x89); em_u8(e, 0x84); em_u8(e, 0x0E); em_u32(e, (uint32_t)imm12);
}
static void emit_op_st16(Emitter *e, uint8_t addr_reg, uint8_t val_reg, int32_t imm12) {
    emit_load_logical(e, R_ECX, addr_reg);
    emit_load_logical(e, R_EAX, val_reg);
    /* mov [esi+ecx+imm32], ax  ->  66 89 /r mod=10 r/m=100 sib (operand-size prefix) */
    em_u8(e, 0x66); em_u8(e, 0x89); em_u8(e, 0x84); em_u8(e, 0x0E);
    em_u32(e, (uint32_t)imm12);
}
static void emit_op_st8(Emitter *e, uint8_t addr_reg, uint8_t val_reg, int32_t imm12) {
    emit_load_logical(e, R_ECX, addr_reg);
    emit_load_logical(e, R_EAX, val_reg);
    /* mov byte ptr [esi+ecx+imm32], al   ->   88 /r mod=10 r/m=100 sib */
    em_u8(e, 0x88); em_u8(e, 0x84); em_u8(e, 0x0E); em_u32(e, (uint32_t)imm12);
}

/* OP_DIV dst, a, b : reg_w(dst, b ? a/b : 0). Signed.
 * Block layout (must keep these sizes in sync with the disp8 below):
 *      85 C9              test ecx, ecx
 *      74 05              jz   +5  -> xor
 *      99                 cdq
 *      F7 F9              idiv ecx        ; eax = a/b
 *      EB 02              jmp  +2  -> store
 *      31 C0              xor  eax, eax
 *      <store [dst], eax>
 */
static void emit_op_div(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    emit_load_logical(e, R_ECX, b);
    em_u8(e, 0x85); em_u8(e, 0xC9);   /* test ecx, ecx */
    em_u8(e, 0x74); em_u8(e, 0x05);   /* jz +5 */
    em_u8(e, 0x99);                   /* cdq */
    em_u8(e, 0xF7); em_u8(e, 0xF9);   /* idiv ecx */
    em_u8(e, 0xEB); em_u8(e, 0x02);   /* jmp +2 */
    em_u8(e, 0x31); em_u8(e, 0xC0);   /* xor eax, eax */
    emit_store_logical(e, d, R_EAX);
}
/* OP_MOD : like DIV but result is in edx; copy to eax before store. */
static void emit_op_mod(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    emit_load_logical(e, R_ECX, b);
    em_u8(e, 0x85); em_u8(e, 0xC9);   /* test ecx, ecx */
    em_u8(e, 0x74); em_u8(e, 0x07);   /* jz +7 -> xor */
    em_u8(e, 0x99);                   /* cdq */
    em_u8(e, 0xF7); em_u8(e, 0xF9);   /* idiv ecx */
    em_u8(e, 0x89); em_u8(e, 0xD0);   /* mov eax, edx */
    em_u8(e, 0xEB); em_u8(e, 0x02);   /* jmp +2 */
    em_u8(e, 0x31); em_u8(e, 0xC0);   /* xor eax, eax */
    emit_store_logical(e, d, R_EAX);
}

/* Variable shifts: count comes from b, masked by &31 implicitly by SHL/SHR/SAR
 * by CL (the x86 instructions already mask the count to 5 bits for 32-bit ops). */
static void emit_op_shift(Emitter *e, uint8_t op_modrm,
                          uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    emit_load_logical(e, R_ECX, b);
    /* D3 /N mod=11 reg=N r/m=000 (eax) - subop selects shl(/4)/shr(/5)/sar(/7) */
    em_u8(e, 0xD3); em_u8(e, op_modrm);
    emit_store_logical(e, d, R_EAX);
}
/* Subop ModR/M values: 0xE0 SHL, 0xE8 SHR, 0xF8 SAR */
static void emit_op_shl(Emitter *e, uint8_t d, uint8_t a, uint8_t b) { emit_op_shift(e, 0xE0, d, a, b); }
static void emit_op_shr(Emitter *e, uint8_t d, uint8_t a, uint8_t b) { emit_op_shift(e, 0xE8, d, a, b); }
static void emit_op_sar(Emitter *e, uint8_t d, uint8_t a, uint8_t b) { emit_op_shift(e, 0xF8, d, a, b); }

/* Type conversions. */
static void emit_op_sx8(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    em_u8(e, 0x0F); em_u8(e, 0xBE); em_u8(e, 0xC0);   /* movsx eax, al */
    emit_store_logical(e, d, R_EAX);
}
static void emit_op_sx16(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    em_u8(e, 0x0F); em_u8(e, 0xBF); em_u8(e, 0xC0);   /* movsx eax, ax */
    emit_store_logical(e, d, R_EAX);
}
static void emit_op_zx8(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, 0xC0);   /* movzx eax, al */
    emit_store_logical(e, d, R_EAX);
}
static void emit_op_zx16(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    em_u8(e, 0x0F); em_u8(e, 0xB7); em_u8(e, 0xC0);   /* movzx eax, ax */
    emit_store_logical(e, d, R_EAX);
}
static void emit_op_trunc8(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    em_u8(e, 0x25); em_u32(e, 0x000000FF);            /* and eax, 0xFF */
    emit_store_logical(e, d, R_EAX);
}
static void emit_op_trunc16(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    em_u8(e, 0x25); em_u32(e, 0x0000FFFF);            /* and eax, 0xFFFF */
    emit_store_logical(e, d, R_EAX);
}

/* OP_PUSH src : stack_push(reg_r(src))
 *      sub dword ptr [ebx+sp], 4
 *      mov ecx, [ebx+sp]
 *      mov eax, [src]
 *      mov [esi+ecx], eax */
static void emit_op_push(Emitter *e, uint8_t src) {
    em_u8(e, 0x83); em_u8(e, 0xAB); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    em_u8(e, 0x04);
    em_u8(e, 0x8B); em_u8(e, 0x8B); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    emit_load_logical(e, R_EAX, src);
    em_u8(e, 0x89); em_u8(e, 0x04); em_u8(e, 0x0E);   /* mov [esi+ecx], eax */
}
/* OP_POP dst : reg_w(dst, stack_pop())
 *      mov ecx, [ebx+sp]
 *      mov eax, [esi+ecx]
 *      add dword ptr [ebx+sp], 4
 *      mov [dst], eax */
static void emit_op_pop(Emitter *e, uint8_t dst) {
    em_u8(e, 0x8B); em_u8(e, 0x8B); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    em_u8(e, 0x8B); em_u8(e, 0x04); em_u8(e, 0x0E);   /* mov eax, [esi+ecx] */
    em_u8(e, 0x83); em_u8(e, 0x83); em_u32(e, (uint32_t)offsetof(RelicVM, sp));
    em_u8(e, 0x04);
    emit_store_logical(e, dst, R_EAX);
}

/* =========================================================================
 *                          FLOATING-POINT HELPERS
 *
 * Float values live in the i32 register file as raw IEEE-754 bit patterns
 * (see vm.c AS_F/AS_I).
 *
 * Two emission strategies, selected at compile time:
 *
 *   Default (modern Windows)
 *     Bit-cast through MOVD and use scalar single-precision SSE ops on
 *     xmm0/xmm1. Compact, register-resident, and well-supported on every
 *     CPU shipped since the Pentium III (1999).
 *
 *   RELIC_WIN95 (legacy `win95-mingw-x86` target)
 *     Use x87 (387/486+ FPU) with the cdecl stack as scratch for FPU
 *     memory operands. SSE was not yet ubiquitous on the Pentium /
 *     Pentium MMX / Pentium Pro / Pentium II / early Athlon hardware
 *     that this build is intended to run on, and emitting SSE there
 *     traps as #UD. The x87 path mirrors `src/jits/x86-dos/jit_x86_dos.c`.
 * ========================================================================= */

/* ModR/M (mod=11) for two registers, hand-encoded since this file does not
 * have a modrm() helper. */
#define FP_MODRM(reg, rm) ((uint8_t)(0xC0 | ((reg) << 3) | (rm)))

#if !defined(RELIC_WIN95)

/* ---- SSE / SSE2 path (modern Windows) ----------------------------------- */

/* movd xmm<dst>, r32<src>   ;  66 0F 6E /r   (xmm low3=dst, r/m=src) */
static void emit_movd_xmm_r32(Emitter *e, uint8_t xmm_dst, uint8_t r32_src) {
    em_u8(e, 0x66);
    em_u8(e, 0x0F); em_u8(e, 0x6E);
    em_u8(e, FP_MODRM(xmm_dst, r32_src));
}

/* movd r32<dst>, xmm<src>   ;  66 0F 7E /r   (xmm in reg field, r/m=dst) */
static void emit_movd_r32_xmm(Emitter *e, uint8_t r32_dst, uint8_t xmm_src) {
    em_u8(e, 0x66);
    em_u8(e, 0x0F); em_u8(e, 0x7E);
    em_u8(e, FP_MODRM(xmm_src, r32_dst));
}

/* SSE scalar-single binary op, fixed on xmm0,xmm1 :  F3 0F <op> C1 */
static void emit_sse_xmm0_xmm1(Emitter *e, uint8_t op) {
    em_u8(e, 0xF3);
    em_u8(e, 0x0F); em_u8(e, op);
    em_u8(e, FP_MODRM(0, 1));
}

/* ucomiss xmm0, xmm1   ;  0F 2E C1  (no F3 prefix) */
static void emit_ucomiss_xmm0_xmm1(Emitter *e) {
    em_u8(e, 0x0F); em_u8(e, 0x2E);
    em_u8(e, FP_MODRM(0, 1));
}

/* cvtsi2ss xmm0, eax   ;  F3 0F 2A C0 */
static void emit_cvtsi2ss_xmm0_eax(Emitter *e) {
    em_u8(e, 0xF3);
    em_u8(e, 0x0F); em_u8(e, 0x2A);
    em_u8(e, FP_MODRM(0, R_EAX));
}

/* cvttss2si eax, xmm0  ;  F3 0F 2C C0  (truncating, matches (i32)f) */
static void emit_cvttss2si_eax_xmm0(Emitter *e) {
    em_u8(e, 0xF3);
    em_u8(e, 0x0F); em_u8(e, 0x2C);
    em_u8(e, FP_MODRM(R_EAX, 0));
}

/* Common preamble: load a->eax, b->ecx, move both into xmm0/xmm1. */
static void emit_fp_load_ab(Emitter *e, uint8_t a, uint8_t b) {
    emit_load_logical(e, R_EAX, a);
    emit_load_logical(e, R_ECX, b);
    emit_movd_xmm_r32(e, 0, R_EAX);
    emit_movd_xmm_r32(e, 1, R_ECX);
}

/* Common tail: extract xmm0->eax, store to logical dst. */
static void emit_fp_store_xmm0(Emitter *e, uint8_t dst) {
    emit_movd_r32_xmm(e, R_EAX, 0);
    emit_store_logical(e, dst, R_EAX);
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
    emit_load_logical(e, R_EAX, a);
    emit_load_logical(e, R_ECX, b);
    emit_movd_xmm_r32(e, 0, R_EAX);               /* movd xmm0, eax  (4) */
    emit_movd_xmm_r32(e, 1, R_ECX);               /* movd xmm1, ecx  (4) */
    emit_sse_xmm0_xmm1(e, 0x5E);                  /* divss xmm0,xmm1 (4) */
    emit_movd_r32_xmm(e, R_EAX, 0);               /* movd eax, xmm0  (4) */
    emit_store_logical(e, d, R_EAX);
}

/* OP_FNEG : flip sign bit in integer domain. */
static void emit_op_fneg(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    /* xor eax, 0x80000000  ;  35 00 00 00 80 */
    em_u8(e, 0x35); em_u32(e, 0x80000000u);
    emit_store_logical(e, d, R_EAX);
}

/* OP_FCEQ : ordered equality (NaN -> false), matches C `==`. */
static void emit_op_fceq(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_fp_load_ab(e, a, b);
    emit_ucomiss_xmm0_xmm1(e);
    em_u8(e, 0x0F); em_u8(e, 0x9B); em_u8(e, FP_MODRM(0, R_EAX));   /* setnp al */
    em_u8(e, 0x0F); em_u8(e, 0x94); em_u8(e, FP_MODRM(0, R_ECX));   /* setz  cl */
    em_u8(e, 0x20); em_u8(e, FP_MODRM(R_ECX, R_EAX));               /* and al, cl */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, FP_MODRM(R_EAX, R_EAX));/* movzx eax,al */
    emit_store_logical(e, d, R_EAX);
}

/* OP_FCLT : a < b (NaN -> false).
 * ucomiss xmm1, xmm0 ; seta al  (CF=0 && ZF=0 => xmm1>xmm0 ordered). */
static void emit_op_fclt(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_fp_load_ab(e, a, b);
    em_u8(e, 0x0F); em_u8(e, 0x2E); em_u8(e, FP_MODRM(1, 0));       /* ucomiss xmm1,xmm0 */
    em_u8(e, 0x0F); em_u8(e, 0x97); em_u8(e, FP_MODRM(0, R_EAX));   /* seta al */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, FP_MODRM(R_EAX, R_EAX));
    emit_store_logical(e, d, R_EAX);
}

/* OP_FCLE : a <= b (NaN -> false).
 * ucomiss xmm1, xmm0 ; setae al  (CF=0 => xmm1>=xmm0 ordered). */
static void emit_op_fcle(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_fp_load_ab(e, a, b);
    em_u8(e, 0x0F); em_u8(e, 0x2E); em_u8(e, FP_MODRM(1, 0));       /* ucomiss xmm1,xmm0 */
    em_u8(e, 0x0F); em_u8(e, 0x93); em_u8(e, FP_MODRM(0, R_EAX));   /* setae al */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, FP_MODRM(R_EAX, R_EAX));
    emit_store_logical(e, d, R_EAX);
}

/* OP_I2F : (float)(i32)a */
static void emit_op_i2f(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    emit_cvtsi2ss_xmm0_eax(e);
    emit_fp_store_xmm0(e, d);
}

/* OP_F2I : (i32)truncate(AS_F(a)) */
static void emit_op_f2i(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    emit_movd_xmm_r32(e, 0, R_EAX);
    emit_cvttss2si_eax_xmm0(e);
    emit_store_logical(e, d, R_EAX);
}

#else /* RELIC_WIN95 : x87 path ------------------------------------------- */

/* Push two 32-bit values from EAX (a) and ECX (b) onto the stack so that
 * [esp+0] = a, [esp+4] = b. */
static void emit_fp_push_ab(Emitter *e) {
    em_u8(e, 0x51);                     /* push ecx */
    em_u8(e, 0x50);                     /* push eax */
}

/* Discard the b-slot left over after `pop eax` consumed the a-slot. */
static void emit_fp_pop_b_slot(Emitter *e) {
    em_u8(e, 0x83); em_u8(e, 0xC4); em_u8(e, 0x04);   /* add esp, 4 */
}

/* x87 binary op on m32 at [esp+4], result popped back into eax.
 * `subop` is the reg field of the D8 /digit form:
 *   0=FADD, 1=FMUL, 4=FSUB, 6=FDIV. */
static void emit_x87_binop_esp4(Emitter *e, uint8_t subop) {
    em_u8(e, 0xD9); em_u8(e, 0x04); em_u8(e, 0x24);             /* fld  dword [esp]   */
    em_u8(e, 0xD8);                                              /* D8 /<subop> */
    em_u8(e, (uint8_t)(0x40 | (subop << 3) | 0x04));             /* mod=01 rm=100 (sib) */
    em_u8(e, 0x24); em_u8(e, 0x04);                              /* sib=esp, disp8=4 */
    em_u8(e, 0xD9); em_u8(e, 0x1C); em_u8(e, 0x24);             /* fstp dword [esp]   */
    em_u8(e, 0x58);                                              /* pop  eax           */
    emit_fp_pop_b_slot(e);
}

static void emit_op_fadd(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    emit_load_logical(e, R_ECX, b);
    emit_fp_push_ab(e);
    emit_x87_binop_esp4(e, 0);          /* FADD m32 = D8 /0 */
    emit_store_logical(e, d, R_EAX);
}
static void emit_op_fsub(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    emit_load_logical(e, R_ECX, b);
    emit_fp_push_ab(e);
    emit_x87_binop_esp4(e, 4);          /* FSUB m32 = D8 /4 */
    emit_store_logical(e, d, R_EAX);
}
static void emit_op_fmul(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    emit_load_logical(e, R_ECX, b);
    emit_fp_push_ab(e);
    emit_x87_binop_esp4(e, 1);          /* FMUL m32 = D8 /1 */
    emit_store_logical(e, d, R_EAX);
}

/* OP_FDIV : raw IEEE 754 division (Inf/NaN propagate). */
static void emit_op_fdiv(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    emit_load_logical(e, R_ECX, b);
    emit_fp_push_ab(e);                           /* push ecx; push eax */
    emit_x87_binop_esp4(e, 6);                    /* fld/fdiv/fstp/pop/add */
    emit_store_logical(e, d, R_EAX);
}

/* OP_FNEG : flip sign bit in integer domain. */
static void emit_op_fneg(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    /* xor eax, 0x80000000  ;  35 00 00 00 80 */
    em_u8(e, 0x35); em_u32(e, 0x80000000u);
    emit_store_logical(e, d, R_EAX);
}

/* Common compare preamble: load a/b to the stack, do FUCOMPP, copy FPU
 * status word into AH and into EFLAGS via SAHF. After this returns:
 *   a > b ordered : CF=0 ZF=0 PF=0
 *   a < b ordered : CF=1 ZF=0 PF=0
 *   a == b        : CF=0 ZF=1 PF=0
 *   unordered     : CF=1 ZF=1 PF=1
 * Discards scratch via LEA so EFLAGS survive for the following SETcc. */
static void emit_fp_compare_ab(Emitter *e, uint8_t a, uint8_t b) {
    emit_load_logical(e, R_EAX, a);
    emit_load_logical(e, R_ECX, b);
    emit_fp_push_ab(e);                                              /* [esp]=a, [esp+4]=b */
    em_u8(e, 0xD9); em_u8(e, 0x44); em_u8(e, 0x24); em_u8(e, 0x04);  /* fld dword [esp+4] -> ST(0)=b */
    em_u8(e, 0xD9); em_u8(e, 0x04); em_u8(e, 0x24);                  /* fld dword [esp]   -> ST(0)=a, ST(1)=b */
    em_u8(e, 0xDA); em_u8(e, 0xE9);                                  /* fucompp */
    em_u8(e, 0xDF); em_u8(e, 0xE0);                                  /* fnstsw ax */
    em_u8(e, 0x9E);                                                  /* sahf */
    /* lea esp, [esp+8] : discard a/b WITHOUT touching EFLAGS. */
    em_u8(e, 0x8D); em_u8(e, 0x64); em_u8(e, 0x24); em_u8(e, 0x08);
}

/* OP_FCEQ : ordered equality, ZF=1 AND PF=0. */
static void emit_op_fceq(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_fp_compare_ab(e, a, b);
    em_u8(e, 0x0F); em_u8(e, 0x9B); em_u8(e, FP_MODRM(0, R_EAX));    /* setnp al */
    em_u8(e, 0x0F); em_u8(e, 0x94); em_u8(e, FP_MODRM(0, R_ECX));    /* setz  cl */
    em_u8(e, 0x20); em_u8(e, FP_MODRM(R_ECX, R_EAX));                /* and al, cl */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, FP_MODRM(R_EAX, R_EAX));/* movzx eax, al */
    emit_store_logical(e, d, R_EAX);
}

/* OP_FCLT : ordered a<b, CF=1 AND PF=0. */
static void emit_op_fclt(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_fp_compare_ab(e, a, b);
    em_u8(e, 0x0F); em_u8(e, 0x92); em_u8(e, FP_MODRM(0, R_EAX));    /* setb  al */
    em_u8(e, 0x0F); em_u8(e, 0x9B); em_u8(e, FP_MODRM(0, R_ECX));    /* setnp cl */
    em_u8(e, 0x20); em_u8(e, FP_MODRM(R_ECX, R_EAX));                /* and al, cl */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, FP_MODRM(R_EAX, R_EAX));
    emit_store_logical(e, d, R_EAX);
}

/* OP_FCLE : ordered a<=b, (CF=1 OR ZF=1) AND PF=0. */
static void emit_op_fcle(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_fp_compare_ab(e, a, b);
    em_u8(e, 0x0F); em_u8(e, 0x96); em_u8(e, FP_MODRM(0, R_EAX));    /* setbe al */
    em_u8(e, 0x0F); em_u8(e, 0x9B); em_u8(e, FP_MODRM(0, R_ECX));    /* setnp cl */
    em_u8(e, 0x20); em_u8(e, FP_MODRM(R_ECX, R_EAX));                /* and al, cl */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, FP_MODRM(R_EAX, R_EAX));
    emit_store_logical(e, d, R_EAX);
}

/* OP_I2F : (float)(i32)a
 *   push eax ; fild [esp] ; fstp [esp] ; pop eax */
static void emit_op_i2f(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    em_u8(e, 0x50);                                       /* push eax */
    em_u8(e, 0xDB); em_u8(e, 0x04); em_u8(e, 0x24);       /* fild dword [esp] */
    em_u8(e, 0xD9); em_u8(e, 0x1C); em_u8(e, 0x24);       /* fstp dword [esp] */
    em_u8(e, 0x58);                                       /* pop eax */
    emit_store_logical(e, d, R_EAX);
}

/* OP_F2I : (i32)truncate(AS_F(a)).
 * x87 default rounding is round-to-nearest, so save CW, force RC=11
 * (truncate toward zero), do FISTP, restore. Matches C `(int)f`. */
static void emit_op_f2i(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_load_logical(e, R_EAX, a);
    em_u8(e, 0x50);                                                       /* push eax */
    em_u8(e, 0x83); em_u8(e, 0xEC); em_u8(e, 0x04);                       /* sub esp, 4 */
    em_u8(e, 0xD9); em_u8(e, 0x3C); em_u8(e, 0x24);                       /* fnstcw [esp] */
    em_u8(e, 0x66); em_u8(e, 0x8B); em_u8(e, 0x04); em_u8(e, 0x24);       /* mov ax,[esp] */
    em_u8(e, 0x66); em_u8(e, 0x0D); em_u8(e, 0x00); em_u8(e, 0x0C);       /* or  ax, 0x0C00 */
    em_u8(e, 0x66); em_u8(e, 0x89); em_u8(e, 0x44); em_u8(e, 0x24); em_u8(e, 0x02); /* mov [esp+2],ax */
    em_u8(e, 0xD9); em_u8(e, 0x44); em_u8(e, 0x24); em_u8(e, 0x04);       /* fld  dword [esp+4] */
    em_u8(e, 0xD9); em_u8(e, 0x6C); em_u8(e, 0x24); em_u8(e, 0x02);       /* fldcw word [esp+2] */
    em_u8(e, 0xDB); em_u8(e, 0x5C); em_u8(e, 0x24); em_u8(e, 0x04);       /* fistp dword [esp+4] */
    em_u8(e, 0xD9); em_u8(e, 0x2C); em_u8(e, 0x24);                       /* fldcw word [esp] */
    em_u8(e, 0x83); em_u8(e, 0xC4); em_u8(e, 0x04);                       /* add esp, 4 */
    em_u8(e, 0x58);                                                       /* pop eax */
    emit_store_logical(e, d, R_EAX);
}

#endif /* RELIC_WIN95 */

/* =========================================================================
 *                          f64 (d-bank) OPERATIONS
 * ----------------------------------------------------------------------------
 * Same model as the x64 JITs and the AOT backends: vm->d[] is authoritative,
 * ops are load-op-store, and d operands never touch the logical-register
 * helpers (they index vm->d[], not vm->r[]). ebp points at vm->r[0], so a
 * d slot is [ebp + (offsetof(d) - offsetof(r)) + 8*n] - always disp32.
 * d0 is a real register: no early-out on dst==0 for d-bank writes.
 * RELIC_SOFTFP builds emit no FP hardware ops at all - D* ops stay on the
 * step1 fallback there.
 * ========================================================================= */
#if !defined(RELIC_SOFTFP)

#define D_BASE_OFF \
    ((int32_t)offsetof(RelicVM, d) - (int32_t)offsetof(RelicVM, r))
static int32_t d_disp(uint8_t n) { return D_BASE_OFF + 8 * (int32_t)n; }

/* [ebp + disp32] modrm for reg field r: mod=10 r/m=101 = 0x85|(r<<3). */
#define D_MODRM(r) ((uint8_t)(0x85 | ((r) << 3)))

/* mov eax, [ebp + disp] / mov [ebp + disp], eax — d[] halves. */
static void emit_mov_eax_dreg(Emitter *e, uint8_t log, int hi) {
    em_u8(e, 0x8B); em_u8(e, D_MODRM(R_EAX));
    em_u32(e, (uint32_t)(d_disp(log) + (hi ? 4 : 0)));
}
static void emit_mov_dreg_eax(Emitter *e, uint8_t log, int hi) {
    em_u8(e, 0x89); em_u8(e, D_MODRM(R_EAX));
    em_u32(e, (uint32_t)(d_disp(log) + (hi ? 4 : 0)));
}
/* mov dword [ebp + disp], imm32   ;  C7 /0 */
static void emit_mov_dreg_imm32(Emitter *e, uint8_t log, int hi, uint32_t imm) {
    em_u8(e, 0xC7); em_u8(e, D_MODRM(0));
    em_u32(e, (uint32_t)(d_disp(log) + (hi ? 4 : 0)));
    em_u32(e, imm);
}
/* RMW dword [ebp + disp], imm32   ;  81 /subop  (4=AND, 6=XOR) */
static void emit_rmw_dreg_hi_imm32(Emitter *e, uint8_t subop, uint8_t log,
                                   uint32_t imm) {
    em_u8(e, 0x81); em_u8(e, D_MODRM(subop));
    em_u32(e, (uint32_t)(d_disp(log) + 4));
    em_u32(e, imm);
}
/* mov [ebp + disp], ecx — DFROMBITS's lo-word store. */
static void emit_mov_dreg_ecx(Emitter *e, uint8_t log, int hi) {
    em_u8(e, 0x89); em_u8(e, D_MODRM(R_ECX));
    em_u32(e, (uint32_t)(d_disp(log) + (hi ? 4 : 0)));
}

/* x87 d-bank helpers are flavour-neutral: fld/fstp qword are legal on
 * every CPU this JIT runs on (SSE2 parts still have an x87), and the
 * SSE2 flavour needs them for U2D/D2U and for the st0-returning libm
 * calls. */
static void emit_fld_dreg(Emitter *e, uint8_t log) {
    em_u8(e, 0xDD); em_u8(e, D_MODRM(0)); em_u32(e, (uint32_t)d_disp(log));
}
static void emit_fstp_dreg(Emitter *e, uint8_t log) {
    em_u8(e, 0xDD); em_u8(e, D_MODRM(3)); em_u32(e, (uint32_t)d_disp(log));
}

#if defined(RELIC_WIN95)
/* ---- x87 flavour ------------------------------------------------------
 * f<op> qword [d[log]] is DC /digit (0 add 1 mul 4 fsub 6 fdiv, 3 fcomp). */
static void emit_f87d_dreg(Emitter *e, uint8_t subop, uint8_t log) {
    em_u8(e, 0xDC); em_u8(e, D_MODRM(subop)); em_u32(e, (uint32_t)d_disp(log));
}
/* PC=10b (53-bit significand), RC=00 — same contract as aot_x86.c's
 * emit_fcw_dbl: f64 math must bit-match the SSE2 backends, so the x87
 * can't keep 80-bit intermediates. Runs at JIT entry and after every C
 * call boundary (step1, libm), never per-op. */
static void emit_fcw_dbl(Emitter *e) {
    em_u8(e, 0x83); em_u8(e, 0xEC); em_u8(e, 4);                 /* sub esp,4 */
    em_u8(e, 0xD9); em_u8(e, 0x3C); em_u8(e, 0x24);              /* fnstcw [esp] */
    em_u8(e, 0x80); em_u8(e, 0x64); em_u8(e, 0x24);
    em_u8(e, 1);    em_u8(e, 0xF0);      /* and byte [esp+1], 0xF0 */
    em_u8(e, 0x80); em_u8(e, 0x4C); em_u8(e, 0x24);
    em_u8(e, 1);    em_u8(e, 0x02);      /* or  byte [esp+1], 0x02 */
    em_u8(e, 0xD9); em_u8(e, 0x2C); em_u8(e, 0x24);              /* fldcw [esp] */
    em_u8(e, 0x83); em_u8(e, 0xC4); em_u8(e, 4);                 /* add esp,4 */
}
#else
/* ---- SSE2 flavour ----------------------------------------------------- */
/* movsd xmm, [ebp + disp32] / movsd [ebp + disp32], xmm   F2 0F 10/11 */
static void emit_movsd_xmm_dreg(Emitter *e, uint8_t xmm, uint8_t log) {
    em_u8(e, 0xF2); em_u8(e, 0x0F); em_u8(e, 0x10);
    em_u8(e, D_MODRM(xmm)); em_u32(e, (uint32_t)d_disp(log));
}
static void emit_movsd_dreg_xmm(Emitter *e, uint8_t log, uint8_t xmm) {
    em_u8(e, 0xF2); em_u8(e, 0x0F); em_u8(e, 0x11);
    em_u8(e, D_MODRM(xmm)); em_u32(e, (uint32_t)d_disp(log));
}
/* addsd/subsd/mulsd/divsd xmm0, xmm1   F2 0F <op> C1 */
static void emit_sse_sd_xmm0_xmm1(Emitter *e, uint8_t op) {
    em_u8(e, 0xF2); em_u8(e, 0x0F); em_u8(e, op);
    em_u8(e, FP_MODRM(0, 1));
}
static void emit_ucomisd_xmm0_xmm1(Emitter *e) {          /* 66 0F 2E C1 */
    em_u8(e, 0x66); em_u8(e, 0x0F); em_u8(e, 0x2E);
    em_u8(e, FP_MODRM(0, 1));
}
static void emit_cvtsi2sd_xmm0_eax(Emitter *e) {          /* F2 0F 2A C0 */
    em_u8(e, 0xF2); em_u8(e, 0x0F); em_u8(e, 0x2A);
    em_u8(e, FP_MODRM(0, R_EAX));
}
static void emit_cvttsd2si_eax_xmm0(Emitter *e) {         /* F2 0F 2C C0 */
    em_u8(e, 0xF2); em_u8(e, 0x0F); em_u8(e, 0x2C);
    em_u8(e, FP_MODRM(R_EAX, 0));
}
static void emit_cvtsd2ss_xmm0_xmm0(Emitter *e) {         /* F2 0F 5A C0 */
    em_u8(e, 0xF2); em_u8(e, 0x0F); em_u8(e, 0x5A);
    em_u8(e, FP_MODRM(0, 0));
}
static void emit_cvtss2sd_xmm0_xmm0(Emitter *e) {         /* F3 0F 5A C0 */
    em_u8(e, 0xF3); em_u8(e, 0x0F); em_u8(e, 0x5A);
    em_u8(e, FP_MODRM(0, 0));
}
/* Load the f32 bits of logical r[log] into xmm0 (F2D source). Handles
 * r0 (+0.0f) and the r14/r15 aliases via the load_logical rules. */
static void emit_f32bits_xmm0(Emitter *e, uint8_t log) {
    emit_load_logical(e, R_EAX, log);
    emit_movd_xmm_r32(e, 0, R_EAX);
}
static void emit_d_load_ab(Emitter *e, uint8_t a, uint8_t b) {
    emit_movsd_xmm_dreg(e, 0, a);
    emit_movsd_xmm_dreg(e, 1, b);
}
#endif /* RELIC_WIN95 */

/* OP_DLDC : d[dst] = bits(lo,hi) — two payload words follow in the IR. */
static void emit_op_dldc(Emitter *e, uint8_t d, uint32_t lo, uint32_t hi) {
    emit_mov_dreg_imm32(e, d, 0, lo);
    emit_mov_dreg_imm32(e, d, 1, hi);
}
/* OP_DMOV : d[dst] = d[a] — two dword copies through eax. */
static void emit_op_dmov(Emitter *e, uint8_t d, uint8_t a) {
    if(d == a) return;
    emit_mov_eax_dreg(e, a, 0); emit_mov_dreg_eax(e, d, 0);
    emit_mov_eax_dreg(e, a, 1); emit_mov_dreg_eax(e, d, 1);
}
/* OP_DNEG : copy, then XOR the high-dword sign bit. */
static void emit_op_dneg(Emitter *e, uint8_t d, uint8_t a) {
    emit_op_dmov(e, d, a);
    emit_rmw_dreg_hi_imm32(e, 6, d, 0x80000000u);
}
/* OP_DFABS : copy, then clear the sign bit. */
static void emit_op_dfabs(Emitter *e, uint8_t d, uint8_t a) {
    emit_op_dmov(e, d, a);
    emit_rmw_dreg_hi_imm32(e, 4, d, 0x7FFFFFFFu);
}
/* OP_DADD/DSUB/DMUL/DDIV. `kind` is 0=add 1=sub 2=mul 3=div, mapped to
 * the per-flavour op encodings. */
#if defined(RELIC_WIN95)
static const uint8_t D_X87_OP[4] = { 0, 4, 1, 6 };   /* DC /digit */
#else
static const uint8_t D_SSE_OP[4] = { 0x58, 0x5C, 0x59, 0x5E };
#endif
static void emit_op_darith(Emitter *e, int kind,
                           uint8_t d, uint8_t a, uint8_t b) {
#if defined(RELIC_WIN95)
    emit_fld_dreg(e, a);
    emit_f87d_dreg(e, D_X87_OP[kind], b);           /* DC /digit qword */
    emit_fstp_dreg(e, d);
#else
    emit_d_load_ab(e, a, b);
    emit_sse_sd_xmm0_xmm1(e, D_SSE_OP[kind]);
    emit_movsd_dreg_xmm(e, d, 0);
#endif
}
static void emit_op_dadd(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    emit_op_darith(e, 0, d, a, b);   /* addsd / fadd */
}
static void emit_op_dsub(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    emit_op_darith(e, 1, d, a, b);   /* subsd / fsub */
}
static void emit_op_dmul(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    emit_op_darith(e, 2, d, a, b);   /* mulsd / fmul */
}
static void emit_op_ddiv(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    emit_op_darith(e, 3, d, a, b);   /* divsd / fdiv */
}
/* OP_DSQRT. */
static void emit_op_dsqrt(Emitter *e, uint8_t d, uint8_t a) {
#if defined(RELIC_WIN95)
    emit_fld_dreg(e, a);
    em_u8(e, 0xD9); em_u8(e, 0xFA);                 /* fsqrt */
    emit_fstp_dreg(e, d);
#else
    emit_movsd_xmm_dreg(e, 0, a);
    em_u8(e, 0xF2); em_u8(e, 0x0F); em_u8(e, 0x51);
    em_u8(e, FP_MODRM(0, 0));                       /* sqrtsd xmm0,xmm0 */
    emit_movsd_dreg_xmm(e, d, 0);
#endif
}
/* Compare flags for x ? y, both flavours leave EFLAGS in the shared
 * unordered-aware pattern (x87 fcomp + fnstsw/sahf mirrors ucomisd). */
static void emit_dcmp_flags(Emitter *e, uint8_t x, uint8_t y) {
#if defined(RELIC_WIN95)
    emit_fld_dreg(e, x);
    emit_f87d_dreg(e, 3, y);                        /* fcomp qword [y] */
    em_u8(e, 0xDF); em_u8(e, 0xE0);                 /* fnstsw ax */
    em_u8(e, 0x9E);                                 /* sahf */
#else
    emit_movsd_xmm_dreg(e, 0, x);
    emit_movsd_xmm_dreg(e, 1, y);
    emit_ucomisd_xmm0_xmm1(e);
#endif
}
/* OP_DCEQ : a==b ordered (NaN -> false): setnp & setz. */
static void emit_op_dceq(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_dcmp_flags(e, a, b);
    em_u8(e, 0x0F); em_u8(e, 0x9B); em_u8(e, FP_MODRM(0, R_EAX));   /* setnp al */
    em_u8(e, 0x0F); em_u8(e, 0x94); em_u8(e, FP_MODRM(0, R_ECX));   /* setz  cl */
    em_u8(e, 0x20); em_u8(e, FP_MODRM(R_ECX, R_EAX));               /* and al, cl */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, FP_MODRM(R_EAX, R_EAX));/* movzx eax,al */
    emit_store_logical(e, d, R_EAX);
}
/* OP_DCLT : a<b ordered — compare b?a, seta (CF=0 && ZF=0). */
static void emit_op_dclt(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_dcmp_flags(e, b, a);
    em_u8(e, 0x0F); em_u8(e, 0x97); em_u8(e, FP_MODRM(0, R_EAX));   /* seta al */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, FP_MODRM(R_EAX, R_EAX));
    emit_store_logical(e, d, R_EAX);
}
/* OP_DCLE : a<=b ordered — compare b?a, setae (CF=0). */
static void emit_op_dcle(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    if(d == 0) return;
    emit_dcmp_flags(e, b, a);
    em_u8(e, 0x0F); em_u8(e, 0x93); em_u8(e, FP_MODRM(0, R_EAX));   /* setae al */
    em_u8(e, 0x0F); em_u8(e, 0xB6); em_u8(e, FP_MODRM(R_EAX, R_EAX));
    emit_store_logical(e, d, R_EAX);
}
/* OP_DLD : d[dst] = f64 at mem[r[a] + imm12] — two dword copies. */
static void emit_op_dld(Emitter *e, uint8_t d, uint8_t a, int32_t imm12) {
    emit_load_logical(e, R_ECX, a);
    em_u8(e, 0x8B); em_u8(e, 0x84); em_u8(e, 0x0E); em_u32(e, (uint32_t)imm12);
    emit_mov_dreg_eax(e, d, 0);                     /* lo */
    em_u8(e, 0x8B); em_u8(e, 0x84); em_u8(e, 0x0E);
    em_u32(e, (uint32_t)(imm12 + 4));
    emit_mov_dreg_eax(e, d, 1);                     /* hi */
}
/* OP_DST : f64 at mem[r[dst] + imm12] = d[a]  (dst is the ADDRESS reg). */
static void emit_op_dst(Emitter *e, uint8_t addr_reg, uint8_t a, int32_t imm12) {
    emit_load_logical(e, R_ECX, addr_reg);
    emit_mov_eax_dreg(e, a, 0);
    em_u8(e, 0x89); em_u8(e, 0x84); em_u8(e, 0x0E); em_u32(e, (uint32_t)imm12);
    emit_mov_eax_dreg(e, a, 1);
    em_u8(e, 0x89); em_u8(e, 0x84); em_u8(e, 0x0E);
    em_u32(e, (uint32_t)(imm12 + 4));
}
/* OP_DBITS_LO / OP_DBITS_HI : r[dst] = dword of d[a]. */
static void emit_op_dbits(Emitter *e, uint8_t d, uint8_t a, int hi) {
    if(d == 0) return;
    emit_mov_eax_dreg(e, a, hi);
    emit_store_logical(e, d, R_EAX);
}
/* OP_DFROMBITS : d[dst] = bits(hi = r[a], lo = r[b]). */
static void emit_op_dfrombits(Emitter *e, uint8_t d, uint8_t a, uint8_t b) {
    emit_load_logical(e, R_EAX, a);              /* hi */
    emit_load_logical(e, R_ECX, b);              /* lo */
    emit_mov_dreg_eax(e, d, 1);
    emit_mov_dreg_ecx(e, d, 0);
}
/* OP_I2D : d[dst] = (double)(i32)r[a] */
static void emit_op_i2d(Emitter *e, uint8_t d, uint8_t a) {
    emit_load_logical(e, R_EAX, a);
#if defined(RELIC_WIN95)
    em_u8(e, 0x50);                                       /* push eax */
    em_u8(e, 0xDB); em_u8(e, 0x04); em_u8(e, 0x24);       /* fild dword [esp] */
    emit_fstp_dreg(e, d);
    em_u8(e, 0x83); em_u8(e, 0xC4); em_u8(e, 0x04);       /* add esp,4 */
#else
    emit_cvtsi2sd_xmm0_eax(e);
    emit_movsd_dreg_xmm(e, d, 0);
#endif
}
/* OP_U2D : d[dst] = (double)(u32)r[a] — fild m64 of the zero-extended
 * value; exact, and works on every flavour (x87 is always present on
 * targets that run this JIT; i386 SSE2 can't express u32->f64 anyway). */
static void emit_op_u2d(Emitter *e, uint8_t d, uint8_t a) {
    emit_load_logical(e, R_EAX, a);
    em_u8(e, 0x83); em_u8(e, 0xEC); em_u8(e, 8);          /* sub esp,8 */
    em_u8(e, 0x89); em_u8(e, 0x04); em_u8(e, 0x24);       /* mov [esp],eax lo */
    em_u8(e, 0xC7); em_u8(e, 0x44); em_u8(e, 0x24);
    em_u8(e, 4);    em_u32(e, 0);                         /* mov [esp+4],0 hi */
    em_u8(e, 0xDF); em_u8(e, 0x2C); em_u8(e, 0x24);       /* fild qword [esp] */
    emit_fstp_dreg(e, d);
    em_u8(e, 0x83); em_u8(e, 0xC4); em_u8(e, 8);          /* add esp,8 */
}
/* Shared truncating f64->int via x87: load under RC=11 (truncate), fistp
 * to scratch, restore saved FCW. m64=1 -> fistp qword (D2U: the i64 low
 * dword is exactly (u32)d), m64=0 -> fistp dword (x87 D2I). Leaves the
 * result in eax. */
static void emit_d2i_common(Emitter *e, uint8_t a, int m64) {
    em_u8(e, 0x83); em_u8(e, 0xEC); em_u8(e, 16);         /* sub esp,16 */
    em_u8(e, 0xD9); em_u8(e, 0x3C); em_u8(e, 0x24);       /* fnstcw [esp] */
    em_u8(e, 0x0F); em_u8(e, 0xB7); em_u8(e, 0x04); em_u8(e, 0x24);
                                                          /* movzx eax,w[esp] */
    em_u8(e, 0x0D); em_u32(e, 0x0C00);                    /* or eax,0x0C00 RC=11 */
    em_u8(e, 0x66); em_u8(e, 0x89); em_u8(e, 0x44); em_u8(e, 0x24);
    em_u8(e, 4);                                          /* mov [esp+4],ax */
    em_u8(e, 0xD9); em_u8(e, 0x6C); em_u8(e, 0x24);
    em_u8(e, 4);                                          /* fldcw [esp+4] */
    emit_fld_dreg(e, a);
    if(m64) {
        em_u8(e, 0xDF); em_u8(e, 0x7C); em_u8(e, 0x24);
        em_u8(e, 8);                                      /* fistp qword [esp+8] */
    } else {
        em_u8(e, 0xDB); em_u8(e, 0x5C); em_u8(e, 0x24);
        em_u8(e, 8);                                      /* fistp dword [esp+8] */
    }
    em_u8(e, 0xD9); em_u8(e, 0x2C); em_u8(e, 0x24);       /* fldcw [esp] */
    em_u8(e, 0x8B); em_u8(e, 0x44); em_u8(e, 0x24);
    em_u8(e, 8);                                          /* mov eax,[esp+8] */
    em_u8(e, 0x83); em_u8(e, 0xC4); em_u8(e, 16);         /* add esp,16 */
}
/* OP_D2I : r[dst] = (i32)truncate(d[a]) */
static void emit_op_d2i(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
#if defined(RELIC_WIN95)
    emit_d2i_common(e, a, 0);
#else
    emit_movsd_xmm_dreg(e, 0, a);
    emit_cvttsd2si_eax_xmm0(e);
#endif
    emit_store_logical(e, d, R_EAX);
}
/* OP_D2U : r[dst] = (i32)(u32)d[a] — via fistp m64 on all flavours. */
static void emit_op_d2u(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
    emit_d2i_common(e, a, 1);
    emit_store_logical(e, d, R_EAX);
}
/* OP_F2D : d[dst] = (double)asfloat(r[a]) — always exact. */
static void emit_op_f2d(Emitter *e, uint8_t d, uint8_t a) {
#if defined(RELIC_WIN95)
    emit_load_logical(e, R_EAX, a);
    em_u8(e, 0x50);                                       /* push eax */
    em_u8(e, 0xD9); em_u8(e, 0x04); em_u8(e, 0x24);       /* fld dword [esp] */
    emit_fstp_dreg(e, d);
    em_u8(e, 0x83); em_u8(e, 0xC4); em_u8(e, 0x04);       /* add esp,4 */
#else
    emit_f32bits_xmm0(e, a);
    emit_cvtss2sd_xmm0_xmm0(e);
    emit_movsd_dreg_xmm(e, d, 0);
#endif
}
/* OP_D2F : r[dst] = asbits((float)d[a]) — round-nearest-even. */
static void emit_op_d2f(Emitter *e, uint8_t d, uint8_t a) {
    if(d == 0) return;
#if defined(RELIC_WIN95)
    emit_fld_dreg(e, a);
    em_u8(e, 0x83); em_u8(e, 0xEC); em_u8(e, 4);          /* sub esp,4 */
    em_u8(e, 0xD9); em_u8(e, 0x1C); em_u8(e, 0x24);       /* fstp dword [esp] */
    em_u8(e, 0x58);                                       /* pop eax */
#else
    emit_movsd_xmm_dreg(e, 0, a);
    emit_cvtsd2ss_xmm0_xmm0(e);
    emit_movd_r32_xmm(e, R_EAX, 0);
#endif
    emit_store_logical(e, d, R_EAX);
}

/* ---- f64 libm calls (cdecl, i386 ABI: double args on the stack,
 * double result in st0 on every flavour) ----
 * Lazily-initialised pointer tables — the same functions the interpreter
 * calls, so results are bit-identical to --no-jit by construction. */
typedef double (*jit_dp_unary_fn)(double);
typedef double (*jit_dp_binary_fn)(double, double);

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
#define D1_SINH  13
#define D1_COSH  14
#define D1_TANH  15
#define D2_POW   0
#define D2_ATAN2 1
#define D2_FMOD  2

/* Push d[log]'s 8 bytes as a cdecl arg at [esp+ofs] inside an already-
 * reserved arg block. */
static void emit_darg(Emitter *e, uint8_t log, uint8_t ofs) {
    emit_mov_eax_dreg(e, log, 0);
    em_u8(e, 0x89); em_u8(e, 0x44); em_u8(e, 0x24);
    em_u8(e, ofs);                                  /* mov [esp+ofs], eax */
    emit_mov_eax_dreg(e, log, 1);
    em_u8(e, 0x89); em_u8(e, 0x44); em_u8(e, 0x24);
    em_u8(e, (uint8_t)(ofs + 4));                   /* mov [esp+ofs+4], eax */
}
/* Unary: sub esp,16 ; d[a] at [esp] ; call ; fstp d[d] ; add esp,16. */
static void emit_op_dmath1(Emitter *e, int idx, uint8_t d, uint8_t a) {
    jit_d_resolve();
    em_u8(e, 0x83); em_u8(e, 0xEC); em_u8(e, 16);         /* sub esp,16 */
    emit_darg(e, a, 0);
    em_u8(e, 0xB8); em_u32(e, (uint32_t)(uintptr_t)g_d1_fn[idx]);
    em_u8(e, 0xFF); em_u8(e, 0xD0);                       /* call eax */
    em_u8(e, 0x83); em_u8(e, 0xC4); em_u8(e, 16);         /* add esp,16 */
    emit_fstp_dreg(e, d);
#if defined(RELIC_WIN95)
    emit_fcw_dbl(e);        /* callee may have dirtied the FCW */
#endif
}
/* Binary: args at [esp] (a) and [esp+8] (b). */
static void emit_op_dmath2(Emitter *e, int idx,
                           uint8_t d, uint8_t a, uint8_t b) {
    jit_d_resolve();
    em_u8(e, 0x83); em_u8(e, 0xEC); em_u8(e, 16);         /* sub esp,16 */
    emit_darg(e, a, 0);
    emit_darg(e, b, 8);
    em_u8(e, 0xB8); em_u32(e, (uint32_t)(uintptr_t)g_d2_fn[idx]);
    em_u8(e, 0xFF); em_u8(e, 0xD0);                       /* call eax */
    em_u8(e, 0x83); em_u8(e, 0xC4); em_u8(e, 16);         /* add esp,16 */
    emit_fstp_dreg(e, d);
#if defined(RELIC_WIN95)
    emit_fcw_dbl(e);
#endif
}
#endif /* !RELIC_SOFTFP */

/* =========================================================================
 *                          MAIN COMPILE PASS
 * ========================================================================= */

/* Decide whether a given opcode is inlined natively. Anything else falls
 * back to relic_vm_step1. Keeping this in one place documents the JIT's
 * current coverage. */
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
#if !defined(RELIC_SOFTFP)
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
#endif
        case OP_JMP:  case OP_JT:     case OP_JF:   case OP_JMPR:
        case OP_CALL: case OP_CALLR:  case OP_CALLP: case OP_RET:
        case OP_ENTER:case OP_LEAVE:
        case OP_PUSH: case OP_POP:
            return 1;
        default:
            return 0;
    }
}

/* For CMP+JT/JF fusion: map a CMP opcode to its SETcc opcode byte.
 * Returns 0 for non-CMP ops (acts as "is this a CMP?" predicate too). */
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

/* Absolute target of the JMP/JT/JF at `pc` (PC-relative encoding,
 * see IR_JTARGET in tigryx.h). */
static int32_t decode_jmp_target(uint32_t instr, int32_t pc) {
    return (int32_t)IR_JTARGET(instr, pc);
}

static bool x86_win_compile(IRBuffer *ir, RelicJitCode **out_code,
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

    /* Allocate the dispatch table on the heap. We reference its address as
     * a baked imm32 in the emitted code, so it must be allocated BEFORE we
     * start emitting. ir->len + 1 entries: the last one is the
     * "main returned past end" sentinel that points at the epilogue. */
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

    /* ---------------- Pre-pass: branch-target bitmap --------------------
     * We only fuse CMP+JT/JF when the JT/JF is NOT itself a branch target
     * from somewhere else in the IR. Marks targets of JMP/JT/JF and every
     * function entry. */
    uint8_t *is_branch_target = (uint8_t*)calloc((size_t)ir->len + 1, 1);
    if(!is_branch_target) {
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
            p++; /* skip 32-bit literal word */
        } else if(o == OP_DLDC) {
            /* DLDC carries TWO payload words: skip both or a literal
             * half could decode as OP_JMP and mark a bogus target. */
            p += 2;
        }
    }
    for(int i = 0; i < ir->nfuncs; i++) {
        int32_t off = IRFNOFF(ir, i);
        if(off >= 0 && off < ir->len) is_branch_target[off] = 1;
    }

    /* ---------------- Prologue ----------------
     * After it, immediately dispatch on vm->pc. The interpreter's caller
     * (main.c) sets up vm->pc to the entry function before calling us. */
    emit_prologue(&e);
    emit_dispatch_via_pc(&e);

    /* ---------------- Per-IR-word emission ---------------- */
    int32_t pc = 0;
    while(pc < ir->len) {
        em_record_pc(&e, pc);
        uint32_t instr = ir->code[pc];
        uint8_t  op    = (uint8_t)((instr >> 24) & 0xFF);
        uint8_t  dst   = (uint8_t)((instr >> 20) & 0x0F);
        uint8_t  srca  = (uint8_t)((instr >> 16) & 0x0F);
        uint8_t  srcb  = (uint8_t)((instr >> 12) & 0x0F);
        int32_t  imm12 = (int32_t)((instr & 0xFFF) | ((instr & 0x800) ? 0xFFFFF000 : 0));

        if(!op_is_inlined(op)) {
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

        /* ---- Peephole: fuse CMP + JT/JF --------------------------------
         * Only when (1) op is a CMP, (2) next IR word exists and is JT/JF,
         * (3) the JT/JF reads our CMP's destination, and (4) the JT/JF is
         * not itself the target of any branch. */
        {
            uint8_t setcc_op = setcc_for_cmp(op);
            if(setcc_op && pc + 1 < ir->len && !is_branch_target[pc + 1]) {
                uint32_t nxt   = ir->code[pc + 1];
                uint8_t  nxt_o = (uint8_t)((nxt >> 24) & 0xFF);
                uint8_t  nxt_a = (uint8_t)((nxt >> 16) & 0x0F);
                if((nxt_o == OP_JT || nxt_o == OP_JF) && nxt_a == dst) {
                    int32_t target_pc = decode_jmp_target(nxt, pc + 1);
                    emit_cmp_branch_fused(&e, setcc_op, dst, srca, srcb,
                                          (nxt_o == OP_JT) ? 1 : 0, target_pc);
                    /* Consume the JT/JF as well. Point its pc_to_jit slot at
                     * the byte just after our fused block, so any defensive
                     * dispatch on it falls through to the next IR word. */
                    pc++;
                    if(pc < ir->len) e.pc_to_jit[pc] = (int32_t)e.len;
                    pc++;
                    continue;
                }
            }
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
                /* Two-word: next IR word is the 32-bit literal. */
                uint32_t lit = (pc + 1 < ir->len) ? ir->code[pc + 1] : 0;
                emit_op_ldc32(&e, dst, lit);
                /* The literal word is not a valid jump target, but mark its
                 * dispatch slot to point at the same JIT addr so a stray
                 * step1-fallback resume on it would still work. */
                pc++; /* extra advance for the literal word */
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

#if !defined(RELIC_SOFTFP)
            /* ---- f64 (d-bank) ops: dst/srca/srcb index vm->d[] for the
             * D* ops. Exceptions: DBITS_LO/HI write an r-reg dst;
             * DFROMBITS and I2D/U2D/D2I/D2U/F2D read r-regs; DLD's srca
             * and DST's dst are r-bank address regs. */
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
#endif /* !RELIC_SOFTFP */

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
                                 : pc + 1; /* invalid: fall through */
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
                /* Should be unreachable since op_is_inlined gates entry. */
                emit_step1_fallback(&e, pc);
                break;
        }

        pc++;
    }

    /* ---------------- Epilogue ---------------- */
    emit_epilogue(&e);

    if(e.oom) goto oom;

    /* ---------------- VirtualAlloc + copy ---------------- */
    SIZE_T page_sz = e.len;
    void *page = VirtualAlloc(NULL, page_sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if(!page) {
        if(err && err_sz > 0) {
            snprintf(err, err_sz,
                     "VirtualAlloc failed (%lu)", (unsigned long)GetLastError());
        }
        em_free(&e); free(dispatch); free(is_branch_target);
        return false;
    }
    memcpy(page, e.buf, e.len);

    /* ---------------- Resolve relocations ----------------
     * For each reloc, write the rel32 displacement into the 4-byte slot.
     * target_pc == ir->len means "epilogue" (sentinel).
     * Since we now know `page`, we can encode rel32 = target_addr - (slot+4). */
    for(size_t i = 0; i < e.relocs_len; i++) {
        Reloc *r = &e.relocs[i];
        int32_t target_off;
        if(r->target_pc == (int32_t)e.pc_count) {
            target_off = e.epilogue_off;
        } else if(r->target_pc < 0 || r->target_pc >= (int32_t)e.pc_count) {
            target_off = e.epilogue_off; /* defensive: bail */
        } else {
            target_off = e.pc_to_jit[r->target_pc];
            if(target_off < 0) target_off = e.epilogue_off;
        }
        int32_t disp = (int32_t)target_off - (int32_t)(r->jit_off + 4);
        memcpy((uint8_t*)page + r->jit_off, &disp, 4);
    }

    /* ---------------- Build dispatch table ---------------- */
    for(size_t i = 0; i < e.pc_count; i++) {
        int32_t off = e.pc_to_jit[i];
        if(off < 0) {
            /* No code emitted at this IR-word (e.g. immediate after LDC32).
             * Point at the epilogue as a defensive fallback. */
            dispatch[i] = (uint8_t*)page + e.epilogue_off;
        } else {
            dispatch[i] = (uint8_t*)page + off;
        }
    }
    /* Sentinel slot used when main() returns - lands in epilogue. */
    dispatch[e.pc_count] = (uint8_t*)page + e.epilogue_off;

    em_free(&e);
    free(is_branch_target);

    /* ---------------- Seal ---------------- */
    DWORD old_prot = 0;
    if(!VirtualProtect(page, page_sz, PAGE_EXECUTE_READ, &old_prot)) {
        if(err && err_sz > 0) {
            snprintf(err, err_sz,
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
    jc->entry        = (int (__cdecl *)(RelicVM*))page;
    *out_code = jc;
    return true;

oom:
    em_free(&e);
    free(dispatch);
    free(is_branch_target);
oom_post:
    if(err && err_sz > 0) {
        const char *m = "out of memory while assembling JIT code";
        int n = (int)strlen(m); if(n >= err_sz) n = err_sz - 1;
        memcpy(err, m, n); err[n] = 0;
    }
    return false;
}

static int x86_win_run(RelicJitCode *code, RelicVM *vm) {
    return code->entry(vm);
}

static void x86_win_free(RelicJitCode *code) {
    if(!code) return;
    if(code->code) VirtualFree(code->code, 0, MEM_RELEASE);
    free(code->dispatch);
    free(code);
}

#endif /* X86_WIN_HAVE_TARGET */

RelicJit relic_jit = {
    .name    = "x86-win",
    .compile = x86_win_compile,
    .run     = x86_win_run,
    .free    = x86_win_free,
};
