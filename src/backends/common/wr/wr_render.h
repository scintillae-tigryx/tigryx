/*
 * wr_render.h
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

/* wr_render.h - shared sector-based "wr_*" world renderer (see OP_WR_* in
 * tigryx.h): BSP front-to-back walk with a solid-segment clip list (so
 * every screen column is written at most once - no z-buffer), per-column
 * textured walls, visplane floors/ceilings, colormap lighting. Ported
 * verbatim from the original allegro-fixed-dos implementation (see git
 * history) so every backend that wants @wr_* support gets the exact same
 * proven algorithm and profiling instrumentation, rather than a
 * reimplementation that could silently diverge in behaviour.
 *
 * This is a HEADER, not a .c file, and is meant to be #included into
 * exactly one place in exactly one backend .c file - the same pattern
 * allegro-tinygl-dos already uses to reuse allegro-dos's headers. That
 * keeps every function `static` and in the SAME translation unit as the
 * rest of that backend, so the compiler's cross-function inlining
 * decisions are unaffected by this code living in a separate file - this
 * matters a great deal for the DOS target, which is tuned against real
 * 486SX hardware (see AGENTS.md) and must not regress.
 *
 * ---- Portability contract ----
 * The renderer is written against a small set of macros/functions the
 * includer must define BEFORE `#include "wr_render.h"`. All of them are
 * either plain textual macros (zero runtime cost - for the existing DOS
 * backend they expand to exactly the same expressions the code used
 * before this file existed) or ordinary functions the includer supplies
 * for backend-specific concerns (hardware palette upload, VRAM bank
 * switching) that a modern backend simply doesn't have.
 *
 *   WrHostState              - type alias for the backend's own state
 *                               struct (e.g. `#define WrHostState
 *                               AllegroState`). Must have the wr_* fields
 *                               below (same names as the original
 *                               AllegroState.wr_* fields):
 *                                 wr_playpal[768], wr_colormap,
 *                                 wr_num_colormaps, wr_palette_set,
 *                                 wr_textures[WR_MAX_TEXTURES]{data,w,h},
 *                                 wr_flats[WR_MAX_FLATS],
 *                                 wr_tex_avg_color/valid[WR_MAX_TEXTURES],
 *                                 wr_flat_avg_color/valid[WR_MAX_FLATS],
 *                                 wr_map_set, wr_vertices/wr_n_vertices,
 *                                 wr_linedefs/wr_n_linedefs,
 *                                 wr_sidedefs/wr_n_sidedefs,
 *                                 wr_sectors/wr_n_sectors,
 *                                 wr_segs/wr_n_segs,
 *                                 wr_subsectors/wr_n_subsectors,
 *                                 wr_nodes/wr_n_nodes,
 *                                 wr_sd_tex, wr_sec_tex, wr_texmap_set,
 *                                 wr_sky_tex_id, wr_flatshade,
 *                                 wr_area_x/y/w/h, win_flags, frame_dirty.
 *   WR_HAS_BUF(s)             - true if the backbuffer/surface exists.
 *   WR_ROW(s, y)              - unsigned char* to the first byte of row y.
 *   WR_PITCH(s, y)            - bytes from row y to row y+1.
 *   WR_BUF_W(s), WR_BUF_H(s)  - backbuffer/surface dimensions in pixels.
 *   WR_MILLIS()               - current time in milliseconds (profiling).
 *   wr_host_clear_backbuf(WrHostState *s, int ox, int oy, int w, int h)
 *                             - clear the sub-rectangle [ox,oy,w,h) of the
 *                               backbuffer to palette index 0.
 *   wr_host_apply_palette(WrHostState *s)
 *                             - called once after wr_playpal is filled in
 *                               (@wr_palette); upload to hardware if the
 *                               backend has a real 8bpp palette, else no-op.
 *   wr_host_on_map_uploaded(WrHostState *s)
 *                             - called once after a @wr_map upload, for
 *                               any backend-specific fixup (e.g. DOS's
 *                               planar-backbuffer -> RAM swap). May no-op.
 *   wr_host_present(WrHostState *s, int x, int y, int w, int h)
 *                             - called after the sub-rectangle [x,y,w,h)
 *                               of the backbuffer/surface has just been
 *                               written (once per @wr_render, once per
 *                               @wr_test, once per @wr_patch). On DOS the
 *                               backbuffer IS the display surface, so
 *                               this is a no-op - @flip presents it as
 *                               usual. On a backend with no persistent
 *                               8bpp framebuffer (e.g. a GL backend),
 *                               this is where that sub-rectangle gets
 *                               expanded through the palette and drawn
 *                               as a textured quad *immediately*, so it
 *                               composites correctly with any ordinary
 *                               2D drawing calls interleaved in the same
 *                               frame (before or after) under the
 *                               backend's normal immediate-mode ordering
 *                               - exactly as if it were just another
 *                               @sprite/@rounded_rectangle_filled call.
 *
 *   WR_ALLOC(n) / WR_FREE(p)  - optional allocator for the texture, flat
 *                               and colormap copies the hot pixel loops
 *                               read (defaults to malloc/free). A backend
 *                               with fast/slow RAM tiers can put these in
 *                               fast RAM.
 *
 * Also requires wr_types.h to already be included (WrVertex etc.) and the
 * standard relic integer typedefs (i16/i32/u8/...) and RelicVM. */
#ifndef RELIC_WR_RENDER_H
#define RELIC_WR_RENDER_H

#ifndef WR_ALLOC
#define WR_ALLOC(n) malloc(n)
#define WR_FREE(p)  free(p)
#endif

/* MSVC has no __attribute__((always_inline)); __forceinline is its
 * equivalent (also understood by clang-cl, which defines _MSC_VER). */
#if defined(_MSC_VER)
#define WR_FORCE_INLINE static __forceinline
#else
#define WR_FORCE_INLINE static inline __attribute__((always_inline))
#endif

/* ---- Fixed-point conventions ----
 * wfixed_t is a 16.16 fixed-point number, the classic engine's convention.
 * There is no float anywhere in the hot path: the only float math in the
 * whole renderer is the once-per-frame view setup in wr_op_render
 * (cosf/sinf/tanf on the camera angle/fov), never per seg/node/column/
 * pixel. */
typedef int wfixed_t;
#define WR_FRACBITS 16
#define WR_FRACUNIT (1 << WR_FRACBITS)

/* ---- Pixel width ----
 * The engine renders 8bpp palette indices by default (DOS, planar C2P,
 * 8bpp hardware palettes). A backend that defines WR_16BPP before this
 * include renders u16 pixels instead: the colormap rows then hold u16
 * entries (palette folded in at upload - cmap16[texel] is the display
 * value directly), and WR_ROW/WR_PITCH address u16 elements. Under
 * WR_16BPP the contract additionally requires:
 *
 *   s->wr_rgb565_lut[256]  - u16 display values per palette index
 *   s->wr_pix2idx          - u8[65536] display-value -> palette index
 *                            (only for the spectre fuzz readback)
 *
 * All `cmap[texel]`/`cmap[pix]` sites are width-agnostic: the colormap
 * IS the output-pixel table in both modes. */
/* WRPix may be predefined by a backend that needs the type before this
 * include (e.g. in its WR_ROW macro). */
#ifndef WR_PIX_DEFINED
#define WR_PIX_DEFINED
#ifdef WR_16BPP
typedef u16 WrPix;
#else
typedef unsigned char WrPix;
#endif
#endif

/* Reinterpret the raw i32 bit pattern the VM passes for a float argument
 * (see tigryx_rc_shim.c's float_bits) as an actual float, without any
 * float<->int conversion. Named distinctly from any backend's own
 * equivalent union (e.g. AllegroFBits) so both can coexist in the same
 * translation unit. */
typedef union { i32 i; float f; } WrFBits;
static float wr_f32_from_bits(i32 bits) { WrFBits fb; fb.i = bits; return fb.f; }

/* 64-bit / 32-bit signed divide. On m68k, `(int64_t)a / b` compiles to a
 * libgcc __divdi3 CALL - a full 64/64 software division (~600+ cycles),
 * while the 68020+ divsl.l instruction does 64/32 in ~50. Every divide
 * in this file already produces a quotient that fits in 32 bits, so the
 * 64/32 hardware divide is exact for all of them. On other targets the
 * plain C divide is already optimal (x86 has idiv for this). */
static inline wfixed_t wr_div64(int64_t num, int den) {
#if defined(__mc68020) || defined(__mc68030) || defined(__mc68040) || \
    defined(__mc68060) || defined(__mc68020__) || defined(__mc68030__) || \
    defined(__mc68040__) || defined(__mc68060__)
    int lo = (int) num, hi = (int) (num >> 32);
    /* divsl %den,%hi:%lo = true 64/32 signed divide (DIVSL encoding,
     * quotient -> lo, remainder -> hi).  NOTE: `divsll %den,%hi,%lo`
     * looks identical but encodes the 32-bit-dividend quot+rem form -
     * the colon pair is load-bearing, do not drop it. */
    __asm__("divsl %2,%1:%0" : "+d"(lo), "+d"(hi) : "d"(den) : "cc");
    return (wfixed_t) lo;
#else
    return (wfixed_t) (num / den);
#endif
}

/* ---- Renderer profiling (ms-level, written to WRPROF.LOG) ----
 * The frame takes tens to hundreds of ms on a 486, so a 1kHz timer is
 * plenty of resolution. wr_op_render times the major phases and flushes
 * averages + last-frame values to WRPROF.LOG every 30 frames so the data
 * survives a hard power-off. Counters (segs visited, wall columns drawn,
 * plane pixels) are cheap increments and pin down whether the time is
 * going into the BSP walk, wall inner loop, or floor/ceiling spans. */
static unsigned int wrp_frames = 0;
static unsigned int wrp_setup_ms = 0;
static unsigned int wrp_clear_ms = 0;
static unsigned int wrp_bsp_ms = 0;
static unsigned int wrp_planes_ms = 0;
static unsigned int wrp_total_ms = 0;
static unsigned int wrp_last_setup = 0;
static unsigned int wrp_last_clear = 0;
static unsigned int wrp_last_bsp = 0;
static unsigned int wrp_last_dspbegin = 0;   /* wr_dsp_begin, last frame  */
static unsigned int wrp_last_dspframe = 0;   /* wr_dsp_walk_frame (walk + replay) */
static unsigned int wrp_last_planes = 0;
static unsigned int wrp_last_total = 0;
static unsigned int wrp_segs_seen = 0;     /* segs handed to wr_add_seg */
static unsigned int wrp_segs_drawn = 0;    /* segs that survived cull/clip */
static unsigned int wrp_wall_cols = 0;     /* wall columns actually drawn */
static unsigned int wrp_col_iters = 0;     /* column-loop iterations (incl. closed-out) */
static unsigned int wrp_wall_px = 0;       /* wall pixels written */
static unsigned int wrp_plane_rows = 0;    /* flat span rows drawn */
static unsigned int wrp_plane_px = 0;      /* flat pixels written */
static unsigned int wrp_map_calls = 0;     /* wr_map_plane calls (incl. early-outs) */
static unsigned int wrp_span_writes = 0;   /* wr_spanstart[] stores in wr_make_spans */
static unsigned int wrp_map_calls_tot = 0; /* same, accumulated across frames */
static unsigned int wrp_span_writes_tot = 0;
static unsigned int wrp_plane_rows_tot = 0;  /* plane rows/pixels, all frames */
static unsigned int wrp_plane_px_tot = 0;
static unsigned int wrp_plane_cols_tot = 0;  /* draw_planes column-scan iters */
static unsigned int wrp_sky_px_tot = 0;      /* sky pixels, all frames */
static unsigned int wrp_visplanes_tot = 0;   /* visplanes, all frames */
static unsigned int wrp_sky_ms = 0;          /* time inside wr_draw_sky_plane */
static unsigned int wrp_flat_ms = 0;         /* time in the make_spans loops */
static unsigned int wrp_sky_px = 0;        /* sky pixels written */
static unsigned int wrp_nodes_visited = 0;
static unsigned int wrp_subsec_visited = 0;
static unsigned int wrp_bbox_rejected = 0;
static unsigned int wrp_dsp_walked = 0;    /* frames walked on the DSP  */
static unsigned int wrp_cpu_walked = 0;    /* frames walked on the CPU  */
/* First-frame counter snapshot: under --auto-walk the camera starts
 * identical in every run, so DSP vs NODSP first-frame counters are a
 * bit-exactness check on the walk stream. */
static unsigned int wrp_f1_nodes = 0, wrp_f1_subs = 0, wrp_f1_bbox = 0;
static unsigned int wrp_f1_seen = 0, wrp_f1_drawn = 0, wrp_f1_cols = 0;
static unsigned int wrp_f1_wallpx = 0, wrp_f1_walked = 0;
/* Visplanes allocated this frame, and how many of those were plane
 * splits rather than fresh entries. Every allocation memsets a
 * full screen-width short array, so a high count here is real per-frame
 * memory traffic (and cache pressure) that the pixel counters below do not
 * account for. */
static unsigned int wrp_visplanes = 0;
static unsigned int wrp_plane_splits = 0;

/* TEMP PROFILING toggles (read once from WRSKIP.DAT, first byte):
 * 'c' = skip per-column loop, 'w' = skip wall-texture draws only. */
static int wr_skip_mode = -1;
#ifdef WR_DSP_COLS
/* WRCPU=1 opens a per-seg cull trace so a CPU reference run can be diffed
 * against the DSP record stream seg-by-seg. */
static FILE *wr_cull_log(void) {
    static FILE *f; static int init;
    if (!init) { init = 1; if (getenv("WRCPU")) f = fopen("CPUS.TXT", "w"); }
    return f;
}
#endif

static void wr_skip_init(void) {
    if (wr_skip_mode < 0) {
        FILE *f = fopen("WRSKIP.DAT", "rb");
        int c = f ? fgetc(f) : EOF;
        if (f) fclose(f);
        wr_skip_mode = (c == 'c') ? 1 : (c == 'w') ? 2 : (c == 's') ? 3 : (c == 't') ? 4 : (c == 'p') ? 5 : (c == 'd') ? 6 : (c == 'm') ? 7 : 0;
    }
}

/* TEMP A/B: a WRCOL.DAT file in the CWD forces the C seg column loop
 * instead of the m68k asm one (same-binary comparison / fallback). */
static int wr_no_asm_col = -1;
static void wr_col_init(void) {
    if (wr_no_asm_col < 0) {
        FILE *f = fopen("WRCOL.DAT", "rb");
        wr_no_asm_col = f ? 1 : 0;
        if (f) fclose(f);
    }
}

/* wr_plane_buf / wr_plane_buf_w (the visplane column-extent storage) are
 * declared in wr_types.h, included near the top of the backend .c file
 * so the backend's shutdown path can free them without needing this
 * whole header visible at that point in the file. */

/* Logical render resolution and pixel offset of the 3D render area inside
 * the backbuffer. Set by wr_op_render from WrHostState.wr_area_*.
 * The BSP/projection math uses wr_screen_w/h; pixel writes add
 * wr_origin_x/y. Declared early so the draw functions below can
 * reference them. */
static int wr_screen_w, wr_screen_h;
static int wr_origin_x, wr_origin_y;

/* Low-detail pixel scaling (the classic "low detail" mode): 1 = full res,
 * 2 = each logical pixel stored as a 2-pixel block. The projection,
 * clip arrays, solidsegs, visplane extents and column walk all run in
 * *logical* pixels (wr_screen_w/h are logical), so halving the column
 * count also halves the per-column divides, clip-array traffic, plane
 * scans and - the dominant cost on a cacheless CPU - the dependent
 * texture+colormap loads. Only the final store maps logical->physical
 * by writing sx x sy pixel blocks. Set via @wr_flatshade r1 bits 8-9. */
static int wr_px_sx = 1, wr_px_sy = 1;

/* "Stripe-shade" (@wr_flatshade r1 bit 1): the middle rung between full
 * texture mapping and flat shade. Wall columns still fill with a single
 * colour, but that colour is the average of the texture COLUMN being
 * sampled rather than one average for the whole texture - so vertical
 * bands, panel edges, door frames and highlights survive as stripes
 * while the inner loop stays a memset-class fill (one dependent colormap
 * load per column, zero per pixel). Floors/ceilings still take the
 * flat-average solid fill (a flat has no useful 1-D projection), and
 * masked midtextures stay skipped exactly as under flatshade. */
static int wr_stripshade = 0;

/* Draws one vertical strip of a wall texture into screen column dc_x, rows
 * dc_yl..dc_yh inclusive (already clipped by the caller against the
 * screen and, eventually, against the solid-segment list). dc_texturemid
 * is the texture-space row (16.16) that would land on the exact vertical
 * centre of the screen; dc_iscale is how many texture rows advance per
 * screen row (16.16) - both constant for the whole column, which is what
 * lets this loop stay divide-free. dc_colormap remaps each sampled
 * palette index through the current light level - the entire lighting
 * model is "look up a different colormap row per sector", the classic
 * approach. */
static void wr_draw_column(WrHostState *s, int dc_x, int dc_yl, int dc_yh,
        wfixed_t dc_iscale, wfixed_t dc_texturemid,
        const unsigned char *dc_source, int dc_texheight,
        const WrPix *dc_colormap) {
    if (!WR_HAS_BUF(s) || !dc_source || dc_texheight <= 0) return;
    if (dc_x < 0 || dc_x >= wr_screen_w) return;
    if (dc_yl < 0) dc_yl = 0;
    if (dc_yh >= wr_screen_h) dc_yh = wr_screen_h - 1;
    if (dc_yl > dc_yh) return;

    wrp_wall_cols++;
    wrp_wall_px += (unsigned int)(dc_yh - dc_yl + 1);

    wfixed_t frac = dc_texturemid + (dc_yl - (wr_screen_h >> 1)) * dc_iscale;
    /* Step the destination pointer by the bitmap pitch instead of doing a
     * line[y] table lookup per pixel. On a cacheless 486 each line[] load
     * is a real memory access; replacing it with an add saves ~2-4 cycles
     * per pixel, and walls are most of the screen. */
    WrPix *dst = WR_ROW(s, dc_yl * wr_px_sy + wr_origin_y) + (dc_x * wr_px_sx + wr_origin_x);
    int pitch = WR_PITCH(s, dc_yl * wr_px_sy + wr_origin_y);
    int npix = dc_yh - dc_yl + 1;       /* pixel count (inclusive range) */
    if (wr_px_sx > 1 || wr_px_sy > 1) {
        /* Low-detail path: one sample fills an sx-by-sy pixel block.
         * The sampling math is unchanged; the stores widen. */
        int rstep = pitch * wr_px_sy;
        if ((dc_texheight & (dc_texheight - 1)) == 0) {
            unsigned mask = (unsigned) dc_texheight - 1u;
            int yy;
            for (yy = 0; yy < npix; yy++, dst += rstep, frac += dc_iscale) {
                WrPix p = dc_colormap[dc_source[((unsigned) (frac >> WR_FRACBITS)) & mask]];
                dst[0] = p;
                if (wr_px_sx > 1) dst[1] = p;
                if (wr_px_sy > 1) {
                    dst[pitch] = p;
                    if (wr_px_sx > 1) dst[pitch + 1] = p;
                }
            }
        } else {
            wfixed_t hf = (wfixed_t) dc_texheight << WR_FRACBITS;
            int yy;
            frac %= hf;
            if (frac < 0) frac += hf;
            for (yy = 0; yy < npix; yy++, dst += rstep) {
                WrPix p = dc_colormap[dc_source[frac >> WR_FRACBITS]];
                frac += dc_iscale;
                while (frac >= hf) frac -= hf;
                dst[0] = p;
                if (wr_px_sx > 1) dst[1] = p;
                if (wr_px_sy > 1) {
                    dst[pitch] = p;
                    if (wr_px_sx > 1) dst[pitch + 1] = p;
                }
            }
        }
        return;
    }
    /* Per-pixel wrapping. A `%` here is a hardware divide PER PIXEL
     * (~40 cycles on a 486, and walls are most of the screen), so the column loop
     * splits into two cases, in the classic way:
     *   - power-of-two heights (all stock WAD textures: 16/32/64/128):
     *     wrap with a bitmask, zero extra cost;
     *   - anything else: reduce frac into [0, h<<16) once up front, then
     *     wrap with compare-and-subtract per pixel (dc_iscale is positive
     *     and in practice far smaller than the texture height, so the
     *     `while` almost never runs more than once). */
    if ((dc_texheight & (dc_texheight - 1)) == 0) {
        unsigned mask = (unsigned) dc_texheight - 1u;
#if defined(__DJGPP__) && defined(__i386__)
        /* Inline assembly texture-column loop. A column
         * needs 7 logical values (frac, iscale, mask,
         * source, colormap, dest, pitch) but only 6 real GPRs are safe
         * to use (ebp is the frame pointer here - see
         * the comment in wr_draw_span above for why ebp is
         * off-limits). mask and pitch are read-only every iteration and
         * never rewritten, so they are left as memory operands rather
         * than fighting for a 7th/8th register; ecx is pure scratch,
         * fully overwritten from frac every iteration via `movl`, so it
         * never carries stale state across iterations. Register roles:
         *   eax = colormap, ebx = frac (read-write), ecx = scratch index
         *   edx = source, esi = iscale, edi = dest (read-write)
         *   mask, pitch = memory operands; counter = memory operand. */
        {
            int cnt = npix;
            __asm__ __volatile__(
                "1:                             \n\t"
                "movl   %%ebx, %%ecx             \n\t"  /* ecx = frac */
                "shrl   $16, %%ecx               \n\t"  /* ecx = frac>>16 */
                "andl   %[mask], %%ecx           \n\t"  /* ecx &= mask */
                /* Sample texture, remap through colormap, write pixel.
                 * ecx is reused as scratch: first the texel index (read
                 * via edx+ecx), then the colormap index (read via
                 * eax+ecx).
                 *
                 * The load zero-extends straight into ecx rather than
                 * loading into cl and widening afterwards: a single
                 * movzx with a memory source
                 * is a 386 instruction and does both in one go: the
                 * address edx+ecx is formed from the OLD ecx before the
                 * result is written back to it, so a single
                 * `movzbl (%edx,%ecx), %ecx` is exactly equivalent to the
                 * pair and saves a 3-cycle register-to-register movzx on
                 * every pixel. */
                "movzbl (%%edx,%%ecx), %%ecx     \n\t"  /* ecx = src[idx] */
                "movb   (%%eax,%%ecx), %%cl      \n\t"  /* cl = cmap[texel] */
                "movb   %%cl, (%%edi)            \n\t"  /* *dest = cl */
                "addl   %%esi, %%ebx             \n\t"  /* frac += iscale */
                "addl   %[pitch], %%edi          \n\t"  /* dest += pitch */
                "decl   %[cnt]                    \n\t"  /* count-- */
                "jnz    1b                        \n\t"
                : [cnt] "+m" (cnt), "+b" (frac), "+D" (dst)
                : "a" (dc_colormap), "d" (dc_source), "S" (dc_iscale),
                  [mask] "m" (mask), [pitch] "m" (pitch)
                : "ecx", "memory", "cc"
            );
        }
#elif defined(__m68k__) || defined(__M68000__)
        /* m68k inner loop, same structure as the i386 version: frac in
         * d2 stepped by iscale (d3), index = (frac>>16)&mask via swap+and
         * (mask < 65536 so the and clears the swapped-in low half), one
         * dependent pair of byte loads per pixel, dest stepped by pitch
         * (d4). Register-asm locals pin each value; dbra counts d7.w.
         *   d0/d1 = scratch, d2 = frac, d3 = iscale, d4 = pitch,
         *   d5 = mask, d7 = count-1, a1 = dest, a2 = source, a3 = cmap */
        {
            register const unsigned char *asrc  __asm__("a2") = dc_source;
            register const WrPix           *acmap __asm__("a3") = dc_colormap;
            register WrPix                 *adst  __asm__("a1") = dst;
            register unsigned             afrac __asm__("d2") = (unsigned) frac;
            register int                  astep __asm__("d3") = dc_iscale;
            register int                  apit  __asm__("d4") = pitch * (int)sizeof(WrPix);
            register unsigned             amsk  __asm__("d5") = mask;
            /* 2x unrolled: dbra costs ~10 cycles taken, amortised over
             * two pixels. d1 is only ever written by the byte load, so
             * moveq once outside keeps it a valid 0-255 index forever.
             * The odd tail pixel is handled in C below. WR_16BPP widens
             * the two stores: word read from a u16 colormap, word write
             * down the column - d4 already holds the byte stride. */
            int pairs = npix >> 1;
            if (pairs) {
                register int acnt __asm__("d7") = pairs - 1;
#ifdef WR_16BPP
                __asm__ __volatile__(
                    "       moveq  #0, %%d1\n\t"
                    "1:\n\t"
                    "movel  %%d2, %%d0\n\t"
                    "swap   %%d0\n\t"
                    "andl   %%d5, %%d0\n\t"
                    "moveb  %%a2@(%%d0:l), %%d1\n\t"
                    "movew  %%a3@(0,%%d1:l:2), %%a1@\n\t"
                    "addl   %%d3, %%d2\n\t"
                    "addl   %%d4, %%a1\n\t"
                    "movel  %%d2, %%d0\n\t"
                    "swap   %%d0\n\t"
                    "andl   %%d5, %%d0\n\t"
                    "moveb  %%a2@(%%d0:l), %%d1\n\t"
                    "movew  %%a3@(0,%%d1:l:2), %%a1@\n\t"
                    "addl   %%d3, %%d2\n\t"
                    "addl   %%d4, %%a1\n\t"
                    "dbra   %%d7, 1b\n\t"
                    : "+r"(afrac), "+r"(adst), "+r"(acnt)
                    : "r"(asrc), "r"(acmap), "r"(astep), "r"(apit), "r"(amsk)
                    : "d0", "d1", "cc", "memory");
#else
                __asm__ __volatile__(
                    "       moveq  #0, %%d1\n\t"
                    "1:\n\t"
                    "movel  %%d2, %%d0\n\t"
                    "swap   %%d0\n\t"
                    "andl   %%d5, %%d0\n\t"
                    "moveb  %%a2@(%%d0:l), %%d1\n\t"
                    "moveb  %%a3@(%%d1:l), %%a1@\n\t"
                    "addl   %%d3, %%d2\n\t"
                    "addl   %%d4, %%a1\n\t"
                    "movel  %%d2, %%d0\n\t"
                    "swap   %%d0\n\t"
                    "andl   %%d5, %%d0\n\t"
                    "moveb  %%a2@(%%d0:l), %%d1\n\t"
                    "moveb  %%a3@(%%d1:l), %%a1@\n\t"
                    "addl   %%d3, %%d2\n\t"
                    "addl   %%d4, %%a1\n\t"
                    "dbra   %%d7, 1b\n\t"
                    : "+r"(afrac), "+r"(adst), "+r"(acnt)
                    : "r"(asrc), "r"(acmap), "r"(astep), "r"(apit), "r"(amsk)
                    : "d0", "d1", "cc", "memory");
#endif
            }
            if (npix & 1) {
                *adst = acmap[asrc[(afrac >> WR_FRACBITS) & amsk]];
            }
        }
#else
        /* C fallback: 2x unrolled loop. */
        {
            int count = npix - 1;
            while (count >= 1) {
                *dst = dc_colormap[dc_source[((unsigned) (frac >> WR_FRACBITS)) & mask]];
                dst += pitch;
                frac += dc_iscale;
                *dst = dc_colormap[dc_source[((unsigned) (frac >> WR_FRACBITS)) & mask]];
                dst += pitch;
                frac += dc_iscale;
                count -= 2;
            }
            if (count == 0) {
                *dst = dc_colormap[dc_source[((unsigned) (frac >> WR_FRACBITS)) & mask]];
            }
        }
#endif
    } else {
        wfixed_t hf = (wfixed_t) dc_texheight << WR_FRACBITS;
        frac %= hf;
        if (frac < 0) frac += hf;
        int y;
        for (y = dc_yl; y <= dc_yh; y++, dst += pitch) {
            *dst = dc_colormap[dc_source[frac >> WR_FRACBITS]];
            frac += dc_iscale;
            while (frac >= hf) frac -= hf;
        }
    }
}

/* Draws one horizontal strip of a 64x64 flat into screen row ds_y, columns
 * ds_x1..ds_x2 inclusive. ds_xfrac/ds_yfrac (16.16) are the flat-space
 * coordinates at column ds_x1; ds_xstep/ds_ystep are how far those move
 * per screen column - both constant for the whole span, so again no
 * per-pixel division. Flats are always exactly 64x64 in the WAD format, so wrapping
 * is a plain &63 rather than a general modulo.
 *
 * On DJGPP/i386 the inner loop uses inline assembly: xfrac and yfrac are packed into a
 * single 32-bit register so both texture coordinates are extracted with
 * two shldl instructions instead of four shift+mask operations, and all
 * loop state stays in registers with zero memory accesses inside the
 * loop. On a cacheless 486 each eliminated memory access is a real bus
 * cycle saved. The C fallback is a 2x unrolled loop. */
