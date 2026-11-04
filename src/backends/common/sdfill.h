/*
 * sdfill.h
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

/* sdfill.h - BBC-exact TROOMS fill renderer for desktop backends.
 *
 * @lines3d fill mode (mode bit 7, indexed blob bit 5) as the BBC draws
 * it, end to end, into a MODE 2 framebuffer image: the parasite half of
 * the BBC backend (projection, culls, staging, depth sort and
 * the wall-record build of l3_occ_wall_fill / hd_emit_wall) and the host
 * half of the BBC host-driver asm (hd_wexp column walk, hd_fill
 * dither, ht_exp texture strips, F9 view setup, F8 void fill). Integer
 * math, table reciprocals and record byte packing are mirrored so a
 * desktop frame is the frame the BBC would draw - the desktop is the
 * design reference, the BBC the port.
 *
 * Deliberate differences from the current BBC code (each a BBC bug the
 * port surfaced, to be fixed on the BBC side in turn):
 *   - textured face strips paint all bP-aP+1 rows (hosttex.S paints one
 *     short, leaving the bottom face row stale - flicker between banks)
 *   - wall heights use the full i32 projection; the BBC's per-vertex
 *     height cache truncates to i16 and wraps for walls hugging the
 *     near plane
 *   - the run cache is not modelled: it only skips rows that already
 *     hold the right colour, so a correct cache draws the same image
 *   - sdf_sort 2 (desktop TGX_SDF_HIRES) orders walls exactly by a
 *     pairwise segment-occlusion test; the BBC's nearest-end bucket
 *     order draws a pillar face before the pit rim that should clip it
 *     (full-height columns, far floor over near floor). sdf_nstcap and
 *     sdf_colw likewise lift the 24-wall cap and 16-px clip columns
 *   - the pillar texture page is a plain panel border (u==0, v==0 or
 *     v==15): the old 8-texel block courses banded narrow faces into
 *     'b'/'d' silhouettes. l3_texel in the BBC backend is its copy
 *
 * sdf_sort selects the front-to-back order: 0 = the BBC's 48 one-unit
 * depth buckets (exact), 1 = a full nearest-depth sort.
 *
 * Output: sdf_fb[row][byte column], MODE 2 bytes (2 px each); decode a
 * pixel with sdf_px(). Rows 0..223 are the view; the HUD band below is
 * the app's. */

#ifndef RELIC_SDFILL_H
#define RELIC_SDFILL_H

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define SDF_NEAR    64
#define SDF_POSTD   ((i32)64 << 8)
#define SDF_SCRW    320
#define SDF_SCRH    256
#define SDF_VROWS   224          /* filled view rows (bottom 32 = HUD) */
#define SDF_COLW    16           /* BBC clip column width, logical px */
#define SDF_NCOL    (SDF_SCRW / SDF_COLW)
#define SDF_MAXCOL  80           /* widest: 4-px columns (TGX_SDF_COLW) */
#define SDF_NST     24           /* staged walls (BBC L3NST, HDWQ slots) */
#define SDF_MTN     256
#define SDF_MAXV    256
#define SDF_MAXW    512
#define SDF_LM16(a, b)   ((i32)(i16)(a) * (i32)(i16)(b))
#define SDF_LM3216(a, b) ((i32)(a) * (i32)(i16)(b))

static u8  sdf_fb[SDF_SCRH][80];
static u8  sdf_palmap[16];
static int sdf_paln;
static u8  sdf_tex[5][256];      /* MODE 2 texel bytes, [u*16 + v] */
static int sdf_texok;
static u32 sdf_cmt;              /* frame commits (computer lamp blink) */
static int sdf_sort = 0;
static int sdf_nwalls_staged, sdf_nwalls_drawn;
static int sdf_dbg = 0;          /* TGX_SDF_DBG: span trace to stderr */
static int sdf_ewall = 0;        /* emit order of the current record */
static int sdf_dbgc[16];         /* stage-reject tallies when sdf_dbg */

/* parasite-side state (BBC l3 statics) */
static i32 sdf_mt[2][SDF_MTN];
static i32 sdf_mtg[2] = { 0x7FFFFFFF, 0x7FFFFFFF };
static u16 sdf_rtab[176], sdf_drt[129];
static u8  sdf_drth[127];
/* Desktop may stage more than the BBC's 24 (TGX_SDF_NST); arrays are
 * sized for the max, sdf_nstcap is the live limit. */
#define SDF_NSTMAX  128
static int sdf_nstcap = SDF_NST;
static i16 sdf_s[SDF_NSTMAX][16];   /* sx1 sb1 st1 sx2 sb2 st2 fl cl z1 z2
                                     * sc1 sf1 sc2 sf2 u1 u2 */
static i32 sdf_vw[SDF_NSTMAX][4];   /* view-space x1 z1 x2 z2 (8.8), for
                                     * the exact order (sdf_sort 2) */
static u8  sdf_ord[SDF_NSTMAX];
static int sdf_nst;
static int sdf_texon, sdf_vc0, sdf_vc1, sdf_band;
static int sdf_ccl, sdf_cfl, sdf_cvd, sdf_csk, sdf_viewh;
/* l3_occ_wall_fill -> hd_emit_wall accumulators */
static i32 sdf_fty, sdf_fby, sdf_fdt, sdf_fdb, sdf_fcy, sdf_ffy, sdf_fdc,
           sdf_fdf, sdf_fdu, sdf_fk, sdf_fdk;
static u8  sdf_fu1, sdf_fneg, sdf_ftv;

/* host-side state (BBC hd_* bytes) */
static u8  sdf_vwt[SDF_MAXCOL], sdf_vwb[SDF_MAXCOL];
static u8  sdf_vcol[4];
static int sdf_vh, sdf_vcl, sdf_vcr;

/* Clip column granularity: the BBC runs 16 logical px (4 MODE 2 bytes);
 * desktop testing can go finer - every per-column step and byte offset
 * scales off these. */
static int sdf_colw = SDF_COLW, sdf_ncol = SDF_NCOL, sdf_cbw = 4;

static void sdf_set_colw(int px) {
    if (px < 4) px = 4;
    if (px > 32) px = 32;
    if (px & 3) px &= ~3;             /* whole MODE 2 byte columns */
    sdf_colw = px;
    sdf_ncol = SDF_SCRW / px;
    sdf_cbw = px / 4;
    if (sdf_ncol > SDF_MAXCOL) sdf_ncol = SDF_MAXCOL;
}

static i16 sdf_blob[1 + SDF_MAXV * 2 + SDF_MAXW * 6];

