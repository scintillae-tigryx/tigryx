/*
 * drawlist.h
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

/* drawlist.h — shared retained draw-list format + tessellator.
 *
 * The app builds a byte buffer of records (below) and submits it with
 * one @draw_list op. This header tessellates the buffer into:
 *   - a triangle vertex list  (x, y + RGBA8)
 *   - an ordered op list      (triangle ranges, text calls, clip rects)
 * so geometric and text output keep the app's draw order.
 *
 * Coordinates in the buffer are i32 "world units" — the app picks the
 * scale (PCBZERO uses KiCad's 100 nm IU). The current transform maps
 * world units to logical pixels; curves are flattened and strokes are
 * generated in logical-pixel space so flatness and width are correct.
 *
 * Record format (all fields little-endian):
 *
 *   0x00 DL_END        (stop early; end of buffer is implied END)
 *   0x01 DL_COLOR      u8 r g b a           (current colour)
 *   0x02 DL_WIDTH      i32 w                (stroke width, world units;
 *                                          <=0 = 1 logical px hairline)
 *   0x03 DL_CAP        u8 cap               (0 butt, 1 round, 2 square)
 *   0x04 DL_JOIN       u8 join              (0 miter, 1 round, 2 bevel)
 *   0x05 DL_DASH       u8 n, n x i32        (on/off lengths, world units;
 *                                          n=0 clears; open paths only)
 *   0x10 DL_MOVE       i32 x y              (start new sub-path)
 *   0x11 DL_LINE       i32 x y              (implicit move if no path)
 *   0x12 DL_QUAD       i32 cx cy x y        (quadratic bezier)
 *   0x13 DL_CUBIC      i32 c1x c1y c2x c2y x y
 *   0x14 DL_ARC        i32 cx cy rx ry, f32 start_deg, sweep_deg
 *                     (appends to current sub-path; MOVE first if
 *                      it should be disconnected)
 *   0x15 DL_CLOSE                           (close current sub-path)
 *   0x20 DL_STROKE                          (stroke + clear path)
 *   0x21 DL_FILL                            (fill + clear path; each
 *                                            sub-path ear-clipped
 *                                            separately — no holes)
 *   0x30 DL_SETX       f32 m[6]             (x' = m0*x + m2*y + m4)
 *   0x31 DL_MULX       f32 m[6]             (y' = m1*x + m3*y + m5;
 *                                            CTM = CTM * m — m applies
 *                                            to the point first)
 *   0x32 DL_PUSHX
 *   0x33 DL_POPX
 *   0x40 DL_CLIP       i32 x y w h          (logical px; w<=0 or h<=0
 *                                            disables clipping)
 *   0x50 DL_TEXT       u32 txt_ptr, u32 font_ptr, i32 x, i32 y,
 *                      i32 size, i32 weight, i32 justify
 *                      (x,y in world units, transformed to logical px;
 *                       colour = current colour; size/weight/justify as
 *                       in @text)
 *   0x60 DL_IMAGE      i32 tex_slot, i32 x, i32 y, i32 w, i32 h
 *                      (dest rect in world units - the four corners go
 *                       through the CTM, so rotation/mirror work; the
 *                       whole texture is drawn, UV 0..1)
 *
 * Non-uniform transforms approximate stroke/dash scale by the average
 * column norm — exact under rotation+uniform scale, off by a few %
 * under the small shears used for italic stroke-font text.
 */

#ifndef RELIC_DRAWLIST_H
#define RELIC_DRAWLIST_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DL_END    0x00
#define DL_COLOR  0x01
#define DL_WIDTH  0x02
#define DL_CAP    0x03
#define DL_JOIN   0x04
#define DL_DASH   0x05
#define DL_MOVE   0x10
#define DL_LINE   0x11
#define DL_QUAD   0x12
#define DL_CUBIC  0x13
#define DL_ARC    0x14
#define DL_CLOSE  0x15
#define DL_STROKE 0x20
#define DL_FILL   0x21
#define DL_SETX   0x30
#define DL_MULX   0x31
#define DL_PUSHX  0x32
#define DL_POPX   0x33
#define DL_CLIP   0x40
#define DL_TEXT   0x50
#define DL_IMAGE  0x60

#define DL_CAP_BUTT   0
#define DL_CAP_ROUND  1
#define DL_CAP_SQUARE 2
#define DL_JOIN_MITER 0
#define DL_JOIN_ROUND 1
#define DL_JOIN_BEVEL 2

typedef struct { float x, y; u8 r, g, b, a; } DlVert;

enum { DLO_TRIS = 1, DLO_TEXT = 2, DLO_CLIP = 3, DLO_IMAGE = 4 };