static void wr_draw_span(WrHostState *s, int ds_y, int ds_x1, int ds_x2,
        wfixed_t ds_xfrac, wfixed_t ds_yfrac, wfixed_t ds_xstep, wfixed_t ds_ystep,
        const unsigned char *ds_source, const WrPix *ds_colormap) {
    if (!WR_HAS_BUF(s) || !ds_source) return;
    if (ds_y < 0 || ds_y >= wr_screen_h) return;
    if (ds_x1 < 0) ds_x1 = 0;
    if (ds_x2 >= wr_screen_w) ds_x2 = wr_screen_w - 1;
    if (ds_x1 > ds_x2) return;

    wrp_plane_rows++;
    wrp_plane_px += (unsigned int)(ds_x2 - ds_x1 + 1);
    if (wr_skip_mode == 5) return;   /* TEMP PROFILING: WRSKIP=p - span setup only, no pixels */

    WrPix *row = WR_ROW(s, ds_y * wr_px_sy + wr_origin_y) + wr_origin_x;
    int npix = ds_x2 - ds_x1 + 1;        /* pixel count (inclusive range) */
    if (wr_px_sx > 1 || wr_px_sy > 1) {
        /* Low-detail path: one flat sample fills an sx-by-sy block. The
         * second physical row (sy=2) is a straight copy of the first -
         * one memcpy per span instead of per-pixel extra stores. */
        WrPix *dst = row + ds_x1 * wr_px_sx;
        wfixed_t xf = ds_xfrac, yf = ds_yfrac;
        int i;
        for (i = 0; i < npix; i++, dst += wr_px_sx, xf += ds_xstep, yf += ds_ystep) {
            WrPix p = ds_colormap[ds_source[(((yf >> WR_FRACBITS) & 63) << 6)
                                            | ((xf >> WR_FRACBITS) & 63)]];
            dst[0] = p;
            if (wr_px_sx > 1) dst[1] = p;
        }
        if (wr_px_sy > 1)
            memcpy(WR_ROW(s, ds_y * wr_px_sy + 1 + wr_origin_y)
                       + wr_origin_x + ds_x1 * wr_px_sx,
                   row + ds_x1 * wr_px_sx, (size_t) npix * wr_px_sx);
        return;
    }

#if defined(__DJGPP__) && defined(__i386__)
    /* Combined coordinate trick: pack xfrac and yfrac into a single
     * 32-bit value so that two shldl shifts extract the 64x64 flat index
     * in 3 instructions instead of 4 (two shifts + two ANDs).
     *
     *   position = (xfrac << 10) & 0xFFFF0000 | (yfrac >> 6) & 0x0000FFFF
     *   step     = (xstep  << 10) & 0xFFFF0000 | (ystep  >> 6) & 0x0000FFFF
     *
     * Per pixel (AT&T: shldl $cnt, src, dst = dst = (dst<<cnt)|(src>>(32-cnt))):
     *   shldl $22, position, ecx   ; pull xfrac integer bits into ecx
     *   shldl $6,  position, ecx   ; pull yfrac integer bits into ecx
     *   andl  $4095, ecx            ; ecx = (y&63)*64 + (x&63) = flat index
     *
     * The two shldl shifts total 28 bits, so whatever garbage was left in
     * ecx from the previous iteration only ever lands in bits 28-31 of the
     * result - always discarded by the "andl $4095" mask (keeps bits 0-11).
     * That means ecx can be reused as pure scratch every iteration with no
     * initialisation needed, which is what lets it double up: first as the
     * flat index for the source lookup, then (after movzbl) as the
     * colormap index for the remap, all without spilling to memory.
     *
     * Register allocation - 6 real GPRs, no ebp (ebp is the frame pointer
     * unless the whole TU is built with -fomit-frame-pointer, so it must
     * never be used as a bare scratch register in inline asm without an
     * explicit push/pop):
     *   eax = colormap (read-only base pointer)
     *   ebx = position (read-write, folded in place each iteration)
     *   ecx = scratch (flat index, then colormap index - never persists)
     *   edx = flat source (read-only base pointer)
     *   esi = step (read-only, added to position each iteration)
     *   edi = dest ptr (read-write, incremented each iteration)
     * The loop counter lives in a plain memory operand (decremented
     * in place) so
     * it never competes for a GPR. */
    {
        unsigned long position, step;
        {
            unsigned long xh = ((unsigned long)ds_xfrac << 10) & 0xFFFF0000UL;
            unsigned long yl = ((unsigned long)ds_yfrac >> 6) & 0x0000FFFFUL;
            position = xh | yl;
        }
        {
            unsigned long xh = ((unsigned long)ds_xstep << 10) & 0xFFFF0000UL;
            unsigned long yl = ((unsigned long)ds_ystep >> 6) & 0x0000FFFFUL;
            step = xh | yl;
        }

        unsigned long pos_rw = position;      /* seeds ebx, discarded after */
        unsigned char *dst_rw = row + ds_x1;  /* seeds edi, discarded after */
        int cnt = npix;
        __asm__ __volatile__(
            "1:                             \n\t"
            /* Extract flat index from position into ecx */
            "shldl  $22, %%ebx, %%ecx       \n\t"  /* fold xfrac bits in */
            "shldl  $6,  %%ebx, %%ecx       \n\t"  /* fold yfrac bits in */
            "andl   $4095, %%ecx             \n\t"  /* ecx = flat index */
            /* Sample flat, remap through colormap, write pixel. ecx is
             * reused as scratch: first the flat index (read via edx+ecx),
             * then the colormap index (read via eax+ecx). Neither eax nor
             * edx is modified, so both remain valid base pointers for
             * every iteration.
             *
             * As in wr_draw_column, the load zero-extends directly into
             * ecx: the address edx+ecx uses the OLD ecx before the result
             * lands in it, so this single movzx replaces a
             * movb+movzbl pair and saves 3 cycles per pixel. */
            "movzbl (%%edx,%%ecx), %%ecx     \n\t"  /* ecx = src[index] */
            "movb   (%%eax,%%ecx), %%cl      \n\t"  /* cl = cmap[texel] */
            "movb   %%cl, (%%edi)            \n\t"  /* *dest = cl */
            /* Advance position and dest */
            "addl   %%esi, %%ebx             \n\t"  /* position += step */
            "incl   %%edi                       \n\t"  /* dest++ */
            "decl   %[cnt]                    \n\t"  /* count-- (in memory) */
            "jnz    1b                        \n\t"
            : [cnt] "+m" (cnt), "+b" (pos_rw), "+D" (dst_rw)
            : "a" (ds_colormap), "d" (ds_source), "S" (step)
            : "ecx", "memory", "cc"
        );
    }
#elif defined(__mc68020) || defined(__mc68030) || defined(__mc68040) || \
      defined(__mc68060) || defined(__mc68020__) || defined(__mc68030__) || \
      defined(__mc68040__) || defined(__mc68060__)
    /* 68020+ version (bfextu): same packed position/step as the i386 loop
     *   position = (xfrac<<10 & 0xFFFF0000) | (yfrac>>6 & 0xFFFF)
     *   flat index = ((position>>10)&63)<<6 | (position>>26)
     * The lsr#26 needs no mask: position's top 6 bits are exactly the
     * x-integer part &63 (xfrac is masked at <<10, anything above bit 21
     * shifted out). dbra counts d7.w.
     *   d0/d1 = scratch, d2 = position, d3 = step, d7 = count-1,
     *   a1 = dest (post-increment), a2 = source, a3 = cmap */
    {
        unsigned long position, step;
        {
            unsigned long xh = ((unsigned long)ds_xfrac << 10) & 0xFFFF0000UL;
            unsigned long yl = ((unsigned long)ds_yfrac >> 6) & 0x0000FFFFUL;
            position = xh | yl;
        }
        {
            unsigned long xh = ((unsigned long)ds_xstep << 10) & 0xFFFF0000UL;
            unsigned long yl = ((unsigned long)ds_ystep >> 6) & 0x0000FFFFUL;
            step = xh | yl;
        }
        {
            register const unsigned char *asrc  __asm__("a2") = ds_source;
            register const WrPix           *acmap __asm__("a3") = ds_colormap;
            register WrPix                 *adst  __asm__("a1") = row + ds_x1;
            register unsigned long        apos  __asm__("d2") = position;
            register unsigned long        astp  __asm__("d3") = step;
            /* 2x unrolled (see wr_draw_column): dbra amortised, d1 only
             * needs moveq once since byte loads keep it 0-255. Odd tail
             * pixel handled in C below. WR_16BPP: u16 cmap + u16 store
             * (%a1@+ steps by 2 for movew). */
            int pairs = npix >> 1;
            if (pairs) {
                register int acnt __asm__("d7") = pairs - 1;
#ifdef WR_16BPP
                __asm__ __volatile__(
                    "       moveq  #0, %%d1\n\t"
                    "1:\n\t"
                    "bfextu %%d2{16:6}, %%d0\n\t"
                    "lsll   #6, %%d0\n\t"
                    "bfextu %%d2{0:6}, %%d1\n\t"
                    "orl    %%d1, %%d0\n\t"
                    "moveb  %%a2@(%%d0:l), %%d1\n\t"
                    "movew  %%a3@(0,%%d1:l:2), %%a1@+\n\t"
                    "addl   %%d3, %%d2\n\t"
                    "bfextu %%d2{16:6}, %%d0\n\t"
                    "lsll   #6, %%d0\n\t"
                    "bfextu %%d2{0:6}, %%d1\n\t"
                    "orl    %%d1, %%d0\n\t"
                    "moveb  %%a2@(%%d0:l), %%d1\n\t"
                    "movew  %%a3@(0,%%d1:l:2), %%a1@+\n\t"
                    "addl   %%d3, %%d2\n\t"
                    "dbra   %%d7, 1b\n\t"
                    : "+r"(apos), "+r"(adst), "+r"(acnt)
                    : "r"(asrc), "r"(acmap), "r"(astp)
                    : "d0", "d1", "cc", "memory");
#else
                __asm__ __volatile__(
                    "       moveq  #0, %%d1\n\t"
                    "1:\n\t"
                    "bfextu %%d2{16:6}, %%d0\n\t" /* y-int &63 (pos bits 10-15) */
                    "lsll   #6, %%d0\n\t"
                    "bfextu %%d2{0:6}, %%d1\n\t"  /* x-int &63 (pos bits 26-31) */
                    "orl    %%d1, %%d0\n\t"
                    "moveb  %%a2@(%%d0:l), %%d1\n\t"
                    "moveb  %%a3@(%%d1:l), %%a1@+\n\t"
                    "addl   %%d3, %%d2\n\t"
                    "bfextu %%d2{16:6}, %%d0\n\t"
                    "lsll   #6, %%d0\n\t"
                    "bfextu %%d2{0:6}, %%d1\n\t"
                    "orl    %%d1, %%d0\n\t"
                    "moveb  %%a2@(%%d0:l), %%d1\n\t"
                    "moveb  %%a3@(%%d1:l), %%a1@+\n\t"
                    "addl   %%d3, %%d2\n\t"
                    "dbra   %%d7, 1b\n\t"
                    : "+r"(apos), "+r"(adst), "+r"(acnt)
                    : "r"(asrc), "r"(acmap), "r"(astp)
                    : "d0", "d1", "cc", "memory");
#endif
            }
            if (npix & 1) {
                *adst = acmap[asrc[((int)((apos >> 10) & 63) << 6) | (int)(apos >> 26)]];
            }
        }
    }
#else
    /* C fallback: 2x unrolled loop. */
    {
        wfixed_t xfrac = ds_xfrac, yfrac = ds_yfrac;
        int x = ds_x1;
        int count = npix - 1;
        while (count >= 1) {
            int tx1 = (xfrac >> WR_FRACBITS) & 63;
            int ty1 = (yfrac >> WR_FRACBITS) & 63;
            xfrac += ds_xstep;
            yfrac += ds_ystep;
            int tx2 = (xfrac >> WR_FRACBITS) & 63;
            int ty2 = (yfrac >> WR_FRACBITS) & 63;
            row[x]     = ds_colormap[ds_source[(ty1 << 6) + tx1]];
            row[x + 1] = ds_colormap[ds_source[(ty2 << 6) + tx2]];
            xfrac += ds_xstep;
            yfrac += ds_ystep;
            x += 2;
            count -= 2;
        }
        if (count == 0) {
            int tx = (xfrac >> WR_FRACBITS) & 63;
            int ty = (yfrac >> WR_FRACBITS) & 63;
            row[x] = ds_colormap[ds_source[(ty << 6) + tx]];
        }
    }
#endif
}

/* @wr_flatshade fast path for floor/ceiling spans (see OP_WR_FLATSHADE in
 * tigryx.h). A span is a contiguous run of screen columns on the same
 * row, so a solid-colour fill can go through a genuine memset()/rep
 * stosb - a categorically different primitive from wr_draw_span's
 * per-pixel table lookups, since it never does a dependent memory read
 * per byte. This is the single biggest win flatshade offers: floor/
 * ceiling rendering was measured at ~46% of total frame time on real
 * 486SX hardware, almost entirely from those per-pixel lookups. */
static void wr_fill_span(WrHostState *s, int ds_y, int ds_x1, int ds_x2, WrPix color) {
    if (!WR_HAS_BUF(s)) return;
    if (ds_y < 0 || ds_y >= wr_screen_h) return;
    if (ds_x1 < 0) ds_x1 = 0;
    if (ds_x2 >= wr_screen_w) ds_x2 = wr_screen_w - 1;
    if (ds_x1 > ds_x2) return;

    wrp_plane_rows++;
    wrp_plane_px += (unsigned int) (ds_x2 - ds_x1 + 1);
    if (wr_skip_mode == 5) return;   /* TEMP PROFILING: WRSKIP=p - no pixels */

    WrPix *row = WR_ROW(s, ds_y * wr_px_sy + wr_origin_y) + wr_origin_x;
    if (wr_px_sx > 1 || wr_px_sy > 1) {
        int rr;
        for (rr = 0; rr < wr_px_sy; rr++)
            memset(WR_ROW(s, ds_y * wr_px_sy + rr + wr_origin_y)
                       + wr_origin_x + ds_x1 * wr_px_sx,
                   color, (size_t) (ds_x2 - ds_x1 + 1) * wr_px_sx);
        return;
    }
#ifdef WR_16BPP
    {
        /* Two u16 pixels per u32 store - the fill rate that made memset
         * attractive in 8bpp. */
        u32 pair = ((u32) color << 16) | (u32) color;
        u32 *p32 = (u32 *) (row + ds_x1);
        int n = ds_x2 - ds_x1 + 1;
        int pairs = n >> 1;
        while (pairs--) *p32++ = pair;
        if (n & 1) *(WrPix *) p32 = color;
    }
#else
    memset(row + ds_x1, color, (size_t) (ds_x2 - ds_x1 + 1));
#endif
}

/* @wr_flatshade fast path for wall columns. Unlike a span, a column is
 * vertically strided (each pixel is `pitch` bytes apart in the
 * framebuffer), so it can't use memset - but it still drops from two
 * dependent memory reads per pixel (texture sample, then colormap
 * remap) down to a single store, which is still a real win on a
 * cacheless 486 where each of those reads was a full bus cycle. */
static void wr_fill_column(WrHostState *s, int dc_x, int dc_yl, int dc_yh, WrPix color) {
    if (!WR_HAS_BUF(s)) return;
    if (dc_x < 0 || dc_x >= wr_screen_w) return;
    if (dc_yl < 0) dc_yl = 0;
    if (dc_yh >= wr_screen_h) dc_yh = wr_screen_h - 1;
    if (dc_yl > dc_yh) return;

    wrp_wall_cols++;
    wrp_wall_px += (unsigned int) (dc_yh - dc_yl + 1);

    WrPix *dst = WR_ROW(s, dc_yl * wr_px_sy + wr_origin_y) + (dc_x * wr_px_sx + wr_origin_x);
    int pitch = WR_PITCH(s, dc_yl * wr_px_sy + wr_origin_y);
    if (wr_px_sx > 1 || wr_px_sy > 1) {
        int y;
        for (y = dc_yl; y <= dc_yh; y++, dst += pitch * wr_px_sy) {
            dst[0] = color;
            if (wr_px_sx > 1) dst[1] = color;
            if (wr_px_sy > 1) {
                dst[pitch] = color;
                if (wr_px_sx > 1) dst[pitch + 1] = color;
            }
        }
        return;
    }
#if defined(__m68k__) || defined(__M68000__)
    {
        register WrPix           *adst  __asm__("a1") = dst;
        register int            apit  __asm__("d4") = pitch * (int)sizeof(WrPix);
        register int            acnt  __asm__("d7") = dc_yh - dc_yl;
        register int            acol  __asm__("d0") = color;
#ifdef WR_16BPP
        __asm__ __volatile__(
            "1:\n\t"
            "movew  %%d0, %%a1@\n\t"
            "addl   %%d4, %%a1\n\t"
            "dbra   %%d7, 1b\n\t"
            : "+r"(adst), "+r"(acnt)
            : "r"(apit), "r"(acol)
            : "cc", "memory");
#else
        __asm__ __volatile__(
            "1:\n\t"
            "moveb  %%d0, %%a1@\n\t"
            "addl   %%d4, %%a1\n\t"
            "dbra   %%d7, 1b\n\t"
            : "+r"(adst), "+r"(acnt)
            : "r"(apit), "r"(acol)
            : "cc", "memory");
#endif
    }
#else
    { int y; for (y = dc_yl; y <= dc_yh; y++, dst += pitch) *dst = color; }
#endif
}

/* @wr_palette(playpal_rgb888, colormap, num_colormaps) - see OP_WR_PALETTE
 * in tigryx.h. Unlike @palette_upload (used by the generic @mesh path),
 * this never builds an RGB->index lookup table: sector-based rendering
 * samples palette indices directly out of texture/flat data, so there is
 * no RGB to match against a palette at draw time at all. */
static void wr_op_palette(RelicVM *vm) {
    WrHostState *s = (WrHostState *) vm->backend_data;
    if (!s) return;
    u32 pal_ptr = (u32) vm->r[1];
    u32 cm_ptr  = (u32) vm->r[2];
    i32 num_cm  = vm->r[3];
    if (num_cm <= 0 || num_cm > 256) return;
    if ((uint64_t) pal_ptr + 768u > (uint64_t) vm->mem_sz) return;
    if ((uint64_t) cm_ptr + (uint64_t) num_cm * 256u > (uint64_t) vm->mem_sz) return;

    memcpy(s->wr_playpal, vm->mem + pal_ptr, 768);

    if (s->wr_colormap) { WR_FREE(s->wr_colormap); s->wr_colormap = NULL; }
#ifdef WR_16BPP
    s->wr_colormap = (unsigned char *) WR_ALLOC((size_t) num_cm * 256u * sizeof(WrPix));
    if (!s->wr_colormap) return;
    /* Build the palette LUT first, then fold the guest's 8bpp colormap
     * rows into u16 display values: cmap16[texel] = lut[cmap8[texel]].
     * Every cmap[...] lookup below then yields a ready-to-store pixel. */
    wr_host_apply_palette(s);
    {
        WrPix *cm16 = (WrPix *) s->wr_colormap;
        const unsigned char *cm8 = vm->mem + cm_ptr;
        int r, i;
        for (r = 0; r < num_cm; r++)
            for (i = 0; i < 256; i++)
                cm16[(size_t) r * 256u + i] = s->wr_rgb565_lut[cm8[(size_t) r * 256u + i]];
    }
    s->wr_num_colormaps = num_cm;
#else
    s->wr_colormap = (unsigned char *) WR_ALLOC((size_t) num_cm * 256u);
    if (!s->wr_colormap) return;
    memcpy(s->wr_colormap, vm->mem + cm_ptr, (size_t) num_cm * 256u);
    s->wr_num_colormaps = num_cm;
#endif

    /* Backend-specific: upload to a real 8bpp hardware palette (DOS), or
     * rebuild an RGBA lookup table (modern backends). Non-8bpp/paletted
     * targets have no palette to set; wr_draw_column/wr_draw_span still
     * work (they write a raw byte value from the colormap), but the byte
     * they write is only meaningful as a palette index, so this renderer
     * is 8bpp-only in practice - wr_host_apply_palette is what turns
     * those indices into visible colours on each backend. */
#ifndef WR_16BPP
    wr_host_apply_palette(s);   /* 16bpp already ran it above cmap16 fill */
#endif

    s->wr_palette_set = true;
}

/* ---- @wr_flatshade support: cached representative colour per texture/
 * flat (see OP_WR_FLATSHADE in tigryx.h). ---- */

/* Nearest-match search over the 256-entry PLAYPAL for a given averaged
 * RGB triple. Only ever runs a handful of times per level (once per
 * distinct texture/flat actually used), so a linear scan is fine - no
 * need for a k-d tree or similar. */
static unsigned char wr_nearest_palette_color(const WrHostState *s, int r, int g, int b) {
    int best_idx = 0;
    long best_dist = 0x7FFFFFFFL;
    int i;
    for (i = 0; i < 256; i++) {
        int dr = r - s->wr_playpal[i * 3 + 0];
        int dg = g - s->wr_playpal[i * 3 + 1];
        int db = b - s->wr_playpal[i * 3 + 2];
        long dist = (long) dr * dr + (long) dg * dg + (long) db * db;
        if (dist < best_dist) { best_dist = dist; best_idx = i; }
    }
    return (unsigned char) best_idx;
}

/* Averages `count` palette-indexed texels (already looked up through
 * PLAYPAL into RGB space - palette indices themselves aren't a linear
 * brightness ramp, so averaging the *indices* directly would give a
 * meaningless result) and returns the nearest single palette entry to
 * that average. */
static unsigned char wr_avg_color_from_row(const WrHostState *s, const unsigned char *row, int count) {
    long sum_r = 0, sum_g = 0, sum_b = 0;
    int i;
    for (i = 0; i < count; i++) {
        unsigned char idx = row[i];
        sum_r += s->wr_playpal[idx * 3 + 0];
        sum_g += s->wr_playpal[idx * 3 + 1];
        sum_b += s->wr_playpal[idx * 3 + 2];
    }
    int avg_r = count > 0 ? (int) (sum_r / count) : 0;
    int avg_g = count > 0 ? (int) (sum_g / count) : 0;
    int avg_b = count > 0 ? (int) (sum_b / count) : 0;
    return wr_nearest_palette_color(s, avg_r, avg_g, avg_b);
}

/* Representative colour for a wall texture, cached on first use. Samples
 * only the texture's middle row rather than the whole width*height area:
 * WAD wall textures are almost always vertically-repetitive band
 * patterns (bricks, panels), so one row is a good proxy for the whole
 * texture at a small fraction of the cost - O(w) instead of O(w*h), which
 * matters when a level references a hundred-plus textures. */
static unsigned char wr_texture_avg_color(WrHostState *s, int tex_id) {
    if (tex_id <= 0 || tex_id >= WR_MAX_TEXTURES) return 0;
    if (s->wr_tex_avg_valid[tex_id]) return s->wr_tex_avg_color[tex_id];

    unsigned char result = 0;
    const unsigned char *data = s->wr_textures[tex_id].data;
    int w = s->wr_textures[tex_id].w, h = s->wr_textures[tex_id].h;
    if (data && w > 0 && h > 0) {
        /* Wall textures are stored column-major (data[col*h+row]), so the
         * middle row isn't contiguous in memory - gather it into a small
         * scratch buffer first. Capped at 1024 columns so a pathological
         * PWAD texture can't blow the stack; sampling the first 1024
         * columns of a wider texture is still representative. */
        unsigned char row[1024];
        int n = w; if (n > 1024) n = 1024;
        int midrow = h >> 1;
        int col;
        for (col = 0; col < n; col++) row[col] = data[(size_t) col * h + midrow];
        result = wr_avg_color_from_row(s, row, n);
    }
    s->wr_tex_avg_color[tex_id] = result;
    s->wr_tex_avg_valid[tex_id] = true;
    return result;
}

/* Representative colour for a flat, cached on first use. Flats are
 * always exactly 64x64 and stored row-major, so the middle row (row 32)
 * is already contiguous - no gather needed. */
static unsigned char wr_flat_avg_color(WrHostState *s, int flat_id) {
    if (flat_id <= 0 || flat_id >= WR_MAX_FLATS) return 0;
    if (s->wr_flat_avg_valid[flat_id]) return s->wr_flat_avg_color[flat_id];

    unsigned char result = 0;
    const unsigned char *data = s->wr_flats[flat_id];
    if (data) {
        result = wr_avg_color_from_row(s, data + 32 * 64, 64);
    }
    s->wr_flat_avg_color[flat_id] = result;
    s->wr_flat_avg_valid[flat_id] = true;
    return result;
}

/* Stripe-shade cache: one representative colour per texture COLUMN, per
 * texture, built lazily on first striped draw of that texture. Wall
 * textures are column-major (data[col*h + row]), so column c is the
 * contiguous run data[c*h .. c*h+h) and the whole table costs a single
 * pass over the texture. Invalidated alongside wr_tex_avg_valid in
 * wr_op_texture. */
static unsigned char *wr_colavg[WR_MAX_TEXTURES];

static unsigned char wr_texture_col_avg(WrHostState *s, int tex_id, int col) {
    if (tex_id <= 0 || tex_id >= WR_MAX_TEXTURES) return 0;
    const unsigned char *data = s->wr_textures[tex_id].data;
    int w = s->wr_textures[tex_id].w, h = s->wr_textures[tex_id].h;
    if (!data || w <= 0 || h <= 0) return 0;
    if (!wr_colavg[tex_id]) {
        unsigned char *tab = (unsigned char *) WR_ALLOC((size_t) w);
        int c;
        if (!tab) return wr_texture_avg_color(s, tex_id);
        for (c = 0; c < w; c++)
            tab[c] = wr_avg_color_from_row(s, data + (size_t) c * h, h);
        wr_colavg[tex_id] = tab;
    }
    if (col < 0) col = 0;
    if (col >= w) col = w - 1;
    return wr_colavg[tex_id][col];
}

/* @wr_flatshade(enable) - see OP_WR_FLATSHADE in tigryx.h. Just flips the
 * mode flag; the colour cache is filled in lazily (see the two helpers
 * above) the first time each texture/flat is actually drawn, not here -
 * scanning every uploaded texture up front would waste time on textures
 * the current level never uses. */
static void wr_op_flatshade(RelicVM *vm) {
    WrHostState *s = (WrHostState *) vm->backend_data;
    if (!s) return;
    /* r1 packs flags: bit0 = flatshade enable; bit1 = stripe-shade
     * enable (per-column-average wall fill; ignored while bit0 is also
     * set - full flatshade wins); bits 8-9 = pixel scale
     * mode (0 = full res, 1 = half-width "low detail", 2 = quarter-res
     * 2x2 blocks). Guests compiled before the field existed pass 0/1,
     * which decodes to full res. */
    {
        int mode = ((int) vm->r[1] >> 8) & 3;
        wr_px_sx = (mode >= 1) ? 2 : 1;
        wr_px_sy = (mode >= 2) ? 2 : 1;
    }
    s->wr_flatshade = (vm->r[1] & 1) != 0;
    wr_stripshade = (vm->r[1] & 2) != 0 && !s->wr_flatshade;
    s->wr_area_x = (int) vm->r[2];
    s->wr_area_y = (int) vm->r[3];
    s->wr_area_w = (int) vm->r[4];
    s->wr_area_h = (int) vm->r[5];
}

/* @wr_patch(x, y, dst_w, dst_h, src_w, src_h, pal_data, light_level) -
 * see OP_WR_PATCH. Draws a palette-indexed 2D patch directly to the
 * backbuffer, stretched to (dst_w,dst_h) via
 * fixed-point nearest-neighbour sampling (one division per axis to compute
 * the 16.16 step, then pure addition per pixel - no per-pixel divide).
 * Index 0 = transparent (skipped). light_level selects a colormap row for
 * distance darkening (0 = full bright for HUD/weapon). Uses direct row
 * access + pitch stepping, same as wr_draw_column. */
static void wr_op_patch(RelicVM *vm) {
    WrHostState *s = (WrHostState *) vm->backend_data;
    if (!s || !WR_HAS_BUF(s) || !s->wr_colormap) return;

    int x = (int) vm->r[1];
    int y = (int) vm->r[2];
    int dst_w = (int) vm->r[3];
    int dst_h = (int) vm->r[4];
    int src_w = (int) vm->r[5];
    int src_h = (int) vm->r[6];
    u32 data_ptr = (u32) vm->r[7];
    int light = (int) vm->r[8];

    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;
    if (src_w > 512 || src_h > 512) return;
    if ((uint64_t) data_ptr + (uint64_t) src_w * (uint64_t) src_h > (uint64_t) vm->mem_sz) return;

    if (light < 0) light = 0;
    if (light >= s->wr_num_colormaps) light = s->wr_num_colormaps - 1;
    const WrPix *cmap = (const WrPix *)s->wr_colormap + (size_t) light * 256u;
    const unsigned char *src = vm->mem + data_ptr;

    /* 16.16 fixed-point source-per-dest step (one divide per axis, not
     * per pixel - same trick as dc_iscale in wr_draw_column). */
    int32_t xstep = (int32_t) wr_div64(((int64_t) src_w << 16), dst_w);
    int32_t ystep = (int32_t) wr_div64(((int64_t) src_h << 16), dst_h);

    /* Clip destination rect to backbuffer bounds, adjusting the source
     * fixed-point start accordingly so clipped columns/rows still sample
     * the correct source texel. */
    int bw = WR_BUF_W(s), bh = WR_BUF_H(s);
    int32_t xfrac0 = 0, yfrac0 = 0;
    if (x < 0) { xfrac0 = (int32_t) (-x) * xstep; dst_w += x; x = 0; }
    if (y < 0) { yfrac0 = (int32_t) (-y) * ystep; dst_h += y; y = 0; }
    if (x + dst_w > bw) dst_w = bw - x;
    if (y + dst_h > bh) dst_h = bh - y;
    if (dst_w <= 0 || dst_h <= 0) return;

    WrPix *dst_row = WR_ROW(s, y);
    int pitch = WR_PITCH(s, y);
    int32_t yfrac = yfrac0;
    int py;
    for (py = 0; py < dst_h; py++) {
        int sy = yfrac >> 16;
        if (sy >= src_h) sy = src_h - 1;
        const unsigned char *srow = src + (size_t) sy * (size_t) src_w;
        WrPix *dx = dst_row + x;
        int32_t xfrac = xfrac0;
        int px;
        for (px = 0; px < dst_w; px++) {
            int sx = xfrac >> 16;
            if (sx >= src_w) sx = src_w - 1;
            unsigned char idx = srow[sx];
            if (idx != 0) dx[px] = cmap[idx];
            xfrac += xstep;
        }
        yfrac += ystep;
        dst_row += pitch;
    }
    wr_host_present(s, x, y, dst_w, dst_h);
    s->frame_dirty = true;
}

