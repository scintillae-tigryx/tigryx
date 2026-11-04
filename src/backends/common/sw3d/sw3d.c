/*
 * sw3d.c
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
 * sw3d.c - shared software 3D rasteriser (see sw3d.h).
 *
 * The span rasteriser is a straight port of sw_draw_tri() from
 * src/backends/allegro-dos/backend_allegro_dos.c, minus the bits that
 * were DOS-specific (Allegro BITMAPs, planar staging, 8bpp palette LUTs)
 * and minus depth generality: output is always 16bpp RGB565. The
 * RELIC_FIXED_POINT_3D paths are kept verbatim - they are the whole
 * reason this is viable on FPU-less 68030/386SX hardware.
 */
#include "sw3d.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */
typedef struct {
    float sx, sy, invw, r, g, b, tu, tv;
} SwVert;

#define SW_SPAN 8          /* perspective-restart sub-span (power of 2) */

#define FP_SHIFT 16
#define FLOAT_TO_FP(x) ((int)((x) * (float)(1 << FP_SHIFT)))

#ifdef RELIC_FIXED_POINT_3D
/* Integer-only fixed-point helpers for FPU-less targets: the emulated
 * float ops in the span loop are the hottest cost. */
#define FIXMUL_I(x, y)  ((int)(((long long)(x) * (long long)(y)) >> 16))
#define FIXRCP_MAX      2147418112  /* 32767.0 in 16.16 */
static int fixrcp_i(int x) {
    if (x <= 0) return FIXRCP_MAX;
    {
        long long r = ((long long)1 << 32) / (long long)x;
        if (r > (long long)FIXRCP_MAX) return FIXRCP_MAX;
        return (int)r;
    }
}
#endif

#define SW_MAXCLIP 8
typedef struct {
    float x, y, z, w, r, g, b, tu, tv;
} SwClipVert;

/* -------------------------------------------------------------------------
 * Lifecycle / state
 * ---------------------------------------------------------------------- */
void sw3d_init(Sw3d *s) {
    memset(s, 0, sizeof(*s));
}

void sw3d_free(Sw3d *s) {
    int i;
    if (!s) return;
    for (i = 0; i < s->tex_cap; i++) {
        if (s->tex[i].px) free(s->tex[i].px);
        s->tex[i].px = NULL;
    }
    free(s->tex);
    s->tex = NULL;
    s->tex_cap = 0;
}

/* Grow the texture table to at least `want` slots (or fail: exceeded
 * SW3D_MAX_TEX / out of memory). New slots are zeroed so callers see
 * them as empty; lazily allocates on first use. */
static int sw3d_tex_reserve(Sw3d *s, int want) {
    if (!s->tex) {
        s->tex = (Sw3dTex *)calloc(SW3D_TEX_CAP_INIT, sizeof(Sw3dTex));
        if (!s->tex) return 0;
        s->tex_cap = SW3D_TEX_CAP_INIT;
    }
    if (want > s->tex_cap) {
        int nc = s->tex_cap;
        Sw3dTex *nt;
        while (nc < want) nc <<= 1;
        if (nc > SW3D_MAX_TEX) nc = SW3D_MAX_TEX;
        if (want > nc) return 0;
        nt = (Sw3dTex *)realloc(s->tex, (size_t)nc * sizeof(Sw3dTex));
        if (!nt) return 0;
        memset(nt + s->tex_cap, 0, (size_t)(nc - s->tex_cap) * sizeof(Sw3dTex));
        s->tex = nt;
        s->tex_cap = nc;
    }
    return 1;
}

static u16 *g_fb;       /* bound render target + dims */
static u16 *g_zb;
static int  g_w, g_h;

/* Paletted chunky target (sw3d_bind8): when set, pixels are mapped
 * RGB565 -> palette index instead of stored as u16. The map runs
 * through a 32K-entry RGB555 cache so the 256-entry nearest scan only
 * fires once per distinct shaded colour per palette. */
static u8  *g_fb8;
static u8   g_pal[768];
static u8   g_q15[32768];
static u8   g_q15v[4096];   /* validity bits for g_q15 (index 255 is real) */

void sw3d_bind(Sw3d *s, u16 *fb, u16 *zbuf, int w, int h) {
    (void)s;
    g_fb = fb; g_zb = zbuf; g_w = w; g_h = h;
    g_fb8 = NULL;
}

void sw3d_bind8(Sw3d *s, u8 *fb8, u16 *zbuf, int w, int h,
                const u8 *pal768) {
    (void)s;
    g_fb = NULL; g_zb = zbuf; g_w = w; g_h = h;
    g_fb8 = fb8;
    sw3d_pal8(s, pal768);
}

void sw3d_pal8(Sw3d *s, const u8 *pal768) {
    (void)s;
    if (pal768) memcpy(g_pal, pal768, sizeof g_pal);
    memset(g_q15v, 0, sizeof g_q15v);
}

/* RGB565 -> nearest palette index, memoised on RGB555. */
static int pix8(u16 c) {
    unsigned k = (unsigned)(c >> 1);
    if (!(g_q15v[k >> 3] & (1u << (k & 7)))) {
        int r = (c >> 8) & 0xF8, g = (c >> 3) & 0xFC, b = (c << 3) & 0xF8;
        int i, best = 0;
        long bd = 0x7FFFFFFFL;
        for (i = 0; i < 256; i++) {
            long dr = (long)g_pal[i * 3]     - r;
            long dg = (long)g_pal[i * 3 + 1] - g;
            long db = (long)g_pal[i * 3 + 2] - b;
            long d = dr * dr + dg * dg + db * db;
            if (d < bd) { bd = d; best = i; }
        }
        g_q15[k] = (u8)best;
        g_q15v[k >> 3] |= (u8)(1u << (k & 7));
    }
    return g_q15[k];
}

