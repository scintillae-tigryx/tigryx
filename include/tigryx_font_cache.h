/*
 * tigryx_font_cache.h
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

/* =========================================================================
 * Shared DOS TrueType font cache format / helpers.
 *
 * The DOS backends (allegro-dos and allegro-tinygl-dos) rasterize TrueType
 * glyphs into SDF atlases on first use.  This is slow, especially for the
 * icon font (2304 glyphs).  The cache stores the pre-generated atlas and
 * glyph metadata so a DOS runtime can load fonts with a few fread() calls.
 *
 * The cache is generated once at build time by tools/font_cache_dos.c and
 * shipped alongside the .ttf files in the distribution.
 *
 * Usage:
 *   #define STB_TRUETYPE_IMPLEMENTATION
 *   #include "tigryx_font_cache.h"
 *
 * The cache is independent of screen colour depth and resolution; it only
 * depends on the font file contents and the bake parameters below.
 * ========================================================================= */

#ifndef TIGRYX_FONT_CACHE_H
#define TIGRYX_FONT_CACHE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "stb_truetype.h"

/* -------------------------------------------------------------------------
 * Bake constants (must match the DOS backends)
 * ---------------------------------------------------------------------- */
#define FONT_GLYPH_COUNT      96   /* ASCII 32-126 */
#define FONT_MAX_FACES        16

/* Text fonts: multi-size bitmap baking (no SDF).  Eight pre-rendered sizes
 * covering the GUI font sizes plus common sub-GUI text (12/13px editor and
 * code text) are stored in the cache; the backend picks the closest at
 * render time for minimal scaling artefacts.
 * File size is ~1.1MB per font (8 atlases) vs 4MB for SDF.
 *
 * 16.0f is the DOS VGA font size (PxPlus_IBM_VGA8, 8x16 cell). The DOS
 * theme's dos_fs() collapses every GUI_FONT_* constant to 16, so every
 * DOS text draw lands at exactly 16px — baking it exactly means the VGA
 * font is rendered 1:1 with no nearest-neighbour resample, preserving
 * its pixel-sharp edges. Without it, 16px text falls back to the 15px
 * bake scaled by 16/15, which blurs the 1bpp glyph outlines. */
#define FONT_MAX_BAKE_SIZES   8
#define FONT_NUM_TEXT_SIZES   8
static const float FONT_BAKE_PIXELS[FONT_MAX_BAKE_SIZES] = {
    12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 17.0f, 20.0f, 26.0f
};
static const int   FONT_BAKE_ATLAS[FONT_MAX_BAKE_SIZES]  = {
    256,   256,   256,   256,   256,   256,   512,   512
};
#define FONT_ATLAS_SIZE        512     /* legacy: used by non-DOS backends */

/* SDF parameters (used only for icon fonts) */
#define FONT_SDF_PADDING      8
#define FONT_SDF_ONEDGE_VALUE 180
#define FONT_SDF_SMOOTHING_PX 1.0f

#define FONT_ICON_ATLAS_SIZE   2048
#define FONT_ICON_GLYPH_COUNT  2304    /* U+E000 – U+E8FF */
#define FONT_ICON_FIRST_CHAR   0xE000
#define FONT_ICON_BAKE_PIXELS  32.0f

/* -------------------------------------------------------------------------
 * Cache file format
 * ---------------------------------------------------------------------- */
#define RELIC_FONT_CACHE_MAGIC   "RFNT"
#define RELIC_FONT_CACHE_VERSION 4u

typedef struct {
    char     magic[4];          /* "RFNT" */
    uint32_t version;           /* RELIC_FONT_CACHE_VERSION */
    uint32_t font_size;         /* size of source .ttf in bytes */
    uint32_t font_crc32;        /* CRC32 of source .ttf */
    int32_t  num_bake_sizes;    /* 1..FONT_MAX_BAKE_SIZES */
    float    bake_pixels[FONT_MAX_BAKE_SIZES]; /* bake heights */
    int32_t  atlas_sizes[FONT_MAX_BAKE_SIZES]; /* atlas dims, 0 if unused */
    int32_t  glyph_count;       /* number of baked glyphs */
    int32_t  first_char;        /* first codepoint */
    int32_t  sdf_padding;       /* SDF padding, 0 if not SDF */
    int32_t  sdf_onedge;        /* SDF on-edge value, 0 if not SDF */
    float    sdf_smoothing_px;  /* SDF smoothing width */
    float    ascent_px;         /* precomputed ascent */
    float    descent_px;        /* precomputed descent */
    char     name[64];          /* display name, extension stripped */
} RelicFontCacheHeader;