/* @wr_texture(id, width, height, column_major_data) - see OP_WR_TEXTURE. */
static void wr_op_texture(RelicVM *vm) {
    WrHostState *s = (WrHostState *) vm->backend_data;
    if (!s) return;
    i32 id = vm->r[1];
    i32 w  = vm->r[2];
    i32 h  = vm->r[3];
    u32 data_ptr = (u32) vm->r[4];
    if (id <= 0 || id >= WR_MAX_TEXTURES) return;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    uint64_t need = (uint64_t) w * (uint64_t) h;
    if ((uint64_t) data_ptr + need > (uint64_t) vm->mem_sz) return;

    if (s->wr_textures[id].data) { WR_FREE(s->wr_textures[id].data); s->wr_textures[id].data = NULL; }
    unsigned char *buf = (unsigned char *) WR_ALLOC((size_t) need);
    if (!buf) return;
    memcpy(buf, vm->mem + data_ptr, (size_t) need);
    s->wr_textures[id].data = buf;
    s->wr_textures[id].w = w;
    s->wr_textures[id].h = h;
    s->wr_tex_avg_valid[id] = false;  /* stale cache: data just changed */
    if (wr_colavg[id]) { WR_FREE(wr_colavg[id]); wr_colavg[id] = NULL; }
}

/* @wr_flat(id, data_64x64) - see OP_WR_FLAT. */
static void wr_op_flat(RelicVM *vm) {
    WrHostState *s = (WrHostState *) vm->backend_data;
    if (!s) return;
    i32 id = vm->r[1];
    u32 data_ptr = (u32) vm->r[2];
    if (id <= 0 || id >= WR_MAX_FLATS) return;
    if ((uint64_t) data_ptr + 4096u > (uint64_t) vm->mem_sz) return;

    if (s->wr_flats[id]) { WR_FREE(s->wr_flats[id]); s->wr_flats[id] = NULL; }
    unsigned char *buf = (unsigned char *) WR_ALLOC(4096);
    if (!buf) return;
    memcpy(buf, vm->mem + data_ptr, 4096);
    s->wr_flats[id] = buf;
    s->wr_flat_avg_valid[id] = false;  /* stale cache: data just changed */
}

/* @wr_test(tex_id, flat_id, light_level) - see OP_WR_TEST. Diagnostic
 * only: no map/BSP is involved. Clears to palette index 0 (the game's PLAYPAL
 * always has black there), draws a strip of wall-texture columns across
 * the top half of the screen and a tiled flat span across the bottom
 * half, both lit through colormap[light_level]. Proves the two pixel
 * loops and the palette/colormap upload are correct before step 2 (BSP
 * walk + solid-segment clipping) puts real geometry behind them. */
static void wr_op_test(RelicVM *vm) {
    WrHostState *s = (WrHostState *) vm->backend_data;
    if (!s || !WR_HAS_BUF(s) || !s->wr_palette_set || !s->wr_colormap) return;

    i32 tex_id  = vm->r[1];
    i32 flat_id = vm->r[2];
    i32 light   = vm->r[3];
    if (light < 0) light = 0;
    if (light >= s->wr_num_colormaps) light = s->wr_num_colormaps - 1;
    const WrPix *cmap = (const WrPix *)s->wr_colormap + (size_t) light * 256u;

    /* wr_draw_column/wr_draw_span clip against wr_screen_w/h, which are
     * otherwise only set by wr_op_render - a program that calls @wr_test
     * before its first @wr_render (or never renders a map at all) would
     * clip every draw against 0 and present a black screen. */
    wr_screen_w = WR_BUF_W(s);
    wr_screen_h = WR_BUF_H(s);
    wr_origin_x = 0;
    wr_origin_y = 0;

    int W = WR_BUF_W(s), H = WR_BUF_H(s);
    wr_host_clear_backbuf(s, 0, 0, W, H);
    int half = H / 2;

    if (tex_id > 0 && tex_id < WR_MAX_TEXTURES && s->wr_textures[tex_id].data) {
        int tw = s->wr_textures[tex_id].w, th = s->wr_textures[tex_id].h;
        /* dc_iscale: texture rows per screen row, such that the whole
         * texture height spans the top half of the screen once.
         * dc_texturemid must cancel wr_draw_column's own
         * (dc_yl - backbuf->h/2) * dc_iscale term, since we want screen
         * row 0 (dc_yl passed below) to sample texture row 0 exactly. */
        wfixed_t dc_iscale = half > 0 ? wr_div64(((int64_t) th << WR_FRACBITS), half) : 0;
        wfixed_t dc_texturemid = (H >> 1) * dc_iscale;
        int col;
        for (col = 0; col < W; col++) {
            int texcol = (col * tw) / (W > 0 ? W : 1);
            if (texcol >= tw) texcol = tw - 1;
            const unsigned char *source = s->wr_textures[tex_id].data + (size_t) texcol * th;
            wr_draw_column(s, col, 0, half - 1, dc_iscale, dc_texturemid, source, th, cmap);
        }
    }

    if (flat_id > 0 && flat_id < WR_MAX_FLATS && s->wr_flats[flat_id]) {
        const unsigned char *source = s->wr_flats[flat_id];
        int y;
        for (y = half; y < H; y++) {
            wfixed_t xstep = WR_FRACUNIT;                     /* 1 texel/column, tiles horizontally */
            wfixed_t yfrac = (wfixed_t) (y - half) * WR_FRACUNIT; /* 1 texel/row, tiles vertically */
            wr_draw_span(s, y, 0, W - 1, 0, yfrac, xstep, 0, source, cmap);
        }
    }

    wr_host_present(s, 0, 0, W, H);
    s->frame_dirty = true;
}

/* ---- @wr_blockmap / @wr_blocked state -------------------------------
 * Native BLOCKMAP collision probe: the guest uploads the raw BLOCKMAP lump
 * once per level (@wr_blockmap); @wr_blocked walks the cells under the
 * probe circle and tests each candidate linedef natively. File-static,
 * same pattern as wr_visplanes: valid only while the matching @wr_map
 * geometry is resident (wr_op_map frees it). Full ops below. */
static unsigned char *wr_blkmap = NULL;
static int  wr_blkmap_size = 0;
static int  wr_blk_w = 0, wr_blk_h = 0, wr_blk_xorg = 0, wr_blk_yorg = 0;
static int *wr_blk_stamp = NULL;   /* per-linedef dedupe stamps */
static int  wr_blk_stamp_n = 0;    /* array size (lines at upload time) */
static int  wr_blk_stamp_no = 0;   /* current stamp generation */

/* @wr_map(header) - see OP_WR_MAP. header points at 7 x {u32 vm-offset,
 * i32 count} in the order vertices, linedefs, sidedefs, sectors, segs,
 * subsectors, nodes (tigryx_rc_shim.c's r_wr_map already copied every
 * array into VM memory and rewrote the offsets, so this just has to copy
 * them once more into backend-owned storage). */
static void wr_op_map(RelicVM *vm) {
    WrHostState *s = (WrHostState *) vm->backend_data;
    if (!s) return;
    u32 header_ptr = (u32) vm->r[1];
    if ((uint64_t) header_ptr + 56u > (uint64_t) vm->mem_sz) {
        return;
    }

    u32 rec[14];
    memcpy(rec, vm->mem + header_ptr, 56);

    if (s->wr_vertices)   { WR_FREE(s->wr_vertices);   s->wr_vertices = NULL;   s->wr_n_vertices = 0; }
    if (s->wr_linedefs)   { WR_FREE(s->wr_linedefs);   s->wr_linedefs = NULL;   s->wr_n_linedefs = 0; }
    if (s->wr_sidedefs)   { WR_FREE(s->wr_sidedefs);   s->wr_sidedefs = NULL;   s->wr_n_sidedefs = 0; }
    if (s->wr_sectors)    { WR_FREE(s->wr_sectors);    s->wr_sectors = NULL;    s->wr_n_sectors = 0; }
    if (s->wr_segs)       { WR_FREE(s->wr_segs);       s->wr_segs = NULL;       s->wr_n_segs = 0; }
    if (s->wr_subsectors) { WR_FREE(s->wr_subsectors); s->wr_subsectors = NULL; s->wr_n_subsectors = 0; }
    if (s->wr_nodes)      { WR_FREE(s->wr_nodes);      s->wr_nodes = NULL;      s->wr_n_nodes = 0; }
    s->wr_map_set = false;

#define WR_COPY_ARRAY(FIELD, CNTFIELD, TYPE, IDX) \
    do { \
        u32 off = rec[(IDX) * 2], cnt = rec[(IDX) * 2 + 1]; \
        if (cnt > 0 && (uint64_t) off + (uint64_t) cnt * sizeof(TYPE) <= (uint64_t) vm->mem_sz) { \
            s->FIELD = (TYPE *) WR_ALLOC((size_t) cnt * sizeof(TYPE)); \
            if (s->FIELD) { \
                memcpy(s->FIELD, vm->mem + off, (size_t) cnt * sizeof(TYPE)); \
                s->CNTFIELD = (int) cnt; \
            } \
        } \
    } while (0)

    WR_COPY_ARRAY(wr_vertices,   wr_n_vertices,   WrVertex,   0);
    WR_COPY_ARRAY(wr_linedefs,   wr_n_linedefs,   WrLinedef,  1);
    WR_COPY_ARRAY(wr_sidedefs,   wr_n_sidedefs,   WrSidedef,  2);
    WR_COPY_ARRAY(wr_sectors,    wr_n_sectors,    WrSector,   3);
    WR_COPY_ARRAY(wr_segs,       wr_n_segs,       WrSeg,      4);
    WR_COPY_ARRAY(wr_subsectors, wr_n_subsectors, WrSubsector, 5);
    WR_COPY_ARRAY(wr_nodes,      wr_n_nodes,      WrNode,     6);
#undef WR_COPY_ARRAY

    s->wr_map_set = (s->wr_vertices && s->wr_linedefs && s->wr_sidedefs &&
                      s->wr_sectors && s->wr_segs && s->wr_subsectors);
    /* wr_nodes may legitimately be NULL/empty for a trivial one-subsector
     * map; everything else is required. */

    /* A new map's linedef array makes any uploaded BLOCKMAP dedupe table
     * stale (line indices may now mean different geometry). Drop both so
     * @wr_blocked reports -1 until @wr_blockmap re-uploads. */
    if (wr_blkmap) { WR_FREE(wr_blkmap); wr_blkmap = NULL; }
    wr_blk_w = wr_blk_h = 0;
    if (wr_blk_stamp) { WR_FREE(wr_blk_stamp); wr_blk_stamp = NULL; }
    wr_blk_stamp_n = 0;
    /* Backend-specific fixup after a map upload (e.g. DOS's planar
     * page-flip backbuffer -> RAM swap, since the classic renderer writes
     * raw linear bytes and can't target VGA plane memory). No-op on
     * backends that always render into a plain linear surface. */
    wr_host_on_map_uploaded(s);

#ifdef WR_DSP_COLS
    /* Queue the packed map tables for the DSP geometry walk. Failure is
     * harmless: wr_dsp_walk_frame falls back to the CPU walk. */
    {
        WrDspMap m;
        m.verts = s->wr_vertices;     m.n_verts = s->wr_n_vertices;
        m.ldefs = s->wr_linedefs;     m.n_ldefs = s->wr_n_linedefs;
        m.sdefs = s->wr_sidedefs;     m.n_sdefs = s->wr_n_sidedefs;
        m.secs  = s->wr_sectors;      m.n_secs  = s->wr_n_sectors;
        m.segs  = s->wr_segs;         m.n_segs  = s->wr_n_segs;
        m.ssecs = s->wr_subsectors;   m.n_ssecs = s->wr_n_subsectors;
        m.nodes = s->wr_nodes;        m.n_nodes = s->wr_n_nodes;
        wr_dsp_map(&m);
    }
#endif
}

/* @wr_blockmap(data_ptr, size, w, h, xorg, yorg): copy the raw BLOCKMAP
 * lump into backend storage. The guest's position_valid_r() does the
 * same cell-walk + linedef probe in VM ops (~150 ops per call on the
 * m68k AOT target made this the largest tick cost); @wr_blocked does it
 * in C instead. */
static void wr_op_blockmap(RelicVM *vm) {
    WrHostState *s = (WrHostState *) vm->backend_data;
    u32 ptr  = (u32) vm->r[1];
    int size = (int) vm->r[2];
    int w    = (int) vm->r[3];
    int h    = (int) vm->r[4];
    int xorg = (int) vm->r[5];
    int yorg = (int) vm->r[6];
    (void) s;

    if (wr_blkmap) { WR_FREE(wr_blkmap); wr_blkmap = NULL; }
    wr_blkmap_size = wr_blk_w = wr_blk_h = 0;
    if (wr_blk_stamp) { WR_FREE(wr_blk_stamp); wr_blk_stamp = NULL; }
    wr_blk_stamp_n = 0;

    if (!ptr || size < 8 || (uint64_t) ptr + (uint64_t) size > (uint64_t) vm->mem_sz)
        return;
    if (w <= 0 || h <= 0 || (int64_t) 8 + (int64_t) w * h * 2 > size)
        return;

    wr_blkmap = (unsigned char *) WR_ALLOC((size_t) size);
    if (!wr_blkmap) return;
    memcpy(wr_blkmap, vm->mem + ptr, (size_t) size);
    wr_blkmap_size = size;
    wr_blk_w = w; wr_blk_h = h; wr_blk_xorg = xorg; wr_blk_yorg = yorg;
}

/* Per-line test, identical to the guest's pv_ld_blocked(): bbox reject,
 * point-segment distance in map units (i64 keeps cross products in range
 * without the guest's /8 scaling), then the classic blocking rules. */
static int wr_blk_line_blocked(WrHostState *s, int li,
                               int px, int pz, int rm, int from_sector) {
    const WrLinedef *ld = &s->wr_linedefs[li];
    int v1 = (unsigned short) ld->v1, v2 = (unsigned short) ld->v2;
    if (v1 >= s->wr_n_vertices || v2 >= s->wr_n_vertices) return 0;
    int x1 = s->wr_vertices[v1].x;
    int z1 = s->wr_vertices[v1].y;
    int x2 = s->wr_vertices[v2].x;
    int z2 = s->wr_vertices[v2].y;

    int lx = x1 < x2 ? x1 : x2, hx = x1 > x2 ? x1 : x2;
    int lz = z1 < z2 ? z1 : z2, hz = z1 > z2 ? z1 : z2;
    if (px < lx - rm || px > hx + rm || pz < lz - rm || pz > hz + rm)
        return 0;

    /* distance from (px,pz) to segment: endpoint/outside tests then the
     * interior |cross| >= rm*len, as cross^2 >= rm^2*len2 (len2 > 0) */
    i64 dx = (i64) x2 - x1, dz = (i64) z2 - z1;
    i64 ex = (i64) px - x1, ez = (i64) pz - z1;
    i64 tn = ex * dx + ez * dz;
    i64 len2 = dx * dx + dz * dz;
    if (tn <= 0) {
        if (ex * ex + ez * ez >= (i64) rm * rm) return 0;
    } else if (tn >= len2) {
        ex -= dx; ez -= dz;
        if (ex * ex + ez * ez >= (i64) rm * rm) return 0;
    } else {
        i64 cross = ez * dx - ex * dz;
        if (cross * cross >= (i64) rm * rm * len2) return 0;
    }

    if (ld->back < 0) return 1;                      /* one-sided wall */
    int front_sd = (unsigned short) ld->front;
    int back_sd  = (unsigned short) ld->back;
    if (front_sd >= s->wr_n_sidedefs || back_sd >= s->wr_n_sidedefs)
        return 0;
    int front_sec = (unsigned short) s->wr_sidedefs[front_sd].sector;
    int back_sec  = (unsigned short) s->wr_sidedefs[back_sd].sector;
    if (front_sec >= s->wr_n_sectors || back_sec >= s->wr_n_sectors)
        return 0;
    int front_floor = s->wr_sectors[front_sec].floor_h;
    int back_floor  = s->wr_sectors[back_sec].floor_h;
    int from_floor = front_floor, to_floor = back_floor;
    if (from_sector == back_sec) {
        from_floor = back_floor; to_floor = front_floor;
    }
    if (to_floor - from_floor > 24) return 1;        /* step too high */
    if (ld->flags & 1) return 1;                     /* impassable */
    int open_top = s->wr_sectors[front_sec].ceil_h;
    int back_ceil = s->wr_sectors[back_sec].ceil_h;
    if (back_ceil < open_top) open_top = back_ceil;
    int open_bot = front_floor;
    if (back_floor > open_bot) open_bot = back_floor;
    return (open_top - open_bot) < 56 ? 1 : 0;       /* opening too low */
}

static void wr_op_blocked(RelicVM *vm) {
    WrHostState *s = (WrHostState *) vm->backend_data;
    int pmx = (int) vm->r[1];
    int pmz = (int) vm->r[2];
    int rm  = (int) vm->r[3];
    int from_sector = (int) vm->r[4];
    vm->r[1] = -1;

    if (!s || !s->wr_map_set || !wr_blkmap || !s->wr_linedefs ||
        !s->wr_vertices || !s->wr_sidedefs || !s->wr_sectors)
        return;

    /* Dedupe stamps sized to the current map (realloc on mismatch -
     * @wr_map may have been re-uploaded since @wr_blockmap). */
    if (wr_blk_stamp_n != s->wr_n_linedefs) {
        if (wr_blk_stamp) { WR_FREE(wr_blk_stamp); wr_blk_stamp = NULL; }
        wr_blk_stamp_n = 0;
        if (s->wr_n_linedefs > 0) {
            wr_blk_stamp = (int *) WR_ALLOC((size_t) s->wr_n_linedefs * sizeof(int));
            if (!wr_blk_stamp) return;
            memset(wr_blk_stamp, 0, (size_t) s->wr_n_linedefs * sizeof(int));
            wr_blk_stamp_n = s->wr_n_linedefs;
            wr_blk_stamp_no = 0;
        } else {
            return;
        }
    }

    int bx0 = (pmx - rm - wr_blk_xorg) / 128;
    int bx1 = (pmx + rm - wr_blk_xorg) / 128;
    int by0 = (pmz - rm - wr_blk_yorg) / 128;
    int by1 = (pmz + rm - wr_blk_yorg) / 128;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 < 0) bx1 = 0;
    if (by1 < 0) by1 = 0;
    if (bx0 >= wr_blk_w) bx0 = wr_blk_w - 1;
    if (by0 >= wr_blk_h) by0 = wr_blk_h - 1;
    if (bx1 >= wr_blk_w) bx1 = wr_blk_w - 1;
    if (by1 >= wr_blk_h) by1 = wr_blk_h - 1;

    int result = 0;
    wr_blk_stamp_no++;
    for (int by = by0; by <= by1 && !result; by++) {
        for (int bx = bx0; bx <= bx1 && !result; bx++) {
            int bco = 8 + (by * wr_blk_w + bx) * 2;
            int boff = (wr_blkmap[bco] | (wr_blkmap[bco + 1] << 8)) * 2;
            while (boff + 1 < wr_blkmap_size) {
                int li = wr_blkmap[boff] | (wr_blkmap[boff + 1] << 8);
                if (li == 0xFFFF) break;
                boff += 2;
                if (li < s->wr_n_linedefs && wr_blk_stamp[li] != wr_blk_stamp_no) {
                    wr_blk_stamp[li] = wr_blk_stamp_no;
                    if (wr_blk_line_blocked(s, li, pmx, pmz, rm, from_sector)) {
                        result = 1;
                        break;
                    }
                }
            }
        }
    }
    vm->r[1] = result;
}

/* @wr_blocked_line(axm, azm, bxm, bzm, li): does movement segment a->b
 * (map units) cross linedef li? Boolean segment-segment intersection,
 * all i64 cross products, no division — the guest's f32 seg_intersect()
 * was ~10 soft-float ops plus a 9-arg marshal per candidate. */
static void wr_op_blocked_line(RelicVM *vm) {
    WrHostState *s = (WrHostState *) vm->backend_data;
    i64 ax = (int) vm->r[1], az = (int) vm->r[2];
    i64 bx = (int) vm->r[3], bz = (int) vm->r[4];
    int li = (int) vm->r[5];
    vm->r[1] = -1;

    if (!s || !s->wr_map_set || !s->wr_linedefs || !s->wr_vertices)
        return;
    if (li < 0 || li >= s->wr_n_linedefs) { vm->r[1] = 0; return; }

    const WrLinedef *ld = &s->wr_linedefs[li];
    int v1 = (unsigned short) ld->v1, v2 = (unsigned short) ld->v2;
    if (v1 >= s->wr_n_vertices || v2 >= s->wr_n_vertices) {
        vm->r[1] = 0; return;
    }
    i64 cx = s->wr_vertices[v1].x, cz = s->wr_vertices[v1].y;
    i64 dx = s->wr_vertices[v2].x, dz = s->wr_vertices[v2].y;

    i64 rx = bx - ax, rz = bz - az;
    i64 sx = dx - cx, sz = dz - cz;
    i64 denom = rx * sz - rz * sx;
    if (denom == 0) { vm->r[1] = 0; return; }        /* parallel */
    i64 qx = cx - ax, qz = cz - az;
    i64 tn = qx * sz - qz * sx;
    i64 un = qx * rz - qz * rx;
    /* t,u in [0,1] iff 0<=n<=denom for denom>0, denom<=n<=0 otherwise */
    int hit;
    if (denom > 0)
        hit = (tn >= 0 && tn <= denom && un >= 0 && un <= denom);
    else
        hit = (tn <= 0 && tn >= denom && un <= 0 && un >= denom);
    vm->r[1] = hit;
}

/* @wr_texmap(sidedef_tex_ptr, n_sidedefs, sector_tex_ptr, n_sectors) -
 * see OP_WR_TEXMAP. Copies the per-sidedef (3 i32s each: mid/upper/lower)
 * and per-sector (2 i32s each: floor/ceil) texture ID tables into
 * backend-owned storage. */
static void wr_op_texmap(RelicVM *vm) {
    WrHostState *s = (WrHostState *) vm->backend_data;
    if (!s) return;
    u32 sd_ptr = (u32) vm->r[1];
    i32 n_sd   = vm->r[2];
    u32 sc_ptr = (u32) vm->r[3];
    i32 n_sc   = vm->r[4];
    if (n_sd <= 0) return;

    if (s->wr_sd_tex)  { WR_FREE(s->wr_sd_tex);  s->wr_sd_tex = NULL; }
    if (s->wr_sec_tex) { WR_FREE(s->wr_sec_tex); s->wr_sec_tex = NULL; }
    s->wr_texmap_set = false;

    uint64_t sd_need = (uint64_t) n_sd * 3ull * 4ull;
    if ((uint64_t) sd_ptr + sd_need > (uint64_t) vm->mem_sz) return;
    s->wr_sd_tex = (i32 *) WR_ALLOC((size_t) sd_need);
    if (!s->wr_sd_tex) return;
    memcpy(s->wr_sd_tex, vm->mem + sd_ptr, (size_t) sd_need);

    if (n_sc > 0) {
        uint64_t sc_need = (uint64_t) n_sc * 2ull * 4ull;
        if ((uint64_t) sc_ptr + sc_need <= (uint64_t) vm->mem_sz) {
            s->wr_sec_tex = (i32 *) WR_ALLOC((size_t) sc_need);
            if (s->wr_sec_tex) {
                memcpy(s->wr_sec_tex, vm->mem + sc_ptr, (size_t) sc_need);
            }
        }
    }

    s->wr_texmap_set = true;
}

/* @wr_sky(tex_id) - see OP_WR_SKY. Sets the @wr_texture id used to fill
 * open (F_SKY1) ceiling gaps; 0 disables sky rendering, leaving those
 * columns untouched (the pre-sky behaviour). */
static void wr_op_sky(RelicVM *vm) {
    WrHostState *s = (WrHostState *) vm->backend_data;
    if (!s) return;
    s->wr_sky_tex_id = (i32) vm->r[1];
}

/* =========================================================================
 * BSP walk (step 2): front-to-back traversal with a solid-segment clip
 * list, so every screen column is written at most once - no z-buffer.
 *
 * Trig note: R_PointOnSide (the BSP partition test) is pure integer, as
 * in the classic engine. The view rotation and the frustum test below are NOT
 * table-based - they use plain
 * softfp sinf/cosf/tanf ONCE PER FRAME (not per seg, node, column or
 * pixel), which is a negligible cost. The per-pixel/per-column inner
 * loops (wr_draw_column/wr_draw_span above) remain entirely fixed-point
 * either way - that is the part that actually matters for frame rate.
 * ---------------------------------------------------------------------- */
#define WR_MAXSEGS 32   /* solid clip ranges; matches the classic engine's bound - a
                          * screen can only be divided into this many
                          * disjoint solid ranges before the clip merge
                          * would have already declared it fully closed. */

typedef struct { int first, last; } WrClipRange;

/* Per-frame renderer state. Global rather than threaded through the
 * recursive BSP walk, the same way the classic engine kept its render state in globals - there
 * is only ever one render in flight at a time. */
static WrClipRange wr_solidsegs[WR_MAXSEGS];
static int wr_solidsegs_n;
static WrHostState *wr_s;
static wfixed_t wr_view_x, wr_view_z;   /* eye position on the ground plane */
static wfixed_t wr_view_y;              /* eye height */
static wfixed_t wr_view_cos, wr_view_sin;
static wfixed_t wr_proj_dist;           /* projection plane distance, in screen pixels (fixed) */
static wfixed_t wr_inv_proj_dist;       /* 1 / proj_dist, precomputed (avoids per-column divide) */
/* (Lighting is resolved per-sector at draw time via wr_sector_colormap,
 * so there is no single "current" colormap for the frame.) */

/* Partition side test: which side of a BSP node's partition line a
 * point falls on. Pure integer, matching the classic engine - this only decides
 * traversal order (near child first), so it runs once per node, not per
 * pixel. Axis-aligned partitions (the common case) take the cheap exact
 * paths; the general case is a 64-bit cross product. */
static int wr_point_on_side(int x, int z, const WrNode *nd) {
    if (nd->dx == 0) {
        if (x <= nd->px) return nd->dy > 0 ? 1 : 0;
        return nd->dy > 0 ? 0 : 1;
    }
    if (nd->dy == 0) {
        if (z <= nd->py) return nd->dx > 0 ? 0 : 1;
        return nd->dx > 0 ? 1 : 0;
    }
    long long dx = x - nd->px;
    long long dz = z - nd->py;
    long long left  = (long long) nd->dy * dx;
    long long right = dz * (long long) nd->dx;
    return (right >= left) ? 1 : 0;
}

/* Mark screen columns [x1,x2] as solid, merging with/absorbing any
 * overlapping existing ranges. The classic solid-range clip
 * structure: find where x1 falls relative to the existing sorted ranges,
 * then where x2 falls, then splice. Kept deliberately simple (an O(n)
 * scan/rebuild over at most WR_MAXSEGS ranges) rather than the classic
 * hand-tuned in-place version, since n is tiny. */
static void wr_solid_add(int x1, int x2) {
    WrClipRange merged[WR_MAXSEGS];
    int n = 0;
    int i = 0;
    /* Ranges strictly before the new one, untouched. */
    while (i < wr_solidsegs_n && wr_solidsegs[i].last < x1 - 1) {
        merged[n++] = wr_solidsegs[i++];
    }
    /* Absorb every range that overlaps or touches [x1,x2]. */
    while (i < wr_solidsegs_n && wr_solidsegs[i].first <= x2 + 1) {
        if (wr_solidsegs[i].first < x1) x1 = wr_solidsegs[i].first;
        if (wr_solidsegs[i].last  > x2) x2 = wr_solidsegs[i].last;
        i++;
    }
    if (n < WR_MAXSEGS) { merged[n].first = x1; merged[n].last = x2; n++; }
    /* Ranges strictly after, untouched. */
    while (i < wr_solidsegs_n && n < WR_MAXSEGS) {
        merged[n++] = wr_solidsegs[i++];
    }
    memcpy(wr_solidsegs, merged, (size_t) n * sizeof(WrClipRange));
    wr_solidsegs_n = n;
}

/* True once solidsegs covers the whole screen - the walk's early-out,
 * since nothing further back can possibly still be visible. */
static int wr_screen_closed(void) {
    return wr_solidsegs_n == 1 && wr_solidsegs[0].first <= 0 &&
           wr_solidsegs[0].last >= wr_screen_w - 1;
}

/* Child-bbox visibility test: decide whether a node's child bounding box is potentially
 * visible, so wr_render_node can skip fully-offscreen or already-occluded
 * subtrees without descending them. This is the classic engine's biggest BSP
 * win on a cacheless 486 — it avoids transforming and projecting every
 * seg in a subtree that can't contribute a single visible pixel.
 *
 * bbox points at 4 i16s in WAD order: top(max y), bottom(min y),
 * left(min x), right(max x). In adoom's coordinate convention the WAD's
 * (x, y) maps to renderer (x, z).
 *
 * The test transforms the 4 corners to view space, rejects the box if
 * all corners are behind the camera (vz <= 0), then projects the visible
 * corners to screen X and checks whether any column in the resulting
 * range is NOT already covered by the solid-segment clip list. If every
 * column is covered, the subtree is fully occluded by nearer walls and
 * can be skipped. */