/* ---- palette (BBC palette upload + colour lookup) ---------------------- */
/* MODE 2 colour = R|G<<1|B<<2 at a 128 threshold; transparent -> 0. */
static void sdf_palette(const u8 *rgba, int n) {
    int i;
    if (n > 16) n = 16;
    sdf_paln = 0;
    for (i = 0; i < n; i++) {
        const u8 *p = rgba + i * 4;
        sdf_palmap[i] = (u8)(p[3] < 128 ? 0 :
            (p[0] > 127 ? 1 : 0) | (p[1] > 127 ? 2 : 0) | (p[2] > 127 ? 4 : 0));
        sdf_paln++;
    }
}

/* Palette index | dither bits -> wire colour word (physical | bits). */
static u8 sdf_pm(int c) {
    int m = sdf_paln > 1 ? sdf_palmap[c & (sdf_paln - 1)] : (c & 7);
    return (u8)(m | (c & 48));
}

/* ---- textures (BBC texel generators) ------------------- */
static const u8 sdf_texof[16] = {
    0, 1, 2, 0, 4, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static u8 sdf_texel(int tx, int u, int v) {
    static const u8 solid[4] = { 0x3F, 0x0F, 0x03, 0x0C };
    u8 c = solid[tx > 3 ? 3 : tx], dim = c & ((v & 1) ? 0x55 : 0xAA);
    switch (tx) {
    case 0:  return (u == 0 || v == 0) ? dim : c;
    case 1:  return v == 0 ? dim : c;
    case 2:
        if (u == 0 || v == 0 || (v & 15) == 15) return dim;
        return c;
    default:
        if (u == 0 || v == 0 || v == 8) return dim;
        if (v == 4 && (u & 3) == 2) return tx == 3 ? 0x3F : dim;
        return c;
    }
}

/* Desktop texture styles: 0 = the BBC's procedural pages. Per-project
 * baked pages (styles 1+) are generated outside this tree and are not
 * part of the runtime source. */
static int sdf_texstyle = 0;

static void sdf_texinit(void) {
    int tx, u, v;
    if (sdf_texok == sdf_texstyle + 1) return;
    for (tx = 0; tx < 5; tx++)
        for (u = 0; u < 16; u++)
            for (v = 0; v < 16; v++)
                sdf_tex[tx][u * 16 + v] = sdf_texel(tx, u, v);
    sdf_texok = sdf_texstyle + 1;
}

/* ---- MODE 2 raster (hd_fill) ---------------------------------------- */
static const u8 sdf_pentab[16] = {
    0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15,
    0x40, 0x41, 0x44, 0x45, 0x50, 0x51, 0x54, 0x55
};

/* Byte rectangle: w byte columns from xb, rows yt..yb inclusive. Both
 * pixels of a byte take the colour; bits 4-5 select the dither. Rows
 * alternate in 2-row blocks (A A B B), byte columns alternate by
 * parity: checker = even cols ink A rows, odd cols B rows; sparse =
 * odd cols ink only the right pixel of B rows. */
static void sdf_fill(int xb, int w, int yt, int yb, int col) {
    u8 pen = (u8)(sdf_pentab[col & 15] * 3);
    int d = col & 0x30, y, j;
    if (yb < yt || w == 0) return;
    if (yb >= SDF_SCRH) yb = SDF_SCRH - 1;
    for (j = 0; j < w; j++) {
        int bc = xb + j;
        u8 a, b;
        if (bc < 0 || bc >= 80) continue;
        if (!d)        { a = pen; b = pen; }
        else if (!(bc & 1)) { a = pen; b = 0; }
        else           { a = 0; b = (d & 0x20) ? (u8)(pen & 0x55) : pen; }
        for (y = yt; y <= yb; y++)
            sdf_fb[y][bc] = ((y >> 1) & 1) ? b : a;
    }
}

/* hd_span: rows [s0, end) of clip column c (u8 bounds, empty = none). */
static void sdf_span(int c, int s0, int end, int col) {
    s0 &= 255; end &= 255;
    if (end <= s0) return;
    sdf_fill(c * sdf_cbw, sdf_cbw, s0, end - 1, col);
}

/* Decoded physical colour of pixel (x 0..159, row). */
static int sdf_px(int x, int row) {
    u8 b = sdf_fb[row][x >> 1];
    if (!(x & 1))
        return ((b >> 1) & 1) | (((b >> 3) & 1) << 1) |
               (((b >> 5) & 1) << 2) | (((b >> 7) & 1) << 3);
    return (b & 1) | (((b >> 2) & 1) << 1) |
           (((b >> 4) & 1) << 2) | (((b >> 6) & 1) << 3);
}

/* ---- textured face strip (hosttex.S ht_exp) ------------------------ */
static const u8 sdf_dm[3][32] = {
    { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
      0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF },
    { 0xFF,0xFF,0x00,0x00,0xFF,0xFF,0x00,0x00, 0x00,0x00,0xFF,0xFF,0x00,0x00,0xFF,0xFF,
      0xFF,0xFF,0x00,0x00,0xFF,0xFF,0x00,0x00, 0x00,0x00,0xFF,0xFF,0x00,0x00,0xFF,0xFF },
    { 0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0xFF,0xFF,0x00,0x00,0x00,0x00,
      0x00,0x00,0x00,0x00,0xFF,0xFF,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF }
};

static u16 sdf_rd16(const u8 *r, int o) { return (u16)(r[o] | (r[o + 1] << 8)); }
static u32 sdf_rd24(const u8 *r, int o) {
    return (u32)r[o] | ((u32)r[o + 1] << 8) | ((u32)r[o + 2] << 16);
}

static void sdf_texstrip(int cx, int aP, int bP, const u8 *rec, int wcl) {
    int h = bP - aP + 1;          /* rows aP-1 .. bP-1 (BBC: one short) */
    int tx = rec[29] & 0x7F, page, j, r, row0 = aP - 1;
    u16 T = sdf_rd16(rec, 5), B = sdf_rd16(rec, 9);
    u32 ts = (u32)(T >> 4), H = (u32)((u16)(B - T) >> 4), K, n;
    u16 u, du4, va, q;
    i32 skip;
    const u8 *mask = sdf_dm[(wcl & 0x30) == 0x30 ? 2 : (wcl & 0x30) >> 4];
    int masked = (wcl & 0x30) != 0;
    u8 ui[8];
    if (h <= 0) return;
    if (tx < 1 || tx > 4) return;
    page = tx - 1;
    if (tx == 4 && (sdf_cmt & 8)) page = 4;
    if (H == 0) H = 1;
    K = sdf_rd24(rec, 23);
    n = (K / H) & 0xFFFFFF;
    if (rec[29] & 0x80) u = (u16)(rec[21] - (n & 0xFFFF));
    else                u = (u16)(rec[21] + (n & 0xFFFF));
    du4 = (u16)(i16)(signed char)rec[22];
    u = (u16)(u << 4); du4 = (u16)(du4 << 4);
    /* du4 is packed as a per-column u step; the strip needs it per MODE 2
     * byte, so rescale by 4/cbw (1,2 or 4 byte cols -> x4, x2, x1). */
    du4 = (u16)((i32)(i16)du4 * 4 / sdf_cbw);
    for (j = 0; j < sdf_cbw; j++) { ui[j] = (u8)(((u >> 8) & 15) << 4); u = (u16)(u + du4); }
    skip = (i32)(row0 + 0x800) - (i32)ts;
    if (skip < 0) skip = 0;
    q = (u16)((((u32)rec[30] << 8)) / H);
    va = (u16)((u32)skip * q);
    for (r = 0; r < h; r++) {
        int y = row0 + r, v = (va >> 8) & 15;
        if (y >= 0 && y < SDF_SCRH) {
            for (j = 0; j < sdf_cbw; j++) {
                int bc = cx * sdf_cbw + j;
                u8 t = sdf_tex[page][ui[j] + v];
                if (masked) t &= mask[(y & 7) + (bc & 3) * 8];
                if (bc < 80) sdf_fb[y][bc] = t;
            }
        }
        va = (u16)(va + q);
    }
}

/* ---- wall record expansion (BBC wall-record expansion) ---------------------- */
/* Edge -> clipped P (row+1): edge = row*16 + $8000 (u16). */
static int sdf_edge(u16 e, int plo, int phi) {
    int t = e >> 4;
    if (t < 0x7FF) return plo;
    t -= 0x7FF;
    if (t >= 256) return phi;
    if (t < plo) return plo;
    if (t > phi) return phi;
    return t;
}

static void sdf_wexp(u8 *rec) {
    int wc0 = rec[0], wc1 = rec[1], x;
    int wfl = rec[2], wcl = rec[3], wfc = rec[4];
    if (wc0 >= sdf_ncol || wc1 >= sdf_ncol) return;
    if (wc0 < sdf_vcl) wc0 = sdf_vcl;
    if (wc1 >= sdf_vcr) wc1 = sdf_vcr - 1;
    if (wc1 < wc0) return;
    if (!(wfl & 4)) memcpy(rec + 13, rec + 5, 8);   /* solid: C,F = T,B */
    for (x = wc0; x <= wc1; x++) {
        if (sdf_vwb[x] >= sdf_vwt[x]) {
            int wb = sdf_vwb[x], wt = sdf_vwt[x], bhi = wb + 1;
            int aP = sdf_edge(sdf_rd16(rec, 5),  wt + 1, wb + 2);
            int kP = sdf_edge(sdf_rd16(rec, 13), wt + 1, wb + 2);
            int bP = sdf_edge(sdf_rd16(rec, 9),  wt, bhi);
            int fP = sdf_edge(sdf_rd16(rec, 17), wt, bhi);
            int ccol = (wfl & 8) ? sdf_vcol[3] : sdf_vcol[0];
            if (sdf_dbg)
                fprintf(stderr, "w%d c%d win %d-%d aP %d kP %d bP %d fP %d"
                        " fl %02x cl %02x fc %02x cc %02x\n",
                        sdf_ewall, x, wt, wb, aP, kP, bP, fP,
                        wfl, wcl, wfc, ccol);
            sdf_span(x, wt, (kP < aP ? kP : aP) - 1, ccol);   /* ceiling */
            sdf_span(x, kP - 1, aP - 1, wcl);                 /* lintel */
            if (!(wfl & 4)) {
                if (wfl & 0x20) {                  /* landscape window */
                    int hz = 128 < aP - 1 ? aP - 1 : 128 > bP ? bP : 128;
                    sdf_span(x, aP - 1, hz, sdf_vcol[3]);   /* sky */
                    sdf_span(x, hz, bP, wcl);              /* ground */
                } else if (wfl & 0x10) sdf_texstrip(x, aP, bP, rec, wcl);
                else            sdf_span(x, aP - 1, bP, wcl);  /* face */
            }
            sdf_span(x, bP, fP, wcl);                         /* step */
            sdf_span(x, fP >= bP ? fP : bP, bhi, wfc);        /* floor */
            if ((wfl & 4) && bP >= aP) {
                sdf_vwb[x] = (u8)(bP - 1); sdf_vwt[x] = (u8)(aP - 1);
            } else {
                sdf_vwt[x] = 255; sdf_vwb[x] = 0;
            }
        }
        /* step: K plain 24-bit add, edges pin on u16 wrap */
        {
            u32 k = (sdf_rd24(rec, 23) + sdf_rd24(rec, 26)) & 0xFFFFFF;
            int o;
            rec[23] = (u8)k; rec[24] = (u8)(k >> 8); rec[25] = (u8)(k >> 16);
            for (o = 5; o <= 17; o += 4) {
                u32 e = sdf_rd16(rec, o), st = sdf_rd16(rec, o + 2), sum = e + st;
                if (!(st & 0x8000)) { if (sum > 0xFFFF) sum = 0xF000; }
                else                { if (sum <= 0xFFFF) sum = 0x1000; }
                rec[o] = (u8)sum; rec[o + 1] = (u8)(sum >> 8);
            }
        }
    }
    sdf_nwalls_drawn++;
}

/* F8: void-fill every still-open column, black letterbox bars, black
 * band below a short view. */
static void sdf_void(void) {
    int x;
    for (x = 0; x < sdf_ncol; x++) {
        if (x < sdf_vcl || x >= sdf_vcr) {
            sdf_span(x, 0, SDF_VROWS, 0);
            continue;
        }
        if (sdf_vwb[x] >= sdf_vwt[x])
            sdf_span(x, sdf_vwt[x], sdf_vwb[x] + 1, sdf_vcol[2]);
        if (sdf_vh < SDF_VROWS) sdf_span(x, sdf_vh, SDF_VROWS, 0);
    }
}

/* ---- parasite: record build (hd_emit_wall) -------------------------- */
static void sdf_emit(int c0, int c1, int fl, int cl) {
    u8 rec[32];
    int tx = (sdf_texon && !(fl & 0x24)) ? sdf_texof[cl & 15] : 0, fw;
    if (tx && (sdf_fdu >= 2048 || sdf_fdu <= -2048)) tx = 0;
    fw = (fl >> 8) & 255;
    memset(rec, 0, sizeof rec);
    rec[0] = (u8)c0; rec[1] = (u8)c1;
    rec[2] = (u8)(fl | (tx ? 0x10 : 0));
    rec[3] = sdf_pm(cl);
    rec[4] = sdf_pm(fw);
    rec[5]  = (u8)sdf_fty; rec[6]  = (u8)(sdf_fty >> 8);
    rec[7]  = (u8)sdf_fdt; rec[8]  = (u8)(sdf_fdt >> 8);
    rec[9]  = (u8)sdf_fby; rec[10] = (u8)(sdf_fby >> 8);
    rec[11] = (u8)sdf_fdb; rec[12] = (u8)(sdf_fdb >> 8);
    rec[13] = (u8)sdf_fcy; rec[14] = (u8)(sdf_fcy >> 8);
    rec[15] = (u8)sdf_fdc; rec[16] = (u8)(sdf_fdc >> 8);
    rec[17] = (u8)sdf_ffy; rec[18] = (u8)(sdf_ffy >> 8);
    rec[19] = (u8)sdf_fdf; rec[20] = (u8)(sdf_fdf >> 8);
    if (tx) {
        rec[21] = sdf_fu1;
        rec[22] = (u8)(sdf_fdu >> 6);
        rec[23] = (u8)sdf_fk; rec[24] = (u8)(sdf_fk >> 8); rec[25] = (u8)(sdf_fk >> 16);
        rec[26] = (u8)sdf_fdk; rec[27] = (u8)(sdf_fdk >> 8); rec[28] = (u8)(sdf_fdk >> 16);
        rec[29] = (u8)(tx | (sdf_fneg ? 0x80 : 0));
        rec[30] = sdf_ftv;
    }
    sdf_wexp(rec);
}

static i32 sdf_bias(i32 v) {
    v += 0x8000;
    if (v < 0x1000) v = 0x1000;
    if (v > 0xF000) v = 0xF000;
    return v;
}
static i32 sdf_step(i32 v) {
    if (v < -0x7000) v = -0x7000;
    if (v > 0x7000) v = 0x7000;
    return v;
}
static i32 sdf_shr(i32 p, int sh) { return p < 0 ? -((-p) >> sh) : p >> sh; }

/* l3_occ_wall_fill (wall-record mode). */
static void sdf_wall(const i16 *s) {
    i16 sx1 = s[0], sb1 = s[1], st1 = s[2];
    i16 sx2 = s[3], sb2 = s[4], st2 = s[5];
    i16 sc1 = s[10], sf1 = s[11], sc2 = s[12], sf2 = s[13];
    i16 su1 = s[14], su2 = s[15];
    i16 fl = s[6], cl = s[7], t, x0, x1;
    int solid = !(fl & 4), c0, c1;
    sdf_ewall++;
    if (sdf_dbg)
        fprintf(stderr, "wall #%d sx %d..%d sb %d..%d st %d..%d fl %04x"
                " cl %02x z %d %d scf %d %d %d %d u %d %d\n", sdf_ewall,
                sx1, sx2, sb1, sb2, st1, st2, fl & 0xFFFF, cl & 0xFF,
                s[8], s[9], sc1, sf1, sc2, sf2, su1, su2);
    if (solid) {
        sdf_ftv = (u8)sc1;
        sc1 = st1; sf1 = sb1; sc2 = st2; sf2 = sb2;
    }
    if ((sf1 < 0 && sf2 < 0) || (sc1 > SDF_SCRH - 1 && sc2 > SDF_SCRH - 1))
        { sdf_dbgc[9]++; return; }
    if (sx1 == sx2) {
        i32 lo = sb1, hi = sb1;
        if (!solid) return;
        if (sb2 < lo) lo = sb2;
        if (st1 < lo) lo = st1;
        if (st2 < lo) lo = st2;
        if (sb2 > hi) hi = sb2;
        if (st1 > hi) hi = st1;
        if (st2 > hi) hi = st2;
        if (sx1 >= sdf_vc0 * sdf_colw && sx1 < (sdf_vc1 + 1) * sdf_colw) {
            i32 ty = (lo << 4) + 0x8000, by = (hi << 4) + 0x8000;
            if (ty < 0x1000) ty = 0x1000;
            if (ty > 0xF000) ty = 0xF000;
            if (by < 0x1000) by = 0x1000;
            if (by > 0xF000) by = 0xF000;
            sdf_fty = ty; sdf_fby = by; sdf_fdt = 0; sdf_fdb = 0;
            sdf_fu1 = (u8)su1; sdf_fneg = 0;
            sdf_fdu = 0; sdf_fk = 0; sdf_fdk = 0;
            sdf_emit(sx1 / sdf_colw, sx1 / sdf_colw, fl, cl);
        }
        return;
    }
    if (sx2 < sx1) {
        t = sx1; sx1 = sx2; sx2 = t;
        t = sb1; sb1 = sb2; sb2 = t;
        t = st1; st1 = st2; st2 = t;
        t = sc1; sc1 = sc2; sc2 = t;
        t = sf1; sf1 = sf2; sf2 = t;
        t = su1; su1 = su2; su2 = t;
    }
    x0 = sx1 < 0 ? 0 : sx1;
    x1 = sx2 > SDF_SCRW - 1 ? SDF_SCRW - 1 : sx2;
    {
        i32 vx0 = sdf_vc0 * sdf_colw, vx1 = (sdf_vc1 + 1) * sdf_colw - 1;
        if (x0 < vx0) x0 = (i16)vx0;
        if (x1 > vx1) x1 = (i16)vx1;
    }
    if (x0 > x1) return;
    c0 = x0 / sdf_colw; c1 = x1 / sdf_colw;
    {
        i32 dxw = sx2 - sx1, pt, pb, pu, pc = 0, pf = 0, r;
        int sh = 0;
        i32 dt, db, du, dc, df;
        while (dxw > 255) { dxw >>= 1; sh++; }
        r = dxw > 128 ? (i32)sdf_drth[dxw - 129] : (i32)sdf_drt[dxw];
        pt = SDF_LM16(st2 - st1, r);
        pb = SDF_LM16(sb2 - sb1, r);
        pu = SDF_LM16(su2 - su1, r);
        if (!solid) {
            pc = SDF_LM16(sc2 - sc1, r);
            pf = SDF_LM16(sf2 - sf1, r);
        }
        sh += 7;
        dt = sdf_shr(pt, sh); db = sdf_shr(pb, sh); du = sdf_shr(pu, sh);
        dc = sdf_shr(pc, sh); df = sdf_shr(pf, sh);
        sdf_fty = ((i32)st1 << 4) + (((x0 - sx1) * dt) >> 4);
        sdf_fby = ((i32)sb1 << 4) + (((x0 - sx1) * db) >> 4);
        sdf_fdt = (dt * sdf_colw) >> 4; sdf_fdb = (db * sdf_colw) >> 4;
        sdf_fcy = ((i32)sc1 << 4) + (((x0 - sx1) * dc) >> 4);
        sdf_ffy = ((i32)sf1 << 4) + (((x0 - sx1) * df) >> 4);
        sdf_fdc = (dc * sdf_colw) >> 4; sdf_fdf = (df * sdf_colw) >> 4;
        sdf_fdu = (du * sdf_colw) >> 4;
    }
    if (sdf_texon && solid) {
        i32 dut = (i32)su2 - su1, h2 = (i32)sb2 - st2, sp;
        sdf_fneg = dut < 0;
        if (dut < 0) dut = -dut;
        if (dut > 4095) dut = 4095;
        if (h2 < 1) h2 = 1;
        sp = (dut * h2) / (sx2 - sx1);
        sdf_fk = sp * (x0 - sx1);
        sdf_fdk = sp * sdf_colw;
        sdf_fu1 = (u8)su1;
    }
    sdf_fty = sdf_bias(sdf_fty); sdf_fby = sdf_bias(sdf_fby);
    sdf_fdt = sdf_step(sdf_fdt); sdf_fdb = sdf_step(sdf_fdb);
    if (!solid) {
        sdf_fcy = sdf_bias(sdf_fcy); sdf_ffy = sdf_bias(sdf_ffy);
        sdf_fdc = sdf_step(sdf_fdc); sdf_fdf = sdf_step(sdf_fdf);
    }
    sdf_emit(c0, c1, fl, cl);
}

/* ---- parasite: projection + staging (BBC fill path) ---- */
static i32 sdf_tm(int i, i32 w) {
    if (w >= 0) {
        if (w >= 65536) w = 65535;
        return w < 256 ? sdf_mt[i][w] : sdf_mt[i][w & 255] + (sdf_mt[i][w >> 8] << 8);
    }
    w = -w;
    if (w >= 65536) w = 65535;
    return w < 256 ? -sdf_mt[i][w] : -(sdf_mt[i][w & 255] + (sdf_mt[i][w >> 8] << 8));
}

static int sdf_dband(i32 nz) {
    if (nz > (SDF_POSTD << 1)) return 32;
    if (nz > SDF_POSTD)        return 16;
    return 0;
}

static void sdf_tables(i32 gs, i32 gc) {
    int k;
    if (gs != sdf_mtg[0] || gc != sdf_mtg[1]) {
        i32 a0 = 0, a1 = 0;
        for (k = 0; k < SDF_MTN; k++) {
            sdf_mt[0][k] = a0; sdf_mt[1][k] = a1;
            a0 += gs; a1 += gc;
        }
        sdf_mtg[0] = gs; sdf_mtg[1] = gc;
    }
    if (!sdf_rtab[1]) {
        sdf_rtab[0] = 20480;
        sdf_drt[0] = 32767; sdf_drt[1] = 32767;
        for (k = 1; k < 176; k++) sdf_rtab[k] = (u16)(20480 / k);
        for (k = 2; k < 129; k++)
            sdf_drt[k] = (u16)(32768 / k < 32767 ? 32768 / k : 32767);
        for (k = 0; k < 127; k++) sdf_drth[k] = (u8)(32768 / (k + 129));
    }
}

static i16 sdf_c2047(i32 v) { return (i16)(v < -2047 ? -2047 : v > 2047 ? 2047 : v); }

static void sdf_stage(const i16 *vt, int nv, const i16 *wp, int nw,
                      i32 cx, i32 cz, i32 gs, i32 gc, i32 eyew, int rmask) {
    i32 az = (SDF_LM3216(cx, gs) + SDF_LM3216(cz, gc)) / 256;
    i32 ax = (SDF_LM3216(cx, gc) - SDF_LM3216(cz, gs)) / 256;
    i32 cxw = cx >> 8, czw = cz >> 8;
    int i;
    sdf_nst = 0;
    for (i = 0; i < nw; i++) {
        const i16 *rh = wp + i * 6;
        i32 z1, x1, z2, x2, sx1, sx2, sb1, sb2, st1, st2, fl, cl, f;
        i32 sc1 = 0, sf1 = 0, sc2 = 0, sf2 = 0, fu1 = 0, fu2 = 0;
        i32 q1, q2, qy1, qy2, vq1 = 0, vq2 = 0, vsx1 = 0, vsx2 = 0;
        i32 wx1, wz1, wx2, wz2, dxw1, dzw1, m1, m2;
        int v1 = rh[0], v2 = rh[1], c1, c2, pleft;
        if (rmask && !(((i32)rh[2] >> 8) & rmask)) { sdf_dbgc[7]++; continue; }
        if ((u16)v1 >= (u16)nv || (u16)v2 >= (u16)nv) { sdf_dbgc[8]++; continue; }
        wx1 = vt[v1 * 2]; wz1 = vt[v1 * 2 + 1];
        wx2 = vt[v2 * 2]; wz2 = vt[v2 * 2 + 1];
        dxw1 = wx1 - cxw; dzw1 = wz1 - czw;
        m1 = labs(dxw1) + labs(dzw1);
        m2 = labs(wx2 - cxw) + labs(wz2 - czw);
        if (m1 > 160 && m2 > 160) { sdf_dbgc[0]++; continue; }
        {
            i32 wdx = wx2 - wx1, wdz = wz2 - wz1;
            i32 ext = labs(wdx) + labs(wdz), zm;
            zm = sdf_tm(0, (wx1 + wx2) / 2) + sdf_tm(1, (wz1 + wz2) / 2) - az;
            if (zm + (ext << 7) < SDF_NEAR) { sdf_dbgc[1]++; continue; }
        }
        fl = (i32)rh[4];
        if (!(fl & 4)) {
            i32 wdx = wx2 - wx1, wdz = wz2 - wz1;
            if (wdz == 0) {
                if (wdx == 0 || dzw1 == 0) { sdf_dbgc[2]++; continue; }
                if ((wdx < 0) != (dzw1 < 0)) { sdf_dbgc[2]++; continue; }
            } else if (wdx == 0) {
                if (dxw1 == 0) { sdf_dbgc[2]++; continue; }
                if ((wdz < 0) == (dxw1 < 0)) { sdf_dbgc[2]++; continue; }
            } else if (SDF_LM16(wdz, dxw1) - SDF_LM16(wdx, dzw1) >= 0) { sdf_dbgc[2]++; continue; }
        }
        pleft = 0;
        if (fl & 4) {
            i32 wdx = wx2 - wx1, wdz = wz2 - wz1;
            pleft = SDF_LM16(wdz, dxw1) - SDF_LM16(wdx, dzw1) >= 0;
        }
        /* l3vfill for both ends */
        z1 = sdf_tm(0, wx1) + sdf_tm(1, wz1) - az;
        x1 = sdf_tm(1, wx1) - sdf_tm(0, wz1) - ax;
        z2 = sdf_tm(0, wx2) + sdf_tm(1, wz2) - az;
        x2 = sdf_tm(1, wx2) - sdf_tm(0, wz2) - ax;
        if (z1 >= SDF_NEAR) {
            i32 zw = z1 >> 8;
            vq1 = zw > 175 ? 90 : (i32)sdf_rtab[zw < 1 ? 1 : zw];
            vsx1 = (i16)(160 + (int)(SDF_LM16(x1 / 16, vq1) >> 11));
        }
        if (z2 >= SDF_NEAR) {
            i32 zw = z2 >> 8;
            vq2 = zw > 175 ? 90 : (i32)sdf_rtab[zw < 1 ? 1 : zw];
            vsx2 = (i16)(160 + (int)(SDF_LM16(x2 / 16, vq2) >> 11));
        }
        if ((x1 > z1 && x2 > z2) || (x1 < -z1 && x2 < -z2)) { sdf_dbgc[3]++; continue; }
        c1 = z1 >= SDF_NEAR; c2 = z2 >= SDF_NEAR;
        if (!c1 && !c2) { sdf_dbgc[4]++; continue; }
        if (z1 < SDF_NEAR) {
            f = ((SDF_NEAR - z1) * 256) / (z2 - z1);
            x1 = x1 + ((x2 - x1) * f) / 256;
            z1 = SDF_NEAR;
            fu1 = f;
        }
        if (z2 < SDF_NEAR) {
            f = ((SDF_NEAR - z2) * 256) / (z1 - z2);
            x2 = x2 + ((x1 - x2) * f) / 256;
            z2 = SDF_NEAR;
            c2 = 0;
            fu2 = f;
        }
        {
            i32 dyb = ((i32)rh[2] & 255) - eyew;
            i32 dyt = ((i32)rh[3] & 255) - eyew;
            q1 = c1 ? vq1 : 20480;
            q2 = c2 ? vq2 : 20480;
            qy1 = (q1 * 3) >> 1; qy2 = (q2 * 3) >> 1;
            sx1 = c1 ? vsx1 : 160 + (int)(SDF_LM16(x1 / 16, q1) >> 11);
            sx2 = c2 ? vsx2 : 160 + (int)(SDF_LM16(x2 / 16, q2) >> 11);
            sb1 = 128 - (int)(SDF_LM16(dyb, qy1) >> 7);
            st1 = 128 - (int)(SDF_LM16(dyt, qy1) >> 7);
            sb2 = 128 - (int)(SDF_LM16(dyb, qy2) >> 7);
            st2 = 128 - (int)(SDF_LM16(dyt, qy2) >> 7);
            if (fl & 4) {
                i32 ch = ((i32)rh[3] >> 8) & 255, dyc, dyf;
                if (pleft) {
                    i32 cl6 = (((i32)rh[4] >> 4) & 15) | (((i32)rh[5] >> 2) & 48);
                    if (cl6) ch = cl6;
                }
                dyc = (ch ? ch : ((i32)rh[3] & 255)) - eyew;
                /* near floor = opening bottom - its 4-bit drop */
                dyf = ((i32)rh[2] & 255) - ((fl >> (pleft ? 12 : 8)) & 15) - eyew;
                sc1 = 128 - (int)(SDF_LM16(dyc, qy1) >> 7);
                sf1 = 128 - (int)(SDF_LM16(dyf, qy1) >> 7);
                sc2 = 128 - (int)(SDF_LM16(dyc, qy2) >> 7);
                sf2 = 128 - (int)(SDF_LM16(dyf, qy2) >> 7);
            }
        }
        if (sx1 < 0 && sx2 < 0) { sdf_dbgc[5]++; continue; }
        if (sx1 >= SDF_SCRW && sx2 >= SDF_SCRW) { sdf_dbgc[6]++; continue; }
        cl = (i32)rh[5] & 63;
        if (sdf_band) cl += sdf_dband(z1 < z2 ? z1 : z2);
        {
            i16 *s;
            int slot = sdf_nst;
            if (sdf_nst >= sdf_nstcap) {
                i32 mz = z1 < z2 ? z1 : z2, worst = -1;
                int j;
                slot = -1;
                for (j = 0; j < sdf_nstcap; j++) {
                    i32 m = sdf_s[j][8] < sdf_s[j][9] ? sdf_s[j][8] : sdf_s[j][9];
                    if (m > worst) { worst = m; slot = j; }
                }
                if (mz >= worst) continue;
            }
            s = sdf_s[slot];
            if (sdf_dbg > 1)
                fprintf(stderr, "stage slot=%d wx=%d,%d..%d,%d "
                        "vx=%d,%d..%d,%d sx=%d..%d\n",
                        slot, (int)wx1, (int)wz1, (int)wx2, (int)wz2,
                        (int)x1, (int)z1, (int)x2, (int)z2,
                        (int)sx1, (int)sx2);
            sdf_vw[slot][0] = x1; sdf_vw[slot][1] = z1;
            sdf_vw[slot][2] = x2; sdf_vw[slot][3] = z2;
            s[0] = (i16)(sx1 < -32768 ? -32768 : sx1 > 32767 ? 32767 : sx1);
            s[1] = sdf_c2047(sb1); s[2] = sdf_c2047(st1);
            s[3] = (i16)(sx2 < -32768 ? -32768 : sx2 > 32767 ? 32767 : sx2);
            s[4] = sdf_c2047(sb2); s[5] = sdf_c2047(st2);
            s[7] = (i16)cl;
            s[8] = (i16)(z1 > 32767 ? 32767 : z1);
            s[9] = (i16)(z2 > 32767 ? 32767 : z2);
            {
                i32 fv = ((i32)rh[5] >> (pleft ? 12 : 8)) & 15, fw;
                fw = fv ? ((fv & 7) | ((fv & 8) ? 32 : 16)) : sdf_cfl;
                /* solid + record flags bit 8: landscape (sky over ground,
             * split at the horizon) - staged as flags bit 5 */
            s[6] = (i16)((fl & 15) | ((!(fl & 4) && (fl & 0x100)) ? 0x20 : 0) |
                         (fw << 8));
            }
            s[10] = sdf_c2047(sc1); s[11] = sdf_c2047(sf1);
            s[12] = sdf_c2047(sc2); s[13] = sdf_c2047(sf2);
            {
                i32 wdx = labs(wx2 - wx1), wdz = labs(wz2 - wz1);
                i32 wl = wdx > wdz ? wdx : wdz;
                s[14] = (i16)((fu1 * wl) >> 3);
                s[15] = (i16)((wl << 5) - ((fu2 * wl) >> 3));
            }
            if (!(fl & 4))
                s[10] = (i16)((((i32)rh[3] & 255) - ((i32)rh[2] & 255)) * 2);
            if (slot == sdf_nst) {
                sdf_ord[sdf_nst] = (u8)sdf_nst;
                sdf_nst++;
            }
        }
    }
}

static i32 sdf_mz(int k) {
    const i16 *s = sdf_s[k];
    return s[8] < s[9] ? (i32)s[8] : (i32)s[9];
}

/* l3_occ_sort: 48 one-world-unit buckets by nearest-end depth, stable
 * within a bucket (sdf_sort 0); or a full stable sort (sdf_sort 1). */
/* Exact visibility order for 2D segments, camera at the view origin.
 * side(L, p) > 0: p on the camera's side of L's line. A occludes B when
 * B lies wholly beyond A's line, or A wholly on the camera side of B's.
 * Only pairs whose screen x ranges overlap constrain the order. */
static int sdf_side(const i32 *l, i32 px, i32 pz) {
    long long ex = (long long)l[2] - l[0], ez = (long long)l[3] - l[1];
    long long cam = ex * (0 - (long long)l[1]) - ez * (0 - (long long)l[0]);
    long long p   = ex * ((long long)pz - l[1]) - ez * ((long long)px - l[0]);
    /* within ~1/8 world unit of the line (8.8 coords) counts as on it */
    long long tol = 32 * ((ex < 0 ? -ex : ex) + (ez < 0 ? -ez : ez));
    if (p >= -tol && p <= tol) return 0;
    return ((p > 0) == (cam > 0)) ? 1 : -1;
}
static int sdf_occludes(int a, int b) {
    const i32 *A = sdf_vw[a], *B = sdf_vw[b];
    int b1 = sdf_side(A, B[0], B[1]), b2 = sdf_side(A, B[2], B[3]);
    int a1, a2;
    if (b1 <= 0 && b2 <= 0 && (b1 | b2)) return 1;   /* B beyond A */
    a1 = sdf_side(B, A[0], A[1]); a2 = sdf_side(B, A[2], A[3]);
    if (a1 >= 0 && a2 >= 0 && (a1 | a2)) return 1;   /* A before B */
    return 0;
}
static int sdf_xover(int a, int b) {
    const i16 *A = sdf_s[a], *B = sdf_s[b];
    int al = A[0] < A[3] ? A[0] : A[3], ar = A[0] < A[3] ? A[3] : A[0];
    int bl = B[0] < B[3] ? B[0] : B[3], br = B[0] < B[3] ? B[3] : B[0];
    /* strict: walls that only touch at a shared corner column do not
     * constrain each other (each lies beyond the other's line there,
     * which would read as a cycle) */
    return al < br && bl < ar;
}
static void sdf_order_exact(void) {
    u8 left[SDF_NSTMAX], out[SDF_NSTMAX];
    int n = sdf_nst, o = 0, i, j;
    memcpy(left, sdf_ord, n);
    while (n > 0) {
        int pick = -1, best = 0x7FFFFFFF;
        for (i = 0; i < n; i++) {
            int blocked = 0;
            for (j = 0; j < n && !blocked; j++)
                if (j != i && sdf_xover(left[j], left[i]) &&
                    sdf_occludes(left[j], left[i]))
                    blocked = 1;
            if (!blocked) {
                i32 mz = sdf_mz(left[i]);
                if (mz < best) { best = mz; pick = i; }
            }
        }
        if (pick < 0) {                    /* cycle: fall back to nearest */
            for (i = 0; i < n; i++)
                if (sdf_mz(left[i]) < best) { best = sdf_mz(left[i]); pick = i; }
            if (sdf_dbg) {
                fprintf(stderr, "order: cycle among %d, picked slot %d;",
                        n, left[pick]);
                for (i = 0; i < n; i++)
                    for (j = 0; j < n; j++)
                        if (j != i && sdf_xover(left[j], left[i]) &&
                            sdf_occludes(left[j], left[i]))
                            fprintf(stderr, " %d>%d", left[j], left[i]);
                fprintf(stderr, "\n");
            }
        }
        out[o++] = left[pick];
        left[pick] = left[--n];
    }
    memcpy(sdf_ord, out, sdf_nst);
}

static void sdf_order(void) {
    int i2, k, o;
    if (sdf_sort == 2) { sdf_order_exact(); return; }
    if (sdf_sort == 1) {
        for (i2 = 1; i2 < sdf_nst; i2++) {
            u8 t = sdf_ord[i2];
            k = i2 - 1;
            while (k >= 0 && sdf_mz(sdf_ord[k]) > sdf_mz(t)) {
                sdf_ord[k + 1] = sdf_ord[k]; k--;
            }
            sdf_ord[k + 1] = t;
        }
        return;
    }
    {
        u8 bh[48], bt[48], bn[SDF_NSTMAX], src[SDF_NSTMAX];
        memset(bh, 0xFF, sizeof bh);
        memcpy(src, sdf_ord, sizeof src);
        for (i2 = 0; i2 < sdf_nst; i2++) {
            i32 mz = sdf_mz(src[i2]);
            int bk = mz < 0 ? 0 : (mz >> 8) > 47 ? 47 : (int)(mz >> 8);
            if (bh[bk] == 0xFF) bh[bk] = (u8)i2;
            else bn[bt[bk]] = (u8)i2;
            bt[bk] = (u8)i2; bn[i2] = 0xFF;
        }
        o = 0;
        for (k = 0; k < 48; k++)
            for (i2 = bh[k]; i2 != 0xFF; i2 = bn[i2])
                sdf_ord[o++] = src[i2];
    }
}

/* cap bits 16-23 carry the camera's room id; the wall blob's trailer
 * {nroom, pvs[nroom], rbnd[nroom][4]} (sdx_prep.py) resolves it to a
 * mask gated by the view cone - a room whose bounds sit wholly behind
 * the near plane, or off one side of the |x|<=z view wedge, cannot be
 * seen through any doorway, so its walls never reach staging. The
 * all-corners-fail-the-same-plane form stays conservative: a big box
 * straddling the wedge keeps its room. 0 = no trailer, consider every
 * wall. Shared by sdf_render and the sdt_render call sites. */
static int sdf_roommask(const u8 *mem, u32 mem_sz, u32 w, i32 nw,
                        i32 cx, i32 cz, i32 gs, i32 gc, int room) {
    i64 tr;
    int nr, r2, j, rmask;
    if ((u64)w + 2 > mem_sz) return 0;
    {
        i32 nv = (i16)(mem[w] | (mem[w + 1] << 8));
        tr = (i64)w + 2 + (i64)nv * 4 + (i64)nw * 12;
    }
    if (tr + 2 > (i64)mem_sz) return 0;
    nr = (i16)(mem[tr] | (mem[tr + 1] << 8));
    if (nr < 1 || nr > 8 || room >= nr ||
        tr + 2 + nr * 10 > (i64)mem_sz)
        return 0;
    rmask = (i16)(mem[tr + 2 + room * 2] |
                  (mem[tr + 3 + room * 2] << 8)) | (1 << room);
    for (r2 = 0; r2 < nr; r2++) {
        if (rmask & (1 << r2)) {
            const u8 *b = mem + tr + 2 + nr * 2 + r2 * 8;
            int allb = 1, allr = 1, alll = 1;
            for (j = 0; j < 4; j++) {
                i32 wx = (i16)(b[(j >> 1) * 4] |
                               (b[(j >> 1) * 4 + 1] << 8));
                i32 wz = (i16)(b[2 + (j & 1) * 4] |
                               (b[3 + (j & 1) * 4] << 8));
                i32 dx = (wx << 8) - cx, dz = (wz << 8) - cz;
                i32 nx = (i32)(dx * (i64)gc - dz * (i64)gs);
                i32 nz = (i32)(dx * (i64)gs + dz * (i64)gc);
                /* nx/nz are 16.16; NEAR=64 in 8.8 is 16384 unscaled. */
                if (nz >= 16384) allb = 0;
                if (nx <= nz)    allr = 0;
                if (nx >= -nz)   alll = 0;
            }
            if (allb | allr | alll) rmask &= ~(1 << r2);
        }
    }
    return rmask;
}

/* Whole frame from @lines3d args. Returns the staged count, -1 on bad
 * args. mem/mem_sz: the VM image the blob pointer indexes. */
static i32 sdf_render(const u8 *mem, u32 mem_sz, u32 w, i32 nw,
                      i32 cx, i32 cz, i32 gs, i32 gc, i32 eyew,
                      i32 cap, i32 mode) {
    int rmask = sdf_roommask(mem, mem_sz, w, nw, cx, cz, gs, gc,
                             (cap >> 16) & 255),
        inset = (cap >> 24) & 1, nv, i;
    const i16 *vt, *wp;
    sdf_texstyle = (cap >> 27) & 7;      /* cap bits 27-29: texture style */
    if (sdf_texstyle > 2) sdf_texstyle = 0;   /* 3/4: sdtrue.h draws */
    sdf_texinit();
    sdf_texon = (cap >> 25) & 1;
    cap &= 0xFFFF;
    if (nw <= 0 || cap <= 0 || nw > SDF_MAXW || !(mode & 128) || !(mode & 32))
        return -1;
    if ((u64)w + 2 > mem_sz) return -1;
    nv = (i16)(mem[w] | (mem[w + 1] << 8));
    if (nv < 0 || nv > SDF_MAXV ||
        (u64)w + 2u + (u64)nv * 4u + (u64)nw * 12u > mem_sz) return -1;
    for (i = 0; i < 1 + nv * 2 + nw * 6; i++)
        sdf_blob[i] = (i16)(mem[w + i * 2] | (mem[w + i * 2 + 1] << 8));
    vt = sdf_blob + 1;
    wp = vt + nv * 2;
    sdf_vc0 = inset ? 2 : 0;
    sdf_vc1 = inset ? sdf_ncol - (48 / sdf_colw) : sdf_ncol - 1;
    if (sdf_vc1 < 0) sdf_vc1 = 0;
    sdf_viewh = (mode >> 28) ? ((mode >> 28) << 5) : SDF_SCRH;
    if (sdf_viewh > SDF_SCRH) sdf_viewh = SDF_SCRH;
    sdf_ccl = (mode >> 8) & 31;
    sdf_cfl = (mode >> 13) & 31;
    sdf_cvd = (mode >> 18) & 31;
    sdf_csk = (mode >> 23) & 31;
    sdf_band = (mode & 16) != 0;
    sdf_tables(gs, gc);
    memset(sdf_dbgc, 0, sizeof sdf_dbgc);
    sdf_stage(vt, nv, wp, nw, cx, cz, gs, gc, eyew, rmask);
    if (sdf_dbg)
        fprintf(stderr, "rejects: far %d near %d back %d offx %d behind %d"
                " offL %d offR %d room %d vidx %d vert %d staged %d\n",
                sdf_dbgc[0], sdf_dbgc[1], sdf_dbgc[2], sdf_dbgc[3],
                sdf_dbgc[4], sdf_dbgc[5], sdf_dbgc[6], sdf_dbgc[7],
                sdf_dbgc[8], sdf_dbgc[9], sdf_nst);
    sdf_order();
    /* F9 view setup */
    sdf_vcol[0] = sdf_pm(sdf_ccl); sdf_vcol[1] = sdf_pm(sdf_cfl);
    sdf_vcol[2] = sdf_pm(sdf_cvd); sdf_vcol[3] = sdf_pm(sdf_csk);
    sdf_vh = sdf_viewh > SDF_VROWS ? SDF_VROWS : sdf_viewh;
    sdf_vcl = sdf_vc0; sdf_vcr = sdf_vc1 + 1;
    for (i = 0; i < sdf_ncol; i++) { sdf_vwt[i] = 0; sdf_vwb[i] = (u8)(sdf_viewh - 1); }
    sdf_nwalls_drawn = 0;
    for (i = 0; i < sdf_nst; i++) sdf_wall(sdf_s[sdf_ord[i]]);
    sdf_void();
    sdf_nwalls_staged = sdf_nst;
    sdf_cmt++;
    return sdf_nst;
}

#endif /* RELIC_SDFILL_H */