typedef struct {
    int kind;
    int start, count;              /* DLO_TRIS vertex range */
    u32 text_ptr, font_ptr;        /* DLO_TEXT (VM pointers) */
    i32 tx, ty, tsize, tweight, tjust;
    float trot;                    /* DLO_TEXT: radians about anchor,
                                      visual CCW-positive            */
    float tsx;                     /* DLO_TEXT: x compression (1.0)  */
    u8 tr, tg, tb, ta;
    i32 cx, cy, cw, ch;            /* DLO_CLIP (logical px) */
    i32 tex;                       /* DLO_IMAGE texture slot */
    float px[8];                   /* DLO_IMAGE quad corners (logical px,
                                      TL TR BR BL in emit order) */
} DlOp;

typedef struct {
    DlVert *v;
    int nv, cv;
    DlOp *o;
    int no, co;
    int err;                       /* 1 alloc fail, 2 malformed buffer */
} DlBatch;

static void dl_batch_init(DlBatch *b) { memset(b, 0, sizeof(*b)); }

static void dl_batch_free(DlBatch *b) {
    free(b->v);
    free(b->o);
    memset(b, 0, sizeof(*b));
}

/* ---------- implementation ---------- */

typedef struct { float x, y; } DlPt;
typedef struct { float m[6]; } DlXf;

typedef struct {
    DlPt *pts;   int np, cp;
    int  *subs;  int ns, cs;       /* start index per sub-path */
    u8   *closed;                  /* parallel to subs[] */
    int   cur;                     /* current sub-path has a move */
} DlPath;

typedef struct {
    const u8 *buf; int len, pos;
    DlXf xf, stack[16]; int nstack;
    u8 cr, cg, cb, ca;
    i32 width;
    int cap, join;
    i32 dash[16]; int ndash;
    float scale;                   /* avg column norm of xf */
    float yflip;                   /* if >0, emit vertex y as yflip - y
                                      (top-left logical -> GL y-up) */
    int vmark;                     /* verts already covered by a TRIS op */
    DlPath path;
    DlBatch *out;
} DlCtx;

static int dl_vpush(DlBatch *b, float x, float y, u8 r, u8 g, u8 bl, u8 a) {
    DlVert *v;
    if (b->err) return 0;
    if (b->nv >= b->cv) {
        int nc = b->cv ? b->cv * 2 : 4096;
        v = (DlVert *)realloc(b->v, (size_t)nc * sizeof(DlVert));
        if (!v) { b->err = 1; return 0; }
        b->v = v; b->cv = nc;
    }
    v = &b->v[b->nv++];
    v->x = x; v->y = y; v->r = r; v->g = g; v->b = bl; v->a = a;
    return 1;
}

static void dl_tri(DlCtx *c, float ax, float ay, float bx, float by,
                   float cx, float cy) {
    dl_vpush(c->out, ax, ay, c->cr, c->cg, c->cb, c->ca);
    dl_vpush(c->out, bx, by, c->cr, c->cg, c->cb, c->ca);
    dl_vpush(c->out, cx, cy, c->cr, c->cg, c->cb, c->ca);
}

static DlOp *dl_op(DlCtx *c, int kind) {
    DlOp *o;
    if (c->out->err) return NULL;
    if (c->out->no >= c->out->co) {
        int nc = c->out->co ? c->out->co * 2 : 64;
        o = (DlOp *)realloc(c->out->o, (size_t)nc * sizeof(DlOp));
        if (!o) { c->out->err = 1; return NULL; }
        c->out->o = o; c->out->co = nc;
    }
    o = &c->out->o[c->out->no++];
    memset(o, 0, sizeof(*o));
    o->kind = kind;
    return o;
}

/* emit triangles buffered since the last flush as one TRIS op */
static void dl_flush_tris(DlCtx *c) {
    DlOp *o;
    if (c->out->nv <= c->vmark) return;
    o = dl_op(c, DLO_TRIS);
    if (o) { o->start = c->vmark; o->count = c->out->nv - c->vmark; }
    c->vmark = c->out->nv;
}

static void dl_xf_pt(const DlXf *t, float x, float y, float *ox,
                     float *oy) {
    *ox = t->m[0] * x + t->m[2] * y + t->m[4];
    *oy = t->m[1] * x + t->m[3] * y + t->m[5];
}

static float dl_xf_scale(const DlXf *t) {
    float sx = sqrtf(t->m[0] * t->m[0] + t->m[1] * t->m[1]);
    float sy = sqrtf(t->m[2] * t->m[2] + t->m[3] * t->m[3]);
    float s = (sx + sy) * 0.5f;
    return s > 1e-9f ? s : 1.0f;
}

/* world -> screen (transform then y-flip if set) */
static void dl_to_screen(DlCtx *c, float x, float y, float *ox,
                         float *oy) {
    dl_xf_pt(&c->xf, x, y, ox, oy);
    if (c->yflip > 0.0f) *oy = c->yflip - *oy;
}