static int wr_check_bbox(const i16 *bbox) {
    /* bbox[0]=top(maxy), bbox[1]=bottom(miny), bbox[2]=left(minx), bbox[3]=right(maxx) */
    int minx = (int) bbox[2];
    int maxx = (int) bbox[3];
    int miny = (int) bbox[1];
    int maxy = (int) bbox[0];

    int centerx = wr_screen_w >> 1;
    int sx1 = wr_screen_w, sx2 = -1;   /* visible screen-x range */
    int any_in_front = 0, any_behind = 0;

    /* Viewer inside the box (vanilla's boxpos == 5): always potentially
     * visible - projecting corners would under-cover the true span. */
    int cx_world = (int) (wr_view_x >> WR_FRACBITS);
    int cz_world = (int) (wr_view_z >> WR_FRACBITS);
    if (cx_world >= minx && cx_world <= maxx &&
        cz_world >= miny && cz_world <= maxy) return 1;

    /* Transform the 4 corners to view space and project the ones that
     * are in front of the camera. We use the same view transform as
     * wr_add_seg: vx = dx*cos - dz*sin, vz = dx*sin + dz*cos. */
    int xs[4] = { minx, maxx, minx, maxx };
    int ys[4] = { miny, miny, maxy, maxy };
    int c;
    for (c = 0; c < 4; c++) {
        int dx = xs[c] - cx_world;
        int dz = ys[c] - cz_world;
        /* 16.16 fixed-point view transform (same as wr_add_seg but
         * with int inputs shifted to 16.16). */
        int64_t vx = ((int64_t) dx << WR_FRACBITS) * wr_view_cos
                   - ((int64_t) dz << WR_FRACBITS) * wr_view_sin;
        int64_t vz = ((int64_t) dx << WR_FRACBITS) * wr_view_sin
                   + ((int64_t) dz << WR_FRACBITS) * wr_view_cos;
        vx >>= WR_FRACBITS;
        vz >>= WR_FRACBITS;
        if (vz <= 0) { any_behind = 1; continue; }  /* behind the camera plane */
        any_in_front = 1;
        /* Project to screen X: sx = centerx + vx * proj_dist / vz. */
        int sx = centerx + (int) (wr_div64(vx * (int64_t) wr_proj_dist, (int) vz) >> WR_FRACBITS);
        if (sx < sx1) sx1 = sx;
        if (sx > sx2) sx2 = sx;
    }

    /* All corners behind the camera: subtree is entirely invisible. */
    if (!any_in_front) return 0;

    /* Box crosses the view plane: the edges running to the behind-camera
     * corners can sweep to the screen edges, so the projected range from
     * the in-front corners alone underestimates. Be conservative and use
     * the full screen span (the solidsegs occlusion test below still
     * applies, so a closed screen still rejects the subtree). */
    if (any_behind) { sx1 = 0; sx2 = wr_screen_w - 1; }

    /* Clip the screen-x range to the actual screen. If the bbox projects
     * entirely off-screen, the subtree is invisible. */
    if (sx2 < 0) return 0;
    if (sx1 >= wr_screen_w) return 0;
    if (sx1 < 0) sx1 = 0;
    if (sx2 >= wr_screen_w) sx2 = wr_screen_w - 1;
    if (sx1 > sx2) return 0;

    /* Check whether any column in [sx1, sx2] is NOT covered by solidsegs.
     * If every column is already claimed by a nearer solid wall, the
     * subtree is fully occluded. This is the key test that makes the
     * far-child reject effective even when the bbox is on-screen. */
    int col = sx1;
    int i;
    for (i = 0; i < wr_solidsegs_n; i++) {
        if (wr_solidsegs[i].last < col) continue;      /* range is before our cursor */
        if (wr_solidsegs[i].first > col) return 1;      /* gap before this range: visible */
        /* Range covers col: advance cursor past it. */
        col = wr_solidsegs[i].last + 1;
        if (col > sx2) return 0;                        /* fully covered */
    }
    /* Cursor didn't reach sx2: there are uncovered columns after the
     * last solid range. */
    return (col <= sx2);
}


/* -------------------------------------------------------------------------
 * Per-column clip arrays and visplanes
 *
 * These are what make the renderer sector-based rather than OpenGL-like:
 * instead of a z-buffer test per pixel, occlusion is tracked as two
 * per-column integers, and floors/ceilings are accumulated as "visplanes"
 * during the BSP walk then drawn afterwards as horizontal spans (one
 * divide per ROW, not per pixel).
 *
 *   ceilingclip[x] = lowest row already covered from the top (exclusive)
 *   floorclip[x]   = highest row already covered from the bottom (exclusive)
 *
 * A column is open between ceilingclip[x]+1 and floorclip[x]-1. A solid
 * wall closes the column completely; a portal (two-sided line) only
 * narrows it, which is how you see through doorways into rooms behind.
 * ---------------------------------------------------------------------- */
#define WR_MAX_SCREEN_W 1024
#define WR_MAX_SCREEN_H 768
#define WR_MAXVISPLANES 128
/* "no range recorded in this column" sentinel for WrVisPlane.top.
 *
 * Deliberately -1 rather than a large positive value: every byte of the
 * short is then 0xFF, which lets a fresh plane's top[] be cleared with a
 * single memset (one `rep stos` moving 4 bytes a cycle) instead of a
 * short-at-a-time store loop. -1 can never collide with a real entry
 * because wr_plane_add_column clamps top to >= 0 before storing it, and
 * every use of this macro is an == / != test, so the actual value carries
 * no other meaning. */
#define WR_PLANE_UNUSED (-1)
/* Sentinel tex_id for the merged sky visplane (see wr_find_plane). Never
 * collides with a real @wr_texture/@wr_flat id, which start at 1, or with
 * 0 ("no plane / sky disabled"). */
#define WR_SKY_PLANE_ID (-1)
/* Sentinel tex_id for the "void" visplane (see wr_void_plane): a plane
 * that draws solid black spans. It exists so that every screen row a
 * wall column claims can be *written* even when the real plane is
 * unavailable - missing flat, sky disabled, plane allocation overflow,
 * or the geometry gates (worldtop<=0 / worldbottom>=0) that never ask
 * for one. With void planes substituted at every claim site, claimed
 * rows are guaranteed written, which is what lets the frame skip its
 * full-screen memset and only black-fill the rows that stayed
 * *unclaimed* at the end of the walk. */
#define WR_VOID_PLANE_ID (-2)

static short wr_ceilingclip[WR_MAX_SCREEN_W];
static short wr_floorclip[WR_MAX_SCREEN_W];

/* Per-column "drawseg" records for masked-sprite occlusion (@wr_sprite).
 * Each time a seg writes clip bounds at column x in wr_add_seg, a snapshot
 * {iscale, ceilingclip, floorclip} is appended here. Because the BSP walk
 * is strictly front-to-back the entries arrive nearest-first (iscale =
 * vz/proj is reciprocal scale: smaller = closer), and because each stored
 * window is written against the already-narrowed clip pair the bounds are
 * cumulative. At sprite time the column's writable window is the snapshot
 * left by the last entry whose seg is closer than the sprite - one scan
 * per column replaces a whole drawseg list, at the cost of a bounded
 * per-column depth (8 transitions covers every realistic portal stack;
 * excess entries are dropped, which can only over-reveal a sprite deep in
 * a stack of openings, never clip one wrongly). */
#define WR_MAX_CLIPSEGS 8
static i32  wr_clipseg_scale[WR_MAX_SCREEN_W][WR_MAX_CLIPSEGS];
static short wr_clipseg_top [WR_MAX_SCREEN_W][WR_MAX_CLIPSEGS];
static short wr_clipseg_bot [WR_MAX_SCREEN_W][WR_MAX_CLIPSEGS];
static unsigned char wr_clipseg_n[WR_MAX_SCREEN_W];

WR_FORCE_INLINE void wr_clipseg_record(int x, wfixed_t iscale, int top, int bot) {
    int n = wr_clipseg_n[x];
    if (n >= WR_MAX_CLIPSEGS) return;
    wr_clipseg_scale[x][n] = iscale;
    wr_clipseg_top[x][n]   = (short) top;
    wr_clipseg_bot[x][n]   = (short) bot;
    wr_clipseg_n[x] = (unsigned char) (n + 1);
}

/* The sprite's writable window at column x: merge the snapshots of every
 * seg closer than the sprite (iscale smaller = closer). Entries are
 * appended nearest-first and each is cumulative, so the last qualifying
 * entry's bounds are the final window; the first entry behind the sprite
 * ends the scan. Returns the open row range [top, bottom] inclusive. */
static void wr_sprite_clip_window(int x, wfixed_t sprite_iscale,
                                  int *out_top, int *out_bot) {
    int top = -1, bot = wr_screen_h;
    int n = wr_clipseg_n[x];
    int i;
    for (i = 0; i < n; i++) {
        if (wr_clipseg_scale[x][i] >= sprite_iscale) break;
        top = wr_clipseg_top[x][i];
        bot = wr_clipseg_bot[x][i];
    }
    *out_top = top + 1;
    *out_bot = bot - 1;
}

/* Masked midtexture (fence/grate) column records. A two-sided line with a
 * mid texture cannot draw it during the wall pass - the portal opening
 * stays open for farther geometry, which would overpaint the fence.
 * The classic engine defers these to a masked-draw pass interleaved with
 * sprites; the equivalent here is that each column of a midtex'd portal
 * pushes a record, and @wr_op_sprite renders them depth-interleaved with
 * the sprite batch. Entries are appended in BSP order (nearest-first,
 * ascending iscale). */
#define WR_MAX_MTEX 4
typedef struct {
    i32  iscale;   /* inverse scale, same units as clipseg scale */
    i32  texmid;   /* texturemid anchor (same convention as wr_draw_column) */
    i32  u;        /* texture u coordinate, 16.16 */
    i16  ceil;     /* first open row of the portal window */
    i16  floor;    /* last open row of the portal window */
    i16  tex;      /* wr_textures id */
    i16  light;    /* colormap row */
} WrMtexCol; /* 20 bytes */
static WrMtexCol    wr_mtex[WR_MAX_SCREEN_W][WR_MAX_MTEX];
static unsigned char wr_mtex_n[WR_MAX_SCREEN_W];

static void wr_mtex_record(int x, wfixed_t iscale, wfixed_t texmid,
                           wfixed_t u, int top, int bot, int tex,
                           int light) {
    int n = wr_mtex_n[x];
    if (n >= WR_MAX_MTEX) return;
    WrMtexCol *m = &wr_mtex[x][n];
    m->iscale = iscale;
    m->texmid = texmid;
    m->u      = u;
    m->ceil   = (short) top;
    m->floor  = (short) bot;
    m->tex    = (short) tex;
    m->light  = (short) light;
    wr_mtex_n[x] = (unsigned char) (n + 1);
}

/* Draw one deferred masked-texture column. Transparent texels (index 0)
 * are skipped and the texture CLAMPS vertically - a midtex never tiles,
 * unlike the repeating wall columns. */
static void wr_mtex_draw(WrHostState *s, int x, const WrMtexCol *m) {
    if (!s || m->tex <= 0 || m->tex >= WR_MAX_TEXTURES) return;
    const unsigned char *data = s->wr_textures[m->tex].data;
    int tw = s->wr_textures[m->tex].w, th = s->wr_textures[m->tex].h;
    if (!data || tw <= 0 || th <= 0) return;

    int yl = m->ceil, yh = m->floor;
    if (yl < 0) yl = 0;
    if (yh >= wr_screen_h) yh = wr_screen_h - 1;
    if (yl > yh) return;

    int tc;
    if ((tw & (tw - 1)) == 0) tc = (int) (((unsigned) (m->u >> WR_FRACBITS)) & ((unsigned) tw - 1u));
    else { tc = (m->u >> WR_FRACBITS) % tw; if (tc < 0) tc += tw; }
    const unsigned char *src = data + (size_t) tc * (size_t) th;
    const WrPix *cmap = s->wr_colormap
        ? (const WrPix *)s->wr_colormap + (size_t) m->light * 256u : NULL;
    if (!cmap) return;

    wfixed_t frac = m->texmid + (yl - (wr_screen_h >> 1)) * m->iscale;
    WrPix *dst = WR_ROW(s, yl * wr_px_sy + wr_origin_y) + (x * wr_px_sx + wr_origin_x);
    int pitch = WR_PITCH(s, yl * wr_px_sy + wr_origin_y);
    int y;
    for (y = yl; y <= yh; y++, dst += pitch * wr_px_sy, frac += m->iscale) {
        int fy = frac >> WR_FRACBITS;
        unsigned char pix;
        if ((unsigned) fy >= (unsigned) th) continue;
        pix = src[fy];
        if (pix != 0) {
            WrPix p = cmap[pix];
            dst[0] = p;
            if (wr_px_sx > 1) dst[1] = p;
            if (wr_px_sy > 1) {
                dst[pitch] = p;
                if (wr_px_sx > 1) dst[pitch + 1] = p;
            }
        }
    }
}

/* Sky scroll set up once per frame in wr_op_render (the only float math
 * sky needs - everything below is fixed-point). wr_sky_u0 is the
 * texture-space U coordinate at the screen's centre column; wr_sky_du_col
 * is the U step per screen column. */
static wfixed_t wr_sky_u0;
static wfixed_t wr_sky_du_col;

typedef struct {
    int height;      /* plane's world height, in map units (0 for sky) */
    int tex_id;      /* @wr_flat id; WR_SKY_PLANE_ID = sky; WR_VOID_PLANE_ID = black */
    int light;       /* colormap row (unused for sky - always full bright) */
    int minx, maxx;
    short *top;      /* per-column first row of this plane (WR_PLANE_UNUSED = none) */
    short *bottom;   /* per-column last row */
    /* Draw inputs resolved once when the plane is created, instead of
     * per emitted span in wr_map_plane (the flat lookup + light-row
     * clamp + colormap pointer cost a real divide-free but non-trivial
     * amount of ALU per call on a 486, and a floor emits hundreds of
     * spans). NULL src = plane records columns but draws nothing. */
    const unsigned char *src;
    const WrPix *cmap;
} WrVisPlane;

static WrVisPlane wr_visplanes[WR_MAXVISPLANES];
static int wr_n_visplanes;
/* wr_plane_buf / wr_plane_buf_w are declared near the top of this file so
 * the includer's shutdown path can free the allocation: one block of
 * WR_MAXVISPLANES * 2 * width shorts, carved up per plane. */
static int wr_spanstart[WR_MAX_SCREEN_H];

static WrVisPlane *wr_check_plane(WrVisPlane *pl, int start, int stop);

/* Plane lookup: reuse an existing visplane with identical height/texture/
 * light, else start a new one. Merging matters a lot - a large floor seen
 * through several portals is one plane, drawn in one pass. */
static WrVisPlane *wr_find_plane(int height, int tex_id, int light) {
    const unsigned char *src = NULL;
    const WrPix *cmap = NULL;
    if (tex_id == 0) {
        /* F_SKY1: merge every sky span in the level into ONE visplane,
         * ignoring height/light, exactly like the classic
         * sky-flat merge - the sky is drawn once regardless of how many
         * sectors/heights refer to it. If no sky texture was uploaded via
         * @wr_sky, leave the plane untouched. */
        if (wr_s->wr_sky_tex_id <= 0) return NULL;
        tex_id = WR_SKY_PLANE_ID;
        height = 0;
        light  = 0;
    } else if (tex_id < 0 || tex_id >= WR_MAX_FLATS || !wr_s->wr_flats[tex_id]) {
        /* Missing/bogus flat id: no plane. The caller substitutes a
         * void (black) plane so the claimed rows are still written. */
        return NULL;
    } else {
        /* Resolve the draw inputs once: the flat pointer plus the
         * colormap row for this plane's light level. */
        src = wr_s->wr_flats[tex_id];
        int cm_row = light;
        if (cm_row < 0) cm_row = 0;
        if (cm_row >= wr_s->wr_num_colormaps) cm_row = wr_s->wr_num_colormaps - 1;
        cmap = (const WrPix *) wr_s->wr_colormap + (size_t) cm_row * 256u;
    }
    int i;
    for (i = 0; i < wr_n_visplanes; i++) {
        WrVisPlane *p = &wr_visplanes[i];
        if (p->height == height && p->tex_id == tex_id && p->light == light) return p;
    }
    if (wr_n_visplanes >= WR_MAXVISPLANES) return NULL;  /* visplane overflow: drop, as vanilla did */
    WrVisPlane *p = &wr_visplanes[wr_n_visplanes];
    p->height = height;
    p->tex_id = tex_id;
    p->light  = light;
    p->src    = src;
    p->cmap   = cmap;
    p->minx = wr_screen_w;
    p->maxx = -1;
    p->top    = wr_plane_buf + (size_t) wr_n_visplanes * 2 * (size_t) wr_plane_buf_w;
    p->bottom = p->top + wr_plane_buf_w;
    /* WR_PLANE_UNUSED is -1, i.e. all bits set, so this is a memset. */
    memset(p->top, 0xFF, (size_t) wr_screen_w * sizeof(short));
    wr_n_visplanes++;
    wrp_visplanes++;
    return p;
}

/* Void plane: records column ranges exactly like a real visplane, but
 * every span it emits is solid black (see WR_VOID_PLANE_ID). Used by
 * wr_seg_columns whenever the real plane lookup returns NULL, so that
 * every row a column claims is guaranteed to be *written* - the black
 * that the old full-screen clear produced, but only where it is needed. */
static WrVisPlane *wr_void_plane(int start, int stop) {
    int i;
    for (i = 0; i < wr_n_visplanes; i++) {
        WrVisPlane *p = &wr_visplanes[i];
        if (p->tex_id == WR_VOID_PLANE_ID) return wr_check_plane(p, start, stop);
    }
    if (wr_n_visplanes >= WR_MAXVISPLANES) return NULL;
    WrVisPlane *p = &wr_visplanes[wr_n_visplanes];
    p->height = 0;
    p->tex_id = WR_VOID_PLANE_ID;
    p->light  = 0;
    p->src    = NULL;
    p->cmap   = NULL;
    p->minx   = start;
    p->maxx   = stop;
    p->top    = wr_plane_buf + (size_t) wr_n_visplanes * 2 * (size_t) wr_plane_buf_w;
    p->bottom = p->top + wr_plane_buf_w;
    memset(p->top, 0xFF, (size_t) wr_screen_w * sizeof(short));
    wr_n_visplanes++;
    wrp_visplanes++;
    return p;
}

/* Plane check: a visplane stores exactly ONE vertical range per screen
 * column, so before a seg writes its column range [start,stop] into a
 * (possibly shared) plane, check whether any of those columns are already
 * in use.  If they are, hand back a fresh plane with the same attributes
 * instead - otherwise the new seg's ranges silently overwrite the old
 * ones and the old areas render as black holes (visible around free-
 * standing pillars, where the same ceiling/floor flat is seen on several
 * sides in the same screen columns). */
static WrVisPlane *wr_check_plane(WrVisPlane *pl, int start, int stop) {
    if (!pl) return NULL;
    int intrl, intrh, unionl, unionh, x;
    if (start < pl->minx) { intrl = pl->minx; unionl = start; }
    else                  { unionl = pl->minx; intrl = start; }
    if (stop > pl->maxx)  { intrh = pl->maxx; unionh = stop; }
    else                  { unionh = pl->maxx; intrh = stop; }
    for (x = intrl; x <= intrh; x++)
        if (pl->top[x] != WR_PLANE_UNUSED) break;
    if (x > intrh) {                    /* no collision: extend in place */
        pl->minx = unionl;
        pl->maxx = unionh;
        return pl;
    }
    if (wr_n_visplanes >= WR_MAXVISPLANES) return NULL;
    WrVisPlane *np = &wr_visplanes[wr_n_visplanes];
    np->height = pl->height;
    np->tex_id = pl->tex_id;
    np->light  = pl->light;
    np->src    = pl->src;
    np->cmap   = pl->cmap;
    np->minx = start;
    np->maxx = stop;
    np->top    = wr_plane_buf + (size_t) wr_n_visplanes * 2 * (size_t) wr_plane_buf_w;
    np->bottom = np->top + wr_plane_buf_w;
    memset(np->top, 0xFF, (size_t) wr_screen_w * sizeof(short));
    wr_n_visplanes++;
    wrp_visplanes++;
    wrp_plane_splits++;
    return np;
}

WR_FORCE_INLINE void wr_plane_add_column(WrVisPlane *pl, int x, int top, int bottom) {
    if (!pl || top > bottom) return;
    if (x < 0 || x >= wr_screen_w) return;
    if (top < 0) top = 0;
    if (bottom >= wr_screen_h) bottom = wr_screen_h - 1;
    if (top > bottom) return;
    pl->top[x] = (short) top;
    pl->bottom[x] = (short) bottom;
    if (x < pl->minx) pl->minx = x;
    if (x > pl->maxx) pl->maxx = x;
}

/* Per-row plane cache - the classic row-cache
 * optimisation. For a given visplane the eye height above
 * the plane (ah) is constant, so ratio = ah/ady depends only on the
 * screen row y. When consecutive spans hit the same row at the same
 * plane height, the divide and both 64-bit multiplies for xstep/ystep
 * can be skipped entirely — just reuse the cached values.
 *
 * wr_plane_cache_ah[y] records which ah value last filled the cache for
 * row y; a mismatch means the row's distance changed (different plane
 * height or the player moved vertically) and the cache must be refilled.
 * Initialised to -1 (sentinel) at the start of each frame in wr_draw_planes. */
static wfixed_t wr_plane_cache_ah[WR_MAX_SCREEN_H];
static wfixed_t wr_plane_cache_ratio[WR_MAX_SCREEN_H];
static wfixed_t wr_plane_cache_xstep[WR_MAX_SCREEN_H];
static wfixed_t wr_plane_cache_ystep[WR_MAX_SCREEN_H];
static wfixed_t wr_plane_cache_dist[WR_MAX_SCREEN_H];
static wfixed_t wr_plane_cache_dcos[WR_MAX_SCREEN_H];   /* dist*cos(a) */
static wfixed_t wr_plane_cache_dsin[WR_MAX_SCREEN_H];   /* dist*sin(a) */

/* R_MapPlane: draw one horizontal run of a floor/ceiling.
 *
 * This is where the "no perspective divide per pixel" claim is actually
 * cashed in. Every pixel in a given ROW of a flat plane is the same
 * distance from the eye, so one divide per row yields the world-space
 * step per column, and the span loop is pure add + table lookup. */
static void wr_map_plane(WrVisPlane *pl, int y, int x1, int x2) {
    wrp_map_calls++;
    if (x2 < x1 || y < 0 || y >= wr_screen_h) return;
    if (pl->tex_id == WR_VOID_PLANE_ID) {
        /* Void plane: claimed-but-unwriteable region - emit black so the
         * frame needs no full-screen clear. */
        wr_fill_span(wr_s, y, x1, x2, (WrPix) 0);
        return;
    }
    const unsigned char *src = pl->src;
    if (!src) return;
    const WrPix *cmap = pl->cmap;

    if (wr_s->wr_flatshade || wr_stripshade) {
        /* Flat-shade fast path: skip the whole perspective-divide plane
         * maths below (ratio/dist/xstep/ystep) as well as the per-pixel
         * span walk - a solid-colour row never needs any of it. Stripe
         * mode shares the path: flats have no useful 1-D projection, so
         * they take the same solid fill either way. */
        WrPix color = cmap[wr_flat_avg_color(wr_s, pl->tex_id)];
        wr_fill_span(wr_s, y, x1, x2, color);
        return;
    }

    int centery = wr_screen_h >> 1;
    int centerx = wr_screen_w >> 1;
    int dy = y - centery;
    if (dy == 0) return;                 /* exactly on the horizon: infinitely far */

    /* Eye height above the plane. Positive => plane is below the eye, so
     * it can only appear below the horizon (dy > 0), and vice versa. */
    wfixed_t h_rel = wr_view_y - ((wfixed_t) pl->height << WR_FRACBITS);
    if (h_rel == 0) return;
    if ((h_rel > 0) != (dy > 0)) return; /* on the wrong side of the horizon */

    wfixed_t ah = h_rel > 0 ? h_rel : -h_rel;
    int ady = dy > 0 ? dy : -dy;

    /* Cache lookup: if this row was last computed for the same plane
     * height (ah), reuse the ratio/xstep/ystep/dist and skip the divide
     * + two 64-bit multiplies. On a 486SX the divide alone is ~40 cycles
     * and each 64-bit multiply is a __muldi3 library call (~30 cycles);
     * skipping all three saves ~100 cycles per cached row, and a typical
     * floor has 100+ rows that repeat across multiple visplanes. */
    wfixed_t ratio, xstep, ystep, dist, dcos, dsin;
    if (wr_plane_cache_ah[y] == ah) {
        ratio  = wr_plane_cache_ratio[y];
        xstep  = wr_plane_cache_xstep[y];
        ystep  = wr_plane_cache_ystep[y];
        dcos   = wr_plane_cache_dcos[y];
        dsin   = wr_plane_cache_dsin[y];
    } else {
        /* ratio = world units per screen column at this row's distance.
         * Derived as dist/proj = (h_rel*proj/dy)/proj = h_rel/dy, so it
         * needs only ONE 32-bit divide (16.16 / int = 16.16). dist then
         * follows with a single 64-bit multiply. */
        ratio = ah / ady;
        if (ratio <= 0) return;
        dist = (wfixed_t) (((long long) ratio * wr_proj_dist) >> WR_FRACBITS);
        if (dist <= 0) return;

        /* Rotate the view-space (right, forward) basis back into world
         * space. right = (cos(a), -sin(a)), forward = (sin(a), cos(a)).
         * Both products are 16.16 * 16.16 = 32.32, so >> 16 gives 16.16. */
        xstep = (wfixed_t) (((long long) ratio * wr_view_cos) >> WR_FRACBITS);
        ystep = (wfixed_t) (-(((long long) ratio * wr_view_sin) >> WR_FRACBITS));

        /* dist*cos / dist*sin are also per-(row, ah) constants - each
         * span on this row needs them for its left-endpoint world coord,
         * so keep them in the cache too and save two 32x32 muls per
         * call. */
        dcos = (wfixed_t) (((long long) dist * wr_view_cos) >> WR_FRACBITS);
        dsin = (wfixed_t) (((long long) dist * wr_view_sin) >> WR_FRACBITS);

        wr_plane_cache_ah[y]    = ah;
        wr_plane_cache_ratio[y] = ratio;
        wr_plane_cache_xstep[y] = xstep;
        wr_plane_cache_ystep[y] = ystep;
        wr_plane_cache_dist[y]  = dist;
        wr_plane_cache_dcos[y]  = dcos;
        wr_plane_cache_dsin[y]  = dsin;
    }

    /* r0*cos = (x1-centerx)*ratio*cos = (x1-centerx)*xstep, and likewise
     * r0*sin folds into -ystep. int * 16.16 is already 16.16, so the
     * i64 product casts straight down - one 64-bit multiply per world
     * axis instead of three. */
    long long dx1 = x1 - centerx;
    wfixed_t wx = wr_view_x
                + (wfixed_t) (dx1 * xstep)
                + dsin;
    wfixed_t wz = wr_view_z
                + (wfixed_t) (dx1 * ystep)
                + dcos;

    wr_draw_span(wr_s, y, x1, x2, wx, wz, xstep, ystep, src, cmap);
}

/* Span conversion: convert per-column top/bottom ranges into horizontal runs,
 * emitting each run exactly once as the column ranges open and close.
 * The classic scheme - the point is that a plane's pixels get
 * visited once total, not once per (row, column) candidate. */
static void wr_make_spans(WrVisPlane *pl, int x, int t1, int b1, int t2, int b2) {
    if (wr_skip_mode == 7) {   /* TEMP PROFILING: WRSKIP=m - same scan, no map_plane calls */
        while (t1 < t2 && t1 <= b1) t1++;
        while (b1 > b2 && b1 >= t1) b1--;
        while (t2 < t1 && t2 <= b2) { wr_spanstart[t2] = x; wrp_span_writes++; t2++; }
        while (b2 > b1 && b2 >= t2) { wr_spanstart[b2] = x; wrp_span_writes++; b2--; }
        return;
    }
    while (t1 < t2 && t1 <= b1) { wr_map_plane(pl, t1, wr_spanstart[t1], x - 1); t1++; }
    while (b1 > b2 && b1 >= t1) { wr_map_plane(pl, b1, wr_spanstart[b1], x - 1); b1--; }
    while (t2 < t1 && t2 <= b2) { wr_spanstart[t2] = x; wrp_span_writes++; t2++; }
    while (b2 > b1 && b2 >= t2) { wr_spanstart[b2] = x; wrp_span_writes++; b2--; }
}

/* Sky range fill (approximate): fills a sky visplane's per-column
 * ranges directly, column by column, instead of going through the
 * horizontal-span machinery above - the sky is "at infinity", so unlike a
 * real flat its texture coordinates don't need a perspective divide at
 * all, just a per-column U (from view angle) and a fixed per-row V
 * (no vertical parallax, matching the classic engine: looking up/down never happened
 * there, and the sky is never affected by sector light or distance). */