typedef struct {
    int x, y;
    int row_h;
} RelicSdfPacker;

/* -------------------------------------------------------------------------
 * Case-insensitive helpers
 * ---------------------------------------------------------------------- */
static inline int relic_font_ascii_tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static inline bool relic_font_ci_contains(const char *haystack, const char *needle) {
    if (!needle || !*needle) return true;
    if (!haystack) return false;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++)
            if (relic_font_ascii_tolower((unsigned char)haystack[i + j]) !=
                relic_font_ascii_tolower((unsigned char)needle[j]))
                break;
        if (j == nlen) return true;
    }
    return false;
}

static inline bool relic_font_is_icon(const char *basename) {
    return relic_font_ci_contains(basename, "icon") ||
           relic_font_ci_contains(basename, "materi");
}

static inline void relic_font_params_for(const char *basename,
                                          int *first_char,
                                          int *glyph_count,
                                          int *num_bake_sizes,
                                          float *out_bake_pixels,
                                          int *out_atlas_sizes,
                                          int *sdf_padding,
                                          int *sdf_onedge) {
    bool is_icon = relic_font_is_icon(basename);
    *first_char  = is_icon ? FONT_ICON_FIRST_CHAR  : 32;
    *glyph_count = is_icon ? FONT_ICON_GLYPH_COUNT : FONT_GLYPH_COUNT;
    if (is_icon) {
        *num_bake_sizes = 1;
        out_bake_pixels[0] = FONT_ICON_BAKE_PIXELS;
        out_atlas_sizes[0] = FONT_ICON_ATLAS_SIZE;
        *sdf_padding = FONT_SDF_PADDING;
        *sdf_onedge  = FONT_SDF_ONEDGE_VALUE;
    } else {
        *num_bake_sizes = FONT_NUM_TEXT_SIZES;
        for (int i = 0; i < FONT_NUM_TEXT_SIZES; i++) {
            out_bake_pixels[i] = FONT_BAKE_PIXELS[i];
            out_atlas_sizes[i] = FONT_BAKE_ATLAS[i];
        }
        *sdf_padding = 0;   /* bitmap mode (no SDF) */
        *sdf_onedge  = 0;
    }
}

/* -------------------------------------------------------------------------
 * Bake-size selection
 *
 * Picks which pre-baked atlas to render a given pixel size from.  `bake_px`
 * / `n` come from the FACE (i.e. from the loaded cache header), never from
 * the compile-time FONT_BAKE_PIXELS table, so a cache generated with a
 * different size list still selects correctly.
 *
 * For SDF faces (is_bitmap == 0) any scale factor resamples cleanly, so the
 * numerically closest bake wins.
 *
 * For BITMAP faces (is_bitmap != 0) the glyphs are pixel art and are drawn
 * with nearest-neighbour sampling: a non-integer scale drops or doubles
 * individual rows/columns, which shreds thin stems and 1px features.  So we
 * prefer, in order:
 *   1. an exact match (scale == 1, no resample at all);
 *   2. an integer upscale (scale == 2, 3, ... = clean pixel replication),
 *      choosing the largest such bake so the multiplier stays small;
 *   3. the numerically closest bake (unavoidable resample).
 * This is what makes 2x SSAA usable on DOS: a 16px request at ss=2 asks for
 * 32px and lands on the 16px bake at exactly 2.0.
 * ---------------------------------------------------------------------- */
static inline int relic_font_select_bake(const float *bake_px, int n,
                                         float want, int is_bitmap) {
    if (n <= 1 || !bake_px) return 0;

    if (is_bitmap) {
        /* 1. exact match */
        for (int i = 0; i < n; i++) {
            if (bake_px[i] > 0.0f && fabsf(want - bake_px[i]) < 0.01f)
                return i;
        }
        /* 2. integer upscale, largest qualifying bake (= smallest multiplier) */
        int best_int = -1;
        float best_int_px = 0.0f;
        for (int i = 0; i < n; i++) {
            if (bake_px[i] <= 0.0f) continue;
            float k = want / bake_px[i];
            if (k < 1.5f) continue;              /* 1x handled above */
            float kr = floorf(k + 0.5f);
            if (fabsf(k - kr) > 0.02f) continue;
            if (bake_px[i] > best_int_px) { best_int_px = bake_px[i]; best_int = i; }
        }
        if (best_int >= 0) return best_int;
    }

    /* 3. numerically closest */
    int best = 0;
    float best_diff = fabsf(want - bake_px[0]);
    for (int i = 1; i < n; i++) {
        if (bake_px[i] <= 0.0f) continue;
        float diff = fabsf(want - bake_px[i]);
        if (diff < best_diff) { best_diff = diff; best = i; }
    }
    return best;
}

