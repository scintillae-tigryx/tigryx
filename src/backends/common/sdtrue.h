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
/* sdtrue.h - TROOMS desktop true-texture renderer (tiers above LOW).
 *
 * A float column ray-caster over the SDXTEX.bin data written by
 * tools/bbc/sdx_prep.py: exact WAD-unit geometry, full-resolution wall
 * textures with WAD offsets and pegging, textured floors and ceilings,
 * COLORMAP light diminishing (sector light, distance, the
 * axis-aligned wall contrast), then either an ordered dither onto the
 * 8 MODE 2 colours (style 3) or the game palette colour itself (style 4,
 * the reference). Same algorithm as tools/bbc/sdref.py: every wall a
 * column's ray crosses is sorted by distance and walked front to back
 * with a [top, bot) row window, so visibility is exact by construction.
 *
 * Camera and room culling come from the TROOMS @lines3d arguments (world
 * units, 8.8; the SDXDATA wall records' room masks), so the BBC-exact
 * sdfill.h path and this one show the same view of the same map.
 *
 * Output: sdt_px[SDT_H][SDT_W] RGBA (byte order R,G,B,A), the 320x256
 * design frame at 2x; rows past the view are left black for the HUD.
 */
#ifndef SDTRUE_H
#define SDTRUE_H

#include <math.h>
#include <string.h>

#define SDT_W    640
#define SDT_H    512
#define SDT_HZ   256.0f          /* horizon row (design CY = 128, x2) */
#define SDT_FX   320.0f          /* focal, px (design FOV 160, x2) */
#define SDT_FY   480.0f          /* x 3/2 vertical scale */
#define SDT_MAXW 512
#define SDT_MAXT 64

typedef struct {
    float ax, ay, bx, by, len;
    int kind, lflags, rooms, contrast;
    struct {
        int xoff, yoff, up, lo, mid, fflat, cflat;
        float floor, ceil;
        int light;
    } s[2];
} SdtWall;

static u32 sdt_px[SDT_H][SDT_W];
static SdtWall sdt_w[SDT_MAXW];
static int sdt_nw, sdt_ntex, sdt_nflat, sdt_sky, sdt_ok;
static const u8 *sdt_blob;
static u32 sdt_blen;
static int sdt_ox, sdt_oy, sdt_hoff, sdt_scale;
static const u8 *sdt_pal, *sdt_cmap;
static int sdt_tw[SDT_MAXT], sdt_th[SDT_MAXT];
static const u8 *sdt_tp[SDT_MAXT], *sdt_fp[SDT_MAXT];
static u32 sdt_rgb[256];          /* game palette as packed RGBA */
static u8 sdt_da[256], sdt_db[256], sdt_df[256];   /* dither pairs */
static int sdt_green;             /* landscape ground palette index */
static int sdt_rok;               /* style-5 reduced set built */

static int sdt_rd16(const u8 *p) { return (i16)(p[0] | (p[1] << 8)); }
static u32 sdt_rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u32 sdt_pack(int r, int g, int b) {
    return (u32)r | ((u32)g << 8) | ((u32)b << 16) | 0xFF000000u;
}

/* Per palette entry: the MODE 2 colour pair (a, b) whose mix line
 * passes closest to the colour, and the mix fraction toward b in 16ths
 * - an ordered dither then picks a or b per pixel. Greys land on
 * black-white, browns on black-red / black-yellow. Mixing is in sRGB
 * (perceptual) space: linear light is physically right but the dots are
 * big enough that the eye does not blend them, and it reads far too
 * dark. */