static void wr_draw_sky_plane(const WrVisPlane *pl) {
    i32 tex_id = wr_s->wr_sky_tex_id;
    if (tex_id <= 0 || tex_id >= WR_MAX_TEXTURES) return;
    const unsigned char *data = wr_s->wr_textures[tex_id].data;
    int tex_w = wr_s->wr_textures[tex_id].w;
    int tex_h = wr_s->wr_textures[tex_id].h;
    if (!data || tex_w <= 0 || tex_h <= 0) return;

    int centerx = wr_screen_w >> 1;
    int centery = wr_screen_h >> 1;
    /* Vertical mapping is 1:1 (one texel per screen row) with wrapping,
     * exactly like the classic engine: the sky's texturemid is
     * (100<<16) and the column step is 1.0 at 320x200, and the sky
     * column is indexed with (frac >> 16) & 127. So:
     *
     *     texel_row = ((y - centery) + skytexturemid) mod tex_h
     *
     * Do NOT scale the texture to each column's yl..yh extent. The sky
     * visplane's per-column range is bounded by whatever wall top is in
     * front of it, so it varies wildly from column to column; scaling to
     * it squeezes the whole 128-row texture into a few pixels in some
     * columns and stretches it in others, which destroys continuity
     * between neighbouring columns and reads as flat horizontal bands. */
    int sky_mid_row = centery;
    const WrPix *cmap = (const WrPix *)wr_s->wr_colormap; /* row 0 = full bright */

    int x;
    for (x = pl->minx; x <= pl->maxx; x++) {
        if (pl->top[x] == WR_PLANE_UNUSED) continue;
        int yl = pl->top[x], yh = pl->bottom[x];
        if (yl < 0) yl = 0;
        if (yh >= wr_screen_h) yh = wr_screen_h - 1;
        if (yl > yh) continue;

        /* wr_sky_du_col is a 16.16 texel step per SCREEN COLUMN, and
         * (x - centerx) is a plain integer, so int * 16.16 is already
         * 16.16 - no shift. (A >> WR_FRACBITS here divides the step by
         * 65536, which collapses tex_col to a constant across the whole
         * screen: one sky column smeared horizontally.) */
        wfixed_t u = wr_sky_u0 + (wfixed_t) (x - centerx) * wr_sky_du_col;
        int tex_col = (int) ((u >> WR_FRACBITS) % tex_w);
        if (tex_col < 0) tex_col += tex_w;
        const unsigned char *col = data + (size_t) tex_col * tex_h;

        int row = (yl - centery) + sky_mid_row;
        row %= tex_h;
        if (row < 0) row += tex_h;
        WrPix *dst = WR_ROW(wr_s, yl * wr_px_sy + wr_origin_y) + (x * wr_px_sx + wr_origin_x);
        int pitch = WR_PITCH(wr_s, yl * wr_px_sy + wr_origin_y);
        int y;
        for (y = yl; y <= yh; y++, dst += pitch * wr_px_sy) {
            WrPix p = cmap[col[row]];
            dst[0] = p;
            if (wr_px_sx > 1) dst[1] = p;
            if (wr_px_sy > 1) {
                dst[pitch] = p;
                if (wr_px_sx > 1) dst[pitch + 1] = p;
            }
            if (++row >= tex_h) row = 0;
        }
        wrp_sky_px += (unsigned int)(yh - yl + 1);
    }
}

/* Plane flush: flush every visplane accumulated during the BSP walk. */
static void wr_draw_planes(void) {
    int i;
    /* Invalidate the per-row plane cache: the player's eye height may
     * have changed since last frame, so every row must recompute its
     * ratio/xstep/ystep on first use. The sentinel -1 never matches a
     * real ah (which is always >= 0). */
    for (i = 0; i < wr_screen_h; i++) wr_plane_cache_ah[i] = -1;
    if (wr_skip_mode == 6) return;   /* TEMP PROFILING: WRSKIP=d - skip draw_planes entirely */
    wrp_visplanes_tot += (unsigned int) wr_n_visplanes;
    for (i = 0; i < wr_n_visplanes; i++) {
        WrVisPlane *pl = &wr_visplanes[i];
        if (pl->maxx < pl->minx) continue;
        if (pl->tex_id == WR_SKY_PLANE_ID) {
            unsigned int ts = WR_MILLIS();
            wr_draw_sky_plane(pl);
            wrp_sky_ms += WR_MILLIS() - ts;
            continue;
        }
        int x;
        wrp_plane_cols_tot += (unsigned int)(pl->maxx - pl->minx + 2);
        {
        /* Sliding window: carry the previous column's range in registers
         * (t1/b1) instead of re-reading top[x-1]/bottom[x-1] each column -
         * halves the array loads and drops the minx/maxx re-checks. */
        int t1 = 0, b1 = -1;            /* no previous column: empty range */
        for (x = pl->minx; x <= pl->maxx + 1; x++) {
            int t2, b2;
            if (x <= pl->maxx && pl->top[x] != WR_PLANE_UNUSED) {
                t2 = pl->top[x]; b2 = pl->bottom[x];
            } else { t2 = 0; b2 = -1; }
            wr_make_spans(pl, x, t1, b1, t2, b2);
            t1 = t2; b1 = b2;
        }
        }
    }
}

/* Colormap row for a sector's light level. The WAD's COLORMAP is ordered
 * brightest-first, and sector light runs 0 (dark) to 255 (bright), so the
 * mapping is inverted. */
static const WrPix *wr_sector_colormap(const WrSector *sec) {
    int light = (int) sec->light;
    if (light < 0) light = 0;
    if (light > 255) light = 255;
    int n = wr_s->wr_num_colormaps;
    int row = ((255 - light) * n) / 256;
    if (row < 0) row = 0;
    if (row >= n) row = n - 1;
    return (const WrPix *) wr_s->wr_colormap + (size_t) row * 256u;
}

static int wr_sector_light_row(const WrSector *sec) {
    int light = (int) sec->light;
    if (light < 0) light = 0;
    if (light > 255) light = 255;
    int n = wr_s->wr_num_colormaps;
    int row = ((255 - light) * n) / 256;
    if (row < 0) row = 0;
    if (row >= n) row = n - 1;
    return row;
}


/* DSP56001 offload hooks (Atari Falcon / videl-tos backend). The backend
 * defines WR_DSP_COLS before including this file and provides the three
 * functions; any column the DSP declines returns 0 and falls through to
 * the CPU path. */
#ifdef WR_DSP_COLS
int  wr_dsp_begin(int w, int h, unsigned char *chunky, int pitch,
                  int ox, int oy);
int  wr_dsp_column(int x, int yl, int yh, long iscale, long texturemid,
                   int tex_id, int tex_col, int tex_w, int tex_h,
                   const unsigned char *texdata,
                   const unsigned char *cmap, const unsigned char *cmap_base);
void wr_dsp_merge(unsigned char *chunky, int pitch, int ox, int oy,
                  int w, int h);
void wr_dsp_stats(int *alive, int *qsent, int *qcmds, int *qacked, int *merged);
int  wr_dsp_fail(void);
int  wr_dsp_isr(void);
void wr_dsp_dbg(int *pw, int *pisr, int *pcalls);
void wr_dsp_times(long *t);   /* drain, dumpx, scatx, dumpy, scaty (ms) */
#endif

/* Draws a single textured wall column. Split out so the one-sided,
 * upper and lower wall cases all share the same inner call. */
static void wr_wall_column(int x, int yl, int yh, wfixed_t iscale,
        wfixed_t dc_texturemid, int tex_id, wfixed_t tex_u,
        const WrPix *cmap) {
    if (wr_skip_mode == 2) return;   /* TEMP PROFILING: WRSKIP=walls */
    const unsigned char *data = NULL;
    int tex_w = 0, tex_h = 0;
    if (tex_id > 0 && tex_id < WR_MAX_TEXTURES) {
        data  = wr_s->wr_textures[tex_id].data;
        tex_w = wr_s->wr_textures[tex_id].w;
        tex_h = wr_s->wr_textures[tex_id].h;
    }
    if (!data || tex_w <= 0 || tex_h <= 0) {
        /* Wall geometry exists here but has no usable texture: fill
         * black so the claimed rows are still written (same result the
         * old full-screen clear produced). */
        wr_fill_column(wr_s, x, yl, yh, (WrPix) 0);
        return;
    }

    if (wr_s->wr_flatshade) {
        /* Flat-shade fast path: skip texture sampling and the horizontal-
         * wrap math below entirely - a solid-colour column needs neither. */
        WrPix color = cmap[wr_texture_avg_color(wr_s, tex_id)];
        wr_fill_column(wr_s, x, yl, yh, color);
        return;
    }

    /* Horizontal wrap: bitmask for power-of-two widths (all stock WAD
     * textures), divide only for the odd non-POT PWAD texture. */
    int tex_col;
    if ((tex_w & (tex_w - 1)) == 0) {
        tex_col = (int) (((unsigned) (tex_u >> WR_FRACBITS)) & ((unsigned) tex_w - 1u));
    } else {
        tex_col = (int) ((tex_u >> WR_FRACBITS) % tex_w);
        if (tex_col < 0) tex_col += tex_w;
    }
    if (wr_stripshade) {
        /* Stripe-shade: one cached colour per texture column, still
         * remapped through the sector colormap so lighting/darkening
         * works exactly as in the textured path. */
        WrPix color = cmap[wr_texture_col_avg(wr_s, tex_id, tex_col)];
        wr_fill_column(wr_s, x, yl, yh, color);
        return;
    }
#ifdef WR_DSP_COLS
    /* The DSP writes one physical pixel per logical pixel - it only
     * knows the full-res store, so scaled columns fall back to CPU. */
    if (wr_px_sx == 1 && wr_px_sy == 1 &&
        wr_dsp_column(x, yl, yh, iscale, dc_texturemid, tex_id, tex_col,
                      tex_w, tex_h, data, cmap, wr_s->wr_colormap))
        return;
#endif
    wr_draw_column(wr_s, x, yl, yh, iscale, dc_texturemid,
                   data + (size_t) tex_col * tex_h, tex_h, cmap);
}

/* A seg that survived transform/cull/screen-clip, reduced to the values
 * the per-column walk needs. Two producers feed this: the CPU walk
 * (wr_add_seg computes every field) and the DSP walk, where the Falcon's
 * DSP runs the same math and ships the span back as a compact stream
 * record (see wr_dsp_walk / wr_add_seg_from_rec). */
typedef struct {
    int side_idx;        /* sidedef the seg is drawn through            */
    int back_side_idx;   /* sidedef on the other side (-1 = one-sided)  */
    int csx1, csx2;      /* clipped screen-column span (inclusive)      */
    int sx1;             /* unclipped projection left edge (tex_u base) */
    wfixed_t scale1;     /* proj/vz interpolated to csx1                */
    wfixed_t scalestep;  /* d(scale)/column over the clipped span       */
    wfixed_t vz1_clip;   /* view z interpolated to csx1                 */
    wfixed_t vz_step;    /* d(vz)/column over the clipped span          */
    wfixed_t u_base;     /* texture U at the unclipped sx1              */
    wfixed_t u_step;     /* d(U)/column over the clipped span           */
    wfixed_t du_dvz;     /* d(U)/d(vz): perspective-true U, 0 = lerp u  */
    wfixed_t u_vz0;      /* vz at the u_base anchor (clipped v1)        */
    int ld_flags;        /* linedef flags (ML_DONTPEGBOTTOM for midtex) */
    /* Perspective-correct U for the C column loop: q = (U-u_base)*scale
     * is affine in screen-x, so U = u_base + q*iscale per column using
     * the iscale the loop already computes. Well conditioned at every
     * wall angle (no divide by the depth change), and anchored on the
     * sub-pixel projected endpoints rather than the truncated sx1/sx2.
     * uq_on = 0 (DSP replay records) keeps the older paths. */
    int      uq_on;
    wfixed_t u_anchor;   /* U where q = 0 (the nearer endpoint)         */
    int64_t  uq0;        /* q at csx1, 16.16 (units * scale)            */
    int64_t  uq_step;    /* dq/column, 16.16                            */
    /* scale at csx1 and d(scale)/column in 16.32: the generic column
     * loop steps scale with these so truncation does not accumulate
     * across the span (it feeds iscale, and through it texture U). */
    int64_t  scale1_x;
    int64_t  scalestep_x;
} WrSegSpan;

/* The per-column walk for one clipped seg: wall pieces, floor/ceiling
 * visplane extents, clip-array updates and the solidsegs close. This is
 * the part that stays on the host when the DSP does the geometry - the
 * draw calls and clip state live here either way.
 *
 * no-ivopts on m68k: without it, GCC's induction-variable pass spots the
 * world*scale / vz*inv_proj multiplies below and strength-reduces each
 * into a running int64 accumulator - ten 64-bit values that cannot fit
 * in registers and end up as ~10 memory ops each per column, which is
 * exactly the code this loop is written to avoid. */
