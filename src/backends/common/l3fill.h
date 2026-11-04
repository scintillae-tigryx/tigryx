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
/* l3fill.h - shared @lines3d fill-mode dispatch.
 *
 * Include AFTER lines3d.h, sdfill.h and sdtrue.h in the backend source.
 * Gives every backend the same fill-mode branch the OpenGL backends
 * carry inline: (mode&128)&&(mode&32) -> sdf_render / sdt_render draws
 * the frame itself, the backend's present callback pushes the image to
 * its surface, and the call reports "already drawn" (bit 30). Anything
 * else falls through to the caller's relic_lines3d record projector.
 *
 * Usage:
 *     static void my_sdf_present(void *ctx, int true_mode) { ... }
 *     static void my_gfx_lines3d(RelicVM *vm) {
 *         MyState *s = vm->backend_data;
 *         i32 r = relic_l3_fill(vm, my_sdf_present, s);
 *         if (r != L3F_NOTFILL) { vm->r[1] = r; return; }
 *         relic_lines3d(vm, s->logical_w, s->logical_h);
 *     }
 */
#ifndef RELIC_L3FILL_H
#define RELIC_L3FILL_H

#define L3F_NOTFILL  (-2147483647)  /* not a fill-mode call: run records */

#if defined(__ia16__)
/* 16-bit real-mode targets cannot host the fill renderer: sdtrue needs
 * ~2MB of static buffers and sdfill/sdtrue take near pointers while
 * vm->mem is __far there. The mode bits still mean the same thing - the
 * caller simply falls through to the record projector, and l3f_palette
 * is a no-op. */
#define L3F_HAS_FILL 0
static int l3f_hires;
static void l3f_config(int want_hires) { (void)want_hires; }
static void l3f_palette(const RELIC_FAR u8 *rgba, int n) {
    (void)rgba; (void)n;
}
static i32 relic_l3_fill(RelicVM *vm,
                         void (*present)(void *ctx, int true_mode),
                         void *ctx) {
    (void)vm; (void)present; (void)ctx;
    return L3F_NOTFILL;
}

#else /* !__ia16__ */

#define L3F_HAS_FILL 1

#include "sdfill.h"
#include "sdtrue.h"

/* Feed the fill renderer the same @palette_upload block (RGBA quads,
 * max 16) every backend forwards. */
static void l3f_palette(const u8 *rgba, int n) { sdf_palette(rgba, n); }

/* Per-frame renderer configuration, identical across backends: cap bit
 * 26 requests the hires variant; TGX_SDF_HIRES=1/0 forces it, and
 * TGX_SDF_COLW=<4..32> / TGX_SDF_SORT=<0|1|2> / TGX_SDF_NST=<1..128>
 * override the parts. */
static int l3f_hires;
static void l3f_config(int want_hires) {
    static int init = 0;
    static int e_hi = -1, e_colw = 0, e_sort = -1, e_nst = 0;
    int hi;
    if (!init) {
        const char *e;
        init = 1;
        e = getenv("TGX_SDF_HIRES"); if (e && e[0]) e_hi = atoi(e) != 0;
        e = getenv("TGX_SDF_COLW");  if (e && e[0]) e_colw = atoi(e);
        e = getenv("TGX_SDF_SORT");  if (e && e[0]) e_sort = atoi(e);
        e = getenv("TGX_SDF_NST");   if (e && e[0]) e_nst = atoi(e);
    }
    hi = e_hi >= 0 ? e_hi : want_hires;
    l3f_hires = hi;
    sdf_set_colw(e_colw ? e_colw : hi ? 4 : SDF_COLW);
    sdf_sort = e_sort >= 0 ? e_sort : hi ? 2 : 0;
    sdf_nstcap = e_nst ? e_nst : hi ? SDF_NSTMAX : SDF_NST;
    if (sdf_nstcap < 1) sdf_nstcap = 1;
    if (sdf_nstcap > SDF_NSTMAX) sdf_nstcap = SDF_NSTMAX;
}

/* Fill-mode dispatch. present(ctx, true_mode) is called with the frame
 * already rendered into sdf_fb / sdt_px: true_mode 0 = MODE 2 bytes
 * (sdf_px(x,row), 160x224 pixels, each 2x1 logical), 1 = the sdtrue
 * RGBA frame (sdt_px[y][x], SDT_W x 448, the 320x224 view at 2x). */