/* Store a pixel in whichever target is bound. */
#define SW_PUTC(row16, row8, x, c) do {                          \
    if (g_fb8) (row8)[(x)] = (u8)pix8((u16)(c));                 \
    else       (row16)[(x)] = (u16)(c); } while (0)

void sw3d_camera(Sw3d *s,
                 const float eye[3], const float tgt[3], const float up[3],
                 float fov_deg, float znear, float zfar) {
    float fx, fy, fz, fl, sx, sy, sz, sl, ux, uy, uz;
    float aspect;

    s->cam_mirror = 0;
    if (fov_deg < 0.0f) { s->cam_mirror = 1; fov_deg = -fov_deg; }
    if (fov_deg <= 0.0f) fov_deg = 60.0f;
    if (znear <= 0.0f) znear = 0.1f;
    if (zfar <= znear) zfar = znear + 1000.0f;
    s->znear = znear; s->zfar = zfar;

    /* Aspect is always the full render surface - a viewport is a scissor,
     * not a re-fit (same contract as the other software backends). */
    aspect = (g_h > 0) ? (float)g_w / (float)g_h : 1.0f;

    if (!s->proj_valid || aspect != s->proj_aspect ||
        fov_deg != s->proj_fov || znear != s->proj_znear ||
        zfar != s->proj_zfar || s->cam_mirror != s->proj_mirror) {
        float fov_rad = fov_deg * (3.14159265358979f / 180.0f);
        float f = 1.0f / tanf(fov_rad * 0.5f);
        memset(s->proj, 0, sizeof(s->proj));
        s->proj[0]  = s->cam_mirror ? -(f / aspect) : (f / aspect);
        s->proj[5]  = f;
        s->proj[10] = (zfar + znear) / (znear - zfar);
        s->proj[11] = -1.0f;
        s->proj[14] = (2.0f * zfar * znear) / (znear - zfar);
        s->proj_valid  = 1;
        s->proj_fov    = fov_deg;
        s->proj_aspect = aspect;
        s->proj_znear  = znear;
        s->proj_zfar   = zfar;
        s->proj_mirror = s->cam_mirror;
    }

    /* Right-handed look-at. The two sqrtf calls stay float even on
     * soft-float builds: the basis must stay orthonormal and a
     * fixed-point sqrt loses too much magnitude precision. Two calls
     * per frame is not the bottleneck. */
    fx = tgt[0] - eye[0]; fy = tgt[1] - eye[1]; fz = tgt[2] - eye[2];
    fl = sqrtf(fx * fx + fy * fy + fz * fz);
    if (fl < 1e-6f) fl = 1.0f;
    fx /= fl; fy /= fl; fz /= fl;
    sx = fy * up[2] - fz * up[1];
    sy = fz * up[0] - fx * up[2];
    sz = fx * up[1] - fy * up[0];
    sl = sqrtf(sx * sx + sy * sy + sz * sz);
    if (sl < 1e-6f) sl = 1.0f;
    sx /= sl; sy /= sl; sz /= sl;
    ux = sy * fz - sz * fy;
    uy = sz * fx - sx * fz;
    uz = sx * fy - sy * fx;
    s->view[0] = sx;  s->view[4] = sy;  s->view[8]  = sz;
    s->view[12] = -(sx * eye[0] + sy * eye[1] + sz * eye[2]);
    s->view[1] = ux;  s->view[5] = uy;  s->view[9]  = uz;
    s->view[13] = -(ux * eye[0] + uy * eye[1] + uz * eye[2]);
    s->view[2] = -fx; s->view[6] = -fy; s->view[10] = -fz;
    s->view[14] =  (fx * eye[0] + fy * eye[1] + fz * eye[2]);
    s->view[3] = 0;   s->view[7] = 0;   s->view[11] = 0;   s->view[15] = 1;

    s->have_camera = 1;

    /* Cache MVP = proj * view. */
    {
        float r[16];
        int c, row, k;
        for (c = 0; c < 4; c++)
            for (row = 0; row < 4; row++) {
                float v = 0;
                for (k = 0; k < 4; k++)
                    v += s->proj[k * 4 + row] * s->view[c * 4 + k];
                r[c * 4 + row] = v;
            }
        memcpy(s->mvp, r, sizeof(r));
    }

    /* Clear z-buffer for the new frame. */
    if (g_zb && g_w > 0 && g_h > 0)
        memset(g_zb, 0, (size_t)g_w * (size_t)g_h * sizeof(u16));
}

void sw3d_light(Sw3d *s, const float dir[3], const u8 *rgb, const u8 *amb) {
    float dl;
    if (!dir) { s->light_on = 0; return; }
    dl = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (dl < 1e-6f) dl = 1.0f;
    s->light_dir[0] = dir[0] / dl;
    s->light_dir[1] = dir[1] / dl;
    s->light_dir[2] = dir[2] / dl;
    s->light_rgb[0] = s->light_rgb[1] = s->light_rgb[2] = 1.0f;
    if (rgb) {
        s->light_rgb[0] = rgb[0] / 255.0f;
        s->light_rgb[1] = rgb[1] / 255.0f;
        s->light_rgb[2] = rgb[2] / 255.0f;
    }
    s->light_amb[0] = s->light_amb[1] = s->light_amb[2] = 0.1f;
    if (amb) {
        s->light_amb[0] = amb[0] / 255.0f;
        s->light_amb[1] = amb[1] / 255.0f;
        s->light_amb[2] = amb[2] / 255.0f;
    }
    s->light_on = 1;
}