#if defined(__m68k__) || defined(__M68000__)
#define WR_NOIV __attribute__((optimize("no-ivopts")))
#else
#define WR_NOIV
#endif
static WR_NOIV void wr_seg_columns(const WrSegSpan *sp) {
    const WrSidedef *sd = &wr_s->wr_sidedefs[sp->side_idx];
    int sec_idx = sd->sector;
    if (sec_idx < 0 || sec_idx >= wr_s->wr_n_sectors) return;
    const WrSector *sec = &wr_s->wr_sectors[sec_idx];

    const WrSector *back_sec = NULL;
    int back_sec_idx = -1;
    if (sp->back_side_idx >= 0 && sp->back_side_idx < wr_s->wr_n_sidedefs) {
        int bsi = wr_s->wr_sidedefs[sp->back_side_idx].sector;
        if (bsi >= 0 && bsi < wr_s->wr_n_sectors) {
            back_sec = &wr_s->wr_sectors[bsi];
            back_sec_idx = bsi;
        }
    }

    /* Texture IDs for this sidedef (mid / upper / lower). */
    int tex_mid = 0, tex_upper = 0, tex_lower = 0;
    if (wr_s->wr_texmap_set && wr_s->wr_sd_tex) {
        tex_mid   = wr_s->wr_sd_tex[sp->side_idx * 3 + 0];
        tex_upper = wr_s->wr_sd_tex[sp->side_idx * 3 + 1];
        tex_lower = wr_s->wr_sd_tex[sp->side_idx * 3 + 2];
    }

    /* Heights relative to the eye (constant along the seg). */
    wfixed_t worldtop    = (((wfixed_t) sec->ceil_h)  << WR_FRACBITS) - wr_view_y;
    wfixed_t worldbottom = (((wfixed_t) sec->floor_h) << WR_FRACBITS) - wr_view_y;
    wfixed_t worldhigh = 0, worldlow = 0;
    if (back_sec) {
        worldhigh = (((wfixed_t) back_sec->ceil_h)  << WR_FRACBITS) - wr_view_y;
        worldlow  = (((wfixed_t) back_sec->floor_h) << WR_FRACBITS) - wr_view_y;
    }

    /* Flat IDs for the front sector, plus the back sector's ceiling flat.
     * tex_id 0 means the script mapped that flat to F_SKY1. Resolved here,
     * ahead of the wall-piece decision below, which has to know whether
     * BOTH sides of a portal are open to the sky. */
    bool have_sec_tex = (wr_s->wr_texmap_set && wr_s->wr_sec_tex);
    int floor_tex = 0, ceil_tex = 0, back_ceil_tex = -1;
    if (have_sec_tex && sec_idx < wr_s->wr_n_sectors) {
        floor_tex = wr_s->wr_sec_tex[sec_idx * 2 + 0];
        ceil_tex  = wr_s->wr_sec_tex[sec_idx * 2 + 1];
    }
    if (have_sec_tex && back_sec_idx >= 0) {
        back_ceil_tex = wr_s->wr_sec_tex[back_sec_idx * 2 + 1];
    }

    /* One-sided lines are drawn from their mid texture and close the
     * column outright. Two-sided lines are drawn as their upper and/or
     * lower pieces instead - crucially INCLUDING closed ones (a shut door,
     * a window recess): those still occlude like a solid wall, but they
     * carry upper/lower textures rather than a mid texture, so treating
     * them as "solid, draw mid" closes the column and draws nothing,
     * leaving a full-height black band. */
    bool one_sided = (back_sec == NULL);
    bool solid = true;
    bool draw_upper = false, draw_lower = false;
    if (!one_sided) {
        draw_upper = (back_sec->ceil_h  < sec->ceil_h);
        draw_lower = (back_sec->floor_h > sec->floor_h);
        /* Degenerate opening => occludes like a wall, but the upper and
         * lower pieces are what actually get drawn (they meet/overlap). */
        solid = (back_sec->ceil_h <= back_sec->floor_h);

        /* Vanilla's "hack to allow height changes in outdoor areas"
         * (R_StoreWallRange): between two F_SKY1 ceilings there is NO
         * upper wall, however far apart the two ceilings are - the sky
         * simply continues down to the BACK ceiling.  worldtop = worldhigh
         * is exactly what vanilla does, and it is load-bearing: these
         * sidedefs legitimately carry no upper texture, so without it
         * draw_upper walks the ceiling clip down to the back ceiling and
         * then draws nothing into the band it just claimed, leaving a
         * black notch that appears and vanishes as you rotate (the wall
         * only claims those columns from some angles).  E1M1's outdoor
         * courtyard hits this on several segs. */
        if (!solid && ceil_tex == 0 && back_ceil_tex == 0) {
            worldtop = worldhigh;
            draw_upper = false;
        }
        /* A drawn upper/lower piece with no texture ("-") still claims
         * its rows. Route it to an out-of-range id so wr_wall_column
         * fills it black rather than leaving the rows unwritten (there
         * is no per-frame clear to hide them). */
        if (draw_upper && tex_upper <= 0) tex_upper = WR_MAX_TEXTURES;
        if (draw_lower && tex_lower <= 0) tex_lower = WR_MAX_TEXTURES;
    }

    int light_row = wr_sector_light_row(sec);
    const WrPix *cmap = wr_sector_colormap(sec);

    /* Floor/ceiling visplanes for the FRONT sector, only where they can
     * actually be seen (below / above the eye respectively) - except for
     * a SKY ceiling, which the classic engine always gives a plane
     * ("ceiling height > view z || ceiling flat is the sky flat").
     * The sky is infinitely far away, so it stays visible above the wall
     * tops even when the sector's nominal ceiling is below the eye;
     * gating it on worldtop > 0 left those columns unfilled. */
    bool sky_ceil = (have_sec_tex && ceil_tex == 0);
    WrVisPlane *floorplane = NULL, *ceilingplane = NULL;
    if (worldbottom < 0) floorplane   = wr_find_plane(sec->floor_h, floor_tex, light_row);
    if (worldtop > 0 || sky_ceil) ceilingplane = wr_find_plane(sec->ceil_h, ceil_tex, light_row);
    /* Plane check: this seg is about to write columns clip_sx1..clip_sx2;
     * if the shared plane already has ranges in any of them, split off a
     * fresh plane so the earlier ranges aren't overwritten. */
    int csx1 = sp->csx1, csx2 = sp->csx2;
    /* Coverage guarantee: every row this seg's columns are about to
     * claim must be written somewhere. Any slot the real plane lookup
     * could not fill (missing flat, sky disabled, overflow, or the
     * worldtop/worldbottom gates above) gets a void plane that records
     * the same ranges and draws black. */
    if (!floorplane)   floorplane   = wr_void_plane(csx1, csx2);
    if (!ceilingplane) ceilingplane = wr_void_plane(csx1, csx2);
    floorplane   = wr_check_plane(floorplane,   csx1, csx2);
    ceilingplane = wr_check_plane(ceilingplane, csx1, csx2);

    /* Vertical texture alignment. The classic convention pegs a one-sided/upper texture to
     * the ceiling and a lower texture to the floor unless the linedef
     * says otherwise; y_off shifts it. */
    wfixed_t y_off = ((wfixed_t) sd->y_off) << WR_FRACBITS;
    wfixed_t mid_texturemid   = worldtop + y_off;
    wfixed_t upper_texturemid = worldtop + y_off;
    wfixed_t lower_texturemid = (back_sec ? worldlow : worldbottom) + y_off;

    /* Masked midtexture on an open portal: deferred to the sprite pass
     * (wr_mtex records) so farther walls/floors still show through the
     * holes. Vertical anchor follows vanilla: normally the top of the
     * texture sits at the lower ceiling (the portal top); LINEDEF flag
     * 0x0010 (ML_DONTPEGBOTTOM) hangs the texture bottom from the higher
     * floor instead (midbars growing up from the floor). */
    wfixed_t midtex_tm = 0;
    bool mtex_ok = false;
    if (!one_sided && !solid && tex_mid > 0 && tex_mid < WR_MAX_TEXTURES
            && wr_skip_mode != 2 && !wr_s->wr_flatshade && !wr_stripshade) {
        int mh = wr_s->wr_textures[tex_mid].h;
        if (wr_s->wr_textures[tex_mid].data && mh > 0) {
            if (sp->ld_flags & 0x0010) {
                midtex_tm = (draw_lower ? worldlow : worldbottom)
                          + (((wfixed_t) mh) << WR_FRACBITS) + y_off;
            } else {
                midtex_tm = (draw_upper ? worldhigh : worldtop) + y_off;
            }
            mtex_ok = true;
        }
    }

    /* Incremental column stepping, as in the classic seg loop
     * (rw_topfrac += rw_topstep and friends). Two formulations:
     *
     * GENERIC: every per-column quantity below is world*scale — a
     * 32x32->64 multiply each, the largest chunk of arithmetic in the
     * seg loop — but `scale` is itself produced by repeated addition of
     * `scalestep`, so each product is exactly linear in the column index
     * and can be carried forward as an int64 accumulator with an add
     * instead:
     *
     *   acc(n) = worldtop*scale1 + n*(worldtop*scalestep)
     *          = worldtop*(scale1 + n*scalestep)
     *          = worldtop*scale
     *
     * Keeping each accumulator UNSHIFTED (the >> happens on the way out,
     * per column, exactly where it did before) is what makes this
     * bit-for-bit identical to the multiply it replaces rather than merely
     * close: no rounding is moved, and no error can accumulate, because
     * the shifted value is never fed back into the accumulator.
     *
     * M68K: ten int64 accumulator/step pairs cannot live in 15 registers,
     * so gcc keeps them on the stack and pays ~10 memory ops per
     * accumulator per column — while a 68030 muls.l yields the required
     * high product word directly in ~30 cycles. On that target the loop
     * carries an int32 `scale` accumulator and pays one 32x32 multiply
     * per column per quantity — the same arithmetic the pre-accumulator
     * version had — plus the same per-column divsl.l for iscale the
     * generic path uses. */
#if defined(__m68k__) || defined(__M68000__)
    wfixed_t col_scale = sp->scale1;
    const wfixed_t col_scalestep = sp->scalestep;
    int64_t col_sc32 = sp->scale1_x;   /* 16.32 twin of col_scale */
#else
    int64_t top_acc   = (int64_t) worldtop    * sp->scale1;
    int64_t top_step  = (int64_t) worldtop    * sp->scalestep;
    int64_t bot_acc   = (int64_t) worldbottom * sp->scale1;
    int64_t bot_step  = (int64_t) worldbottom * sp->scalestep;
    int64_t high_acc  = (int64_t) worldhigh   * sp->scale1;
    int64_t high_step = (int64_t) worldhigh   * sp->scalestep;
    int64_t low_acc   = (int64_t) worldlow    * sp->scale1;
    int64_t low_step  = (int64_t) worldlow    * sp->scalestep;
    /* scale (proj/vz) is affine in screen-x so it steps linearly; iscale
     * = 1/scale is recovered per column by divide instead - vz is
     * hyperbolic in screen-x, not linear, so it cannot be stepped.
     * The classic seg loop pays the same per-column FixedDiv. */
    int64_t sc_acc    = sp->scale1_x;                /* 16.32 */
    const int64_t sc_step = sp->scalestep_x;
#endif

    const wfixed_t u_step    = sp->u_step;
    const int      scrh      = wr_screen_h;
    const int      cols_live = (wr_skip_mode != 1);
    wfixed_t tex_u = sp->u_base + (wfixed_t) (((int64_t) u_step * (csx1 - sp->sx1)));

    int centery = scrh >> 1;
#if defined(__m68k__) || defined(__M68000__)
    /* Hand-scheduled m68k column loop. At -Os GCC keeps ~20 loop
     * invariants in a ~400-byte frame and spills heavily around the
     * wall/plane calls — ~1000 iterations/frame cost ~24 ms. Register
     * plan: d7=x d6=csx2 d5=col_scale d4=tex_u a5=&ctx (loop-carried);
     * d3=iscale a2=y_top d2=y_bot a4=cclip a3=fclip are callee-saved so
     * they survive the jsr calls; d0,d1,a0,a1 are scratch. Every
     * invariant and per-column temp lives in ctx at a fixed offset so
     * the asm needs no memory operands. */
    wr_col_init();
    const int run_c_loop = wr_no_asm_col;
    if (!run_c_loop && cols_live && csx1 <= csx2) {
        /* Flag bytes go LAST so the 4-byte fields all sit at multiples
         * of 4 under either the 4- or 2-byte int-alignment ABI. */
        struct wrcol_ctx {          /* offsets used by the asm below */
            wfixed_t mid_tm;        /*  0 */
            wfixed_t up_tm;         /*  4 */
            wfixed_t lo_tm;         /*  8 */
            wfixed_t mtex_tm;       /* 12 */
            int      tex_mid;       /* 16 */
            int      tex_upper;     /* 20 */
            int      tex_lower;     /* 24 */
            int      light_row;     /* 28 */
            const WrPix *cmap;      /* 32 */
            wfixed_t du_dvz;        /* 36 */
            wfixed_t u_base;        /* 40 */
            wfixed_t u_vz0;         /* 44 */
            wfixed_t proj_dist;     /* 48 */
            wfixed_t scalestep;     /* 52 */
            wfixed_t u_step;        /* 56 */
            int      centery;       /* 60 */
            int      scrh;          /* 64 (word access via 66) */
            WrVisPlane *cpl;        /* 68 */
            WrVisPlane *fpl;        /* 72 */
            wfixed_t worldtop;      /* 76 */
            wfixed_t worldbottom;   /* 80 */
            wfixed_t worldhigh;     /* 84 */
            wfixed_t worldlow;      /* 88 */
            int      yl;            /*  92 (per-column temps) */
            int      yh;            /*  96 */
            int      ncol;          /* 100 */
            int      nfloor;        /* 104 */
            wfixed_t col_u;         /* 108 */
            unsigned char f_one;    /* 112 */
            unsigned char f_solid;  /* 113 */
            unsigned char f_upper;  /* 114 */
            unsigned char f_lower;  /* 115 */
            unsigned char f_mtex;   /* 116 */
            unsigned char f_uq;     /* 117: perspective U (WrSegSpan.uq_on) */
            unsigned char pad_[2];  /* 118: keeps 120 4-aligned on both ABIs */
            unsigned long q_hi;     /* 120: q accumulator, 64-bit BE pair */
            unsigned long q_lo;     /* 124 */
            unsigned long qs_hi;    /* 128: dq/column */
            unsigned long qs_lo;    /* 132 */
            wfixed_t u_anchor;      /* 136 */
            unsigned long sfrac;    /* 140: low 16 bits of the 16.32 scale, << 16 */
            unsigned long sstepf;   /* 144: low 16 bits of the 16.32 step, << 16 */
        } ctx = {
            mid_texturemid, upper_texturemid, lower_texturemid,
            midtex_tm, tex_mid, tex_upper, tex_lower, light_row,
            cmap, sp->du_dvz, sp->u_base, sp->u_vz0,
            wr_proj_dist, col_scalestep, u_step, centery, scrh,
            ceilingplane, floorplane,
            worldtop, worldbottom, worldhigh, worldlow,
            0, 0, 0, 0, 0,
            (unsigned char) one_sided, (unsigned char) solid,
            (unsigned char) draw_upper, (unsigned char) draw_lower,
            (unsigned char) mtex_ok,
            (unsigned char) sp->uq_on, { 0, 0 },
            (unsigned long) ((uint64_t) sp->uq0 >> 32),
            (unsigned long) (uint64_t) sp->uq0,
            (unsigned long) ((uint64_t) sp->uq_step >> 32),
            (unsigned long) (uint64_t) sp->uq_step,
            sp->u_anchor,
            (unsigned long) ((sp->scale1_x & 0xFFFF) << 16),
            (unsigned long) ((sp->scalestep_x & 0xFFFF) << 16)
        };
        typedef char wrcol_ctx_offsets[
            (offsetof(struct wrcol_ctx, col_u)  == 108 &&
             offsetof(struct wrcol_ctx, f_mtex) == 116 &&
             offsetof(struct wrcol_ctx, f_uq)   == 117 &&
             offsetof(struct wrcol_ctx, q_hi)   == 120 &&
             offsetof(struct wrcol_ctx, u_anchor) == 136 &&
             offsetof(struct wrcol_ctx, sstepf) == 144) ? 1 : -1];
        /* The asm reaches these via jsr/symbol refs; address-taken asm
         * operands keep the static functions and arrays emitted now the
         * C references all live in the other #if branch. */
        __asm__("" :: "r"(&wr_wall_column), "r"(&wr_mtex_record),
                      "r"(wr_ceilingclip), "r"(wr_floorclip),
                      "r"(wr_clipseg_scale), "r"(wr_clipseg_top),
                      "r"(wr_clipseg_bot), "r"(&wrp_col_iters));
        register int ax     __asm__("d7") = csx1;
        register int aend   __asm__("d6") = csx2;
        register int ascale __asm__("d5") = col_scale;
        register int atexu  __asm__("d4") = tex_u;
        register struct wrcol_ctx *actx __asm__("a5") = &ctx;
        __asm__ __volatile__ (
".Lcol%=:\n\t"
"        addql   #1,_wrp_col_iters\n\t"
"        movew   _wr_ceilingclip(%%d7.l*2),%%d0\n\t"
"        extl    %%d0\n\t"
"        movew   _wr_floorclip(%%d7.l*2),%%d1\n\t"
"        extl    %%d1\n\t"
"        movel   %%d0,%%a4\n\t"
"        movel   %%d1,%%a3\n\t"
"        addql   #2,%%d0\n\t"
"        cmpl    %%d1,%%d0\n\t"
"        jgt     .Lnxt%=\n\t"
"        clrl    %%d3\n\t"
"        tstl    %%d5\n\t"
"        jle     1f\n\t"
"        clrl    %%d0\n\t"
"        moveq   #1,%%d1\n\t"
"        divsl   %%d5,%%d1:%%d0\n\t"
"        movel   %%d0,%%d3\n\t"
"1:\n\t"
"        movel   %%d5,%%d0\n\t"
"        muls.l  76(%%a5),%%d1:%%d0\n\t"
"        movel   60(%%a5),%%a2\n\t"
"        subl    %%d1,%%a2\n\t"
"        movel   %%d5,%%d0\n\t"
"        muls.l  80(%%a5),%%d1:%%d0\n\t"
"        movel   60(%%a5),%%d2\n\t"
"        subl    %%d1,%%d2\n\t"
"        movel   %%d4,108(%%a5)\n\t"
/* Perspective U: col_u = u_anchor + (q * iscale) >> 16, q a 64-bit
 * hi:lo pair, iscale (d3) >= 0. Low 32 bits of the 80-bit shift:
 * ((q_lo*isc) >> 16) + (low32(q_hi*isc) << 16) - bit-identical to the
 * C loop's int64 expression. */
"        tstb    117(%%a5)\n\t"
"        jeq     .Luqn%=\n\t"
"        movel   124(%%a5),%%d0\n\t"
"        mulu.l  %%d3,%%d1:%%d0\n\t"
"        swap    %%d0\n\t"
"        swap    %%d1\n\t"
"        movew   %%d0,%%d1\n\t"
"        movel   120(%%a5),%%d0\n\t"
"        muls.l  %%d3,%%d0\n\t"
"        swap    %%d0\n\t"
"        clrw    %%d0\n\t"
"        addl    %%d0,%%d1\n\t"
"        addl    136(%%a5),%%d1\n\t"
"        movel   %%d1,108(%%a5)\n\t"
"        jra     2f\n\t"
".Luqn%=:\n\t"
"        tstl    36(%%a5)\n\t"
"        jeq     2f\n\t"
"        movel   %%d3,%%d0\n\t"
"        cmpl    #8388608,%%d0\n\t"
"        jle     3f\n\t"
"        movel   #8388608,%%d0\n\t"
"3:      muls.l  48(%%a5),%%d1:%%d0\n\t"
"        swap    %%d0\n\t"
"        swap    %%d1\n\t"
"        movew   %%d0,%%d1\n\t"
"        subl    44(%%a5),%%d1\n\t"
"        movel   %%d1,%%d0\n\t"
"        muls.l  36(%%a5),%%d1:%%d0\n\t"
"        swap    %%d0\n\t"
"        swap    %%d1\n\t"
"        movew   %%d0,%%d1\n\t"
"        addl    40(%%a5),%%d1\n\t"
"        movel   %%d1,108(%%a5)\n\t"
"2:\n\t"
"        movel   %%a2,%%d0\n\t"
"        cmpl    %%a4,%%d0\n\t"
"        jgt     4f\n\t"
"        movel   %%a4,%%d0\n\t"
"        addql   #1,%%d0\n\t"
"4:      movel   %%d0,92(%%a5)\n\t"
"        movel   %%d2,%%d1\n\t"
"        cmpl    %%a3,%%d1\n\t"
"        jlt     5f\n\t"
"        movel   %%a3,%%d1\n\t"
"        subql   #1,%%d1\n\t"
"5:      movel   %%d1,96(%%a5)\n\t"
"        movel   68(%%a5),%%a0\n\t"
"        tstl    %%a0\n\t"
"        jeq     6f\n\t"
"        movel   %%a2,%%d0\n\t"
"        cmpl    %%a3,%%d0\n\t"
"        jle     7f\n\t"
"        movel   %%a3,%%d0\n\t"
"7:      subql   #1,%%d0\n\t"
"        movel   %%a4,%%d1\n\t"
"        addql   #1,%%d1\n\t"
"        cmpl    %%d0,%%d1\n\t"
"        jgt     6f\n\t"
"        movel   20(%%a0),%%a1\n\t"
"        movew   %%d1,0(%%a1,%%d7.l*2)\n\t"
"        movel   24(%%a0),%%a1\n\t"
"        movew   %%d0,0(%%a1,%%d7.l*2)\n\t"
"        cmpl    12(%%a0),%%d7\n\t"
"        jge     8f\n\t"
"        movel   %%d7,12(%%a0)\n\t"
"8:      cmpl    16(%%a0),%%d7\n\t"
"        jle     6f\n\t"
"        movel   %%d7,16(%%a0)\n\t"
"6:\n\t"
"        movel   72(%%a5),%%a0\n\t"
"        tstl    %%a0\n\t"
"        jeq     9f\n\t"
"        movel   %%d2,%%d1\n\t"
"        cmpl    %%a4,%%d1\n\t"
"        jge     10f\n\t"
"        movel   %%a4,%%d1\n\t"
"10:     addql   #1,%%d1\n\t"
"        movel   %%a3,%%d0\n\t"
"        subql   #1,%%d0\n\t"
"        cmpl    %%d0,%%d1\n\t"
"        jgt     9f\n\t"
"        movel   20(%%a0),%%a1\n\t"
"        movew   %%d1,0(%%a1,%%d7.l*2)\n\t"
"        movel   24(%%a0),%%a1\n\t"
"        movew   %%d0,0(%%a1,%%d7.l*2)\n\t"
"        cmpl    12(%%a0),%%d7\n\t"
"        jge     11f\n\t"
"        movel   %%d7,12(%%a0)\n\t"
"11:     cmpl    16(%%a0),%%d7\n\t"
"        jle     9f\n\t"
"        movel   %%d7,16(%%a0)\n\t"
"9:\n\t"
"        tstb    112(%%a5)\n\t"
"        jeq     .Ltwo%=\n\t"
"        movel   92(%%a5),%%d0\n\t"
"        cmpl    96(%%a5),%%d0\n\t"
"        jgt     12f\n\t"
"        movel   32(%%a5),-(%%sp)\n\t"
"        movel   108(%%a5),-(%%sp)\n\t"
"        movel   16(%%a5),-(%%sp)\n\t"
"        movel   0(%%a5),-(%%sp)\n\t"
"        movel   %%d3,-(%%sp)\n\t"
"        movel   96(%%a5),-(%%sp)\n\t"
"        movel   %%d0,-(%%sp)\n\t"
"        movel   %%d7,-(%%sp)\n\t"
"        jsr     _wr_wall_column\n\t"
"        lea     32(%%sp),%%sp\n\t"
"12:     movew   66(%%a5),_wr_ceilingclip(%%d7.l*2)\n\t"
"        movew   #-1,_wr_floorclip(%%d7.l*2)\n\t"
"        movel   64(%%a5),%%d0\n\t"
"        moveq   #-1,%%d1\n\t"
"        jbsr    .Lcs%=\n\t"
"        jra     .Lnxt%=\n\t"
".Ltwo%=:\n\t"
"        movel   %%a4,100(%%a5)\n\t"
"        movel   %%a3,104(%%a5)\n\t"
"        tstb    114(%%a5)\n\t"
"        jeq     .Lnoup%=\n\t"
"        movel   %%d5,%%d0\n\t"
"        muls.l  84(%%a5),%%d1:%%d0\n\t"
"        movel   60(%%a5),%%a0\n\t"
"        subl    %%d1,%%a0\n\t"
"        movel   %%a0,%%d0\n\t"
"        subql   #1,%%d0\n\t"
"        movel   100(%%a5),%%d1\n\t"
"        cmpl    %%d1,%%d0\n\t"
"        jle     13f\n\t"
"        movel   %%d0,100(%%a5)\n\t"
"13:     cmpl    %%a3,%%d0\n\t"
"        jlt     14f\n\t"
"        movel   %%a3,%%d0\n\t"
"        subql   #1,%%d0\n\t"
"14:     movel   92(%%a5),%%d1\n\t"
"        cmpl    %%d0,%%d1\n\t"
"        jgt     .Llow%=\n\t"
"        tstl    20(%%a5)\n\t"
"        jle     .Llow%=\n\t"
"        movel   32(%%a5),-(%%sp)\n\t"
"        movel   108(%%a5),-(%%sp)\n\t"
"        movel   20(%%a5),-(%%sp)\n\t"
"        movel   4(%%a5),-(%%sp)\n\t"
"        movel   %%d3,-(%%sp)\n\t"
"        movel   %%d0,-(%%sp)\n\t"
"        movel   %%d1,-(%%sp)\n\t"
"        movel   %%d7,-(%%sp)\n\t"
"        jsr     _wr_wall_column\n\t"
"        lea     32(%%sp),%%sp\n\t"
"        jra     .Llow%=\n\t"
".Lnoup%=:\n\t"
"        movel   %%a2,%%d0\n\t"
"        subql   #1,%%d0\n\t"
"        movel   100(%%a5),%%d1\n\t"
"        cmpl    %%d1,%%d0\n\t"
"        jle     .Llow%=\n\t"
"        movel   %%d0,100(%%a5)\n\t"
".Llow%=:\n\t"
"        tstb    115(%%a5)\n\t"
"        jeq     .Lnolow%=\n\t"
"        movel   %%d5,%%d0\n\t"
"        muls.l  88(%%a5),%%d1:%%d0\n\t"
"        movel   60(%%a5),%%a0\n\t"
"        subl    %%d1,%%a0\n\t"
"        movel   104(%%a5),%%d0\n\t"
"        cmpl    %%d0,%%a0\n\t"
"        jge     15f\n\t"
"        movel   %%a0,%%d0\n\t"
"        movel   %%d0,104(%%a5)\n\t"
"15:     movel   %%a0,%%d1\n\t"
"        cmpl    %%a4,%%d1\n\t"
"        jgt     16f\n\t"
"        movel   %%a4,%%d1\n\t"
"        addql   #1,%%d1\n\t"
"16:     movel   96(%%a5),%%d0\n\t"
"        cmpl    %%d0,%%d1\n\t"
"        jgt     .Lsol%=\n\t"
"        tstl    24(%%a5)\n\t"
"        jle     .Lsol%=\n\t"
"        movel   32(%%a5),-(%%sp)\n\t"
"        movel   108(%%a5),-(%%sp)\n\t"
"        movel   24(%%a5),-(%%sp)\n\t"
"        movel   8(%%a5),-(%%sp)\n\t"
"        movel   %%d3,-(%%sp)\n\t"
"        movel   %%d0,-(%%sp)\n\t"
"        movel   %%d1,-(%%sp)\n\t"
"        movel   %%d7,-(%%sp)\n\t"
"        jsr     _wr_wall_column\n\t"
"        lea     32(%%sp),%%sp\n\t"
"        jra     .Lsol%=\n\t"
".Lnolow%=:\n\t"
"        movel   %%d2,%%d1\n\t"
"        addql   #1,%%d1\n\t"
"        cmpl    104(%%a5),%%d1\n\t"
"        jge     .Lsol%=\n\t"
"        movel   %%d1,104(%%a5)\n\t"
".Lsol%=:\n\t"
"        tstb    113(%%a5)\n\t"
"        jeq     .Lopen%=\n\t"
"        tstb    114(%%a5)\n\t"
"        jne     17f\n\t"
"        tstb    115(%%a5)\n\t"
"        jne     17f\n\t"
"        movel   92(%%a5),%%d0\n\t"
"        cmpl    96(%%a5),%%d0\n\t"
"        jgt     17f\n\t"
"        movel   32(%%a5),-(%%sp)\n\t"
"        movel   108(%%a5),-(%%sp)\n\t"
"        movel   16(%%a5),-(%%sp)\n\t"
"        movel   0(%%a5),-(%%sp)\n\t"
"        movel   %%d3,-(%%sp)\n\t"
"        movel   96(%%a5),-(%%sp)\n\t"
"        movel   %%d0,-(%%sp)\n\t"
"        movel   %%d7,-(%%sp)\n\t"
"        jsr     _wr_wall_column\n\t"
"        lea     32(%%sp),%%sp\n\t"
"17:     movew   66(%%a5),_wr_ceilingclip(%%d7.l*2)\n\t"
"        movew   #-1,_wr_floorclip(%%d7.l*2)\n\t"
"        movel   64(%%a5),%%d0\n\t"
"        moveq   #-1,%%d1\n\t"
"        jbsr    .Lcs%=\n\t"
"        jra     .Lnxt%=\n\t"
".Lopen%=:\n\t"
"        movel   100(%%a5),%%a2\n\t"
"        cmpl    #-1,%%a2\n\t"
"        jge     18f\n\t"
"        movel   #-1,%%a2\n\t"
"18:     movel   104(%%a5),%%d2\n\t"
"        cmpl    64(%%a5),%%d2\n\t"
"        jle     19f\n\t"
"        movel   64(%%a5),%%d2\n\t"
"19:     movew   %%a2,_wr_ceilingclip(%%d7.l*2)\n\t"
"        movew   %%d2,_wr_floorclip(%%d7.l*2)\n\t"
"        movel   %%a2,%%d0\n\t"
"        movel   %%d2,%%d1\n\t"
"        jbsr    .Lcs%=\n\t"
"        tstb    116(%%a5)\n\t"
"        jeq     .Lnxt%=\n\t"
"        lea     1(%%a2),%%a0\n\t"
"        movel   %%d2,%%d1\n\t"
"        subql   #1,%%d1\n\t"
"        cmpl    %%d1,%%a0\n\t"
"        jgt     .Lnxt%=\n\t"
"        movel   28(%%a5),-(%%sp)\n\t"
"        movel   16(%%a5),-(%%sp)\n\t"
"        movel   %%d1,-(%%sp)\n\t"
"        movel   %%a0,-(%%sp)\n\t"
"        movel   108(%%a5),-(%%sp)\n\t"
"        movel   12(%%a5),-(%%sp)\n\t"
"        movel   %%d3,-(%%sp)\n\t"
"        movel   %%d7,-(%%sp)\n\t"
"        jsr     _wr_mtex_record\n\t"
"        lea     32(%%sp),%%sp\n\t"
".Lnxt%=:\n\t"
"        addql   #1,%%d7\n\t"
/* 16.32 scale step: fraction add carries into d5 through X (move
 * leaves X alone). Then the 64-bit q step the same way. */
"        movel   144(%%a5),%%d0\n\t"
"        addl    %%d0,140(%%a5)\n\t"
"        movel   52(%%a5),%%d0\n\t"
"        addxl   %%d0,%%d5\n\t"
"        movel   132(%%a5),%%d0\n\t"
"        addl    %%d0,124(%%a5)\n\t"
"        movel   128(%%a5),%%d0\n\t"
"        movel   120(%%a5),%%d1\n\t"
"        addxl   %%d0,%%d1\n\t"
"        movel   %%d1,120(%%a5)\n\t"
"        addl    56(%%a5),%%d4\n\t"
"        cmpl    %%d6,%%d7\n\t"
"        jle     .Lcol%=\n\t"
"        jra     .Ldone%=\n\t"
".Lcs%=:\n\t"
"        movel   %%d2,-(%%sp)\n\t"
"        lea     _wr_clipseg_n,%%a0\n\t"
"        clrl    %%d2\n\t"
"        moveb   0(%%a0,%%d7.l),%%d2\n\t"
"        cmpb    #8,%%d2\n\t"
"        jcc     20f\n\t"
"        addqb   #1,0(%%a0,%%d7.l)\n\t"
"        movel   %%d7,%%a0\n\t"
"        addl    %%a0,%%a0\n\t"
"        addl    %%a0,%%a0\n\t"
"        addl    %%a0,%%a0\n\t"
"        addl    %%d2,%%a0\n\t"
"        lea     _wr_clipseg_scale,%%a1\n\t"
"        movel   %%d3,0(%%a1,%%a0.l*4)\n\t"
"        lea     _wr_clipseg_top,%%a1\n\t"
"        movew   %%d0,0(%%a1,%%a0.l*2)\n\t"
"        lea     _wr_clipseg_bot,%%a1\n\t"
"        movew   %%d1,0(%%a1,%%a0.l*2)\n\t"
"20:     movel   (%%sp)+,%%d2\n\t"
"        rts\n\t"
".Ldone%=:\n\t"
            : [x]"+r"(ax), [scale]"+r"(ascale), [texu]"+r"(atexu)
            : [end]"r"(aend), [ctx]"r"(actx)
            : "d0","d1","d2","d3","a0","a1","a2","a3","a4",
              "cc","memory");
    }
#endif
    /* C column loop: the only path on non-m68k targets; on m68k it runs
     * when WRCOL.DAT is present (A/B comparison and fallback). */
#if !(defined(__m68k__) || defined(__M68000__))
    const int run_c_loop = 1;
#endif
    {
        int x;
        int64_t uq = sp->uq0;
        for (x = csx1; x <= csx2 && cols_live && run_c_loop; x++, uq += sp->uq_step,
#if defined(__m68k__) || defined(__M68000__)
            col_sc32 += sp->scalestep_x,
            col_scale = (wfixed_t) (col_sc32 >> WR_FRACBITS),
#else
            top_acc  += top_step,  bot_acc += bot_step,
            high_acc += high_step, low_acc += low_step,
            sc_acc   += sc_step,
#endif
            tex_u    += u_step) {
        wrp_col_iters++;
        /* Load the clip pair once. Both are read up to six times in this
         * body, and because wr_plane_add_column and wr_wall_column write
         * through pointers the compiler cannot prove don't alias these
         * file-scope arrays, every read after a call would otherwise
         * reload from memory. Neither callee touches ceilingclip/floorclip
         * (wr_wall_column only reaches the framebuffer via
         * wr_draw_column/wr_fill_column), so caching is safe; the
         * write-backs at the end of the body still go to the arrays. */
        int cclip = wr_ceilingclip[x];
        int fclip = wr_floorclip[x];

        /* Column already fully closed by nearer geometry? */
        if (cclip + 1 > fclip - 1) continue;

#if defined(__m68k__) || defined(__M68000__)
        wfixed_t iscale = col_scale > 0
            ? wr_div64(((int64_t) 1) << (2 * WR_FRACBITS), col_scale) : 0;

        int y_top = centery - (int) (((int64_t) worldtop    * col_scale) >> (WR_FRACBITS * 2));
        int y_bot = centery - (int) (((int64_t) worldbottom * col_scale) >> (WR_FRACBITS * 2));
#else
        wfixed_t scale_x = (wfixed_t) (sc_acc >> WR_FRACBITS);
        wfixed_t iscale = scale_x > 0
            ? wr_div64(((int64_t) 1) << (2 * WR_FRACBITS), scale_x) : 0;

        int y_top = centery - (int) (top_acc >> (WR_FRACBITS * 2));
        int y_bot = centery - (int) (bot_acc >> (WR_FRACBITS * 2));
#endif
        wfixed_t col_u = tex_u;
        if (sp->uq_on) {
            /* Perspective-correct U: q is affine in x, iscale = 1/scale. */
            col_u = sp->u_anchor + (wfixed_t) ((uq * (int64_t) iscale) >> WR_FRACBITS);
        } else if (sp->du_dvz) {
            /* Perspective-true texture column: u is affine in vz, and
             * vz = proj/scale = iscale*proj is exact here. Clamp the
             * reciprocal at vz ~ 29000 map units: iscale is also the
             * texture step, but at that depth every texel spans the
             * screen anyway - the bound only keeps the i64 products
             * below from overflowing when scale_x sits near zero. */
            wfixed_t isc_c = iscale > (1 << 23) ? (1 << 23) : iscale;
            wfixed_t vzx = (wfixed_t) (((int64_t) isc_c * wr_proj_dist)
                                       >> WR_FRACBITS);
            col_u = sp->u_base
                  + (wfixed_t) (((int64_t) sp->du_dvz
                                 * (vzx - sp->u_vz0)) >> WR_FRACBITS);
        }

        /* Clip the wall's own extent against what is already covered. */
        int yl = y_top, yh = y_bot;
        if (yl <= cclip) yl = cclip + 1;
        if (yh >= fclip) yh = fclip - 1;

        /* Ceiling plane fills from the current ceiling clip down to the
         * top of the wall; floor plane from the wall's bottom down to the
         * floor clip. This is why planes cost nothing extra to discover -
         * they are exactly the gaps the walls leave behind. */
        if (ceilingplane) {
            int t = cclip + 1;
            int b = (y_top < fclip ? y_top : fclip) - 1;
            wr_plane_add_column(ceilingplane, x, t, b);
        }
        if (floorplane) {
            int t = (y_bot > cclip ? y_bot : cclip) + 1;
            int b = fclip - 1;
            wr_plane_add_column(floorplane, x, t, b);
        }

        if (one_sided) {
            if (yl <= yh) {
                wr_wall_column(x, yl, yh, iscale, mid_texturemid, tex_mid, col_u, cmap);
            }
            /* Fully closed from here on. */
            wr_ceilingclip[x] = (short) scrh;
            wr_floorclip[x]   = -1;
            wr_clipseg_record(x, iscale, scrh, -1);
            continue;
        }

        /* ---- Two-sided line: draw the upper and lower pieces, then
         * either narrow the column (open portal) or close it (a shut
         * door - still drawn from upper/lower, never from a mid). ---- */
        int new_ceil = cclip;
        int new_floor = fclip;

        if (draw_upper) {
#if defined(__m68k__) || defined(__M68000__)
            int y_high = centery - (int) (((int64_t) worldhigh * col_scale) >> (WR_FRACBITS * 2));
#else
            int y_high = centery - (int) (high_acc >> (WR_FRACBITS * 2));
#endif
            int ul = yl;
            int uh = y_high - 1;
            if (uh >= fclip) uh = fclip - 1;
            if (ul <= uh && tex_upper > 0) {
                wr_wall_column(x, ul, uh, iscale, upper_texturemid, tex_upper, col_u, cmap);
            }
            if (y_high - 1 > new_ceil) new_ceil = y_high - 1;
        } else if (y_top - 1 > new_ceil) {
            new_ceil = y_top - 1;
        }

        if (draw_lower) {
#if defined(__m68k__) || defined(__M68000__)
            int y_low = centery - (int) (((int64_t) worldlow * col_scale) >> (WR_FRACBITS * 2));
#else
            int y_low = centery - (int) (low_acc >> (WR_FRACBITS * 2));
#endif
            int ll = y_low;
            int lh = yh;
            if (ll <= cclip) ll = cclip + 1;
            if (ll <= lh && tex_lower > 0) {
                wr_wall_column(x, ll, lh, iscale, lower_texturemid, tex_lower, col_u, cmap);
            }
            if (y_low < new_floor) new_floor = y_low;
        } else if (y_bot + 1 < new_floor) {
            /* The floor plane covers y_bot+1 down, so row y_bot stays
             * open for the sector behind (vanilla: floorclip = yh+1).
             * Closing it at y_bot left a never-written seam row. */
            new_floor = y_bot + 1;
        }

        if (solid) {
            /* Closed opening: occlude fully, now that upper/lower have
             * been drawn over the whole span. If the linedef somehow has
             * neither piece, fall back to the mid texture so the column
             * never ends up closed-but-blank. */
            if (!draw_upper && !draw_lower && yl <= yh) {
                wr_wall_column(x, yl, yh, iscale, mid_texturemid, tex_mid, col_u, cmap);
            }
            wr_ceilingclip[x] = (short) scrh;
            wr_floorclip[x]   = -1;
            wr_clipseg_record(x, iscale, scrh, -1);
        } else {
            if (new_ceil < -1) new_ceil = -1;
            if (new_floor > scrh) new_floor = scrh;
            wr_ceilingclip[x] = (short) new_ceil;
            wr_floorclip[x]   = (short) new_floor;
            wr_clipseg_record(x, iscale, new_ceil, new_floor);
            if (mtex_ok && new_ceil + 1 <= new_floor - 1) {
                wr_mtex_record(x, iscale, midtex_tm, col_u,
                               new_ceil + 1, new_floor - 1,
                               tex_mid, light_row);
            }
        }
        }
    }

    /* Only a genuinely solid wall may close the range for the BSP
     * early-out; a portal must leave it open so rooms behind it still get
     * visited. */
    if (solid) wr_solid_add(csx1, csx2);
}

/* Processes one seg: view-space transform, back-face cull, near clip,
 * projection, then a per-column walk that draws the wall pieces, records
 * floor/ceiling visplane extents, and updates the clip arrays.
 *
 * Every divide is per-seg or per-row - never per pixel. */
#ifdef WR_DSP_COLS
#define WRCL(r) do{ if(wr_cull_log()) { fprintf(wr_cull_log(), \
    "seg %d %s vx1 %ld vz1 %ld vx2 %ld vz2 %ld\n", \
    (int)(seg-wr_s->wr_segs),(r),(long)vx1,(long)vz1,(long)vx2,(long)vz2); \
    fflush(wr_cull_log()); } }while(0)
