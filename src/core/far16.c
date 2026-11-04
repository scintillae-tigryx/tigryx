/*
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
/* =========================================================================
 * far16.c — far-memory portability layer (16-bit real-mode targets)
 *
 * On ia16 (__far exists, but segment arithmetic does NOT normalize) these
 * helpers provide huge-pointer-style access to objects larger than one
 * 64K segment, allocated with _dos_allocmem.  On flat-addressing hosts the
 * same functions are plain malloc/memcpy/fread wrappers so shared code
 * (vm.c, ir.c) never needs to know which world it lives in.
 *
 * Implemented in its own translation unit so the 16-bit portability layer
 * stays out of the core sources - the same idea as the per-platform
 * backend/ directory split, but for address-space quirks rather than
 * graphics/audio.
 * ========================================================================= */
#include "tigryx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RELIC_IA16
#include <libi86/string.h>   /* _fmemcpy, _fmemset, _fmemmove */
#endif

#ifdef RELIC_RISCOS
#include <kernel.h>
#include <swis.h>

/* ro_swi - _kernel_swi replacement that works on RISC OS 3.1.
 *
 * SCL's _kernel_swi dispatches every call through OS_CallASWIR12
 * (SWI &71), which was only introduced in RISC OS 3.5.  On RO3.1x
 * (Arculator A5000 et al) each call fails with "SWI &71 not known",
 * silently killing every OS service the backend touches.
 *
 * Instead we patch a literal `SWI <num>` instruction into a two-word
 * data stub and call through it.  The stub path is only ever taken on
 * kernels old enough to lack CallASWIR12, i.e. ARM2/ARM3-class machines
 * with no cache (ARM2) or a unified write-through cache (ARM3), so no
 * code-area synchronisation is required.  Newer kernels keep using
 * _kernel_swi, so StrongARM/Iyonix/ARMv7 are unaffected.
 *
 * The X bit is always set so an error returns with V set + R0 = error
 * pointer - the same contract _kernel_swi exposes. */

static int ro_swi_broken = -1;    /* -1 unprobed, 0 _kernel_swi ok, 1 stub */

/* r0 = swi number (X bit OR'd in by caller), r1 = in regs ptr,
 * r2 = out regs ptr.  Patches the literal SWI instruction inline, loads
 * r0-r7 from in, issues it, stores r0-r7 to out; returns r0 (the error
 * pointer if V set, else 0).  The SWI executes inline so its return
 * flags (in particular V for X-SWI errors) reach movvc unmangled.
 * Only ever used on kernels lacking CallASWIR12, i.e. ARM2/ARM3-class
 * machines - no I-cache to synchronise there. */
__asm__ (
    "    .global ro_swi_dyn\n"
    "    .type ro_swi_dyn, %function\n"
    "ro_swi_dyn:\n"
    "    stmfd   sp!, {r8, lr}\n"
    "    mov     r8, r2\n"              /* out regs - callee-saved, SWI-safe */
    "    orr     r12, r0, #0xEF000000\n"
    "    ldr     r3,  =ro_swi_pt\n"
    "    str     r12, [r3]\n"
    "    mov     r12, r12\n"            /* let the store pass the prefetch   */
    "    mov     r12, r12\n"            /* window before the patched word    */
    "    ldmia   r1, {r0-r7}\n"         /* in regs                        */
    "ro_swi_pt:\n"
    "    swi     0\n"                   /* patched: SWI <no|X>            */
    "    stmia   r8, {r0-r7}\n"         /* out regs                       */
    "    movvc   r0, #0\n"             /* V clear -> success -> NULL     */
    "    ldmfd   sp!, {r8, pc}\n"
);
extern int ro_swi_dyn(int no, void *in_regs, void *out_regs);

_kernel_oserror *ro_swi(int no, _kernel_swi_regs *in, _kernel_swi_regs *out) {
    if (ro_swi_broken < 0) {
        /* Probe with XOS_Byte 0 (read OS version) - always succeeds if
         * _kernel_swi's dispatcher is usable. */
        _kernel_swi_regs p;
        memset(&p, 0, sizeof p);
        ro_swi_broken = (_kernel_swi(0x20006, &p, &p) != NULL);
    }
    if (!ro_swi_broken) return _kernel_swi(no, in, out);
    return (_kernel_oserror *)(size_t)ro_swi_dyn(no | 0x20000, in, out);
}

/* RISC OS 3.x application space is hard-capped at 28MB-32K: a large
 * --heap alone can nearly fill it, leaving SCL malloc too little room
 * for the JIT dispatch table and the wr_* renderer tables. Serve big
 * far-allocs from an OS dynamic area instead - it lives outside
 * application space and is bounded only by free RAM. Dynamic areas are
 * NOT released automatically when the task exits, so they are tracked
 * here and deleted both on relic_far_free and at exit. */