int sw3d_tex_store(Sw3d *s, int slot, const u8 *src, int w, int h) {
    Sw3dTex *t;
    int pw = 1, ph = 1, x, y, has_alpha = 0;
    u8 *buf;

    if (!src || w <= 0 || h <= 0 || w > 2048 || h > 2048) return 0;
    if (slot <= 0 || !sw3d_tex_reserve(s, slot + 1)) return 0;
    /* reserve guarantees slot < tex_cap unless the ceiling was hit */
    if (slot >= s->tex_cap) return 0;

    while (pw < w) pw <<= 1;
    while (ph < h) ph <<= 1;
    buf = (u8 *)malloc((size_t)pw * (size_t)ph * 4u);
    if (!buf) return 0;

    for (y = 0; y < ph; y++) {
        int sy = y % h;
        for (x = 0; x < pw; x++) {
            int sx = x % w;
            int os = (sy * w + sx) * 4, od = (y * pw + x) * 4;
            buf[od]     = src[os];
            buf[od + 1] = src[os + 1];
            buf[od + 2] = src[os + 2];
            buf[od + 3] = src[os + 3];
            if (src[os + 3] < 255) has_alpha = 1;
        }
    }
    t = &s->tex[slot];
    if (t->px) free(t->px);
    t->px = buf;
    t->w = pw;       t->h = ph;
    t->src_w = w;    t->src_h = h;
    t->w_mask = pw - 1;
    t->h_mask = ph - 1;
    t->has_alpha = has_alpha;
    return slot;
}

/* -------------------------------------------------------------------------
 * sw_draw_tri  --  scanline rasteriser, 16bpp RGB565 + u16 z-buffer.
 * Straight port of the allegro-dos version; see it for the commentary
 * on the fixed-point path and sub-span UV approximation.
 * ---------------------------------------------------------------------- */
