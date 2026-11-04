/*
 * sw3d.h
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
 * sw3d.h - shared software 3D rasteriser for linear true-colour targets.
 *
 * Ported from the allegro-dos backend's software rasteriser (which was
 * itself modelled on relic3d-win32): top-left fill convention scanline
 * rasteriser with a 16-bit z-buffer, Lambert per-vertex lighting,
 * Sutherland-Hodgman near-plane clip, back-face cull, sub-span
 * perspective-correct texturing.
 *
 * Unlike the DOS original this unit knows nothing about the VM, GEMDOS
 * or Allegro: the backend feeds it plain pointers. The render target is
 * a 16bpp RGB565 linear framebuffer plus a u16 z-buffer, both owned by
 * the caller (the backend decides which memory pool they live in - on
 * Atari the framebuffer is ST-RAM and the z-buffer prefers TT-RAM).
 *
 * Endianness: all arrays are HOST-ORDER. Guest vertex data is a native
 * float array (x,y,z,nx,ny,nz,u,v x N); index buffers are native
 * u16/u32. Backends must pass pointers into vm->mem unchanged - do NOT
 * byte-assemble little-endian (that broke the DOS code on BE hosts and
 * silently reads garbage on m68k).
 *
 * RELIC_FIXED_POINT_3D: define for FPU-less targets. Per-scanline edge
 * walking and the perspective UV sub-span loop switch to 16.16 integer
 * math (identical port of the allegro-dos soft-float path). With the
 * FPU build (-m68881) leave it undefined for full-precision float.
 */
#ifndef RELIC_SW3D_H
#define RELIC_SW3D_H

#include "tigryx.h"   /* u8/u16/u32/i32 etc. */

#ifdef __cplusplus
extern "C" {
#endif

/* Texture slot table sizing. The table grows on demand (see
 * sw3d_tex_reserve) instead of being a fixed array, so a small
 * program pays for 256 slots while one with many textures can grow
 * further - SW3D_MAX_TEX is now the ceiling, not the allocation
 * (@texture yields a u16 slot id, so 65535 is the highest
 * addressable). The table must stay in lock-step with the backend's
 * own slot ids, so this ceiling matches theirs. */
#define SW3D_TEX_CAP_INIT 256
#define SW3D_MAX_TEX 65535

typedef struct Sw3dTex {
    u8 *px;           /* RGBA32, POT dimensions */
    int w, h;         /* POT */
    int src_w, src_h; /* original size (UV scale) */
    int w_mask, h_mask;
    int has_alpha;
} Sw3dTex;

typedef struct Sw3d {
    /* Camera */
    float proj[16], view[16], mvp[16];
    float znear, zfar;
    int   cam_mirror, have_camera;
    /* projection-parameter cache (tanf is expensive on soft-float) */
    int   proj_valid;
    float proj_fov, proj_aspect, proj_znear, proj_zfar;
    int   proj_mirror;
    /* Directional light */
    int   light_on;
    float light_dir[3], light_rgb[3], light_amb[3];
    /* Textures: grown on demand by sw3d_tex_reserve; tex_cap is the
     * allocated slot count. Slot 0 is reserved as "no texture". */
    Sw3dTex *tex;
    int      tex_cap;
} Sw3d;

void sw3d_init(Sw3d *s);
void sw3d_free(Sw3d *s);

/* fb: RGB565 back page to rasterise into (re-point it every frame if
 * the backend page-flips). zbuf: w*h u16 scratch owned by the caller.
 * sw3d_mesh() silently no-ops until fb AND zbuf AND camera are set. */
void sw3d_bind(Sw3d *s, u16 *fb, u16 *zbuf, int w, int h);

/* Paletted variant: fb8 is a chunky 8bpp surface; each shaded pixel's
 * RGB565 value is mapped to the nearest entry of pal768 (768 bytes,
 * RGB triples) through a lazily-filled RGB555 cache. Call again (or
 * sw3d_pal8) whenever the palette changes - the cache is per palette.
 * Binding here replaces any sw3d_bind target and vice versa. */
void sw3d_bind8(Sw3d *s, u8 *fb8, u16 *zbuf, int w, int h,
                const u8 *pal768);
void sw3d_pal8(Sw3d *s, const u8 *pal768);

/* @camera: build proj+view+mvp, clear the bound z-buffer. */
void sw3d_camera(Sw3d *s,
                 const float eye[3], const float tgt[3], const float up[3],
                 float fov_deg, float znear, float zfar);

/* @light: dir==NULL disables lighting. rgb/amb are RGB888 or NULL for
 * defaults (white light / 0.1 ambient). */
void sw3d_light(Sw3d *s, const float dir[3], const u8 *rgb, const u8 *amb);

/* Store an RGBA32 texture at an explicit slot (kept in lock-step with
 * the backend's own texture ids so @mesh's tex_id indexes both). */
int  sw3d_tex_store(Sw3d *s, int slot, const u8 *rgba, int w, int h);

/* @mesh: verts is vcount * 8 floats {x,y,z,nx,ny,nz,u,v}; idx is a
 * native u16 index array, or u32 when icount < 0 (bit31 = RLM v3
 * 32-bit-index flag). rgba is the flat tint or NULL for white. */
void sw3d_mesh(Sw3d *s, const float *verts, int vcount,
               const void *idx, int icount, int tex_id, const u8 *rgba);

/* Debug: count of triangles that survived clipping and reached the
 * rasteriser since the last reset. */
int sw3d_debug_tris(void);
void sw3d_debug_tris_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* RELIC_SW3D_H */