#else
#define WRCL(r) do{}while(0)
#endif
static void wr_add_seg(const WrSeg *seg) {
    if (seg->v1 < 0 || seg->v1 >= wr_s->wr_n_vertices) return;
    if (seg->v2 < 0 || seg->v2 >= wr_s->wr_n_vertices) return;
    const WrVertex *wv1 = &wr_s->wr_vertices[seg->v1];
    const WrVertex *wv2 = &wr_s->wr_vertices[seg->v2];

    if (seg->linedef < 0 || seg->linedef >= wr_s->wr_n_linedefs) return;
    const WrLinedef *ld = &wr_s->wr_linedefs[seg->linedef];
    int side_idx = (seg->side == 1) ? ld->back : ld->front;
    int back_side_idx = (seg->side == 1) ? ld->front : ld->back;
    if (side_idx < 0 || side_idx >= wr_s->wr_n_sidedefs) return;
    const WrSidedef *sd = &wr_s->wr_sidedefs[side_idx];

    wr_skip_init();
    if (wr_skip_mode == 3) return;   /* TEMP PROFILING: traversal only */

    /* ---- World -> view space ---- */
    wfixed_t dx1 = (((wfixed_t) wv1->x) << WR_FRACBITS) - wr_view_x;
    wfixed_t dz1 = (((wfixed_t) wv1->y) << WR_FRACBITS) - wr_view_z;
    wfixed_t dx2 = (((wfixed_t) wv2->x) << WR_FRACBITS) - wr_view_x;
    wfixed_t dz2 = (((wfixed_t) wv2->y) << WR_FRACBITS) - wr_view_z;
    /* View transform: forward = (sin a, cos a), right = (cos a, -sin a).
     *   vx = dx*cos(a) - dz*sin(a)   (right component)
     *   vz = dx*sin(a) + dz*cos(a)   (forward component)
     * At angle 0: vx = dx, vz = dz.  Player faces +Z (north), +X (east)
     * is to the right.  Positive vx maps to the right on screen. */
    wfixed_t vx1 = (wfixed_t) (((int64_t) dx1 * wr_view_cos - (int64_t) dz1 * wr_view_sin) >> WR_FRACBITS);
    wfixed_t vz1 = (wfixed_t) (((int64_t) dx1 * wr_view_sin + (int64_t) dz1 * wr_view_cos) >> WR_FRACBITS);
    wfixed_t vx2 = (wfixed_t) (((int64_t) dx2 * wr_view_cos - (int64_t) dz2 * wr_view_sin) >> WR_FRACBITS);
    wfixed_t vz2 = (wfixed_t) (((int64_t) dx2 * wr_view_sin + (int64_t) dz2 * wr_view_cos) >> WR_FRACBITS);

    /* Back-face cull: only ever drawn from the seg's front side.
     *
     * WAD segs have their front face on the RIGHT side of the v1->v2
     * direction.  When the viewer is on the front side (inside the
     * subsector), the seg goes left-to-right in view space (v1 on the
     * left, v2 on the right), which makes the 2D cross product of the
     * position vectors NEGATIVE:  cross = vx1*vz2 - vx2*vz1 < 0.
     * Back-facing segs (viewer behind) go right-to-left and give
     * cross > 0.  So we cull when cross >= 0.
     *
     * The comparison MUST stay in 64-bit: vx and vz are 16.16, so the
     * product is 32.32 and routinely exceeds int32 range.  Truncating to
     * wfixed_t (as an earlier version did) wrapped large cross products
     * to the opposite sign, randomly drawing backfaces and culling front
     * faces depending on seg size/distance. */
    if ((int64_t) vx1 * vz2 - (int64_t) vx2 * vz1 >= 0) { WRCL("bf"); return; }

    /* Track how much of the seg's U range the near clip eats, so texture
     * alignment stays correct on walls that cross the view plane. */
    wfixed_t u_frac_start = 0, u_frac_end = WR_FRACUNIT;
    const wfixed_t NEARZ = WR_FRACUNIT * 4;
    if (vz1 < NEARZ && vz2 < NEARZ) { WRCL("near"); return; }
    if (vz1 < NEARZ) {
        wfixed_t t = wr_div64(((int64_t) (NEARZ - vz1) << WR_FRACBITS), vz2 - vz1);
        vx1 += (wfixed_t) (((int64_t) (vx2 - vx1) * t) >> WR_FRACBITS);
        vz1 = NEARZ;
        u_frac_start = t;
    } else if (vz2 < NEARZ) {
        wfixed_t t = wr_div64(((int64_t) (NEARZ - vz2) << WR_FRACBITS), vz1 - vz2);
        vx2 += (wfixed_t) (((int64_t) (vx1 - vx2) * t) >> WR_FRACBITS);
        vz2 = NEARZ;
        u_frac_end = WR_FRACUNIT - t;
    }

    int centerx = wr_screen_w >> 1;
    int centery = wr_screen_h >> 1;
    /* A nearly edge-on seg can make vx*proj/vz exceed int32. The 68020+
     * divsl.l used by wr_div64 leaves its destination UNCHANGED on
     * overflow, so the projection cannot simply be computed anyway - it
     * would come from an arbitrary register value. The test below is the
     * cheap equivalent of |q| >= 2^31 (a 64-bit shift and compare, no
     * divide).
     *
     * Such a seg is NOT degenerate, though: it is a wall right beside the
     * eye, running away at a steep angle, and the part of it that crosses
     * the screen is perfectly ordinary. Rejecting it outright dropped that
     * wall and left a black band down the side of the screen (the band
     * comes and goes as you turn, because the overflow depends on the
     * viewing angle). So instead clip the seg against the two side planes
     * of the frustum in VIEW space, which bounds |vx/vz| and therefore
     * makes the projection representable.
     *
     * The clip runs ONLY on the overflow path, so every seg that projects
     * normally keeps bit-identical output - which matters because the
     * Falcon DSP walk (WR_DSP_COLS) reproduces this function's
     * arithmetic exactly. The DSP still culls on overflow; those segs are
     * simply absent from its record stream, i.e. that backend keeps the
     * old behaviour on them until sg_proj in the DSP build learns the same
     * clip. */
    {
        int64_t n1 = (int64_t) vx1 * wr_proj_dist;
        int64_t n2 = (int64_t) vx2 * wr_proj_dist;
        int64_t h1 = (n1 < 0 ? -n1 : n1) >> 31, h2 = (n2 < 0 ? -n2 : n2) >> 31;
        int64_t d1 = vz1 < 0 ? -(int64_t) vz1 : (int64_t) vz1;
        int64_t d2 = vz2 < 0 ? -(int64_t) vz2 : (int64_t) vz2;
        if (h1 >= d1 || h2 >= d2) {
            /* Side plane at screen offset S from centerx is the set of
             * view-space points with vx*proj == (S<<16)*vz; f is negative
             * on the inboard side of the right plane and (after the sign
             * flip) of the left plane too. The planes sit one column
             * outside the screen: the projection truncates, and sx2 is
             * then made exclusive (-1 below), so a seg clipped exactly to
             * the edge would lose its last on-screen column. */
            int i;
            for (i = 0; i < 2; i++) {
                int S = (i == 0) ? (wr_screen_w - centerx + 1) : -(centerx + 1);
                int64_t k = ((int64_t) S) << WR_FRACBITS;
                int64_t f1 = (int64_t) vx1 * wr_proj_dist - k * vz1;
                int64_t f2 = (int64_t) vx2 * wr_proj_dist - k * vz2;
                if (i == 1) { f1 = -f1; f2 = -f2; }
                if (f1 <= 0 && f2 <= 0) continue;        /* fully inboard */
                if (f1 > 0 && f2 > 0) { WRCL("frustum"); return; }
                /* t = f1 / (f1 - f2) as 16.16. f1/f2 are 32.32-scale
                 * products (up to ~2^56), so neither f1 << 16 nor a
                 * 32-bit divisor is representable: bring both down to a
                 * common scale first. The intersection ratio is scale-
                 * invariant, so this costs nothing but low bits that are
                 * far below one screen column. */
                int64_t dfm = f1 - f2;
                int64_t mag = dfm < 0 ? -dfm : dfm;
                int sh = 0;
                while ((mag >> sh) > 0x3FFFFFFFLL) sh++;    /* divisor into 31 bits */
                int64_t tn = (f1 >> sh) << WR_FRACBITS;     /* |f1| <= |dfm| here */
                wfixed_t t = wr_div64(tn, (int) (dfm >> sh));
                wfixed_t nx = vx1 + (wfixed_t) (((int64_t) (vx2 - vx1) * t) >> WR_FRACBITS);
                wfixed_t nz = vz1 + (wfixed_t) (((int64_t) (vz2 - vz1) * t) >> WR_FRACBITS);
                wfixed_t nu = u_frac_start +
                    (wfixed_t) (((int64_t) (u_frac_end - u_frac_start) * t) >> WR_FRACBITS);
                if (f1 > 0) { vx1 = nx; vz1 = nz; u_frac_start = nu; }
                else        { vx2 = nx; vz2 = nz; u_frac_end   = nu; }
            }
            /* The side planes pass through the eye, so an endpoint can land
             * marginally inside the near plane; the near clip above already
             * fixed the depth range, so just re-floor it. */
            if (vz1 < NEARZ) vz1 = NEARZ;
            if (vz2 < NEARZ) vz2 = NEARZ;
            n1 = (int64_t) vx1 * wr_proj_dist;
            n2 = (int64_t) vx2 * wr_proj_dist;
            h1 = (n1 < 0 ? -n1 : n1) >> 31; h2 = (n2 < 0 ? -n2 : n2) >> 31;
            d1 = vz1 < 0 ? -(int64_t) vz1 : (int64_t) vz1;
            d2 = vz2 < 0 ? -(int64_t) vz2 : (int64_t) vz2;
            if (h1 >= d1 || h2 >= d2) { WRCL("ovf"); return; }
        }
    }
    /* Sub-pixel projected endpoints (16.16 offset from centerx), kept for
     * the perspective texture-U setup below; sx1/sx2 truncate them. */
    wfixed_t px1 = wr_div64((int64_t) vx1 * wr_proj_dist, vz1);
    wfixed_t px2 = wr_div64((int64_t) vx2 * wr_proj_dist, vz2);
    int sx1 = centerx + (int) (px1 >> WR_FRACBITS);
    int sx2 = centerx + (int) (px2 >> WR_FRACBITS);

    /* With the (64-bit) backface cull above, a front-facing seg with both
     * endpoints at vz >= NEARZ always projects with sx1 < sx2 (projection
     * is monotonic in view angle).  sx1 >= sx2 can only be a zero-width
     * sliver after rounding - drop it. */
    if (sx1 >= sx2) { WRCL("slv"); return; }
    if (sx2 < 0 || sx1 >= wr_screen_w) { WRCL("off"); return; }
    sx2 -= 1;                            /* right edge is exclusive */
    if (sx2 < sx1) { WRCL("slv2"); return; }
    wrp_segs_drawn++;
    WRCL("draw");

    /* Clip the screen span to the actual visible columns BEFORE computing
     * the per-column scale/depth step.  Without this, a seg that projects
     * to a huge off-screen range (e.g. sx1=-11113, sx2=7776) makes
     * full_span enormous, and the integer division (scale2-scale1)/full_span
     * loses most of its precision.  The accumulated error over the on-screen
     * columns then produces wrong wall heights, wrong texture V, and black
     * gaps where the wall should be.  Clipping first keeps full_span small
     * (at most screen_w) so the step precision is maximised. */
    int clip_sx1 = sx1 < 0 ? 0 : sx1;
    int clip_sx2 = sx2 >= wr_screen_w ? wr_screen_w - 1 : sx2;
    if (clip_sx1 > clip_sx2) { WRCL("clip"); return; }

    /* Recompute scale/vz at the clipped endpoints by interpolating from
     * the original endpoints.  scale and vz are linear in screen-x, so
     * the value at screen column x is  s(x) = s1 + (s2-s1)*(x-sx1)/(sx2-sx1).
     * We compute the clipped-endpoint values in 64-bit to avoid overflow. */
    int orig_span = sx2 - sx1;
    wfixed_t scale1_full = wr_div64(((int64_t) wr_proj_dist) << WR_FRACBITS, vz1);
    wfixed_t scale2_full = wr_div64(((int64_t) wr_proj_dist) << WR_FRACBITS, vz2);
    wfixed_t scale1, scale2;
    /* Anchor the ramp on the SUB-PIXEL projected endpoints px1/px2 and
     * sample at column centres, rather than on the truncated sx1/sx2:
     * the truncation shifted the whole ramp by up to a column, which
     * the perspective texture U (q * iscale below) turns directly into
     * texels sliding across the wall as the edges step between columns. */
    int64_t dxf = (int64_t) px2 - px1;                  /* 16.16 columns */
    int64_t ds  = (int64_t) scale2_full - scale1_full;
    int64_t sstep32 = 0;                                /* 16.32 / column */
    int64_t s1_32 = (int64_t) scale1_full << WR_FRACBITS;
    if (dxf > 0) {
        int64_t off1 = ((int64_t) (clip_sx1 - centerx) << WR_FRACBITS)
                     + (WR_FRACUNIT >> 1) - px1;
        sstep32 = (ds << (2 * WR_FRACBITS)) / dxf;
        s1_32  += (sstep32 * off1) >> WR_FRACBITS;
    }
    if (s1_32 < WR_FRACUNIT) s1_32 = WR_FRACUNIT;
    scale1 = (wfixed_t) (s1_32 >> WR_FRACBITS);
    {
        int64_t s2_32 = s1_32 + sstep32 * (clip_sx2 - clip_sx1);
        if (s2_32 < WR_FRACUNIT) s2_32 = WR_FRACUNIT;
        scale2 = (wfixed_t) (s2_32 >> WR_FRACBITS);
    }
    wfixed_t scalestep_x = (wfixed_t) (sstep32 >> WR_FRACBITS);
    /* scale = proj/vz IS affine in screen-x, so the interpolation above
     * is exact. vz itself is NOT: for a projected line it is a Möbius
     * function of screen-x (a shallow hyperbola). Interpolating vz
     * linearly underestimates it by up to an order of magnitude on walls
     * running past the eye - exactly the "approaching a corner" case -
     * which made iscale (and texture U anchored on it) explode into
     * giant flat-colour texels. Recover vz from the interpolated scale:
     * vz = proj/scale, two divides per seg instead of two lerps. */
    wfixed_t vz1_clip = scale1 > 0
        ? wr_div64(((int64_t) wr_proj_dist) << WR_FRACBITS, scale1) : vz1;
    wfixed_t vz2_clip = scale2 > 0
        ? wr_div64(((int64_t) wr_proj_dist) << WR_FRACBITS, scale2) : vz2;

    int full_span = clip_sx2 - clip_sx1;
    wfixed_t scalestep = full_span > 0 ? scalestep_x : 0;
    wfixed_t vz_step   = full_span > 0 ? (vz2_clip - vz1_clip) / full_span : 0;

    /* Texture U: seg->offset is how far along the linedef this seg starts,
     * sd->x_off shifts the texture along the wall. U advances linearly in
     * screen space (the classic approximation). seg_len is precomputed once
     * at level load (see wad.tgx) so we avoid a per-seg-per-frame sqrt. */
    int seg_len = (int) seg->length;
    wfixed_t u_base = (((wfixed_t) ((int) seg->offset + (int) sd->x_off)) << WR_FRACBITS);
    /* Visible length in world units after the near clip ate u_frac_start
     * .. u_frac_end of the seg. */
    wfixed_t u_span = (wfixed_t) (((int64_t) seg_len * (u_frac_end - u_frac_start)) >> WR_FRACBITS);
    u_base += (wfixed_t) ((int64_t) seg_len * u_frac_start);   /* int * 16.16 fraction = 16.16 */
    /* u_span is spread over the seg's FULL projected width (orig_span),
     * NOT the on-screen part: u_base is the U at the unclipped sx1 and
     * u_base + u_span is the U at the unclipped sx2, so those are the two
     * columns the ramp has to join.  Dividing by full_span instead
     * squeezes the seg's whole texture range into just the visible
     * columns, so any wall touching a screen edge gets a wrong - and,
     * since the clip moves as you turn, continuously changing - texture
     * column.  (Measured on E1M1: up to 5500 world units of U error on a
     * seg projecting to sx1=-1592, i.e. an essentially arbitrary texture
     * column; correct parameterisation keeps it to ~48, in line with the
     * perspective approximation's own error on unclipped segs.)
     * tex_u below then re-bases the ramp onto the first visible column. */
    wfixed_t u_step = orig_span > 0
        ? wr_div64(((int64_t) u_span) << WR_FRACBITS, orig_span)
        : 0;

    /* Texture U is affine in vz (both are affine in the seg parameter),
     * so the per-column walk can track it exactly: u(vz_x) = u_base +
     * du_dvz*(vz_x - vz1), with vz_x recovered from the stepped scale.
     * For a near-constant-depth wall (|dvz| small) that ratio is
     * ill-conditioned; there U is affine in screen-x anyway and the
     * u_step ramp is exact, so du_dvz = 0 keeps the old path.
     * The guard also keeps the quotient in int32: |dvz| > 2*|u_span| is
     * precisely |u_span<<32 / dvz| < 2^31. */
    int64_t dvz = (int64_t) vz2 - vz1;
    int64_t au = u_span < 0 ? -(int64_t) u_span : u_span;
    wfixed_t du_dvz = 0;
    if (dvz > 2 * au || dvz < -2 * au)
        du_dvz = wr_div64(((int64_t) u_span) << (2 * WR_FRACBITS), (int) dvz);

    WrSegSpan sp;
    sp.side_idx      = side_idx;
    sp.back_side_idx = back_side_idx;
    sp.csx1 = clip_sx1; sp.csx2 = clip_sx2;
    sp.sx1  = sx1;
    sp.scale1    = scale1;    sp.scalestep = scalestep;
    sp.vz1_clip  = vz1_clip;  sp.vz_step   = vz_step;
    sp.u_base    = u_base;    sp.u_step    = u_step;
    sp.du_dvz    = du_dvz;    sp.u_vz0     = vz1;
    sp.ld_flags  = ld->flags;
    sp.scale1_x    = s1_32;
    sp.scalestep_x = full_span > 0 ? sstep32 : 0;
    /* q(x) = (U - u_anchor) * scale is affine in screen-x (U and 1 are
     * both affine in the seg parameter, and so is U*scale / scale), so
     * U = u_anchor + q * iscale per column. Anchored at the NEARER
     * endpoint: there iscale has the fewest significant bits, and
     * anchoring makes U - u_anchor -> 0 where that matters. Sampled at
     * column centres. Magnitudes: |q| <= 32767 * 2^22 (< 2^37), the
     * slope numerator < 2^53, q * iscale ~ (U - u_anchor) * 2^32. */
    sp.uq_on = 0; sp.uq0 = 0; sp.uq_step = 0; sp.u_anchor = u_base;
    if (dxf > 0) {
        int64_t off = ((int64_t) (clip_sx1 - centerx) << WR_FRACBITS)
                    + (WR_FRACUNIT >> 1) - px1;
        int64_t qa, slope;
        if (scale2_full > scale1_full) {        /* v2 end is nearer */
            sp.u_anchor = u_base + (u_span << WR_FRACBITS);
            qa    = -(int64_t) u_span * scale1_full;
            slope = (((int64_t) u_span * scale1_full) << WR_FRACBITS) / dxf;
        } else {
            sp.u_anchor = u_base;
            qa    = 0;
            slope = (((int64_t) u_span * scale2_full) << WR_FRACBITS) / dxf;
        }
        sp.uq_step = slope;
        sp.uq0     = qa + ((slope * off) >> WR_FRACBITS);
        sp.uq_on   = 1;
    }
#ifdef WR_DSP_COLS
    if (wr_cull_log())
        fprintf(wr_cull_log(),
                "F %d %d %d %ld %ld %ld %ld %ld %ld %ld\n",
                (int) (seg - wr_s->wr_segs), clip_sx1, clip_sx2,
                (long) sx1, (long) scale1, (long) scalestep,
                (long) vz1_clip, (long) vz_step, (long) u_base, (long) u_step);
#endif
    wr_seg_columns(&sp);
}

static void wr_render_subsector(int ssi) {
    if (ssi < 0 || ssi >= wr_s->wr_n_subsectors) return;
    wr_skip_init();
    wrp_subsec_visited++;
#ifdef WR_DSP_COLS
    if (wr_cull_log()) { fprintf(wr_cull_log(), "sub %d\n", ssi); fflush(wr_cull_log()); }
#endif
    const WrSubsector *ss = &wr_s->wr_subsectors[ssi];
    int i;

    for (i = 0; i < ss->numsegs; i++) {
        int segi = ss->firstseg + i;
        if (segi < 0 || segi >= wr_s->wr_n_segs) continue;
        wrp_segs_seen++;
        if (wr_skip_mode != 4) wr_add_seg(&wr_s->wr_segs[segi]);
        if (wr_screen_closed()) return;
    }
}

/* Reset the per-walk state the BSP traversal accumulates into: clip
 * arrays, solidsegs, visplanes, sprite clip records and the counters.
 * Runs once per frame in wr_op_render, and again before a CPU fallback
 * walk if a DSP replay bailed mid-stream with partial state applied. */
static void wr_walk_reset(void) {
    int x;
    wr_solidsegs_n = 0;
    wr_n_visplanes = 0;
    for (x = 0; x < wr_screen_w; x++) {
        wr_ceilingclip[x] = -1;
        wr_floorclip[x] = (short) wr_screen_h;
        wr_clipseg_n[x] = 0;
        wr_mtex_n[x] = 0;
    }
    wrp_segs_seen = 0; wrp_segs_drawn = 0;
    wrp_nodes_visited = 0; wrp_subsec_visited = 0; wrp_bbox_rejected = 0;
    wrp_visplanes = 0; wrp_plane_splits = 0;
}

/* BSP node walk: recursive NEAR-side-first descent.
 *
 * The near-first order is load-bearing, not a detail: solidsegs and the
 * clip arrays record "already covered", so anything visited later loses.
 * Descending the far side first would let distant walls claim columns and
 * clip away the near ones - which is exactly the fragmented-slivers
 * failure mode this had before.
 *
 * wr_point_on_side returns 0 for the front/right side, matching the WAD
 * children[] order (0 = right, 1 = left), so children[side] is the near
 * child and children[side^1] the far one.
 *
 * The near child always contains the viewer, so it is always visible and
 * recursed unconditionally (same as the classic engine). The far child is only
 * recursed if wr_check_bbox says its bounding box is potentially visible
 * — not entirely behind the camera, and not fully covered by solidsegs.
 * This is the single biggest BSP-walk optimisation on a cacheless 486:
 * it prunes entire off-screen / already-occluded subtrees (everything
 * behind you, everything outside the FOV that earlier walls have covered)
 * without transforming a single seg inside them. */
static void wr_render_node(int node_idx) {
    if (wr_screen_closed()) return;
    if (node_idx & WR_NF_SUBSECTOR) {
        wr_render_subsector(node_idx & ~WR_NF_SUBSECTOR);
        return;
    }
    if (node_idx < 0 || node_idx >= wr_s->wr_n_nodes) return;
    wrp_nodes_visited++;
    const WrNode *nd = &wr_s->wr_nodes[node_idx];
    int side = wr_point_on_side((int) (wr_view_x >> WR_FRACBITS),
                                (int) (wr_view_z >> WR_FRACBITS), nd);
    /* Near child: always recurse (contains the viewer). */
    wr_render_node(side == 0 ? nd->right_child : nd->left_child);
    if (wr_screen_closed()) return;
    /* Far child: only recurse if its bbox is potentially visible.
     * The bbox pointer selects rt_* or lt_* depending on which child
     * is the far one. */
    const i16 *bbox = (side == 0) ? &nd->lt_top : &nd->rt_top;
    if (wr_check_bbox(bbox)) {
        wr_render_node(side == 0 ? nd->left_child : nd->right_child);
    } else {
        wrp_bbox_rejected++;
    }
}

#ifdef WR_DSP_COLS
/* ---- DSP geometry-walk replay ---------------------------------------------
 * The Falcon DSP56001 runs the whole visibility walk (BSP descent, bbox
 * rejection, solidsegs, seg cull/clip/projection) and ships back a compact
 * record stream; the host replays each record through wr_seg_columns so
 * the draw calls and clip state are identical to the CPU walk. The DSP's
 * fixed-point output is expected to be bit-exact - a wrong record is a
 * bug, not a tolerance issue. Stream layout (see dsp_wr.h):
 *   0x50xxxx  R_SEG: seg index; 16 words follow (csx pair, reserved, then
 *             sx1/scale1/scalestep/vz1c/vzstep/u_base/ustep as hi:lo16)
 *   0x52xxxx  R_SUB: subsector index (counter parity only)
 *   0xFFFFFF  R_END + 5 counters + ack
 * The tag carries the seg index rather than a sidedef index: the DSP's
 * map tables hold flattened segs, so the host resolves side_idx and
 * back_side_idx from its own structures at replay (same lookups and
 * early-outs as wr_add_seg itself).
 * Any malformed word aborts the replay - wr_op_render then re-runs the
 * whole walk on the CPU, which is also the fallback when the DSP is
 * absent. */
#define WR_REC_SEG 0x500000UL
#define WR_REC_SUB 0x520000UL
#define WR_REC_END 0xFFFFFFUL
#define WR_REC_TAG(w) ((w) & 0xFF0000UL)
#define WR_REC_IDX(w) ((int) ((w) & 0xFFFFUL))

static int wr_stream32(const unsigned long *r, int i) {
    return (int) ((r[i] << 16) | (r[i + 1] & 0xFFFFUL));
}

static void wr_add_seg_from_rec(const unsigned long *r) {
    /* The DSP walked the seg and emitted its index; resolve the sidedef
     * pair here exactly as wr_add_seg does, including the early-outs for
     * malformed indices (a skipped record matches a CPU-side early
     * return - the seg simply contributes nothing). */
    int segi = WR_REC_IDX(r[0]);
    if (segi >= wr_s->wr_n_segs) return;
    const WrSeg *seg = &wr_s->wr_segs[segi];
    if (seg->linedef < 0 || seg->linedef >= wr_s->wr_n_linedefs) return;
    const WrLinedef *ld = &wr_s->wr_linedefs[seg->linedef];
    int side_idx = (seg->side == 1) ? ld->back : ld->front;
    if (side_idx < 0 || side_idx >= wr_s->wr_n_sidedefs) return;

    WrSegSpan sp;
    sp.side_idx      = side_idx;
    sp.back_side_idx = (seg->side == 1) ? ld->front : ld->back;
    sp.csx1          = (int) ((r[1] >> 12) & 0xFFF);
    sp.csx2          = (int) (r[1] & 0xFFF);
    sp.sx1       = wr_stream32(r, 3);
    sp.scale1    = wr_stream32(r, 5);
    sp.scalestep = wr_stream32(r, 7);
    sp.vz1_clip  = wr_stream32(r, 9);
    sp.vz_step   = wr_stream32(r, 11);
    sp.u_base    = wr_stream32(r, 13);
    sp.u_step    = wr_stream32(r, 15);
    /* The DSP walk still parameterises U over screen columns; du_dvz = 0
     * keeps wr_seg_columns on the linear-u ramp for its records (its
     * vz_step is likewise still consumed as it ships). */
    sp.du_dvz    = 0;
    sp.u_vz0     = 0;
    sp.uq_on     = 0;
    sp.uq0 = 0; sp.uq_step = 0; sp.u_anchor = sp.u_base;
    sp.scale1_x    = (int64_t) sp.scale1 << WR_FRACBITS;
    sp.scalestep_x = (int64_t) sp.scalestep << WR_FRACBITS;
    sp.ld_flags  = ld->flags;
    wr_seg_columns(&sp);
}

/* Run the BSP walk on the DSP and replay its record stream. Returns 1
 * when the whole stream consumed cleanly, 0 on any failure (the caller
 * re-runs wr_render_node on the CPU; clip state is reinitialised). */
static int wr_dsp_walk_frame(WrHostState *s) {
    WrDspView v;
    v.x = wr_view_x;  v.z = wr_view_z;  v.y = wr_view_y;
    v.vcos = wr_view_cos; v.vsin = wr_view_sin;
    v.proj = wr_proj_dist;  v.invproj = wr_inv_proj_dist;
    v.w = wr_screen_w;      v.h = wr_screen_h;
    v.root = (s->wr_n_nodes > 0) ? s->wr_n_nodes - 1 : -1;

    int nw = 0;
    const unsigned long *st = wr_dsp_walk(&v, &nw);
    if (!st) return 0;

    int i = 0;
    for (;;) {
        if (i >= nw) return 0;                  /* ran off the end      */
        unsigned long w = st[i];
        if (w == WR_REC_END) {
            if (i + 6 > nw) return 0;
            /* Counter parity: the DSP walked the same nodes/subsecs the
             * CPU counters report. */
            wrp_nodes_visited  = (unsigned) st[i + 1];
            wrp_subsec_visited = (unsigned) st[i + 2];
            wrp_bbox_rejected  = (unsigned) st[i + 3];
            wrp_segs_seen      = (unsigned) st[i + 4];
            wrp_segs_drawn     = (unsigned) st[i + 5];
            return 1;
        }
        unsigned long tag = WR_REC_TAG(w);
        if (tag == WR_REC_SEG) {
            /* 17 words: the tag plus 16 payload words (the last field is
             * read as st[i+15]/st[i+16]) - advancing by 16 desyncs. */
            if (i + 17 > nw) return 0;
            if (WR_REC_IDX(w) >= (unsigned) wr_s->wr_n_segs) return 0;
            wr_add_seg_from_rec(&st[i]);
            i += 17;
        } else if (tag == WR_REC_SUB) {
            /* Record kept for stream inspection/sprite use; the totals
             * arrive with R_END, so nothing is counted here. */
            i += 1;
        } else {
            return 0;                           /* corrupt stream       */
        }
    }
}
#endif /* WR_DSP_COLS */

/* Flush the accumulated renderer profile to WRPROF.LOG.  Called every 30
 * frames during normal play (so the data survives a hard power-off) and
 * once at shutdown.  When --fps is active (RELIC_WINFLAG_NO_VSYNC) the
 * periodic calls are skipped to keep disk I/O out of the benchmark, and
 * only the shutdown flush runs.
 *
 * Opt-in: the log is only written when the TGXWRPROF environment variable
 * is set (e.g. `set TGXWRPROF=1` on DOS), so release runs never drop a
 * profile file next to the program.  The counters themselves are cheap
 * increments and keep running either way. */
#include <stdlib.h>
static int wrp_log_enabled = -1;   /* -1 = TGXWRPROF not checked yet */
static void wr_prof_flush(void) {
    if (wrp_log_enabled < 0) wrp_log_enabled = getenv("TGXWRPROF") != NULL;
    if (!wrp_log_enabled) return;
    if (wrp_frames == 0) return;
    FILE *f = fopen("WRPROF.LOG", "w");
    if (!f) return;
    fprintf(f, "=== WR RENDERER PROFILE (%u frames) ===\n", wrp_frames);
    fprintf(f, "AVERAGES (ms/frame):\n");
    fprintf(f, "  setup+clear:  %u  (%u avg)\n", wrp_clear_ms, wrp_clear_ms / wrp_frames);
    fprintf(f, "  bsp walk:     %u  (%u avg)\n", wrp_bsp_ms,   wrp_bsp_ms / wrp_frames);
    fprintf(f, "  planes:       %u  (%u avg)\n", wrp_planes_ms, wrp_planes_ms / wrp_frames);
    fprintf(f, "  TOTAL:        %u  (%u avg)\n", wrp_total_ms, wrp_total_ms / wrp_frames);
    fprintf(f, "\nLAST FRAME:\n");
    fprintf(f, "  setup+clear=%u  bsp=%u  planes=%u  total=%u\n",
            wrp_last_setup, wrp_last_bsp, wrp_last_planes, wrp_last_total);
    fprintf(f, "\nCOUNTERS (last frame):\n");
    fprintf(f, "  nodes visited:    %u\n", wrp_nodes_visited);
    fprintf(f, "  subsec visited:   %u\n", wrp_subsec_visited);
    fprintf(f, "  bbox rejected:    %u\n", wrp_bbox_rejected);
    fprintf(f, "  segs seen:        %u\n", wrp_segs_seen);
    fprintf(f, "  segs drawn:       %u\n", wrp_segs_drawn);
    fprintf(f, "  wall columns:     %u (iters %u)\n", wrp_wall_cols, wrp_col_iters);
    fprintf(f, "  wall pixels:      %u\n", wrp_wall_px);
    fprintf(f, "  plane rows:       %u\n", wrp_plane_rows);
    fprintf(f, "  plane pixels:     %u\n", wrp_plane_px);
    fprintf(f, "  map_plane calls:  %u (spanstart writes %u)\n",
            wrp_map_calls, wrp_span_writes);
    fprintf(f, "  map_plane avg:    %u calls/f, %u span-writes/f\n",
            wrp_map_calls_tot / wrp_frames, wrp_span_writes_tot / wrp_frames);
    fprintf(f, "  planes avg:       %u rows/f, %u px/f, %u colscan/f\n",
            wrp_plane_rows_tot / wrp_frames, wrp_plane_px_tot / wrp_frames,
            wrp_plane_cols_tot / wrp_frames);
    fprintf(f, "  planes split:     sky %u ms/f (%u px/f), flat %u ms/f, vpl/f %u\n",
            wrp_sky_ms / wrp_frames, wrp_sky_px_tot / wrp_frames,
            wrp_flat_ms / wrp_frames, wrp_visplanes_tot / wrp_frames);
    fprintf(f, "  visplanes:        %u (%u splits, %u bytes cleared)\n",
            wrp_visplanes, wrp_plane_splits,
            wrp_visplanes * (unsigned int) (wr_screen_w * sizeof(short)));
    fprintf(f, "  sky pixels:       %u\n", wrp_sky_px);
    fprintf(f, "  total px written: %u\n",
            wrp_wall_px + wrp_plane_px + wrp_sky_px);
    fprintf(f, "  screen size:      %dx%d = %u px\n",
            wr_screen_w, wr_screen_h,
            (unsigned)wr_screen_w * (unsigned)wr_screen_h);
#ifdef WR_DSP_COLS
    {
        int alive, qsent, qcmds, qacked, merged;
        wr_dsp_stats(&alive, &qsent, &qcmds, &qacked, &merged);
        {
            int pw = -1, pisr = -1, pcalls = -1;
            wr_dsp_dbg(&pw, &pisr, &pcalls);
            fprintf(f, "  dsp: alive=%d qsent=%d cmds=%d acked=%d merged=%d fail=%d isr=%d pumpw=%d pumpisr=%d pumps=%d\n",
                    alive, qsent, qcmds, qacked, merged, wr_dsp_fail(),
                    wr_dsp_isr(), pw, pisr, pcalls);
            fprintf(f, "  dsp-walk: dsp_frames=%u cpu_frames=%u stream_words=%d walk_fail=%d\n",
                    wrp_dsp_walked, wrp_cpu_walked,
                    wr_dsp_walk_words(), wr_dsp_walk_fail());
            fprintf(f, "  dsp-time (last frame ms): begin=%u walkframe=%u\n",
                    wrp_last_dspbegin, wrp_last_dspframe);
            fprintf(f, "  frame1: walked=%u nodes=%u subs=%u bbox=%u seen=%u drawn=%u cols=%u wallpx=%u\n",
                    wrp_f1_walked, wrp_f1_nodes, wrp_f1_subs, wrp_f1_bbox,
                    wrp_f1_seen, wrp_f1_drawn, wrp_f1_cols, wrp_f1_wallpx);
            {
                long tt[6];
                int lc = -1, lw = -1, tf = -1, qd = -1;
                wr_dsp_times(tt);
                wr_dsp_dbg2(&lc, &lw, &tf, &qd);
                fprintf(f, "  dsp-merge ms: drain=%ld dumpx=%ld scatx=%ld dumpy=%ld scaty=%ld cols=%ld\n",
                        tt[0], tt[1], tt[2], tt[3], tt[4], tt[5]);
                fprintf(f, "  dsp-io: loady=%d loadyw=%d texflush=%d qdrains=%d pumphz=%ld drainhz=%ld\n",
                        lc, lw, tf, qd, wr_dsp_hz(0), wr_dsp_hz(1));
            }
        }
    }
#endif
    fclose(f);
}

