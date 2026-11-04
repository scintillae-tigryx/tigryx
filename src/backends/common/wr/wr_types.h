/*
 * wr_types.h
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

/* wr_types.h - shared on-disk WAD map structs for the sector-based "wr_*"
 * world renderer (see OP_WR_* in tigryx.h). Split out from the renderer
 * itself (wr_render.h) because these types are needed by a backend's
 * state struct (e.g. AllegroState's wr_vertices/wr_linedefs/... fields),
 * which is normally declared long before the renderer functions that use
 * it - so this header is included near the TOP of a backend .c file,
 * while wr_render.h is included further down, after the state struct is
 * fully defined.
 *
 * IMPORTANT: this file is included verbatim into multiple backend
 * translation units (allegro-fixed-dos, allegro-dos, allegro-tinygl-dos,
 * opengl-win32, opengl-linux, opengl-macos, opengl-emscripten,
 * relic3d-win32). It must never depend on anything backend-specific - only
 * on the portable integer typedefs (i16/i32/u8/...) that every backend
 * already has from tigryx.h before including this. */
#ifndef RELIC_WR_TYPES_H
#define RELIC_WR_TYPES_H

/* Field order/sizes must exactly match samples/adoom's state.tgx (Vertex/
 * Linedef/Sidedef/Sector/Seg/Subsector/BSPNode + its SIZEOF_* constants)
 * and the copy in tigryx_rc_shim.c's r_wr_map. All fields are i16/i32/
 * u8[8], so GCC's default alignment already packs these with no padding
 * (verified against each SIZEOF_* below); __attribute__((packed)) /
 * #pragma pack is added anyway since correctness here depends on exactly
 * matching an external binary layout, not on whatever the default
 * happens to be. */
#pragma pack(push, 1)
typedef struct { i16 x, y; } WrVertex;                              /* 4 bytes */
typedef struct { i16 v1, v2, flags, special, tag, front, back; } WrLinedef; /* 14 bytes */
typedef struct { i16 x_off, y_off; u8 upper_tex[8], lower_tex[8], mid_tex[8]; i16 sector; } WrSidedef; /* 30 bytes */
typedef struct { i16 floor_h, ceil_h; u8 floor_flat[8], ceil_flat[8]; i16 light, special, tag; } WrSector; /* 26 bytes */
typedef struct { i16 v1, v2, angle, linedef, side, offset, length; } WrSeg; /* 14 bytes */
typedef struct { i16 numsegs, firstseg; } WrSubsector;              /* 4 bytes */
/* WrNode includes the two per-child bounding boxes from the WAD NODES lump
 * (rt_top..rt_right and lt_top..lt_right), stored in WAD order: top(max y),
 * bottom(min y), left(min x), right(max x). These let wr_render_node reject
 * fully-offscreen or already-occluded subtrees without descending them --
 * the classic bounding-box reject, the single biggest BSP-walk optimisation on
 * a cacheless 486. */
typedef struct {
    i16 px, py, dx, dy;
    i16 rt_top, rt_bot, rt_left, rt_right;
    i16 lt_top, lt_bot, lt_left, lt_right;
    i32 right_child, left_child;
} WrNode; /* 32 bytes */
#pragma pack(pop)

/* High bit of a BSP node's child index marks "this is a subsector index",
 * not a further node - same convention as the raw WAD NODES lump (see
 * adoom's bsp.tgx, which reads the child as u16 without masking it). */
#define WR_NF_SUBSECTOR 0x8000

/* Per-slot texture/flat storage caps (see OP_WR_TEXTURE/OP_WR_FLAT in
 * tigryx.h). A typical game IWAD uses around 130 wall textures and 130-odd
 * flats; these leave generous room for PWADs without the per-slot
 * storage (a pointer + 2 ints, or a pointer) costing anything
 * meaningful. */
#define WR_MAX_TEXTURES 512
#define WR_MAX_FLATS    512

/* Visplane per-column extent arrays for the world renderer (see
 * wr_render.h). Declared here - included near the TOP of a backend .c
 * file, well before wr_render.h itself - so the backend's shutdown path
 * can release the allocation without needing the whole renderer visible
 * at that point in the file. */
static short *wr_plane_buf;
static int wr_plane_buf_w;

#endif /* RELIC_WR_TYPES_H */