static float sdt_lin(float v) { return v / 255.0f; }
static void sdt_dither_init(void) {
    int i, a, b, c;
    for (i = 0; i < 256; i++) {
        float p[3], best = 1e9f, bf = 0;
        int ba = 0, bb = 0;
        for (c = 0; c < 3; c++) p[c] = sdt_lin(sdt_pal[i * 3 + c]);
        {   /* lift: gamma 0.8 and 1.4x saturation, so the palette's dim browns
             * keep their hue instead of dithering to sparse grey */
            float m = (p[0] + p[1] + p[2]) / 3.0f;
            for (c = 0; c < 3; c++) {
                float v = m + (p[c] - m) * 1.4f;
                v = v < 0 ? 0 : v > 1 ? 1 : v;
                p[c] = powf(v, 0.8f);
            }
        }
        for (a = 0; a < 8; a++)
            for (b = a; b < 8; b++) {
                float ab[3], den = 0, dot = 0, f, d = 0;
                /* nested pairs only (a's bits within b's): black-red,
                 * red-yellow, yellow-white... Complementary pairs such
                 * as red-cyan average to a brown but read as noise */
                if ((a & b) != a) continue;
                for (c = 0; c < 3; c++) {
                    ab[c] = (float)(((b >> c) & 1) - ((a >> c) & 1));
                    den += ab[c] * ab[c];
                    dot += (p[c] - ((a >> c) & 1)) * ab[c];
                }
                f = den > 0 ? dot / den : 0;
                if (f < 0) f = 0;
                if (f > 1) f = 1;
                for (c = 0; c < 3; c++) {
                    float q = ((a >> c) & 1) + f * ab[c] - p[c];
                    d += q * q;
                }
                if (d < best) { best = d; ba = a; bb = b; bf = f; }
            }
        sdt_da[i] = (u8)ba; sdt_db[i] = (u8)bb;
        sdt_df[i] = (u8)(bf * 16.0f + 0.5f);
        sdt_rgb[i] = sdt_pack(sdt_pal[i * 3], sdt_pal[i * 3 + 1], sdt_pal[i * 3 + 2]);
    }
}

/* Parse SDXTEX.bin (layout: tools/bbc/sdx_prep.py write_tex). */
static int sdt_load(const u8 *b, u32 n) {
    int i, ver;
    u32 o;
    if (b == sdt_blob && sdt_ok) return 1;
    sdt_ok = 0;
    if (n < 24 || memcmp(b, "TRXT", 4) != 0) return 0;
    ver = sdt_rd16(b + 4);
    if (ver != 1) return 0;
    sdt_nw = sdt_rd16(b + 6); sdt_ntex = sdt_rd16(b + 8);
    sdt_nflat = sdt_rd16(b + 10); sdt_sky = sdt_rd16(b + 12);
    sdt_ox = sdt_rd16(b + 14); sdt_oy = sdt_rd16(b + 16);
    sdt_hoff = sdt_rd16(b + 18); sdt_scale = sdt_rd16(b + 20);
    if (sdt_nw > SDT_MAXW || sdt_ntex > SDT_MAXT || sdt_nflat > SDT_MAXT)
        return 0;
    o = 24;
    sdt_pal = b + o;  o += 768;
    sdt_cmap = b + o; o += 32 * 256;
    if (o + (u32)sdt_nw * 44 > n) return 0;
    for (i = 0; i < sdt_nw; i++, o += 44) {
        const u8 *r = b + o;
        SdtWall *w = &sdt_w[i];
        int k;
        w->ax = (float)sdt_rd16(r);     w->ay = (float)sdt_rd16(r + 2);
        w->bx = (float)sdt_rd16(r + 4); w->by = (float)sdt_rd16(r + 6);
        w->kind = r[8];
        w->lflags = r[10] | (r[11] << 8);
        w->len = sqrtf((w->bx - w->ax) * (w->bx - w->ax) +
                       (w->by - w->ay) * (w->by - w->ay));
        /* The classic fake contrast: E-W walls one level darker, N-S lighter */
        w->contrast = w->ay == w->by ? -1 : w->ax == w->bx ? 1 : 0;
        for (k = 0; k < 2; k++) {
            const u8 *s = r + 12 + k * 16;
            w->s[k].xoff = sdt_rd16(s); w->s[k].yoff = sdt_rd16(s + 2);
            w->s[k].up = s[4]; w->s[k].lo = s[5]; w->s[k].mid = s[6];
            w->s[k].fflat = s[7]; w->s[k].cflat = s[8];
            w->s[k].floor = (float)sdt_rd16(s + 10);
            w->s[k].ceil = (float)sdt_rd16(s + 12);
            w->s[k].light = sdt_rd16(s + 14);
        }
    }
    if (o + (u32)sdt_ntex * 8 + (u32)sdt_nflat * 4 > n) return 0;
    for (i = 0; i < sdt_ntex; i++, o += 8) {
        u32 off = sdt_rd32(b + o + 4);
        sdt_tw[i] = sdt_rd16(b + o) & 0xFFFF;
        sdt_th[i] = sdt_rd16(b + o + 2) & 0xFFFF;
        if (off + (u32)sdt_tw[i] * (u32)sdt_th[i] > n) return 0;
        sdt_tp[i] = b + off;
    }
    for (i = 0; i < sdt_nflat; i++, o += 4) {
        u32 off = sdt_rd32(b + o);
        if (off + 4096 > n) return 0;
        sdt_fp[i] = b + off;
    }
    sdt_dither_init();
    {   /* the greenest mid-bright palette entry: landscape ground */
        int best = -1000;
        for (i = 0; i < 256; i++) {
            int r = sdt_pal[i * 3], g = sdt_pal[i * 3 + 1], bl = sdt_pal[i * 3 + 2];
            int sc = g - (r + bl) / 2 - (g > 200 ? 100 : 0);
            if (sc > best) { best = sc; sdt_green = i; }
        }
    }
    sdt_blob = b; sdt_blen = n; sdt_ok = 1; sdt_rok = 0;
    return 1;
}