/* -------------------------------------------------------------------------
 * Pixel-font special case (PxPlus_IBM_VGA8, the DOS theme's system font)
 *
 * PxPlus_IBM_VGA8 is a TrueType recreation of the IBM VGA 8x16 text-mode
 * font: strictly 1-bit pixel art on an 8x16 cell.  Its unitsPerEm is 1600
 * and ascent-descent is exactly 1600, so 1 pixel of the cell == 100 font
 * units.  Baked at exactly 16.0px (16/1600 = 0.01 px per unit) every glyph
 * edge lands on a whole pixel boundary and stb_truetype emits pure 0/255
 * coverage - the font is reproduced bit-for-bit.  At ANY other bake size
 * the outlines fall between pixel centres and ~55-80% of the inked pixels
 * come out as intermediate greys, which is what makes the font look blurry
 * and cause eye strain on DOS.
 *
 * Faces matching this are therefore special-cased in the DOS backends:
 *   - bake selection prefers the exact/integer-multiple size (above);
 *   - coverage is thresholded to 0 or 255, so the result stays 1-bit even
 *     if a non-ideal bake ever gets picked;
 *   - the destination origin is snapped to the SSAA resolve grid.
 * This is deliberately NOT applied to Inter/Roboto/JetBrains, which are
 * also bitmap-baked on DOS but are real anti-aliased typefaces.
 * ---------------------------------------------------------------------- */
static inline int relic_font_is_pixel_font(const char *name) {
    if (!name) return 0;
    return relic_font_ci_contains(name, "PxPlus") ? 1 : 0;
}

/* Round `v` to the nearest multiple of `grid`.
 *
 * Needed because ssaa_downsample_2x box-averages the supersampled buffer on
 * the EVEN grid (dx*2, dy*2).  A glyph doubled to exactly 2x but landing on
 * an ODD destination x or y straddles two resolve boxes, so every vertical
 * stem is averaged 50/50 with its neighbour and comes out as two grey
 * columns instead of one solid one - pixel-perfect doubling destroyed on
 * the way to the screen.  Snapping the origin to the grid keeps it intact.
 *
 * Letter spacing is unaffected: the VGA cell advance is 8px, i.e. 16
 * supersampled pixels, which is already even - so snapping the first glyph
 * keeps the whole run aligned with zero cumulative drift. */
static inline int relic_font_snap_grid(int v, int grid) {
    if (grid <= 1) return v;
    int r = v % grid;
    if (r < 0) r += grid;
    return (2 * r >= grid) ? (v + (grid - r)) : (v - r);
}

/* Integer pixel-replication factor for a bitmap glyph, or 0 if the scale is
 * not close enough to a whole number.  Callers snap the destination rect to
 * gw*k / gh*k so nearest-neighbour becomes exact replication. */
static inline int relic_font_bitmap_int_scale(float scale) {
    if (scale < 0.98f) return 0;
    float kr = floorf(scale + 0.5f);
    if (kr < 1.0f) return 0;
    if (fabsf(scale - kr) > 0.02f) return 0;
    return (int)kr;
}

/* Strip trailing ".ttf" or ".fnt" (case-insensitive) from a basename. */
static inline void relic_font_make_name(const char *basename, char *out, size_t out_sz) {
    size_t L = strlen(basename);
    if (L >= out_sz) L = out_sz - 1;
    memcpy(out, basename, L);
    out[L] = '\0';
    if (L >= 4) {
        char *dot = out + L - 4;
        if (relic_font_ascii_tolower((unsigned char)dot[0]) == '.' &&
            ((relic_font_ascii_tolower((unsigned char)dot[1]) == 't' &&
              relic_font_ascii_tolower((unsigned char)dot[2]) == 't' &&
              relic_font_ascii_tolower((unsigned char)dot[3]) == 'f') ||
             (relic_font_ascii_tolower((unsigned char)dot[1]) == 'f' &&
              relic_font_ascii_tolower((unsigned char)dot[2]) == 'n' &&
              relic_font_ascii_tolower((unsigned char)dot[3]) == 't') ||
             (relic_font_ascii_tolower((unsigned char)dot[1]) == 'o' &&
              relic_font_ascii_tolower((unsigned char)dot[2]) == 't' &&
              relic_font_ascii_tolower((unsigned char)dot[3]) == 'f'))) {
            *dot = '\0';
        }
    }
}