#define FAR_DA_MIN   (1u << 20)
#define FAR_DA_MAX   32

typedef struct { void *base; int area; } FarDA;
static FarDA far_da[FAR_DA_MAX];
static int   far_da_n = 0;
static int   far_da_hooked = 0;

static void far_da_release_all(void) {
    int i;
    for(i = 0; i < far_da_n; i++) {
        _kernel_swi_regs r;
        r.r[0] = 1; r.r[1] = far_da[i].area;
        ro_swi(OS_DynamicArea, &r, &r);
    }
    far_da_n = 0;
}

static int far_da_index(const void *p) {
    int i;
    for(i = 0; i < far_da_n; i++)
        if(far_da[i].base == p) return i;
    return -1;
}

static void *far_da_alloc(u32 bytes) {
    _kernel_swi_regs r;
    memset(&r, 0, sizeof(r));
    r.r[0] = 0;                /* create */
    r.r[1] = -1;               /* OS allocates the area number */
    r.r[2] = (int)bytes;       /* initial size */
    r.r[3] = -1;               /* OS allocates the base address */
    r.r[4] = 0;                /* flags: cached, buffered, singly-mapped */
    r.r[5] = (int)bytes;       /* maximum size = initial size */
    r.r[6] = 0;                /* no handler routine */
    r.r[7] = 0;                /* workspace (unused, R6 = 0) */
    r.r[8] = (int)"Relic heap";
    if(ro_swi(OS_DynamicArea, &r, &r)) return NULL;
    if(!far_da_hooked) { atexit(far_da_release_all); far_da_hooked = 1; }
    if(far_da_n < FAR_DA_MAX) {
        far_da[far_da_n].base = (void *)r.r[3];
        far_da[far_da_n].area = r.r[1];
        far_da_n++;
    }
    return (void *)r.r[3];
}
#endif

void RELIC_FAR *relic_far_alloc(u32 bytes) {
#ifdef RELIC_IA16
    unsigned paras = (unsigned)((bytes + 15uL) >> 4);
    unsigned seg = 0;
    if(paras == 0) paras = 1;
    if(_dos_allocmem(paras, &seg) != 0) return NULL;
    return MK_FP(seg, 0);
#else
    void *p;
#ifdef RELIC_RISCOS
    if(bytes >= FAR_DA_MIN) {
        p = far_da_alloc(bytes);
        if(p) return p;
    }
#endif
    p = malloc((size_t)bytes);
    return p;
#endif
}

void relic_far_free(void RELIC_FAR *p) {
#ifdef RELIC_IA16
    if(p) _dos_freemem(FP_SEG(p));
#else
#ifdef RELIC_RISCOS
    {
        int i = far_da_index(p);
        if(i >= 0) {
            _kernel_swi_regs r;
            r.r[0] = 1; r.r[1] = far_da[i].area;
            ro_swi(OS_DynamicArea, &r, &r);
            far_da[i] = far_da[--far_da_n];
            return;
        }
    }
#endif
    free(p);
#endif
}

u32 relic_far_avail(void) {
#ifdef RELIC_IA16
    /* _dos_allocmem returns the largest free block size in `seg` when the
     * request cannot be satisfied. */
    unsigned seg = 0;
    _dos_allocmem(0xFFFF, &seg);
    return (u32)seg << 4;
#else
    return 0xFFFFFFFFu;
#endif
}

/* Bytes remaining before `p` hits the end of its segment. */
static u32 relic_far_window(const void RELIC_FAR *p, u32 n) {
#ifdef RELIC_IA16
    u32 w = 0x10000uL - (u32)FP_OFF(p);
    return n < w ? n : w;
#else
    (void)p; return n;
#endif
}

void relic_far_write(u8 RELIC_FAR *dst, u32 off, const void RELIC_FAR *src, u32 n) {
#ifdef RELIC_IA16
    while(n) {
        u8         RELIC_FAR *d = relic_fp_add(dst, off);
        const u8   RELIC_FAR *s = (const u8 RELIC_FAR *)src;
        u32 w = relic_far_window(d, n);
        u32 sw = relic_far_window(s, n);
        if(sw < w) w = sw;
        _fmemcpy(d, s, (size_t)w);
        off += w;
        src = relic_cfp_add(s, w);
        n   -= w;
    }
#else
    memcpy(dst + off, src, (size_t)n);
#endif
}

void relic_far_read(const u8 RELIC_FAR *src, u32 off, void RELIC_FAR *dst, u32 n) {
#ifdef RELIC_IA16
    while(n) {
        const u8 RELIC_FAR *s = relic_cfp_add(src, off);
        u8         RELIC_FAR *d = (u8 RELIC_FAR *)dst;
        u32 w = relic_far_window(s, n);
        u32 dw = relic_far_window(d, n);
        if(dw < w) w = dw;
        _fmemcpy(d, s, (size_t)w);
        off += w;
        dst = relic_fp_add((u8 RELIC_FAR *)d, w);
        n   -= w;
    }
#else
    memcpy(dst, src + off, (size_t)n);
#endif
}