/* Classic light diminishing: colormap row for a sector light at a
 * distance (WAD units), wall contrast folded into the light number. */
static int sdt_level(int light, int contrast, float dist) {
    int lightnum = (light >> 4) + contrast, lv;
    if (lightnum < 0) lightnum = 0;
    if (lightnum > 15) lightnum = 15;
    if (dist < 1.0f) dist = 1.0f;
    lv = (15 - lightnum) * 4 - (int)(1280.0f / dist);
    if (lv < 0) lv = 0;
    if (lv > 31) lv = 31;
    return lv;
}

static const u8 sdt_bayer[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
};
static const u32 sdt_m2[8] = {
    0xFF000000u, 0xFF0000FFu, 0xFF00FF00u, 0xFF00FFFFu,
    0xFFFF0000u, 0xFFFF00FFu, 0xFFFFFF00u, 0xFFFFFFFFu
};
static int sdt_style;

static void sdt_put(int x, int y, int idx) {
    if (sdt_style == 4) { sdt_px[y][x] = sdt_rgb[idx]; return; }
    sdt_px[y][x] = sdt_m2[sdt_df[idx] > sdt_bayer[y & 3][x & 3]
                          ? sdt_db[idx] : sdt_da[idx]];
}

/* ---- style 5: the BBC-sized variant ---------------------------------
 * What a BBC could hold and draw: every texture reduced to RW x RH
 * texels, ONE BYTE per texel = MODE 2 colour pair (a, b) + a 2-bit mix
 * level (bits 0-2 a, 3-5 b, 6-7 level 0..3). The pixel picks a or b by
 * a 2x2 screen-space pattern - on the 6502 a 512-byte lookup (texel
 * byte x row parity -> MODE 2 byte), the cost of today's texel masks -
 * so a mid-grey texel reads as fine grey, not a big black/white check.
 * No per-pixel COLORMAP: distance and sector light pick one of five
 * halftone masks (full, 3/4, 1/2, 1/4, black) in screen space, like the
 * BBC fill renderer's distance bands. Floors and ceilings are each
 * flat's average colour (the BBC has no perspective-textured flats).
 * The frame is resolved to MODE 2 pixels (each 4 x 2 of these 640x512
 * px) - exactly the BBC's 160 x 256. TGX_SDF_RTEX=WxH sets the texel
 * grid (default 32x32, 4..64); the set costs RW*RH bytes a texture. */