static void dl_xf_mul(DlXf *c, const float m[6]) {
    float n[6];
    n[0] = c->m[0] * m[0] + c->m[2] * m[1];
    n[1] = c->m[1] * m[0] + c->m[3] * m[1];
    n[2] = c->m[0] * m[2] + c->m[2] * m[3];
    n[3] = c->m[1] * m[2] + c->m[3] * m[3];
    n[4] = c->m[0] * m[4] + c->m[2] * m[5] + c->m[4];
    n[5] = c->m[1] * m[4] + c->m[3] * m[5] + c->m[5];
    memcpy(c->m, n, sizeof(n));
}

static int dl_ppush(DlPath *p, float x, float y) {
    DlPt *q;
    if (p->np > 0 && fabsf(p->pts[p->np - 1].x - x) < 1e-5f &&
        fabsf(p->pts[p->np - 1].y - y) < 1e-5f)
        return 1;
    if (p->np >= p->cp) {
        int nc = p->cp ? p->cp * 2 : 256;
        q = (DlPt *)realloc(p->pts, (size_t)nc * sizeof(DlPt));
        if (!q) return 0;
        p->pts = q; p->cp = nc;
    }
    p->pts[p->np].x = x; p->pts[p->np].y = y;
    p->np++;
    return 1;
}

static int dl_spush(DlPath *p, int start) {
    if (p->ns >= p->cs) {
        int nc = p->cs ? p->cs * 2 : 32;
        int *s = (int *)realloc(p->subs, (size_t)nc * sizeof(int));
        u8 *cl;
        if (!s) return 0;
        p->subs = s; p->cs = nc;
        cl = (u8 *)realloc(p->closed, (size_t)nc);
        if (!cl) return 0;
        p->closed = cl;
    }
    p->subs[p->ns] = start;
    p->closed[p->ns] = 0;
    p->ns++;
    return 1;
}

/* transform a world point and append to the current sub-path */
static int dl_path_pt(DlCtx *c, float wx, float wy, int move) {
    float x, y;
    dl_to_screen(c, wx, wy, &x, &y);
    if (move || !c->path.cur) {
        if (!dl_spush(&c->path, c->path.np)) return 0;
        c->path.cur = 1;
    }
    return dl_ppush(&c->path, x, y);
}

static void dl_path_close(DlCtx *c) {
    if (!c->path.cur || c->path.ns == 0) return;
    c->path.closed[c->path.ns - 1] = 1;
    c->path.cur = 0;
}

static void dl_path_reset(DlCtx *c) {
    c->path.np = c->path.ns = c->path.cur = 0;
}