/* @wr_render(x, y, z, angle, fov) - see OP_WR_RENDER in tigryx.h. Renders
 * the whole visible world from this viewpoint into the backbuffer: BSP
 * front-to-back with a solid-segment clip list (so every column is
 * written at most once - no z-buffer, no overdraw, no perspective divide
 * in the inner loops). Requires @wr_map and @wr_palette to have been
 * called first. y is height (matching @camera's eye convention), x/z are
 * the ground-plane position. */
static void wr_op_render(RelicVM *vm) {
    WrHostState *s = (WrHostState *) vm->backend_data;
    if (!s || !WR_HAS_BUF(s) || !s->wr_map_set || !s->wr_palette_set || !s->wr_colormap) {
        return;
    }

    unsigned int t_total = WR_MILLIS();

    float fx = wr_f32_from_bits(vm->r[1]);
    float fy = wr_f32_from_bits(vm->r[2]);
    float fz = wr_f32_from_bits(vm->r[3]);
    float fangle = wr_f32_from_bits(vm->r[4]);
    float ffov = wr_f32_from_bits(vm->r[5]);
    if (ffov < 0.1f) ffov = 0.1f;
    if (ffov > 3.0f) ffov = 3.0f;

    /* The only float math in the whole renderer: one-time-per-frame
     * setup, not a hot loop. Everything from here down is fixed-point. */
    wr_view_x = (wfixed_t) (fx * (float) WR_FRACUNIT);
    wr_view_y = (wfixed_t) (fy * (float) WR_FRACUNIT);
    wr_view_z = (wfixed_t) (fz * (float) WR_FRACUNIT);
    /* Player forward is (sin(angle), cos(angle)) in (x,z) - see adoom's
     * update_camera(): target = player + (sin a, cos a). Right is that
     * rotated -90 deg: (cos a, -sin a). So a world offset (dx,dz) from
     * the eye has view-space (right, forward):
     *   vx = dx*cos(a) - dz*sin(a)
     *   vz = dx*sin(a) + dz*cos(a)
     * (verified: for a point straight ahead, dx=d*sin(a), dz=d*cos(a),
     * this gives vx=0, vz=d, as it must). wr_view_cos/sin below are used
     * directly as cos(a)/sin(a) in that formula (see wr_add_seg /
     * wr_map_plane) - NOT negated, despite the temptation to think of
     * this as "rotate world by -angle". */
    wr_view_cos = (wfixed_t) (cosf(fangle) * (float) WR_FRACUNIT);
    wr_view_sin = (wfixed_t) (sinf(fangle) * (float) WR_FRACUNIT);

    wr_s = s;
    /* Use the 3D render area if set (@wr_flatshade), otherwise the full
     * backbuffer. The projection math (centerx/centery, proj_dist) uses
     * the render area dimensions; pixel writes add wr_origin_x/y. */
    if (s->wr_area_w > 0 && s->wr_area_h > 0) {
        wr_screen_w = s->wr_area_w;
        wr_screen_h = s->wr_area_h;
        wr_origin_x = s->wr_area_x;
        wr_origin_y = s->wr_area_y;
    } else {
        wr_screen_w = WR_BUF_W(s);
        wr_screen_h = WR_BUF_H(s);
        wr_origin_x = 0;
        wr_origin_y = 0;
    }
    /* Low-detail: logical render resolution shrinks by the pixel scale.
     * Everything downstream (clip arrays, projection, planes, sprite
     * clip records) works in logical pixels; the draw functions widen
     * the final stores back to the physical area. */
    wr_screen_w /= wr_px_sx;
    wr_screen_h /= wr_px_sy;
    if (wr_screen_w > WR_MAX_SCREEN_W) wr_screen_w = WR_MAX_SCREEN_W;
    if (wr_screen_h > WR_MAX_SCREEN_H) wr_screen_h = WR_MAX_SCREEN_H;
    if (wr_screen_w < 1 || wr_screen_h < 1) return;
    wr_proj_dist = (wfixed_t) (((float) (wr_screen_w / 2) / tanf(ffov * 0.5f)) * (float) WR_FRACUNIT);
    if (wr_proj_dist <= 0) return;
    wr_inv_proj_dist = wr_div64(((int64_t) WR_FRACUNIT) << WR_FRACBITS, wr_proj_dist);

    /* Sky scroll setup (see wr_draw_sky_plane). Reproduces the classic
     * sky-scroll ratio: a full 360-degree turn scrolls the sky
     * texture across itself 4 times (its own width is only ~90 degrees
     * of view, so this is what makes it feel like a wraparound backdrop
     * rather than a single flat panel). Column offsets are approximated
     * as linear in screen x, which is imperceptible for a background this
     * far away and avoids a per-column atan. */
    if (s->wr_sky_tex_id > 0 && s->wr_sky_tex_id < WR_MAX_TEXTURES && s->wr_textures[s->wr_sky_tex_id].data) {
        int sky_tex_w = s->wr_textures[s->wr_sky_tex_id].w;
        float scroll_per_rad = ((float) sky_tex_w * 4.0f) / 6.28318530718f;
        wr_sky_u0     = (wfixed_t) (fangle * scroll_per_rad * (float) WR_FRACUNIT);
        wr_sky_du_col = (wfixed_t) ((ffov * scroll_per_rad / (float) wr_screen_w) * (float) WR_FRACUNIT);
    } else {
        wr_sky_u0 = 0;
        wr_sky_du_col = 0;
    }

    /* Visplane column arrays, allocated once (and reallocated only if the
     * resolution changes). One block for all planes keeps it to a single
     * malloc: 64 planes * 2 arrays * width shorts = 80 KB at 320 wide. */
    if (!wr_plane_buf || wr_plane_buf_w != wr_screen_w) {
        if (wr_plane_buf) WR_FREE(wr_plane_buf);
        wr_plane_buf_w = wr_screen_w;
#ifdef WR_ALLOC_ST
        /* Visplane column arrays are touched per column in both the walk
         * (wr_plane_add_column) and the draw_planes scan - keep them in
         * cached ST-RAM rather than TT-RAM. */
        wr_plane_buf = (short *) WR_ALLOC_ST((size_t) WR_MAXVISPLANES * 2u *
                                        (size_t) wr_plane_buf_w * sizeof(short));
#else
        wr_plane_buf = (short *) WR_ALLOC((size_t) WR_MAXVISPLANES * 2u *
                                        (size_t) wr_plane_buf_w * sizeof(short));
#endif
        if (!wr_plane_buf) { wr_plane_buf_w = 0; return; }
    }

    unsigned int t0 = WR_MILLIS();

    /* No full-screen clear: every row a wall column claims is now
     * written by a wall piece, a real plane, or a void (black) plane,
     * and the post-walk open-band fill below blacks the rest - the
     * old unconditional 64 KB memset was redundant work on sealed
     * maps. The WRSKIP profiling modes deliberately suppress pieces of
     * the drawing, so they keep the old clear. */
    wr_skip_init();
    if (wr_skip_mode != 0) {
        wr_host_clear_backbuf(s, wr_origin_x, wr_origin_y,
                              wr_screen_w * wr_px_sx, wr_screen_h * wr_px_sy);
    }

    wr_walk_reset();
    wrp_wall_cols = 0; wrp_wall_px = 0; wrp_col_iters = 0;
    wrp_plane_rows = 0; wrp_plane_px = 0;
    wrp_map_calls = 0; wrp_span_writes = 0;
    wrp_sky_px = 0;

    wrp_last_setup = WR_MILLIS() - t0;

    t0 = WR_MILLIS();
#ifdef WR_DSP_COLS
    /* Start the DSP's frame before the walk below: with the geometry
     * offload it runs the whole BSP traversal and ships back a compact
     * record stream the replay consumes. chunky/pitch/offset also arm
     * the optional pixel-merge path (WRPIX opt-in). */
    wr_dsp_begin(wr_screen_w, wr_screen_h,
                 WR_ROW(s, 0), WR_PITCH(s, 0), wr_origin_x, wr_origin_y);
    wrp_last_dspbegin = WR_MILLIS() - t0;
#endif
    int walked = 0;
#ifdef WR_DSP_COLS
    /* The geometry walk needs the record stream bit-exact; the WRSKIP
     * profiling modes alter the column loop on the CPU side only, so any
     * active skip mode routes to the plain CPU walk instead. */
    wr_skip_init();
    /* WRNODSP=1 forces the CPU walk without touching the column loop, so a
     * reference run is directly comparable to the DSP replay. */
    {
        static int s_nodsp = -1;
        if (s_nodsp < 0) s_nodsp = getenv("WRNODSP") ? 1 : 0;
        if (!wr_skip_mode && !s_nodsp && s->wr_map_set) {
            unsigned int tw = WR_MILLIS();
            walked = wr_dsp_walk_frame(s);
            wrp_last_dspframe = WR_MILLIS() - tw;
        }
    }
    if (!walked)
        wr_walk_reset();   /* a failed replay leaves partial clip state */
#endif
    if (!walked) {
        if (s->wr_n_nodes > 0) {
            wr_render_node(s->wr_n_nodes - 1);   /* WAD convention: root is the last node */
        } else if (s->wr_n_subsectors > 0) {
            wr_render_subsector(0);              /* trivial one-subsector map */
        }
    }
    if (walked) wrp_dsp_walked++; else wrp_cpu_walked++;
    if (wrp_frames == 0) {
        wrp_f1_walked = (unsigned) walked;
        wrp_f1_nodes = wrp_nodes_visited; wrp_f1_subs = wrp_subsec_visited;
        wrp_f1_bbox = wrp_bbox_rejected;
        wrp_f1_seen = wrp_segs_seen; wrp_f1_drawn = wrp_segs_drawn;
        wrp_f1_cols = wrp_wall_cols; wrp_f1_wallpx = wrp_wall_px;
    }
    wrp_last_bsp = WR_MILLIS() - t0;

    t0 = WR_MILLIS();
    /* Floors/ceilings last, as horizontal spans - one divide per row. */
    wr_draw_planes();
    wrp_last_planes = WR_MILLIS() - t0;

    /* Sparse hole fill: rows still open at frame end - never claimed
     * by any seg (columns the walk never reached, portals looking into
     * void) - get the black the old full-screen clear gave them. In a
     * sealed map this scans 320 shorts and writes nothing. Runs after
     * the planes so void-plane spans (claimed-but-black regions) are
     * not overwritten; it can only touch rows no draw ever wrote. */
    {
        int hx;
        for (hx = 0; hx < wr_screen_w; hx++) {
            int ht = wr_ceilingclip[hx] + 1;
            int hb = wr_floorclip[hx] - 1;
            WrPix *hdst;
            int hpitch, hy;
            if (ht > hb) continue;
            hdst = WR_ROW(s, ht * wr_px_sy + wr_origin_y) + (hx * wr_px_sx + wr_origin_x);
            hpitch = WR_PITCH(s, ht * wr_px_sy + wr_origin_y);
            for (hy = ht; hy <= hb; hy++, hdst += hpitch * wr_px_sy) {
                int rr;
                for (rr = 0; rr < wr_px_sy; rr++) {
                    hdst[rr * hpitch] = (WrPix) 0;
                    if (wr_px_sx > 1) hdst[rr * hpitch + 1] = (WrPix) 0;
                }
            }
        }
    }

#ifdef WR_DSP_COLS
    /* DSP wall pixels land in the chunky buffer: walls and the
     * CPU-drawn spans partition the screen (clip-list write-once), so
     * the stream order vs. the earlier span fill doesn't matter. */
    wr_dsp_merge(WR_ROW(s, 0), WR_PITCH(s, 0), wr_origin_x, wr_origin_y,
                 wr_screen_w, wr_screen_h);
#endif

    wrp_last_total = WR_MILLIS() - t_total;
    wrp_last_clear = wrp_last_setup; /* clear+clip init measured together */

    /* Accumulate averages. */
    wrp_frames++;
    wrp_setup_ms += wrp_last_setup;
    wrp_clear_ms += wrp_last_setup;
    wrp_bsp_ms   += wrp_last_bsp;
    wrp_planes_ms += wrp_last_planes;
    wrp_map_calls_tot += wrp_map_calls;
    wrp_span_writes_tot += wrp_span_writes;
    wrp_plane_rows_tot += wrp_plane_rows;
    wrp_plane_px_tot += wrp_plane_px;
    wrp_sky_px_tot += wrp_sky_px;
    wrp_total_ms += wrp_last_total;

    /* Flush to WRPROF.LOG every 30 frames so data survives a hard
     * power-off. The file I/O itself runs after all the timed phases,
     * so it never pollutes its own numbers.  Skip the periodic flush
     * when --fps is active (NO_VSYNC): two fopen/fclose pairs every 30
     * frames perturb the very FPS we're trying to measure.  The shutdown
     * path still writes a final log so no data is lost. */
    if (wrp_frames % 30 == 0 &&
        (s->win_flags & RELIC_WINFLAG_NO_VSYNC) == 0) {
        wr_prof_flush();
    }

    wr_host_present(s, wr_origin_x, wr_origin_y,
                    wr_screen_w * wr_px_sx, wr_screen_h * wr_px_sy);
    s->frame_dirty = true;
}

/* ---- @wr_sprite: masked world-space billboards (WAD things) ---- */

/* One item in the @wr_sprite batch array: 11 i32s = 44 bytes, matching
 * the packed tgx struct the script passes (f32 fields arrive as their raw
 * bit pattern, reinterpreted via wr_f32_from_bits). The `data` field is a
 * guest pointer (32-bit offset into vm->mem), same convention as the
 * WrMapHeader pointers in wr_op_map. */
#define WR_SPRITE_ITEM_SIZE 44
#define WR_SPRITE_MAX     256
enum {
    WR_SPR_F_X = 0,  /* f32: centre position, map units */
    WR_SPR_F_Z,      /* f32: centre position, map units */
    WR_SPR_F_HL,     /* f32: map units the sprite extends LEFT of centre */
    WR_SPR_F_HR,     /* f32: map units the sprite extends RIGHT of centre */
    WR_SPR_F_YB,     /* f32: world height of the billboard bottom, map units */
    WR_SPR_F_YT,     /* f32: world height of the billboard top, map units */
    WR_SPR_F_LIGHT,  /* i32: sector light level 0..255 (mapped like walls) */
    WR_SPR_F_W,      /* i32: patch width  (texels) */
    WR_SPR_F_H,      /* i32: patch height (texels) */
    WR_SPR_F_FLAGS,  /* i32: bit0 = mirror horizontally,
                      *      bit1 = fuzzy (spectre): covered pixels remap the
                      *      framebuffer through colormap row 6 (the fuzz table)
                      *      instead of writing patch texels */
    WR_SPR_F_DATA    /* guest ptr: row-major u8 palette indices, 0 = transparent.
                      * Last field on purpose: in the tigryxrc native shim the
                      * item struct keeps the real pointer 8-aligned with no
                      * interior padding on either 32- or 64-bit hosts. */
};
#define WR_SPRITE_MIRROR 1
#define WR_SPRITE_FUZZY  2

typedef struct {
    wfixed_t vz;       /* view depth (16.16 map units); larger = farther */
    int      item_off; /* byte offset of this item in the guest array */
    int      sx1_full; /* unclipped left column (may be off-screen) */
    int      span_full;/* unclipped screen width in columns */
    int      sx1, sx2; /* screen column span, clipped to [0, wr_screen_w) */
    int      ytop, ybot;   /* unclipped screen rows of sprite top/bottom */
} WrSpriteProj;

static WrSpriteProj wr_spr_proj[WR_SPRITE_MAX];
static int wr_spr_order[WR_SPRITE_MAX];

/* Project one item to screen space. Returns 0 when the sprite is entirely
 * off-screen/behind the camera and should be skipped. */
static int wr_sprite_project(const i32 *it, WrSpriteProj *p) {
    float fx  = wr_f32_from_bits(it[WR_SPR_F_X]);
    float fz  = wr_f32_from_bits(it[WR_SPR_F_Z]);
    float fhl = wr_f32_from_bits(it[WR_SPR_F_HL]);
    float fhr = wr_f32_from_bits(it[WR_SPR_F_HR]);
    float fyb = wr_f32_from_bits(it[WR_SPR_F_YB]);
    float fyt = wr_f32_from_bits(it[WR_SPR_F_YT]);

    wfixed_t dx = (wfixed_t) (fx * (float) WR_FRACUNIT) - wr_view_x;
    wfixed_t dz = (wfixed_t) (fz * (float) WR_FRACUNIT) - wr_view_z;
    wfixed_t vx = (wfixed_t) (((int64_t) dx * wr_view_cos - (int64_t) dz * wr_view_sin) >> WR_FRACBITS);
    wfixed_t vz = (wfixed_t) (((int64_t) dx * wr_view_sin + (int64_t) dz * wr_view_cos) >> WR_FRACBITS);
    /* A sprite whose centre sits at/behind the near plane still wants to
     * draw (a melee-range monster fills the view); clamp rather than
     * reject - only fully-behind-centre sprites are dropped. The wall
     * clip records then bound whatever columns the huge projection
     * covers. */
    if (vz <= 0) return 0;
    if (vz < WR_FRACUNIT) vz = WR_FRACUNIT;   /* 1 map unit minimum depth */

    wfixed_t scale = wr_div64(((int64_t) wr_proj_dist) << WR_FRACBITS, vz);
    if (scale <= 0) return 0;

    int centerx = wr_screen_w >> 1;
    int centery = wr_screen_h >> 1;
    int cx = centerx + (int) (((int64_t) vx * scale) >> (WR_FRACBITS * 2));
    int hl_px = (int) (((int64_t) (wfixed_t) (fhl * (float) WR_FRACUNIT) * scale) >> (WR_FRACBITS * 2));
    int hr_px = (int) (((int64_t) (wfixed_t) (fhr * (float) WR_FRACUNIT) * scale) >> (WR_FRACBITS * 2));
    int sx1_full = cx - hl_px;
    int span_full = hl_px + hr_px;   /* exclusive right edge = sx1_full + span */
    int sx1 = sx1_full;
    int sx2 = sx1_full + span_full;
    if (span_full <= 0 || sx2 <= 0 || sx1 >= wr_screen_w) return 0;
    if (sx1 < 0) sx1 = 0;
    if (sx2 > wr_screen_w) sx2 = wr_screen_w;
    if (sx1 >= sx2) return 0;

    /* Vertical: screen row = centery - (world_rel_eye * scale). */
    wfixed_t yb = (wfixed_t) (fyb * (float) WR_FRACUNIT) - wr_view_y;
    wfixed_t yt = (wfixed_t) (fyt * (float) WR_FRACUNIT) - wr_view_y;
    int ybot = centery - (int) (((int64_t) yb * scale) >> (WR_FRACBITS * 2));
    int ytop = centery - (int) (((int64_t) yt * scale) >> (WR_FRACBITS * 2));
    if (ybot <= ytop || ybot <= 0 || ytop >= wr_screen_h) return 0;

    p->vz = vz;
    p->sx1_full = sx1_full;
    p->span_full = span_full;
    p->sx1 = sx1;
    p->sx2 = sx2;
    p->ytop = ytop;
    p->ybot = ybot;
    return 1;
}

/* @wr_sprite(items, count) - see OP_WR_SPRITE in tigryx.h. Draws `count`
 * masked billboard sprites (WAD things) into the backbuffer, sorted
 * back-to-front by depth, each column clipped against the portal windows
 * the wall pass recorded (wr_clipseg_*). Must be called after @wr_render
 * in the same frame - the clip records and view transform persist from
 * that call. Column drawing is divide-free like wr_draw_column: one
 * 16.16 v-step per sprite, accumulated per pixel; palette index 0 is
 * transparent. */
static void wr_op_sprite(RelicVM *vm) {
    WrHostState *s = (WrHostState *) vm->backend_data;
    if (!s || !WR_HAS_BUF(s) || !s->wr_map_set || !s->wr_colormap) return;
    if (wr_screen_w <= 0 || wr_screen_h <= 0) return;

    /* Track the union dirty rect so the host present happens once for
     * the whole batch, not once per sprite. */
    int dirty_x1 = wr_screen_w, dirty_y1 = wr_screen_h;
    int dirty_x2 = 0, dirty_y2 = 0;

    /* Masked-midtexture pass A: draw every deferred fence column
     * back-to-front (records are nearest-first, so iterate reversed).
     * Sprites drawn below may overpaint these; entries nearer than a
     * sprite are redrawn over it per column in pass B. */
    {
        int x;
        for (x = 0; x < wr_screen_w; x++) {
            int i = (int) wr_mtex_n[x] - 1;
            for (; i >= 0; i--) {
                const WrMtexCol *m = &wr_mtex[x][i];
                wr_mtex_draw(s, x, m);
                if (x < dirty_x1) dirty_x1 = x;
                if (x > dirty_x2) dirty_x2 = x;
                if (m->ceil < dirty_y1) dirty_y1 = m->ceil;
                if (m->floor > dirty_y2) dirty_y2 = m->floor;
            }
        }
    }

    u32 items_ptr = (u32) vm->r[1];
    i32 count = vm->r[2];
    if (count < 0) count = 0;
    if (count > WR_SPRITE_MAX) count = WR_SPRITE_MAX;
    if ((uint64_t) items_ptr + (uint64_t) count * WR_SPRITE_ITEM_SIZE
            > (uint64_t) vm->mem_sz) count = 0;

    /* Project + validate every item, collecting the drawable ones. */
    int n_proj = 0;
    int i;
    for (i = 0; i < count; i++) {
        int off = (int) items_ptr + i * WR_SPRITE_ITEM_SIZE;
        i32 it[11];
        memcpy(it, vm->mem + off, sizeof(it));
        int src_w = it[WR_SPR_F_W], src_h = it[WR_SPR_F_H];
        u32 data_ptr = (u32) it[WR_SPR_F_DATA];
        if (src_w <= 0 || src_h <= 0 || src_w > 512 || src_h > 512) continue;
        if (data_ptr == 0 ||
            (uint64_t) data_ptr + (uint64_t) src_w * (uint64_t) src_h
                > (uint64_t) vm->mem_sz) continue;
        WrSpriteProj *p = &wr_spr_proj[n_proj];
        p->item_off = off;
        if (wr_sprite_project(it, p)) {
            wr_spr_order[n_proj] = n_proj;
            n_proj++;
        }
    }

    /* Painter's order: sort projected sprites by depth, farthest first.
     * Insertion sort - n is small (level thing counts are tens, capped at
     * 256) and the list is usually nearly ordered already. */
    for (i = 1; i < n_proj; i++) {
        int o = wr_spr_order[i];
        wfixed_t d = wr_spr_proj[o].vz;
        int j = i - 1;
        while (j >= 0 && wr_spr_proj[wr_spr_order[j]].vz < d) {
            wr_spr_order[j + 1] = wr_spr_order[j];
            j--;
        }
        wr_spr_order[j + 1] = o;
    }

    int oi;
    for (oi = 0; oi < n_proj; oi++) {
        const WrSpriteProj *p = &wr_spr_proj[wr_spr_order[oi]];
        i32 it[11];
        memcpy(it, vm->mem + (size_t) p->item_off, sizeof(it));

        int light = it[WR_SPR_F_LIGHT];
        if (light < 0) light = 0;
        if (light > 255) light = 255;
        int row = ((255 - light) * s->wr_num_colormaps) / 256;
        if (row < 0) row = 0;
        if (row >= s->wr_num_colormaps) row = s->wr_num_colormaps - 1;
        const WrPix *cmap = (const WrPix *)s->wr_colormap + (size_t) row * 256u;

        const unsigned char *src = vm->mem + (u32) it[WR_SPR_F_DATA];
        int src_w = it[WR_SPR_F_W], src_h = it[WR_SPR_F_H];
        int mirror = (it[WR_SPR_F_FLAGS] & WR_SPRITE_MIRROR) != 0;
        /* Spectre fuzz: dest pixels darken through colormap row 6 (the classic
         * fuzz table). The sprite's own texels only gate coverage. */
        const WrPix *fuzzmap = NULL;
        if ((it[WR_SPR_F_FLAGS] & WR_SPRITE_FUZZY) != 0) {
            int fr = s->wr_num_colormaps - 1;
            if (fr > 6) fr = 6;
            if (fr < 0) fr = 0;
            fuzzmap = (const WrPix *) s->wr_colormap + (size_t) fr * 256u;
        }

        /* The sprite's reciprocal scale for the depth compare in the clip
         * records - same quantity the wall loop tracks per column. */
        wfixed_t iscale = (wfixed_t) (((int64_t) p->vz * wr_inv_proj_dist) >> WR_FRACBITS);

        int spr_h = p->ybot - p->ytop;        /* unclipped screen height */
        /* Texture stepping: u runs across the sprite's UNCLIPPED span so a
         * screen-edge clip still samples the right texel columns; v runs
         * down the column. One divide each per sprite, adds per pixel. */
        wfixed_t vstep = (wfixed_t) (((int64_t) src_h << WR_FRACBITS) / spr_h);
        wfixed_t ustep = (wfixed_t) (((int64_t) src_w << WR_FRACBITS) / p->span_full);

        int x;
        int drew = 0;
        for (x = p->sx1; x < p->sx2; x++) {
            int clip_t, clip_b;
            wr_sprite_clip_window(x, iscale, &clip_t, &clip_b);
            int yl = p->ytop, yh = p->ybot - 1;
            if (yl < clip_t) yl = clip_t;
            if (yh > clip_b) yh = clip_b;
            if (yl < 0) yl = 0;
            if (yh >= wr_screen_h) yh = wr_screen_h - 1;
            if (yl > yh) continue;

            int tex_col = (int) (((int64_t) (x - p->sx1_full) * ustep) >> WR_FRACBITS);
            if (tex_col < 0) tex_col = 0;
            if (tex_col >= src_w) tex_col = src_w - 1;
            if (mirror) tex_col = src_w - 1 - tex_col;

            /* v continues from the clipped top: texel row of screen row
             * yl is (yl - ytop) * vstep - the clip eats sprite rows off
             * the top, it does not re-anchor the texture. */
            wfixed_t v = (wfixed_t) (yl - p->ytop) * vstep;
            const unsigned char *cell = src + tex_col;
            WrPix *dst = WR_ROW(s, yl * wr_px_sy + wr_origin_y) + (x * wr_px_sx + wr_origin_x);
            int pitch = WR_PITCH(s, yl * wr_px_sy + wr_origin_y);
            int y;
            for (y = yl; y <= yh; y++, dst += pitch * wr_px_sy, v += vstep) {
                unsigned char idx = cell[((unsigned) (v >> WR_FRACBITS)) * (unsigned) src_w];
#ifdef WR_16BPP
                /* The buffer holds u16 display values, not palette
                 * indices - the fuzz table is indexed by palette index,
                 * so the readback goes through pix2idx first. */
                if (idx != 0) {
                    WrPix pv = fuzzmap
                        ? fuzzmap[s->wr_pix2idx ? s->wr_pix2idx[dst[0]] : 0]
                        : cmap[idx];
#else
                if (idx != 0) {
                    WrPix pv = fuzzmap ? fuzzmap[dst[0]] : cmap[idx];
#endif
                    dst[0] = pv;
                    if (wr_px_sx > 1) dst[1] = pv;
                    if (wr_px_sy > 1) {
                        dst[pitch] = pv;
                        if (wr_px_sx > 1) dst[pitch + 1] = pv;
                    }
                }
            }

            /* Masked-midtexture pass B: fence columns NEARER than this
             * sprite (smaller iscale) were drawn in pass A and may have
             * been overpainted by the sprite - redraw them on top,
             * farthest of the nearer entries first. Records ascend in
             * iscale, so the nearer set is the prefix; walk it backwards. */
            {
                int mi = (int) wr_mtex_n[x] - 1;
                while (mi >= 0 && wr_mtex[x][mi].iscale >= iscale) mi--;
                for (; mi >= 0; mi--) wr_mtex_draw(s, x, &wr_mtex[x][mi]);
            }
            drew = 1;
        }
        if (drew) {
            if (p->sx1 < dirty_x1) dirty_x1 = p->sx1;
            if (p->ytop < dirty_y1) dirty_y1 = p->ytop;
            if (p->sx2 - 1 > dirty_x2) dirty_x2 = p->sx2 - 1;
            if (p->ybot - 1 > dirty_y2) dirty_y2 = p->ybot - 1;
        }
    }

    if (dirty_x2 >= dirty_x1 && dirty_y2 >= dirty_y1) {
        wr_host_present(s, wr_origin_x + dirty_x1, wr_origin_y + dirty_y1,
                        dirty_x2 - dirty_x1 + 1, dirty_y2 - dirty_y1 + 1);
        s->frame_dirty = true;
    }
}

#endif /* RELIC_WR_RENDER_H */