static u8 sdt_rt[SDT_MAXT][64 * 64];  /* reduced texture, [u*RH + v] */
static u8 sdt_fa[SDT_MAXT];           /* flat average -> game palette */
static int sdt_rw = 32, sdt_rh = 32, sdt_rbytes;

static int sdt_nearest(int r, int g, int b) {
    int i, best = 0, bd = 1 << 30;
    for (i = 0; i < 256; i++) {
        int dr = r - sdt_pal[i * 3], dg = g - sdt_pal[i * 3 + 1],
            db = b - sdt_pal[i * 3 + 2], d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static void sdt_reduce(void) {
    int t, u, v, x, y, f;
    if (sdt_rok) return;
    {
        const char *e = getenv("TGX_SDF_RTEX");
        int w = 0, h = 0;
        if (e && sscanf(e, "%dx%d", &w, &h) == 2) {
            if (w < 4) w = 4;
            if (w > 64) w = 64;
            if (h < 4) h = 4;
            if (h > 64) h = 64;
            sdt_rw = w; sdt_rh = h;
        }
    }
    sdt_rbytes = 512;                             /* the pattern LUT */
    for (t = 0; t < sdt_ntex; t++) {
        int tw = sdt_tw[t], th = sdt_th[t];
        if (t == sdt_sky) continue;
        sdt_rbytes += sdt_rw * sdt_rh;
        for (u = 0; u < sdt_rw; u++)
            for (v = 0; v < sdt_rh; v++) {
                int x0 = u * tw / sdt_rw, x1 = (u + 1) * tw / sdt_rw;
                int y0 = v * th / sdt_rh, y1 = (v + 1) * th / sdt_rh;
                int r = 0, g = 0, b = 0, n = 0, idx;
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                for (x = x0; x < x1; x++)
                    for (y = y0; y < y1; y++) {
                        int p = sdt_tp[t][x * th + y];
                        r += sdt_pal[p * 3]; g += sdt_pal[p * 3 + 1];
                        b += sdt_pal[p * 3 + 2]; n++;
                    }
                idx = sdt_nearest(r / n, g / n, b / n);
                {
                    int a = sdt_da[idx], bb = sdt_db[idx];
                    int lv = (sdt_df[idx] + 2) / 4;      /* 16ths -> 0..4 */
                    if (lv >= 4) { a = bb; lv = 0; }
                    sdt_rt[t][u * sdt_rh + v] = (u8)(a | (bb << 3) | (lv << 6));
                }
            }
    }
    for (f = 0; f < sdt_nflat; f++) {
        int r = 0, g = 0, b = 0, i;
        for (i = 0; i < 4096; i++) {
            int p = sdt_fp[f][i];
            r += sdt_pal[p * 3]; g += sdt_pal[p * 3 + 1]; b += sdt_pal[p * 3 + 2];
        }
        sdt_fa[f] = (u8)sdt_nearest(r / 4096, g / 4096, b / 4096);
    }
    sdt_rok = 1;
}

/* COLORMAP level (0 bright .. 31 black) -> mask coverage in 16ths */
static int sdt_cover(int lv) {
    return lv < 8 ? 16 : lv < 14 ? 12 : lv < 20 ? 8 : lv < 26 ? 4 : 0;
}

/* style 5 pixel: MODE 2 colour c through the light mask, on the MODE 2
 * pixel grid (mx = x/4, my = y/2) */
static void sdt_put5(int x, int y, int c, int lv) {
    int mx = x >> 2, my = y >> 1;
    sdt_px[y][x] = sdt_m2[sdt_bayer[my & 3][mx & 3] < sdt_cover(lv) ? c : 0];
}

/* style 5 texel byte -> pixel: the 2x2 pattern picks a or b */
static const u8 sdt_b2[2][2] = { { 0, 2 }, { 3, 1 } };
static void sdt_put5t(int x, int y, int tb, int lv) {
    int mx = x >> 2, my = y >> 1;
    int c = ((tb >> 6) & 3) > sdt_b2[my & 1][mx & 1] ? (tb >> 3) & 7 : tb & 7;
    sdt_put5(x, y, c, lv);
}

/* style 5 flat: average colour dithered in screen space, masked */
static void sdt_put5f(int x, int y, int idx, int lv) {
    int mx = x >> 2, my = y >> 1;
    int c = sdt_df[idx] > sdt_bayer[my & 3][mx & 3] ? sdt_db[idx] : sdt_da[idx];
    sdt_put5(x, y, c, lv);
}

typedef struct { float u, v; int w, right; } SdtHit;

/* Render a frame. cx, cz: camera world 8.8; gs, gc: yaw sin/cos 8.8;
 * eyew: eye height in world units; walls: SDXDATA record table (room
 * masks at +2 high byte); rmask: visible rooms; viewh: design view rows
 * (224 or 192); inset: letterboxed columns; style 3 dither, 4 palette. */
static int sdt_render(const i16 *walls, int nw, i32 cx, i32 cz, i32 gs,
                      i32 gc, i32 eyew, int rmask, int viewh, int inset,
                      int style) {
    static SdtHit hit[SDT_MAXW];
    static int vis[SDT_MAXW];
    float camx, camy, s, c, eye;
    int i, nv = 0, X, VH, x0 = 0, x1 = SDT_W;
    if (!sdt_ok) return -1;
    sdt_style = style;
    if (style == 5) sdt_reduce();
    if (nw > sdt_nw) nw = sdt_nw;
    for (i = 0; i < nw; i++)
        if (!rmask || (((int)walls[i * 6 + 2] >> 8) & rmask)) vis[nv++] = i;
    camx = cx / 256.0f * sdt_scale;
    camy = cz / 256.0f * sdt_scale;
    s = gs / 256.0f; c = gc / 256.0f;
    /* world eye = floor + 5 units: back to the classic 41 above the floor */
    eye = (float)((eyew - sdt_hoff - 5) * sdt_scale) + 41.0f;
    VH = viewh * 2;
    if (inset) { x0 = 64; x1 = SDT_W - 64; }
    memset(sdt_px, 0, sizeof sdt_px);
    for (X = x0; X < x1; X++) {
        float t = (X + 0.5f - SDT_W / 2) / (SDT_W / 2);
        float dx = s + c * t, dy = c - s * t;
        float top = 0, bot = (float)VH;
        int nh = 0, k;
        for (k = 0; k < nv; k++) {
            SdtWall *w = &sdt_w[vis[k]];
            float ex = w->bx - w->ax, ey = w->by - w->ay;
            float den = dx * ey - dy * ex, u, v, cr;
            if (fabsf(den) < 1e-9f) continue;
            u = ((w->ax - camx) * ey - (w->ay - camy) * ex) / den;
            v = ((w->ax - camx) * dy - (w->ay - camy) * dx) / den;
            if (u <= 0.5f || v < 0 || v > 1) continue;
            cr = ex * (camy - w->ay) - ey * (camx - w->ax);
            if (w->kind != 1 && cr >= 0) continue;   /* solid back face */
            hit[nh].u = u; hit[nh].v = v; hit[nh].w = vis[k];
            hit[nh].right = cr < 0;
            nh++;
        }
        for (k = 1; k < nh; k++) {               /* insertion sort by u */
            SdtHit h = hit[k];
            int j = k - 1;
            while (j >= 0 && hit[j].u > h.u) { hit[j + 1] = hit[j]; j--; }
            hit[j + 1] = h;
        }
        for (k = 0; k < nh && top < bot; k++) {
            SdtWall *w = &sdt_w[hit[k].w];
            int ns = hit[k].right ? 0 : 1;       /* near side */
            float depth = hit[k].u;
            float nf = w->s[ns].floor, nc = w->s[ns].ceil;
            float ct = SDT_HZ - (nc - eye) * SDT_FY / depth;
            float fb = SDT_HZ - (nf - eye) * SDT_FY / depth;
            float colu = hit[k].v * w->len + w->s[ns].xoff;
            int y, ya, yb;
            /* near sector's ceiling and floor up to this wall */
            ya = (int)ceilf(top - 0.5f); yb = (int)ceilf((ct < bot ? ct : bot) - 0.5f);
            for (y = ya < 0 ? 0 : ya; y < yb && y < VH; y++) {
                float d = (nc - eye) * SDT_FY / (SDT_HZ - (y + 0.5f));
                int cf = w->s[ns].cflat;
                if (d <= 0) continue;
                if (cf == 255 || cf >= sdt_nflat) {
                    sdt_put(X, y, sdt_green);
                    continue;
                }
                {
                    int fx = (int)floorf(camx + dx * d + sdt_ox) & 63;
                    int fy = (int)floorf(-(camy + dy * d + sdt_oy)) & 63;
                    int lv = sdt_level(w->s[ns].light, 0, d);
                    if (style == 5) sdt_put5f(X, y, sdt_fa[cf], lv);
                    else sdt_put(X, y, sdt_cmap[lv * 256 + sdt_fp[cf][fy * 64 + fx]]);
                }
            }
            if (ct > top) top = ct < bot ? ct : bot;
            ya = (int)ceilf((fb > top ? fb : top) - 0.5f); yb = (int)ceilf(bot - 0.5f);
            for (y = ya < 0 ? 0 : ya; y < yb && y < VH; y++) {
                float d = (eye - nf) * SDT_FY / ((y + 0.5f) - SDT_HZ);
                int ff = w->s[ns].fflat;
                if (d <= 0 || ff >= sdt_nflat) continue;
                {
                    int fx = (int)floorf(camx + dx * d + sdt_ox) & 63;
                    int fy = (int)floorf(-(camy + dy * d + sdt_oy)) & 63;
                    int lv = sdt_level(w->s[ns].light, 0, d);
                    if (style == 5) sdt_put5f(X, y, sdt_fa[ff], lv);
                    else sdt_put(X, y, sdt_cmap[lv * 256 + sdt_fp[ff][fy * 64 + fx]]);
                }
            }
            if (fb < bot) bot = fb > top ? fb : top;
            if (top >= bot) break;
            {
                int lvw = sdt_level(w->s[ns].light, w->contrast, depth);
                /* one textured band [r0, r1): texture tx anchored with its
                 * top row at height `anchor` (WAD pegging) */
#define SDT_BAND(R0, R1, TX, ANCHOR)                                        \
                do {                                                        \
                    int tx_ = (TX), y_, a_ = (int)ceilf((R0) - 0.5f),       \
                        b_ = (int)ceilf((R1) - 0.5f);                        \
                    if (tx_ < 0 || tx_ >= sdt_ntex) break;                  \
                    {                                                       \
                        int tw_ = sdt_tw[tx_], th_ = sdt_th[tx_];           \
                        int col_ = ((int)floorf(colu) % tw_ + tw_) % tw_;   \
                        const u8 *cp_ = sdt_tp[tx_] + col_ * th_;           \
                        for (y_ = a_ < 0 ? 0 : a_; y_ < b_ && y_ < VH; y_++) { \
                            float h_ = eye + (SDT_HZ - (y_ + 0.5f)) * depth / SDT_FY; \
                            int r_ = (int)floorf((ANCHOR) - h_ + w->s[ns].yoff); \
                            r_ = (r_ % th_ + th_) % th_;                    \
                            if (style == 5)                                 \
                                sdt_put5t(X, y_, sdt_rt[tx_][(col_ * sdt_rw / tw_) * sdt_rh \
                                          + r_ * sdt_rh / th_], lvw);       \
                            else                                            \
                                sdt_put(X, y_, sdt_cmap[lvw * 256 + cp_[r_]]); \
                        }                                                   \
                    }                                                       \
                } while (0)
                if (w->kind == 2) {                  /* landscape window */
                    float hz = SDT_HZ, of = w->s[1].floor;
                    int y2, a2 = (int)ceilf(top - 0.5f), b2 = (int)ceilf(bot - 0.5f);
                    for (y2 = a2 < 0 ? 0 : a2; y2 < b2 && y2 < VH; y2++) {
                        if (y2 + 0.5f < hz || eye <= of) {
                            /* Classic sky: 4 repeats per turn, row 100 at
                             * the horizon, design rows = texture rows x 1.5 */
                            float ang = atan2f(dx, dy);
                            int tw = sdt_tw[sdt_sky], th = sdt_th[sdt_sky];
                            int sc = ((int)(ang / 6.2831853f * 1024.0f) % tw + tw) % tw;
                            int sr = 100 + (int)(((y2 + 0.5f) / 2 - 128) / 1.5f);
                            if (sr < 0) sr = 0;
                            if (sr >= th) sr = th - 1;
                            if (style == 5) sdt_put5(X, y2, 6, 0);   /* cyan */
                            else sdt_put(X, y2, sdt_tp[sdt_sky][sc * th + sr]);
                        } else {
                            float d = (eye - of) * SDT_FY / ((y2 + 0.5f) - hz);
                            int lv = sdt_level(160, 0, d);
                            if (style == 5) sdt_put5(X, y2, 2, lv);  /* green */
                            else sdt_put(X, y2, sdt_cmap[lv * 256 + sdt_green]);
                        }
                    }
                    top = bot;
                    break;
                }
                if (w->kind == 0) {                  /* one-sided: middle */
                    float anchor = (w->lflags & 16)
                        ? nf + (w->s[ns].mid < sdt_ntex ? sdt_th[w->s[ns].mid] : 0)
                        : nc;
                    SDT_BAND(top, bot, w->s[ns].mid, anchor);
                    top = bot;
                    break;
                }
                {
                    int fs = 1 - ns;
                    float bfl = w->s[fs].floor, bcl = w->s[fs].ceil;
                    float up = SDT_HZ - (bcl - eye) * SDT_FY / depth;
                    float lo = SDT_HZ - (bfl - eye) * SDT_FY / depth;
                    if (bcl < nc && up > top) {      /* upper texture */
                        int ut = w->s[ns].up;
                        float anchor = (w->lflags & 8) ? nc
                            : bcl + (ut < sdt_ntex ? sdt_th[ut] : 0);
                        float r1 = up < bot ? up : bot;
                        SDT_BAND(top, r1, ut, anchor);
                        top = r1;
                    }
                    if (bfl > nf && lo < bot) {      /* lower texture */
                        float anchor = (w->lflags & 16) ? nc : bfl;
                        float r0 = lo > top ? lo : top;
                        SDT_BAND(r0, bot, w->s[ns].lo, anchor);
                        bot = r0;
                    }
                }
#undef SDT_BAND
            }
        }
    }
    if (style == 5) {           /* resolve to MODE 2 pixels: 4 x 2 blocks */
        int y;
        for (y = 0; y < VH; y++)
            for (X = 0; X < SDT_W; X++)
                sdt_px[y][X] = sdt_px[(y & ~1) + 1][(X & ~3) + 2];
    }
    return nv;
}

#endif /* SDTRUE_H */
