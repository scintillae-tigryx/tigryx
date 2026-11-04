/*
 * lines3d.h
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

/* lines3d.h - shared @lines3d implementation for every backend that
 * lacks a hand-tuned projector (the BBC and VIDEL ports
 * carry their own). All integer math, byte-wise memory access via the
 * relic_rd16le / relic_far_read / relic_far_write helpers so it is
 * correct on 16-bit far-pointer hosts and big-endian machines alike.
 *
 * Op args (r[1..10]):
 *   walls u32  - nw wall records, 8 x i16 LE: x1 z1 x2 z2 ybot ytop
 *                flags colour (flags bit0/bit1 = end posts; colour is
 *                passed through to the output record when mode&1)
 *   cam_x,cam_z, gs,gc - camera position and sin/cos of yaw, 8.8 fixed
 *   eye        - eye height, world units
 *   out u32    - output buffer, cap records
 *   cap        - max records
 *   mode       - bit0: records are 5 x i32 (x1 y1 x2 y2 colour);
 *                otherwise 4 x i32
 *              - bit3: hidden-line removal. Walls are back-face culled
 *                (one-sided only; flags bit2 marks a two-sided portal
 *                slab, never culled), staged, insertion-sorted front-
 *                to-back by nearest endpoint depth, then emitted
 *                through a per-column visibility window - the
 *                wireframe equivalent of a solid-segment clip list. A one-sided
 *                wall is solid rock and closes its columns outright; a
 *                portal narrows the window to its opening. Requires
 *                scrw <= RL3_COLS*8 (the table is fixed-size); wider
 *                screens fall back to the unclipped path.
 *              - bit7: span fill. Runs the same occ walk but, per open
 *                16-px clip column, emits filled spans instead of edge
 *                lines: a solid wall gives [ceiling|face|floor], a
 *                portal [lintel|step] around its opening, and columns
 *                still open after the last wall get a void span, so a
 *                frame tiles the whole view top-to-bottom. Records keep
 *                the line shape {x0, y0, x1, y1, col}; col is the
 *                5-bit colour word (palette index | bit4 halftone) ORed
 *                with RL3FTAG. Surface colours ride in the mode word:
 *                ceiling bits 8-12, floor 13-17, void 18-22, sky 23-27;
 *                bits 28-30 give the filled view height in 32-row
 *                bands (0 = scrh). On the BBC backend these records
 *                become host byte-column fills.
 *
 * Projection constants are the corridor-renderer subset MDOOM uses:
 * near plane 64 (8.8), manhattan far-cull 160 world units, posts
 * dropped beyond 96, coords clamped +-512, centred on scrw x scrh with
 * focal length scrw/2. Returns the record count in r[1], -1 when the
 * args are out of range.
 *
 * This is the low-end variant of the API and mirrors the BBC backend's
 * occluded projector closely, minus its cycle-level tricks (per-frame
 * product tables, u8 windows, i16 accumulators): those exist because
 * the BBC projector runs on a 4MHz parasite. Keep the two in step -
 * record emission order and clip semantics are meant to match, so a
 * per-frame diff sees a stable stream on both. */

#ifndef RELIC_LINES3D_H
#define RELIC_LINES3D_H

#include <string.h>          /* memset in rl3_occ_run */

#define RL3_NEAR   64
#define RL3_MAXD   ((i32)160 << 8)
#define RL3_MAXDW  160          /* same bound in world units (occ path) */
#define RL3_POSTD  ((i32)96 << 8)
#define RL3_CLAMP  512
#define RL3_COLS   160          /* 8-px columns; occ needs scrw <= 1280 */
#define RL3_MAXW   64           /* staged walls for the occlusion pass */
/* Vertical anamorphic correction - see the L3_YSCALE comment in
 * the BBC backend. The classic corridor view is 320x168 at focal 160;
 * showing the same world on a 320x256-class screen at that focal
 * length spreads it over ~1.5x the vertical angle, so every room
 * reads flat. x keeps focal scrw/2 (the classic 90 degree horizontal FOV).
 * The same 3/2 is applied by emit_wall in MDOOM.tgx and by
 * tools/mdoom/mdx_ref.py so all four paths frame alike. It must
 * truncate toward zero, not floor - the offsets go negative for every
 * ceiling above eye level, and MDOOM.tgx expresses it as a division
 * (Tigryx's >> is logical even on i32), so t+(t>>1) would disagree.
 * Macro: `t` must be a plain variable, it is evaluated up to 3 times. */
#define RL3_YSCALE(t) ((t) < 0 ? -((-(t) * 3) >> 1) : (((t) * 3) >> 1))

static i16 rl3_rd16(RelicVM *vm, u32 a) {
    u8 b[2];
    relic_far_read(vm->mem, a, b, 2);
    return (i16)relic_rd16le(b);
}

static i32 rl3_clamp(i32 x) {
    if (x > RL3_CLAMP)  return RL3_CLAMP;
    if (x < -RL3_CLAMP) return -RL3_CLAMP;
    return x;
}