/* flatten transformed curves in screen space */
static void dl_flat_quad(DlCtx *c, DlPt p0, DlPt p1, DlPt p2, int depth) {
    float mx = (p0.x + p2.x) * 0.5f, my = (p0.y + p2.y) * 0.5f;
    if (fabsf(p1.x - mx) + fabsf(p1.y - my) < 0.6f || depth >= 14) {
        dl_ppush(&c->path, p2.x, p2.y);
        return;
    }
    {
        DlPt a = { (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f };
        DlPt b = { (p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f };
        DlPt m = { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
        dl_flat_quad(c, p0, a, m, depth + 1);
        dl_flat_quad(c, m, b, p2, depth + 1);
    }
}

static void dl_flat_cubic(DlCtx *c, DlPt p0, DlPt p1, DlPt p2, DlPt p3,
                          int depth) {
    float dx = p3.x - p0.x, dy = p3.y - p0.y;
    float len = sqrtf(dx * dx + dy * dy) + 1e-9f;
    float d1 = fabsf(dx * (p0.y - p1.y) - dy * (p0.x - p1.x)) / len;
    float d2 = fabsf(dx * (p0.y - p2.y) - dy * (p0.x - p2.x)) / len;
    if (d1 + d2 < 0.5f || depth >= 14) {
        dl_ppush(&c->path, p3.x, p3.y);
        return;
    }
    {
        DlPt a = { (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f };
        DlPt b = { (p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f };
        DlPt d = { (p2.x + p3.x) * 0.5f, (p2.y + p3.y) * 0.5f };
        DlPt e = { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
        DlPt f = { (b.x + d.x) * 0.5f, (b.y + d.y) * 0.5f };
        DlPt m = { (e.x + f.x) * 0.5f, (e.y + f.y) * 0.5f };
        dl_flat_cubic(c, p0, a, e, m, depth + 1);
        dl_flat_cubic(c, m, f, d, p3, depth + 1);
    }
}

/* ---- stroker (screen space) ---- */

/* fan from a0 through `sweep` radians around p at radius r */
static void dl_fan(DlCtx *c, DlPt p, float a0, float sweep, float r) {
    int steps = (int)(fabsf(sweep) / 0.26f) + 1;
    int i;
    float lx = p.x + cosf(a0) * r, ly = p.y + sinf(a0) * r;
    for (i = 1; i <= steps; i++) {
        float a = a0 + sweep * (float)i / (float)steps;
        float nx = p.x + cosf(a) * r, ny = p.y + sinf(a) * r;
        dl_tri(c, p.x, p.y, lx, ly, nx, ny);
        lx = nx; ly = ny;
    }
}

/* stroke a flattened open or closed path, screen space */
static void dl_stroke_path(DlCtx *c, const DlPt *p, int n, int closed,
                           float halfw) {
    int i, nseg;
    float *nx, *ny;
    if (n < 2 || halfw <= 0) return;
    nseg = closed ? n : n - 1;
    nx = (float *)malloc((size_t)nseg * sizeof(float) * 2);
    if (!nx) { c->out->err = 1; return; }
    ny = nx + nseg;
    for (i = 0; i < nseg; i++) {
        const DlPt *a = &p[i], *b = &p[(i + 1) % n];
        float dx = b->x - a->x, dy = b->y - a->y;
        float l = sqrtf(dx * dx + dy * dy);
        if (l < 1e-6f) { nx[i] = 0; ny[i] = 1; }
        else { nx[i] = -dy / l; ny[i] = dx / l; }
    }
    for (i = 0; i < nseg; i++) {
        const DlPt *a = &p[i], *b = &p[(i + 1) % n];
        float ax1 = a->x + nx[i] * halfw, ay1 = a->y + ny[i] * halfw;
        float ax2 = a->x - nx[i] * halfw, ay2 = a->y - ny[i] * halfw;
        float bx1 = b->x + nx[i] * halfw, by1 = b->y + ny[i] * halfw;
        float bx2 = b->x - nx[i] * halfw, by2 = b->y - ny[i] * halfw;
        dl_tri(c, ax1, ay1, ax2, ay2, bx1, by1);
        dl_tri(c, ax2, ay2, bx2, by2, bx1, by1);
    }
    {
        int j0 = closed ? 0 : 1, j1 = closed ? n : n - 1, k;
        for (k = j0; k < j1; k++) {
            int ip = (k - 1 + nseg) % nseg, in = k % nseg;
            const DlPt *v = &p[k];
            /* direction of a segment is (ny, -nx) */
            float cross = ny[ip] * -nx[in] - -nx[ip] * ny[in];
            float s, ax0, ay0, ax1, ay1;
            if (fabsf(cross) < 1e-4f) continue;
            s = cross > 0 ? -1.0f : 1.0f;   /* outer side */
            ax0 = v->x + s * halfw * nx[ip];
            ay0 = v->y + s * halfw * ny[ip];
            ax1 = v->x + s * halfw * nx[in];
            ay1 = v->y + s * halfw * ny[in];
            if (c->join == DL_JOIN_ROUND) {
                float a0 = atan2f(ay0 - v->y, ax0 - v->x);
                float a1 = atan2f(ay1 - v->y, ax1 - v->x);
                float sw = a1 - a0;
                while (sw > (float)M_PI) sw -= 2.0f * (float)M_PI;
                while (sw < -(float)M_PI) sw += 2.0f * (float)M_PI;
                dl_fan(c, *v, a0, sw, halfw);
            } else if (c->join == DL_JOIN_MITER) {
                float d = nx[ip] * nx[in] + ny[ip] * ny[in];
                if (1.0f + d > 0.125f) {  /* miter len < 4x halfw */
                    float f = halfw * s / (1.0f + d);
                    float mx = v->x + (nx[ip] + nx[in]) * f;
                    float my = v->y + (ny[ip] + ny[in]) * f;
                    dl_tri(c, v->x, v->y, ax0, ay0, mx, my);
                    dl_tri(c, v->x, v->y, mx, my, ax1, ay1);
                } else {
                    dl_tri(c, v->x, v->y, ax0, ay0, ax1, ay1);
                }
            } else { /* bevel */
                dl_tri(c, v->x, v->y, ax0, ay0, ax1, ay1);
            }
        }
    }
    if (!closed && c->cap != DL_CAP_BUTT) {
        int e;
        for (e = 0; e < 2; e++) {
            /* e=0 start (seg 0, bulge -dir); e=1 end (seg nseg-1,+dir) */
            int si = e == 0 ? 0 : nseg - 1;
            DlPt v = e == 0 ? p[0] : p[n - 1];
            float sgn = e == 0 ? -1.0f : 1.0f;
            /* segment direction = (ny, -nx); cap dir = sgn * dir */
            if (c->cap == DL_CAP_SQUARE) {
                float ex = v.x + sgn * ny[si] * halfw;
                float ey = v.y - sgn * nx[si] * halfw;
                float ax = v.x + nx[si] * halfw, ay = v.y + ny[si] * halfw;
                float bx = v.x - nx[si] * halfw, by = v.y - ny[si] * halfw;
                dl_tri(c, ax, ay, bx, by, ex + nx[si] * halfw,
                       ey + ny[si] * halfw);
                dl_tri(c, bx, by, ex - nx[si] * halfw,
                       ey - ny[si] * halfw, ex + nx[si] * halfw,
                       ey + ny[si] * halfw);
            } else { /* round */
                float a0 = atan2f(ny[si], nx[si]);          /* +n side */
                float a_cap = atan2f(-sgn * nx[si], sgn * ny[si]);
                float sw = a_cap - a0;
                while (sw > (float)M_PI) sw -= 2.0f * (float)M_PI;
                while (sw < -(float)M_PI) sw += 2.0f * (float)M_PI;
                dl_fan(c, v, a0, sw > 0 ? (float)M_PI : -(float)M_PI,
                       halfw);
            }
        }
    }
    free(nx);
}

/* dash-split an open flattened path into "on" spans, screen space.
 * Returns count of spans; out_pts/out_spans are malloc'd. */
static int dl_dash_path(DlCtx *c, const DlPt *p, int n, float sc,
                        DlPt **out_pts, int **out_spans) {
    int cap = n + 16, np = 0, nsp = 0, scap = 16;
    DlPt *pts = (DlPt *)malloc((size_t)cap * sizeof(DlPt));
    int *spans = (int *)malloc((size_t)scap * 2 * sizeof(int));
    int di = 0, on = 1, i, span_start = 0;
    float dleft;
    DlPt cur;
    if (!pts || !spans) { free(pts); free(spans); return 0; }
    dleft = (float)c->dash[0] * sc;
    if (dleft <= 0) dleft = 1e-4f;
    pts[np++] = cur = p[0];
    for (i = 1; i < n; i++) {
        float sx = p[i].x - cur.x, sy = p[i].y - cur.y;
        float sl = sqrtf(sx * sx + sy * sy), ux, uy;
        if (sl < 1e-9f) continue;
        ux = sx / sl; uy = sy / sl;
        while (sl > 1e-9f) {
            float step = sl < dleft ? sl : dleft;
            cur.x += ux * step; cur.y += uy * step;
            sl -= step; dleft -= step;
            if (dleft <= 1e-9f) {
                di = (di + 1) % c->ndash;
                on = (di % 2) == 0;
                dleft = (float)c->dash[di] * sc;
                if (dleft <= 0) dleft = 1e-4f;
                if (np >= cap) {
                    cap *= 2;
                    pts = (DlPt *)realloc(pts, (size_t)cap * sizeof(DlPt));
                }
                if (on) {
                    span_start = np;
                    pts[np++] = cur;
                } else {
                    pts[np++] = cur;
                    if (nsp >= scap) {
                        scap *= 2;
                        spans = (int *)realloc(spans,
                                               (size_t)scap * 2 * sizeof(int));
                    }
                    spans[nsp * 2] = span_start;
                    spans[nsp * 2 + 1] = np - span_start;
                    nsp++;
                }
            }
        }
        if (on) {
            if (np >= cap) {
                cap *= 2;
                pts = (DlPt *)realloc(pts, (size_t)cap * sizeof(DlPt));
            }
            pts[np++] = cur;
        }
    }
    if (on) { /* trailing open span */
        if (nsp >= scap) {
            scap *= 2;
            spans = (int *)realloc(spans, (size_t)scap * 2 * sizeof(int));
        }
        spans[nsp * 2] = span_start;
        spans[nsp * 2 + 1] = np - span_start;
        nsp++;
    }
    *out_pts = pts;
    *out_spans = spans;
    return nsp;
}

/* ---- fill: ear-clipper per sub-path ---- */

static float dl_area2(const DlPt *p, const int *idx, int n) {
    float a = 0;
    int i;
    for (i = 0; i < n; i++) {
        const DlPt *u = &p[idx[i]], *v = &p[idx[(i + 1) % n]];
        a += u->x * v->y - v->x * u->y;
    }
    return a;
}

static int dl_pt_in_tri(const DlPt *q, const DlPt *a, const DlPt *b,
                        const DlPt *c) {
    float d1 = (q->x - a->x) * (b->y - a->y) - (q->y - a->y) * (b->x - a->x);
    float d2 = (q->x - b->x) * (c->y - b->y) - (q->y - b->y) * (c->x - b->x);
    float d3 = (q->x - c->x) * (a->y - c->y) - (q->y - c->y) * (a->x - c->x);
    int neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    int pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

static void dl_fill_path(DlCtx *c, const DlPt *p, int n) {
    int *idx, i, cnt, guard = 0;
    if (n < 3) return;
    idx = (int *)malloc((size_t)n * sizeof(int));
    if (!idx) { c->out->err = 1; return; }
    /* dedupe: consecutive duplicates and a trailing point equal to
       the start count as "inside" every ear and wedge the clipper */
    cnt = 0;
    for (i = 0; i < n; i++) {
        if (cnt > 0 && p[idx[cnt - 1]].x == p[i].x &&
            p[idx[cnt - 1]].y == p[i].y) continue;
        idx[cnt++] = i;
    }
    if (cnt > 1 && p[idx[0]].x == p[idx[cnt - 1]].x &&
        p[idx[0]].y == p[idx[cnt - 1]].y) cnt--;
    if (cnt < 3) { free(idx); return; }
    if (dl_area2(p, idx, cnt) < 0)
        for (i = 0; i < cnt / 2; i++) {
            int t = idx[i]; idx[i] = idx[cnt - 1 - i];
            idx[cnt - 1 - i] = t;
        }
    while (cnt > 3 && guard++ < n * n) {
        int clipped = 0;
        for (i = 0; i < cnt; i++) {
            int i0 = idx[(i + cnt - 1) % cnt], i1 = idx[i],
                i2 = idx[(i + 1) % cnt];
            const DlPt *a = &p[i0], *b = &p[i1], *d = &p[i2];
            float cr = (b->x - a->x) * (d->y - a->y) -
                       (b->y - a->y) * (d->x - a->x);
            int k;
            if (cr <= 1e-7f) continue;
            for (k = 0; k < cnt; k++) {
                int ik = idx[k];
                if (ik == i0 || ik == i1 || ik == i2) continue;
                if (dl_pt_in_tri(&p[ik], a, b, d)) break;
            }
            if (k < cnt) continue;
            dl_tri(c, a->x, a->y, b->x, b->y, d->x, d->y);
            memmove(&idx[i], &idx[i + 1],
                    (size_t)(cnt - i - 1) * sizeof(int));
            cnt--;
            clipped = 1;
            break;
        }
        if (!clipped) break;
    }
    if (cnt == 3)
        dl_tri(c, p[idx[0]].x, p[idx[0]].y, p[idx[1]].x, p[idx[1]].y,
               p[idx[2]].x, p[idx[2]].y);
    free(idx);
}

/* ---- record interpreter ---- */

static int dl_take(DlCtx *c, int n) {
    if (c->pos + n > c->len) { c->out->err = 2; return 0; }
    return 1;
}

static void dl_stroke_all(DlCtx *c) {
    DlPath *p = &c->path;
    float halfw = c->width <= 0 ? 0.5f : (float)c->width * c->scale * 0.5f;
    int s;
    for (s = 0; s < p->ns; s++) {
        int st = p->subs[s];
        int en = (s + 1 < p->ns) ? p->subs[s + 1] : p->np;
        int n = en - st;
        if (n < 2) continue;
        if (c->ndash > 0 && !p->closed[s]) {
            DlPt *dp = NULL; int *spans = NULL; int nsp, k;
            nsp = dl_dash_path(c, &p->pts[st], n, c->scale, &dp, &spans);
            for (k = 0; k < nsp; k++) {
                int ss = spans[k * 2], nn = spans[k * 2 + 1];
                if (nn >= 2)
                    dl_stroke_path(c, &dp[ss], nn, 0, halfw);
            }
            free(dp); free(spans);
        } else {
            dl_stroke_path(c, &p->pts[st], n, p->closed[s], halfw);
        }
    }
    dl_path_reset(c);
}

static void dl_fill_all(DlCtx *c) {
    DlPath *p = &c->path;
    int s;
    for (s = 0; s < p->ns; s++) {
        int st = p->subs[s];
        int en = (s + 1 < p->ns) ? p->subs[s + 1] : p->np;
        dl_fill_path(c, &p->pts[st], en - st);
    }
    dl_path_reset(c);
}

int dl_tessellate(const u8 *buf, int len, float yflip, DlBatch *out) {
    DlCtx c;
    memset(&c, 0, sizeof(c));
    c.buf = buf; c.len = len;
    c.out = out;
    c.cr = c.cg = c.cb = 0; c.ca = 255;
    c.xf.m[0] = c.xf.m[3] = 1.0f;
    c.scale = 1.0f;
    c.yflip = yflip;
    c.cap = DL_CAP_BUTT; c.join = DL_JOIN_MITER;
    while (c.pos < c.len && !out->err) {
        int op = buf[c.pos++];
        switch (op) {
        case DL_END:
            goto done;
        case DL_COLOR:
            if (!dl_take(&c, 4)) goto done;
            c.cr = buf[c.pos]; c.cg = buf[c.pos + 1];
            c.cb = buf[c.pos + 2]; c.ca = buf[c.pos + 3];
            c.pos += 4;
            break;
        case DL_WIDTH:
            if (!dl_take(&c, 4)) goto done;
            c.width = (i32)relic_rd32le(buf + c.pos); c.pos += 4;
            break;
        case DL_CAP:
            if (!dl_take(&c, 1)) goto done;
            c.cap = buf[c.pos++] <= 2 ? buf[c.pos - 1] : 0;
            break;
        case DL_JOIN:
            if (!dl_take(&c, 1)) goto done;
            c.join = buf[c.pos++] <= 2 ? buf[c.pos - 1] : 0;
            break;
        case DL_DASH: {
            int n, i;
            if (!dl_take(&c, 1)) goto done;
            n = buf[c.pos++];
            if (!dl_take(&c, n * 4) || n > 16) goto done;
            c.ndash = n;
            for (i = 0; i < n; i++) {
                c.dash[i] = (i32)relic_rd32le(buf + c.pos);
                c.pos += 4;
            }
            break;
        }
        case DL_MOVE:
        case DL_LINE: {
            i32 x, y;
            if (!dl_take(&c, 8)) goto done;
            x = (i32)relic_rd32le(buf + c.pos);
            y = (i32)relic_rd32le(buf + c.pos + 4);
            c.pos += 8;
            dl_path_pt(&c, (float)x, (float)y, op == DL_MOVE);
            break;
        }
        case DL_QUAD: {
            DlPt p0, p1, p2;
            float x1, y1, x2, y2;
            if (!dl_take(&c, 16)) goto done;
            x1 = (float)(i32)relic_rd32le(buf + c.pos);
            y1 = (float)(i32)relic_rd32le(buf + c.pos + 4);
            x2 = (float)(i32)relic_rd32le(buf + c.pos + 8);
            y2 = (float)(i32)relic_rd32le(buf + c.pos + 12);
            c.pos += 16;
            if (!c.path.cur || c.path.np == 0) break;
            p0 = c.path.pts[c.path.np - 1];
            dl_to_screen(&c, x1, y1, &p1.x, &p1.y);
            dl_to_screen(&c, x2, y2, &p2.x, &p2.y);
            dl_flat_quad(&c, p0, p1, p2, 0);
            break;
        }
        case DL_CUBIC: {
            DlPt p0, p1, p2, p3;
            float x1, y1, x2, y2, x3, y3;
            if (!dl_take(&c, 24)) goto done;
            x1 = (float)(i32)relic_rd32le(buf + c.pos);
            y1 = (float)(i32)relic_rd32le(buf + c.pos + 4);
            x2 = (float)(i32)relic_rd32le(buf + c.pos + 8);
            y2 = (float)(i32)relic_rd32le(buf + c.pos + 12);
            x3 = (float)(i32)relic_rd32le(buf + c.pos + 16);
            y3 = (float)(i32)relic_rd32le(buf + c.pos + 20);
            c.pos += 24;
            if (!c.path.cur || c.path.np == 0) break;
            p0 = c.path.pts[c.path.np - 1];
            dl_to_screen(&c, x1, y1, &p1.x, &p1.y);
            dl_to_screen(&c, x2, y2, &p2.x, &p2.y);
            dl_to_screen(&c, x3, y3, &p3.x, &p3.y);
            dl_flat_cubic(&c, p0, p1, p2, p3, 0);
            break;
        }
        case DL_ARC: {
            float cx, cy, rx, ry, a0, sweep, rsc;
            int i, n;
            if (!dl_take(&c, 24)) goto done;
            cx = (float)(i32)relic_rd32le(buf + c.pos);
            cy = (float)(i32)relic_rd32le(buf + c.pos + 4);
            rx = (float)(i32)relic_rd32le(buf + c.pos + 8);
            ry = (float)(i32)relic_rd32le(buf + c.pos + 12);
            memcpy(&a0, buf + c.pos + 16, 4);
            memcpy(&sweep, buf + c.pos + 20, 4);
            c.pos += 24;
            /* chord-error sized: a full circle of screen radius r
             * needs ~6.3*sqrt(r) segments for <0.5px deviation, so a
             * 3px junction dot gets ~11, not 46+ */
            rsc = (rx > ry ? rx : ry) * c.scale;
            if (rsc < 1.0f) rsc = 1.0f;
            n = (int)(6.3f * sqrtf(rsc) * fabsf(sweep) / 360.0f) + 1;
            if (n > 256) n = 256;
            if (n < 2) n = 2;
            for (i = 0; i <= n; i++) {
                float a = (a0 + sweep * (float)i / (float)n) *
                          ((float)M_PI / 180.0f);
                dl_path_pt(&c, cx + cosf(a) * rx, cy + sinf(a) * ry, 0);
            }
            break;
        }
        case DL_CLOSE:
            dl_path_close(&c);
            break;
        case DL_STROKE:
            dl_stroke_all(&c);
            break;
        case DL_FILL:
            dl_fill_all(&c);
            break;
        case DL_SETX:
            if (!dl_take(&c, 24)) goto done;
            memcpy(c.xf.m, buf + c.pos, 24);
            c.pos += 24;
            c.scale = dl_xf_scale(&c.xf);
            break;
        case DL_MULX: {
            float m[6];
            if (!dl_take(&c, 24)) goto done;
            memcpy(m, buf + c.pos, 24);
            c.pos += 24;
            dl_xf_mul(&c.xf, m);
            c.scale = dl_xf_scale(&c.xf);
            break;
        }
        case DL_PUSHX:
            if (c.nstack < 16) c.stack[c.nstack++] = c.xf;
            break;
        case DL_POPX:
            if (c.nstack > 0) c.xf = c.stack[--c.nstack];
            c.scale = dl_xf_scale(&c.xf);
            break;
        case DL_CLIP: {
            DlOp *o;
            if (!dl_take(&c, 16)) goto done;
            dl_flush_tris(&c);
            o = dl_op(&c, DLO_CLIP);
            if (o) {
                o->cx = (i32)relic_rd32le(buf + c.pos);
                o->cy = (i32)relic_rd32le(buf + c.pos + 4);
                o->cw = (i32)relic_rd32le(buf + c.pos + 8);
                o->ch = (i32)relic_rd32le(buf + c.pos + 12);
            }
            c.pos += 16;
            break;
        }
        case DL_IMAGE: {
            DlOp *o;
            i32 x, y, w, h;
            float cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3;
            if (!dl_take(&c, 20)) goto done;
            x = (i32)relic_rd32le(buf + c.pos + 4);
            y = (i32)relic_rd32le(buf + c.pos + 8);
            w = (i32)relic_rd32le(buf + c.pos + 12);
            h = (i32)relic_rd32le(buf + c.pos + 16);
            dl_to_screen(&c, (float)x, (float)y, &cx0, &cy0);
            dl_to_screen(&c, (float)(x + w), (float)y, &cx1, &cy1);
            dl_to_screen(&c, (float)(x + w), (float)(y + h), &cx2, &cy2);
            dl_to_screen(&c, (float)x, (float)(y + h), &cx3, &cy3);
            dl_flush_tris(&c);
            o = dl_op(&c, DLO_IMAGE);
            if (o) {
                o->tex = (i32)relic_rd32le(buf + c.pos);
                o->px[0] = cx0; o->px[1] = cy0;
                o->px[2] = cx1; o->px[3] = cy1;
                o->px[4] = cx2; o->px[5] = cy2;
                o->px[6] = cx3; o->px[7] = cy3;
            }
            c.pos += 20;
            break;
        }
        case DL_TEXT: {
            DlOp *o;
            float sx, sy;
            if (!dl_take(&c, 36)) goto done;
            dl_xf_pt(&c.xf, (float)(i32)relic_rd32le(buf + c.pos + 8),
                     (float)(i32)relic_rd32le(buf + c.pos + 12), &sx, &sy);
            dl_flush_tris(&c);
            o = dl_op(&c, DLO_TEXT);
            if (o) {
                float fx;
                o->text_ptr = relic_rd32le(buf + c.pos);
                o->font_ptr = relic_rd32le(buf + c.pos + 4);
                o->tx = (i32)(sx + 0.5f);
                o->ty = (i32)(sy + 0.5f);
                o->tsize = (i32)relic_rd32le(buf + c.pos + 16);
                o->tweight = (i32)relic_rd32le(buf + c.pos + 20);
                o->tjust = (i32)relic_rd32le(buf + c.pos + 24);
                {
                    u32 bits = relic_rd32le(buf + c.pos + 28);
                    memcpy(&fx, &bits, 4);
                }
                o->trot = fx;
                {
                    u32 bits = relic_rd32le(buf + c.pos + 32);
                    memcpy(&fx, &bits, 4);
                }
                o->tsx = (fx != 0.0f) ? fx : 1.0f;
                o->tr = c.cr; o->tg = c.cg; o->tb = c.cb; o->ta = c.ca;
            }
            c.pos += 36;
            break;
        }
        default:
            out->err = 2;
            goto done;
        }
    }
done:
    dl_flush_tris(&c);
    free(c.path.pts); free(c.path.subs); free(c.path.closed);
    return out->err == 0;
}

#endif /* RELIC_DRAWLIST_H */