/* -------------------------------------------------------------------------
 * 8.3 cache filenames (DOS/FAT16)
 *
 * On plain DOS the shipped .fnt caches must have 8.3-safe names.  The table
 * below gives the canonical short stem for each shipped face; unknown fonts
 * fall back to the first 8 filename-safe uppercased characters of the stem.
 * The cache header still stores the full display name, so face selection is
 * unaffected - only the on-disk filename is short.
 * ---------------------------------------------------------------------- */
#define RELIC_FONT83_MAP(X) \
    X("Inter-Regular",                 "INTER-R")  \
    X("Inter-Bold",                    "INTER-B")  \
    X("JetBrainsMono-Regular",         "JBMONO-R") \
    X("JetBrainsMono-ExtraBold",       "JBMONO-X") \
    X("MaterialIconsOutlined-Regular", "MATICONS") \
    X("PxPlus_IBM_VGA8",               "VGA8X16")  \
    X("Roboto-Regular",                "ROBOTO-R") \
    X("Roboto-Bold",                   "ROBOTO-B")
#define RELIC_FONT83_ROW(long_name, short_name) { long_name, short_name },

static inline int relic_font83_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (relic_font_ascii_tolower((unsigned char)*a) !=
            relic_font_ascii_tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Map a font file basename (with or without extension) to its 8.3 cache
 * stem.  Known faces use the canonical table; unknown fonts get the first
 * 8 filename-safe uppercased characters. */
static inline void relic_font_cache_stem83(const char *basename,
                                           char *out, size_t out_sz) {
    char stem[64];
    relic_font_make_name(basename, stem, sizeof(stem));  /* strip .ttf/.fnt/.otf */

    static const char *const map[][2] = {
        RELIC_FONT83_MAP(RELIC_FONT83_ROW)
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(stem, map[i][0]) == 0) {
            strncpy(out, map[i][1], out_sz - 1);
            out[out_sz - 1] = '\0';
            return;
        }
    }

    size_t n = 0;
    for (size_t i = 0; stem[i] && n < 8 && n + 1 < out_sz; i++) {
        int c = relic_font_ascii_tolower((unsigned char)stem[i]);
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_')
            out[n++] = (char)c;
    }
    out[n] = '\0';
}

/* Reverse of relic_font_cache_stem83: map an 8.3 cache stem back to the
 * shipped font's full stem (for .ttf fallback when the cache fails).
 * Unknown stems pass through unchanged. */
static inline void relic_font_cache_long_stem(const char *basename,
                                              char *out, size_t out_sz) {
    char stem[64];
    relic_font_make_name(basename, stem, sizeof(stem));

    static const char *const map[][2] = {
        RELIC_FONT83_MAP(RELIC_FONT83_ROW)
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (relic_font83_eq_ci(stem, map[i][1])) {
            strncpy(out, map[i][0], out_sz - 1);
            out[out_sz - 1] = '\0';
            return;
        }
    }
    strncpy(out, stem, out_sz - 1);
    out[out_sz - 1] = '\0';
}

/* Build "<dir of path>/<8.3 stem>.fnt" for a font path (any extension). */
static inline void relic_font_cache_path83(const char *path,
                                           char *out, size_t out_sz) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    char stem83[16];
    relic_font_cache_stem83(base, stem83, sizeof(stem83));
    size_t dirlen = (size_t)(base - path);   /* includes the separator */
    if (dirlen)
        snprintf(out, out_sz, "%.*s%s.fnt", (int)dirlen, path, stem83);
    else
        snprintf(out, out_sz, "%s.fnt", stem83);
}

/* Build "<dir of path>/<full stem>.ttf" - the sibling TrueType source for a
 * font path.  For an 8.3 cache name (JBMONO-R.fnt) this yields the full name
 * (JetBrainsMono-Regular.ttf); for other paths it is the same "<stem>.ttf"
 * an extension swap produces. */