static void sw_draw_tri(const Sw3d *s, SwVert v0, SwVert v1, SwVert v2,
                        const Sw3dTex *tex) {
    SwVert *a = &v0, *b = &v1, *c = &v2, *tmp;
    int W = g_w, H = g_h;
    float z_scale = 65535.0f * (s->znear > 0.0f ? s->znear : 1.0f);
    int half;

    if ((!g_fb && !g_fb8) || !g_zb) return;

    /* Sort top-to-bottom. */
    if (a->sy > b->sy) { tmp = a; a = b; b = tmp; }
    if (b->sy > c->sy) { tmp = b; b = c; c = tmp; }
    if (a->sy > b->sy) { tmp = a; a = b; b = tmp; }

    {
        int y_top = (int)(a->sy + 0.5f);
        int y_mid = (int)(b->sy + 0.5f);
        int y_bot = (int)(c->sy + 0.5f);
        if (y_top == y_bot || y_bot <= 0 || y_top >= H) return;

#ifdef RELIC_FIXED_POINT_3D
#define DLTA(F, P, Q, D) (((Q)->F - (P)->F) * (D))
#else
#define DLTA(F, P, Q, D) (((Q)->F - (P)->F) / (D))
#endif
        {
            float dy_ac = (float)(y_bot - y_top);
            if (dy_ac < 1e-4f) return;
#ifdef RELIC_FIXED_POINT_3D
            {
                float dy_ac_d = 1.0f / dy_ac;
#else
            {
                float dy_ac_d = dy_ac;
#endif
                /* Long-edge (a->c) deltas per scanline. */
                float ac_dx = DLTA(sx, a, c, dy_ac_d), ac_dw = DLTA(invw, a, c, dy_ac_d);
                float ac_dr = DLTA(r, a, c, dy_ac_d), ac_dg = DLTA(g, a, c, dy_ac_d), ac_db = DLTA(b, a, c, dy_ac_d);
                float ac_du = DLTA(tu, a, c, dy_ac_d), ac_dv = DLTA(tv, a, c, dy_ac_d);
#ifdef RELIC_FIXED_POINT_3D
                int ac_dx_fp = FLOAT_TO_FP(ac_dx), ac_dw_fp = FLOAT_TO_FP(ac_dw);
                int ac_dr_fp = FLOAT_TO_FP(ac_dr), ac_dg_fp = FLOAT_TO_FP(ac_dg), ac_db_fp = FLOAT_TO_FP(ac_db);
                int ac_du_fp = FLOAT_TO_FP(ac_du), ac_dv_fp = FLOAT_TO_FP(ac_dv);
                int a_sx_fp = FLOAT_TO_FP(a->sx), a_sy_fp = FLOAT_TO_FP(a->sy);
                int a_invw_fp = FLOAT_TO_FP(a->invw), a_r_fp = FLOAT_TO_FP(a->r);
                int a_g_fp = FLOAT_TO_FP(a->g), a_b_fp = FLOAT_TO_FP(a->b);
                int a_tu_fp = FLOAT_TO_FP(a->tu), a_tv_fp = FLOAT_TO_FP(a->tv);
                int z_scale_int = (int)z_scale;
#endif
                /* Texture fields hoisted out of the inner loop. */
                int wm = 0, hm = 0, tw_shift = 0, akey = 0;
                const u8 *tpx = NULL;
                if (tex) {
                    wm = tex->w_mask; hm = tex->h_mask;
                    tpx = tex->px;  akey = tex->has_alpha;
                    {
                        int tmp_w = tex->w;
                        while (tmp_w > 1) { tw_shift++; tmp_w >>= 1; }
                    }
                }

                for (half = 0; half < 2; half++) {
                    SwVert *sv = (half == 0) ? a : b;
                    SwVert *bv = (half == 0) ? b : c;
                    int y0 = (half == 0) ? y_top : y_mid;
                    int y1 = (half == 0) ? y_mid : y_bot;
                    int ys, ye, y;
                    if (y0 == y1) continue;

                    {
                        float dys = (float)(y1 - y0);
#ifdef RELIC_FIXED_POINT_3D
                        float dys_d = 1.0f / dys;
#else
                        float dys_d = dys;
#endif
                        float sh_dx = DLTA(sx, sv, bv, dys_d), sh_dw = DLTA(invw, sv, bv, dys_d);
                        float sh_dr = DLTA(r, sv, bv, dys_d), sh_dg = DLTA(g, sv, bv, dys_d), sh_db = DLTA(b, sv, bv, dys_d);
                        float sh_du = DLTA(tu, sv, bv, dys_d), sh_dv = DLTA(tv, sv, bv, dys_d);
#ifdef RELIC_FIXED_POINT_3D
                        int sh_dx_fp = FLOAT_TO_FP(sh_dx), sh_dw_fp = FLOAT_TO_FP(sh_dw);
                        int sh_dr_fp = FLOAT_TO_FP(sh_dr), sh_dg_fp = FLOAT_TO_FP(sh_dg), sh_db_fp = FLOAT_TO_FP(sh_db);
                        int sh_du_fp = FLOAT_TO_FP(sh_du), sh_dv_fp = FLOAT_TO_FP(sh_dv);
                        int sv_sx_fp = FLOAT_TO_FP(sv->sx), sv_sy_fp = FLOAT_TO_FP(sv->sy);
                        int sv_invw_fp = FLOAT_TO_FP(sv->invw), sv_r_fp = FLOAT_TO_FP(sv->r);
                        int sv_g_fp = FLOAT_TO_FP(sv->g), sv_b_fp = FLOAT_TO_FP(sv->b);
                        int sv_tu_fp = FLOAT_TO_FP(sv->tu), sv_tv_fp = FLOAT_TO_FP(sv->tv);
#endif

                        /* Clamp scanline range to screen before walking
                         * (an off-screen vertex would otherwise spin the
                         * edge walker for millions of lines). */
                        ys = y0 < 0 ? 0 : y0;
                        ye = y1 > H ? H : y1;
                        if (ys >= ye) continue;

#ifdef RELIC_FIXED_POINT_3D
                        {
                            int pl_fp = (ys << 16) - a_sy_fp + (1 << 15);
                            int ac_x = a_sx_fp + FIXMUL_I(ac_dx_fp, pl_fp);
                            int ac_w = a_invw_fp + FIXMUL_I(ac_dw_fp, pl_fp);
                            int ac_r = a_r_fp + FIXMUL_I(ac_dr_fp, pl_fp);
                            int ac_g = a_g_fp + FIXMUL_I(ac_dg_fp, pl_fp);
                            int ac_b = a_b_fp + FIXMUL_I(ac_db_fp, pl_fp);
                            int ac_u = a_tu_fp + FIXMUL_I(ac_du_fp, pl_fp);
                            int ac_v = a_tv_fp + FIXMUL_I(ac_dv_fp, pl_fp);

                            int ps_fp = (ys << 16) - sv_sy_fp + (1 << 15);
                            int sh_x = sv_sx_fp + FIXMUL_I(sh_dx_fp, ps_fp);
                            int sh_w = sv_invw_fp + FIXMUL_I(sh_dw_fp, ps_fp);
                            int sh_r = sv_r_fp + FIXMUL_I(sh_dr_fp, ps_fp);
                            int sh_g = sv_g_fp + FIXMUL_I(sh_dg_fp, ps_fp);
                            int sh_b = sv_b_fp + FIXMUL_I(sh_db_fp, ps_fp);
                            int sh_u = sv_tu_fp + FIXMUL_I(sh_du_fp, ps_fp);
                            int sh_v = sv_tv_fp + FIXMUL_I(sh_dv_fp, ps_fp);
#else
                        {
                            float pl = (float)ys - a->sy + 0.5f;
                            float ac_x = a->sx + ac_dx * pl, ac_w = a->invw + ac_dw * pl;
                            float ac_r = a->r + ac_dr * pl, ac_g = a->g + ac_dg * pl, ac_b = a->b + ac_db * pl;
                            float ac_u = a->tu + ac_du * pl, ac_v = a->tv + ac_dv * pl;

                            float ps = (float)ys - sv->sy + 0.5f;
                            float sh_x = sv->sx + sh_dx * ps, sh_w = sv->invw + sh_dw * ps;
                            float sh_r = sv->r + sh_dr * ps, sh_g = sv->g + sh_dg * ps, sh_b = sv->b + sh_db * ps;
                            float sh_u = sv->tu + sh_du * ps, sh_v = sv->tv + sh_dv * ps;
#endif

                            for (y = ys; y < ye; y++,
#ifdef RELIC_FIXED_POINT_3D
                                    ac_x += ac_dx_fp, ac_w += ac_dw_fp, ac_r += ac_dr_fp, ac_g += ac_dg_fp, ac_b += ac_db_fp, ac_u += ac_du_fp, ac_v += ac_dv_fp,
                                    sh_x += sh_dx_fp, sh_w += sh_dw_fp, sh_r += sh_dr_fp, sh_g += sh_dg_fp, sh_b += sh_db_fp, sh_u += sh_du_fp, sh_v += sh_dv_fp) {
#else
                                    ac_x += ac_dx, ac_w += ac_dw, ac_r += ac_dr, ac_g += ac_dg, ac_b += ac_db, ac_u += ac_du, ac_v += ac_dv,
                                    sh_x += sh_dx, sh_w += sh_dw, sh_r += sh_dr, sh_g += sh_dg, sh_b += sh_db, sh_u += sh_du, sh_v += sh_dv) {
#endif
                                u16 *row16 = g_fb  ? g_fb  + (size_t)y * (size_t)W : NULL;
                                u8  *row8  = g_fb8 ? g_fb8 + (size_t)y * (size_t)W : NULL;
#ifdef RELIC_FIXED_POINT_3D
                                int lx, rx, lw, rw, lr, rr, lg, rg, lb, rb, lu, ru, lv, rv;
#else
                                float lx, rx, lw, rw, lr, rr, lg, rg, lb, rb, lu, ru, lv, rv;
#endif
                                int x0s, x1s, span, x;
                                u16 *zrow;

                                if (ac_x <= sh_x) {
                                    lx = ac_x; rx = sh_x; lw = ac_w; rw = sh_w;
                                    lr = ac_r; rr = sh_r; lg = ac_g; rg = sh_g;
                                    lb = ac_b; rb = sh_b; lu = ac_u; ru = sh_u;
                                    lv = ac_v; rv = sh_v;
                                } else {
                                    lx = sh_x; rx = ac_x; lw = sh_w; rw = ac_w;
                                    lr = sh_r; rr = ac_r; lg = sh_g; rg = ac_g;
                                    lb = sh_b; rb = ac_b; lu = sh_u; ru = ac_u;
                                    lv = sh_v; rv = ac_v;
                                }
#ifdef RELIC_FIXED_POINT_3D
                                x0s = (lx + (1 << 15)) >> 16;
                                if (x0s < 0) x0s = 0;
                                x1s = (rx + (1 << 15)) >> 16;
#else
                                x0s = (int)(lx + 0.5f);
                                if (x0s < 0) x0s = 0;
                                x1s = (int)(rx + 0.5f);
#endif
                                if (x1s > W) x1s = W;
                                span = x1s - x0s;
                                if (span <= 0) continue;

                                /* Per-scanline deltas, normalised by the TRUE
                                 * edge width - not the clamped span - so
                                 * off-screen clipping doesn't over-
                                 * interpolate depth/UV (see allegro-dos). */
                                {
#ifdef RELIC_FIXED_POINT_3D
                                    int edge_span = rx - lx;
                                    int inv_sp = (edge_span > 0) ? fixrcp_i(edge_span)
                                            : ((1 << 16) / (span > 0 ? span : 1));
                                    int fxp = (x0s << 16) - lx + (1 << 15);
                                    int d_w = FIXMUL_I(rw - lw, inv_sp);
                                    int c_w = lw + FIXMUL_I(d_w, fxp);
                                    int idr = FIXMUL_I(rr - lr, inv_sp);
                                    int idg = FIXMUL_I(rg - lg, inv_sp);
                                    int idb = FIXMUL_I(rb - lb, inv_sp);
                                    int icr = lr + FIXMUL_I(idr, fxp);
                                    int icg = lg + FIXMUL_I(idg, fxp);
                                    int icb = lb + FIXMUL_I(idb, fxp);
                                    int d_u = FIXMUL_I(ru - lu, inv_sp), d_v = FIXMUL_I(rv - lv, inv_sp);
                                    int c_u = lu + FIXMUL_I(d_u, fxp), c_v = lv + FIXMUL_I(d_v, fxp);
                                    int c_w_fp = c_w, d_w_fp = d_w;
                                    int c_u_fp = c_u, d_u_fp = d_u;
                                    int c_v_fp = c_v, d_v_fp = d_v;
#else
                                    float edge_span = rx - lx;
                                    float inv_sp = (edge_span > 1e-6f) ? (1.0f / edge_span)
                                            : (1.0f / (float)span);
                                    float fxp = (float)x0s - lx + 0.5f;
                                    float d_w = (rw - lw) * inv_sp;
                                    float c_w = lw + d_w * fxp;
                                    int idr = FLOAT_TO_FP((rr - lr) * inv_sp);
                                    int idg = FLOAT_TO_FP((rg - lg) * inv_sp);
                                    int idb = FLOAT_TO_FP((rb - lb) * inv_sp);
                                    int icr = FLOAT_TO_FP(lr + (rr - lr) * inv_sp * fxp);
                                    int icg = FLOAT_TO_FP(lg + (rg - lg) * inv_sp * fxp);
                                    int icb = FLOAT_TO_FP(lb + (rb - lb) * inv_sp * fxp);
                                    float d_u = (ru - lu) * inv_sp, d_v = (rv - lv) * inv_sp;
                                    float c_u = lu + d_u * fxp, c_v = lv + d_v * fxp;
#endif
                                    zrow = g_zb + (size_t)y * (size_t)W;
                                    x = x0s;

                                    if (tpx) {
#ifdef RELIC_FIXED_POINT_3D
                                        int iz = (int)(((long long)c_w * z_scale_int) >> 16);
                                        int idz = (int)(((long long)d_w * z_scale_int) >> 16);
#else
                                        int iz = (int)(c_w * z_scale);
                                        int idz = (int)(d_w * z_scale);
#endif
                                        while (x < x1s) {
                                            int xe = x + SW_SPAN;
                                            if (xe > x1s) xe = x1s;
                                            {
                                                int sub = xe - x;
                                                int pr = icr >> FP_SHIFT;
                                                int pg = icg >> FP_SHIFT;
                                                int pb = icb >> FP_SHIFT;
                                                int xi;
                                                int iu0, iv0, iu1, iv1, idu, idv, iu, iv;
                                                if (pr < 0) pr = 0; if (pr > 255) pr = 255;
                                                if (pg < 0) pg = 0; if (pg > 255) pg = 255;
                                                if (pb < 0) pb = 0; if (pb > 255) pb = 255;
                                                icr += idr * sub;
                                                icg += idg * sub;
                                                icb += idb * sub;
#ifdef RELIC_FIXED_POINT_3D
                                                {
                                                    int w0 = fixrcp_i(c_w_fp);
                                                    int ew_fp = c_w_fp + (int)((long long)d_w_fp * sub);
                                                    int w1 = fixrcp_i(ew_fp);
                                                    iu0 = FIXMUL_I(c_u_fp, w0);
                                                    iv0 = FIXMUL_I(c_v_fp, w0);
                                                    {
                                                        int eu = c_u_fp + (int)((long long)d_u_fp * sub);
                                                        int ev = c_v_fp + (int)((long long)d_v_fp * sub);
                                                        iu1 = FIXMUL_I(eu, w1);
                                                        iv1 = FIXMUL_I(ev, w1);
                                                    }
                                                }
#else
                                                {
                                                    float w0 = (c_w > 1e-9f) ? 1.0f / c_w : 1e9f;
                                                    float ew = c_w + d_w * (float)sub;
                                                    float w1 = (ew > 1e-9f) ? 1.0f / ew : 1e9f;
                                                    if (w0 > 32767.0f) w0 = 32767.0f;
                                                    if (w1 > 32767.0f) w1 = 32767.0f;
                                                    iu0 = FLOAT_TO_FP(c_u * w0);
                                                    iv0 = FLOAT_TO_FP(c_v * w0);
                                                    iu1 = FLOAT_TO_FP((c_u + d_u * (float)sub) * w1);
                                                    iv1 = FLOAT_TO_FP((c_v + d_v * (float)sub) * w1);
                                                }
#endif
                                                idu = (sub > 1) ? (iu1 - iu0) / sub : 0;
                                                idv = (sub > 1) ? (iv1 - iv0) / sub : 0;
                                                iu = iu0; iv = iv0;
                                                if (pr >= 255 && pg >= 255 && pb >= 255) {
                                                    /* Full-bright: no modulate. */
                                                    for (xi = x; xi < xe; xi++, iu += idu, iv += idv, iz += idz) {
                                                        u16 nz = (iz >= 65535) ? 65535u : (iz <= 0 ? 0u : (u16)iz);
                                                        int tx, ty;
                                                        const u8 *px;
                                                        if (nz <= zrow[xi]) continue;
                                                        tx = (iu >> FP_SHIFT) & wm;
                                                        ty = (iv >> FP_SHIFT) & hm;
                                                        px = tpx + (((ty << tw_shift) + tx) << 2);
                                                        if (akey && px[3] == 0) continue;
                                                        zrow[xi] = nz;
                                                        SW_PUTC(row16, row8, xi,
                                                                (((px[0] >> 3) << 11) |
                                                                 ((px[1] >> 2) << 5) |
                                                                 ((px[2] >> 3))));
                                                    }
                                                } else {
                                                    for (xi = x; xi < xe; xi++, iu += idu, iv += idv, iz += idz) {
                                                        u16 nz = (iz >= 65535) ? 65535u : (iz <= 0 ? 0u : (u16)iz);
                                                        int tx, ty;
                                                        const u8 *px;
                                                        if (nz <= zrow[xi]) continue;
                                                        tx = (iu >> FP_SHIFT) & wm;
                                                        ty = (iv >> FP_SHIFT) & hm;
                                                        px = tpx + (((ty << tw_shift) + tx) << 2);
                                                        if (akey && px[3] == 0) continue;
                                                        zrow[xi] = nz;
                                                        SW_PUTC(row16, row8, xi,
                                                                (((((px[0] * pr) >> 8) >> 3) << 11) |
                                                                 ((((px[1] * pg) >> 8) >> 2) << 5) |
                                                                 (((px[2] * pb) >> 8) >> 3)));
                                                    }
                                                }
#ifdef RELIC_FIXED_POINT_3D
                                                c_w_fp += (int)((long long)d_w_fp * sub);
                                                c_u_fp += (int)((long long)d_u_fp * sub);
                                                c_v_fp += (int)((long long)d_v_fp * sub);
#else
                                                c_w += d_w * (float)sub;
                                                c_u += d_u * (float)sub;
                                                c_v += d_v * (float)sub;
#endif
                                            }
                                            x = xe;
                                        }
                                    } else {
                                        /* Untextured: pack the span colour once. */
#ifdef RELIC_FIXED_POINT_3D
                                        int iz = (int)(((long long)c_w * z_scale_int) >> 16);
                                        int idz = (int)(((long long)d_w * z_scale_int) >> 16);
#else
                                        int iz = (int)(c_w * z_scale);
                                        int idz = (int)(d_w * z_scale);
#endif
                                        x = x0s;
                                        while (x < x1s) {
                                            int xe = x + SW_SPAN;
                                            if (xe > x1s) xe = x1s;
                                            {
                                                int sub = xe - x;
                                                int pr = icr >> FP_SHIFT;
                                                int pg = icg >> FP_SHIFT;
                                                int pb = icb >> FP_SHIFT;
                                                int xi;
                                                u16 packed;
                                                if (pr < 0) pr = 0; if (pr > 255) pr = 255;
                                                if (pg < 0) pg = 0; if (pg > 255) pg = 255;
                                                if (pb < 0) pb = 0; if (pb > 255) pb = 255;
                                                icr += idr * sub;
                                                icg += idg * sub;
                                                icb += idb * sub;
                                                packed = (u16)(((pr >> 3) << 11) |
                                                       ((pg >> 2) << 5) |
                                                       ((pb >> 3)));
                                                for (xi = x; xi < xe; xi++, iz += idz) {
                                                    u16 nz = (iz >= 65535) ? 65535u : (iz <= 0 ? 0u : (u16)iz);
                                                    if (nz <= zrow[xi]) continue;
                                                    zrow[xi] = nz;
                                                    SW_PUTC(row16, row8, xi, packed);
                                                }
                                            }
                                            x = xe;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
#undef DLTA
}

/* -------------------------------------------------------------------------
 * Near-plane clip + mesh submission
 * ---------------------------------------------------------------------- */
static int sw_clip_near(const SwClipVert *in, int n,
                        SwClipVert *out, float znear) {
    int m = 0, i;
    for (i = 0; i < n; i++) {
        const SwClipVert *a = &in[i], *b = &in[(i + 1) % n];
        int ai = (a->w >= znear), bi = (b->w >= znear);
        if (ai && m < SW_MAXCLIP) out[m++] = *a;
        if (ai != bi && m < SW_MAXCLIP) {
            float da = a->w - znear, db = b->w - znear, denom = da - db;
            float t = (denom != 0.0f) ? da / denom : 0.0f;
            SwClipVert cv;
            if (t < 0) t = 0; else if (t > 1) t = 1;
            cv.x = a->x + (b->x - a->x) * t;
            cv.y = a->y + (b->y - a->y) * t;
            cv.z = a->z + (b->z - a->z) * t;
            cv.w = znear;
            cv.r = a->r + (b->r - a->r) * t;
            cv.g = a->g + (b->g - a->g) * t;
            cv.b = a->b + (b->b - a->b) * t;
            cv.tu = a->tu + (b->tu - a->tu) * t;
            cv.tv = a->tv + (b->tv - a->tv) * t;
            out[m++] = cv;
        }
    }
    return m;
}

static void sw3d_mat4_xform(const float m[16], const float p[3], float out[4]) {
    out[0] = m[0] * p[0] + m[4] * p[1] + m[8]  * p[2] + m[12];
    out[1] = m[1] * p[0] + m[5] * p[1] + m[9]  * p[2] + m[13];
    out[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
    out[3] = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
}

static int g_dbg_tris = 0;   /* tris reaching the rasteriser (debug) */
int  sw3d_debug_tris(void)        { return g_dbg_tris; }
void sw3d_debug_tris_reset(void)  { g_dbg_tris = 0; }

void sw3d_mesh(Sw3d *s, const float *verts, int vcount,
               const void *idx, int icount, int tex_id, const u8 *rgba) {
    typedef struct { float cx, cy, cz, cw, r, g, b, tu, tv; } VXf;
#define VXF_STACK 256
    VXf vx_stack[VXF_STACK];
    VXf *vx;
    int idx_u32, max_tris, t, i;
    float cr = 1.0f, cg = 1.0f, cb = 1.0f;
    const Sw3dTex *tex = NULL;
    int tex_src_w = 1, tex_src_h = 1;
    float vp_cx, vp_cy, vp_hw, vp_hh, ZNEAR;
    const float *mvp;

    if (!s || (!g_fb && !g_fb8) || !g_zb || !s->have_camera) return;
    if (vcount <= 0) return;

    idx_u32 = (icount < 0);
    if (idx_u32) icount &= 0x7FFFFFFF;

    if (rgba) {
        cr = rgba[0] / 255.0f;
        cg = rgba[1] / 255.0f;
        cb = rgba[2] / 255.0f;
    }

    if (tex_id > 0 && tex_id < s->tex_cap && s->tex[tex_id].px) {
        tex = &s->tex[tex_id];
        tex_src_w = tex->src_w;
        tex_src_h = tex->src_h;
    }

    vx = (vcount <= VXF_STACK) ? vx_stack : (VXf *)malloc(sizeof(VXf) * (size_t)vcount);
    if (!vx) return;

    /* Projection centre/scale is always the full render surface. */
    vp_cx = vp_hw = g_w * 0.5f;
    vp_cy = vp_hh = g_h * 0.5f;

    mvp = s->mvp;

    /* Whole-mesh cull: skip if every vertex is behind the camera.
     * View row 2: vz = -f.(p-eye); <=0 means in front. */
    {
        const float *V = s->view;
        float rx = V[2], ry = V[6], rz = V[10], rd = V[14];
        int all_behind = 1, ci;
        for (ci = 0; ci < vcount && all_behind; ci++) {
            const float *v = verts + (size_t)ci * 8;
            float vz = rx * v[0] + ry * v[1] + rz * v[2] + rd;
            if (vz <= 0.0f) all_behind = 0;
        }
        if (all_behind) { if (vx != vx_stack) free(vx); return; }
    }

    for (i = 0; i < vcount; i++) {
        const float *v = verts + (size_t)i * 8;
        float pos[3] = { v[0], v[1], v[2] };
        float nrm[3] = { v[3], v[4], v[5] };
        float clip[4];
        float lr = cr, lg = cg, lb = cb;
        sw3d_mat4_xform(mvp, pos, clip);
        vx[i].cx = clip[0]; vx[i].cy = clip[1];
        vx[i].cz = clip[2]; vx[i].cw = clip[3];
        if (s->light_on) {
            float nl = nrm[0] * (-s->light_dir[0])
                     + nrm[1] * (-s->light_dir[1])
                     + nrm[2] * (-s->light_dir[2]);
            if (nl < 0) nl = 0;
            lr = cr * (s->light_amb[0] + s->light_rgb[0] * nl);
            lg = cg * (s->light_amb[1] + s->light_rgb[1] * nl);
            lb = cb * (s->light_amb[2] + s->light_rgb[2] * nl);
            if (lr > 1) lr = 1; if (lg > 1) lg = 1; if (lb > 1) lb = 1;
        }
        vx[i].r = lr * 255.0f;
        vx[i].g = lg * 255.0f;
        vx[i].b = lb * 255.0f;
        vx[i].tu = v[6] * (float)tex_src_w;
        vx[i].tv = v[7] * (float)tex_src_h;
    }

    ZNEAR = s->znear > 0.0f ? s->znear : 0.05f;
    max_tris = (idx && icount > 0) ? (icount / 3) : (vcount / 3);

    for (t = 0; t < max_tris; t++) {
        int i0, i1, i2, nc, k;
        SwClipVert cin[3], cout[SW_MAXCLIP];
        float pr_sx[SW_MAXCLIP], pr_sy[SW_MAXCLIP], pr_iw[SW_MAXCLIP];
        int j;

        if (idx && icount > 0) {
            if (idx_u32) {
                const u32 *ix = (const u32 *)idx + (size_t)t * 3;
                i0 = (int)ix[0]; i1 = (int)ix[1]; i2 = (int)ix[2];
            } else {
                const u16 *ix = (const u16 *)idx + (size_t)t * 3;
                i0 = ix[0]; i1 = ix[1]; i2 = ix[2];
            }
        } else {
            i0 = t * 3; i1 = t * 3 + 1; i2 = t * 3 + 2;
        }
        if (i0 < 0 || i0 >= vcount || i1 < 0 || i1 >= vcount ||
            i2 < 0 || i2 >= vcount) continue;

        /* Clip-space trivial reject. */
        {
            const VXf *p0 = &vx[i0], *p1 = &vx[i1], *p2 = &vx[i2];
            if (p0->cw < ZNEAR && p1->cw < ZNEAR && p2->cw < ZNEAR) continue;
            if (p0->cw > 0.0f && p1->cw > 0.0f && p2->cw > 0.0f) {
                if (p0->cx < -p0->cw && p1->cx < -p1->cw && p2->cx < -p2->cw) continue;
                if (p0->cx >  p0->cw && p1->cx >  p1->cw && p2->cx >  p2->cw) continue;
                if (p0->cy >  p0->cw && p1->cy >  p1->cw && p2->cy >  p2->cw) continue;
                if (p0->cy < -p0->cw && p1->cy < -p1->cw && p2->cy < -p2->cw) continue;
            }
        }

        /* Near-plane clip, fan-triangulate. */
        for (j = 0; j < 3; j++) {
            int id = (j == 0) ? i0 : (j == 1) ? i1 : i2;
            cin[j].x = vx[id].cx; cin[j].y = vx[id].cy;
            cin[j].z = vx[id].cz; cin[j].w = vx[id].cw;
            cin[j].r = vx[id].r;  cin[j].g = vx[id].g; cin[j].b = vx[id].b;
            cin[j].tu = vx[id].tu; cin[j].tv = vx[id].tv;
        }
        nc = sw_clip_near(cin, 3, cout, ZNEAR);
        if (nc < 3) continue;

        /* Project every clipped vertex once (cull + rasterise share it). */
        {
            int pi;
            for (pi = 0; pi < nc; pi++) {
                float invw = 1.0f / cout[pi].w;
                float sx = vp_cx + cout[pi].x * invw * vp_hw;
                float sy = vp_cy - cout[pi].y * invw * vp_hh;
#ifdef RELIC_FIXED_POINT_3D
                /* Keep projected coords inside the 16.16 range the edge
                 * walker converts to; a near-plane-grazing vertex can
                 * project to tens of thousands of pixels. */
                if (sx > 30000.0f) sx = 30000.0f; else if (sx < -30000.0f) sx = -30000.0f;
                if (sy > 30000.0f) sy = 30000.0f; else if (sy < -30000.0f) sy = -30000.0f;
#endif
                pr_iw[pi] = invw;
                pr_sx[pi] = sx;
                pr_sy[pi] = sy;
            }
        }

        /* Back-face cull on the first triangle of the clipped polygon
         * (cam_mirror flips winding). */
        {
            float crs = (pr_sx[1] - pr_sx[0]) * (pr_sy[2] - pr_sy[0])
                      - (pr_sy[1] - pr_sy[0]) * (pr_sx[2] - pr_sx[0]);
            if (s->cam_mirror) {
                if (crs <= -0.5f) continue;
            } else {
                if (crs >= 0.5f) continue;
            }
        }

        for (k = 1; k + 1 < nc; k++) {
            SwVert sv[3];
            int fi;
            for (fi = 0; fi < 3; fi++) {
                int ci = (fi == 0) ? 0 : (fi == 1) ? k : k + 1;
                const SwClipVert *cv = &cout[ci];
                float invw = pr_iw[ci];
                sv[fi].sx = pr_sx[ci];
                sv[fi].sy = pr_sy[ci];
                sv[fi].invw = invw;
                sv[fi].r = cv->r;
                sv[fi].g = cv->g;
                sv[fi].b = cv->b;
                sv[fi].tu = cv->tu * invw;
                sv[fi].tv = cv->tv * invw;
            }
            sw_draw_tri(s, sv[0], sv[1], sv[2], tex);
            g_dbg_tris++;
        }
    }

    if (vx != vx_stack) free(vx);
#undef VXF_STACK
}