static i32 relic_l3_fill(RelicVM *vm,
                         void (*present)(void *ctx, int true_mode),
                         void *ctx) {
    i32 mode = vm->r[10], cap = vm->r[9], n;
    int style;
    if (!(mode & 128) || !(mode & 32) || getenv("TGX_SDF_LEGACY"))
        return L3F_NOTFILL;
    style = (cap >> 27) & 7;
    l3f_config((cap >> 26) & 1);
    /* Texture styles 3/4 (tiers above LOW): the true-texture renderer,
     * sdtrue.h. The app passes its loaded SDXTEX.bin in the span-output
     * argument (r8), unused by the fill path. */
    if (style >= 3 && vm->r[8] > 0 && (u32)vm->r[8] < vm->mem_sz) {
        u32 tp = (u32)vm->r[8], w = (u32)vm->r[1];
        if (sdt_load(vm->mem + tp, vm->mem_sz - tp) &&
            (u64)w + 2 <= vm->mem_sz) {
            int nv = (i16)(vm->mem[w] | (vm->mem[w + 1] << 8));
            u32 wt = w + 2 + (u32)nv * 4;
            if (nv >= 0 && (u64)wt + (u64)vm->r[2] * 12 <= vm->mem_sz) {
                n = sdt_render((const i16 *)(vm->mem + wt), vm->r[2],
                               vm->r[3], vm->r[4], vm->r[5], vm->r[6],
                               vm->r[7],
                               sdf_roommask(vm->mem, vm->mem_sz, w,
                                            vm->r[2], vm->r[3], vm->r[4],
                                            vm->r[5], vm->r[6],
                                            (cap >> 16) & 255),
                               ((mode >> 28) & 7) * 32, (cap >> 24) & 1,
                               style);
                if (n >= 0) {
                    /* TGX_SDT_RAW=<path>: dump the RGBA frame on commit
                     * TGX_SDF_RAW_FRAME (default 30). */
                    static u32 sdt_cmt;
                    const char *rp = getenv("TGX_SDT_RAW");
                    if (rp && rp[0]) {
                        const char *rf = getenv("TGX_SDF_RAW_FRAME");
                        u32 want = (rf && rf[0]) ? (u32)strtoul(rf, NULL, 10)
                                                 : 30u;
                        if (sdt_cmt == want + 1u) {
                            FILE *f = fopen(rp, "wb");
                            if (f) { fwrite(sdt_px, 4, SDT_W * SDT_H, f);
                                     fclose(f); }
                        }
                    }
                    sdt_cmt++;
                    present(ctx, 1);
                    return n | 0x40000000;
                }
            }
        }
    }
    n = sdf_render(vm->mem, vm->mem_sz, (u32)vm->r[1], vm->r[2],
                   vm->r[3], vm->r[4], vm->r[5], vm->r[6], vm->r[7],
                   vm->r[9], mode);
    if (n < 0) return -1;
    {
        /* TGX_SDF_RAW=<path>: the MODE 2 bytes (256 rows x 80,
         * row-major) of frame TGX_SDF_RAW_FRAME (default 30), for byte
         * comparison against a BBC screen dump. */
        const char *rp = getenv("TGX_SDF_RAW");
        if (rp && rp[0]) {
            const char *rf = getenv("TGX_SDF_RAW_FRAME");
            u32 want = (rf && rf[0]) ? (u32)strtoul(rf, NULL, 10) : 30u;
            if (sdf_cmt == want + 1u) {
                FILE *f = fopen(rp, "wb");
                if (f) { fwrite(sdf_fb, 1, sizeof sdf_fb, f); fclose(f); }
            }
        }
    }
    present(ctx, 0);
    return n | 0x40000000;
}

/* The MODE 2 colour table, RGBA byte order (as the GL backends upload
 * it): entry c -> r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) &
 * 0xFF. Indices 8-15 duplicate 0-7 for the halftone variants. */
static const u32 l3f_bbc[16] = {
    0xFF000000, 0xFF0000FF, 0xFF00FF00, 0xFF00FFFF,
    0xFFFF0000, 0xFFFF00FF, 0xFFFFFF00, 0xFFFFFFFF,
    0xFF000000, 0xFF0000FF, 0xFF00FF00, 0xFF00FFFF,
    0xFFFF0000, 0xFFFF00FF, 0xFFFFFF00, 0xFFFFFFFF
};

#endif /* !__ia16__ */
#endif /* RELIC_L3FILL_H */