static inline void relic_font_ttf_sibling_path(const char *path,
                                               char *out, size_t out_sz) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    char stem[64];
    relic_font_cache_long_stem(base, stem, sizeof(stem));
    size_t dirlen = (size_t)(base - path);
    if (dirlen)
        snprintf(out, out_sz, "%.*s%s.ttf", (int)dirlen, path, stem);
    else
        snprintf(out, out_sz, "%s.ttf", stem);
}

/* -------------------------------------------------------------------------
 * SDF atlas shelf packer
 * ---------------------------------------------------------------------- */
static inline void relic_sdf_packer_init(RelicSdfPacker *p) {
    p->x = 0;
    p->y = 0;
    p->row_h = 0;
}

static inline int relic_sdf_packer_alloc(RelicSdfPacker *p, int w, int h,
                                          int atlas_size, int *out_x, int *out_y) {
    if (w > atlas_size || h > atlas_size) return 0;
    if (p->x + w > atlas_size) {
        p->y += p->row_h;
        p->x = 0;
        p->row_h = 0;
    }
    if (p->y + h > atlas_size) return 0;
    *out_x = p->x;
    *out_y = p->y;
    p->x += w;
    if (h > p->row_h) p->row_h = h;
    return 1;
}

/* -------------------------------------------------------------------------
 * CRC32 (small, no table)
 * ---------------------------------------------------------------------- */
static inline uint32_t relic_font_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

/* -------------------------------------------------------------------------
 * Cache I/O helpers
 *
 * The file layout is:
 *   RelicFontCacheHeader
 *   atlas_size * atlas_size bytes of 8-bit SDF / alpha
 *   glyph_count * sizeof(stbtt_bakedchar) bytes of glyph metadata
 * ---------------------------------------------------------------------- */
static inline int relic_font_cache_write_header(FILE *f, const RelicFontCacheHeader *h) {
    if (fwrite(h, sizeof(RelicFontCacheHeader), 1, f) != 1) return 0;
    return 1;
}

static inline int relic_font_cache_read_header(FILE *f, RelicFontCacheHeader *h) {
    if (fread(h, sizeof(RelicFontCacheHeader), 1, f) != 1) return 0;
    return 1;
}

static inline int relic_font_cache_write_atlas(FILE *f, const unsigned char *atlas, int atlas_size) {
    size_t n = (size_t)atlas_size * (size_t)atlas_size;
    if (fwrite(atlas, 1, n, f) != n) return 0;
    return 1;
}

static inline int relic_font_cache_read_atlas(FILE *f, unsigned char *atlas, int atlas_size) {
    size_t n = (size_t)atlas_size * (size_t)atlas_size;
    if (fread(atlas, 1, n, f) != n) return 0;
    return 1;
}

static inline int relic_font_cache_write_cdata(FILE *f, const stbtt_bakedchar *cdata, int glyph_count) {
    if (fwrite(cdata, sizeof(stbtt_bakedchar), (size_t)glyph_count, f) != (size_t)glyph_count) return 0;
    return 1;
}

static inline int relic_font_cache_read_cdata(FILE *f, stbtt_bakedchar *cdata, int glyph_count) {
    if (fread(cdata, sizeof(stbtt_bakedchar), (size_t)glyph_count, f) != (size_t)glyph_count) return 0;
    return 1;
}

/* -------------------------------------------------------------------------
 * Bitmap atlas generation (plain alpha, no SDF)
 * ---------------------------------------------------------------------- */
static inline int relic_font_bake_bitmap(const stbtt_fontinfo *info,
                                          int first_char, int glyph_count,
                                          float bake_pixels, int atlas_size,
                                          unsigned char *atlas,
                                          stbtt_bakedchar *cdata,
                                          float *out_ascent_px,
                                          float *out_descent_px) {
    memset(atlas, 0, (size_t)atlas_size * (size_t)atlas_size);
    int ascent_u = 0, descent_u = 0, line_gap_u = 0;
    stbtt_GetFontVMetrics(info, &ascent_u, &descent_u, &line_gap_u);
    float em_scale = stbtt_ScaleForMappingEmToPixels(info, bake_pixels);
    float px_scale = stbtt_ScaleForPixelHeight(info, bake_pixels);
    float bake_h = bake_pixels;
    if (px_scale > 1e-6f)
        bake_h = bake_pixels * (em_scale / px_scale);
    stbtt_BakeFontBitmap(info->data, info->fontstart, bake_h,
                         atlas, atlas_size, atlas_size,
                         first_char, glyph_count, cdata);
    *out_ascent_px  = (float)ascent_u  * em_scale;
    *out_descent_px = (float)descent_u * em_scale;
    return 1;
}