void relic_far_set(u8 RELIC_FAR *dst, u32 off, int c, u32 n) {
#ifdef RELIC_IA16
    while(n) {
        u8 RELIC_FAR *d = relic_fp_add(dst, off);
        u32 w = relic_far_window(d, n);
        _fmemset(d, c, (size_t)w);
        off += w;
        n   -= w;
    }
#else
    memset(dst + off, c, (size_t)n);
#endif
}

void relic_far_move(u8 RELIC_FAR *base, u32 dst, u32 src, u32 n) {
#ifdef RELIC_IA16
    if(dst == src || n == 0) return;
    if(dst < src) {
        while(n) {
            u8 RELIC_FAR *d = relic_fp_add(base, dst);
            u8 RELIC_FAR *s = relic_fp_add(base, src);
            u32 w = relic_far_window(d, n);
            u32 sw = relic_far_window(s, n);
            if(sw < w) w = sw;
            _fmemmove(d, s, (size_t)w);
            dst += w; src += w; n -= w;
        }
    } else {
        /* Backward pass to preserve memmove overlap semantics. `w` is the
         * number of bytes ending at dst+n-1 that stay within its segment. */
        while(n) {
            u8 RELIC_FAR *d = relic_fp_add(base, dst + n - 1);
            u8 RELIC_FAR *s = relic_fp_add(base, src + n - 1);
            u32 w = (u32)FP_OFF(d) + 1;
            u32 sw = (u32)FP_OFF(s) + 1;
            if(sw < w) w = sw;
            if(w > n) w = n;
            _fmemmove(relic_fp_add(base, dst + n - w),
                      relic_fp_add(base, src + n - w),
                      (size_t)w);
            n -= w;
        }
    }
#else
    memmove(base + dst, base + src, (size_t)n);
#endif
}

size_t relic_far_str(char *dst, const char RELIC_FAR *src, size_t cap) {
    size_t i = 0;
    if(cap == 0) return 0;
    while(i + 1 < cap) {
        char c = *(const char RELIC_FAR *)relic_cfp_add((const u8 RELIC_FAR *)src, (u32)i);
        dst[i++] = c;
        if(c == 0) return i - 1;
    }
    dst[i] = 0;
    return i;
}

/* stdio needs near pointers on ia16 - bounce through a small buffer.
 * Shared between both functions (they never interleave - single-threaded,
 * one call completes before another starts): two 4K buffers would cost
 * 8K of DGROUP, which the near heap cannot afford. Static (not stack):
 * the real-mode stack is only a few KB. */
#ifdef RELIC_IA16
static u8 relic_io_bounce[2048];
#endif

size_t relic_fread_far(FILE *f, u8 RELIC_FAR *dst, u32 n) {
#ifdef RELIC_IA16
    u32 total = 0;
    while(total < n) {
        u32 want = n - total; if(want > sizeof(relic_io_bounce)) want = sizeof(relic_io_bounce);
        size_t got = fread(relic_io_bounce, 1, (size_t)want, f);
        if(got) relic_far_write(dst, total, relic_io_bounce, (u32)got);
        total += (u32)got;
        if(got < want) break;
    }
    return (size_t)total;
#elif defined(RELIC_RISCOS)
    /* Arculator's HostFS never implements OS_GBPB (hostfs_gbpb is an empty
     * stub): SCL fread switches to GBPB for bulk reads and spins forever.
     * Byte reads go through OS_BGet, which works - read via fgetc. */
    u32 total = 0;
    u8 buf[256];
    while(total < n) {
        u32 want = n - total, j;
        int c;
        if(want > sizeof(buf)) want = sizeof(buf);
        for(j = 0; j < want; j++) {
            c = fgetc(f);
            if(c == EOF) return (size_t)total;
            buf[j] = (u8)c;
        }
        relic_far_write(dst, total, buf, want);
        total += want;
    }
    return (size_t)total;
#else
    return fread(dst, 1, (size_t)n, f);
#endif
}

size_t relic_fwrite_far(FILE *f, const u8 RELIC_FAR *src, u32 n) {
#ifdef RELIC_IA16
    u32 total = 0;
    while(total < n) {
        u32 want = n - total; if(want > sizeof(relic_io_bounce)) want = sizeof(relic_io_bounce);
        relic_far_read(src, total, relic_io_bounce, want);
        size_t put = fwrite(relic_io_bounce, 1, (size_t)want, f);
        total += (u32)put;
        if(put < want) break;
    }
    return (size_t)total;
#else
    return fwrite(src, 1, (size_t)n, f);
#endif
}