static i16 rl3_i16(i32 v) {
    return (i16)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* Emit context: the occlusion helpers sit four calls deep, so the
 * common parameters live in file-scope statics rather than being
 * threaded through every signature (same trick as the native backends). */
static RelicVM *rl3v;
static u32      rl3out;
static i32      rl3cap, rl3recw, rl3n, rl3scrw, rl3scrh;

/* put_line: clamp, drop lines fully off one screen side (they would
 * still cost a full draw for zero pixels), drop zero-length records
 * (a lone dot reads as speckle, not geometry), store if under cap. */
static void rl3_emit(i32 x1, i32 y1, i32 x2, i32 y2, i32 col) {
    i32 n = rl3n;
    u8 rec[20];
    x1 = rl3_clamp(x1); y1 = rl3_clamp(y1);
    x2 = rl3_clamp(x2); y2 = rl3_clamp(y2);
    if ((x1 < 0 && x2 < 0) || (x1 >= rl3scrw && x2 >= rl3scrw)) return;
    if ((y1 < 0 && y2 < 0) || (y1 >= rl3scrh && y2 >= rl3scrh)) return;
    if (x1 == x2 && y1 == y2) return;
    if (n >= rl3cap) return;
    relic_wr32le(rec,      (u32)x1);
    relic_wr32le(rec + 4,  (u32)y1);
    relic_wr32le(rec + 8,  (u32)x2);
    relic_wr32le(rec + 12, (u32)y2);
    if (rl3recw == 20) relic_wr32le(rec + 16, (u32)col);
    relic_far_write(rl3v->mem, rl3out + (u32)n * (u32)rl3recw, rec,
                    (u32)rl3recw);
    rl3n = n + 1;
}

/* ---- mode bit 3: hidden-line removal -------------------------------
 * Each 8-px screen column carries the still-OPEN window [rl3wt,rl3wb]
 * (closed when wt > wb). Walls emit nearest-first; a one-sided wall
 * closes its columns outright, a portal narrows the window to its
 * opening. Edges/posts emit only where they remain inside the window.
 * Window bounds are u16 so any scrh fits; the closed sentinel is
 * wt=scrh, wb=0. */
static i16 rl3s[RL3_MAXW][10];  /* sx1 sb1 st1 sx2 sb2 st2 fl cl z1 z2 */
static u8  rl3ord[RL3_MAXW];
static int rl3nst;
static u16 rl3wt[RL3_COLS], rl3wb[RL3_COLS];
static i16 rl3ph[4][3];         /* recent post ring: x, ylo, yhi */
static int rl3phn, rl3phi;
static int rl3band;             /* mode bit 4: depth-band colour index */
static int rl3fill;             /* mode bit 7: column span-fill */
static int rl3viewh;            /* mode bits 28-30: filled view height */
static int rl3vc0, rl3vc1;      /* cap bit 24: active clip-column range */
static int rl3ccl, rl3cfl, rl3cvd, rl3csk;
#define RL3_FCOLW 16            /* fill clip column = 16 logical px */
#define RL3FTAG   0x08000000    /* fill record's colour-word tag bit */

/* mode bit 4: the record colour index gains 2*band measured on the
 * NEARER endpoint depth (the front-to-back sort key): near = within
 * POSTD/2, mid = within POSTD, far = beyond. The app's palette carries
 * the banded entries, so a distance cue costs one compare per wall and
 * nothing per pixel. */
static int rl3_dband(i32 nz) {
    if (nz > RL3_POSTD)        return 4;
    if (nz > (RL3_POSTD >> 1)) return 2;
    return 0;
}

/* Fill mode (bit 7) darkens by halftone density instead of hue: colour
 * bit 4 = checkerboard, bit 5 = sparse - a wall keeps its hue and fades
 * to black. Same thresholds as the line-mode ramp. */
static int rl3_dband_fill(i32 nz) {
    if (nz > RL3_POSTD)        return 32;
    if (nz > (RL3_POSTD >> 1)) return 16;
    return 0;
}

/* One vertical post at column x, spanning [ylo,yhi], clipped to the
 * column's open window. The window is a single interval, so this is
 * one intersection and at most one record - and therefore cannot emit
 * the same post twice, which under EOR would erase it. */
static void rl3_occ_post(i16 x, i16 ylo, i16 yhi, i16 cl) {
    i16 c, t, a, b;
    if(x < 0 || x >= rl3scrw) return;
    if(ylo > yhi) { t = ylo; ylo = yhi; yhi = t; }
    c = x >> 3;
    a = (i16)rl3wt[c]; b = (i16)rl3wb[c];
    if(a > b) return;                       /* column closed */
    if(ylo > a) a = ylo;
    if(yhi < b) b = yhi;
    /* Two walls can emit the identical post: adjacent quads sharing a
     * vertex post it once each, and a border post can coincide with a
     * corner post. Identical records EOR-erase each other, so a recent
     * duplicate is skipped. Post emitters are adjacent in depth order,
     * so a short ring suffices. */
    if(a <= b) {
        int i2;
        for(i2 = 0; i2 < rl3phn; i2++)
            if(rl3ph[i2][0] == x && rl3ph[i2][1] == a &&
               rl3ph[i2][2] == b) return;
        rl3ph[rl3phi][0] = x; rl3ph[rl3phi][1] = a; rl3ph[rl3phi][2] = b;
        rl3phi = (rl3phi + 1) & 3;
        if(rl3phn < 4) rl3phn++;
        rl3_emit(x, a, x, b, cl);
    }
}

/* Emit one visible x-run of a wall edge: exact lerp at the run ends so
 * the record stream stays deterministic for any caller-side diff. */
static void rl3_seg(i16 xa, i16 ya, i32 dy, i16 rx0, i16 rx1, i16 cl) {
    /* Clamp to the screen so an edge beyond the border emits its
     * clipped silhouette along it - the difference between "wall fills
     * your view" and a blank screen. (With no clamp a wall covering
     * every column emits nothing at all, yet still claims the window
     * and hides everything behind it.) */
    i32 y0 = ya + (((rx0 - xa) * dy) >> 8);
    i32 y1 = ya + (((rx1 - xa) * dy) >> 8);
    if (y0 < 0) y0 = 0; else if (y0 >= rl3scrh) y0 = rl3scrh - 1;
    if (y1 < 0) y1 = 0; else if (y1 >= rl3scrh) y1 = rl3scrh - 1;
    rl3_emit(rx0, y0, rx1, y1, cl);
}

/* One staged wall through the clip table: a single 8-px column walk
 * serves the ceiling edge, the floor edge and the window claim at once
 * - the edge y at a column IS the claim bound, so both edges' visible
 * runs and the narrowed window share the two interpolated
 * accumulators.
 *
 * A solid (one-sided) wall closes its columns; a portal narrows them
 * to its opening. Both claims happen after that column's visibility
 * test, so a wall is never occluded by itself - and the posts above
 * are emitted before any claim, so a wall's own corner posts survive
 * its claim too. ty/by are i32 where the BBC uses i16: a wall spanning
 * more than the screen vertically cannot wrap here. */
static void rl3_occ_wall(const i16 *s) {
    i16 sx1 = s[0], sb1 = s[1], st1 = s[2];
    i16 sx2 = s[3], sb2 = s[4], st2 = s[5];
    i16 fl = s[6], cl = s[7];
    i32 dt, db, ty, by, dt4, db4;
    i32 sh1 = rl3scrh - 1;
    i16 t, x0, x1, c0, c1, c, rt, rb;
    int solid = !(fl & 4);
    /* Quad entirely above or below the screen contributes nothing and
     * must not claim either - without this it closes the column anyway
     * and hides geometry it never covered. */
    if((sb1 < 0 && sb2 < 0) || (st1 > sh1 && st2 > sh1))
        return;
    if(sx1 == sx2) {
        i16 lo = sb1, hi = sb1;
        if(sb2 < lo) lo = sb2; if(st1 < lo) lo = st1; if(st2 < lo) lo = st2;
        if(sb2 > hi) hi = sb2; if(st1 > hi) hi = st1; if(st2 > hi) hi = st2;
        rl3_occ_post(sx1, lo, hi, cl);
        /* Edge-on: a solid sliver still closes its column; an edge-on
         * portal has no opening to speak of and occludes nothing. */
        if(solid && sx1 >= 0 && sx1 < rl3scrw) {
            c = sx1 >> 3; rl3wt[c] = (u16)rl3scrh; rl3wb[c] = 0;
        }
        return;
    }
    if(sx2 < sx1) {
        t = sx1; sx1 = sx2; sx2 = t;
        t = sb1; sb1 = sb2; sb2 = t;
        t = st1; st1 = st2; st2 = t;
    }
    x0 = sx1 < 0 ? 0 : sx1;
    x1 = sx2 > rl3scrw - 1 ? (i16)(rl3scrw - 1) : sx2;
    if(x0 > x1) return;
    /* Posts first: this wall's own claim (below) covers their columns
     * exactly, so they must draw while only NEARER walls have claimed. */
    if((fl & 1) && s[8] <= (i16)RL3_POSTD)
        rl3_occ_post(s[0], s[2], s[1], cl);
    if((fl & 2) && s[9] <= (i16)RL3_POSTD)
        rl3_occ_post(s[3], s[5], s[4], cl);
    c0 = x0 >> 3; c1 = x1 >> 3;
    /* One scan classifies the span before the column walk, since both
     * fast cases are common: a span still fully open emits both edges
     * as one record each and claims with no visibility test; a span
     * fully closed cannot emit at all, and a solid claim on it is
     * already a no-op. `wt | (wb ^ sh1)` is zero only for a fully open
     * column, so both flags come out of the same two loads. */
    {
        int cls = 0;
        for(c = c0; c <= c1; c++) {
            i16 wt = (i16)rl3wt[c], wb = (i16)rl3wb[c];
            if(wt <= wb) cls |= 1;                    /* something open */
            if(wt | (wb ^ sh1)) cls |= 2;             /* not fully open */
        }
        if(!(cls & 1)) return;
        dt = ((i32)(st2 - st1) << 8) / (sx2 - sx1);
        db = ((i32)(sb2 - sb1) << 8) / (sx2 - sx1);
        ty = ((i32)st1 << 4) + (((x0 - sx1) * dt) >> 4);
        by = ((i32)sb1 << 4) + (((x0 - sx1) * db) >> 4);
        dt4 = dt >> 1; db4 = db >> 1;
        /* Border posts: a wall running off the left or right edge has
         * its clipped silhouette in the border column - the span the
         * quad covers there. Without these a wall filling the whole
         * screen emits only its two border edges and the frame reads
         * as empty space. */
        /* Solid walls only: a portal's border post would draw across
         * its own opening, and two portals both spanning a border
         * column with the window still open would emit the identical
         * record - EOR-erasing each other. sx2 == 0 / sx1 == scrw-1
         * ends exactly at the border: the corner post already draws
         * that column. */
        if(sx1 < 0 && sx2 > 0 && solid && s[8] <= (i16)RL3_POSTD)
            rl3_occ_post(0, (i16)(ty >> 4), (i16)(by >> 4), cl);
        if(sx2 > rl3scrw - 1 && sx1 < rl3scrw - 1 && solid &&
           s[9] <= (i16)RL3_POSTD)
            rl3_occ_post((i16)(rl3scrw - 1),
                         (i16)(st1 + (((rl3scrw - 1 - sx1) * dt) >> 8)),
                         (i16)(sb1 + (((rl3scrw - 1 - sx1) * db) >> 8)),
                         cl);
        if(cls & 2) goto partial;
    }
    rl3_seg(sx1, st1, dt, x0, x1, cl);
    rl3_seg(sx1, sb1, db, x0, x1, cl);
    if(solid) {
        for(c = c0; c <= c1; c++) { rl3wt[c] = (u16)rl3scrh; rl3wb[c] = 0; }
        return;
    }
    for(c = c0; c <= c1; c++) {
        i32 wt = ty >> 4, wb = by >> 4;
        /* Where a just-emitted edge lies on a screen border that row
         * is now drawn: fold it out of the open window so a later
         * wall's edge reaching the same border does not emit the
         * identical line and EOR-erase it. Either edge can sit on
         * either border (a tilted quad's floor edge can land on the
         * top row), so both test both bounds. */
        rl3wt[c] = (u16)((wt <= 0 || wb <= 0) ? 1 : wt > sh1 ? sh1 : wt);
        rl3wb[c] = (u16)((wt >= sh1 || wb >= sh1) ? sh1 - 1
                                                 : wb < 0 ? 0 : wb);
        ty += dt4; by += db4;
    }
    return;
partial:
    rt = rb = -1;
    for(c = c0; c <= c1 + 1; c++) {
        i32 yt = ty >> 4, yb = by >> 4;
        i16 rx0, rx1;
        int vt = 0, vb = 0;
        if(c <= c1) {
            i16 wt = (i16)rl3wt[c], wb = (i16)rl3wb[c];
            if(wt <= wb) {               /* open: visible iff inside */
                /* An edge inside the open window emits where it is. A
                 * SOLID edge past the screen border emits too - rl3_seg
                 * clamps it onto the border, giving a screen-filling
                 * wall its silhouette - but only when the bound is the
                 * screen edge itself (wt=0 / wb=sh1): a claimed bound
                 * belongs to a nearer occluder whose edge is already
                 * drawn there, and redrawing it would EOR-erase it.
                 * Solid-only: a portal's off-screen edge is an opening
                 * whose lintel/sill lies beyond the screen - there is
                 * no edge to draw and it never narrows the open bound,
                 * so two portals would emit the identical border line
                 * (EOR-erasing each other). The face must still reach
                 * the border (yb >= wt / yt <= wb) or the quad never
                 * touches the screen. */
                vt = (yt >= wt && yt <= wb) ||
                     (solid && yt < 0 && wt == 0 && yb >= wt);
                vb = (yb >= wt && yb <= wb) ||
                     (solid && yb > sh1 && wb == sh1 && yt <= wb);
                /* Claim after the test: close outright if solid, else
                 * narrow to this column's slice of the opening. */
                if(solid) { rl3wt[c] = (u16)rl3scrh; rl3wb[c] = 0; }
                else {
                    i32 ytc = yt < 0 ? 0 : yt > sh1 ? sh1 : yt;
                    i32 ybc = yb < 0 ? 0 : yb > sh1 ? sh1 : yb;
                    if(ytc > wt) rl3wt[c] = (u16)ytc;
                    if(ybc < wb) rl3wb[c] = (u16)ybc;
                }
                /* A piece emitted ON the border row consumes it: a
                 * later wall's edge reaching the same border must not
                 * emit the identical line and EOR-erase it. */
                if(((vt && yt <= 0) || (vb && yb <= 0)) &&
                   rl3wt[c] == 0) rl3wt[c] = 1;
                if(((vt && yt >= sh1) || (vb && yb >= sh1)) &&
                   rl3wb[c] == sh1) rl3wb[c] = (u16)(sh1 - 1);
            }
        }
        if(vt) { if(rt < 0) rt = c; }
        else if(rt >= 0) {
            rx0 = rt * 8; rx1 = c * 8 - 1;
            if(rx0 < x0) rx0 = x0; if(rx1 > x1) rx1 = x1;
            rl3_seg(sx1, st1, dt, rx0, rx1, cl); rt = -1;
        }
        if(vb) { if(rb < 0) rb = c; }
        else if(rb >= 0) {
            rx0 = rb * 8; rx1 = c * 8 - 1;
            if(rx0 < x0) rx0 = x0; if(rx1 > x1) rx1 = x1;
            rl3_seg(sx1, sb1, db, rx0, rx1, cl); rb = -1;
        }
        ty += dt4; by += db4;
    }
}

/* Emit staged walls front-to-back through the column clip table.
 * Insertion sort on min view-z: the list is small and a stable order
 * keeps the emitted records deterministic, so a per-frame diff sees
 * zero changes on a static camera. */
static void rl3_occ_run(void) {
    int i2, j;
    rl3phn = 0; rl3phi = 0;
    memset(rl3wt, 0, sizeof rl3wt);              /* whole screen open */
    for(i2 = 0; i2 < RL3_COLS; i2++)
        rl3wb[i2] = (u16)(rl3scrh - 1);
    for(i2 = 1; i2 < rl3nst; i2++) {
        u8 k = rl3ord[i2];
        i16 kz = rl3s[k][8] < rl3s[k][9] ? rl3s[k][8] : rl3s[k][9];
        for(j = i2; j > 0; j--) {
            const i16 *p = rl3s[rl3ord[j - 1]];
            if((p[8] < p[9] ? p[8] : p[9]) <= kz) break;
            rl3ord[j] = rl3ord[j - 1];
        }
        rl3ord[j] = k;
    }
    for(i2 = 0; i2 < rl3nst; i2++)
        rl3_occ_wall(rl3s[rl3ord[i2]]);
}

/* ---- mode bit 7: span fill -----------------------------------------
 * Mirrors the BBC fill-mode occlusion walk: 16-px clip columns (the BBC
 * column = 4 MODE 2 byte-columns), one fill per column region, stored
 * as {x0, y0, x1, y1, col|RL3FTAG} so the record stream carries the
 * same tag the BBC model path emits for its VDU rect plots. */

/* One fill column's span [yt,yb], clipped to the filled view height. */
static void rl3_emit_fill(i32 c, i32 yt, i32 yb, i32 cl) {
    i32 n = rl3n;
    u8 rec[20];
    if (yt > yb) return;
    if (yb < 0 || yt >= rl3viewh) return;
    if (yt < 0) yt = 0;
    if (yb > rl3viewh - 1) yb = rl3viewh - 1;
    if (n >= rl3cap) return;
    relic_wr32le(rec,      (u32)(c * RL3_FCOLW));
    relic_wr32le(rec + 4,  (u32)yt);
    relic_wr32le(rec + 8,  (u32)(c * RL3_FCOLW + RL3_FCOLW - 1));
    relic_wr32le(rec + 12, (u32)yb);
    if (rl3recw == 20) relic_wr32le(rec + 16, (u32)(cl | RL3FTAG));
    relic_far_write(rl3v->mem, rl3out + (u32)n * (u32)rl3recw, rec,
                    (u32)rl3recw);
    rl3n = n + 1;
}

static void rl3_occ_wall_fill(const i16 *s) {
    i16 sx1 = s[0], sb1 = s[1], st1 = s[2];
    i16 sx2 = s[3], sb2 = s[4], st2 = s[5];
    i16 fl = s[6], cl = s[7];
    i32 dt, db, ty, by;
    i16 t, x0, x1, c0, c1, c;
    int solid = !(fl & 4);
    if((sb1 < 0 && sb2 < 0) || (st1 > rl3scrh - 1 && st2 > rl3scrh - 1))
        return;
    if(sx1 == sx2) {
        /* Edge-on: fill the window-clipped sliver; solid still closes. */
        i32 lo = sb1, hi = sb1;
        if(sb2 < lo) lo = sb2; if(st1 < lo) lo = st1; if(st2 < lo) lo = st2;
        if(sb2 > hi) hi = sb2; if(st1 > hi) hi = st1; if(st2 > hi) hi = st2;
        if(sx1 >= rl3vc0 * RL3_FCOLW && sx1 < (rl3vc1 + 1) * RL3_FCOLW) {
            i32 a, b;
            c = (i16)(sx1 / RL3_FCOLW);
            a = rl3wt[c]; b = rl3wb[c];
            if(lo > a) a = lo;
            if(hi < b) b = hi;
            if(a <= b) rl3_emit_fill(c, a, b, cl);
            if(solid) { rl3wt[c] = (u16)rl3scrh; rl3wb[c] = 0; }
        }
        return;
    }
    if(sx2 < sx1) {
        t = sx1; sx1 = sx2; sx2 = t;
        t = sb1; sb1 = sb2; sb2 = t;
        t = st1; st1 = st2; st2 = t;
    }
    x0 = sx1 < 0 ? 0 : sx1;
    x1 = sx2 > rl3scrw - 1 ? (i16)(rl3scrw - 1) : sx2;
    { i32 vx0 = rl3vc0 * RL3_FCOLW, vx1 = (rl3vc1 + 1) * RL3_FCOLW - 1;
      if(x0 < vx0) x0 = (i16)vx0;
      if(x1 > vx1) x1 = (i16)vx1; }
    if(x0 > x1) return;
    c0 = (i16)(x0 / RL3_FCOLW); c1 = (i16)(x1 / RL3_FCOLW);
    dt = ((i32)(st2 - st1) << 8) / (sx2 - sx1);
    db = ((i32)(sb2 - sb1) << 8) / (sx2 - sx1);
    ty = ((i32)st1 << 4) + (((x0 - sx1) * dt) >> 4);
    by = ((i32)sb1 << 4) + (((x0 - sx1) * db) >> 4);
    /* Per-column step in the 4.12 accumulators: dt is per-pixel *256,
     * so 16 px contributes (dt*16)>>4 = dt. */
    for(c = c0; c <= c1; c++) {
        i32 yt = ty >> 4, yb = by >> 4;
        i32 wt = rl3wt[c], wb = rl3wb[c];
        i32 a, b;
        if(wt <= wb) {
            a = yt; if(a < wt) a = wt; if(a > wb + 1) a = wb + 1;
            b = yb; if(b > wb) b = wb; if(b < wt - 1) b = wt - 1;
            if(solid) {
                if(a > wt)
                    rl3_emit_fill(c, wt, a - 1,
                                  (fl & 8) ? rl3csk : rl3ccl);
                if(a <= b) rl3_emit_fill(c, a, b, cl);
                if(b < wb) rl3_emit_fill(c, b + 1, wb, rl3cfl);
                rl3wt[c] = (u16)rl3scrh; rl3wb[c] = 0;
            } else {
                if(a > wt) rl3_emit_fill(c, wt, a - 1, cl);
                if(b < wb) rl3_emit_fill(c, b + 1, wb, cl);
                if(a > b) { rl3wt[c] = (u16)rl3scrh; rl3wb[c] = 0; }
                else      { rl3wt[c] = (u16)a;  rl3wb[c] = (u16)b; }
            }
        }
        ty += dt; by += db;             /* 16 px per column */
    }
}

static void rl3_occ_run_fill(void) {
    int i2, j;
    memset(rl3wt, 0, sizeof rl3wt);
    for(i2 = 0; i2 < RL3_COLS; i2++)
        rl3wb[i2] = (u16)(rl3scrh - 1);
    for(i2 = 1; i2 < rl3nst; i2++) {
        u8 k = rl3ord[i2];
        i16 kz = rl3s[k][8] < rl3s[k][9] ? rl3s[k][8] : rl3s[k][9];
        for(j = i2; j > 0; j--) {
            const i16 *p = rl3s[rl3ord[j - 1]];
            if((p[8] < p[9] ? p[8] : p[9]) <= kz) break;
            rl3ord[j] = rl3ord[j - 1];
        }
        rl3ord[j] = k;
    }
    for(i2 = 0; i2 < rl3nst; i2++)
        rl3_occ_wall_fill(rl3s[rl3ord[i2]]);
    /* Columns still open tile out with the void span, so the record
     * stream covers the whole filled view every frame - same contract
     * as the BBC walk. Margin columns get the letterbox bar (palette 0
     * black) instead: the BBC host driver paints those itself. */
    {
        i32 ncol = (rl3scrw + RL3_FCOLW - 1) / RL3_FCOLW;
        for(i2 = 0; i2 < ncol; i2++) {
            if(i2 < rl3vc0 || i2 > rl3vc1) {
                rl3_emit_fill(i2, 0, rl3scrh - 1, 0);
                continue;
            }
            if(rl3wt[i2] <= rl3wb[i2])
                rl3_emit_fill(i2, rl3wt[i2], rl3wb[i2], rl3cvd);
        }
    }
}

/* mode bit 5: indexed wall blob - i16 vertex count, nv x {x,z}, then
 * nw x {v1,v2,ybot,ytop,flags,col}. Expanded into this scratch once
 * per call so the projection loop stays format-agnostic (the BBC
 * native path indexes the vertex cache directly instead). */
static i16 rl3xbuf[256 * 8];

/* Projects the wall list; caller supplies the logical screen size.
 * Sets r[1] to the record count, or -1 for bad args. */
static void relic_lines3d(RelicVM *vm, i32 scrw, i32 scrh) {
    u32 w = (u32)vm->r[1], out = (u32)vm->r[8];
    i32 nw = vm->r[2], cap = vm->r[9], n = 0, i;
    i32 cx = vm->r[3], cz = vm->r[4], gs = vm->r[5], gc = vm->r[6];
    i32 eye8 = vm->r[7] << 8, mode = vm->r[10];
    i32 cxx = scrw / 2, cyy = scrh / 2, fsc = scrw / 2;
    i32 recw = (mode & 1) ? 20 : 16;
    /* mode bit 3 = hidden-line removal through the staged column clip;
     * bit 7 (span fill) implies the same staging with a span emit.
     * The table is a fixed RL3_COLS entries, so wider screens keep the
     * original unclipped path rather than clip wrongly. */
    int occ = (mode & 136) && scrw <= RL3_COLS * 8;
    i32 cxw = cx >> 8, czw = cz >> 8;
    int rl3xh = 0;
    /* Fill mode packs extra fields into high bytes (TROOMS's SDXDATA):
     * cap bits 16-23 visible-room mask, bit 24 letterbox inset, ybot
     * bits 8-15 room tags, ytop bits 8-15 portal ceiling, flags/col
     * bits 8-15 portal floors. The room tags are ignored here (PVS is
     * a BBC-backend optimisation) - strip them so the base fields read
     * as before. */
    rl3vc0 = ((mode & 128) && ((cap >> 24) & 1)) ? 2 : 0;
    rl3vc1 = rl3vc0 ? scrw / RL3_FCOLW - 3 : scrw / RL3_FCOLW - 1;
    if (mode & 128) cap &= 0xFFFF;
    if (w + 16 > vm->mem_sz || out + 16 > vm->mem_sz || nw <= 0 || cap <= 0
        || scrw <= 0 || scrh <= 0) {
        vm->r[1] = -1;
        return;
    }
    if (mode & 32) {
        i32 k, nv;
        if (nw > 256 || w + 4 > vm->mem_sz) { vm->r[1] = -1; return; }
        nv = (i32)rl3_rd16(vm, w);
        if (nv < 0 || nv > 512 ||
            w + 2u + (u32)nv * 4u + (u32)nw * 12u > vm->mem_sz) {
            vm->r[1] = -1;
            return;
        }
        for (k = 0; k < nw; k++) {
            u32 rb = w + 2u + (u32)nv * 4u + (u32)k * 12u;
            i32 v1 = (i32)rl3_rd16(vm, rb);
            i32 v2 = (i32)rl3_rd16(vm, rb + 2);
            if (v1 < 0 || v1 >= nv) v1 = 0;
            if (v2 < 0 || v2 >= nv) v2 = 0;
            rl3xbuf[k * 8 + 0] = rl3_rd16(vm, w + 2u + (u32)v1 * 4u);
            rl3xbuf[k * 8 + 1] = rl3_rd16(vm, w + 4u + (u32)v1 * 4u);
            rl3xbuf[k * 8 + 2] = rl3_rd16(vm, w + 2u + (u32)v2 * 4u);
            rl3xbuf[k * 8 + 3] = rl3_rd16(vm, w + 4u + (u32)v2 * 4u);
            rl3xbuf[k * 8 + 4] = rl3_rd16(vm, rb + 4);
            rl3xbuf[k * 8 + 5] = rl3_rd16(vm, rb + 6);
            rl3xbuf[k * 8 + 6] = rl3_rd16(vm, rb + 8);
            rl3xbuf[k * 8 + 7] = rl3_rd16(vm, rb + 10);
            if (mode & 128) {
                rl3xbuf[k * 8 + 4] &= 0xFF;
                rl3xbuf[k * 8 + 5] &= 0xFF;
                rl3xbuf[k * 8 + 6] &= 0xFF;
                rl3xbuf[k * 8 + 7] &= 0x3F;
            }
        }
        rl3xh = 1;
    }
#define RL3R16(b, o) \
    (rl3xh ? (i32)rl3xbuf[((b) + (o)) >> 1] : (i32)rl3_rd16(vm, (b) + (o)))
    rl3v = vm; rl3out = out; rl3cap = cap; rl3recw = recw; rl3n = 0;
    rl3scrw = scrw; rl3scrh = scrh; rl3nst = 0;
    rl3band = (mode & 16) != 0;
    rl3fill = (mode & 128) != 0;
    rl3viewh = (mode >> 28) ? (mode >> 28) << 5 : scrh;
    if (rl3viewh > scrh) rl3viewh = scrh;
    if (rl3fill) {
        rl3ccl = (mode >> 8)  & 31;
        rl3cfl = (mode >> 13) & 31;
        rl3cvd = (mode >> 18) & 31;
        rl3csk = (mode >> 23) & 31;
    }
    for (i = 0; i < nw; i++) {
        u32 b = (u32)i * 16u + (rl3xh ? 0 : w);
        i32 dx1, dz1, dx2, dz2, z1, x1, z2, x2, sx1, sx2;
        i32 sb1, sb2, st1, st2, fl, cl, f;
        i32 m1, m2, m3, m4;
        if (!rl3xh && b + 16 > vm->mem_sz) break;
        {
            i32 v1 = RL3R16(b, 0), v2 = RL3R16(b, 2);
            i32 v3 = RL3R16(b, 4), v4 = RL3R16(b, 6);
            dx1 = (v1 << 8) - cx; dz1 = (v2 << 8) - cz;
            dx2 = (v3 << 8) - cx; dz2 = (v4 << 8) - cz;
            if (occ) {
                /* Far cull + back-face cull in world units (camera on
                 * the RIGHT of x1,z1 -> x2,z2, the generator's winding;
                 * flags bit2 portals are exempt). World units keep the
                 * sign test's products inside i32, and ~85% of typical WAD
                 * geometry is axis-aligned where one wall delta is zero
                 * and the sign falls out of operand signs alone. */
                i32 wx1 = v1 - cxw, wz1 = v2 - czw;
                i32 wx2 = v3 - cxw, wz2 = v4 - czw;
                m1 = wx1 < 0 ? -wx1 : wx1; m2 = wz1 < 0 ? -wz1 : wz1;
                m3 = wx2 < 0 ? -wx2 : wx2; m4 = wz2 < 0 ? -wz2 : wz2;
                if (m1 + m2 > RL3_MAXDW && m3 + m4 > RL3_MAXDW) continue;
                fl = RL3R16(b, 12);
                if (!(fl & 4)) {
                    i32 wdx = wx2 - wx1, wdz = wz2 - wz1;
                    if (wdz == 0) {           /* cull iff wdx*wz1 <= 0 */
                        if (wdx == 0 || wz1 == 0) continue;
                        if ((wdx < 0) != (wz1 < 0)) continue;
                    } else if (wdx == 0) {    /* cull iff wdz*wx1 >= 0 */
                        if (wx1 == 0) continue;
                        if ((wdz < 0) == (wx1 < 0)) continue;
                    } else if (wdz * wx1 - wdx * wz1 >= 0) continue;
                }
            }
        }
        if (!occ) {
            m1 = dx1 < 0 ? -dx1 : dx1; m2 = dz1 < 0 ? -dz1 : dz1;
            m3 = dx2 < 0 ? -dx2 : dx2; m4 = dz2 < 0 ? -dz2 : dz2;
            if (m1 + m2 > RL3_MAXD && m3 + m4 > RL3_MAXD) continue;
        }

        z1 = (dx1 * gs + dz1 * gc) / 256;
        x1 = (dx1 * gc - dz1 * gs) / 256;
        z2 = (dx2 * gs + dz2 * gc) / 256;
        x2 = (dx2 * gc - dz2 * gs) / 256;
        if (z1 < RL3_NEAR && z2 < RL3_NEAR) continue;
        if (z1 < RL3_NEAR) {
            f = ((RL3_NEAR - z1) * 256) / (z2 - z1);
            x1 = x1 + ((x2 - x1) * f) / 256;
            z1 = RL3_NEAR;
        }
        if (z2 < RL3_NEAR) {
            f = ((RL3_NEAR - z2) * 256) / (z1 - z2);
            x2 = x2 + ((x1 - x2) * f) / 256;
            z2 = RL3_NEAR;
        }
        sx1 = cxx + (x1 * fsc) / z1;
        sx2 = cxx + (x2 * fsc) / z2;
        if (sx1 < 0 && sx2 < 0) continue;
        if (sx1 >= scrw && sx2 >= scrw) continue;

        {
            i32 yb = (RL3R16(b, 8)  << 8) - eye8;
            i32 yt = (RL3R16(b, 10) << 8) - eye8;
            /* Locals: RL3_YSCALE re-reads its argument. */
            i32 pb1 = (yb * fsc) / z1, pb2 = (yb * fsc) / z2;
            i32 pt1 = (yt * fsc) / z1, pt2 = (yt * fsc) / z2;
            sb1 = cyy - RL3_YSCALE(pb1);
            sb2 = cyy - RL3_YSCALE(pb2);
            st1 = cyy - RL3_YSCALE(pt1);
            st2 = cyy - RL3_YSCALE(pt2);
        }
        cl = RL3R16(b, 14);
        if (rl3band) {
            if (rl3fill) cl += rl3_dband_fill(z1 < z2 ? z1 : z2);
            else         cl += rl3_dband(z1 < z2 ? z1 : z2);
        }
        if (!occ) fl = RL3R16(b, 12);

        if (occ) {
            /* Hidden-line mode: stage the projected wall; emission is
             * deferred to the front-to-back window pass below. */
            i16 *s;
            if (rl3nst >= RL3_MAXW) continue;
            s = rl3s[rl3nst];
            /* sb/st are clamped tighter than i16: +-2047 is still ~4x
             * the largest supported screen height, so every window
             * test and clamped-edge emit reads identically, while the
             * tighter bound keeps downstream fixed-point safe. Same
             * bound as the BBC staging, for identical record output. */
            s[0] = rl3_i16(sx1); s[3] = rl3_i16(sx2);
            s[1] = rl3_i16(sb1 < -2047 ? -2047 : sb1 > 2047 ? 2047 : sb1);
            s[2] = rl3_i16(st1 < -2047 ? -2047 : st1 > 2047 ? 2047 : st1);
            s[4] = rl3_i16(sb2 < -2047 ? -2047 : sb2 > 2047 ? 2047 : sb2);
            s[5] = rl3_i16(st2 < -2047 ? -2047 : st2 > 2047 ? 2047 : st2);
            s[6] = rl3_i16(fl);  s[7] = rl3_i16(cl);
            s[8] = rl3_i16(z1);  s[9] = rl3_i16(z2);
            rl3ord[rl3nst] = (u8)rl3nst;
            rl3nst++;
            continue;
        }

        if (sx1 == sx2) {
            /* Edge-on wall: every candidate line is a vertical at sx1 -
             * one spanning line carries the same silhouette. */
            i32 lo = sb1, hi = sb1;
            if (sb2 < lo) lo = sb2;
            if (st1 < lo) lo = st1;
            if (st2 < lo) lo = st2;
            if (sb2 > hi) hi = sb2;
            if (st1 > hi) hi = st1;
            if (st2 > hi) hi = st2;
            rl3_emit(sx1, lo, sx1, hi, cl);
            continue;
        }
        /* Emit order: floor edge, ceiling edge, post@end1, post@end2.
         * Posts beyond POSTD collapse to sub-pixel specks - cull. */
        rl3_emit(sx1, sb1, sx2, sb2, cl);
        rl3_emit(sx1, st1, sx2, st2, cl);
        if ((fl & 1) && z1 <= RL3_POSTD)
            rl3_emit(sx1, sb1, sx1, st1, cl);
        if ((fl & 2) && z2 <= RL3_POSTD)
            rl3_emit(sx2, sb2, sx2, st2, cl);
    }
    if (occ) { if (rl3fill) rl3_occ_run_fill(); else rl3_occ_run(); }
    n = rl3n;
    vm->r[1] = n;
#undef RL3R16
}

#endif /* RELIC_LINES3D_H */