/* -------------------------------------------------------------------------
 * SDF atlas generation (same algorithm as the DOS backends)
 *
 * Returns 0 on total failure, 1 if every glyph was packed, and 2 if some
 * glyphs could not be packed because the atlas ran out of room (those
 * glyphs keep their advance width but have an empty rect, so they render
 * as blanks). Callers that can grow the atlas should treat 2 as a retry
 * signal; callers with a fixed atlas size can treat any non-zero value as
 * success and accept the missing glyphs.
 * ---------------------------------------------------------------------- */
static inline int relic_font_bake_sdf(const stbtt_fontinfo *info,
                                      int first_char, int glyph_count,
                                      float bake_pixels, int atlas_size,
                                      int sdf_padding, int sdf_onedge,
                                      unsigned char *atlas,
                                      stbtt_bakedchar *cdata,
                                      float *out_ascent_px,
                                      float *out_descent_px) {
    memset(atlas, 0, (size_t)atlas_size * (size_t)atlas_size);

    RelicSdfPacker packer;
    relic_sdf_packer_init(&packer);
    float sdf_scale = stbtt_ScaleForMappingEmToPixels(info, bake_pixels);
    float pixel_dist_scale = (float)sdf_onedge / (float)sdf_padding;
    int loaded = 0;
    int packfail = 0;   /* glyphs dropped because the atlas was full */

    for (int cp = first_char; cp < first_char + glyph_count; cp++) {
        int gi = cp - first_char;
        if (stbtt_FindGlyphIndex(info, cp) == 0) {
            cdata[gi].x0 = 0; cdata[gi].y0 = 0;
            cdata[gi].x1 = 0; cdata[gi].y1 = 0;
            cdata[gi].xoff = 0.0f; cdata[gi].yoff = 0.0f;
            cdata[gi].xadvance = 0.0f;
            loaded++;
            continue;
        }

        int advance, lsb;
        stbtt_GetCodepointHMetrics(info, cp, &advance, &lsb);
        float xadvance = advance * sdf_scale;

        int w, h, xoff, yoff;
        unsigned char *sdf = stbtt_GetCodepointSDF(info, sdf_scale, cp,
                                                   sdf_padding, sdf_onedge,
                                                   pixel_dist_scale, &w, &h, &xoff, &yoff);
        if (!sdf) {
            cdata[gi].x0 = 0; cdata[gi].y0 = 0;
            cdata[gi].x1 = 0; cdata[gi].y1 = 0;
            cdata[gi].xoff = 0.0f; cdata[gi].yoff = 0.0f;
            cdata[gi].xadvance = xadvance;
            loaded++;
            continue;
        }

        int ax, ay;
        if (!relic_sdf_packer_alloc(&packer, w, h, atlas_size, &ax, &ay)) {
            stbtt_FreeSDF(sdf, NULL);
            cdata[gi].x0 = 0; cdata[gi].y0 = 0;
            cdata[gi].x1 = 0; cdata[gi].y1 = 0;
            cdata[gi].xoff = 0.0f; cdata[gi].yoff = 0.0f;
            cdata[gi].xadvance = xadvance;
            loaded++;
            packfail++;
            continue;
        }
        for (int y = 0; y < h; y++) {
            memcpy(atlas + (ay + y) * atlas_size + ax, sdf + y * w, w);
        }

        cdata[gi].x0 = (unsigned short)ax;
        cdata[gi].y0 = (unsigned short)ay;
        cdata[gi].x1 = (unsigned short)(ax + w);
        cdata[gi].y1 = (unsigned short)(ay + h);
        cdata[gi].xoff = (float)xoff;
        cdata[gi].yoff = (float)yoff;
        cdata[gi].xadvance = xadvance;

        stbtt_FreeSDF(sdf, NULL);
        loaded++;
    }
    if (loaded == 0) return 0;

    int ascent_u = 0, descent_u = 0, line_gap_u = 0;
    stbtt_GetFontVMetrics(info, &ascent_u, &descent_u, &line_gap_u);
    *out_ascent_px  = (float)ascent_u  * sdf_scale;
    *out_descent_px = (float)descent_u * sdf_scale;
    return packfail ? 2 : 1;
}

#endif /* TIGRYX_FONT_CACHE_H */
