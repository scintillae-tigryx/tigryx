/*
 * backend_allegro_dos.c
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
 * backend_allegro_dos.c - DOS Allegro backend.
 *
 * Implements the full Relic backend surface on DJGPP + Allegro 4:
 *   - 2D: window/mode select, clear, line, polygon, polyline, flip, text.
 *   - 3D: custom software rasteriser (no Allegro 3D / TinyGL dependency).
 *         Per-pixel 16-bit z-buffer, perspective-correct texturing with
 *         16-pixel affine sub-spans, Lambert directional lighting.
 *   - Input/system/sound/file I/O.
 *
 * 3D pipeline summary
 * -------------------
 *  gfx_camera  -- builds proj + view matrices, resets per-frame sw_zbuf.
 *  gfx_light   -- stores normalised directional light + ambient for mesh use.
 *  gfx_texture -- stores raw RGBA32 pixels; no POT padding required.
 *  gfx_mesh    -- MVP transform, Lambert per-vertex shading, near-plane clip,
 *                 back-face cull, then sw_draw_tri() for each visible triangle.
 *
 *  sw_draw_tri -- top-left fill convention scanline rasteriser:
 *    * 16.16 fixed-point edge stepping.
 *    * Per-vertex 1/w interpolated; every 16 pixels the true (u/w, v/w, 1/w)
 *      is recomputed and the sub-span is drawn affinely -- classic BSP-engine
 *      sub-span trick, imperceptible at DOS resolutions.
 *    * 16-bit z-buffer (1/w * 65535, larger = closer).  Pixel written iff
 *      new_z > zbuf[pixel].
 *    * Untextured path: Gouraud RGB written as makecol_depth().
 */
#include "tigryx.h"
#include "tigryx_tier.h"

#define ALLEGRO_USE_C
#define HAVE_STDINT_H 1

#ifdef TGX_R2D
/* relic2d-dos: compile this backend against the in-house Relic2D shim
 * (src/backends/common/relic2d) instead of Allegro 4.4. */
#include "../common/relic2d/relic2d.h"
#else
#include <allegro.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <dpmi.h>
#include <pc.h>
#include <go32.h>
#include <dos.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef HAVE_DOSNET
#include "backend_dos_http.h"
#ifdef TIGRYX_EXPERIMENTAL_SERVER
#include "backend_dos_server.h"
#endif
#endif

/* stb_truetype + shared DOS font cache format / helpers.
 *
 * RELIC_NO_TTF_BAKE (set for RELIC_SOFTFP builds, see CMakeLists.txt) omits
 * STB_TRUETYPE_IMPLEMENTATION, so stb_truetype.h contributes only its type
 * declarations (stbtt_fontinfo, stbtt_bakedchar, ...) and none of the
 * rasteriser's function bodies -- those bodies are where the double-
 * precision geometry math this platform can't provide comes from. The .fnt
 * cache read/write path in this header needs only those declarations, not
 * the implementation, so it is unaffected either way. See the call sites
 * below (load_font_face_data) for what live baking becomes instead. */
#ifndef RELIC_NO_TTF_BAKE
#define STB_TRUETYPE_IMPLEMENTATION
#endif
#include "tigryx_font_cache.h"

/* -------------------------------------------------------------------------
 * 3D constants
 * ---------------------------------------------------------------------- */
/* Texture slot table sizing. The table grows on demand (see
 * ALLEGRO_TEX_RESERVE) instead of being a fixed array, so a small
 * program pays for 256 slots while a WAD-scale one can hold
 * thousands. A fixed 256 used to be the hard limit, and because
 * @texture has no way to report "table full" beyond returning 0 -
 * which callers treat as "use the fallback texture" - overflowing it
 * showed up as a scene that silently rendered untextured rather than
 * as any kind of error.
 *
 * RELIC_MAX_TEXTURES is now the ceiling rather than the allocation:
 * @texture yields a u16 slot id, so 65535 is the highest addressable. */
#define RELIC_TEX_CAP_INIT 256
#define RELIC_MAX_TEXTURES 65535

/* Sector-based world renderer (@wr_*): shared portable engine, used
 * identically by every backend that supports it. See wr_render.h for the
 * full rationale; this pulls in the on-disk WAD map structs (WrVertex
 * etc.), WR_MAX_TEXTURES/WR_MAX_FLATS, and the wr_plane_buf storage that
 * AllegroState's fields below and allegro_shutdown() need. The renderer
 * functions themselves (wr_op_render and friends) are pulled in later,
 * via wr_render.h, once AllegroState is fully defined - see the matching
 * #include further down this file. */
#include "../common/wr/wr_types.h"
#include "../common/local_clip.h"
#include "../common/drawlist/drawlist.h"
#include "../common/lines3d.h"
#include "../common/sdfill.h"
#include "../common/sdtrue.h"
#include "../common/l3fill.h"

/* @window flag bits (matches typecheck doc-comment). */
#define RELIC_WINFLAG_STRETCH 0x00000001u
#define RELIC_WINFLAG_NO_VSYNC 0x00000004u  /* set by --fps; used to suppress
                                             * periodic profiler flushes so
                                             * disk I/O stays out of the
                                             * benchmark (see wr_render.h) */
#define RELIC_WINFLAG_AA     0x00000008u
#define RELIC_WINFLAG_DIRECT 0x00000100u  /* draw to screen, skip backbuffer blit */
#define RELIC_WINFLAG_FLIP   0x00000200u  /* VRAM backbuffer + show_video_bitmap */
#define RELIC_WINFLAG_8BPP   0x00000400u  /* force 8-bit paletted mode; exit if unavailable */

/* 8bpp RGB->palette-index lookup table geometry. SW_PAL_BITS is the bits per
 * channel used to index the table, so SW_PAL_SHIFT is the right-shift applied
 * to each 0..255 channel. See sw_build_lookup_table() for why 5 bits suffices. */
#define SW_PAL_BITS        5
#define SW_PAL_SHIFT       (8 - SW_PAL_BITS)
#define SW_PAL_LUT_SIZE    (1 << SW_PAL_BITS)
#define SW_PAL_LUT_ENTRIES (SW_PAL_LUT_SIZE * SW_PAL_LUT_SIZE * SW_PAL_LUT_SIZE)

/* 8bpp texture-colour sampling: collect up to this many RGB samples from
 * uploaded textures. At 3 bytes each this is 72 KB — fine on a 4 MB system.
 * Sampling every 4th pixel (stride 4 in both x and y = 1/16 of texels) from
 * ~35 WAD textures of 64x64 yields ~9K samples, well under this cap. */
#define SW_MAX_SAMPLES 24576
/* Number of palette entries reserved for texture colours. The remaining
 * 20 (256-236) are kept for UI colours in sw_setup_8bit_palette(). */
#define SW_PAL_TEXTURE_ENTRIES 236

typedef union {
    i32 i;
    float f;
} AllegroFBits;

/* Raw RGBA32 texture for the SW rasteriser (depth-independent). */
typedef struct {
    u8 *pixels;   /* RGBA32, w*h*4 bytes */
    int w, h;     /* POT dimensions */
    int src_w, src_h;
    int w_mask, h_mask;
    int has_alpha;/* 1 if any texel alpha < 255 (needs per-pixel alpha-test) */
    BITMAP *bmp;  /* Cached Allegro BITMAP (screen depth) for fast blit path */
} SwTexture;

/* Median-cut colour quantisation box (see sw_build_optimised_palette). */
typedef struct {
    int start;  /* index into sw_col_samples where this box's colours start */
    int count;  /* number of colours in this box */
    u8 rmin, rmax, gmin, gmax, bmin, bmax;  /* bounding box in RGB */
} McBox;

#define RELIC_MAX_PROC 8      /* max concurrent spawned subprocess handles */

/* Subprocess slot for @process_spawn/poll/read/kill/close.
 * DOS is single-tasking: spawn runs the child to completion (swap-out-and-
 * exec, the classic DOS pattern), so a slot is always "exited" by the time
 * spawn returns. Child stdout/stderr are captured into temp files which the
 * read ops stream back to the guest. */
typedef struct {
    int  in_use;
    int  exit_code;
    char out_path[64];      /* captured stdout temp file */
    char err_path[64];      /* captured stderr temp file */
    FILE *out_fp;           /* lazily opened by first read, closed on close */
    FILE *err_fp;
} DosProcSlot;

/* -------------------------------------------------------------------------
 * Backend state
 * ---------------------------------------------------------------------- */
typedef struct {
    int width, height;
    int virt_w, virt_h;
    int offset_x, offset_y;
    int color_depth;
    int last_key;
    bool keys[KEY_MAX];
    int mouse_x, mouse_y, mouse_buttons, mouse_wheel;
    /* Software mouse cursor compositing. Allegro's timer-ISR cursor is
     * disabled entirely (allegro_gfx_window calls show_mouse(NULL) in every
     * mode): it drew straight into the physical `screen` bitmap, and the next
     * frame's backbuffer blit overwrote it, so the pointer only flickered
     * into view on mouse-move ticks. Instead gfx_flip draws mouse_sprite onto
     * the presented surface once per frame. cur_x/cur_y remember where the
     * cursor was last composited so a mouse move can mark an otherwise idle
     * frame dirty and keep the pointer tracking. */
    int have_mouse;
    int cur_x, cur_y;
    volatile bool quit;

    /* 3D pipeline */
    float proj[16];
    float view[16];
    float mvp[16];
    float znear, zfar;
    bool have_camera;
    bool cam_mirror;   /* negative FOV: horizontal mirror for BSP-style games */
    float light_dir[3];
    float light_rgb[3];
    float light_amb[3];
    bool light_on;

    /* Projection matrix cache: FOV/aspect/znear/zfar rarely change between
     * frames, so we skip the tanf() + division on every @camera call.
     * proj_valid is false until first set, and cleared when any parameter
     * changes.  On a 486SX the tanf alone costs ~50ms via -lemu. */
    bool proj_valid;
    float proj_fov_deg;
    float proj_aspect;
    float proj_znear;
    float proj_zfar;
    bool proj_mirror;


    /* frame_dirty: set by every drawing primitive, cleared at the end of
     * flip.  When false, gfx_flip skips the blit to avoid burning CPU
     * on idle wait-loops. */
    bool frame_dirty;

    /* True once @window has successfully entered a graphics mode. Console-only
     * programs never set this, so shutdown must not force a text-mode reset
     * (which would clear the screen and destroy the console output). */
    bool gfx_mode_set;

    /* 3D viewport (scissor test) state */
    int vp_3d_x;
    int vp_3d_y;
    int vp_3d_w;
    int vp_3d_h;

    /* 2D clip rectangle for @clip (disabled when w<=0 or h<=0). */
    bool clip_enabled;
    int clip_x, clip_y, clip_w, clip_h;

    /* RELIC_WINFLAG_STRETCH: script renders into a fixed logical_w x
     * logical_h backbuffer; at flip time stretch_blit scales it to
     * fill the entire physical screen (non-uniform stretch).
     * Without stretch the backbuffer matches the physical VESA mode. */
    u32 win_flags;
    bool stretch_enabled;
    bool aa_enabled;
    /* Global supersample anti-aliasing (RELIC_WINFLAG_AA). When active the
     * backbuffer is allocated at ss x the logical size, every 2D coordinate
     * and size is scaled by ss, and gfx_flip box-filter downsamples the
     * oversized backbuffer into `resolve` before presenting. ss==1 disables
     * (default / when the flag is absent or the colour depth is unsupported). */
    int ss;
    BITMAP *resolve;   /* logical-size downsample target (only when ss>1) */
    int logical_w;
    int logical_h;
    int vp_x, vp_y; /* stretch destination in physical pixels */

    /* Grown on demand by ALLEGRO_TEX_RESERVE; tex_cap = allocated
     * slots. Slot 0 is reserved as "no texture". */
    SwTexture *textures;
    int tex_cap;

    /* 16-bit SW z-buffer: value = clamp(1/w * 65535, 0, 65535). */
    u16 *sw_zbuf;
    int sw_zbuf_w, sw_zbuf_h;

    BITMAP *backbuf;

    /* Direct-draw mode: when true, all 2D primitives draw straight to the
     * physical `screen` bitmap and gfx_flip skips the blit. This eliminates
     * the RAM->VRAM copy (~52 ms/frame on a 486SX at 320x200x8bpp) at the
     * cost of visible tearing / no double-buffering. Enabled by passing
     * RELIC_WINFLAG_DIRECT (bit 8) in the @window flags argument. */
    bool direct_draw;

    /* Page-flip mode (RELIC_WINFLAG_FLIP): two full-screen pages allocated in
     * video memory from the virtual screen reserved at mode-set time. The
     * script always renders into the hidden one (backbuf == vpage[vpage_active])
     * and gfx_flip makes it visible with show_video_bitmap(), then switches
     * backbuf to the other page. Zero pixel copy at flip time, tear-free via
     * the vblank wait inside show_video_bitmap. If either page cannot be
     * allocated this stays false and the normal RAM-backbuffer + blit path is
     * used instead. */
    bool page_flip;
    BITMAP *vpage[2];
    int vpage_active;   /* index of the page currently being drawn into */

    /* True when the flip backbuffer is a planar (mode Y / VESA 101h) bitmap.
     * When set, sw_draw_tri renders into a per-scanline system-RAM staging
     * buffer and flushes to VRAM in 4 plane-grouped passes (one Map Mask
     * register write per plane per scanline) instead of doing a per-pixel
     * plane select.  This is the classic trick: 4 port writes per scanline vs.
     * one per texel.  Linear (non-planar) video bitmaps set this false and
     * the rasteriser writes directly to VRAM as usual. */
    bool planar_backbuf;

    /* Per-scanline staging buffer and dirty mask for the planar rasteriser
     * path.  planar_stage holds one row of palette indices (render_w bytes);
     * planar_dirty is a bit-per-pixel mask (render_w/8 bytes) recording which
     * pixels passed the z-test and must be flushed.  NULL when not planar. */
    unsigned char *planar_stage;
    unsigned char *planar_dirty;

    /* 64x64x64 RGB->palette lookup for fast 8bpp rendering (256KB, NULL if not 8bpp) */
    unsigned char *rgb_to_idx;

    /* ---- 8bpp optimised palette from texture colours ----
     *
     * Instead of a fixed 3-3-2 palette (which makes game textures' browns/greens look
     * grey-blue), we sample colours from uploaded textures at @texture() time,
     * then at the first @camera() call run median-cut to build a 236-colour
     * palette tailored to the actual texture colours. The remaining 20 entries
     * are reserved for UI colours (see sw_setup_8bit_palette).
     *
     * Memory budget (designed for 4 MB systems):
     *   sw_col_samples:  up to SW_MAX_SAMPLES * 3 bytes (e.g. 24576 * 3 = 72 KB)
     *   sw_pal_built:    flag, no memory
     *   The median-cut runs in-place on the sample array, no extra alloc.
     *   The lookup table build reuses the existing rgb_to_idx (32^3 = 32 KB).
     *
     * Timing on a 486SX-25:
     *   Sampling: ~5 ms (every 4th pixel of ~9K total samples)
     *   Median-cut: ~50 ms (in-place on 9K colours)
     *   Lookup table build: ~200 ms (tree-guided nearest search)
     *   Total: ~255 ms — imperceptible, less than one frame at 30 fps.
     */
    unsigned char *sw_col_samples;  /* packed RGB triples, NULL if not 8bpp */
    int sw_col_count;               /* number of samples collected so far */
    bool sw_pal_built;              /* true once optimised palette is set */

    /* ---- 8bpp palette mode override (@palette_upload) ----
     *
     * Default is SW_PAL_AUTO, the texture-sampled median-cut palette above.
     * A script may call @palette_upload() before the first @camera() to pick
     * a different strategy:
     *   - count == 0            -> SW_PAL_FLAT: keep the flat 3-3-2 palette
     *     (sw_setup_8bit_palette) as final instead of building a median-cut
     *     one; useful for general-purpose apps that don't want a palette
     *     that shifts based on load order / texture content.
     *   - count > 0, ptr != 0   -> SW_PAL_CUSTOM: use the supplied RGB888
     *     table verbatim (e.g. the game's PLAYPAL, already parsed by the script
     *     from the WAD) with no UI-colour reservation, since callers that
     *     supply their own full palette are expected to own all 256 slots.
     * Both alternate modes finalise the palette immediately (or skip the
     * sampling step altogether) rather than waiting for @camera(). */
    enum { SW_PAL_AUTO, SW_PAL_FLAT, SW_PAL_CUSTOM } sw_pal_mode;
    unsigned char sw_pal_custom[256 * 3];  /* RGB888, only used in SW_PAL_CUSTOM */
    int sw_pal_custom_count;

    /* ---- TrueType font state ---- */
    bool font_init;
    int nfaces;
    int default_face;
    struct {
        unsigned char *buffer;
        stbtt_fontinfo info;
        unsigned char *atlas[FONT_MAX_BAKE_SIZES];
        stbtt_bakedchar *cdata[FONT_MAX_BAKE_SIZES];
        int first_char;
        int glyph_count;
        int atlas_size[FONT_MAX_BAKE_SIZES];
        float bake_pixels[FONT_MAX_BAKE_SIZES];
        int num_bake_sizes;
        int sdf_padding;
        float ascent_px;
        float descent_px;
        char name[64];
        char path[256];   /* source .fnt/.ttf path, for lazy loading */
        const unsigned char *mem_data;  /* packed asset font data (no fopen needed) */
        u32 mem_size;
        bool init;        /* true once atlas/cdata have been loaded */
        bool is_bold;
        /* True for PxPlus_IBM_VGA8, the DOS theme's 1-bit VGA pixel font.
         * Enables the crisp-rendering special case: alpha thresholding and
         * SSAA grid snapping (see relic_font_is_pixel_font). */
        bool is_pixel_font;
    } faces[FONT_MAX_FACES];

#ifdef HAVE_DOSNET
    DosHttpReq http[DOS_HTTP_MAX];
#endif

    /* ---- Sector-based world renderer (@wr_*) ----
     * Same field layout as allegro-fixed-dos; the shared wr_render.h
     * engine accesses these via the WrHostState typedef. 8bpp-only:
     * wr_host_apply_palette exits early when color_depth != 8, and
     * @wr_render/@wr_test/@wr_patch check WR_HAS_BUF before drawing. */
    bool wr_palette_set;
    unsigned char wr_playpal[768];
    unsigned char *wr_colormap;
    int wr_num_colormaps;
    struct {
        unsigned char *data;
        int w, h;
    } wr_textures[WR_MAX_TEXTURES];
    unsigned char *wr_flats[WR_MAX_FLATS];
    bool wr_map_set;
    WrVertex    *wr_vertices;   int wr_n_vertices;
    WrLinedef   *wr_linedefs;   int wr_n_linedefs;
    WrSidedef   *wr_sidedefs;   int wr_n_sidedefs;
    WrSector    *wr_sectors;    int wr_n_sectors;
    WrSeg       *wr_segs;       int wr_n_segs;
    WrSubsector *wr_subsectors; int wr_n_subsectors;
    WrNode      *wr_nodes;      int wr_n_nodes;
    i32 *wr_sd_tex;
    i32 *wr_sec_tex;
    bool wr_texmap_set;
    i32 wr_sky_tex_id;
    bool wr_flatshade;
    unsigned char wr_tex_avg_color[WR_MAX_TEXTURES];
    bool wr_tex_avg_valid[WR_MAX_TEXTURES];
    unsigned char wr_flat_avg_color[WR_MAX_FLATS];
    bool wr_flat_avg_valid[WR_MAX_FLATS];
    int wr_area_x, wr_area_y, wr_area_w, wr_area_h;

    DosProcSlot proc[RELIC_MAX_PROC];
} AllegroState;

static AllegroState *g_state = NULL;
static RelicVM *g_vm = NULL;

/* Pick the pre-baked font size to render `pixel_size` from.  Bitmap faces
 * prefer an exact or integer-multiple bake (see relic_font_select_bake) so
 * nearest-neighbour sampling never drops or doubles glyph rows. */
static int select_bake_size(const AllegroState *s, int idx, float pixel_size) {
    return relic_font_select_bake(s->faces[idx].bake_pixels,
                                  s->faces[idx].num_bake_sizes,
                                  pixel_size,
                                  s->faces[idx].sdf_padding == 0);
}

/* All 2D/3D drawing primitives target the offscreen backbuffer when
 * available; gfx_flip copies it to the live `screen`. Falls back to the
 * live screen pre-window-call (so error paths still work). */
#define DRAW_TGT(s) (((s) && (s)->backbuf) ? (s)->backbuf : screen)

/* Persistent log file for @console output (forward declaration; defined
 * lazily by allegro_console_open_log below). */
static FILE *g_console_log = NULL;

/* Forward declaration for the one-shot PCM sample tracked by
 * allegro_snd_play; defined near snd_play below. Declared here so
 * allegro_shutdown can release it without an inline `extern` that
 * conflicts with the later `static` definition. */
static SAMPLE *g_snd_prev = NULL;

static float g_master_vol = 1.0f;
static float g_sfx_vol = 1.0f;
static float g_muted_master = -1.0f;

/* Currently playing music sample (MUS pre-rendered into a looping
 * SAMPLE buffer). */
static SAMPLE *g_mus_sample = NULL;

/* Real-time OPL2 FM music stream. The MUS/GENMIDI synth fills small PCM
 * chunks on demand (pumped from gfx_flip) instead of pre-rendering the
 * whole track up front, so playback starts instantly like a classic DOS FPS. */
static AUDIOSTREAM *g_mus_stream = NULL;

/* Defined alongside the FM synth below; called from gfx_flip each frame to
 * keep the music stream fed. */
static void mus_stream_poll(void);

/* Currently playing Allegro MIDI object (native OPL3/DIGMID playback). */
static MIDI *g_midi_playing = NULL;

/* High-resolution millisecond counter. Allegro takes ownership of the PIT
 * in install_timer(), which prevents DJGPP's uclock() (it reprograms the
 * same channel) from working -- @millis() then quantises to the ~36.4 Hz
 * coarse interrupt tick. Instead we install our own Allegro timer interrupt
 * at 1 kHz and count milliseconds directly. The variable and handler must be
 * locked into memory so they remain accessible inside the ISR (which can fire
 * during paging / disk access). */
static volatile unsigned int g_millis_counter = 0;

static void relic_millis_tick(void) {
    g_millis_counter++;
}

END_OF_FUNCTION(relic_millis_tick)

/* Shutdown progress trace. Exit hangs on real DOS leave nothing on screen
 * (the video mode is still graphics, or the machine is wedged), so the only
 * way to see how far teardown got is a file that is flushed and closed after
 * every step. Writes straight to TIGRYX.LOG in append mode rather than reusing
 * g_console_log, because the log has to survive past the point where that
 * handle is closed. Enabled by --verbose only, so normal runs create nothing.
 * If the machine hangs, reboot and read the last line of TIGRYX.LOG: the step
 * named there is the one that did not return. */
static int g_shutdown_trace_on = 0;   /* latched from vm->verbose on entry,
                                       * so tracing outlives g_vm = NULL */

/* ---- 3D rendering profiler (--gfx-prof) ----
 * File-scope accumulators, dumped at shutdown.  Zero overhead when
 * g_relic_gfx_prof is 0: the hot paths check the flag once per mesh
 * (allegro_gfx_mesh) or once per scanline (sw_draw_tri), not per pixel. */
static struct {
    unsigned int mesh_calls;
    unsigned int verts_xformed;
    unsigned int tris_considered;   /* entered the max_tris loop */
    unsigned int tris_culled;       /* rejected by trivial reject / back-face */
    unsigned int tris_rasterised;   /* called sw_draw_tri */
    unsigned long scanlines;        /* total scanlines across all tris */
    unsigned long span_pixels;      /* total span width (pre z-test) */
    unsigned long pixels_written;   /* passed z-test and wrote to framebuffer */
    unsigned int frames;            /* gfx_flip calls */
    unsigned int vt_ms;             /* vertex transform (allegro_gfx_mesh) */
    unsigned int tri_ms;            /* triangle loop (clip + cull + rasterise) */
    int backbuf_w, backbuf_h;       /* backbuffer dimensions */
    int zbuf_w, zbuf_h;             /* z-buffer dimensions */
    int render_w, render_h;         /* logical render resolution */
} g_prof;

static void allegro_shutdown_trace(const char *step) {
    if (!g_shutdown_trace_on) return;
    FILE *f = fopen("TIGRYX.LOG", "a");
    if (!f) return;
    fprintf(f, "DOS: shutdown: %s\n", step);
    fflush(f);
    fclose(f);
}

/* Mode-selection trace. @window can spend minutes inside failed VESA BIOS
 * calls on real hardware, and a mode-set that wedges the machine leaves
 * nothing on screen to explain why, so each probe is written to TIGRYX.LOG and
 * the file flushed and closed immediately. After a hang, the last line names
 * the call that did not return.
 *
 * Deliberately NOT gated on --verbose: a bound executable has no way to pass
 * runtime flags, and this is a couple of dozen lines once at start-up. */
static void allegro_mode_log(const char *fmt, ...) {
    FILE *f = fopen("TIGRYX.LOG", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
    fclose(f);
}

/* Forward declarations */
        static void allegro_console_open_log(void);
        static void allegro_pump_keys(AllegroState *s);
        static void allegro_font_shutdown(AllegroState *s);
        static void sw_setup_8bit_palette(PALETTE pal);
        static void sw_build_lookup_table(unsigned char **out_table);
        static void sw_sample_texture_colours(AllegroState *s,
                                              const u8 *pixels, int pw, int ph);
        static void sw_build_optimised_palette(AllegroState *s);
        static void sw_build_lookup_table_guided(unsigned char **out_table,
                const PALETTE pal);
        static void allegro_apply_clip(AllegroState *s);

/* -------------------------------------------------------------------------
 * Init / shutdown
 * ---------------------------------------------------------------------- */
/* Sound init: compiled exes (RELICE_WINDOWED rt archive) opt IN with a
 * --sound program arg (@args) — the SB/MPU-401 probe cascade is
 * port-timeout bound and takes minutes on a soundless machine. The
 * interpreter always probes; a per-program flag makes no sense there. */
static int relic_arg_sound(const RelicVM *vm) {
#ifdef RELICE_WINDOWED
    int i;
    for (i = 0; i < vm->cli_argc; i++)
        if (vm->cli_argv[i] && !strcmp(vm->cli_argv[i], "--sound"))
            return 1;
    return 0;
#else
    (void)vm;
    return 1;
#endif
}

        static void allegro_init_backend(RelicVM *vm) {
    if (allegro_init() != 0) {
        fprintf(stderr, "Allegro initialization failed\n");
        return;
    }

    AllegroState *s = calloc(1, sizeof(AllegroState));
    s->width = 320;
    s->height = 200;
    s->ss = 1;
    g_state = s;
    g_vm = vm;
    vm->backend_data = s;

    install_keyboard();
    s->have_mouse = (install_mouse() >= 0);
    install_timer();

    /* Install a 1 kHz millisecond counter for @millis(). Lock the handler
     * and its counter into memory first (Allegro ISR requirement). */
    LOCK_VARIABLE(g_millis_counter);
    LOCK_FUNCTION(relic_millis_tick);
    install_int_ex(relic_millis_tick, BPS_TO_TIMER(1000));

    /* Try to initialise digital + MIDI sound. Failure is non-fatal — snd_play
     * just becomes a no-op if there's no SB-compatible device. The probe
     * sequence waits on SB/MPU-401 port timeouts, which takes minutes on a
     * soundless VM; it only runs when the program is given --sound. */
    if (relic_arg_sound(vm) &&
        install_sound(DIGI_AUTODETECT, MIDI_AUTODETECT, NULL) != 0) {
        /* Try digi-only as fallback (MIDI might not be available). */
        if (install_sound(DIGI_AUTODETECT, MIDI_NONE, NULL) != 0) {
            /* leave digi_driver as DIGI_NONE; play_sample will be skipped */
        }
    }

    srand((unsigned int) time(NULL));
}

static void allegro_proc_shutdown(AllegroState *s);

static void allegro_shutdown(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) return;

    g_shutdown_trace_on = vm->verbose;
    allegro_shutdown_trace("enter");

    /* Tear the mouse down BEFORE any video mode switch. allegro_gfx_window
     * called show_mouse(screen), which makes the timer-driven mouse ISR draw
     * the software cursor into the `screen` bitmap. set_gfx_mode() destroys
     * and rebuilds `screen`, so if the cursor is still shown the ISR keeps
     * writing through a stale VESA linear-framebuffer pointer. On DOSBox the
     * emulated memory tolerates that; on real FreeDOS under a V86 memory
     * manager it faults inside the interrupt handler, which wedges the
     * machine and leaves the cursor's saved background behind as a small
     * black block over the last rendered frame. Allegro requires
     * show_mouse(NULL) before changing modes or destroying the target bitmap. */
    show_mouse(NULL);
    remove_mouse();
    allegro_shutdown_trace("mouse removed");

    /* Only restore text mode if we actually entered a graphics mode.
     * Console-only programs (no @window call) already run in text mode;
     * resetting the video mode there clears the screen and throws away
     * everything the program printed. */
    if (s->gfx_mode_set) {
        /* Unhook our own 1 kHz millisecond ISR too. The mode switch below
         * ends in a real-mode BIOS INT 10h; a protected-mode handler firing
         * at 1 kHz through the DPMI host mid-call is the other way FreeDOS
         * hangs here. Allegro's own timer subsystem stays installed because
         * the VESA driver needs it during the switch. */
        remove_int(relic_millis_tick);
        allegro_shutdown_trace("millis ISR removed");

        /* Restore standard 80x25 16-colour text mode ourselves, straight
         * through the real-mode BIOS. This is the *only* mode reset performed
         * on DOS; see the comment below for why Allegro's own one cannot be
         * used. JEMM386 reflects the INT 10h down to real mode, and it is
         * harmless on MS-DOS/DOSBox (it just re-selects mode 3). */
        __dpmi_regs r;
        memset(&r, 0, sizeof r);
        r.x.ax = 0x0003;   /* AH=00h set video mode, AL=03h = text 80x25 */
        __dpmi_int(0x10, &r);
        allegro_shutdown_trace("BIOS int 10h mode 3 returned");

        /* Clear the text screen via BIOS INT 10h AH=06h (scroll up window)
         * with AL=0, which blanks the whole window with the given attribute.
         * The mode set above re-programs the CRTC but does not necessarily
         * blank VRAM, so without this the shell can come back over the top
         * of whatever bytes the graphics mode left behind. */
        __dpmi_regs sr;
        memset(&sr, 0, sizeof sr);
        sr.x.ax = 0x0600;   /* AH=06h scroll up, AL=00h = full clear */
        sr.x.bx = 0x0700;   /* BH=07h attribute (white on black) */
        sr.x.cx = 0x0000;   /* top-left = (0,0) */
        sr.x.dx = 0x184F;   /* bottom-right = (24,79) */
        __dpmi_int(0x10, &sr);
        allegro_shutdown_trace("text screen cleared");

        /* Do NOT call set_gfx_mode(GFX_TEXT, ...) here -- and make sure Allegro
         * cannot call it either.
         *
         * On FreeDOS + JEMM386 with a VESA2L linear-framebuffer mode, Allegro's
         * own mode teardown cannot be used at all. Called before the BIOS mode
         * set it hangs forever (the machine keeps servicing interrupts, so the
         * BIOS beeps on key presses once its type-ahead buffer fills, but the
         * process never returns to COMMAND.COM). Called after the BIOS mode set
         * it faults instead: "Page fault ... error=0006" is a *write* to a
         * not-present page, its VESA exit path writing through the framebuffer
         * mapping that mode 3 has just invalidated.
         *
         * The display is already correct courtesy of the INT 10h above, so the
         * only thing Allegro's teardown can still contribute is the crash.
         * Clearing gfx_driver makes Allegro believe no graphics mode is active,
         * so the shutdown_gfx() exit hook that allegro_exit() runs skips its
         * set_gfx_mode(GFX_TEXT) call. The VESA selectors and the screen bitmap
         * are consequently never released, which is harmless here: the DPMI
         * host reclaims all of it when the process image is torn down moments
         * later, and every actual interrupt hook is still removed explicitly
         * below. */
        gfx_driver = NULL;
        allegro_shutdown_trace("gfx_driver cleared (Allegro mode reset skipped)");
    }

    /* Audio teardown runs AFTER the video mode is back. Stopping an SB16 mid
     * DMA burst is the other known way DOS exits stall, and if it does stall
     * here the user is at least looking at a readable text screen rather than
     * a frozen last frame. */
    if (g_midi_playing) {
        stop_midi();
        destroy_midi(g_midi_playing);
        g_midi_playing = NULL;
    }
    if (g_mus_sample) {
        stop_sample(g_mus_sample);
        destroy_sample(g_mus_sample);
        g_mus_sample = NULL;
    }
    if (g_mus_stream) {
        stop_audio_stream(g_mus_stream);
        g_mus_stream = NULL;
    }
    if (g_snd_prev) {
        stop_sample(g_snd_prev);
        destroy_sample(g_snd_prev);
        g_snd_prev = NULL;
    }
    allegro_shutdown_trace("samples stopped");
    if (digi_driver && digi_driver->id != DIGI_NONE) {
        remove_sound();
    }
    allegro_shutdown_trace("remove_sound returned");

    remove_keyboard();
    remove_timer();
    allegro_shutdown_trace("keyboard/timer removed");

    /* Release 3D resources. */
    {
        int i;
        for (i = 1; i < s->tex_cap; i++) {
            if (s->textures[i].pixels) {
                free(s->textures[i].pixels);
                s->textures[i].pixels = NULL;
            }
            if (s->textures[i].bmp) {
                destroy_bitmap(s->textures[i].bmp);
                s->textures[i].bmp = NULL;
            }
        }
        free(s->textures);
        s->textures = NULL;
        s->tex_cap = 0;
    }
    if (s->sw_zbuf) {
        free(s->sw_zbuf);
        s->sw_zbuf = NULL;
    }
    /* In page-flip mode backbuf aliases one of the VRAM pages, and both pages
     * were already invalidated by the video mode reset above (Allegro destroys
     * every video bitmap on a mode change, and the VESA mapping they pointed
     * into is gone). Drop the pointers without destroying them; destroying a
     * stale video bitmap here would write through an unmapped selector. */
    if (s->page_flip) {
        s->vpage[0] = NULL;
        s->vpage[1] = NULL;
        s->backbuf = NULL;
        s->page_flip = false;
    }
    if (s->backbuf) {
        destroy_bitmap(s->backbuf);
        s->backbuf = NULL;
    }
    if (s->resolve) {
        destroy_bitmap(s->resolve);
        s->resolve = NULL;
    }
    if (s->planar_stage) {
        free(s->planar_stage);
        s->planar_stage = NULL;
    }
    if (s->planar_dirty) {
        free(s->planar_dirty);
        s->planar_dirty = NULL;
    }
    if (s->rgb_to_idx) {
        free(s->rgb_to_idx);
        s->rgb_to_idx = NULL;
    }
    if (s->sw_col_samples) {
        free(s->sw_col_samples);
        s->sw_col_samples = NULL;
    }

    /* Sector-based world renderer resources. */
    {
        int i;
        if (s->wr_colormap) { free(s->wr_colormap); s->wr_colormap = NULL; }
        for (i = 0; i < WR_MAX_TEXTURES; i++) {
            if (s->wr_textures[i].data) { free(s->wr_textures[i].data); s->wr_textures[i].data = NULL; }
        }
        for (i = 0; i < WR_MAX_FLATS; i++) {
            if (s->wr_flats[i]) { free(s->wr_flats[i]); s->wr_flats[i] = NULL; }
        }
        if (s->wr_vertices)   { free(s->wr_vertices);   s->wr_vertices = NULL; }
        if (s->wr_linedefs)   { free(s->wr_linedefs);   s->wr_linedefs = NULL; }
        if (s->wr_sidedefs)   { free(s->wr_sidedefs);   s->wr_sidedefs = NULL; }
        if (s->wr_sectors)    { free(s->wr_sectors);    s->wr_sectors = NULL; }
        if (s->wr_segs)       { free(s->wr_segs);       s->wr_segs = NULL; }
        if (s->wr_subsectors) { free(s->wr_subsectors); s->wr_subsectors = NULL; }
        if (s->wr_nodes)      { free(s->wr_nodes);      s->wr_nodes = NULL; }
        if (s->wr_sd_tex)     { free(s->wr_sd_tex);     s->wr_sd_tex = NULL; }
        if (s->wr_sec_tex)    { free(s->wr_sec_tex);    s->wr_sec_tex = NULL; }
        if (wr_plane_buf)     { free(wr_plane_buf);     wr_plane_buf = NULL; wr_plane_buf_w = 0; }
    }

    allegro_shutdown_trace("bitmaps freed");

    /* Subprocess capture files: close any open read handle and delete the
     * temp files (DOS does not clean TMP itself). */
    allegro_proc_shutdown(s);

    /* --gfx-prof dump: write to TIGRYX.LOG before the state is freed. */
    if (g_relic_gfx_prof && g_prof.frames > 0) {
        FILE *pf = fopen("TIGRYX.LOG", "a");
        if (pf) {
            unsigned long bpix = (unsigned long)g_prof.zbuf_w * g_prof.zbuf_h;
            unsigned long over_x10 = bpix ? (g_prof.span_pixels * 10UL) / (bpix * g_prof.frames) : 0;
            fprintf(pf, "\n=== GFX PROFILER (%u frames) ===\n", g_prof.frames);
            fprintf(pf, "backbuf: %dx%d  zbuf: %dx%d  render: %dx%d\n",
                    g_prof.backbuf_w, g_prof.backbuf_h,
                    g_prof.zbuf_w, g_prof.zbuf_h,
                    g_prof.render_w, g_prof.render_h);
            fprintf(pf, "mesh_calls:   %u  (%u/frame)\n",
                    g_prof.mesh_calls, g_prof.mesh_calls / g_prof.frames);
            fprintf(pf, "verts_xformed: %u  (%u/frame)\n",
                    g_prof.verts_xformed, g_prof.verts_xformed / g_prof.frames);
            fprintf(pf, "tris_considered: %u  (%u/frame)\n",
                    g_prof.tris_considered, g_prof.tris_considered / g_prof.frames);
            fprintf(pf, "tris_rasterised: %u  (%u/frame)\n",
                    g_prof.tris_rasterised, g_prof.tris_rasterised / g_prof.frames);
            fprintf(pf, "tris_culled:     %u  (%u/frame)\n",
                    g_prof.tris_considered - g_prof.tris_rasterised,
                    (g_prof.tris_considered - g_prof.tris_rasterised) / g_prof.frames);
            fprintf(pf, "scanlines:    %lu  (%lu/frame)\n",
                    g_prof.scanlines, g_prof.scanlines / g_prof.frames);
            fprintf(pf, "span_pixels:  %lu  (%lu/frame)\n",
                    g_prof.span_pixels, g_prof.span_pixels / g_prof.frames);
            fprintf(pf, "backbuf_pix:  %lu  overdraw: %u.%ux\n",
                    bpix, over_x10 / 10, over_x10 % 10);
            fprintf(pf, "vt_ms:   %u  (%u ms/frame)\n",
                    g_prof.vt_ms, g_prof.vt_ms / g_prof.frames);
            fprintf(pf, "tri_ms:  %u  (%u ms/frame)\n",
                    g_prof.tri_ms, g_prof.tri_ms / g_prof.frames);
            fprintf(pf, "=== END PROFILER ===\n");
            fflush(pf);
            fclose(pf);
        }
    }

    allegro_font_shutdown(s);
    allegro_shutdown_trace("fonts freed");

#ifdef HAVE_DOSNET
    dos_http_shutdown(s->http);
#ifdef TIGRYX_EXPERIMENTAL_SERVER
    dos_serve_shutdown(vm);
#endif
    allegro_shutdown_trace("tcpip shut down");
#endif

    if (g_console_log) {
        fflush(g_console_log);
        fclose(g_console_log);
        g_console_log = NULL;
    }

    free(s);
    vm->backend_data = NULL;
    g_state = NULL;
    g_vm = NULL;

    allegro_shutdown_trace("calling allegro_exit");
    allegro_exit();
    allegro_shutdown_trace("allegro_exit returned");
}

/* -------------------------------------------------------------------------
 * Graphics functions (Stage 2 - implementation)
 * ---------------------------------------------------------------------- */

/* Standard VGA resolutions for DOS */
static const int resolutions[][2] = {
        {320, 200},
        {640, 480},
        {800, 600},
        {1024, 768},
        {1280, 1024}
};
static const int num_resolutions = 5;

/* Detect an Oak Technology VGA chipset (OTI-067 / OTI-077) from the VBE
 * controller-info block's OEM string.
 *
 * These cards cannot page-flip. The flip works by pointing the CRTC Start
 * Address (registers 0x0C/0x0D) at the second page, which on a 320x200x8
 * two-page layout sits above the 64 KB mark. The OTI chipsets map 128 KB
 * graphics memory incorrectly and mis-bank once the start address crosses a
 * 64 KB boundary: page 1 resolves into the wrong VRAM segment and lands on
 * the text-mode attribute region, so alternate frames show blue text-mode
 * garbage instead of the rendered page. It is a documented hardware fault
 * that affects other DOS software too, not something software can work
 * around — the classic workaround flips between two 16 KB mode Y pages
 * that never cross the boundary.
 *
 * Read-only probe: asks the BIOS for the info block and follows its OEM
 * string pointer. No chipset ports are poked, so it is safe on any card; a
 * card without VBE (or with no OEM string) simply reports "not Oak". */
static bool dos_gfx_is_oak(void) {
    __dpmi_regs r;
    unsigned char info[512];
    char oem[128];
    unsigned oem_off, oem_seg;
    unsigned long oem_lin;
    int i;

    /* "VBE2" in the request block asks for VBE 2.x extensions; VBE 1.x BIOSes
     * ignore it, so it is safe to send unconditionally. */
    dosmemput("VBE2", 4, __tb);

    memset(&r, 0, sizeof r);
    r.x.ax = 0x4F00;                      /* VBE: return controller info */
    r.x.es = (unsigned short) ((__tb >> 4) & 0xFFFFu);
    r.x.di = (unsigned short) (__tb & 0x0Fu);
    __dpmi_int(0x10, &r);
    if (r.x.ax != 0x004F) return false;   /* no VBE support */

    dosmemget(__tb, sizeof info, info);
    if (memcmp(info, "VESA", 4) != 0) return false;

    /* Offset 6 in the info block is a real-mode far pointer to the OEM
     * string: two little-endian words, offset then segment. */
    oem_off = (unsigned) info[6]  | ((unsigned) info[7] << 8);
    oem_seg = (unsigned) info[8]  | ((unsigned) info[9] << 8);
    oem_lin = ((unsigned long) oem_seg << 4) + oem_off;
    if (oem_lin == 0) return false;

    dosmemget(oem_lin, sizeof oem - 1, oem);
    oem[sizeof oem - 1] = '\0';
    for (i = 0; oem[i]; i++)
        if (oem[i] >= 'a' && oem[i] <= 'z') oem[i] = (char) (oem[i] - 32);

    allegro_mode_log("DOS: gfx: VBE OEM string: \"%s\"\n", oem);
    return strstr(oem, "OAK") != NULL || strstr(oem, "OTI") != NULL;
}

static void allegro_gfx_window(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) return;

    /* A repeat @window call re-enters set_gfx_mode below, which destroys and
     * rebuilds `screen`. Hide the cursor first so the mouse ISR cannot draw
     * through the outgoing screen bitmap mid-switch; show_mouse(screen) is
     * re-issued once the new mode is up. */
    if (s->gfx_mode_set) show_mouse(NULL);

    /* @window(x, y, width, height [, flags]) — args in r[1..4]. r[1],r[2]
     * are x,y (ignored on DOS). r[5] is an optional u32 flags word:
     * bit 0 = RELIC_WINFLAG_STRETCH, bit 3 = RELIC_WINFLAG_AA (2x SSAA). */
    int req_w = vm->r[3];
    int req_h = vm->r[4];
    if (req_w <= 0) req_w = 640;
    if (req_h <= 0) req_h = 480;
    u32 flags = (u32) vm->r[5];
    (void) flags; /* stretch setup below */

    /* Page-flip mode: the probe loop below asks for a virtual screen tall
     * enough for two pages so Allegro selects an unchained planar mode (mode Y
     * at 320x200x8) or a VESA mode with enough LFB for a second page.
     * want_flip is the script's request; got_flip records whether the mode we
     * settled on actually has the extra page. s->page_flip is only latched
     * once create_video_bitmap has also succeeded. */
    bool want_flip = (flags & RELIC_WINFLAG_FLIP) != 0;
    bool force_8bpp = (flags & RELIC_WINFLAG_8BPP) != 0;
    bool got_flip = false;

    /* Oak OTI cards mis-bank the second page (see dos_gfx_is_oak): honour the
     * script's WINFLAG_FLIP as a request, not a demand, and quietly fall back
     * to the RAM backbuffer + blit present rather than render garbage. The
     * probe costs one BIOS call, so only run it when a flip was actually
     * asked for. */
    if (want_flip && dos_gfx_is_oak()) {
        want_flip = false;
        allegro_mode_log("DOS: gfx: Oak OTI chipset detected; page-flip is "
                         "unsupported on this card (it mis-banks the second "
                         "page across the 64K boundary) -- using RAM "
                         "backbuffer + blit instead\n");
    }

    /* A repeat @window call re-enters set_gfx_mode, which invalidates every
     * video bitmap Allegro has handed out. Drop our references to the old
     * pages WITHOUT destroying them (the mode set does that itself; calling
     * destroy_bitmap on a stale video bitmap would write through a freed VESA
     * mapping). backbuf aliases one of the pages in flip mode, so it has to be
     * cleared here too or the RAM-bitmap teardown below would double-free it. */
    if (s->page_flip) {
        s->vpage[0] = NULL;
        s->vpage[1] = NULL;
        s->backbuf = NULL;
    }
    s->page_flip = false;
    s->planar_backbuf = false;
    s->vpage_active = 0;

    /* Snap requested size to the closest supported standard resolution. */
    int best_w = resolutions[0][0];
    int best_h = resolutions[0][1];
    int best_idx = 0;
    long best_score = -1;
    for (int i = 0; i < num_resolutions; i++) {
        long dw = resolutions[i][0] - req_w;
        long dh = resolutions[i][1] - req_h;
        long score = dw * dw + dh * dh;
        if (best_score < 0 || score < best_score) {
            best_score = score;
            best_idx = i;
            best_w = resolutions[i][0];
            best_h = resolutions[i][1];
        }
    }

    /* A repeat @window that resolves to the same mode and flags is a no-op:
     * DOS ignores the title argument and x/y, so nothing changes. Skipping
     * the re-entry avoids paying the VESA probe ladder again AND avoids the
     * second set_gfx_mode entirely — re-setting the mode under CWSDPMI +
     * JEMM386 can leave the VESA driver state wedged at teardown (observed
     * as a hang after TWIN.tgx's retitle call on FreeDOS). */
    if (s->gfx_mode_set && s->width == best_w && s->height == best_h &&
        s->win_flags == flags) {
        allegro_mode_log("DOS: gfx: repeat @window %dx%d same mode/flags - "
                         "no-op\n", req_w, req_h);
        return;
    }

    /* Build the colour-depth ladder. 16bpp comes first because the software
     * rasteriser is written around it: sw_put_pix packs a pixel with a couple
     * of shifts, whereas 8bpp has to index a palette lookup table per pixel
     * AND pay for building that 64^3-entry table (via bestfit_color) up front
     * in sw_build_lookup_table. 8bpp is therefore a last resort, not a
     * fast path.
     *
     * Keep this ladder short: every failed set_gfx_mode() is a round-trip
     * through the VESA BIOS, which on real 486-era hardware can cost seconds.
     * 24/32bpp are only probed when explicitly asked for via --colour-depth.
     *
     * WINFLAG_8BPP overrides the entire ladder with just {8}. */
    extern int g_relic_preferred_color_depth;
    int depths[4];
    int num_depths = 0;
    int requested_depth = g_relic_preferred_color_depth;
    if (force_8bpp) {
        depths[num_depths++] = 8;
        allegro_mode_log("DOS: gfx: WINFLAG_8BPP set; forcing 8bpp-only "
                         "depth ladder\n");
    } else {
        if (requested_depth != 0)
            depths[num_depths++] = requested_depth;
        /* Default ladder: 16 (preferred on DOS), then 15 for cards that only do
         * 5-5-5, then 8 as the universally-available paletted mode. */
        int default_depths[3] = {16, 15, 8};
        for (int i = 0; i < 3; i++) {
            bool dup = false;
            for (int j = 0; j < num_depths; j++)
                if (depths[j] == default_depths[i]) dup = true;
            if (!dup) depths[num_depths++] = default_depths[i];
        }
        if (requested_depth != 0 && requested_depth != 8 && requested_depth != 16) {
            allegro_mode_log("DOS: gfx: --colour-depth %d requested; "
                             "rasteriser is optimised for 8/16 bpp\n",
                    requested_depth);
        }
    }

    int chosen_w = 0, chosen_h = 0;
    bool ok = false;

    /* Build the try-list. Without WINFLAG_STRETCH the snapped request is a
     * hard requirement — the app lays out for exactly that resolution, so
     * no other mode is probed. With stretch the script accepts a
     * logical->physical rescale, so the ladder steps down from the
     * anchor: --res WxH pins the physical target (snapped), otherwise the
     * snapped request. Stepping UP is never tried — if the card cannot do
     * 320x200 it certainly cannot do 1280x1024 — and each doomed attempt
     * costs seconds of VESA BIOS probing on real hardware. */
    bool want_stretch = (flags & RELIC_WINFLAG_STRETCH) != 0;
    int anchor_w = best_w, anchor_h = best_h, anchor_idx = best_idx;
    if (want_stretch && g_relic_stretch_mode_w > 0 && g_relic_stretch_mode_h > 0) {
        long bs = -1;
        for (int i = 0; i < num_resolutions; i++) {
            long dw = resolutions[i][0] - g_relic_stretch_mode_w;
            long dh = resolutions[i][1] - g_relic_stretch_mode_h;
            long score = dw * dw + dh * dh;
            if (bs < 0 || score < bs) {
                bs = score; anchor_idx = i;
                anchor_w = resolutions[i][0]; anchor_h = resolutions[i][1];
            }
        }
        allegro_mode_log("DOS: gfx: --res %dx%d -> physical target %dx%d\n",
                g_relic_stretch_mode_w, g_relic_stretch_mode_h,
                anchor_w, anchor_h);
    }
    int try_list[16][2];
    int n_try = 0;
    try_list[n_try][0] = anchor_w;
    try_list[n_try][1] = anchor_h;
    n_try++;
    if (want_stretch) {
        for (int i = anchor_idx - 1; i >= 0; i--) {
            try_list[n_try][0] = resolutions[i][0];
            try_list[n_try][1] = resolutions[i][1];
            n_try++;
        }
    }

    /* Record what we are about to attempt, so a log recovered after a hang
     * shows the plan as well as how far it got. */
    allegro_mode_log("DOS: gfx: @window requested %dx%d -> snapped to %dx%d\n",
            req_w, req_h, best_w, best_h);
    {
        char buf[160];
        int n = 0;
        n += snprintf(buf + n, sizeof buf - n, "DOS: gfx: depth ladder:");
        for (int d = 0; d < num_depths && n < (int) sizeof buf - 8; d++)
            n += snprintf(buf + n, sizeof buf - n, " %d", depths[d]);
        allegro_mode_log("%s\n", buf);
        n = 0;
        n += snprintf(buf + n, sizeof buf - n, "DOS: gfx: resolution ladder:");
        for (int t = 0; t < n_try && n < (int) sizeof buf - 16; t++)
            n += snprintf(buf + n, sizeof buf - n, " %dx%d",
                    try_list[t][0], try_list[t][1]);
        allegro_mode_log("%s\n", buf);
    }

    /* Driver probe order. GFX_AUTODETECT is last because it internally walks
     * every registered driver, so it only adds value once the explicit VESA
     * attempts have failed. */
    static const struct { int id; const char *name; } drivers[] = {
            {GFX_VESA2L,     "VESA2L"},
            {GFX_VESA2B,     "VESA2B"},
            {GFX_VESA1,      "VESA1"},
            {GFX_AUTODETECT, "AUTODETECT"},
    };
    /* Page-flip probe order. The VESA drivers are tried first because a VESA
     * virtual screen is LINEAR: the rasteriser writes it exactly like a system
     * bitmap and no plane batching is needed at all. GFX_MODEX is the fallback
     * for plain VGA / VESA 1.x cards that cannot give a virtual screen — it
     * yields an unchained planar (mode Y) page, which works but needs the
     * plane-batched write path. AUTODETECT is omitted: it would happily pick a
     * chained mode with no second page, which defeats the whole exercise. */
    static const struct { int id; const char *name; } flip_drivers[] = {
            {GFX_VESA2L,     "VESA2L"},
            {GFX_VESA2B,     "VESA2B"},
            {GFX_VESA1,      "VESA1"},
            {GFX_MODEX,      "MODEX"},
    };
    const int n_drivers = (int) (sizeof drivers / sizeof drivers[0]);
    const int n_flip_drivers = (int) (sizeof flip_drivers / sizeof flip_drivers[0]);

    unsigned int probe_start_ms = g_millis_counter;
    int n_probes = 0;

    /* Pass 1 (page-flip only): ask for a virtual screen twice the requested
     * height, which is what reserves the second VRAM page the flip swaps to.
     * This is a complete pass in its own right — every resolution, depth and
     * flip-capable driver is tried with the virtual screen before pass 2 gives
     * up on flipping. Interleaving the two (trying a mode without the virtual
     * screen as soon as one driver refused it) would settle for a blit-only
     * mode on the first driver that happened to fail, and never reach the
     * MODEX fallback that would have delivered a real second page. */
    if (want_flip) {
        for (int t = 0; t < n_try && !ok; t++) {
            int w = try_list[t][0];
            int h = try_list[t][1];

            for (int d = 0; d < num_depths && !ok; d++) {
                set_color_depth(depths[d]);

                for (int k = 0; k < n_flip_drivers && !ok; k++) {
                    unsigned int t0 = g_millis_counter;
                    /* Log BEFORE the call: if this one wedges the machine, the
                     * recovered log ends here and names the culprit. */
                    allegro_mode_log("DOS: gfx: try %dx%d %dbpp via %s "
                                     "(virtual %dx%d for page-flip) ...\n",
                            w, h, depths[d], flip_drivers[k].name, w, h * 2);
                    int rc = set_gfx_mode(flip_drivers[k].id, w, h, w, h * 2);
                    n_probes++;
                    allegro_mode_log("DOS: gfx:   %s after %u ms\n",
                            rc == 0 ? "OK" : "failed", g_millis_counter - t0);
                    if (rc == 0) {
                        chosen_w = w;
                        chosen_h = h;
                        got_flip = true;
                        ok = true;
                    }
                }
            }
        }
        if (!ok) {
            allegro_mode_log("DOS: gfx: no mode could reserve a second page; "
                             "page-flip unavailable, falling back to blit\n");
        }
    }

    /* Pass 2: ordinary (single-page) mode set. Resolution-major: honour the
     * requested size first, walking the depth ladder within it, before
     * stepping down to a smaller mode. */
    for (int t = 0; t < n_try && !ok; t++) {
        int w = try_list[t][0];
        int h = try_list[t][1];

        for (int d = 0; d < num_depths && !ok; d++) {
            set_color_depth(depths[d]);

            for (int k = 0; k < n_drivers && !ok; k++) {
                unsigned int t0 = g_millis_counter;
                allegro_mode_log("DOS: gfx: try %dx%d %dbpp via %s ...\n",
                        w, h, depths[d], drivers[k].name);
                int rc = set_gfx_mode(drivers[k].id, w, h, 0, 0);
                unsigned int dt = g_millis_counter - t0;
                n_probes++;
                allegro_mode_log("DOS: gfx:   %s after %u ms\n",
                        rc == 0 ? "OK" : "failed", dt);
                if (rc == 0) {
                    chosen_w = w;
                    chosen_h = h;
                    ok = true;
                }
            }
        }
    }

    if (!ok && !want_stretch && best_w == 320 && best_h == 200) {
        /* Mode 13h IS the requested resolution, driven via the plain VGA
         * driver rather than VESA — one last probe of the same mode. */
        allegro_mode_log("DOS: gfx: trying VGA mode 13h driver path\n");
        set_color_depth(8);
        int rc = set_gfx_mode(GFX_VGA, 320, 200, 0, 0);
        n_probes++;
        if (rc != 0) rc = set_gfx_mode(GFX_AUTODETECT, 320, 200, 0, 0);
        if (rc == 0) { chosen_w = 320; chosen_h = 200; ok = true; }
    }

    if (!ok && want_stretch) {
        /* Stretch means "run on whatever mode we get": universal fallback
         * is 320x200 VGA mode 13h, which is 8bpp only — asking for any
         * other depth here can only fail. */
        allegro_mode_log("DOS: gfx: all probes failed; falling back to "
                         "VGA mode 13h (320x200x8)\n");
        set_color_depth(8);
        unsigned int t0 = g_millis_counter;
        int rc = set_gfx_mode(GFX_VGA, 320, 200, 0, 0);
        n_probes++;
        if (rc != 0) {
            allegro_mode_log("DOS: gfx:   GFX_VGA failed after %u ms; "
                             "trying AUTODETECT\n", g_millis_counter - t0);
            rc = set_gfx_mode(GFX_AUTODETECT, 320, 200, 0, 0);
            n_probes++;
        }
        if (rc == 0) {
            chosen_w = 320;
            chosen_h = 200;
            ok = true;
            /* Neither fallback reserved a virtual screen, so there is no
             * second page to flip to regardless of what the script asked. */
            got_flip = false;
        }
    }

    if (!ok) {
        /* The requested mode is unavailable and the script did not opt
         * into stretch — run nothing rather than silently degrade into a
         * mode the layout was not written for. */
        allegro_mode_log("DOS: gfx: FATAL: mode %dx%d unavailable "
                         "(%d probes, %u ms); change the @window size or "
                         "pass WINFLAG_STRETCH (+ --res WxH)\n",
                best_w, best_h, n_probes, g_millis_counter - probe_start_ms);
        fprintf(stderr, "DOS: graphics mode %dx%d unavailable; change the "
                        "@window size or pass WINFLAG_STRETCH (--res WxH)\n",
                best_w, best_h);
        exit(1);
    }

    allegro_mode_log("DOS: gfx: selected %dx%d %dbpp after %d probe(s), %u ms\n",
            chosen_w, chosen_h, bitmap_color_depth(screen),
            n_probes, g_millis_counter - probe_start_ms);

    s->width = chosen_w;
    s->height = chosen_h;
    s->virt_w = req_w;
    s->virt_h = req_h;
    s->color_depth = bitmap_color_depth(screen);
    s->frame_dirty = true;  /* first flip after mode-set must present */
    s->gfx_mode_set = true; /* shutdown must now restore text mode */

    /* Stretch / letterbox setup */
    s->win_flags = flags;
    s->stretch_enabled = want_stretch;
    s->aa_enabled = (flags & RELIC_WINFLAG_AA) != 0;
    s->direct_draw = (flags & RELIC_WINFLAG_DIRECT) != 0;
    s->logical_w = req_w;
    s->logical_h = req_h;

    /* Page-flip and direct-draw are mutually exclusive: direct-draw has no
     * backbuffer at all, page-flip's backbuffer IS the offscreen VRAM page.
     * Direct-draw wins if a script sets both (it is the more explicit "no
     * double buffering at all" request). */
    if (s->direct_draw) got_flip = false;

    /* Page-flip presents by swapping the display start address to a
     * full-screen VRAM page, so the page has to BE the full physical screen —
     * there is no scaling step in which to stretch or letterbox. Stretch and
     * SSAA are therefore disabled; the requested logical size is honoured by
     * centring the drawing origin inside the page (offset_x/y below), which is
     * the same letterbox the flip-less path gets from its blit destination. */
    if (got_flip) {
        s->stretch_enabled = false;
        s->aa_enabled = false;
    }

    /* Supersample factor: 2x when AA requested and colour depth supports
     * box-filter downsample (needs >= 15bpp for meaningful averaging).
     * 8bpp paletted mode can't average palette indices, so SSAA is disabled. */
    s->ss = (s->aa_enabled && s->color_depth >= 15) ? 2 : 1;

    int render_w = req_w * s->ss;
    int render_h = req_h * s->ss;

    /* The flip page spans the whole physical screen. */
    if (got_flip) {
        render_w = chosen_w;
        render_h = chosen_h;
    }

    if (s->stretch_enabled) {
        /* Non-uniform stretch: fill the entire physical screen.
         * This matches classic DOS behaviour (e.g. 320x200 -> 640x480)
         * and is what most users expect from "stretch" mode. */
        s->vp_x = 0;
        s->vp_y = 0;
    } else {
        /* Letterbox: centre the virtual surface on the physical screen.
         * If the physical mode is smaller, offset_x/y stay at 0 and the
         * virtual surface is simply clipped. */
        s->vp_x = (chosen_w - req_w) / 2;
        s->vp_y = (chosen_h - req_h) / 2;
        if (s->vp_x < 0) s->vp_x = 0;
        if (s->vp_y < 0) s->vp_y = 0;
    }

    /* Setup 8-bit palette and lookup table if needed */
    if (s->rgb_to_idx) {
        free(s->rgb_to_idx);
        s->rgb_to_idx = NULL;
    }
    /* Free any previous texture-colour sample buffer (from a prior @window). */
    if (s->sw_col_samples) {
        free(s->sw_col_samples);
        s->sw_col_samples = NULL;
    }
    s->sw_col_count = 0;
    s->sw_pal_built = false;
    s->sw_pal_mode = SW_PAL_AUTO;
    s->sw_pal_custom_count = 0;
    if (s->color_depth == 8) {
        PALETTE pal;
        unsigned int t0 = g_millis_counter;
        /* Set a 3-3-2 palette as the initial fallback. The optimised
         * palette from texture colours is built at the first @camera()
         * call, after textures have been uploaded. */
        sw_setup_8bit_palette(pal);
        set_palette(pal);  /* Must set before building lookup table! */
        sw_build_lookup_table(&s->rgb_to_idx);
        /* Allocate the texture-colour sample buffer (72 KB). This is the
         * only extra memory the optimised palette path needs. Freed at
         * shutdown or when the optimised palette is built. */
        s->sw_col_samples = (unsigned char *) malloc(SW_MAX_SAMPLES * 3);
        if (!s->sw_col_samples) {
            allegro_mode_log("DOS: gfx: 8bpp optimised palette disabled "
                    "(could not allocate %d-byte sample buffer)\n",
                    SW_MAX_SAMPLES * 3);
        }
        allegro_mode_log("DOS: gfx: 8bpp palette LUT (%d entries) built in %u ms%s\n",
                SW_PAL_LUT_ENTRIES, g_millis_counter - t0,
                s->rgb_to_idx ? "" : " (ALLOCATION FAILED)");
    }

    allegro_console_open_log();
    if (g_console_log && g_vm && g_vm->verbose) {
        fprintf(g_console_log, "  ss=%d aa=%d depth=%d render=%dx%d\n",
                s->ss, (int)s->aa_enabled, s->color_depth, render_w, render_h);
        fflush(g_console_log);
    }

    /* (Re)allocate the offscreen backbuffer at the render size.
     * Allocate this FIRST so if it fails at supersampled size we can fall
     * back before wasting memory on a large z-buffer.
     * gfx_flip blits or stretch_blits it to the physical screen.
     *
     * In direct-draw mode (RELIC_WINFLAG_DIRECT) the backbuffer is skipped
     * entirely: DRAW_TGT() returns `screen` and gfx_flip skips the blit.
     * SSAA is meaningless without a backbuffer, so force ss=1. */
    if (s->backbuf) {
        destroy_bitmap(s->backbuf);
        s->backbuf = NULL;
    }
    if (s->direct_draw) {
        s->ss = 1;
        render_w = req_w;
        render_h = req_h;
        if (g_console_log && g_vm && g_vm->verbose) {
            fprintf(g_console_log, "  direct-draw: backbuffer skipped (drawing to screen)\n");
            fflush(g_console_log);
        }
    } else if (got_flip) {
        /* Page-flip: two full-screen pages carved out of the video card's own
         * memory (the virtual screen reserved by the mode set above). The
         * script renders into whichever one is hidden; DRAW_TGT() returns it
         * unchanged, so every 2D primitive and the SW rasteriser write straight
         * into VRAM and gfx_flip presents by swapping the CRTC start address
         * rather than copying the whole framebuffer.
         *
         * Two pages are required, not one: with a single page the frame being
         * drawn would be the frame on screen, which is the tearing this mode
         * exists to avoid. */
        s->ss = 1;
        /* No depth argument: a video bitmap is carved out of the live
         * framebuffer, so it always has the screen's colour depth. */
        s->vpage[0] = create_video_bitmap(render_w, render_h);
        s->vpage[1] = create_video_bitmap(render_w, render_h);
        allegro_mode_log("DOS: gfx: page-flip pages %dx%d: %s / %s\n",
                render_w, render_h,
                s->vpage[0] ? "OK" : "FAILED",
                s->vpage[1] ? "OK" : "FAILED");
        if (s->vpage[0] && s->vpage[1]) {
            s->page_flip = true;
            /* Planar (mode Y / VESA 101h) pages need the plane-batched
             * rasteriser path; linear LFB pages are written directly. */
            s->planar_backbuf = is_planar_bitmap(s->vpage[0]) ? true : false;
            /* Start both pages black so the first frame cannot show whatever
             * the previous mode left in VRAM. One-time init cost. */
            clear_to_color(s->vpage[0], makecol(0, 0, 0));
            clear_to_color(s->vpage[1], makecol(0, 0, 0));
            /* Show page 0, draw into page 1. */
            show_video_bitmap(s->vpage[0]);
            s->vpage_active = 1;
            s->backbuf = s->vpage[1];
            allegro_mode_log("DOS: gfx: page-flip active, %s framebuffer\n",
                    s->planar_backbuf ? "planar (plane-batched rasteriser)"
                                      : "linear");
        } else {
            /* Not enough VRAM for two pages after all — release whichever one
             * did allocate and fall back to system RAM + blit. */
            if (s->vpage[0]) destroy_bitmap(s->vpage[0]);
            if (s->vpage[1]) destroy_bitmap(s->vpage[1]);
            s->vpage[0] = s->vpage[1] = NULL;
            got_flip = false;
            render_w = req_w;
            render_h = req_h;
            s->backbuf = create_bitmap_ex(s->color_depth, render_w, render_h);
            if (s->backbuf) clear_to_color(s->backbuf, makecol(0, 0, 0));
            allegro_mode_log("DOS: gfx: page-flip unavailable; using RAM "
                             "backbuffer + blit\n");
        }
    } else {
    s->backbuf = create_bitmap_ex(s->color_depth, render_w, render_h);
    if (g_console_log && g_vm && g_vm->verbose) {
        fprintf(g_console_log, "  backbuf %dx%d: %s\n", render_w, render_h,
                s->backbuf ? "OK" : "FAILED");
        fflush(g_console_log);
    }
    if (s->backbuf) {
        clear_to_color(s->backbuf, makecol(0, 0, 0));
    } else if (s->ss > 1) {
        /* Supersampled backbuffer too large for available memory — fall
         * back to logical-size backbuffer and disable SSAA. */
        s->ss = 1;
        render_w = req_w;
        render_h = req_h;
        s->backbuf = create_bitmap_ex(s->color_depth, render_w, render_h);
        if (g_console_log && g_vm && g_vm->verbose) {
            fprintf(g_console_log, "  SSAA disabled, backbuf fallback %dx%d: %s\n",
                    render_w, render_h, s->backbuf ? "OK" : "FAILED");
            fflush(g_console_log);
        }
        if (s->backbuf)
            clear_to_color(s->backbuf, makecol(0, 0, 0));
    }
    } /* end else (not direct_draw) */

    /* Reallocate SW z-buffer for the (possibly reduced) render resolution. */
    if (s->sw_zbuf) {
        free(s->sw_zbuf);
        s->sw_zbuf = NULL;
    }
    s->sw_zbuf = (u16 *) malloc((size_t) render_w * (size_t) render_h * sizeof(u16));
    s->sw_zbuf_w = render_w;
    s->sw_zbuf_h = render_h;
    if (s->sw_zbuf)
        memset(s->sw_zbuf, 0, (size_t) render_w * (size_t) render_h * sizeof(u16));

    /* Capture dimensions for the --gfx-prof dump. */
    if (g_relic_gfx_prof) {
        g_prof.render_w = render_w;
        g_prof.render_h = render_h;
        g_prof.zbuf_w = render_w;
        g_prof.zbuf_h = render_h;
        g_prof.backbuf_w = s->backbuf ? s->backbuf->w : 0;
        g_prof.backbuf_h = s->backbuf ? s->backbuf->h : 0;
    }
    if (g_console_log && g_vm && g_vm->verbose) {
        fprintf(g_console_log, "  zbuf %dx%d: %s\n", render_w, render_h,
                s->sw_zbuf ? "OK" : "FAILED");
        fflush(g_console_log);
    }
    /* SW textures are RGBA32 (depth-independent); no re-upload needed. */
    s->frame_dirty = true;

    /* (Re)allocate the resolve bitmap at logical size for SSAA downsample. */
    if (s->resolve) {
        destroy_bitmap(s->resolve);
        s->resolve = NULL;
    }
    if (s->ss > 1) {
        s->resolve = create_bitmap_ex(s->color_depth, req_w, req_h);
        if (s->resolve) {
            clear_to_color(s->resolve, makecol(0, 0, 0));
        } else {
            /* Resolve allocation failed — disable SSAA, use backbuf directly. */
            s->ss = 1;
        }
        if (g_console_log && g_vm && g_vm->verbose) {
            fprintf(g_console_log, "  resolve %dx%d: %s\n", req_w, req_h,
                    s->resolve ? "OK" : "FAILED (SSAA disabled)");
            fflush(g_console_log);
        }
    }

    /* (Re)allocate the per-scanline staging buffers used by the plane-batched
     * rasteriser path. One row of palette indices plus a bit-per-pixel dirty
     * mask; only needed for a planar flip page. */
    if (s->planar_stage) {
        free(s->planar_stage);
        s->planar_stage = NULL;
    }
    if (s->planar_dirty) {
        free(s->planar_dirty);
        s->planar_dirty = NULL;
    }
    if (s->page_flip && s->planar_backbuf) {
        s->planar_stage = (unsigned char *) malloc((size_t) render_w);
        s->planar_dirty = (unsigned char *) malloc((size_t) ((render_w + 7) / 8));
        if (!s->planar_stage || !s->planar_dirty) {
            /* Without the staging row the plane-batched path cannot run and
             * per-pixel plane selects would be slower than the blit we are
             * trying to avoid. Drop to the ordinary RAM backbuffer instead. */
            free(s->planar_stage);
            s->planar_stage = NULL;
            free(s->planar_dirty);
            s->planar_dirty = NULL;
            s->planar_backbuf = false;
            allegro_mode_log("DOS: gfx: planar staging alloc failed; "
                             "page-flip rasteriser path disabled\n");
        }
    }

    /* offset_x/y centres the logical surface on the physical screen for
     * 2D primitives.  The backbuffer is always logical size, so
     * 2D coords draw directly into the backbuf and physical centreing is
     * handled by the flip-time blit only (vp_x/y).
     *
     * Page-flip is the exception: its backbuffer IS the full physical page and
     * there is no blit to centre in, so physical centring is baked into
     * the drawing origin here instead. */
    s->offset_x = 0;
    s->offset_y = 0;
    if (s->page_flip) {
        s->offset_x = (chosen_w - req_w) / 2;
        s->offset_y = (chosen_h - req_h) / 2;
        if (s->offset_x < 0) s->offset_x = 0;
        if (s->offset_y < 0) s->offset_y = 0;
        s->vp_x = 0;
        s->vp_y = 0;
    }

    /* Re-apply the 2D clip rectangle on the new backbuffer. */
    allegro_apply_clip(s);

    /* Never arm Allegro's timer-ISR software cursor. In blit mode it draws
     * into the physical `screen` bitmap and the next frame's backbuffer blit
     * erases it (the ISR only redraws on mouse movement, so a stationary
     * pointer stays erased — the "invisible 99% of the time" bug). In
     * page-flip mode it would smear across alternating pages. gfx_flip
     * composites mouse_sprite onto the presented surface once per frame
     * instead, which is correct in both paths.
     *
     * set_mouse_sprite(NULL) installs the driver's default arrow in the
     * current colour depth; called unconditionally so a mode switch to a
     * different depth recreates it. */
    show_mouse(NULL);
    if (s->have_mouse) set_mouse_sprite(NULL);
    s->cur_x = -1;   /* force a composite on the first flip */
    s->cur_y = -1;

    s->frame_dirty = true;

    /* Log the chosen resolution for debugging */
    allegro_console_open_log();
    if (g_console_log && g_vm && g_vm->verbose) {
        if (s->stretch_enabled) {
            fprintf(g_console_log, "Graphics mode set: %dx%d stretch -> logical %dx%d (fill screen)%s\n",
                    chosen_w, chosen_h, req_w, req_h,
                    s->ss > 1 ? " [SSAA 2x]" : (s->aa_enabled ? " [SSAA requested but disabled]" : ""));
        } else {
            fprintf(g_console_log, "Graphics mode set: %dx%d (requested: %dx%d, offset: %d,%d)%s\n",
                    chosen_w, chosen_h, req_w, req_h, s->offset_x, s->offset_y,
                    s->ss > 1 ? " [SSAA 2x]" : (s->aa_enabled ? " [SSAA requested but disabled]" : ""));
        }
        if (s->ss > 1) {
            fprintf(g_console_log, "  SSAA: backbuf %dx%d, resolve %dx%d, depth %d\n",
                    s->backbuf ? s->backbuf->w : 0, s->backbuf ? s->backbuf->h : 0,
                    s->resolve ? s->resolve->w : 0, s->resolve ? s->resolve->h : 0,
                    s->color_depth);
        }
        if (s->page_flip) {
            fprintf(g_console_log, "  page-flip: %s VRAM page %dx%d, "
                            "origin %d,%d (no blit)\n",
                    s->planar_backbuf ? "planar" : "linear",
                    s->backbuf ? s->backbuf->w : 0,
                    s->backbuf ? s->backbuf->h : 0,
                    s->offset_x, s->offset_y);
        } else if (want_flip) {
            fprintf(g_console_log, "  page-flip requested but unavailable; "
                                   "using RAM backbuffer + blit\n");
        }
        fflush(g_console_log);
    }
}

static void allegro_gfx_clear(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) return;

    /* When --skip-clear is set (g_relic_skip_clear), skip the colour buffer
     * clear for programs whose 3D scene fully covers the framebuffer (e.g.
     * tdoom's sky + walls + floors + ceilings cover every pixel). The
     * z-buffer clear is always preserved for correct depth testing. */
    if (!g_relic_skip_clear) {
        u32 rgba_ptr = (u32) vm->r[1];
        int r = 0, g = 0, b = 0;
        if (rgba_ptr + 4 <= vm->mem_sz) {
            r = vm->mem[rgba_ptr];
            g = vm->mem[rgba_ptr + 1];
            b = vm->mem[rgba_ptr + 2];
        }
        int color = makecol(r, g, b);
        clear_to_color(DRAW_TGT(s), color);
    }
    /* Clear SW z-buffer (0 = nothing drawn) — only needed when 3D camera
     * rendering is active. @camera also clears it, so 2D-only programs (like
     * wdoom) skip this 128 KB memset every frame. */
    if (s->sw_zbuf && s->have_camera)
        memset(s->sw_zbuf, 0, (size_t) s->sw_zbuf_w * (size_t) s->sw_zbuf_h * sizeof(u16));
    s->frame_dirty = true;
}

/* -------------------------------------------------------------------------
 * TrueType font helpers (adapted from opengl-win32 / allegro-tinygl-dos)
 * ---------------------------------------------------------------------- */

static int ascii_tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static bool ci_contains(const char *haystack, const char *needle) {
    if (!needle || !*needle) return true;
    if (!haystack) return false;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++)
            if (ascii_tolower((unsigned char) haystack[i + j]) !=
                    ascii_tolower((unsigned char) needle[j]))
                break;
        if (j == nlen) return true;
    }
    return false;
}

/* Check if haystack starts with needle (case-insensitive).  Used for DOS
 * 8.3 filename matching where "JETBRA~1" should match "JetBrains". */
static bool ci_starts_with(const char *haystack, const char *needle) {
    if (!needle || !*needle) return true;
    if (!haystack) return false;
    size_t nl = strlen(needle);
    for (size_t i = 0; i < nl; i++) {
        if (haystack[i] == '\0') return false;
        if (ascii_tolower((unsigned char)haystack[i]) !=
            ascii_tolower((unsigned char)needle[i]))
            return false;
    }
    return true;
}

/* Check if a stored face name matches a requested font name, handling DOS
 * 8.3 truncation (e.g. JETBRA~1 matches "JetBrains"). */
static bool face_name_matches(const char *stored, const char *needle) {
    if (ci_contains(stored, needle)) return true;
    const char *tilde = strchr(stored, '~');
    if (tilde) {
        /* DOS 8.3 truncation: "MATERI~1" should match "MaterialIconsOutlined"
         * because the prefix before ~ matches the start of the needle. */
        if (ci_starts_with(stored, needle)) return true;
        size_t prefix_len = (size_t)(tilde - stored);
        if (prefix_len > 0) {
            bool match = true;
            for (size_t i = 0; i < prefix_len; i++) {
                if (needle[i] == '\0') { match = false; break; }
                if (ascii_tolower((unsigned char)stored[i]) !=
                    ascii_tolower((unsigned char)needle[i])) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
    }
    return false;
}

static int has_ttf_in_dir(const char *d) {
    struct al_ffblk tmp;
    char pat[256];
    const char *exts[] = { "*.fnt", "*.ttf", "*.otf" };
    for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
        if (d && *d) snprintf(pat, sizeof(pat), "%s/%s", d, exts[e]);
        else snprintf(pat, sizeof(pat), "%s", exts[e]);
        if (al_findfirst(pat, &tmp, FA_ARCH | FA_RDONLY | FA_HIDDEN | FA_SYSTEM) == 0) {
            al_findclose(&tmp);
            return 1;
        }
    }
    return 0;
}

/* Return 1 and fill out[] with a directory containing .ttf/.fnt files.
 * Returns empty string for current directory (no ./ prefix).
 * Search order: TIGRYX_ARTIFACTS env, "fonts" and "assets" subdirs of CWD,
 * CWD itself, then "fonts" and "assets" subdirs of the executable's dir,
 * and finally the executable's dir itself. The "assets" paths cover the
 * installed layout where the payload.zip extracts fonts into assets/. */
static int find_fonts_dir(char *out, int out_sz) {
    const char *env = getenv("TIGRYX_ARTIFACTS");
    if (env && *env) {
        snprintf(out, out_sz, "%s/fonts", env);
        if (has_ttf_in_dir(out)) return 1;
        snprintf(out, out_sz, "%s/assets", env);
        if (has_ttf_in_dir(out)) return 1;
        snprintf(out, out_sz, "%s", env);
        if (has_ttf_in_dir(out)) return 1;
    }
    snprintf(out, out_sz, "fonts");
    if (has_ttf_in_dir(out)) return 1;
    snprintf(out, out_sz, "assets");
    if (has_ttf_in_dir(out)) return 1;
    if (has_ttf_in_dir("")) {
        out[0] = '\0';
        return 1;
    }
    char exe[512];
    get_executable_name(exe, sizeof(exe));
    char *last_slash = strrchr(exe, '/');
    if (!last_slash) last_slash = strrchr(exe, '\\');
    if (last_slash) {
        *last_slash = '\0';
        snprintf(out, out_sz, "%s/fonts", exe);
        if (has_ttf_in_dir(out)) return 1;
        snprintf(out, out_sz, "%s/assets", exe);
        if (has_ttf_in_dir(out)) return 1;
        snprintf(out, out_sz, "%s", exe);
        if (has_ttf_in_dir(out)) return 1;
    }
    out[0] = '\0';
    return 0;
}

/* Register a font face without loading its (potentially multi-MB) atlas.
 * Only lightweight metadata is recorded here; the heavy atlas/glyph data is
 * loaded on demand by load_font_face_data() the first time the face is used.
 * This keeps idle fonts out of memory — important on DOS, where eagerly
 * baking/loading every face can exhaust DPMI memory. */
static int register_font_face(AllegroState *s, const char *path, const char *basename) {
    if (s->nfaces >= FONT_MAX_FACES) return 0;

    /* On DOS the on-disk filename may be 8.3-truncated (e.g. JETBRA~1.FNT),
     * which loses the weight suffix and makes it impossible to tell
     * JetBrainsMono-Regular from JetBrainsMono-ExtraBold.  The .fnt cache
     * header embeds the original long name, so read it and prefer it for
     * naming and bold detection.  This works even on bare DOS/DOSBox where
     * LFN is unavailable. */
    char longname[64];
    const char *effective = basename;
    size_t plen = strlen(path);
    if (plen >= 4 &&
        ascii_tolower((unsigned char)path[plen - 4]) == '.' &&
        ascii_tolower((unsigned char)path[plen - 3]) == 'f' &&
        ascii_tolower((unsigned char)path[plen - 2]) == 'n' &&
        ascii_tolower((unsigned char)path[plen - 1]) == 't') {
        FILE *hf = fopen(path, "rb");
        if (hf) {
            RelicFontCacheHeader h;
            if (relic_font_cache_read_header(hf, &h) &&
                memcmp(h.magic, RELIC_FONT_CACHE_MAGIC, 4) == 0 &&
                h.name[0]) {
                snprintf(longname, sizeof(longname), "%s", h.name);
                effective = longname;
            }
            fclose(hf);
        }
    }

    char name[sizeof(s->faces[0].name)];
    relic_font_make_name(effective, name, sizeof(name));

    /* Skip if this face is already registered (e.g. both .ttf and .fnt exist). */
    for (int i = 0; i < s->nfaces; i++) {
        if (strcmp(s->faces[i].name, name) == 0) return 0;
    }

    int idx = s->nfaces;
    memset(&s->faces[idx], 0, sizeof(s->faces[idx]));
    s->faces[idx].buffer = NULL;
    s->faces[idx].init = false;   /* data not loaded yet */
    snprintf(s->faces[idx].name, sizeof(s->faces[idx].name), "%s", name);
    snprintf(s->faces[idx].path, sizeof(s->faces[idx].path), "%s", path);
    s->faces[idx].is_bold = ci_contains(effective, "Bold");
    s->faces[idx].is_pixel_font = relic_font_is_pixel_font(name) != 0;
    s->nfaces++;
    return 1;
}

/* Register a font face from a packed asset memory buffer (no file path needed).
 * Only .ttf files are supported from memory (no .fnt cache path lookup). */
static int register_font_face_mem(AllegroState *s, const char *basename,
                                   const unsigned char *data, u32 size) {
    if (s->nfaces >= FONT_MAX_FACES) return 0;

    char name[sizeof(s->faces[0].name)];
    relic_font_make_name(basename, name, sizeof(name));

    for (int i = 0; i < s->nfaces; i++) {
        if (strcmp(s->faces[i].name, name) == 0) return 0;
    }

    int idx = s->nfaces;
    memset(&s->faces[idx], 0, sizeof(s->faces[idx]));
    s->faces[idx].buffer = NULL;
    s->faces[idx].init = false;
    snprintf(s->faces[idx].name, sizeof(s->faces[idx].name), "%s", name);
    s->faces[idx].path[0] = '\0';
    s->faces[idx].mem_data = data;
    s->faces[idx].mem_size = size;
    s->faces[idx].is_bold = ci_contains(basename, "Bold");
    s->faces[idx].is_pixel_font = relic_font_is_pixel_font(name) != 0;
    s->nfaces++;
    return 1;
}

/* Load the atlas + glyph data for a previously-registered face.  Returns 1 if
 * the face is ready to use (already loaded or loaded now), 0 on failure. */
static int load_font_face_data(AllegroState *s, int idx) {
    if (idx < 0 || idx >= s->nfaces) return 0;
    if (s->faces[idx].init) return 1;  /* already loaded */

    const char *path = s->faces[idx].path;
    const char *basename = s->faces[idx].name;

    int fc, gc, num_sizes, sdf_padding, sdf_onedge;
    float bp[FONT_MAX_BAKE_SIZES];
    int as[FONT_MAX_BAKE_SIZES];
    relic_font_params_for(basename, &fc, &gc, &num_sizes, bp, as,
                          &sdf_padding, &sdf_onedge);
    if (num_sizes <= 0 || num_sizes > FONT_MAX_BAKE_SIZES) return 0;

    if (g_console_log && g_vm && g_vm->verbose) {
        fprintf(g_console_log, "DOS: loading font face '%s' from '%s'\n", basename,
                s->faces[idx].mem_data ? "<packed>" : path);
        fflush(g_console_log);
    }

    /* If this face has packed asset data, skip the .fnt cache and bake from
     * the in-memory .ttf buffer directly. */
    if (s->faces[idx].mem_data) {
#ifdef RELIC_NO_TTF_BAKE
        /* No live baking on this platform, and packed in-memory assets have
         * no .fnt-cache equivalent to fall back to (see the comment on
         * register_font_face_mem) -- so a memory-packed .ttf simply cannot
         * be used here. Ship this font as a .fnt on disk instead. */
        if (g_console_log && g_vm && g_vm->verbose) {
            fprintf(g_console_log,
                    "DOS: font '%s' is a packed in-memory .ttf, but this build "
                    "has live TTF baking disabled (RELIC_NO_TTF_BAKE) -- ship "
                    "it as a .fnt file on disk instead\n", basename);
            fflush(g_console_log);
        }
        return 0;
#else
        unsigned char *buf = (unsigned char *)s->faces[idx].mem_data;

        if (!stbtt_InitFont(&s->faces[idx].info, buf, stbtt_GetFontOffsetForIndex(buf, 0)))
            return 0;

        for (int i = 0; i < num_sizes; i++) {
            s->faces[idx].atlas[i] = (unsigned char *)malloc((size_t)as[i] * (size_t)as[i]);
            s->faces[idx].cdata[i] = (stbtt_bakedchar *)calloc((size_t)gc, sizeof(stbtt_bakedchar));
            if (!s->faces[idx].atlas[i] || !s->faces[idx].cdata[i]) return 0;
            float asc, dsc;
            if (sdf_padding > 0) {
                if (!relic_font_bake_sdf(&s->faces[idx].info, fc, gc, bp[i], as[i],
                                         sdf_padding, sdf_onedge,
                                         s->faces[idx].atlas[i], s->faces[idx].cdata[i], &asc, &dsc))
                    return 0;
            } else {
                if (!relic_font_bake_bitmap(&s->faces[idx].info, fc, gc, bp[i], as[i],
                                            s->faces[idx].atlas[i], s->faces[idx].cdata[i], &asc, &dsc))
                    return 0;
            }
            s->faces[idx].bake_pixels[i] = bp[i];
            s->faces[idx].atlas_size[i] = as[i];
            if (i == 0) { s->faces[idx].ascent_px = asc; s->faces[idx].descent_px = dsc; }
        }

        s->faces[idx].buffer = NULL;  /* don't free packed asset memory */
        s->faces[idx].first_char = fc;
        s->faces[idx].glyph_count = gc;
        s->faces[idx].num_bake_sizes = num_sizes;
        s->faces[idx].sdf_padding = sdf_padding;
        s->faces[idx].init = true;
        return 1;
#endif /* RELIC_NO_TTF_BAKE */
    }

    /* Try the pre-baked cache first.  If the .fnt file exists and its header
     * looks compatible, load the atlas and glyph metadata directly.  This
     * avoids the slow SDF bake and keeps the .ttf file out of memory. */
    char cache_path[512];
    size_t path_len = strlen(path);
    if (path_len >= 4 && path_len + 1 <= sizeof(cache_path)) {
        memcpy(cache_path, path, path_len - 4);
        memcpy(cache_path + path_len - 4, ".fnt", 5);
    } else {
        snprintf(cache_path, sizeof(cache_path), "%s.fnt", path);
    }

    /* Plain-DOS installs carry the cache under an 8.3 stem (JBMONO-R.fnt):
     * try the mapped short name in the same directory first (canonical),
     * then the long name (legacy/dev caches). */
    char p83[512];
    relic_font_cache_path83(path, p83, sizeof p83);
    FILE *cf = fopen(p83, "rb");
    if (!cf && strcmp(p83, cache_path) != 0) cf = fopen(cache_path, "rb");
    if (cf) {
        RelicFontCacheHeader h;
        bool cache_ok = false;
        if (relic_font_cache_read_header(cf, &h)) {
            /* NOTE: the cache's OWN bake-size list is authoritative - we do
             * NOT require it to match the compile-time FONT_BAKE_PIXELS
             * table.  Everything downstream (select_bake_size, the scale
             * calculation) reads bake_pixels[]/atlas_size[] off the face, so
             * a cache baked with a different size list renders correctly; it
             * just may not have the ideal size for every request.  An exact
             * match must not be required: on DOS the fallback path cannot
             * recover (the registered path is the .fnt itself, and no .ttf is
             * shipped), so a bake-table change would make ALL text silently
             * vanish. */
            if (memcmp(h.magic, RELIC_FONT_CACHE_MAGIC, 4) == 0 &&
                h.version == RELIC_FONT_CACHE_VERSION &&
                h.first_char == fc &&
                h.glyph_count == gc &&
                h.sdf_padding == sdf_padding &&
                h.sdf_onedge == sdf_onedge &&
                h.num_bake_sizes > 0 &&
                h.num_bake_sizes <= FONT_MAX_BAKE_SIZES) {
                int cache_sizes = h.num_bake_sizes;
                int read_ok = 1;
                for (int i = 0; i < cache_sizes && read_ok; i++) {
                    int a_sz = h.atlas_sizes[i];
                    if (a_sz <= 0 || h.bake_pixels[i] <= 0.0f) { read_ok = 0; break; }
                    s->faces[idx].atlas[i] = (unsigned char *)malloc((size_t)a_sz * (size_t)a_sz);
                    s->faces[idx].cdata[i] = (stbtt_bakedchar *)calloc((size_t)gc, sizeof(stbtt_bakedchar));
                    if (!s->faces[idx].atlas[i] || !s->faces[idx].cdata[i] ||
                        !relic_font_cache_read_atlas(cf, s->faces[idx].atlas[i], a_sz) ||
                        !relic_font_cache_read_cdata(cf, s->faces[idx].cdata[i], gc)) {
                        read_ok = 0;
                    } else {
                        s->faces[idx].bake_pixels[i] = h.bake_pixels[i];
                        s->faces[idx].atlas_size[i] = a_sz;
                    }
                }
                if (read_ok) {
                    cache_ok = true;
                    fclose(cf);
                    if (h.name[0])
                        snprintf(s->faces[idx].name, sizeof(s->faces[idx].name), "%s", h.name);
                    s->faces[idx].buffer = NULL;
                    s->faces[idx].first_char = fc;
                    s->faces[idx].glyph_count = gc;
                    s->faces[idx].num_bake_sizes = cache_sizes;
                    s->faces[idx].sdf_padding = h.sdf_padding;
                    s->faces[idx].ascent_px = h.ascent_px;
                    s->faces[idx].descent_px = h.descent_px;
                    s->faces[idx].init = true;
                    if (g_console_log && g_vm && g_vm->verbose && cache_sizes != num_sizes) {
                        fprintf(g_console_log,
                                "DOS: font '%s' cache has %d bake size(s), build expects %d "
                                "(stale cache - regenerate with font_cache_dos)\n",
                                basename, cache_sizes, num_sizes);
                        fflush(g_console_log);
                    }
                    return 1;
                }
                /* Partial read: drop whatever was allocated so the runtime
                 * bake below starts from a clean slate. */
                for (int i = 0; i < FONT_MAX_BAKE_SIZES; i++) {
                    free(s->faces[idx].atlas[i]); s->faces[idx].atlas[i] = NULL;
                    free(s->faces[idx].cdata[i]); s->faces[idx].cdata[i] = NULL;
                }
            }
        }
        if (!cache_ok) fclose(cf);
    }

#ifdef RELIC_NO_TTF_BAKE
    /* No usable .fnt cache, and this build has live baking disabled: fail
     * cleanly instead of falling back to a .ttf bake. Regenerate the cache
     * with font_cache_dos and ship the resulting .fnt alongside this font's
     * .ttf (font_cache_dos itself runs on the host, where baking is cheap;
     * only the frelic3d *runtime* is barred from it). */
    if (g_console_log && g_vm && g_vm->verbose) {
        fprintf(g_console_log,
                "DOS: font '%s' has no usable .fnt cache, and this build has "
                "live TTF baking disabled (RELIC_NO_TTF_BAKE) -- regenerate "
                "the cache with font_cache_dos\n", basename);
        fflush(g_console_log);
    }
    return 0;
#else
    /* No usable cache: bake at runtime from the source .ttf.  `path` may be
     * the .fnt (the font scan registers .fnt first and skips the duplicate
     * .ttf), and stbtt_InitFont on a .fnt fails - which would take the whole
     * face down.  Resolve the sibling .ttf explicitly instead. */
    char ttf_path[512];
    snprintf(ttf_path, sizeof(ttf_path), "%s", path);
    size_t tlen = strlen(ttf_path);
    if (tlen >= 4 &&
        ascii_tolower((unsigned char)ttf_path[tlen - 4]) == '.' &&
        ascii_tolower((unsigned char)ttf_path[tlen - 3]) == 'f' &&
        ascii_tolower((unsigned char)ttf_path[tlen - 2]) == 'n' &&
        ascii_tolower((unsigned char)ttf_path[tlen - 1]) == 't') {
        memcpy(ttf_path + tlen - 4, ".ttf", 5);
    }

    FILE *f = fopen(ttf_path, "rb");
    if (!f) {
        /* If `path` was an 8.3 cache name (JBMONO-R.fnt) its sibling .ttf
         * carries the font's full name (JetBrainsMono-Regular.ttf) - probe
         * that before failing. */
        char ttf_full[512];
        relic_font_ttf_sibling_path(path, ttf_full, sizeof ttf_full);
        if (strcmp(ttf_full, ttf_path) != 0) {
            f = fopen(ttf_full, "rb");
            if (f) snprintf(ttf_path, sizeof(ttf_path), "%s", ttf_full);
        }
    }
    if (!f) {
        if (g_console_log && g_vm && g_vm->verbose) {
            fprintf(g_console_log,
                    "DOS: font '%s' has no usable .fnt cache and no .ttf at '%s'\n",
                    basename, ttf_path);
            fflush(g_console_log);
        }
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return 0;
    }

    unsigned char *buf = (unsigned char *) malloc((size_t) size);
    if (!buf) {
        fclose(f);
        return 0;
    }
    if (fread(buf, 1, (size_t) size, f) != (size_t) size) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);

    if (!stbtt_InitFont(&s->faces[idx].info, buf, stbtt_GetFontOffsetForIndex(buf, 0))) {
        free(buf);
        return 0;
    }

    for (int i = 0; i < num_sizes; i++) {
        s->faces[idx].atlas[i] = (unsigned char *)malloc((size_t)as[i] * (size_t)as[i]);
        s->faces[idx].cdata[i] = (stbtt_bakedchar *)calloc((size_t)gc, sizeof(stbtt_bakedchar));
        if (!s->faces[idx].atlas[i] || !s->faces[idx].cdata[i]) return 0;
        float asc, dsc;
        if (sdf_padding > 0) {
            if (!relic_font_bake_sdf(&s->faces[idx].info, fc, gc, bp[i], as[i],
                                     sdf_padding, sdf_onedge,
                                     s->faces[idx].atlas[i], s->faces[idx].cdata[i], &asc, &dsc))
                return 0;
        } else {
            if (!relic_font_bake_bitmap(&s->faces[idx].info, fc, gc, bp[i], as[i],
                                        s->faces[idx].atlas[i], s->faces[idx].cdata[i], &asc, &dsc))
                return 0;
        }
        s->faces[idx].bake_pixels[i] = bp[i];
        s->faces[idx].atlas_size[i] = as[i];
        if (i == 0) { s->faces[idx].ascent_px = asc; s->faces[idx].descent_px = dsc; }
    }

    s->faces[idx].buffer = buf;
    s->faces[idx].first_char = fc;
    s->faces[idx].glyph_count = gc;
    s->faces[idx].num_bake_sizes = num_sizes;
    s->faces[idx].sdf_padding = sdf_padding;
    s->faces[idx].init = true;
    return 1;
#endif /* RELIC_NO_TTF_BAKE */
}

/* Pick the best matching registered face for (name, weight).  Returns the
 * face index (metadata only) or -1; does NOT load the atlas. */
static int match_face(AllegroState *s, const char *name, int weight) {
    if (s->nfaces <= 0) return -1;
    bool want_bold = (weight >= 600);
    const char *needle = (name && *name) ? name : NULL;
    if (!needle) {
        if (want_bold) {
            for (int i = 0; i < s->nfaces; i++) {
                if (face_name_matches(s->faces[i].name, "JetBrains") && s->faces[i].is_bold)
                    return i;
            }
        }
        return s->default_face;
    }
    int any_match = -1;
    for (int i = 0; i < s->nfaces; i++) {
        if (!face_name_matches(s->faces[i].name, needle)) continue;
        if (s->faces[i].is_bold == want_bold) return i;
        if (any_match < 0) any_match = i;
    }
    if (any_match >= 0) return any_match;
    return s->default_face;
}

/* Select a face and ensure its atlas/glyph data is loaded (lazy load on first
 * use).  Returns a ready-to-use face index, or -1 if nothing usable. */
static int select_face(AllegroState *s, const char *name, int weight) {
    int idx = match_face(s, name, weight);
    if (idx < 0) return -1;
    if (!load_font_face_data(s, idx)) {
        if (g_console_log && g_vm && g_vm->verbose) {
            fprintf(g_console_log, "DOS: failed to load font face '%s'\n", s->faces[idx].name);
            fflush(g_console_log);
        }
        return -1;
    }
    return idx;
}

static int utf8_next(const char **pp) {
    const unsigned char *p = (const unsigned char *)*pp;
    int c = *p;
    if (c == 0) return 0;
    int cp;
    if (c < 0x80) {
        cp = c; p++;
    } else if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        cp = ((c & 0x1F) << 6) | (p[1] & 0x3F); p += 2;
    } else if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
    } else if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        cp = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4;
    } else {
        p++; *pp = (const char *)p; return -1;
    }
    *pp = (const char *)p;
    return cp;
}

static void measure_text_baked(const stbtt_bakedchar *cdata, int fc, int gc,
        float pad, const char *text,
        float *out_w, float *out_above, float *out_below, float *out_min_x) {
    float cx = 0.0f, cy = 0.0f;
    float maxx = 0.0f;
    float min_x = 0.0f;
    float min_y0 = 0.0f, max_y1 = 0.0f;
    bool any = false;
    for (const char *p = text; *p; ) {
        int cp = utf8_next(&p);
        if (cp <= 0) continue;
        int gi = cp - fc;
        if (gi < 0 || gi >= gc) continue;
        const stbtt_bakedchar *b = &cdata[gi];
        float gw = (float)(b->x1 - b->x0);
        float gh = (float)(b->y1 - b->y0);
        float qx0 = cx + b->xoff + pad;
        float qy0 = cy + b->yoff + pad;
        float qx1 = qx0 + gw - 2.0f * pad;
        float qy1 = qy0 + gh - 2.0f * pad;
        cx += b->xadvance;
        if (gw <= 0.0f || gh <= 0.0f) continue;
        if (!any) { min_x = qx0; min_y0 = qy0; max_y1 = qy1; any = true; }
        else {
            if (qx0 < min_x) min_x = qx0;
            if (qy0 < min_y0) min_y0 = qy0;
            if (qy1 > max_y1) max_y1 = qy1;
        }
        if (qx1 > maxx) maxx = qx1;
    }
    if (!any) {
        *out_w = 0.0f;
        *out_above = 0.0f;
        *out_below = 0.0f;
        if (out_min_x) *out_min_x = 0.0f;
        return;
    }
    *out_w = maxx - min_x;
    *out_above = -min_y0;
    *out_below = max_y1;
    if (out_min_x) *out_min_x = min_x;
}

/* The bake's tallest ink extents: how far above/below the baseline the
 * deepest glyph in the atlas can reach. Unlike measure_text_baked this is
 * a property of the whole bake, not the string, so every run anchored on
 * it shares one baseline. Same pad-trimmed ink convention as
 * measure_text_baked. */
static void baked_cell_metrics(const stbtt_bakedchar *cdata, int gc,
        float pad, float *out_above, float *out_below) {
    float above = 0.0f, below = 0.0f;
    for (int i = 0; i < gc; i++) {
        float gh = (float)(cdata[i].y1 - cdata[i].y0);
        if (gh <= 0.0f) continue;
        float a = -(cdata[i].yoff + pad);
        float b = cdata[i].yoff + gh - pad;
        if (a > above) above = a;
        if (b > below) below = b;
    }
    *out_above = above;
    *out_below = below;
}

/* Nearest-neighbour sample a pre-rendered alpha bitmap glyph and alpha-blend
 * onto dst.  Bitmap fonts (sdf_padding == 0) are pixel art — the DOS VGA font
 * in particular — so bilinear interpolation blurs them into a fuzzy haze.
 * Nearest-neighbour preserves crisp whole-pixel edges. */
static void draw_glyph_bitmap(BITMAP *dst, int dst_x, int dst_y,
        int dst_w, int dst_h,
        const unsigned char *atlas, int atlas_size,
        int src_x, int src_y, int src_w, int src_h,
        int r, int g, int b, int text_a, int threshold) {
    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;
    const int x0 = src_x, y0 = src_y;
    const int x1 = src_x + src_w, y1 = src_y + src_h;
    for (int dy = 0; dy < dst_h; dy++) {
        int oy = dst_y + dy;
        if (oy < 0 || oy >= dst->h) continue;
        /* Nearest-neighbour: round to the closest source texel. */
        int syi = (int)((float)(dy + 0.5f) * (float)src_h / (float)dst_h);
        if (syi < 0) syi = 0;
        if (syi >= src_h) syi = src_h - 1;
        int sy0 = syi + src_y;
        if (sy0 < y0) sy0 = y0;
        if (sy0 >= y1) sy0 = y1 - 1;
        for (int dx = 0; dx < dst_w; dx++) {
            int ox = dst_x + dx;
            if (ox < 0 || ox >= dst->w) continue;
            int sxi = (int)((float)(dx + 0.5f) * (float)src_w / (float)dst_w);
            if (sxi < 0) sxi = 0;
            if (sxi >= src_w) sxi = src_w - 1;
            int sx0 = sxi + src_x;
            if (sx0 < x0) sx0 = x0;
            if (sx0 >= x1) sx0 = x1 - 1;

            int alpha_i = atlas[sy0 * atlas_size + sx0];
            /* Pixel-art fonts: collapse coverage to fully on/off.  The VGA
             * font is 1-bit by design, so any intermediate value is a bake
             * artefact rather than intentional shading. */
            if (threshold) alpha_i = (alpha_i >= 128) ? 255 : 0;
            if (alpha_i <= 0) continue;
            if (text_a != 255) alpha_i = (alpha_i * text_a) / 255;
            if (alpha_i <= 0) continue;
            if (alpha_i >= 255) {
                putpixel(dst, ox, oy, makecol(r, g, b));
            } else {
                int dst_col = getpixel(dst, ox, oy);
                int dr = getr(dst_col);
                int dg = getg(dst_col);
                int db = getb(dst_col);
                int out_r = (r * alpha_i + dr * (255 - alpha_i)) / 255;
                int out_g = (g * alpha_i + dg * (255 - alpha_i)) / 255;
                int out_b = (b * alpha_i + db * (255 - alpha_i)) / 255;
                putpixel(dst, ox, oy, makecol(out_r, out_g, out_b));
            }
        }
    }
}

/* Each destination pixel samples the SDF bilinearly and converts it to an
 * alpha value using the on-edge threshold plus a scale-dependent smoothing
 * range, so edges stay anti-aliased at any render size. */
static void draw_glyph_alpha(BITMAP *dst, int dst_x, int dst_y,
        int dst_w, int dst_h,
        const unsigned char *atlas, int atlas_size,
        int src_x, int src_y, int src_w, int src_h,
        int r, int g, int b, int text_a, float scale) {
    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;

    const float pixel_dist_scale = (float)FONT_SDF_ONEDGE_VALUE / (float)FONT_SDF_PADDING;
    float sdf_range = (scale > 0.0f) ? (pixel_dist_scale / scale) : pixel_dist_scale;
    if (sdf_range > 64.0f) sdf_range = 64.0f;
    const float edge = (float)FONT_SDF_ONEDGE_VALUE;

    const int x0 = src_x, y0 = src_y;
    const int x1 = src_x + src_w, y1 = src_y + src_h;
    for (int dy = 0; dy < dst_h; dy++) {
        int oy = dst_y + dy;
        if (oy < 0 || oy >= dst->h) continue;
        float sy = (float)(dy + 0.5f) * (float)src_h / (float)dst_h - 0.5f;
        int syi = (int)floorf(sy);
        float fy = sy - (float)syi;
        for (int dx = 0; dx < dst_w; dx++) {
            int ox = dst_x + dx;
            if (ox < 0 || ox >= dst->w) continue;
            float sx = (float)(dx + 0.5f) * (float)src_w / (float)dst_w - 0.5f;
            int sxi = (int)floorf(sx);
            float fx = sx - (float)sxi;

            int sx0 = sxi + src_x;
            int sy0 = syi + src_y;
            int sx1 = sx0 + 1;
            int sy1 = sy0 + 1;
            if (sx0 < x0) sx0 = x0;
            if (sx1 >= x1) sx1 = x1 - 1;
            if (sy0 < y0) sy0 = y0;
            if (sy1 >= y1) sy1 = y1 - 1;

            unsigned char a00 = atlas[sy0 * atlas_size + sx0];
            unsigned char a01 = atlas[sy0 * atlas_size + sx1];
            unsigned char a10 = atlas[sy1 * atlas_size + sx0];
            unsigned char a11 = atlas[sy1 * atlas_size + sx1];

            float a0 = (float)a00 + ((float)a01 - (float)a00) * fx;
            float a1 = (float)a10 + ((float)a11 - (float)a10) * fx;
            float sdf = a0 + (a1 - a0) * fy;

            float alpha = (sdf - edge) / (FONT_SDF_SMOOTHING_PX * sdf_range) + 0.5f;
            if (alpha <= 0.0f) continue;
            if (alpha >= 1.0f) alpha = 1.0f;
            int alpha_i = (int)(alpha * 255.0f + 0.5f);
            if (alpha_i <= 0) continue;
            if (text_a != 255) alpha_i = (alpha_i * text_a) / 255;
            if (alpha_i <= 0) continue;

            if (alpha_i >= 255) {
                putpixel(dst, ox, oy, makecol(r, g, b));
            } else {
                int dst_col = getpixel(dst, ox, oy);
                int dr = getr(dst_col);
                int dg = getg(dst_col);
                int db = getb(dst_col);
                int out_r = (r * alpha_i + dr * (255 - alpha_i)) / 255;
                int out_g = (g * alpha_i + dg * (255 - alpha_i)) / 255;
                int out_b = (b * alpha_i + db * (255 - alpha_i)) / 255;
                putpixel(dst, ox, oy, makecol(out_r, out_g, out_b));
            }
        }
    }
}

static int allegro_font_init(AllegroState *s) {
    if (s->font_init) return s->nfaces > 0;
    s->font_init = true;
    s->nfaces = 0;
    s->default_face = -1;

    char dir[512];
    if (!find_fonts_dir(dir, sizeof(dir))) {
        dir[0] = '\0';
        allegro_console_open_log();
        if (g_console_log && g_vm && g_vm->verbose) {
            fprintf(g_console_log, "DOS: find_fonts_dir failed, trying packed assets\n");
            fflush(g_console_log);
        }
    }

    allegro_console_open_log();
    if (g_console_log && g_vm && g_vm->verbose) {
        fprintf(g_console_log, "DOS: font dir='%s'\n", dir[0] ? dir : ".");
        fflush(g_console_log);
    }

    const char *sep = (dir[0] == '\0') ? "" : "/";

    /* Scan directory for .fnt, .ttf and .otf font files (.fnt first to prefer cache) */
    struct al_ffblk info;
    char pattern[512];
    const char *exts[] = { "*.fnt", "*.ttf", "*.otf" };
    for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
        if (dir[0])
            snprintf(pattern, sizeof(pattern), "%s/%s", dir, exts[e]);
        else
            snprintf(pattern, sizeof(pattern), "%s", exts[e]);

        int ret = al_findfirst(pattern, &info, FA_ARCH | FA_RDONLY | FA_HIDDEN | FA_SYSTEM);
        if (g_console_log && g_vm && g_vm->verbose) {
            fprintf(g_console_log, "DOS: al_findfirst('%s') ret=%d\n", pattern, ret);
            fflush(g_console_log);
        }
        if (ret == 0) {
            do {
                if (info.attrib & FA_DIREC) continue;
                char path[512];
                snprintf(path, sizeof(path), "%s%s%s", dir, sep, info.name);
                if (g_console_log && g_vm && g_vm->verbose) {
                    fprintf(g_console_log, "DOS: found font '%s'\n", path);
                    fflush(g_console_log);
                }
                if (!register_font_face(s, path, info.name)) {
                    if (g_console_log && g_vm && g_vm->verbose) {
                        fprintf(g_console_log, "DOS: skipped font %s\n", info.name);
                        fflush(g_console_log);
                    }
                }
            } while (al_findnext(&info) == 0);
            al_findclose(&info);
        }
    }

    /* Fallback: probe hardcoded font filenames in each candidate dir.
     *
     * Probe with fopen, not exists()/findfirst: findfirst needs a search
     * handle from the redirector — and on FreeDOS+VMware the HGFS
     * shared-folder driver leaks those across processes, so after enough
     * program runs directory enumeration returns "empty" everywhere
     * (including the has_ttf_in_dir checks above, which is why `dir` can
     * be empty here) while plain fopen still works. This fallback is the
     * only path that can still find fonts in that state. */
    if (s->nfaces == 0) {
        const char *env = getenv("TIGRYX_ARTIFACTS");
        char exe[512] = "";
        get_executable_name(exe, sizeof(exe));
        char *ls = strrchr(exe, '/');
        if (!ls) ls = strrchr(exe, '\\');
        if (ls) *ls = '\0';

        char envf[512], env_assets[512], exef[512], exe_assets[512];
        envf[0] = env_assets[0] = exef[0] = exe_assets[0] = 0;
        if (env && *env) {
            snprintf(envf, sizeof(envf), "%s/fonts", env);
            snprintf(env_assets, sizeof(env_assets), "%s/assets", env);
        }
        if (exe[0]) {
            snprintf(exef, sizeof(exef), "%s/fonts", exe);
            snprintf(exe_assets, sizeof(exe_assets), "%s/assets", exe);
        }
        const char *cands[] = {
                dir, envf, env_assets,
                "fonts", "assets",
                exef, exe_assets, exe, ""
        };
        const char *known[] = {
                "JBMONO-R.FNT",       /* JetBrainsMono-Regular 8.3 cache  */
                "JBMONO-X.FNT",       /* JetBrainsMono-ExtraBold 8.3 cache */
                "INTER-R.FNT",        /* Inter-Regular                   */
                "INTER-B.FNT",        /* Inter-Bold                      */
                "MATICONS.FNT",       /* MaterialIconsOutlined           */
                "ROBOTO-R.FNT",       /* Roboto-Regular                  */
                "ROBOTO-B.FNT",       /* Roboto-Bold                     */
                "VGA8X16.FNT",        /* PxPlus IBM VGA8                 */
                "JetBrainsMono-Regular.fnt",
                "JetBrainsMono-Regular.ttf",
                "JetBrainsMono-ExtraBold.fnt",
                "JetBrainsMono-ExtraBold.ttf",
                "Roboto-Regular.fnt",
                "Roboto-Regular.ttf",
                "Roboto-Bold.fnt",
                "Roboto-Bold.ttf",
        };
        for (size_t d = 0; d < sizeof(cands) / sizeof(cands[0]); d++) {
            const char *cd = cands[d];
            const char *csep = (cd && cd[0]) ? "/" : "";
            for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
                char path[512];
                snprintf(path, sizeof(path), "%s%s%s",
                         cd ? cd : "", csep, known[i]);
                FILE *probe = fopen(path, "rb");
                if (g_console_log && g_vm && g_vm->verbose) {
                    fprintf(g_console_log, "DOS: fallback try '%s' open=%d\n",
                            path, probe != NULL);
                    fflush(g_console_log);
                }
                if (probe) {
                    fclose(probe);
                    register_font_face(s, path, known[i]);
                }
            }
            if (s->nfaces > 0) break;
        }
    }

    if (s->nfaces == 0) {
        /* Fallback: extract .ttf fonts from packed VM assets. */
        if (g_vm && g_vm->assets) {
            RelicAssets *a = g_vm->assets;
            for (int i = 0; i < a->count && s->nfaces < FONT_MAX_FACES; i++) {
                const char *n = a->entries[i].name;
                size_t nl = strlen(n);
                if (nl < 4) continue;
                const char *ext = n + nl - 4;
                if (ext[0] != '.') continue;
                /* Check .ttf/.otf extension (case-insensitive) */
                if (!((ascii_tolower((unsigned char)ext[1]) == 't' &&
                       ascii_tolower((unsigned char)ext[2]) == 't' &&
                       ascii_tolower((unsigned char)ext[3]) == 'f') ||
                      (ascii_tolower((unsigned char)ext[1]) == 'o' &&
                       ascii_tolower((unsigned char)ext[2]) == 't' &&
                       ascii_tolower((unsigned char)ext[3]) == 'f')))
                    continue;
                const char *bn = strrchr(n, '/');
                if (!bn) bn = strrchr(n, '\\');
                bn = bn ? bn + 1 : n;
                register_font_face_mem(s, bn,
                    a->data + a->entries[i].blob_offset,
                    a->entries[i].blob_size);
            }
            if (g_console_log && g_vm && g_vm->verbose && s->nfaces > 0) {
                fprintf(g_console_log, "DOS: loaded %d font(s) from packed assets\n", s->nfaces);
                fflush(g_console_log);
            }
        }
    }

    if (s->nfaces == 0) {
        if (g_console_log && g_vm && g_vm->verbose) {
            fprintf(g_console_log, "DOS: no fonts loaded from '%s'\n",
                    dir[0] ? dir : ".");
            fflush(g_console_log);
        }
        return 0;
    }

    if (g_console_log && g_vm && g_vm->verbose) {
        fprintf(g_console_log, "DOS: registered %d font face(s) from '%s' (loaded on demand)\n",
                s->nfaces, dir[0] ? dir : ".");
        for (int i = 0; i < s->nfaces; i++) {
            fprintf(g_console_log, "  face[%d]: %s bold=%d\n",
                    i, s->faces[i].name, s->faces[i].is_bold);
        }
        fflush(g_console_log);
    }

    for (int i = 0; i < s->nfaces; i++) {
        if (face_name_matches(s->faces[i].name, "JetBrains") && !s->faces[i].is_bold) {
            s->default_face = i;
            break;
        }
    }
    if (s->default_face < 0) {
        for (int i = 0; i < s->nfaces; i++) {
            if (face_name_matches(s->faces[i].name, "JetBrains")) {
                s->default_face = i;
                break;
            }
        }
    }
    if (s->default_face < 0) s->default_face = 0;
    return 1;
}

static void allegro_font_shutdown(AllegroState *s) {
    if (!s) return;
    for (int i = 0; i < s->nfaces; i++) {
        if (s->faces[i].buffer) {
            free(s->faces[i].buffer);
            s->faces[i].buffer = NULL;
        }
        for (int j = 0; j < FONT_MAX_BAKE_SIZES; j++) {
            if (s->faces[i].atlas[j]) {
                free(s->faces[i].atlas[j]);
                s->faces[i].atlas[j] = NULL;
            }
            if (s->faces[i].cdata[j]) {
                free(s->faces[i].cdata[j]);
                s->faces[i].cdata[j] = NULL;
            }
        }
        s->faces[i].init = false;
    }
    s->nfaces = 0;
    s->default_face = -1;
    s->font_init = false;
}

/* @sprite(x, y, tex_id, src_x, src_y, src_w, src_h, dst_w, dst_h, angle, &tint)
 * Draw a textured 2D sprite.  tex_id is a slot returned by @texture.
 * src_w<=0 or src_h<=0 means "use full texture size".
 * dst_w<=0 or dst_h<=0 means "same as src_w/src_h" (legacy 9-arg behaviour).
 * angle is in radians, rotated about the sprite centre.
 * tint is a 4-byte RGBA in VM memory; white (255,255,255,255) = no tint. */
static void allegro_gfx_sprite(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s || !screen) return;

    i32 x = vm->r[1];
    i32 y = vm->r[2];
    i32 slot = vm->r[3];
    i32 src_x = vm->r[4];
    i32 src_y = vm->r[5];
    i32 src_w = vm->r[6];
    i32 src_h = vm->r[7];
    i32 dst_w = vm->r[8];
    i32 dst_h = vm->r[9];
    float angle = ((AllegroFBits) {.i = vm->r[10]}).f;
    u32 rgba_ptr = (u32) vm->r[11];

    if (slot < 1 || slot >= s->tex_cap) return;
    SwTexture *tex = &s->textures[slot];
    if (!tex || !tex->pixels) return;

    int tex_w = tex->src_w;
    int tex_h = tex->src_h;
    if (tex_w <= 0 || tex_h <= 0) return;
    if (src_w <= 0) src_w = tex_w;
    if (src_h <= 0) src_h = tex_h;
    if (dst_w <= 0) dst_w = src_w;
    if (dst_h <= 0) dst_h = src_h;

    /* Tint */
    int tr = 255, tg = 255, tb = 255, ta = 255;
    if (rgba_ptr + 4 <= vm->mem_sz) {
        tr = vm->mem[rgba_ptr];
        tg = vm->mem[rgba_ptr + 1];
        tb = vm->mem[rgba_ptr + 2];
        ta = vm->mem[rgba_ptr + 3];
    }
    int tint_on = (tr < 255 || tg < 255 || tb < 255 || ta < 255);

    int depth = bitmap_color_depth(screen);
    int dst_x0 = x * s->ss + s->offset_x;
    int dst_y0 = y * s->ss + s->offset_y;
    int sw_ss = dst_w * s->ss;   /* destination width in supersampled pixels */
    int sh_ss = dst_h * s->ss;   /* destination height in supersampled pixels */
    BITMAP *dst = DRAW_TGT(s);

    if (angle == 0.0f) {
        /* Fast path: axis-aligned blit */
        int akey = tex->has_alpha;
        /* Ultra-fast path: native Allegro blit()/draw_sprite() from cached
         * BITMAP. Only when no tint and the cached BITMAP exists.
         *
         * For opaque textures (no alpha), blit() is used — a straight
         * memcpy-class operation.
         *
         * For textures with alpha, draw_sprite()/masked_blit() is used
         * instead. These skip pixels matching the bitmap mask color
         * (palette index 0 in 8bpp, written for transparent texels at
         * upload time) and use Allegro's optimized assembly routines
         * rather than per-pixel putpixel() which is catastrophically slow
         * on VESA banked framebuffer modes. */
        if (!tint_on && tex->bmp) {
            if (akey) {
                if (s->ss > 1) {
                    stretch_sprite(dst, tex->bmp, dst_x0, dst_y0, sw_ss, sh_ss);
                } else {
                    draw_sprite(dst, tex->bmp, dst_x0, dst_y0);
                }
            } else {
                if (s->ss > 1) {
                    stretch_blit(tex->bmp, dst, src_x, src_y, src_w, src_h,
                                 dst_x0, dst_y0, sw_ss, sh_ss);
                } else {
                    blit(tex->bmp, dst, src_x, src_y, dst_x0, dst_y0, src_w, src_h);
                }
            }
        } else {
            const u8 *tpx = tex->pixels;
            int tpitch = tex->w;
            for (int py = 0; py < sh_ss; py++) {
                int sy = src_y + (py * src_h) / (sh_ss > 0 ? sh_ss : 1);
                if (sy < 0 || sy >= tex_h) continue;
                int row_off = sy * tpitch;
                for (int px = 0; px < sw_ss; px++) {
                    int sx = src_x + (px * src_w) / (sw_ss > 0 ? sw_ss : 1);
                    if (sx < 0 || sx >= tex_w) continue;
                    const u8 *p = tpx + ((row_off + sx) << 2);
                    if (akey && p[3] == 0) continue;
                    int rr = p[0], gg = p[1], bb = p[2];
                    if (tint_on) {
                        rr = (rr * tr) >> 8;
                        gg = (gg * tg) >> 8;
                        bb = (bb * tb) >> 8;
                    }
                    if (akey && p[3] < 255) {
                        /* Alpha-blend with destination for anti-aliased edges. */
                        int bgp = getpixel(dst, dst_x0 + px, dst_y0 + py);
                        int br_, bg_, bb_;
                        if (depth == 8) {
                            br_ = getr_depth(depth, bgp) * 255 / 63;
                            bg_ = getg_depth(depth, bgp) * 255 / 63;
                            bb_ = getb_depth(depth, bgp) * 255 / 63;
                        } else {
                            br_ = getr_depth(depth, bgp);
                            bg_ = getg_depth(depth, bgp);
                            bb_ = getb_depth(depth, bgp);
                        }
                        int a = p[3];
                        rr = (rr * a + br_ * (255 - a)) / 255;
                        gg = (gg * a + bg_ * (255 - a)) / 255;
                        bb = (bb * a + bb_ * (255 - a)) / 255;
                    }
                    putpixel(dst, dst_x0 + px, dst_y0 + py,
                            makecol_depth(depth, rr, gg, bb));
                }
            }
        }
    } else {
        /* Rotated path: inverse rotation per pixel inside AABB.
         * dhx/dhy are half-extents in supersampled destination space;
         * hx/hy are the corresponding destination-space half-extents. */
        float cosA = cosf(angle);
        float sinA = sinf(angle);
        float ss_f = (float) s->ss;
        float hx = dst_w * 0.5f;
        float hy = dst_h * 0.5f;
        float dhx = hx * ss_f;
        float dhy = hy * ss_f;

        float rx[4] = {-dhx, dhx, dhx, -dhx};
        float ry[4] = {-dhy, -dhy, dhy, dhy};
        float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
        for (int i = 0; i < 4; i++) {
            float rrx = rx[i] * cosA - ry[i] * sinA;
            float rry = rx[i] * sinA + ry[i] * cosA;
            if (rrx < min_x) min_x = rrx;
            if (rrx > max_x) max_x = rrx;
            if (rry < min_y) min_y = rry;
            if (rry > max_y) max_y = rry;
        }

        int bb_x0 = (int) (dst_x0 + dhx + min_x);
        int bb_y0 = (int) (dst_y0 + dhy + min_y);
        int bb_x1 = (int) (dst_x0 + dhx + max_x + 1.0f);
        int bb_y1 = (int) (dst_y0 + dhy + max_y + 1.0f);

        int scr_w = dst->w;
        int scr_h = dst->h;
        if (bb_x0 < 0) bb_x0 = 0;
        if (bb_y0 < 0) bb_y0 = 0;
        if (bb_x1 > scr_w) bb_x1 = scr_w;
        if (bb_y1 > scr_h) bb_y1 = scr_h;

        const u8 *tpx = tex->pixels;
        int tpitch = tex->w;
        int akey = tex->has_alpha;
        for (int py = bb_y0; py < bb_y1; py++) {
            for (int px = bb_x0; px < bb_x1; px++) {
                float dx = (float) (px - dst_x0) - dhx;
                float dy = (float) (py - dst_y0) - dhy;
                /* Convert from supersampled dest space to unsupersampled
                 * dest space, rotate back to axis-aligned, then scale into
                 * source-crop space (dst_w/dst_h may differ from src_w/src_h). */
                float sdx = dx / ss_f;
                float sdy = dy / ss_f;
                float rdx = sdx * cosA + sdy * sinA;
                float rdy = -sdx * sinA + sdy * cosA;
                int sx = src_x + (int) ((hx + rdx) * (float)src_w / (float)dst_w);
                int sy = src_y + (int) ((hy + rdy) * (float)src_h / (float)dst_h);
                if (sx < 0 || sy < 0 || sx >= tex_w || sy >= tex_h) continue;
                const u8 *p = tpx + (((sy * tpitch) + sx) << 2);
                if (akey && p[3] == 0) continue;
                int rr = p[0], gg = p[1], bb_ = p[2];
                if (tint_on) {
                    rr = (rr * tr) >> 8;
                    gg = (gg * tg) >> 8;
                    bb_ = (bb_ * tb) >> 8;
                }
                if (akey && p[3] < 255) {
                    int bgp = getpixel(dst, px, py);
                    int br2, bg2, bb2;
                    if (depth == 8) {
                        br2 = getr_depth(depth, bgp) * 255 / 63;
                        bg2 = getg_depth(depth, bgp) * 255 / 63;
                        bb2 = getb_depth(depth, bgp) * 255 / 63;
                    } else {
                        br2 = getr_depth(depth, bgp);
                        bg2 = getg_depth(depth, bgp);
                        bb2 = getb_depth(depth, bgp);
                    }
                    int a = p[3];
                    rr = (rr * a + br2 * (255 - a)) / 255;
                    gg = (gg * a + bg2 * (255 - a)) / 255;
                    bb_ = (bb_ * a + bb2 * (255 - a)) / 255;
                }
                putpixel(dst, px, py, makecol_depth(depth, rr, gg, bb_));
            }
        }
    }
    s->frame_dirty = true;
}

/* @plot(x, y, rgba_ptr) — set a single pixel. */
static void allegro_gfx_plot(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s || !screen) return;

    int x = vm->r[1];
    int y = vm->r[2];
    u32 rgba_ptr = (u32) vm->r[3];

    int r = 255, g = 255, b = 255;
    if (rgba_ptr + 4 <= vm->mem_sz) {
        r = vm->mem[rgba_ptr];
        g = vm->mem[rgba_ptr + 1];
        b = vm->mem[rgba_ptr + 2];
    }

    putpixel(DRAW_TGT(s), x * s->ss + s->offset_x, y * s->ss + s->offset_y, makecol(r, g, b));
    s->frame_dirty = true;
}

/* Set 3D viewport. w<=0 or h<=0 resets (disables clipping, full-screen 3D). */
static void allegro_gfx_viewport(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) return;

    i32 x = vm->r[1];
    i32 y = vm->r[2];
    i32 w = vm->r[3];
    i32 h = vm->r[4];

    /* w<=0 or h<=0 resets the viewport (disables clipping, full-screen 3D) */
    if (w <= 0 || h <= 0) {
        s->vp_3d_x = 0;
        s->vp_3d_y = 0;
        s->vp_3d_w = 0;
        s->vp_3d_h = 0;
        return;
    }

    /* Clamp to logical window bounds */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > s->logical_w) w = s->logical_w - x;
    if (y + h > s->logical_h) h = s->logical_h - y;

    s->vp_3d_x = x;
    s->vp_3d_y = y;
    s->vp_3d_w = w;
    s->vp_3d_h = h;
    s->frame_dirty = true;
}

/* Box-filter 2x2 downsample from src (2*dw x 2*dh) into dst (dw x dh).
 * Averages four source pixels per destination pixel for anti-aliased output.
 * Uses direct memory access for 15/16/32 bpp (fast path); falls back to
 * getpixel/putpixel for 24bpp.  Called only when ss==2. */
static void ssaa_downsample_2x(BITMAP *src, BITMAP *dst, int depth) {
    int dw = dst->w, dh = dst->h;

    if (depth == 16) {
        /* 5-6-5 packed RGB: extract, average, repack. */
        for (int dy = 0; dy < dh; dy++) {
            int sy0 = dy * 2, sy1 = sy0 + 1;
            if (sy1 >= src->h) sy1 = sy0;
            u16 *s0a = (u16 *)src->line[sy0];
            u16 *s0b = (u16 *)src->line[sy0] + 1;
            u16 *s1a = (u16 *)src->line[sy1];
            u16 *s1b = (u16 *)src->line[sy1] + 1;
            u16 *drow = (u16 *)dst->line[dy];
            for (int dx = 0; dx < dw; dx++) {
                int sx0 = dx * 2;
                u16 c0 = s0a[sx0], c1 = s0b[sx0], c2 = s1a[sx0], c3 = s1b[sx0];
                int r = ((c0 >> 11) & 0x1F) + ((c1 >> 11) & 0x1F) +
                        ((c2 >> 11) & 0x1F) + ((c3 >> 11) & 0x1F);
                int g = ((c0 >> 5) & 0x3F) + ((c1 >> 5) & 0x3F) +
                        ((c2 >> 5) & 0x3F) + ((c3 >> 5) & 0x3F);
                int b = (c0 & 0x1F) + (c1 & 0x1F) + (c2 & 0x1F) + (c3 & 0x1F);
                drow[dx] = (u16)(((r >> 2) << 11) | ((g >> 2) << 5) | (b >> 2));
            }
        }
    } else if (depth == 15) {
        /* 5-5-5 packed RGB. */
        for (int dy = 0; dy < dh; dy++) {
            int sy0 = dy * 2, sy1 = sy0 + 1;
            if (sy1 >= src->h) sy1 = sy0;
            u16 *s0a = (u16 *)src->line[sy0];
            u16 *s0b = (u16 *)src->line[sy0] + 1;
            u16 *s1a = (u16 *)src->line[sy1];
            u16 *s1b = (u16 *)src->line[sy1] + 1;
            u16 *drow = (u16 *)dst->line[dy];
            for (int dx = 0; dx < dw; dx++) {
                int sx0 = dx * 2;
                u16 c0 = s0a[sx0], c1 = s0b[sx0], c2 = s1a[sx0], c3 = s1b[sx0];
                int r = ((c0 >> 10) & 0x1F) + ((c1 >> 10) & 0x1F) +
                        ((c2 >> 10) & 0x1F) + ((c3 >> 10) & 0x1F);
                int g = ((c0 >> 5) & 0x1F) + ((c1 >> 5) & 0x1F) +
                        ((c2 >> 5) & 0x1F) + ((c3 >> 5) & 0x1F);
                int b = (c0 & 0x1F) + (c1 & 0x1F) + (c2 & 0x1F) + (c3 & 0x1F);
                drow[dx] = (u16)(((r >> 2) << 10) | ((g >> 2) << 5) | (b >> 2));
            }
        }
    } else if (depth == 32) {
        /* 8-8-8-8 packed RGBA. */
        for (int dy = 0; dy < dh; dy++) {
            int sy0 = dy * 2, sy1 = sy0 + 1;
            if (sy1 >= src->h) sy1 = sy0;
            u32 *s0a = (u32 *)src->line[sy0];
            u32 *s0b = (u32 *)src->line[sy0] + 1;
            u32 *s1a = (u32 *)src->line[sy1];
            u32 *s1b = (u32 *)src->line[sy1] + 1;
            u32 *drow = (u32 *)dst->line[dy];
            for (int dx = 0; dx < dw; dx++) {
                int sx0 = dx * 2;
                u32 c0 = s0a[sx0], c1 = s0b[sx0], c2 = s1a[sx0], c3 = s1b[sx0];
                int r = ((c0 >> 16) & 0xFF) + ((c1 >> 16) & 0xFF) +
                        ((c2 >> 16) & 0xFF) + ((c3 >> 16) & 0xFF);
                int g = ((c0 >> 8) & 0xFF) + ((c1 >> 8) & 0xFF) +
                        ((c2 >> 8) & 0xFF) + ((c3 >> 8) & 0xFF);
                int b = (c0 & 0xFF) + (c1 & 0xFF) + (c2 & 0xFF) + (c3 & 0xFF);
                int a = ((c0 >> 24) & 0xFF) + ((c1 >> 24) & 0xFF) +
                        ((c2 >> 24) & 0xFF) + ((c3 >> 24) & 0xFF);
                drow[dx] = ((u32)(a >> 2) << 24) | ((u32)(r >> 2) << 16) |
                           ((u32)(g >> 2) << 8) | (u32)(b >> 2);
            }
        }
    } else {
        /* 24bpp fallback: use slow getpixel/putpixel. */
        for (int dy = 0; dy < dh; dy++) {
            int sy0 = dy * 2;
            int sy1 = sy0 + 1;
            if (sy1 >= src->h) sy1 = sy0;
            for (int dx = 0; dx < dw; dx++) {
                int sx0 = dx * 2;
                int sx1 = sx0 + 1;
                if (sx1 >= src->w) sx1 = sx0;
                int c0 = getpixel(src, sx0, sy0);
                int c1 = getpixel(src, sx1, sy0);
                int c2 = getpixel(src, sx0, sy1);
                int c3 = getpixel(src, sx1, sy1);
                int r = (getr_depth(depth, c0) + getr_depth(depth, c1) +
                         getr_depth(depth, c2) + getr_depth(depth, c3)) >> 2;
                int g = (getg_depth(depth, c0) + getg_depth(depth, c1) +
                         getg_depth(depth, c2) + getg_depth(depth, c3)) >> 2;
                int b = (getb_depth(depth, c0) + getb_depth(depth, c1) +
                         getb_depth(depth, c2) + getb_depth(depth, c3)) >> 2;
                putpixel(dst, dx, dy, makecol_depth(depth, r, g, b));
            }
        }
    }
}

static void allegro_gfx_flip(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) return;

    if (g_relic_gfx_prof) g_prof.frames++;

    /* Keep the real-time FM music stream fed. Runs every frame, including
     * idle frames that skip the blit below, so the OPL2 synth never
     * underruns. */
    mus_stream_poll();

    /* Current cursor top-left in physical pixels. masked_blit clips the
     * sprite against the target, so offscreen positions are safe. */
    if (s->have_mouse && mouse_needs_poll()) poll_mouse();
    int cur_tx = mouse_x - mouse_x_focus;
    int cur_ty = mouse_y - mouse_y_focus;
    /* A mouse move alone can't wake a page-flip frame (there is no blit to
     * hide it behind — compositing onto the displayed page would bake the
     * sprite into it), so it only applies to the blit path. */
    int cursor_moved = s->have_mouse && s->gfx_mode_set && !s->page_flip &&
                       !s->direct_draw &&
                       (cur_tx != s->cur_x || cur_ty != s->cur_y);

    /* If nothing was drawn since the last flip, skip the blit entirely.
     * This prevents wait-loop flicker and saves a full-screen blit when
     * the script is idle.  We still pump keys so input events aren't
     * dropped. A mouse move forces the refresh: the last blit erased the
     * composited cursor, so without this the pointer freezes whenever the
     * script stops drawing. */
    if (!s->frame_dirty && !cursor_moved) {
        allegro_pump_keys(s);
        return;
    }

    /* Pump pending keypresses. */
    allegro_pump_keys(s);

    /* Page-flip present: point the CRTC at the page the script just finished
     * drawing and start drawing into the one it replaced. This is the whole
     * point of the mode — no pixels move, the card simply scans out of a
     * different address, so presenting costs a couple of register writes
     * instead of the ~52 ms RAM->VRAM blit below (measured on a 486SX-25 at
     * 320x200x8). show_video_bitmap also waits for the vertical retrace, so the
     * swap is tear-free and doubles as the vsync. */
    if (s->page_flip && s->backbuf) {
        /* Composite the cursor into the page being presented, just before it
         * becomes visible. It stays on that page until the script repaints it
         * two frames from now — the same contract apps drawing their own
         * cursor before @flip already follow. */
        if (s->have_mouse && mouse_sprite) {
            masked_blit(mouse_sprite, s->backbuf, 0, 0, cur_tx, cur_ty,
                        mouse_sprite->w, mouse_sprite->h);
            s->cur_x = cur_tx;
            s->cur_y = cur_ty;
        }
        show_video_bitmap(s->vpage[s->vpage_active]);
        s->vpage_active ^= 1;
        s->backbuf = s->vpage[s->vpage_active];
        /* Allegro stores the clip rectangle per bitmap, so the newly selected
         * page needs the script's @clip re-applied before anything draws. */
        allegro_apply_clip(s);
        s->frame_dirty = false;
        return;
    }

    if (s->backbuf && screen) {
        /* When SSAA is active, box-filter downsample the oversized backbuffer
         * into the logical-size resolve bitmap, then blit resolve to screen.
         * Without SSAA, blit backbuf directly (existing behaviour). */
        BITMAP *src = s->backbuf;
        int src_w = s->backbuf->w;
        int src_h = s->backbuf->h;

        if (s->ss > 1 && s->resolve) {
            ssaa_downsample_2x(s->backbuf, s->resolve, s->color_depth);
            src = s->resolve;
            src_w = s->resolve->w;
            src_h = s->resolve->h;
        }

        /* Scare the software mouse cursor off the physical screen for the
         * duration of the blit. show_mouse(NULL) means the ISR cursor is
         * never armed, so this is defensive only — but if anything ever
         * re-arms it, an ISR firing mid-blit would snapshot a *partially*
         * updated screen as its "saved background" and restore that stale
         * snapshot next tick, painting trails that track the cursor. */
        scare_mouse();

        if (s->stretch_enabled) {
            /* Non-uniform stretch: fill the entire physical screen. */
            stretch_blit(src, screen,
                    0, 0, src_w, src_h,
                    0, 0, s->width, s->height);
        } else {
            /* The 3D viewport (@viewport) is scissored in sw_draw_tri at
             * rasterise time (glScissor semantics), so the whole surface is
             * always presented — 2D drawn outside the viewport must show. */
            blit(src, screen, 0, 0, s->vp_x, s->vp_y,
                    src_w, src_h);
        }

        /* Composite the cursor onto the physical screen AFTER the blit so it
         * survives until the next frame. The ISR cursor is disabled; this is
         * the only place the pointer is drawn in blit mode. */
        if (s->have_mouse && mouse_sprite) {
            masked_blit(mouse_sprite, screen, 0, 0, cur_tx, cur_ty,
                        mouse_sprite->w, mouse_sprite->h);
            s->cur_x = cur_tx;
            s->cur_y = cur_ty;
        }

        unscare_mouse();
    }

    s->frame_dirty = false;
}

/* Constant-alpha translucent drawing for solid-colour 2D primitives.
 * Allegro 4's translucency blender works in the truecolor (15/16/24/32 bpp)
 * modes; 8bpp paletted would need a precomputed trans table, so it is skipped
 * (draws opaque). Returns true if translucency was enabled — the caller MUST
 * pair it with alg_end_alpha() to restore DRAW_MODE_SOLID. alpha >= 255 is a
 * no-op (fully opaque). NOTE: this affects blended primitives (line, polygon,
 * circlefill, rectfill); putpixel ignores the blender, so @plot stays opaque.
 * Overlapping primitives double-blend (e.g. a thick line's quad + caps), which
 * is acceptable for the simple constant-alpha path. */
static bool alg_begin_alpha(BITMAP *tgt, int alpha) {
    if (alpha >= 255) return false;
    if (alpha < 0) alpha = 0;
    if (!tgt || bitmap_color_depth(tgt) <= 8) return false;
    set_trans_blender(0, 0, 0, alpha);
    drawing_mode(DRAW_MODE_TRANS, NULL, 0, 0);
    return true;
}

static void alg_end_alpha(bool on) {
    if (on) drawing_mode(DRAW_MODE_SOLID, NULL, 0, 0);
}

/* Apply the stored 2D clip rectangle to the drawing target. Allegro 4's
 * set_clip_rect clips all subsequent drawing operations on the bitmap.
 * The clip is stored in logical top-left coordinates and applied to the
 * backbuffer (or screen before a window is created). */
static void allegro_apply_clip(AllegroState *s) {
    BITMAP *tgt = DRAW_TGT(s);
    if (!tgt) return;
    if (s->clip_enabled && s->clip_w > 0 && s->clip_h > 0) {
        /* Scale logical clip rect to the (possibly supersampled) backbuffer,
         * including the page-flip centring offset every draw applies. */
        int x1 = s->clip_x * s->ss + s->offset_x;
        int y1 = s->clip_y * s->ss + s->offset_y;
        int x2 = (s->clip_x + s->clip_w) * s->ss + s->offset_x - 1;
        int y2 = (s->clip_y + s->clip_h) * s->ss + s->offset_y - 1;
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 >= tgt->w) x2 = tgt->w - 1;
        if (y2 >= tgt->h) y2 = tgt->h - 1;
        if (x2 >= x1 && y2 >= y1)
            set_clip_rect(tgt, x1, y1, x2, y2);
        else
            set_clip_rect(tgt, 0, 0, tgt->w - 1, tgt->h - 1);
    } else {
        set_clip_rect(tgt, 0, 0, tgt->w - 1, tgt->h - 1);
    }
}

/* Store a 2D clip rect in logical top-left coords and apply it.
 * w<=0 or h<=0 disables. Shared by @clip and draw-list DL_CLIP. */
static void allegro_set_clip(AllegroState *s, i32 x, i32 y, i32 w, i32 h) {
    s->clip_enabled = (w > 0 && h > 0);
    s->clip_x = x;
    s->clip_y = y;
    s->clip_w = w;
    s->clip_h = h;

    allegro_apply_clip(s);
}

/* @clip(x, y, w, h) — 2D scissor in logical top-left coords. w<=0 or h<=0
 * disables. Script-managed (IMGUI style). */
static void allegro_gfx_clip(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) return;

    i32 x = vm->r[1];
    i32 y = vm->r[2];
    i32 w = vm->r[3];
    i32 h = vm->r[4];

    allegro_set_clip(s, x, y, w, h);
}

/* Pen state for the do_line callback below. */
static int alg_pen_width;
static int alg_pen_color;
static int alg_pen_sx;  /* perpendicular span axis (1/0/-1) */
static int alg_pen_sy;

/* Draw a short axis-aligned segment through the current pixel, perpendicular to
 * the line direction. This fills the gap between parallel 1-pixel tracks so
 * diagonal strokes are solid rather than striped. */
static void alg_pen_stamp(BITMAP *bmp, int x, int y, int d) {
    int r = (alg_pen_width - 1) / 2;
    int extra = alg_pen_width - (2 * r + 1);
    line(bmp,
         x - alg_pen_sx * r, y - alg_pen_sy * r,
         x + alg_pen_sx * (r + extra), y + alg_pen_sy * (r + extra),
         alg_pen_color);
}

/* Draw a line of the given pixel width with thickness applied PERPENDICULAR
 * to the line direction so all strokes have the same apparent weight. The
 * old polygon-offset method rounded the perpendicular vector to a single
 * integer, which collapsed for diagonal strokes (especially width 2 and 4).
 * We walk the line with do_line and stamp a short perpendicular span at each
 * pixel, which is both solid and much cheaper than a width x width square. */
static void alg_thick_line(BITMAP *tgt, int x1, int y1, int x2, int y2,
        int width, int color) {
    if (width <= 1) {
        line(tgt, x1, y1, x2, y2, color);
        return;
    }

    float dx = (float) (x2 - x1);
    float dy = (float) (y2 - y1);
    float len2 = dx * dx + dy * dy;
    if (len2 < 1e-8f) {
        /* Degenerate (zero-length) line: draw a filled width x width square. */
        int r = (width - 1) / 2;
        int extra = width - (2 * r + 1);
        rectfill(tgt, x1 - r, y1 - r,
                x1 + r + extra, y1 + r + extra, color);
        return;
    }

    /* Thicken along the minor axis: mostly-horizontal lines get a vertical
     * span, mostly-vertical lines get a horizontal span. Diagonals use the
     * vertical span, which covers the adjacent diagonal pixel tracks. */
    if (fabsf(dy) > fabsf(dx)) {
        alg_pen_sx = 1;
        alg_pen_sy = 0;
    } else {
        alg_pen_sx = 0;
        alg_pen_sy = 1;
    }

    alg_pen_width = width;
    alg_pen_color = color;
    do_line(tgt, x1, y1, x2, y2, 0, alg_pen_stamp);
}

static void allegro_gfx_line(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) return;

    int x1 = vm->r[1], y1 = vm->r[2];
    int x2 = vm->r[3], y2 = vm->r[4];
    u32 rgba_ptr = (u32) vm->r[5];
    int width = vm->r[6];
    if (width < 1) width = 1;
    if (rgba_ptr + 4 > vm->mem_sz) return;

    int r = vm->mem[rgba_ptr];
    int g = vm->mem[rgba_ptr + 1];
    int b = vm->mem[rgba_ptr + 2];
    int a = vm->mem[rgba_ptr + 3];

    /* Scale to supersampled backbuffer and apply centering offset */
    x1 = x1 * s->ss + s->offset_x;
    y1 = y1 * s->ss + s->offset_y;
    x2 = x2 * s->ss + s->offset_x;
    y2 = y2 * s->ss + s->offset_y;
    width = width * s->ss;

    int color = makecol(r, g, b);
    BITMAP *tgt = DRAW_TGT(s);
    bool trans = alg_begin_alpha(tgt, a);
    alg_thick_line(tgt, x1, y1, x2, y2, width, color);
    alg_end_alpha(trans);
    s->frame_dirty = true;
}

static void allegro_gfx_polygon(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) return;

    u32 points_ptr = (u32) vm->r[1];
    i32 count = vm->r[2];
    u32 rgba_ptr = (u32) vm->r[3];

    if (count < 3 || points_ptr + count * 4 > vm->mem_sz) return;
    if (rgba_ptr + 4 > vm->mem_sz) return;

    int r = vm->mem[rgba_ptr];
    int g = vm->mem[rgba_ptr + 1];
    int b = vm->mem[rgba_ptr + 2];
    int a = vm->mem[rgba_ptr + 3];
    int color = makecol(r, g, b);

    /* Allegro's polygon function needs an array of points. Use a static
     * buffer for common polygon sizes (wdoom uses quads = 4 verts) to avoid
     * malloc/free overhead on every call. */
    #define ALG_POLY_MAX 64
    static int s_points[ALG_POLY_MAX * 2];
    int *points;
    bool allocated = false;
    if (count <= ALG_POLY_MAX) {
        points = s_points;
    } else {
        points = malloc(count * 2 * sizeof(int));
        if (!points) return;
        allocated = true;
    }

    for (int i = 0; i < count; i++) {
        u32 offset = points_ptr + i * 4;
        i16 px = (i16)(vm->mem[offset] | (vm->mem[offset + 1] << 8));
        i16 py = (i16)(vm->mem[offset + 2] | (vm->mem[offset + 3] << 8));
        /* Scale to supersampled backbuffer and apply centering offset */
        points[i * 2] = px * s->ss + s->offset_x;
        points[i * 2 + 1] = py * s->ss + s->offset_y;
    }

    BITMAP *tgt = DRAW_TGT(s);
    bool trans = alg_begin_alpha(tgt, a);
    polygon(tgt, count, points, color);
    alg_end_alpha(trans);
    if (allocated) free(points);
    s->frame_dirty = true;
}

static void allegro_gfx_polyline(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) return;

    u32 points_ptr = (u32) vm->r[1];
    i32 count = vm->r[2];
    u32 rgba_ptr = (u32) vm->r[3];
    int width = vm->r[4];
    if (width < 1) width = 1;

    if (count < 2 || points_ptr + count * 4 > vm->mem_sz) return;
    if (rgba_ptr + 4 > vm->mem_sz) return;

    int r = vm->mem[rgba_ptr];
    int g = vm->mem[rgba_ptr + 1];
    int b = vm->mem[rgba_ptr + 2];
    int a = vm->mem[rgba_ptr + 3];
    int color = makecol(r, g, b);

    BITMAP *tgt = DRAW_TGT(s);
    bool trans = alg_begin_alpha(tgt, a);

    /* Draw lines between consecutive points */
    for (int i = 0; i < count - 1; i++) {
        u32 offset1 = points_ptr + i * 4;
        u32 offset2 = points_ptr + (i + 1) * 4;
        i16 x1 = (i16)(vm->mem[offset1] | (vm->mem[offset1 + 1] << 8));
        i16 y1 = (i16)(vm->mem[offset1 + 2] | (vm->mem[offset1 + 3] << 8));
        i16 x2 = (i16)(vm->mem[offset2] | (vm->mem[offset2 + 1] << 8));
        i16 y2 = (i16)(vm->mem[offset2 + 2] | (vm->mem[offset2 + 3] << 8));

        /* Scale to supersampled backbuffer and apply centering offset */
        x1 = x1 * s->ss + s->offset_x;
        y1 = y1 * s->ss + s->offset_y;
        x2 = x2 * s->ss + s->offset_x;
        y2 = y2 * s->ss + s->offset_y;

        alg_thick_line(tgt, x1, y1, x2, y2, width * s->ss, color);
    }
    alg_end_alpha(trans);
    s->frame_dirty = true;
}

/* =========================================================================
 * Custom software 3D rasteriser
 * =========================================================================
 * Vertex format: float pos[3], normal[3], uv[2]  (VSIZE = 32 bytes)
 * ========================================================================= */

static void alg_read_vec3(RelicVM *vm, u32 ptr, float out[3]) {
    if (ptr + 12 > vm->mem_sz) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    memcpy(out, vm->mem + ptr, 12);
}

static void alg_mat4_mul(const float a[16], const float b[16], float out[16]) {
    float r[16];
    int c, row, k;
    for (c = 0; c < 4; c++)
        for (row = 0; row < 4; row++) {
            float v = 0;
            for (k = 0; k < 4; k++) v += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = v;
        }
    memcpy(out, r, sizeof(r));
}

static void alg_mat4_xform(const float m[16], const float p[3], float out[4]) {
    out[0] = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    out[1] = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    out[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
    out[3] = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
}

/* -------------------------------------------------------------------------
 * sw_draw_tri  --  scanline rasteriser with 16-bit z-buffer.
 *
 * Attributes per vertex (SwVert):
 *   sx, sy   screen position (float)
 *   invw     1/clip_w  (for depth test and perspective UV)
 *   r,g,b    lit colour 0..255
 *   tu, tv   u/w * tex_w,  v/w * tex_h  (pre-divided; recover with *w)
 *
 * Sub-span length SW_SPAN: affine approximation is restarted every
 * SW_SPAN pixels (classic BSP-engine trick); 16px is imperceptible at 320x200.
 * ---------------------------------------------------------------------- */
typedef struct {
    float sx, sy, invw, r, g, b, tu, tv;
} SwVert;

/* Sub-span width for perspective-correct UV restart.
 * Must be a power of 2 so the reciprocal can be a constant. */
#define SW_SPAN 8

/* 16.16 fixed-point helpers */
#define FP_SHIFT 16
#define FP_ONE   (1 << FP_SHIFT)
#define FLOAT_TO_FP(x) ((int)((x) * (float)FP_ONE))

#ifdef RELIC_FIXED_POINT_3D
/* -------------------------------------------------------------------------
 * Integer-only fixed-point helpers for FPU-less 386SX.
 *
 * The sub-span perspective UV loop in sw_draw_tri() is the hottest FPU path:
 * two 1/w divisions and ~6 multiplies per 8-pixel sub-span.  On a 386SX each
 * x87 instruction traps into DJGPP's -lemu emulator (~100 cycles), while a
 * 32x32→64 imul runs in hardware (~15 cycles).  These helpers replace the
 * emulated float ops with 64-bit-integer equivalents.
 *
 * fixmul_i:  16.16 × 16.16 → 16.16  (64-bit intermediate, no FPU)
 * fixrcp_i:  16.16 → 16.16 reciprocal (64-bit divide, clamped to 32767.0)
 *
 * Allegro's own fixmul/fixdiv fall back to ftofix(fixtof(x)*fixtof(y)) on
 * ALLEGRO_I386 (see fmaths.inl), which uses the FPU — so we can't rely on
 * them for the FPU-free path.
 * ---------------------------------------------------------------------- */
#define FIXMUL_I(x, y)  ((int)(((long long)(x) * (long long)(y)) >> 16))
#define FIXRCP_MAX      2147418112  /* 32767.0 in 16.16 (just under INT32_MAX) */
static __inline__ int fixrcp_i(int x) {
    if (x <= 0) return FIXRCP_MAX;
    long long r = ((long long)1 << 32) / (long long)x;
    if (r > (long long)FIXRCP_MAX) return FIXRCP_MAX;
    return (int)r;
}

/* NOTE: the per-vertex transform deliberately stays in float even on the
 * soft-float target.  A 16.16 fixed-point version was tried and removed:
 * clip-space w/z for distant sector-map geometry exceed the ±32768 range that
 * 16.16 can hold, and the path has to divide back to float per vertex
 * anyway (4 soft-float divides), so it cost precision for no real gain. */

#endif /* RELIC_FIXED_POINT_3D */



/* Precomputed pixel-format descriptor – avoids makecol_depth per pixel. */
typedef struct {
    int depth;
    /* For 15/16 bpp packed RGB: shift and mask for each channel */
    int rs, gs, bs;        /* left-shift to place channel in word */
    int rw, gw, bw;        /* channel width in bits (for >>= to 0..2^w-1) */
    /* For 32 bpp we store the Allegro makecol result directly (correct BGR). */
    /* For 8 bpp: 64x64x64 RGB->palette lookup (256KB, fits in L2). */
    unsigned char *rgb_to_idx;  /* malloc'd SW_PAL_LUT_ENTRIES table, NULL for non-8bpp */
    /* Plane-batched (mode Y) staging: when non-NULL the 8bpp writes are going
     * into a system-RAM scanline buffer rather than the framebuffer, and each
     * written pixel must be recorded here so the flush knows which ones passed
     * the z-test. Bit per pixel, LSB first. NULL for every non-planar target,
     * which keeps the ordinary paths branch-identical to before. */
    unsigned char *dirty;
} PixFmt;

static PixFmt sw_make_pixfmt(int depth) {
    PixFmt pf;
    pf.depth = depth;
    pf.rgb_to_idx = NULL;
    pf.dirty = NULL;
    if (depth == 15) {
        /* 5-5-5: discard low 3 bits of each 8-bit channel. */
        pf.rs = _rgb_r_shift_15;
        pf.rw = 3;
        pf.gs = _rgb_g_shift_15;
        pf.gw = 3;
        pf.bs = _rgb_b_shift_15;
        pf.bw = 3;
    } else if (depth == 16) {
        /* 5-6-5: R/B discard 3 bits, G discards 2 bits. */
        pf.rs = _rgb_r_shift_16;
        pf.rw = 3;
        pf.gs = _rgb_g_shift_16;
        pf.gw = 2;
        pf.bs = _rgb_b_shift_16;
        pf.bw = 3;
    } else {
        pf.rs = pf.gs = pf.bs = pf.rw = pf.gw = pf.bw = 0;
    }
    return pf;
}

/* -------------------------------------------------------------------------
 * 8bpp optimised palette: sample texture colours, median-cut to 236 entries.
 *
 * The palette is tailored to the actual
 * texture colours, dramatically improving colour accuracy for classic-FPS
 * textures (browns, greens, flesh tones) that the uniform 3-3-2 cube
 * renders as grey-blue.
 *
 * Memory: 72 KB for the sample buffer (SW_MAX_SAMPLES * 3 bytes).
 *         Median-cut runs in-place on the sample array — no extra alloc.
 *         The lookup table reuses the existing 32 KB rgb_to_idx buffer.
 *
 * Timing on a 486SX-25: ~255 ms total (sampling 5 ms, median-cut 50 ms,
 * lookup table build 200 ms with tree-guided search).
 * ------------------------------------------------------------------------- */

/* Sample colours from a texture into sw_col_samples[].
 * Samples every 4th pixel in both x and y (1/16 of texels) to keep the
 * sample count low without missing distinct colours — game textures are
 * 64x64 with large flat regions, so this captures all unique colours. */
static void sw_sample_texture_colours(AllegroState *s,
                                      const u8 *pixels, int pw, int ph) {
    if (!s->sw_col_samples || s->sw_col_count >= SW_MAX_SAMPLES) return;

    /* Stride: sample every 4th pixel. For very small textures (< 8px in
     * either dimension) sample every pixel so we don't miss everything. */
    int stride = (pw < 8 || ph < 8) ? 1 : 4;

    int x, y;
    for (y = 0; y < ph; y += stride) {
        for (x = 0; x < pw; x += stride) {
            if (s->sw_col_count >= SW_MAX_SAMPLES) return;
            int off = (y * pw + x) * 4;
            /* Skip fully-transparent pixels (alpha == 0) — they're not
             * visible and would waste palette entries. */
            if (pixels[off + 3] == 0) continue;
            int idx = s->sw_col_count * 3;
            s->sw_col_samples[idx + 0] = pixels[off + 0];
            s->sw_col_samples[idx + 1] = pixels[off + 1];
            s->sw_col_samples[idx + 2] = pixels[off + 2];
            s->sw_col_count++;
        }
    }
}

/* Median-cut colour quantisation.
 *
 * Operates in-place on the sample array (no heap alloc). The algorithm:
 *   1. Start with one "box" covering all samples.
 *   2. Repeatedly split the box with the largest range along its longest
 *      axis at the median, until we have SW_PAL_TEXTURE_ENTRIES boxes.
 *   3. The average colour of each box becomes one palette entry.
 *
 * The box list is a small stack-allocated array (236 entries * 12 bytes =
 * ~2.8 KB). Each box tracks its start index and count in the sample array;
 * samples are partitioned in-place so each box's samples are contiguous. */

static void mc_compute_bounds(const unsigned char *samples, McBox *box) {
    box->rmin = box->gmin = box->bmin = 255;
    box->rmax = box->gmax = box->bmax = 0;
    for (int i = 0; i < box->count; i++) {
        int idx = (box->start + i) * 3;
        u8 r = samples[idx], g = samples[idx + 1], b = samples[idx + 2];
        if (r < box->rmin) box->rmin = r;
        if (r > box->rmax) box->rmax = r;
        if (g < box->gmin) box->gmin = g;
        if (g > box->gmax) box->gmax = g;
        if (b < box->bmin) box->bmin = b;
        if (b > box->bmax) box->bmax = b;
    }
}

/* Partition a box's colours around the median along the given axis.
 * After this call, the first half of the box's colours have axis-value
 * <= median, the second half have > median. Returns the split point
 * (count of colours in the left partition). */
static int mc_partition(unsigned char *samples, McBox *box, int axis) {
    /* Simple selection: find the median value along the axis, then
     * partition in-place (Lomuto-style). For small boxes this is faster
     * than a full sort. */
    int n = box->count;
    int start = box->start;

    /* Extract axis values */
    /* Use a simple nth-element via insertion-sort partial: for n <= 256
     * (typical), a counting sort on the axis channel is O(n) and stable. */
    int hist[256];
    memset(hist, 0, sizeof(hist));
    for (int i = 0; i < n; i++) {
        int idx = (start + i) * 3 + axis;
        hist[samples[idx]]++;
    }
    /* Find median value */
    int median_pos = n / 2;
    int acc = 0, median_val = 0;
    for (int v = 0; v < 256; v++) {
        acc += hist[v];
        if (acc >= median_pos) { median_val = v; break; }
    }
    /* Partition: colours with axis <= median_val go left, > go right */
    int left = 0;
    for (int i = 0; i < n; i++) {
        int idx = (start + i) * 3 + axis;
        if (samples[idx] <= median_val) {
            /* Swap colour at position i with colour at position left */
            if (i != left) {
                int li = (start + left) * 3;
                int ii = (start + i) * 3;
                unsigned char tmp[3];
                tmp[0] = samples[li]; tmp[1] = samples[li+1]; tmp[2] = samples[li+2];
                samples[li] = samples[ii]; samples[li+1] = samples[ii+1]; samples[li+2] = samples[ii+2];
                samples[ii] = tmp[0]; samples[ii+1] = tmp[1]; samples[ii+2] = tmp[2];
            }
            left++;
        }
    }
    /* Ensure neither partition is empty (can happen if all colours are
     * identical along this axis). */
    if (left == 0) left = 1;
    if (left == n) left = n - 1;
    return left;
}

/* Build the RGB->palette-index lookup table using box-guided nearest-colour
 * search. This is used instead of sw_build_lookup_table() when the palette
 * is the optimised median-cut palette (not 3-3-2), because the 3-3-2
 * arithmetic shortcut in sw_build_lookup_table() would produce wrong results.
 *
 * The search strategy:
 *   1. Collect all 256 palette entries (20 UI + 236 texture) into an array
 *   2. Sort by red channel (enables early-termination scan)
 *   3. For each LUT cell, binary-search for the nearest red, then scan
 *      outward stopping when dr*dr >= best_dist
 *
 * Timing: 32K cells × (log 256 + ~5 neighbours) × ~10 cycles = ~170ms
 * on a 486SX-25.  Compare: brute force would be ~6 seconds. */
static void sw_build_lookup_table_guided(unsigned char **out_table,
        const PALETTE pal) {
    *out_table = (unsigned char *) malloc(SW_PAL_LUT_ENTRIES);
    if (!*out_table) return;

    /* Collect all 256 palette entries as 0..255 RGB, tagged with their
     * palette index. Sort by red channel for early-termination scan. */
    typedef struct { u8 r, g, b, idx; } PalEntry;
    PalEntry entries[256];
    int n_entries = 0;
    for (int i = 0; i < 256; i++) {
        entries[n_entries].r = (u8)((pal[i].r << 2) | (pal[i].r >> 4));
        entries[n_entries].g = (u8)((pal[i].g << 2) | (pal[i].g >> 4));
        entries[n_entries].b = (u8)((pal[i].b << 2) | (pal[i].b >> 4));
        entries[n_entries].idx = (u8)i;
        n_entries++;
    }

    /* Simple insertion sort by red channel (256 entries, cheap). */
    for (int i = 1; i < n_entries; i++) {
        PalEntry tmp = entries[i];
        int j = i;
        while (j > 0 && entries[j - 1].r > tmp.r) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = tmp;
    }

    /* For each LUT cell, find the nearest palette entry. */
    for (int r = 0; r < SW_PAL_LUT_SIZE; r++) {
        int r8 = (r << 3) | (r >> 2);
        for (int g = 0; g < SW_PAL_LUT_SIZE; g++) {
            int g8 = (g << 3) | (g >> 2);
            for (int b = 0; b < SW_PAL_LUT_SIZE; b++) {
                int b8 = (b << 3) | (b >> 2);

                /* Binary search for the entry with the closest red value. */
                int lo = 0, hi = n_entries - 1;
                while (lo < hi) {
                    int mid = (lo + hi) >> 1;
                    if (entries[mid].r < r8) lo = mid + 1;
                    else hi = mid;
                }
                int start = lo;

                /* Scan outward from the starting point. Go left first,
                 * then right, stopping in each direction when dr*dr
                 * exceeds best_dist. */
                int best = entries[start].idx;
                int dr0 = r8 - entries[start].r;
                long best_dist = (long)dr0 * dr0
                               + (long)(g8 - entries[start].g) * (g8 - entries[start].g)
                               + (long)(b8 - entries[start].b) * (b8 - entries[start].b);

                /* Scan left (decreasing red). */
                for (int i = start - 1; i >= 0; i--) {
                    int dr = r8 - entries[i].r;
                    long dr2 = (long)dr * dr;
                    if (dr2 >= best_dist) break;  /* red too far left */
                    int dg = g8 - entries[i].g;
                    long d = dr2 + (long)dg * dg;
                    if (d >= best_dist) continue;
                    int db = b8 - entries[i].b;
                    d += (long)db * db;
                    if (d < best_dist) {
                        best_dist = d;
                        best = entries[i].idx;
                    }
                }

                /* Scan right (increasing red). */
                for (int i = start + 1; i < n_entries; i++) {
                    int dr = r8 - entries[i].r;
                    long dr2 = (long)dr * dr;
                    if (dr2 >= best_dist) break;  /* red too far right */
                    int dg = g8 - entries[i].g;
                    long d = dr2 + (long)dg * dg;
                    if (d >= best_dist) continue;
                    int db = b8 - entries[i].b;
                    d += (long)db * db;
                    if (d < best_dist) {
                        best_dist = d;
                        best = entries[i].idx;
                    }
                }

                (*out_table)[((r * SW_PAL_LUT_SIZE) + g) * SW_PAL_LUT_SIZE + b] =
                        (unsigned char) best;
            }
        }
    }
}


/* Build an optimised 236-colour palette from sampled texture colours using
 * median-cut, then set it as the VGA palette and rebuild the lookup table.
 *
 * Falls back to the existing 3-3-2 palette if no samples were collected
 * (e.g. a 2D-only program with no textures). */
static void sw_build_optimised_palette(AllegroState *s) {
    if (s->sw_pal_built) return;
    s->sw_pal_built = true;  /* Set regardless of outcome so we only try once */

    if (s->color_depth != 8 || !s->sw_col_samples || s->sw_col_count < 16) {
        /* Not 8bpp, or too few samples — keep the 3-3-2 palette that was
         * already set at @window time. */
        return;
    }

    /* Median-cut: partition sw_col_samples into SW_PAL_TEXTURE_ENTRIES boxes.
     * Box list is stack-allocated (~2.8 KB). */
    McBox boxes[SW_PAL_TEXTURE_ENTRIES];
    int n_boxes = 1;
    boxes[0].start = 0;
    boxes[0].count = s->sw_col_count;
    mc_compute_bounds(s->sw_col_samples, &boxes[0]);

    /* Repeatedly split the box with the largest volume until we have enough. */
    while (n_boxes < SW_PAL_TEXTURE_ENTRIES) {
        /* Find the box with the largest range (longest axis). */
        int best_box = -1;
        int best_range = 0;
        for (int i = 0; i < n_boxes; i++) {
            if (boxes[i].count < 2) continue;  /* Can't split a 1-colour box */
            int rr = boxes[i].rmax - boxes[i].rmin;
            int gr = boxes[i].gmax - boxes[i].gmin;
            int br = boxes[i].bmax - boxes[i].bmin;
            int max_range = rr;
            if (gr > max_range) max_range = gr;
            if (br > max_range) max_range = br;
            if (max_range > best_range) {
                best_range = max_range;
                best_box = i;
            }
        }
        if (best_box < 0) break;  /* All boxes have 1 colour, can't split further */

        /* Determine the longest axis of the best box. */
        McBox *b = &boxes[best_box];
        int rr = b->rmax - b->rmin;
        int gr = b->gmax - b->gmin;
        int br = b->bmax - b->bmin;
        int axis = 0;  /* 0=R, 1=G, 2=B */
        if (gr >= rr && gr >= br) axis = 1;
        if (br >= rr && br >= gr) axis = 2;

        /* Split at the median along that axis. */
        int left_count = mc_partition(s->sw_col_samples, b, axis);

        /* The new box takes the right partition; the old box keeps the left. */
        McBox right;
        right.start = b->start + left_count;
        right.count = b->count - left_count;
        b->count = left_count;

        /* Recompute bounds for both halves. */
        mc_compute_bounds(s->sw_col_samples, b);
        mc_compute_bounds(s->sw_col_samples, &right);

        /* Insert the new box at the end (order doesn't matter). */
        boxes[n_boxes++] = right;
    }

    /* Build the palette: average colour of each box. */
    PALETTE pal;

    /* Start with the 3-3-2 palette as a base (fills all 256 entries). */
    generate_332_palette(pal);

    /* Override entries 0..19 with UI colours (same as sw_setup_8bit_palette
     * but with 20 reserved entries instead of 16). */
    pal[0].r = pal[0].g = pal[0].b = 0;  /* black / transparency mask */
    const unsigned char ui_colors[19][3] = {
        {255, 255, 255},  /* 1: white */
        {192, 192, 192},  /* 2: light gray */
        {128, 128, 128},  /* 3: medium gray */
        {64, 64, 64},     /* 4: dark gray */
        {255, 0, 0},      /* 5: red */
        {0, 255, 0},      /* 6: green */
        {0, 0, 255},      /* 7: blue */
        {255, 255, 0},    /* 8: yellow */
        {255, 0, 255},    /* 9: magenta */
        {0, 255, 255},    /* 10: cyan */
        {255, 128, 0},    /* 11: orange */
        {128, 64, 0},     /* 12: brown */
        {255, 192, 192},  /* 13: pink */
        {0, 128, 0},      /* 14: dark green */
        {128, 0, 0},      /* 15: dark red */
        {32, 32, 32},     /* 16: very dark gray */
        {160, 160, 160},  /* 17: mid-light gray */
        {96, 96, 96},     /* 18: dark-medium gray */
        {255, 255, 128},  /* 19: light yellow */
    };
    for (int i = 0; i < 19; i++) {
        pal[i + 1].r = ui_colors[i][0] >> 2;
        pal[i + 1].g = ui_colors[i][1] >> 2;
        pal[i + 1].b = ui_colors[i][2] >> 2;
    }

    /* Fill entries 20..255 with the median-cut colours. */
    int pal_idx = 20;
    for (int i = 0; i < n_boxes && pal_idx < 256; i++) {
        /* Average colour of this box. */
        long sr = 0, sg = 0, sb = 0;
        for (int j = 0; j < boxes[i].count; j++) {
            int idx = (boxes[i].start + j) * 3;
            sr += s->sw_col_samples[idx];
            sg += s->sw_col_samples[idx + 1];
            sb += s->sw_col_samples[idx + 2];
        }
        int cnt = boxes[i].count;
        if (cnt > 0) {
            pal[pal_idx].r = (unsigned char)((sr / cnt) >> 2);
            pal[pal_idx].g = (unsigned char)((sg / cnt) >> 2);
            pal[pal_idx].b = (unsigned char)((sb / cnt) >> 2);
            pal_idx++;
        }
    }

    /* If we didn't fill all 236 texture slots, the remaining entries stay
     * as the 3-3-2 base — they'll still be valid palette colours. */

    /* Set the new palette and rebuild the lookup table. */
    set_palette(pal);

    /* Free the old lookup table and rebuild against the new palette.
     * Use the box-guided search instead of sw_build_lookup_table() because
     * the 3-3-2 arithmetic shortcut in that function doesn't apply to the
     * optimised median-cut palette. */
    if (s->rgb_to_idx) {
        free(s->rgb_to_idx);
        s->rgb_to_idx = NULL;
    }
    sw_build_lookup_table_guided(&s->rgb_to_idx, pal);

    int total_samples = s->sw_col_count;
    int texture_pal_entries = pal_idx - 20;

    /* Free the sample buffer — no longer needed. */
    if (s->sw_col_samples) {
        free(s->sw_col_samples);
        s->sw_col_samples = NULL;
    }
    s->sw_col_count = 0;

    allegro_mode_log("DOS: gfx: 8bpp optimised palette built from %d texture "
                     "colour samples, %d palette entries\n",
                     total_samples, texture_pal_entries);
}

/* Lock in the flat 3-3-2 palette as final (SW_PAL_FLAT), instead of letting
 * the first @camera() call replace it with a texture-sampled median-cut
 * palette. Frees the (now unneeded) sample buffer. */
static void sw_finalize_flat_palette(AllegroState *s) {
    PALETTE pal;
    sw_setup_8bit_palette(pal);
    set_palette(pal);

    if (s->rgb_to_idx) {
        free(s->rgb_to_idx);
        s->rgb_to_idx = NULL;
    }
    /* The plain (non-guided) builder relies on the palette's regular 3-3-2
     * structure, which is exactly what sw_setup_8bit_palette produces. */
    sw_build_lookup_table(&s->rgb_to_idx);

    if (s->sw_col_samples) {
        free(s->sw_col_samples);
        s->sw_col_samples = NULL;
    }
    s->sw_col_count = 0;
    allegro_mode_log("DOS: gfx: 8bpp flat 3-3-2 palette selected via "
                     "@palette_upload (general-use mode)\n");
}

/* Build the final palette from an explicit RGB888 table uploaded by the
 * script (SW_PAL_CUSTOM), e.g. the game's PLAYPAL. Used verbatim, entry for
 * entry, with no UI-colour reservation: callers supplying a full palette are
 * assumed to already cover their own UI/text needs from the same table. */
static void sw_build_custom_palette(AllegroState *s) {
    PALETTE pal;
    memset(pal, 0, sizeof(pal));

    int count = s->sw_pal_custom_count;
    if (count > 256) count = 256;
    for (int i = 0; i < count; i++) {
        pal[i].r = (unsigned char) (s->sw_pal_custom[i * 3 + 0] >> 2);
        pal[i].g = (unsigned char) (s->sw_pal_custom[i * 3 + 1] >> 2);
        pal[i].b = (unsigned char) (s->sw_pal_custom[i * 3 + 2] >> 2);
    }
    /* Any entries beyond `count` stay black — matches sw_setup_8bit_palette's
     * treatment of index 0 and is harmless since the caller only asked for
     * `count` colours in the first place. */

    set_palette(pal);
    if (s->rgb_to_idx) {
        free(s->rgb_to_idx);
        s->rgb_to_idx = NULL;
    }
    /* The 32K-entry RGB->index LUT is deterministic given the palette, but
     * the guided nearest-colour search is very slow on FPU-less hardware
     * (~40 s on a 486SX-25 for the game's 256-entry PLAYPAL, because the
     * early-termination scan ends up checking most palette entries per
     * cell).  Cache the finished LUT to RDPAL.LUT so subsequent runs load
     * it in ~1 ms.  Validated by a 32-bit checksum of the palette bytes,
     * so a palette change silently rebuilds rather than loading a stale
     * table.  samples/tdoom/builds/gen_lut.py generates a bit-identical
     * file on a fast host for shipping alongside the binary. */
    {
        u32 pal_cksum = 0;
        for (int i = 0; i < count; i++) {
            pal_cksum += ((u32)pal[i].r << 16) | ((u32)pal[i].g << 8)
                         | (u32)pal[i].b;
            pal_cksum = (pal_cksum << 1) | (pal_cksum >> 31);
        }

        unsigned char *lut = (unsigned char *) malloc(SW_PAL_LUT_ENTRIES);
        int loaded = 0;
        if (lut) {
            FILE *cf = fopen("RDPAL.LUT", "rb");
            if (cf) {
                u32 magic = 0, cksum = 0, n = 0;
                if (fread(&magic, 4, 1, cf) == 1 &&
                    fread(&cksum, 4, 1, cf) == 1 &&
                    fread(&n, 4, 1, cf) == 1 &&
                    magic == 0x4C50414Cu &&  /* "LPAL" */
                    cksum == pal_cksum &&
                    n == SW_PAL_LUT_ENTRIES &&
                    fread(lut, 1, SW_PAL_LUT_ENTRIES, cf) == SW_PAL_LUT_ENTRIES) {
                    loaded = 1;
                }
                fclose(cf);
            }
        }
        if (loaded) {
            s->rgb_to_idx = lut;
            allegro_mode_log("DOS: gfx: 8bpp LUT loaded from RDPAL.LUT cache\n");
        } else {
            /* Build the LUT the slow way, then save it for next time.
             * Custom palettes have no regular structure to exploit, so
             * this uses the box-guided nearest-colour search (same as the
             * median-cut path). */
            if (lut) free(lut);
            sw_build_lookup_table_guided(&s->rgb_to_idx, pal);
            if (s->rgb_to_idx) {
                FILE *cf = fopen("RDPAL.LUT", "wb");
                if (cf) {
                    u32 magic = 0x4C50414Cu, n = SW_PAL_LUT_ENTRIES;
                    fwrite(&magic, 4, 1, cf);
                    fwrite(&pal_cksum, 4, 1, cf);
                    fwrite(&n, 4, 1, cf);
                    fwrite(s->rgb_to_idx, 1, SW_PAL_LUT_ENTRIES, cf);
                    fclose(cf);
                    allegro_mode_log("DOS: gfx: 8bpp LUT built and saved to RDPAL.LUT\n");
                }
            }
        }
    }

    if (s->sw_col_samples) {
        free(s->sw_col_samples);
        s->sw_col_samples = NULL;
    }
    s->sw_col_count = 0;
    allegro_mode_log("DOS: gfx: 8bpp custom palette uploaded (%d entries)\n",
                     count);
}


/* Build the 8bpp RGB->palette-index lookup table.
 * Must be called AFTER set_palette() so the palette being matched against is
 * the one actually in use.
 *
 * Indexed at SW_PAL_BITS bits per channel (see sw_put_pix). 5 bits is plenty:
 * the destination palette is only 3-3-2, i.e. 8 red / 8 green / 4 blue levels,
 * so quantising the *input* to 32 levels per channel cannot introduce a
 * visible error. It does make the table 8x smaller than a 6-bit one
 * (32 KB vs 256 KB, which matters on a 7 MB machine) and, more importantly,
 * 8x cheaper to build: this runs inside @window on hardware where the whole
 * point is to not spend 30 seconds before the first frame.
 *
 * The nearest-colour search is done here rather than via Allegro's
 * bestfit_color() so that the palette's regular 3-3-2 structure can be used to
 * seed the search with a very good guess, letting most cells exit after a
 * handful of comparisons instead of scanning all 256 entries. */
static void sw_build_lookup_table(unsigned char **out_table) {
    int r, g, b, i;

    *out_table = (unsigned char *) malloc(SW_PAL_LUT_ENTRIES);
    if (!*out_table) return;

    /* Cache the palette as 0..255 RGB up front; _current_palette stores 0..63
     * and reading it per comparison is needless work in the hot loop. */
    unsigned char pal_r[256], pal_g[256], pal_b[256];
    for (i = 0; i < 256; i++) {
        pal_r[i] = (unsigned char) ((_current_palette[i].r << 2) |
                (_current_palette[i].r >> 4));
        pal_g[i] = (unsigned char) ((_current_palette[i].g << 2) |
                (_current_palette[i].g >> 4));
        pal_b[i] = (unsigned char) ((_current_palette[i].b << 2) |
                (_current_palette[i].b >> 4));
    }

    /* The palette is 3-3-2 (8 red, 8 green, 4 blue levels) with indices 0..15
     * overridden by sw_setup_8bit_palette() for UI colours.  For indices >= 16
     * the nearest entry can be computed arithmetically — no search at all.
     * Only colours that would map to indices 0..15 (dark, low-red colours)
     * need a search, and only against the 16 overridden entries plus the
     * nearest non-overridden 3-3-2 entry (index 16).  This cuts the build
     * from ~8M comparisons to ~100K on a 486SX with no FPU. */
    for (r = 0; r < SW_PAL_LUT_SIZE; r++) {
        int r8 = (r << 3) | (r >> 2);
        /* 3-3-2 red index (rounded to nearest of 8 levels). */
        int r_idx = (r8 + 16) >> 5; if (r_idx > 7) r_idx = 7;
        for (g = 0; g < SW_PAL_LUT_SIZE; g++) {
            int g8 = (g << 3) | (g >> 2);
            int g_idx = (g8 + 16) >> 5; if (g_idx > 7) g_idx = 7;
            for (b = 0; b < SW_PAL_LUT_SIZE; b++) {
                int b8 = (b << 3) | (b >> 2);
                int b_idx = (b8 + 32) >> 6; if (b_idx > 3) b_idx = 3;

                int direct = (r_idx << 5) | (g_idx << 2) | b_idx;

                if (direct >= 16) {
                    /* Not overridden — the arithmetic result is exact. */
                    (*out_table)[((r * SW_PAL_LUT_SIZE) + g) * SW_PAL_LUT_SIZE + b] =
                            (unsigned char) direct;
                } else {
                    /* Falls in the overridden range — search the 16 UI
                     * entries plus the nearest non-overridden 3-3-2 entry
                     * (index 16 = r_idx=2, g_idx=0, b_idx=0). */
                    int best = direct;
                    long best_dist = (long)(r8 - pal_r[direct]) * (r8 - pal_r[direct])
                                   + (long)(g8 - pal_g[direct]) * (g8 - pal_g[direct])
                                   + (long)(b8 - pal_b[direct]) * (b8 - pal_b[direct]);
                    for (i = 0; i < 16; i++) {
                        int dr = r8 - pal_r[i];
                        long d = (long) dr * dr;
                        if (d >= best_dist) continue;
                        int dg = g8 - pal_g[i];
                        d += (long) dg * dg;
                        if (d >= best_dist) continue;
                        int db = b8 - pal_b[i];
                        d += (long) db * db;
                        if (d < best_dist) {
                            best_dist = d;
                            best = i;
                        }
                    }
                    /* Also consider index 16 (first non-overridden entry). */
                    {
                        int dr = r8 - pal_r[16];
                        long d = (long) dr * dr;
                        if (d < best_dist) {
                            int dg = g8 - pal_g[16];
                            d += (long) dg * dg;
                            if (d < best_dist) {
                                int db = b8 - pal_b[16];
                                d += (long) db * db;
                                if (d < best_dist) {
                                    best = 16;
                                }
                            }
                        }
                    }
                    (*out_table)[((r * SW_PAL_LUT_SIZE) + g) * SW_PAL_LUT_SIZE + b] =
                            (unsigned char) best;
                }
            }
        }
    }
}

/* Setup 8-bit palette with comprehensive color coverage.
 * Uses 3-3-2 base (256 colors) plus ensures good UI color distribution. */
static void sw_setup_8bit_palette(PALETTE pal) {
    int i;

    /* Start with standard 3-3-2 palette (8 red, 8 green, 4 blue levels) */
    generate_332_palette(pal);

    /* Reserve first 16 entries for common UI/text colors to ensure they're exact */
    /* Index 0: keep as black (also MASK_COLOR_8 for transparency) */
    pal[0].r = pal[0].g = pal[0].b = 0;

    /* Index 1-15: Common UI colors - grays and primaries */
    const unsigned char ui_colors[15][3] = {
            {255, 255, 255},  /* 1: white */
            {192, 192, 192},  /* 2: light gray */
            {128, 128, 128},  /* 3: medium gray */
            {64, 64, 64},  /* 4: dark gray */
            {255, 0, 0},  /* 5: red */
            {0, 255, 0},  /* 6: green */
            {0, 0, 255},  /* 7: blue */
            {255, 255, 0},  /* 8: yellow */
            {255, 0, 255},  /* 9: magenta */
            {0, 255, 255},  /* 10: cyan */
            {255, 128, 0},  /* 11: orange */
            {128, 64, 0},  /* 12: brown */
            {255, 192, 192},  /* 13: pink */
            {0, 128, 0},  /* 14: dark green */
            {128, 0, 0},  /* 15: dark red */
    };

    for (i = 0; i < 15; i++) {
        pal[i + 1].r = ui_colors[i][0] >> 2;  /* Allegro stores 0..63 */
        pal[i + 1].g = ui_colors[i][1] >> 2;
        pal[i + 1].b = ui_colors[i][2] >> 2;
    }
}

/* Inline pixel write — zero branches in inner loop for common depths. */
static __inline__ void sw_put_pix(const PixFmt *pf,
        unsigned char *row, int xi,
        int r, int g, int b) {
    if (pf->depth == 16 || pf->depth == 15) {
        ((u16 *) row)[xi] = (u16)(((r >> pf->rw) << pf->rs) |
                ((g >> pf->gw) << pf->gs) |
                ((b >> pf->bw) << pf->bs));
    } else if (pf->depth == 32) {
        ((u32 *) row)[xi] = (u32) makecol_depth(32, r, g, b);
    } else if (pf->depth == 8 && pf->rgb_to_idx) {
        /* Paletted: quantise each channel to SW_PAL_BITS and index the
         * prebuilt nearest-colour table. */
        int rq = r >> SW_PAL_SHIFT;
        int gq = g >> SW_PAL_SHIFT;
        int bq = b >> SW_PAL_SHIFT;
        ((u8 *) row)[xi] =
                pf->rgb_to_idx[((rq * SW_PAL_LUT_SIZE) + gq) * SW_PAL_LUT_SIZE + bq];
        /* Plane-batched target: `row` is a staging buffer, so note that this
         * pixel is one of the ones the flush has to push to VRAM. */
        if (pf->dirty) pf->dirty[xi >> 3] |= (unsigned char) (1 << (xi & 7));
    } else {
        ((u8 *) row)[xi] = (u8) makecol_depth(pf->depth, r, g, b);
        if (pf->dirty) pf->dirty[xi >> 3] |= (unsigned char) (1 << (xi & 7));
    }
}

/* Push one staged scanline into an unchained (mode Y) planar framebuffer.
 *
 * This is the whole reason page-flip is affordable on a planar page. In mode Y
 * one byte of video memory addresses four horizontally adjacent pixels, one in
 * each of the four bit planes, and which of them a write lands in is chosen by
 * the VGA sequencer's Map Mask register (index 2 at port 0x3C4). Writing a
 * pixel therefore costs an `outportw` to select its plane — around a
 * microsecond on a 486 — and doing that per texel (which is what Allegro's own
 * planar putpixel does, and what a direct store into dst->line[y] cannot do at
 * all because the byte offsets do not correspond to pixels) would cost more
 * than the RAM->VRAM blit the flip exists to remove: ~64000 port writes per
 * frame at 320x200.
 *
 * Instead the span was rendered into a system-RAM staging row, and this walks
 * it four times, once per plane, programming the Map Mask exactly once per
 * plane. That is 4 port writes per scanline instead of one per pixel, and each
 * pixel still costs a single byte store. Pixels that failed the z-test are
 * skipped via the dirty mask so they keep whatever the page already held.
 *
 * x0/x1 are the half-open span actually touched; only whole bytes of the mask
 * inside that range are consulted, so the cost scales with the span, not the
 * screen width. */
static void sw_flush_planar_row(BITMAP *dst, int y, int x0, int x1,
        const unsigned char *stage, const unsigned char *dirty) {
    if (x1 <= x0) return;

    bmp_select(dst);
    uintptr_t addr = bmp_write_line(dst, y);

    int p;
    for (p = 0; p < 4; p++) {
        /* Map Mask := this plane only. High byte is the data, low byte the
         * sequencer register index, so one 16-bit port write does both. */
        outportw(0x3C4, (0x0100 << p) | 0x02);

        /* First x >= x0 whose plane is p, then every 4th pixel after it. */
        int x = x0 + (((p - x0) & 3));
        for (; x < x1; x += 4) {
            if (!(dirty[x >> 3] & (1 << (x & 7)))) continue;
            bmp_write8(addr + (unsigned) (x >> 2), stage[x]);
        }
    }

    /* Leave the mask enabling all planes: Allegro's own primitives (and the
     * mouse cursor, clear, blit paths) assume the default state. */
    outportw(0x3C4, 0x0F02);
    bmp_unwrite_line(dst);
}

static void sw_draw_tri(AllegroState *s, BITMAP *dst,
        SwVert v0, SwVert v1, SwVert v2,
        const SwTexture *tex) {
    SwVert *a = &v0, *b = &v1, *c = &v2, *tmp;
    int W = s->sw_zbuf_w, H = s->sw_zbuf_h;

    /* 3D viewport scissor (@viewport): clip rasterisation to the viewport
     * rect, mapped from logical to surface coords (x*ss + offset, the same
     * transform every 2D draw uses). Matches the GL backend's glScissor —
     * the scene still projects for the full surface, only the writes are
     * clipped, and 2D drawn outside the rect presents normally. */
    int sc_x0 = 0, sc_y0 = 0, sc_x1 = W, sc_y1 = H;
    if (s->vp_3d_w > 0 && s->vp_3d_h > 0) {
        sc_x0 = s->vp_3d_x * s->ss + s->offset_x;
        sc_y0 = s->vp_3d_y * s->ss + s->offset_y;
        sc_x1 = (s->vp_3d_x + s->vp_3d_w) * s->ss + s->offset_x;
        sc_y1 = (s->vp_3d_y + s->vp_3d_h) * s->ss + s->offset_y;
        if (sc_x0 < 0) sc_x0 = 0;
        if (sc_y0 < 0) sc_y0 = 0;
        if (sc_x1 > W) sc_x1 = W;
        if (sc_y1 > H) sc_y1 = H;
        if (sc_x1 <= sc_x0 || sc_y1 <= sc_y0) return;
    }
    /* Depth encoding: z = invw * z_scale. invw ranges (0, 1/znear], so
     * scaling by 65535*znear maps the near plane to 65535 and keeps the
     * whole visible range inside 16 bits. Without this, geometry closer
     * than 1.0 world unit overflowed u16 and wrapped to a small depth,
     * making near walls lose the depth test (see-through walls). */
    float z_scale = 65535.0f * (s->znear > 0.0f ? s->znear : 1.0f);
    PixFmt pf = sw_make_pixfmt(s->color_depth);
    pf.rgb_to_idx = s->rgb_to_idx;  /* Use prebuilt 8bpp lookup if available */

    /* Plane-batched output. On an unchained planar page (mode Y, i.e. a
     * page-flip target on a card with no VESA virtual screen) a byte of video
     * memory is four pixels wide, so the usual store into dst->line[y] does not
     * address pixels at all. Render the span into a system-RAM staging row
     * instead and push it out one plane at a time at the end of each scanline
     * (see sw_flush_planar_row). Every other target — RAM bitmaps, linear VESA
     * pages, all the 15/16/32bpp modes — keeps writing straight to dst. */
    const bool planar = (s->planar_backbuf && dst == s->backbuf &&
            s->planar_stage && s->planar_dirty);
    if (planar) pf.dirty = s->planar_dirty;

    /* Sort top→bottom. */
    if (a->sy > b->sy) {
        tmp = a;
        a = b;
        b = tmp;
    }
    if (b->sy > c->sy) {
        tmp = b;
        b = c;
        c = tmp;
    }
    if (a->sy > b->sy) {
        tmp = a;
        a = b;
        b = tmp;
    }

    int y_top = (int) (a->sy + 0.5f);
    int y_mid = (int) (b->sy + 0.5f);
    int y_bot = (int) (c->sy + 0.5f);
    if (y_top == y_bot || y_bot <= 0 || y_top >= H) return;

#ifdef RELIC_FIXED_POINT_3D
/* Per-scanline attribute delta.  Takes the RECIPROCAL of the scanline count
 * and multiplies, rather than dividing by the count directly: all 7 attributes
 * on an edge share the same denominator, so one reciprocal serves all of them.
 * That turns 21 divides per triangle (7 attrs x 3 edge setups) into 3 divides
 * + 21 multiplies.  On the soft-float target a divide costs several times a
 * multiply, and triangle setup -- not pixel fill -- dominates this renderer
 * (at 80x50 a frame is ~300 triangles but only ~5.5k span pixels), so this is
 * squarely on the hot path.  Unlike a fixed-point conversion it changes no
 * ranges or precision characteristics; only the rounding of the last bit.
 * Gated to the fixed-point (soft-float) build only: on a real FPU a divide
 * is not meaningfully more expensive than a multiply, so systems with an
 * FPU keep the direct division below for full floating-point precision. */
#define DLTA(F, P, Q, D) (((Q)->F - (P)->F) * (D))
#else
#define DLTA(F, P, Q, D) (((Q)->F - (P)->F) / (D))
#endif

    float dy_ac = (float) (y_bot - y_top);
    if (dy_ac < 1e-4f) return;
#ifdef RELIC_FIXED_POINT_3D
    float dy_ac_d = 1.0f / dy_ac;
#else
    float dy_ac_d = dy_ac;
#endif

    /* Long-edge (a→c) deltas per scanline. The start values are recomputed
     * per-half at the clamped scanline range below. */
    float ac_dx = DLTA(sx, a, c, dy_ac_d), ac_dw = DLTA(invw, a, c, dy_ac_d);
    float ac_dr = DLTA(r, a, c, dy_ac_d), ac_dg = DLTA(g, a, c, dy_ac_d), ac_db = DLTA(b, a, c, dy_ac_d);
    float ac_du = DLTA(tu, a, c, dy_ac_d), ac_dv = DLTA(tv, a, c, dy_ac_d);
#ifdef RELIC_FIXED_POINT_3D
    /* Fixed-point edge deltas and vertex positions (16.16).  Converting
     * once per triangle costs ~30 FLOAT_TO_FP calls (each one float mul +
     * f2i trap on 486SX), but eliminates ~25 float ops per scanline × ~200
     * scanlines × ~500 triangles = ~2.5M traps/frame. */
    int ac_dx_fp = FLOAT_TO_FP(ac_dx), ac_dw_fp = FLOAT_TO_FP(ac_dw);
    int ac_dr_fp = FLOAT_TO_FP(ac_dr), ac_dg_fp = FLOAT_TO_FP(ac_dg), ac_db_fp = FLOAT_TO_FP(ac_db);
    int ac_du_fp = FLOAT_TO_FP(ac_du), ac_dv_fp = FLOAT_TO_FP(ac_dv);
    int a_sx_fp = FLOAT_TO_FP(a->sx), a_sy_fp = FLOAT_TO_FP(a->sy);
    int a_invw_fp = FLOAT_TO_FP(a->invw), a_r_fp = FLOAT_TO_FP(a->r);
    int a_g_fp = FLOAT_TO_FP(a->g), a_b_fp = FLOAT_TO_FP(a->b);
    int a_tu_fp = FLOAT_TO_FP(a->tu), a_tv_fp = FLOAT_TO_FP(a->tv);
    int z_scale_int = (int) z_scale;
#endif

    /* Hoist pixel-format fields to avoid struct pointer chase per pixel. */
    const int pf_depth = pf.depth;
    const int pf_rs = pf.rs, pf_gs = pf.gs, pf_bs = pf.bs;
    const int pf_rw = pf.rw, pf_gw = pf.gw, pf_bw = pf.bw;

    /* Cache texture fields to avoid struct deref in inner loop. */
    int wm = 0, hm = 0, tw = 0, tw_shift = 0, akey = 0;
    const u8 *tpx = NULL;
    if (tex) {
        wm = tex->w_mask;
        hm = tex->h_mask;
        tw = tex->w;
        tpx = tex->pixels;
        akey = tex->has_alpha;  /* only alpha-test textures that need it */
        /* Compute log2(tw) for fast row indexing: ty*tw = ty<<tw_shift */
        int tmp_w = tw;
        tw_shift = 0;
        while (tmp_w > 1) {
            tw_shift++;
            tmp_w >>= 1;
        }
    }

    int half;
    for (half = 0; half < 2; half++) {
        const int prof_on = g_relic_gfx_prof;
        SwVert *sv = (half == 0) ? a : b;
        SwVert *bv = (half == 0) ? b : c;
        int y0 = (half == 0) ? y_top : y_mid;
        int y1 = (half == 0) ? y_mid : y_bot;
        if (y0 == y1) continue;

        float dys = (float) (y1 - y0);
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

        /* Clamp the scanline range to the screen BEFORE walking. Without
         * this, a vertex projecting far off-screen (large |sy|, or an int
         * overflow from a near-plane-grazing vertex) yields a y range of
         * hundreds of millions of scanlines and the edge walker spins
         * effectively forever (observed as a full hang on DOS). Edge
         * accumulators are linear in y, so evaluate them directly at the
         * clamped start rather than stepping from y_top/y0. */
        int ys = y0 < sc_y0 ? sc_y0 : y0;
        int ye = y1 > sc_y1 ? sc_y1 : y1;
        if (ys >= ye) continue;

#ifdef RELIC_FIXED_POINT_3D
        /* Fixed-point edge accumulators (16.16).  pl/ps are the fractional
         * scanline offsets from the vertex to the first clamped scanline. */
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
        float pl = (float) ys - a->sy + 0.5f;
        float ac_x = a->sx + ac_dx * pl, ac_w = a->invw + ac_dw * pl;
        float ac_r = a->r + ac_dr * pl, ac_g = a->g + ac_dg * pl, ac_b = a->b + ac_db * pl;
        float ac_u = a->tu + ac_du * pl, ac_v = a->tv + ac_dv * pl;

        float ps = (float) ys - sv->sy + 0.5f;
        float sh_x = sv->sx + sh_dx * ps, sh_w = sv->invw + sh_dw * ps;
        float sh_r = sv->r + sh_dr * ps, sh_g = sv->g + sh_dg * ps, sh_b = sv->b + sh_db * ps;
        float sh_u = sv->tu + sh_du * ps, sh_v = sv->tv + sh_dv * ps;
#endif

        int y;
        for (y = ys; y < ye; y++,
#ifdef RELIC_FIXED_POINT_3D
                ac_x += ac_dx_fp, ac_w += ac_dw_fp, ac_r += ac_dr_fp, ac_g += ac_dg_fp, ac_b += ac_db_fp, ac_u += ac_du_fp, ac_v += ac_dv_fp,
                sh_x += sh_dx_fp, sh_w += sh_dw_fp, sh_r += sh_dr_fp, sh_g += sh_dg_fp, sh_b += sh_db_fp, sh_u += sh_du_fp, sh_v += sh_dv_fp) {
#else
                ac_x += ac_dx, ac_w += ac_dw, ac_r += ac_dr, ac_g += ac_dg, ac_b += ac_db, ac_u += ac_du, ac_v += ac_dv,
                sh_x += sh_dx, sh_w += sh_dw, sh_r += sh_dr, sh_g += sh_dg, sh_b += sh_db, sh_u += sh_du, sh_v += sh_dv) {
#endif
            unsigned char *row_ptr = planar ? s->planar_stage : dst->line[y];

#ifdef RELIC_FIXED_POINT_3D
            int lx, rx, lw, rw, lr, rr, lg, rg, lb, rb, lu, ru, lv, rv;
#else
            float lx, rx, lw, rw, lr, rr, lg, rg, lb, rb, lu, ru, lv, rv;
#endif
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
            int x0s = (lx + (1 << 15)) >> 16;
            if (x0s < sc_x0) x0s = sc_x0;
            int x1s = (rx + (1 << 15)) >> 16;
#else
            int x0s = (int) (lx + 0.5f);
            if (x0s < sc_x0) x0s = sc_x0;
            int x1s = (int) (rx + 0.5f);
#endif
            if (x1s > sc_x1) x1s = sc_x1;
            int span = x1s - x0s;
            if (span <= 0) continue;
            if (prof_on) {
                g_prof.scanlines++;
                g_prof.span_pixels += (unsigned long) span;
            }

            /* Staging row carries over from the previous scanline, so clear the
             * dirty bits covering this span before anything writes into it.
             * Only the bytes the span touches are cleared, so this costs
             * span/8 bytes rather than a full-width memset per scanline. */
            if (planar) {
                int db0 = x0s >> 3;
                int db1 = (x1s + 7) >> 3;
                memset(s->planar_dirty + db0, 0, (size_t) (db1 - db0));
            }

            /* Per-scanline linear deltas. Normalise by the TRUE edge width
             * (rx - lx), not the clamped on-screen span: when a triangle
             * runs off the left/right of the screen, x0s/x1s are clamped so
             * `span` is shorter than the real edge. Using `span` here would
             * inflate the per-pixel step, over-interpolating depth and UV
             * (streaky textures + walls losing the z-test = see-through). */
#ifdef RELIC_FIXED_POINT_3D
            /* All per-scanline math in 16.16 fixed-point — no FPU traps.
             * z is computed via 64-bit intermediate: iz = (c_w * z_scale) >> 16
             * where z_scale is a plain int (not fixed-point). */
            int edge_span = rx - lx;
            int inv_sp = (edge_span > 0) ? fixrcp_i(edge_span) : ((1 << 16) / (span > 0 ? span : 1));
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

            /* c_w/d_w/c_u/d_u/c_v/d_v are already 16.16 fixed-point — the
             * sub-span UV loop uses them directly, no separate _fp copies. */
            int c_w_fp = c_w, d_w_fp = d_w;
            int c_u_fp = c_u, d_u_fp = d_u;
            int c_v_fp = c_v, d_v_fp = d_v;
#else
            float edge_span = rx - lx;
            float inv_sp = (edge_span > 1e-6f) ? (1.0f / edge_span)
                    : (1.0f / (float) span);
            float fxp = (float) x0s - lx + 0.5f;

            /* Z: use float accumulator, convert to u16 per pixel. */
            float d_w = (rw - lw) * inv_sp;
            float c_w = lw + d_w * fxp;

            /* Gouraud: 16.16 fixed-point R,G,B. */
            int idr = FLOAT_TO_FP((rr - lr) * inv_sp);
            int idg = FLOAT_TO_FP((rg - lg) * inv_sp);
            int idb = FLOAT_TO_FP((rb - lb) * inv_sp);
            int icr = FLOAT_TO_FP(lr + (rr - lr) * inv_sp * fxp);
            int icg = FLOAT_TO_FP(lg + (rg - lg) * inv_sp * fxp);
            int icb = FLOAT_TO_FP(lb + (rb - lb) * inv_sp * fxp);

            /* UV: float accumulators for perspective divide at sub-span ends. */
            float d_u = (ru - lu) * inv_sp, d_v = (rv - lv) * inv_sp;
            float c_u = lu + d_u * fxp, c_v = lv + d_v * fxp;
#endif

            u16 *zrow = s->sw_zbuf + y * W;
            int x = x0s;

            if (tpx) {
                /* Textured path: perspective UV every SW_SPAN pixels. */
                /* Convert z accumulator to integer steps to avoid float mul per pixel. */
#ifdef RELIC_FIXED_POINT_3D
                int iz = (int) (((long long) c_w * z_scale_int) >> 16);
                int idz = (int) (((long long) d_w * z_scale_int) >> 16);
#else
                int iz = (int) (c_w * z_scale);
                int idz = (int) (d_w * z_scale);
#endif

                while (x < x1s) {
                    int xe = x + SW_SPAN;
                    if (xe > x1s) xe = x1s;
                    int sub = xe - x;

                    /* Hoist Gouraud colour — once per span. */
                    int pr = icr >> FP_SHIFT;
                    if (pr < 0) pr = 0;
                    if (pr > 255) pr = 255;
                    int pg = icg >> FP_SHIFT;
                    if (pg < 0) pg = 0;
                    if (pg > 255) pg = 255;
                    int pb = icb >> FP_SHIFT;
                    if (pb < 0) pb = 0;
                    if (pb > 255) pb = 255;
                    icr += idr * sub;
                    icg += idg * sub;
                    icb += idb * sub;

                    int xi;
                    {
#ifdef RELIC_FIXED_POINT_3D
                        /* Fixed-point perspective UV — no FPU on 386SX.
                         * fixrcp_i computes 1/x via 64-bit integer divide
                         * (clamped to 32767.0); FIXMUL_I does 16.16×16.16
                         * multiply via 64-bit intermediate.  The sub-span
                         * affine approximation makes the reciprocal
                         * precision more than adequate (8px spans). */
                        int w0 = fixrcp_i(c_w_fp);
                        int ew_fp = c_w_fp + (int)((long long)d_w_fp * sub);
                        int w1 = fixrcp_i(ew_fp);
                        int iu0 = FIXMUL_I(c_u_fp, w0);
                        int iv0 = FIXMUL_I(c_v_fp, w0);
                        int eu = c_u_fp + (int)((long long)d_u_fp * sub);
                        int ev = c_v_fp + (int)((long long)d_v_fp * sub);
                        int iu1 = FIXMUL_I(eu, w1);
                        int iv1 = FIXMUL_I(ev, w1);
#else
                        /* Full perspective-correct texture (LOD removed). */
                        float w0 = (c_w > 1e-9f) ? 1.0f / c_w : 1e9f;
                        if (w0 > 32767.0f) w0 = 32767.0f;
                        float ew = c_w + d_w * (float) sub;
                        float w1 = (ew > 1e-9f) ? 1.0f / ew : 1e9f;
                        if (w1 > 32767.0f) w1 = 32767.0f;
                        int iu0 = FLOAT_TO_FP(c_u * w0);
                        int iv0 = FLOAT_TO_FP(c_v * w0);
                        int iu1 = FLOAT_TO_FP((c_u + d_u * (float) sub) * w1);
                        int iv1 = FLOAT_TO_FP((c_v + d_v * (float) sub) * w1);
#endif
                        int idu = (sub > 1) ? (iu1 - iu0) / sub : 0;
                        int idv = (sub > 1) ? (iv1 - iv0) / sub : 0;
                        int iu = iu0, iv = iv0;
                        if (pr >= 255 && pg >= 255 && pb >= 255 && pf_depth == 16) {
                            /* Hottest path: full-bright + 16bpp — direct u16 write, no clamp needed. */
                            u16 *row16 = (u16 *) row_ptr;
                            for (xi = x; xi < xe; xi++, iu += idu, iv += idv, iz += idz) {
                                u16 nz = (iz >= 65535) ? 65535u : (iz <= 0 ? 0u : (u16) iz);
                                if (nz <= zrow[xi]) continue;
                                int tx = (iu >> FP_SHIFT) & wm;
                                int ty = (iv >> FP_SHIFT) & hm;
                                const u8 *px = tpx + (((ty << tw_shift) + tx) << 2);
                                /* Alpha-test: transparent texels (sprite cutout)
                                 * neither draw nor write depth. akey is a
                                 * loop-invariant so opaque walls/floors skip
                                 * the px[3] read entirely (short-circuit). */
                                if (akey && px[3] == 0) continue;
                                zrow[xi] = nz;
                                row16[xi] = (u16)(((px[0] >> pf_rw) << pf_rs) | ((px[1] >> pf_gw) << pf_gs) | (
                                        (px[2] >> pf_bw) << pf_bs));
                            }
                        } else if (pr >= 255 && pg >= 255 && pb >= 255) {
                            for (xi = x; xi < xe; xi++, iu += idu, iv += idv, iz += idz) {
                                u16 nz = (iz >= 65535) ? 65535u : (iz <= 0 ? 0u : (u16) iz);
                                if (nz <= zrow[xi]) continue;
                                int tx = (iu >> FP_SHIFT) & wm;
                                int ty = (iv >> FP_SHIFT) & hm;
                                const u8 *px = tpx + (((ty << tw_shift) + tx) << 2);
                                if (akey && px[3] == 0) continue;
                                zrow[xi] = nz;
                                sw_put_pix(&pf, row_ptr, xi, px[0], px[1], px[2]);
                            }
                        } else {
                            for (xi = x; xi < xe; xi++, iu += idu, iv += idv, iz += idz) {
                                u16 nz = (iz >= 65535) ? 65535u : (iz <= 0 ? 0u : (u16) iz);
                                if (nz <= zrow[xi]) continue;
                                int tx = (iu >> FP_SHIFT) & wm;
                                int ty = (iv >> FP_SHIFT) & hm;
                                const u8 *px = tpx + (((ty << tw_shift) + tx) << 2);
                                if (akey && px[3] == 0) continue;
                                zrow[xi] = nz;
                                sw_put_pix(&pf, row_ptr, xi,
                                        (px[0] * pr) >> 8, (px[1] * pg) >> 8, (px[2] * pb) >> 8);
                            }
                        }
                    }
#ifdef RELIC_FIXED_POINT_3D
                    c_w_fp += (int)((long long)d_w_fp * sub);
                    c_u_fp += (int)((long long)d_u_fp * sub);
                    c_v_fp += (int)((long long)d_v_fp * sub);
#else
                    c_w += d_w * (float) sub;
                    c_u += d_u * (float) sub;
                    c_v += d_v * (float) sub;
#endif
                    x = xe;
                }
            } else {
                /* Untextured path: pure fixed-point, no UV at all.
                 * Walk in SW_SPAN chunks so colour is hoisted out of the
                 * inner pixel loop (same pattern as the textured path).
                 * For 16bpp (the common DOS case) pack the colour once per
                 * span and do a direct u16 write — avoids the sw_put_pix
                 * function call overhead that was making this path slower
                 * than the textured full-bright path. */
#ifdef RELIC_FIXED_POINT_3D
                int iz = (int) (((long long) c_w * z_scale_int) >> 16);
                int idz = (int) (((long long) d_w * z_scale_int) >> 16);
#else
                int iz = (int) (c_w * z_scale);
                int idz = (int) (d_w * z_scale);
#endif
                int x = x0s;
                if (pf_depth == 16 || pf_depth == 15) {
                    u16 *row16 = (u16 *) row_ptr;
                    while (x < x1s) {
                        int xe = x + SW_SPAN;
                        if (xe > x1s) xe = x1s;
                        int sub = xe - x;
                        int pr = icr >> FP_SHIFT;
                        if (pr < 0) pr = 0;
                        if (pr > 255) pr = 255;
                        int pg = icg >> FP_SHIFT;
                        if (pg < 0) pg = 0;
                        if (pg > 255) pg = 255;
                        int pb = icb >> FP_SHIFT;
                        if (pb < 0) pb = 0;
                        if (pb > 255) pb = 255;
                        icr += idr * sub;
                        icg += idg * sub;
                        icb += idb * sub;
                        u16 packed = (u16)(((pr >> pf_rw) << pf_rs) |
                                ((pg >> pf_gw) << pf_gs) |
                                ((pb >> pf_bw) << pf_bs));
                        int xi;
                        for (xi = x; xi < xe; xi++, iz += idz) {
                            u16 nz = (iz >= 65535) ? 65535u : (iz <= 0 ? 0u : (u16) iz);
                            if (nz <= zrow[xi]) continue;
                            zrow[xi] = nz;
                            row16[xi] = packed;
                        }
                        x = xe;
                    }
                } else {
                    int xi;
                    for (xi = x; xi < x1s; xi++,
                            iz += idz, icr += idr, icg += idg, icb += idb) {
                        u16 nz = (iz >= 65535) ? 65535u : (iz <= 0 ? 0u : (u16) iz);
                        if (nz <= zrow[xi]) continue;
                        zrow[xi] = nz;
                        int pr = icr >> FP_SHIFT;
                        if (pr < 0) pr = 0;
                        if (pr > 255) pr = 255;
                        int pg = icg >> FP_SHIFT;
                        if (pg < 0) pg = 0;
                        if (pg > 255) pg = 255;
                        int pb = icb >> FP_SHIFT;
                        if (pb < 0) pb = 0;
                        if (pb > 255) pb = 255;
                        sw_put_pix(&pf, row_ptr, xi, pr, pg, pb);
                    }
                }
            }

            /* Scanline complete: push the staged pixels into the planar page,
             * four plane passes for the whole span rather than a plane select
             * per pixel. */
            if (planar)
                sw_flush_planar_row(dst, y, x0s, x1s,
                        s->planar_stage, s->planar_dirty);
        }
    }
#undef DLTA
}

#undef FP_SHIFT
#undef FP_ONE
#undef FLOAT_TO_FP
#ifdef RELIC_FIXED_POINT_3D
#undef FIXMUL_I
#undef FIXRCP_MAX
#endif

/* Near-plane (w >= znear) Sutherland-Hodgman clip in clip space.
 * Returns output vertex count (0..SW_MAXCLIP). */
#define SW_MAXCLIP 8
typedef struct {
    float x, y, z, w, r, g, b, tu, tv;
} SwClipVert;

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
            if (t < 0) t = 0; else if (t > 1) t = 1;
            SwClipVert cv;
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

/* -------------------------------------------------------------------------
 * gfx_camera
 * ---------------------------------------------------------------------- */
/* Page-flip is a 2D-only present mode on a linear VRAM page.
 *
 * Two independent reasons, one correctness and one performance.
 *
 * Correctness: every 2D primitive goes through Allegro (blit/putpixel/textout),
 * which reaches a video bitmap via bmp_select()/bmp_write*(). The SW 3D
 * rasteriser does not — sw_draw_tri() takes `row_ptr = dst->line[y]` and stores
 * through it directly, which is only a valid near pointer for a memory bitmap.
 * On a linear VESA page the line[] entries are offsets inside the framebuffer's
 * far segment, so storing through them writes to low `ds` addresses, which
 * DJGPP keeps unmapped as null-pointer protection. Confirmed in DOSBox
 * (VBE OEM "S3 INCORPORATED. TRIO64", so not the Oak fault): SIGSEGV with
 * error=0006 (write to a not-present page) and edi=0x3000, a framebuffer row
 * offset. The title screen survives only because it is a single @sprite.
 *
 * Performance: fixing that by rasterising straight into VRAM would still be the
 * wrong trade for this renderer. Flip earns its keep when each pixel is written
 * about once (wdoom's column renderer), because it removes the whole RAM->VRAM
 * blit. tdoom is a brute-force z-buffer renderer with no culling that submits
 * the entire level per frame and draws the sky first to be overdrawn, so it
 * writes ~2.5x per pixel. Paying that in VRAM costs more than paying it in
 * cached RAM plus one bulk blit — measured VRAM bandwidth on a 486SX-25 is
 * ~1.2 MB/s (the ~52 ms 64 KB blit noted in allegro_gfx_flip), which makes
 * 2.5x scattered VRAM writes a clear loss against a 1.0x bulk copy. A
 * zero-copy flip only becomes worthwhile if overdraw drops to ~1.0 via
 * front-to-back submission and frustum culling.
 *
 * Planar (mode X / mode Y) pages are deliberately left flipping. There the
 * rasteriser already renders into a system-RAM staging row and pushes it out
 * through sw_flush_planar_row(), which uses bmp_select()/bmp_write8() properly,
 * so that path is both correct and write-combined per scanline — and it is the
 * one that matters on real 486-era hardware, where the card often has no
 * linear framebuffer at all.
 *
 * @camera is the hook because allegro_gfx_mesh() refuses to draw until
 * have_camera is set, so no triangle can reach VRAM before this runs. A no-op
 * unless a linear page-flip is currently active, so calling it once per frame
 * costs a single predictable branch. */
static void allegro_pageflip_disable_for_3d(AllegroState *s) {
    if (!s || !s->page_flip || s->planar_backbuf) return;

    /* The pages are the render size, which is also what the z-buffer was sized
     * to, so the replacement matches and sw_zbuf can stay as it is. */
    int w = s->sw_zbuf_w, h = s->sw_zbuf_h;
    if (w <= 0 || h <= 0) return;

    /* Allocate the replacement first: if there is not enough memory for it,
     * keep flipping rather than end up with no backbuffer at all. */
    BITMAP *ram = create_bitmap_ex(s->color_depth, w, h);
    if (!ram) {
        allegro_mode_log("DOS: gfx: 3D with linear page-flip needs a RAM "
                         "backbuffer, but allocation failed; keeping flip\n");
        return;
    }
    clear_to_color(ram, makecol(0, 0, 0));

    /* Put the CRTC back at the start of the framebuffer while the pages are
     * still valid, so what stays on screen is the memory `screen` itself owns
     * rather than a start address that is about to be freed. */
    if (s->vpage[0]) show_video_bitmap(s->vpage[0]);

    /* backbuf aliases one of the two pages, so destroying the pages disposes
     * of the old backbuffer too — do not destroy it separately. */
    if (s->vpage[0]) destroy_bitmap(s->vpage[0]);
    if (s->vpage[1]) destroy_bitmap(s->vpage[1]);
    s->vpage[0] = s->vpage[1] = NULL;
    s->vpage_active = 0;
    s->page_flip = false;
    s->backbuf = ram;

    /* Only the planar path uses these, and that path is gone with the flip. */
    if (s->planar_stage) {
        free(s->planar_stage);
        s->planar_stage = NULL;
    }
    if (s->planar_dirty) {
        free(s->planar_dirty);
        s->planar_dirty = NULL;
    }

    /* Allegro stores the clip rectangle per bitmap, so the script's @clip has
     * to be re-applied to the bitmap that just became the drawing target. */
    allegro_apply_clip(s);
    allegro_mode_log("DOS: gfx: page-flip disabled for 3D (linear framebuffer "
                     "cannot take direct rasteriser writes); using RAM "
                     "backbuffer + blit\n");
}

static void allegro_gfx_camera(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) return;

    allegro_pageflip_disable_for_3d(s);

    /* 8bpp: build the optimised palette from sampled texture colours on the
     * first @camera() call. By this point all textures have been uploaded
     * (scripts load textures before entering the render loop). This defers
     * the ~255ms palette build from @window time (where no textures exist
     * yet) to here, where the samples are available. The 3-3-2 palette set
     * at @window time remains in effect until this runs. */
    if (s->color_depth == 8 && !s->sw_pal_built) {
        sw_build_optimised_palette(s);
    }
    u32 eye_ptr = (u32) vm->r[1];
    u32 tgt_ptr = (u32) vm->r[2];
    u32 up_ptr = (u32) vm->r[3];
    AllegroFBits fb;
    fb.i = vm->r[4];
    float fov_deg = fb.f;
    fb.i = vm->r[5];
    float znear = fb.f;
    fb.i = vm->r[6];
    float zfar = fb.f;

    float eye[3], tgt[3], up[3];
    alg_read_vec3(vm, eye_ptr, eye);
    alg_read_vec3(vm, tgt_ptr, tgt);
    alg_read_vec3(vm, up_ptr, up);

    /* A negative FOV requests a horizontally mirrored
     * (left-handed) view. Swap the projection X scale to reflect
     * horizontally, and flip back-face culling winding. */
    s->cam_mirror = false;
    if (fov_deg < 0.0f) {
        s->cam_mirror = true;
        fov_deg = -fov_deg;
    }
    if (fov_deg <= 0.0f) fov_deg = 60.0f;
    if (znear <= 0.0f) znear = 0.1f;
    if (zfar <= znear) zfar = znear + 1000.0f;

    s->znear = znear;
    s->zfar = zfar;

    /* Perspective projection — same formula as the GL backend.
     * In stretch mode the render surface is logical_w x logical_h,
     * so aspect must be computed from those dimensions.
     * The 3D viewport is a pure display clip (a scissor rect, like the
     * GL backend's glScissor): the scene is always projected for the
     * full surface and the flip blit shows only the viewport rect, so
     * aspect/center must NOT come from the viewport.
     *
     * Projection matrix cache: FOV/aspect/znear/zfar rarely change between
     * frames (tdoom never changes them after startup).  Skip the tanf()
     * + divisions when the parameters match the cached values.  On a 486SX
     * the tanf alone costs ~50ms via -lemu. */
    int vp_w = s->stretch_enabled ? s->logical_w : s->width;
    int vp_h = s->stretch_enabled ? s->logical_h : s->height;
    float aspect = (vp_h > 0) ? (float) vp_w / (float) vp_h : 1.0f;

    if (!s->proj_valid || aspect != s->proj_aspect || fov_deg != s->proj_fov_deg ||
        znear != s->proj_znear || zfar != s->proj_zfar || s->cam_mirror != s->proj_mirror) {
        float fov_rad = fov_deg * (3.14159265358979f / 180.0f);
        float f = 1.0f / tanf(fov_rad * 0.5f);
        memset(s->proj, 0, sizeof(s->proj));
        s->proj[0] = s->cam_mirror ? -(f / aspect) : (f / aspect);
        s->proj[5] = f;
        s->proj[10] = (zfar + znear) / (znear - zfar);
        s->proj[11] = -1.0f;
        s->proj[14] = (2.0f * zfar * znear) / (znear - zfar);
        s->proj_valid = true;
        s->proj_fov_deg = fov_deg;
        s->proj_aspect = aspect;
        s->proj_znear = znear;
        s->proj_zfar = zfar;
        s->proj_mirror = s->cam_mirror;
    }

    /* Right-handed look-at — matches the GL backend exactly.
     *
     * These two sqrtf calls stay in float even on the soft-float target.
     * An integer-sqrt version was tried and reverted: the view basis has
     * to stay orthonormal, and a 24.8 fixed-point sqrt quantises the
     * magnitude to 1/16 (so a unit vector can come back as 0.9375) and
     * overflows int32 once |target-eye| gets large, which corrupts the
     * whole view matrix. Two sqrts per frame is not worth that risk. */
    float fx = tgt[0] - eye[0], fy = tgt[1] - eye[1], fz = tgt[2] - eye[2];
    float fl = sqrtf(fx * fx + fy * fy + fz * fz);
    if (fl < 1e-6f) fl = 1.0f;
    fx /= fl;
    fy /= fl;
    fz /= fl;
    float sx = fy * up[2] - fz * up[1];
    float sy = fz * up[0] - fx * up[2];
    float sz = fx * up[1] - fy * up[0];
    float sl = sqrtf(sx * sx + sy * sy + sz * sz);
    if (sl < 1e-6f) sl = 1.0f;
    sx /= sl;
    sy /= sl;
    sz /= sl;
    float ux = sy * fz - sz * fy;
    float uy = sz * fx - sx * fz;
    float uz = sx * fy - sy * fx;
    s->view[0] = sx;
    s->view[4] = sy;
    s->view[8] = sz;
    s->view[12] = -(sx * eye[0] + sy * eye[1] + sz * eye[2]);
    s->view[1] = ux;
    s->view[5] = uy;
    s->view[9] = uz;
    s->view[13] = -(ux * eye[0] + uy * eye[1] + uz * eye[2]);
    s->view[2] = -fx;
    s->view[6] = -fy;
    s->view[10] = -fz;
    s->view[14] = (fx * eye[0] + fy * eye[1] + fz * eye[2]);
    s->view[3] = 0;
    s->view[7] = 0;
    s->view[11] = 0;
    s->view[15] = 1;

    s->have_camera = true;
    /* Cache MVP = proj * view so @mesh calls don't recompute it. */
    alg_mat4_mul(s->proj, s->view, s->mvp);

    /* Clear z-buffer for the new frame. */
    if (s->sw_zbuf)
        memset(s->sw_zbuf, 0, (size_t) s->sw_zbuf_w * (size_t) s->sw_zbuf_h * sizeof(u16));
    s->frame_dirty = true;
}

/* @palette_upload(ptr_rgb888_or_0, count) - select an 8bpp palette strategy.
 * count == 0            -> SW_PAL_FLAT  (flat 3-3-2, general use)
 * count > 0, ptr != 0   -> SW_PAL_CUSTOM (exact RGB888 table, e.g. PLAYPAL)
 * Not calling this leaves SW_PAL_AUTO (default: texture-sampled median-cut,
 * built at the first @camera() call). No-op on non-8bpp modes. */
static void allegro_gfx_palette_upload(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) return;

    /* The @lines3d fill renderer (sdfill.h) reads the same upload as
     * RGBA quads regardless of the 8bpp strategy below - feed it first
     * so the palette hook means the same thing on every backend. */
    {
        u32 q = (u32) vm->r[1];
        i32 n = vm->r[2];
        if (n > 16) n = 16;
        if (q && n > 0 && (u64)q + (u64)n * 4u <= vm->mem_sz)
            sdf_palette(vm->mem + q, n);
    }

    if (s->color_depth != 8) return;

    u32 ptr = (u32) vm->r[1];
    i32 count = vm->r[2];

    if (count <= 0) {
        s->sw_pal_mode = SW_PAL_FLAT;
        if (!s->sw_pal_built) {
            s->sw_pal_built = true;
            sw_finalize_flat_palette(s);
        }
        return;
    }

    if (count > 256) count = 256;
    if ((uint64_t) ptr + (uint64_t) count * 3ull > (uint64_t) vm->mem_sz) return;

    memcpy(s->sw_pal_custom, vm->mem + ptr, (size_t) count * 3u);
    s->sw_pal_custom_count = count;
    s->sw_pal_mode = SW_PAL_CUSTOM;
    if (!s->sw_pal_built) {
        s->sw_pal_built = true;
        sw_build_custom_palette(s);
    }
}

/* Grow the texture slot table to at least `want` slots (or fail: the
 * request exceeded RELIC_MAX_TEXTURES / ran out of memory). New slots
 * are zeroed so the scan below sees them as free. Lazily allocates the
 * table on first use. */
static bool allegro_tex_reserve(AllegroState *s, int want) {
    if (!s->textures) {
        s->textures = (SwTexture *) calloc(RELIC_TEX_CAP_INIT, sizeof(SwTexture));
        if (!s->textures) return false;
        s->tex_cap = RELIC_TEX_CAP_INIT;
    }
    if (want > s->tex_cap) {
        int nc = s->tex_cap;
        while (nc < want) nc <<= 1;
        if (nc > RELIC_MAX_TEXTURES) nc = RELIC_MAX_TEXTURES;
        if (want > nc) return false;
        SwTexture *nt = (SwTexture *) realloc(s->textures, (size_t) nc * sizeof(SwTexture));
        if (!nt) return false;
        memset(nt + s->tex_cap, 0, (size_t) (nc - s->tex_cap) * sizeof(SwTexture));
        s->textures = nt;
        s->tex_cap = nc;
    }
    return true;
}

static void allegro_gfx_texture(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) {
        vm->r[1] = 0;
        return;
    }

    u32 pixels_ptr = (u32) vm->r[1];
    i32 w = vm->r[2], h = vm->r[3];
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
        vm->r[1] = 0;
        return;
    }
    if ((uint64_t) pixels_ptr + (uint64_t) w * (uint64_t) h * 4ull > (uint64_t) vm->mem_sz) {
        vm->r[1] = 0;
        return;
    }

    if (!allegro_tex_reserve(s, RELIC_TEX_CAP_INIT)) {
        vm->r[1] = 0;
        return;
    }
    int slot = 0, i;
    for (i = 1; i < s->tex_cap; i++) {
        if (!s->textures[i].pixels) {
            slot = i;
            break;
        }
    }
    if (!slot && allegro_tex_reserve(s, s->tex_cap + 1)) {
        for (i = 1; i < s->tex_cap; i++) {
            if (!s->textures[i].pixels) {
                slot = i;
                break;
            }
        }
    }
    if (!slot) {
        vm->r[1] = 0;
        return;
    }

    /* Round up to POT for bitmask wrap. */
    int pw = 1;
    while (pw < w) pw <<= 1;
    int ph = 1;
    while (ph < h) ph <<= 1;

    u8 *buf = (u8 *) malloc((size_t) pw * (size_t) ph * 4u);
    if (!buf) {
        vm->r[1] = 0;
        return;
    }

    const u8 *src = vm->mem + pixels_ptr;
    int x, y, has_alpha = 0;
    for (y = 0; y < ph; y++) {
        int sy = y % h;
        for (x = 0; x < pw; x++) {
            int sx = x % w;
            int os = (sy * w + sx) * 4, od = (y * pw + x) * 4;
            buf[od + 0] = src[os + 0];
            buf[od + 1] = src[os + 1];
            buf[od + 2] = src[os + 2];
            buf[od + 3] = src[os + 3];
            if (src[os + 3] < 255) has_alpha = 1;
        }
    }
    s->textures[slot].pixels = buf;
    s->textures[slot].w = pw;
    s->textures[slot].h = ph;
    s->textures[slot].src_w = w;
    s->textures[slot].src_h = h;
    s->textures[slot].w_mask = pw - 1;
    s->textures[slot].h_mask = ph - 1;
    s->textures[slot].has_alpha = has_alpha;

    /* 8bpp: sample this texture's colours for the optimised palette.
     * The palette is built at the first @camera() call, after all textures
     * have been uploaded. Sampling here is cheap (every 4th pixel, ~5ms
     * total for a typical WAD map) and the buffer is only allocated once
     * at @window time when 8bpp mode is selected. */
    if (s->color_depth == 8 && s->sw_col_samples && !s->sw_pal_built) {
        sw_sample_texture_colours(s, buf, pw, ph);
    }

    /* Build a cached Allegro BITMAP at screen color depth for fast blit().
     * This converts RGBA32 to the screen's native format once at upload
     * time, so sprite blits can use Allegro's optimized blit()/draw_sprite()
     * (memcpy-like) instead of per-pixel putpixel() which is extremely slow
     * on DOS VESA.
     *
     * For textures with alpha, mostly-transparent texels (alpha < 128) are
     * written as the bitmap mask color (palette index 0 in 8bpp). This lets
     * draw_sprite()/masked_blit() skip them automatically. Texels at 128+
     * become opaque: draw_sprite() cannot blend, and treating alpha < 255
     * as transparent erased every antialiased edge pixel, leaving sprites
     * moth-eaten. Alpha-test at 128 is the usual cutoff. */
    s->textures[slot].bmp = NULL;
    if (s->color_depth > 0) {
        BITMAP *tbmp = create_bitmap_ex(s->color_depth, w, h);
        if (tbmp) {
            int mc = bitmap_mask_color(tbmp);
            int bx, by;
            for (by = 0; by < h; by++) {
                for (bx = 0; bx < w; bx++) {
                    int bo = (by * pw + bx) * 4;
                    if (buf[bo + 3] < 128) {
                        putpixel(tbmp, bx, by, mc);
                    } else {
                        putpixel(tbmp, bx, by,
                                 makecol_depth(s->color_depth,
                                               buf[bo], buf[bo + 1], buf[bo + 2]));
                    }
                }
            }
            s->textures[slot].bmp = tbmp;
        }
    }

    vm->r[1] = (i32) slot;
    s->frame_dirty = true;
}

static void allegro_gfx_light(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) return;

    u32 dir_ptr = (u32) vm->r[1];
    u32 rgb_ptr = (u32) vm->r[2];
    u32 amb_ptr = (u32) vm->r[3];

    if (dir_ptr == 0) {
        s->light_on = false;
        return;
    }

    float dir[3];
    alg_read_vec3(vm, dir_ptr, dir);
    float dl = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (dl < 1e-6f) dl = 1.0f;
    s->light_dir[0] = dir[0] / dl;
    s->light_dir[1] = dir[1] / dl;
    s->light_dir[2] = dir[2] / dl;

    s->light_rgb[0] = 1;
    s->light_rgb[1] = 1;
    s->light_rgb[2] = 1;
    if (rgb_ptr + 3 <= vm->mem_sz) {
        s->light_rgb[0] = vm->mem[rgb_ptr] / 255.0f;
        s->light_rgb[1] = vm->mem[rgb_ptr + 1] / 255.0f;
        s->light_rgb[2] = vm->mem[rgb_ptr + 2] / 255.0f;
    }

    s->light_amb[0] = 0.1f;
    s->light_amb[1] = 0.1f;
    s->light_amb[2] = 0.1f;
    if (amb_ptr + 3 <= vm->mem_sz) {
        s->light_amb[0] = vm->mem[amb_ptr] / 255.0f;
        s->light_amb[1] = vm->mem[amb_ptr + 1] / 255.0f;
        s->light_amb[2] = vm->mem[amb_ptr + 2] / 255.0f;
    }

    s->light_on = true;
    s->frame_dirty = true;
}

/* @text(text, x, y, size, weight, font_name, justify, color)
 * TrueType rendering via stb_truetype baked atlas with alpha blending
 * onto the backbuffer. Mirrors the GL backend's layout engine but uses
 * per-pixel alpha blending instead of textured quads. */
/* Draw a string with explicit args. Shared by @text and draw-list
 * DL_TEXT records; x/y are logical top-left coords. */
static void allegro_text_draw(RelicVM *vm, u32 text_ptr, i32 x, i32 y,
                              i32 size, i32 weight, u32 fname_ptr,
                              i32 justify, int col_r, int col_g,
                              int col_b, int col_a) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s || !screen) return;

    if (text_ptr >= vm->mem_sz) return;

    /* Pull string from VM memory. */
    char text_buf[1024];
    int n = 0;
    while (text_ptr < vm->mem_sz && n < (int) sizeof(text_buf) - 1) {
        u8 c = vm->mem[text_ptr++];
        if (c == 0) break;
        text_buf[n++] = (char) c;
    }
    text_buf[n] = 0;

    if (!allegro_font_init(s) || s->nfaces <= 0) {
        /* One-time diagnostic for the fixed-font regression seen on
         * FreeDOS after many windows: font_init=1 with nfaces=0 means the
         * font scan found nothing (resource/handle exhaustion); anything
         * else points at state corruption. */
        static int font_fallback_warned = 0;
        if (!font_fallback_warned) {
            font_fallback_warned = 1;
            allegro_console_open_log();
            if (g_console_log) {
                fprintf(g_console_log,
                        "DOS: @text fallback to built-in font - font_init=%d nfaces=%d\n",
                        (int)s->font_init, s->nfaces);
                fflush(g_console_log);
            }
        }
        /* Fallback: Allegro built-in font (ignores size/weight/justify) */
        int col = makecol(col_r, col_g, col_b);
        textout_ex(DRAW_TGT(s), font, text_buf,
                x * s->ss + s->offset_x, y * s->ss + s->offset_y, col, -1);
        s->frame_dirty = true;
        return;
    }

    char fname_buf[64] = "";
    if (fname_ptr && fname_ptr < vm->mem_sz) {
        int fn = 0;
        while (fname_ptr + fn < vm->mem_sz && fn < (int) sizeof(fname_buf) - 1) {
            u8 c = vm->mem[fname_ptr + fn];
            if (c == 0) break;
            fname_buf[fn++] = (char) c;
        }
        fname_buf[fn] = 0;
    }

    int face_idx = select_face(s, fname_buf, weight);
    if (face_idx < 0 || !s->faces[face_idx].init) return;



    /* Select best bake size for requested pixel size (scaled by ss for SSAA). */
    float pixel_size = (size > 0) ? (float) size : s->faces[face_idx].bake_pixels[0];
    pixel_size *= (float) s->ss;
    int sel_i = select_bake_size(s, face_idx, pixel_size);
    float sel_bp = s->faces[face_idx].bake_pixels[sel_i];
    float scale = pixel_size / sel_bp;

    const stbtt_bakedchar *sel_cdata = s->faces[face_idx].cdata[sel_i];
    const unsigned char *sel_atlas = s->faces[face_idx].atlas[sel_i];
    int sel_atlas_size = s->faces[face_idx].atlas_size[sel_i];
    float sel_pad = (float)s->faces[face_idx].sdf_padding;
    bool is_bitmap = (s->faces[face_idx].sdf_padding == 0);
    int is_pixel = s->faces[face_idx].is_pixel_font ? 1 : 0;

    float baked_w = 0, baked_above = 0, baked_below = 0, baked_min_x = 0;
    measure_text_baked(sel_cdata, s->faces[face_idx].first_char,
                       s->faces[face_idx].glyph_count, sel_pad,
                       text_buf, &baked_w, &baked_above, &baked_below, &baked_min_x);
    float box_w = baked_w * scale;
    float box_h = (baked_above + baked_below) * scale;
    float above = baked_above * scale;

    int j = (justify >= 0 && justify <= 8) ? justify : 0;
    int h_mode = (j <= 2) ? 0 : (j <= 5 ? 1 : 2);
    int v_mode;
    switch (j) {
        case 0:
        case 3:
        case 6:
            v_mode = 0;
            break;
        case 1:
        case 4:
        case 7:
            v_mode = 1;
            break;
        case 2:
        case 5:
        case 8:
            v_mode = 2;
            break;
        default:
            v_mode = 0;
    }

    float fx = (float) x * (float) s->ss;
    float fy = (float) y * (float) s->ss;

    float box_left;
    if (h_mode == 0) box_left = fx - box_w * 0.5f;
    else if (h_mode == 1) box_left = fx;
    else box_left = fx - box_w;

    /* Two anchor conventions are in play, and picking the wrong one is
     * what makes multi-run text wobble:
     *
     *   - The ink box (above/below) is what you want when centring a
     *     lone symbol in a box - notably icon-font glyphs, whose shape
     *     sits off-centre in the advance cell. The CENTRE mode keeps
     *     using it, so icon placement is unchanged.
     *
     *   - A stable per-bake "cell" is what you want for text, because it
     *     does not depend on which glyphs the string happens to contain.
     *     Anchoring text by its own ink box gives every run its own
     *     baseline: "." is drawn at ascender height and "_" floats up.
     *     That breaks anything drawing a line in several runs (syntax
     *     highlighting), so the TOP/BOTTOM modes - defined as "text sits
     *     above/below y" - anchor the cell instead.
     *
     * The cell extremes come from the baked atlas itself (tallest ink
     * above/below the baseline across all glyphs), NOT from the face's
     * ascent_px/descent_px: those are hhea metrics recorded at
     * bake_pixels[0] and run ~1.8x the baked ink on the DOS font caches,
     * which would drop LEFTBOTTOM text a dozen-plus pixels below the
     * anchor point. */
    float cell_above = 0.0f, cell_below = 0.0f;
    if (v_mode != 0) {
        baked_cell_metrics(sel_cdata, s->faces[face_idx].glyph_count,
                           sel_pad, &cell_above, &cell_below);
        cell_above *= scale;
        cell_below *= scale;
    }

    float baseline_y;
    if (v_mode == 0) baseline_y = fy - box_h * 0.5f + above;  /* ink-centred */
    else if (v_mode == 1) baseline_y = fy - cell_below;       /* above y */
    else baseline_y = fy + cell_above;                        /* below y */

    BITMAP *tgt = DRAW_TGT(s);
    int fc2 = s->faces[face_idx].first_char;
    int gc2 = s->faces[face_idx].glyph_count;
    float cx = -baked_min_x, cy = 0.0f;
    for (const char *p = text_buf; *p; ) {
        int cp2 = utf8_next(&p);
        if (cp2 <= 0) continue;
        int gi = cp2 - fc2;
        if (gi < 0 || gi >= gc2) continue;

        stbtt_bakedchar *b = &sel_cdata[gi];
        int gw = b->x1 - b->x0;
        int gh = b->y1 - b->y0;
        float qx0 = cx + b->xoff;
        float qy0 = cy + b->yoff;
        cx += b->xadvance;
        if (gw <= 0 || gh <= 0) continue;

        float sx0 = box_left + qx0 * scale;
        float sx1 = box_left + (qx0 + (float)gw) * scale;
        float sy0 = baseline_y + qy0 * scale;
        float sy1 = baseline_y + (qy0 + (float)gh) * scale;

        int ax0 = b->x0, ay0 = b->y0;

        int dst_x = (int) (sx0 + 0.5f) + s->offset_x;
        int dst_y = (int) (sy0 + 0.5f) + s->offset_y;
        int dst_w = (int) (sx1 + 0.5f) - (int) (sx0 + 0.5f);
        int dst_h = (int) (sy1 + 0.5f) - (int) (sy0 + 0.5f);

        /* Pixel-art glyphs: when the scale is a whole number, force the
         * destination to exactly gw*k / gh*k.  Rounding sx0/sx1 independently
         * can yield gw*k +/- 1, and that single extra/missing column makes
         * nearest-neighbour duplicate or drop a source column, which is what
         * makes bitmap text look ragged. */
        if (is_bitmap) {
            int k = relic_font_bitmap_int_scale(scale);
            if (k > 0) {
                dst_w = gw * k;
                dst_h = gh * k;
                /* Keep the integer-doubled block aligned with the SSAA
                 * resolve grid, or the box filter averages it away again. */
                if (is_pixel && s->ss > 1) {
                    dst_x = relic_font_snap_grid(dst_x, s->ss);
                    dst_y = relic_font_snap_grid(dst_y, s->ss);
                }
            }
        }

        if (dst_w < 1) dst_w = 1;
        if (dst_h < 1) dst_h = 1;

        if (is_bitmap) {
            draw_glyph_bitmap(tgt, dst_x, dst_y, dst_w, dst_h,
                    sel_atlas, sel_atlas_size,
                    ax0, ay0, gw, gh,
                    col_r, col_g, col_b, col_a, is_pixel);
        } else {
            draw_glyph_alpha(tgt, dst_x, dst_y, dst_w, dst_h,
                    sel_atlas, sel_atlas_size,
                    ax0, ay0, gw, gh,
                    col_r, col_g, col_b, col_a, scale);
        }
    }
    s->frame_dirty = true;
}

static void alg_aa_polygon_fill(BITMAP *tgt, const float *pts, int n,
                                int r, int g, int b, int a);

static void allegro_gfx_text(RelicVM *vm) {
    u32 color_ptr = (u32) vm->r[8];
    int col_r = 255, col_g = 255, col_b = 255, col_a = 255;
    if (color_ptr && color_ptr + 4 <= vm->mem_sz) {
        col_r = vm->mem[color_ptr];
        col_g = vm->mem[color_ptr + 1];
        col_b = vm->mem[color_ptr + 2];
        col_a = vm->mem[color_ptr + 3];
    }
    allegro_text_draw(vm, (u32) vm->r[1], vm->r[2], vm->r[3], vm->r[4],
                      vm->r[5], (u32) vm->r[6], vm->r[7],
                      col_r, col_g, col_b, col_a);
}

/* @draw_list(buf, len) - submit a retained primitive buffer. The shared
 * tessellator (common/drawlist/drawlist.h) turns it into triangles plus
 * ordered text/clip ops; each triangle is filled through the same
 * supersampled polygon path the other 2D prims use. */
static void allegro_gfx_draw_list(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    u32 ptr = (u32) vm->r[1];
    i32 len = vm->r[2];
    DlBatch b;
    int i;
    if (!s || len <= 0) return;
    if ((uint64_t)ptr + (uint64_t)len > (uint64_t)vm->mem_sz) return;

    dl_batch_init(&b);
    /* no y-flip: the bitmap backend is already top-left y-down */
    if (!dl_tessellate(vm->mem + ptr, len, 0.0f, &b)) {
        dl_batch_free(&b);
        return;
    }
    if (b.no == 0) { dl_batch_free(&b); return; }
    s->frame_dirty = true;

    {
        BITMAP *tgt = DRAW_TGT(s);
        float ss = (float) s->ss;
        float ox = (float) s->offset_x, oy = (float) s->offset_y;
        for (i = 0; i < b.no; i++) {
            DlOp *o = &b.o[i];
            if (o->kind == DLO_TRIS) {
                int k;
                if (o->count <= 0 || !b.v) continue;
                for (k = 0; k + 2 < o->count; k += 3) {
                    const DlVert *v = &b.v[o->start + k];
                    float pts[6];
                    bool t;
                    pts[0] = v[0].x * ss + ox; pts[1] = v[0].y * ss + oy;
                    pts[2] = v[1].x * ss + ox; pts[3] = v[1].y * ss + oy;
                    pts[4] = v[2].x * ss + ox; pts[5] = v[2].y * ss + oy;
                    t = alg_begin_alpha(tgt, v->a);
                    alg_aa_polygon_fill(tgt, pts, 3,
                                        v->r, v->g, v->b, v->a);
                    alg_end_alpha(t);
                }
            } else if (o->kind == DLO_CLIP) {
                allegro_set_clip(s, o->cx, o->cy, o->cw, o->ch);
            } else if (o->kind == DLO_TEXT) {
                allegro_text_draw(vm, o->text_ptr, o->tx, o->ty,
                                  o->tsize, o->tweight, o->font_ptr,
                                  o->tjust, o->tr, o->tg, o->tb, o->ta);
            }
        }
    }
    dl_batch_free(&b);
}

/* @text_measure(text, size, weight, font_name, out)
 *   Measures text without drawing and writes a TextMetrics struct (5 i32):
 *   +0 width, +4 height, +8 ascent, +12 descent, +16 line_h (pixels). */
static void allegro_gfx_text_measure(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;

    u32 text_ptr = (u32) vm->r[1];
    i32 size = vm->r[2];
    i32 weight = vm->r[3];
    u32 fname_ptr = (u32) vm->r[4];
    u32 out_ptr = (u32) vm->r[5];

    if (out_ptr + 20u > vm->mem_sz) return;

    i32 width = 0, height = 0, ascent = 0, descent = 0, line_h = 0;

    if (s && text_ptr < vm->mem_sz) {
        /* Pull string from VM memory. */
        char text_buf[1024];
        int n = 0;
        while (text_ptr < vm->mem_sz && n < (int) sizeof(text_buf) - 1) {
            u8 c = vm->mem[text_ptr++];
            if (c == 0) break;
            text_buf[n++] = (char) c;
        }
        text_buf[n] = 0;

        if (!allegro_font_init(s) || s->nfaces <= 0) {
            /* Built-in Allegro font metrics. */
            width = text_length(font, text_buf);
            height = text_height(font);
            ascent = height;
            descent = 0;
            line_h = height;
        } else {
            char fname_buf[64] = "";
            if (fname_ptr && fname_ptr < vm->mem_sz) {
                int fn = 0;
                while (fname_ptr + fn < vm->mem_sz && fn < (int) sizeof(fname_buf) - 1) {
                    u8 c = vm->mem[fname_ptr + fn];
                    if (c == 0) break;
                    fname_buf[fn++] = (char) c;
                }
                fname_buf[fn] = 0;
            }

            int face_idx = select_face(s, fname_buf, weight);
            if (face_idx >= 0 && s->faces[face_idx].init) {
                float pixel_size = (size > 0) ? (float) size : s->faces[face_idx].bake_pixels[0];
                int sel_i = select_bake_size(s, face_idx, pixel_size);
                float sel_bp = s->faces[face_idx].bake_pixels[sel_i];
                float scale = pixel_size / sel_bp;

                const stbtt_bakedchar *sel_cdata = s->faces[face_idx].cdata[sel_i];
                float sel_pad = (float)s->faces[face_idx].sdf_padding;

                float baked_w = 0, baked_above = 0, baked_below = 0, baked_min_x = 0;
                measure_text_baked(sel_cdata, s->faces[face_idx].first_char,
                                   s->faces[face_idx].glyph_count, sel_pad,
                                   text_buf, &baked_w, &baked_above, &baked_below, &baked_min_x);

                /* ascent_px/descent_px are recorded at bake_pixels[0], so
                 * they scale from there - `scale` is relative to whichever
                 * bake got selected, which is a different basis. */
                float m_scale = pixel_size / s->faces[face_idx].bake_pixels[0];
                float asc = s->faces[face_idx].ascent_px * m_scale;
                float dsc = -s->faces[face_idx].descent_px * m_scale;

                width = (i32) (baked_w * scale + 0.5f);
                ascent = (i32) (asc + 0.5f);
                descent = (i32) (dsc + 0.5f);
                height = (i32) ((baked_above + baked_below) * scale + 0.5f);
                line_h = ascent + descent;
            }
        }
    }

    memcpy(vm->mem + out_ptr + 0, &width, 4);
    memcpy(vm->mem + out_ptr + 4, &height, 4);
    memcpy(vm->mem + out_ptr + 8, &ascent, 4);
    memcpy(vm->mem + out_ptr + 12, &descent, 4);
    memcpy(vm->mem + out_ptr + 16, &line_h, 4);
}

/* -------------------------------------------------------------------------
 * Ellipse and rounded rectangle primitives
 * ---------------------------------------------------------------------- */
#define ALG_ELLIPSE_SEGS 64
#define ALG_RRECT_CORNER_SEGS 16

/* -------------------------------------------------------------------------
 * Anti-aliased primitives for circles and rounded rectangles.
 * Uses per-pixel alpha blending with subpixel edge coverage.
 * Falls back to aliased drawing in 8bpp paletted modes.
 * ---------------------------------------------------------------------- */

static void alg_blend_pixel(BITMAP *tgt, int x, int y,
                            int r, int g, int b, int alpha) {
    if (x < 0 || x >= tgt->w || y < 0 || y >= tgt->h) return;
    if (alpha <= 0) return;
    if (alpha >= 255 || bitmap_color_depth(tgt) <= 8) {
        putpixel(tgt, x, y, makecol(r, g, b));
        return;
    }
    int dst_col = getpixel(tgt, x, y);
    int dr = getr(dst_col);
    int dg = getg(dst_col);
    int db = getb(dst_col);
    int out_r = (r * alpha + dr * (255 - alpha)) / 255;
    int out_g = (g * alpha + dg * (255 - alpha)) / 255;
    int out_b = (b * alpha + db * (255 - alpha)) / 255;
    putpixel(tgt, x, y, makecol(out_r, out_g, out_b));
}

/* Anti-aliased polygon fill using scanline edge coverage.
 * pts is an array of float pairs (x,y) interleaved, n vertices. */
static void alg_aa_polygon_fill(BITMAP *tgt, const float *pts, int n,
                                int r, int g, int b, int a) {
    if (n < 3) return;
    /* AA is handled by global SSAA; always use fast path. */
    if (1) {
        int ipts[ALG_ELLIPSE_SEGS * 2 + 16];
        for (int i = 0; i < n && i < ALG_ELLIPSE_SEGS + 8; i++) {
            ipts[i * 2] = (int)(pts[i * 2] + 0.5f);
            ipts[i * 2 + 1] = (int)(pts[i * 2 + 1] + 0.5f);
        }
        polygon(tgt, n, ipts, makecol(r, g, b));
        return;
    }

    float ymin = pts[1], ymax = pts[1];
    for (int i = 1; i < n; i++) {
        float y = pts[i * 2 + 1];
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
    }
    int y0 = (int)floorf(ymin);
    int y1 = (int)ceilf(ymax) - 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= tgt->h) y1 = tgt->h - 1;

    float crossings[ALG_ELLIPSE_SEGS + 8];

    for (int y = y0; y <= y1; y++) {
        float sy = (float)y + 0.5f;
        int nc = 0;

        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            float ey0 = pts[i * 2 + 1];
            float ey1 = pts[j * 2 + 1];
            if (ey0 == ey1) continue;
            float ex0 = pts[i * 2];
            float ex1 = pts[j * 2];
            if (ey0 < ey1) {
                if (sy < ey0 || sy >= ey1) continue;
            } else {
                if (sy < ey1 || sy >= ey0) continue;
            }
            float t = (sy - ey0) / (ey1 - ey0);
            crossings[nc++] = ex0 + t * (ex1 - ex0);
        }

        for (int i = 1; i < nc; i++) {
            float key = crossings[i];
            int j = i - 1;
            while (j >= 0 && crossings[j] > key) {
                crossings[j + 1] = crossings[j];
                j--;
            }
            crossings[j + 1] = key;
        }

        for (int i = 0; i + 1 < nc; i += 2) {
            float xl = crossings[i];
            float xr = crossings[i + 1];
            int x0 = (int)floorf(xl);
            int x1 = (int)floorf(xr);

            if (x0 == x1) {
                float cov = xr - xl;
                if (cov > 0.0f) {
                    int alpha_i = (int)(cov * (float)a + 0.5f);
                    alg_blend_pixel(tgt, x0, y, r, g, b, alpha_i);
                }
            } else {
                float left_cov = (float)(x0 + 1) - xl;
                if (left_cov > 0.0f) {
                    int alpha_i = (int)(left_cov * (float)a + 0.5f);
                    alg_blend_pixel(tgt, x0, y, r, g, b, alpha_i);
                }
                for (int x = x0 + 1; x < x1; x++) {
                    alg_blend_pixel(tgt, x, y, r, g, b, a);
                }
                float right_cov = xr - (float)x1;
                if (right_cov > 0.0f) {
                    int alpha_i = (int)(right_cov * (float)a + 0.5f);
                    alg_blend_pixel(tgt, x1, y, r, g, b, alpha_i);
                }
            }
        }
    }
}

/* Anti-aliased thick line drawn as a filled AA quad. */
static void alg_aa_thick_line(BITMAP *tgt, float x0, float y0,
                              float x1, float y1, int width,
                              int r, int g, int b, int a) {
    if (width <= 1) {
        /* AA is handled by global SSAA; always use fast path. */
        {
            line(tgt, (int)(x0 + 0.5f), (int)(y0 + 0.5f),
                 (int)(x1 + 0.5f), (int)(y1 + 0.5f), makecol(r, g, b));
            return;
        }
        /* Wu-style AA line */
        float dx = x1 - x0, dy = y1 - y0;
        float adx = fabsf(dx), ady = fabsf(dy);
        if (adx < 0.01f && ady < 0.01f) {
            alg_blend_pixel(tgt, (int)(x0 + 0.5f), (int)(y0 + 0.5f), r, g, b, a);
            return;
        }
        if (adx >= ady) {
            if (x1 < x0) { float t; t=x0;x0=x1;x1=t; t=y0;y0=y1;y1=t; }
            float grad = (adx > 0.01f) ? dy / dx : 0.0f;
            for (int x = (int)ceilf(x0); x <= (int)floorf(x1); x++) {
                float yf = y0 + grad * ((float)x - x0);
                int yi = (int)floorf(yf);
                float frac = yf - (float)yi;
                alg_blend_pixel(tgt, x, yi, r, g, b, (int)((1.0f-frac)*a+0.5f));
                alg_blend_pixel(tgt, x, yi+1, r, g, b, (int)(frac*a+0.5f));
            }
        } else {
            if (y1 < y0) { float t; t=x0;x0=x1;x1=t; t=y0;y0=y1;y1=t; }
            float grad = (ady > 0.01f) ? dx / dy : 0.0f;
            for (int y = (int)ceilf(y0); y <= (int)floorf(y1); y++) {
                float xf = x0 + grad * ((float)y - y0);
                int xi = (int)floorf(xf);
                float frac = xf - (float)xi;
                alg_blend_pixel(tgt, xi, y, r, g, b, (int)((1.0f-frac)*a+0.5f));
                alg_blend_pixel(tgt, xi+1, y, r, g, b, (int)(frac*a+0.5f));
            }
        }
        return;
    }

    /* AA is handled by global SSAA; always use fast path. */
    {
        alg_thick_line(tgt, (int)(x0 + 0.5f), (int)(y0 + 0.5f),
                       (int)(x1 + 0.5f), (int)(y1 + 0.5f), width, makecol(r, g, b));
        return;
    }

    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) return;
    float hw = (float)(width / 2);
    float px = -dy / len * hw;
    float py = dx / len * hw;
    float pts[8] = {
        x0 + px, y0 + py,
        x1 + px, y1 + py,
        x1 - px, y1 - py,
        x0 - px, y0 - py
    };
    alg_aa_polygon_fill(tgt, pts, 4, r, g, b, a);
}

/* Build an elliptical arc vertex list (screen coords, Y down) into pts.
 * pts must have space for (ALG_ELLIPSE_SEGS + 1) * 2 ints.
 * Returns the number of vertices written. */
static int alg_build_ellipse_arc(int *pts, int cx, int cy, int rx, int ry,
                                 float start_deg, float finish_deg) {
    float span = finish_deg - start_deg;
    int N = (int) (fabsf(span) / 360.0f * (float) ALG_ELLIPSE_SEGS);
    if (N < 4) N = 4;
    if (N > ALG_ELLIPSE_SEGS) N = ALG_ELLIPSE_SEGS;
    float step = span / (float) N;
    float rad = 3.14159265f / 180.0f;
    int n = 0;
    for (int i = 0; i <= N; i++) {
        float ang = (start_deg + step * (float) i) * rad;
        pts[n * 2]     = cx + (int) (cosf(ang) * (float) rx);
        pts[n * 2 + 1] = cy + (int) (sinf(ang) * (float) ry);
        n++;
    }
    return n;
}

/* Float version: produces subpixel-accurate vertices for AA fill. */
static int alg_build_ellipse_arc_f(float *pts, float cx, float cy,
                                   float rx, float ry,
                                   float start_deg, float finish_deg) {
    float span = finish_deg - start_deg;
    int N = (int)(fabsf(span) / 360.0f * (float)ALG_ELLIPSE_SEGS);
    if (N < 4) N = 4;
    if (N > ALG_ELLIPSE_SEGS) N = ALG_ELLIPSE_SEGS;
    float step = span / (float)N;
    float rad = 3.14159265f / 180.0f;
    int n = 0;
    for (int i = 0; i <= N; i++) {
        float ang = (start_deg + step * (float)i) * rad;
        pts[n * 2]     = cx + cosf(ang) * rx;
        pts[n * 2 + 1] = cy + sinf(ang) * ry;
        n++;
    }
    return n;
}

/* @ellipse(x, y, width, height, stroke_width, &stroke_col, start, finish) */
static void allegro_gfx_ellipse(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s || !screen) return;

    int cx = vm->r[1];
    int cy = vm->r[2];
    int w = vm->r[3];
    int h = vm->r[4];
    int sw = vm->r[5];
    u32 sc_ptr = (u32) vm->r[6];
    int start = vm->r[7];
    int finish = vm->r[8];

    if (sw <= 0) return;
    if (sc_ptr + 4 > vm->mem_sz) return;
    int r = vm->mem[sc_ptr];
    int g = vm->mem[sc_ptr + 1];
    int b = vm->mem[sc_ptr + 2];
    int a = vm->mem[sc_ptr + 3];

    int rx = w / 2;
    int ry = h / 2;
    if (rx < 1) rx = 1;
    if (ry < 1) ry = 1;

    cx = cx * s->ss + s->offset_x;
    cy = cy * s->ss + s->offset_y;
    rx *= s->ss;
    ry *= s->ss;
    sw *= s->ss;

    float fpts[(ALG_ELLIPSE_SEGS + 1) * 2];
    int n = alg_build_ellipse_arc_f(fpts, (float)cx, (float)cy,
                                    (float)rx, (float)ry,
                                    (float)start, (float)finish);
    if (n < 2) return;

    BITMAP *tgt = DRAW_TGT(s);
    for (int i = 0; i < n - 1; i++) {
        alg_aa_thick_line(tgt, fpts[i * 2], fpts[i * 2 + 1],
                          fpts[(i + 1) * 2], fpts[(i + 1) * 2 + 1],
                          sw, r, g, b, a);
    }
    s->frame_dirty = true;
}

/* @ellipse_filled(x, y, width, height, stroke_width, &stroke_col, start, finish, &fill_col) */
static void allegro_gfx_ellipse_filled(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s || !screen) return;

    int cx = vm->r[1];
    int cy = vm->r[2];
    int w = vm->r[3];
    int h = vm->r[4];
    int sw = vm->r[5];
    u32 sc_ptr = (u32) vm->r[6];
    int start = vm->r[7];
    int finish = vm->r[8];
    u32 fc_ptr = (u32) vm->r[9];

    int rx = w / 2;
    int ry = h / 2;
    if (rx < 1) rx = 1;
    if (ry < 1) ry = 1;

    cx = cx * s->ss + s->offset_x;
    cy = cy * s->ss + s->offset_y;
    rx *= s->ss;
    ry *= s->ss;
    sw *= s->ss;

    float fpts[(ALG_ELLIPSE_SEGS + 2) * 2];
    int n = alg_build_ellipse_arc_f(fpts, (float)cx, (float)cy,
                                    (float)rx, (float)ry,
                                    (float)start, (float)finish);
    if (n < 2) return;

    BITMAP *tgt = DRAW_TGT(s);

    /* Fill the arc as a polygon with the centre point. */
    if (fc_ptr + 4 <= vm->mem_sz) {
        int fr = vm->mem[fc_ptr];
        int fg = vm->mem[fc_ptr + 1];
        int fb = vm->mem[fc_ptr + 2];
        int fa = vm->mem[fc_ptr + 3];
        float fill_pts[(ALG_ELLIPSE_SEGS + 2) * 2];
        fill_pts[0] = (float)cx;
        fill_pts[1] = (float)cy;
        for (int i = 0; i < n; i++) {
            fill_pts[(i + 1) * 2] = fpts[i * 2];
            fill_pts[(i + 1) * 2 + 1] = fpts[i * 2 + 1];
        }
        alg_aa_polygon_fill(tgt, fill_pts, n + 1, fr, fg, fb, fa);
    }

    /* Stroke. */
    if (sw > 0 && sc_ptr + 4 <= vm->mem_sz) {
        int sr = vm->mem[sc_ptr];
        int sg = vm->mem[sc_ptr + 1];
        int sb = vm->mem[sc_ptr + 2];
        int sa = vm->mem[sc_ptr + 3];
        for (int i = 0; i < n - 1; i++) {
            alg_aa_thick_line(tgt, fpts[i * 2], fpts[i * 2 + 1],
                              fpts[(i + 1) * 2], fpts[(i + 1) * 2 + 1],
                              sw, sr, sg, sb, sa);
        }
    }

    s->frame_dirty = true;
}

/* Build a rounded-rectangle vertex list (clockwise, screen coords) into pts.
 * pts must have space for at least (ALG_ELLIPSE_SEGS + 4) * 2 ints.
 * Returns the number of vertices written. */
static int alg_build_rrect(int *pts, int x1, int y1, int x2, int y2, int radius) {
    int r = radius;
    int half_w = (x2 - x1) / 2;
    int half_h = (y2 - y1) / 2;
    if (r > half_w) r = half_w;
    if (r > half_h) r = half_h;
    if (r < 0) r = 0;

    int segs = ALG_RRECT_CORNER_SEGS;
    int n = 0;

    /* corner centres in screen space (Y-down) */
    int cx[4] = { x1 + r, x2 - r, x2 - r, x1 + r };
    int cy[4] = { y1 + r, y1 + r, y2 - r, y2 - r };
    float start_deg[4] = { 180.0f, 270.0f, 0.0f, 90.0f };

    for (int c = 0; c < 4; c++) {
        for (int i = 0; i <= segs; i++) {
            float ang = (start_deg[c] + 90.0f * (float) i / (float) segs)
                        * 3.14159265f / 180.0f;
            pts[n * 2]     = cx[c] + (int) (cosf(ang) * (float) r);
            pts[n * 2 + 1] = cy[c] + (int) (sinf(ang) * (float) r);
            n++;
        }
    }
    return n;
}

/* Float version: produces subpixel-accurate vertices for AA fill. */
static int alg_build_rrect_f(float *pts, float x1, float y1,
                             float x2, float y2, float radius) {
    float r = radius;
    float half_w = (x2 - x1) * 0.5f;
    float half_h = (y2 - y1) * 0.5f;
    if (r > half_w) r = half_w;
    if (r > half_h) r = half_h;
    if (r < 0.0f) r = 0.0f;

    int segs = ALG_RRECT_CORNER_SEGS;
    int n = 0;
    float cx[4] = { x1 + r, x2 - r, x2 - r, x1 + r };
    float cy[4] = { y1 + r, y1 + r, y2 - r, y2 - r };
    float start_deg[4] = { 180.0f, 270.0f, 0.0f, 90.0f };

    for (int c = 0; c < 4; c++) {
        for (int i = 0; i <= segs; i++) {
            float ang = (start_deg[c] + 90.0f * (float)i / (float)segs)
                        * 3.14159265f / 180.0f;
            pts[n * 2]     = cx[c] + cosf(ang) * r;
            pts[n * 2 + 1] = cy[c] + sinf(ang) * r;
            n++;
        }
    }
    return n;
}

/* @rounded_rectangle(x1, y1, x2, y2, stroke_width, &stroke_col, radius) */
static void allegro_gfx_rounded_rect(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s || !screen) return;

    int x1 = vm->r[1];
    int y1 = vm->r[2];
    int x2 = vm->r[3];
    int y2 = vm->r[4];
    int sw = vm->r[5];
    u32 sc_ptr = (u32) vm->r[6];
    int radius = vm->r[7];

    if (sw <= 0) return;
    if (sc_ptr + 4 > vm->mem_sz) return;
    int r = vm->mem[sc_ptr];
    int g = vm->mem[sc_ptr + 1];
    int b = vm->mem[sc_ptr + 2];
    int a = vm->mem[sc_ptr + 3];

    x1 = x1 * s->ss + s->offset_x;
    y1 = y1 * s->ss + s->offset_y;
    x2 = x2 * s->ss + s->offset_x;
    y2 = y2 * s->ss + s->offset_y;
    radius *= s->ss;
    sw *= s->ss;

    float fpts[(ALG_ELLIPSE_SEGS + 8) * 2];
    int n = alg_build_rrect_f(fpts, (float)x1, (float)y1, (float)x2, (float)y2, (float)radius);
    if (n < 2) return;

    BITMAP *tgt = DRAW_TGT(s);
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        alg_aa_thick_line(tgt, fpts[i * 2], fpts[i * 2 + 1],
                          fpts[j * 2], fpts[j * 2 + 1],
                          sw, r, g, b, a);
    }
    s->frame_dirty = true;
}

/* @rounded_rectangle_filled(x1, y1, x2, y2, stroke_width, &stroke_col, radius, &fill_col, elevation) */
static void allegro_gfx_rounded_rect_filled(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s || !screen) return;

    int x1 = vm->r[1];
    int y1 = vm->r[2];
    int x2 = vm->r[3];
    int y2 = vm->r[4];
    int sw = vm->r[5];
    u32 sc_ptr = (u32) vm->r[6];
    int radius = vm->r[7];
    u32 fc_ptr = (u32) vm->r[8];
    int elevation = vm->r[9];

    x1 = x1 * s->ss + s->offset_x;
    y1 = y1 * s->ss + s->offset_y;
    x2 = x2 * s->ss + s->offset_x;
    y2 = y2 * s->ss + s->offset_y;
    radius *= s->ss;
    sw *= s->ss;
    elevation *= s->ss;

    BITMAP *tgt = DRAW_TGT(s);

    float fpts[(ALG_ELLIPSE_SEGS + 8) * 2];

    /* Optional drop-shadow/elevation. */
    if (elevation > 0) {
        int shadow_n = alg_build_rrect_f(fpts,
                                         (float)(x1 + elevation), (float)(y1 + elevation),
                                         (float)(x2 + elevation), (float)(y2 + elevation),
                                         (float)radius);
        if (shadow_n >= 3) {
            alg_aa_polygon_fill(tgt, fpts, shadow_n, 0, 0, 0, 64);
        }
    }

    int n = alg_build_rrect_f(fpts, (float)x1, (float)y1, (float)x2, (float)y2, (float)radius);
    if (n < 3) return;

    /* Fill. */
    if (fc_ptr + 4 <= vm->mem_sz) {
        int fr = vm->mem[fc_ptr];
        int fg = vm->mem[fc_ptr + 1];
        int fb = vm->mem[fc_ptr + 2];
        int fa = vm->mem[fc_ptr + 3];
        alg_aa_polygon_fill(tgt, fpts, n, fr, fg, fb, fa);
    }

    /* Stroke. */
    if (sw > 0 && sc_ptr + 4 <= vm->mem_sz) {
        int sr = vm->mem[sc_ptr];
        int sg = vm->mem[sc_ptr + 1];
        int sb = vm->mem[sc_ptr + 2];
        int sa = vm->mem[sc_ptr + 3];
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            alg_aa_thick_line(tgt, fpts[i * 2], fpts[i * 2 + 1],
                              fpts[j * 2], fpts[j * 2 + 1],
                              sw, sr, sg, sb, sa);
        }
    }

    s->frame_dirty = true;
}

static void allegro_gfx_mesh(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s || !s->backbuf || !s->sw_zbuf || !s->have_camera) return;

    u32 verts_ptr = (u32) vm->r[1];
    i32 vcount = vm->r[2];
    u32 idx_ptr = (u32) vm->r[3];
    i32 icount = vm->r[4];
    i32 tex_id = vm->r[5];
    u32 rgba_ptr = (u32) vm->r[6];

    /* Bit 31 of icount signals u32 index mode (RLM v3). */
    int idx_u32 = (icount < 0);
    if (idx_u32) icount = icount & 0x7FFFFFFF;

    if (vcount <= 0) return;
    const size_t VSIZE = 8 * sizeof(float);
    size_t idx_stride = idx_u32 ? 4 : 2;
    if ((uint64_t) verts_ptr + (uint64_t) vcount * VSIZE > (uint64_t) vm->mem_sz) return;
    if (idx_ptr && (uint64_t) idx_ptr + (uint64_t) icount * idx_stride > (uint64_t) vm->mem_sz) return;

    float cr = 1, cg = 1, cb = 1;
    if (rgba_ptr && rgba_ptr + 4 <= vm->mem_sz) {
        cr = vm->mem[rgba_ptr] / 255.0f;
        cg = vm->mem[rgba_ptr + 1] / 255.0f;
        cb = vm->mem[rgba_ptr + 2] / 255.0f;
    }

    const SwTexture *tex = NULL;
    int tex_src_w = 1, tex_src_h = 1;
    if (tex_id > 0 && tex_id < s->tex_cap && s->textures[tex_id].pixels) {
        tex = &s->textures[tex_id];
        tex_src_w = tex->src_w;
        tex_src_h = tex->src_h;
    }

    BITMAP *dst = s->backbuf;
    const float *mvp = s->mvp;

    /* Per-vertex SW clip-space data. Use stack for small meshes. */
    typedef struct {
        float cx, cy, cz, cw, r, g, b, tu, tv;
    } VXf;
#define VXF_STACK 256
    VXf vx_stack[VXF_STACK];
    VXf *vx = (vcount <= VXF_STACK) ? vx_stack : (VXf *) malloc(sizeof(VXf) * (size_t) vcount);
    if (!vx) return;

    /* Projection centre and scale: always the full backbuffer.
     * @viewport is a pure clip (like the GL backend's glScissor) — the
     * flip blit shows only that rect, so the scene must project for the
     * full surface rather than re-fitting into the viewport.
     * (Centreing via offset_x/y would push the centre off the edge when
     * the backbuf is smaller than the physical screen.) */
    float vp_cx = (float) (dst->w * 0.5f);
    float vp_cy = (float) (dst->h * 0.5f);
    float vp_hw = (float) (dst->w * 0.5f);
    float vp_hh = (float) (dst->h * 0.5f);

    /* Whole-mesh cull: skip if every vertex is strictly behind the camera.
     * View row 2 (column-major): V[2]=-fx, V[6]=-fy, V[10]=-fz, V[14]=+f·eye.
     * view-space Z = -f·(p-eye): negative = in front, positive = behind.
     * Only skip when ALL vertices have vz > 0 (nothing in front at all). */
    {
        const float *V = s->view;
        float rx = V[2], ry = V[6], rz = V[10], rd = V[14];
        int all_behind = 1;
        int ci;
        for (ci = 0; ci < vcount && all_behind; ci++) {
            const float *v = (const float *) (vm->mem + verts_ptr + (size_t) ci * VSIZE);
            float vz = rx * v[0] + ry * v[1] + rz * v[2] + rd;
            if (vz <= 0.0f) {all_behind = 0;}
        }
        if (all_behind) {
            if (vx != vx_stack) free(vx);
            return;
        }
    }

    int i;
    unsigned int prof_vt0 = g_relic_gfx_prof ? g_millis_counter : 0;
    for (i = 0; i < vcount; i++) {
        const float *v = (const float *) (vm->mem + verts_ptr + (size_t) i * VSIZE);
        float pos[3] = {v[0], v[1], v[2]}, nrm[3] = {v[3], v[4], v[5]};
        float clip[4];
        alg_mat4_xform(mvp, pos, clip);
        vx[i].cx = clip[0];
        vx[i].cy = clip[1];
        vx[i].cz = clip[2];
        vx[i].cw = clip[3];

        float lr = cr, lg = cg, lb = cb;
        if (s->light_on) {
            float nl = nrm[0] * (-s->light_dir[0]) + nrm[1] * (-s->light_dir[1]) + nrm[2] * (-s->light_dir[2]);
            if (nl < 0) nl = 0;
            lr = cr * (s->light_amb[0] + s->light_rgb[0] * nl);
            lg = cg * (s->light_amb[1] + s->light_rgb[1] * nl);
            lb = cb * (s->light_amb[2] + s->light_rgb[2] * nl);
            if (lr > 1)lr = 1;
            if (lg > 1)lg = 1;
            if (lb > 1)lb = 1;
        }
        vx[i].r = lr * 255.0f;
        vx[i].g = lg * 255.0f;
        vx[i].b = lb * 255.0f;

        /* UVs stored as u/w * src_w (perspective-divided at emit time). */
        vx[i].tu = v[6] * (float) tex_src_w;
        vx[i].tv = v[7] * (float) tex_src_h;
    }

    if (g_relic_gfx_prof) {
        g_prof.verts_xformed += vcount;
        g_prof.vt_ms += g_millis_counter - prof_vt0;
    }

    const float ZNEAR = s->znear > 0.0f ? s->znear : 0.05f;
    int max_tris = (idx_ptr && icount > 0) ? (icount / 3) : (vcount / 3);
    int t;
    unsigned int prof_tri0 = g_relic_gfx_prof ? g_millis_counter : 0;
    for (t = 0; t < max_tris; t++) {
        if (g_relic_gfx_prof) g_prof.tris_considered++;
        int i0, i1, i2;
        if (idx_ptr && icount > 0) {
            if (idx_u32) {
                u32 o = idx_ptr + (u32) t * 12;
                i0 = vm->mem[o]   | (vm->mem[o+1]<<8) | (vm->mem[o+2]<<16) | (vm->mem[o+3]<<24);
                i1 = vm->mem[o+4] | (vm->mem[o+5]<<8) | (vm->mem[o+6]<<16) | (vm->mem[o+7]<<24);
                i2 = vm->mem[o+8] | (vm->mem[o+9]<<8) | (vm->mem[o+10]<<16) | (vm->mem[o+11]<<24);
            } else {
                u32 o = idx_ptr + (u32) t * 6;
                i0 = vm->mem[o] | (vm->mem[o + 1] << 8);
                i1 = vm->mem[o + 2] | (vm->mem[o + 3] << 8);
                i2 = vm->mem[o + 4] | (vm->mem[o + 5] << 8);
            }
        } else {
            i0 = t * 3;
            i1 = t * 3 + 1;
            i2 = t * 3 + 2;
        }
        if (i0 < 0 || i0 >= vcount || i1 < 0 || i1 >= vcount || i2 < 0 || i2 >= vcount) continue;

        /* Clip-space trivial reject: skip if all 3 verts are outside the same
         * frustum half-space (behind, left, right, above, below). */
        {
            const VXf *p0 = &vx[i0], *p1 = &vx[i1], *p2 = &vx[i2];
            /* All behind near plane */
            if (p0->cw < ZNEAR && p1->cw < ZNEAR && p2->cw < ZNEAR) continue;
            /* Remaining tests only valid when all cw > 0 */
            if (p0->cw > 0.0f && p1->cw > 0.0f && p2->cw > 0.0f) {
                if (p0->cx < -p0->cw && p1->cx < -p1->cw && p2->cx < -p2->cw) continue;
                if (p0->cx > p0->cw && p1->cx > p1->cw && p2->cx > p2->cw) continue;
                if (p0->cy > p0->cw && p1->cy > p1->cw && p2->cy > p2->cw) continue;
                if (p0->cy < -p0->cw && p1->cy < -p1->cw && p2->cy < -p2->cw) continue;
            }
        }

        /* Clip against near plane (w >= ZNEAR), fan-triangulate output polygon. */
        SwClipVert cin[3], cout[SW_MAXCLIP];
        int j;
        for (j = 0; j < 3; j++) {
            int idx = (j == 0) ? i0 : (j == 1) ? i1 : i2;
            cin[j].x = vx[idx].cx;
            cin[j].y = vx[idx].cy;
            cin[j].z = vx[idx].cz;
            cin[j].w = vx[idx].cw;
            cin[j].r = vx[idx].r;
            cin[j].g = vx[idx].g;
            cin[j].b = vx[idx].b;
            cin[j].tu = vx[idx].tu;
            cin[j].tv = vx[idx].tv;
        }
        int nc = sw_clip_near(cin, 3, cout, ZNEAR);
        if (nc < 3) continue;

        /* Project every clipped vertex ONCE.  The back-face cull below needs
         * screen x/y for the first three, and the fan loop then needs them for
         * all of them -- computing both separately meant the shared vertices
         * were divided twice (3 + 3*(nc-2) divides instead of nc).  For the
         * common unclipped triangle that is 6 soft-float divides where 3 do,
         * on a path where per-triangle setup already dominates the frame. */
        float pr_sx[SW_MAXCLIP], pr_sy[SW_MAXCLIP], pr_iw[SW_MAXCLIP];
        {
            int pi;
            for (pi = 0; pi < nc; pi++) {
                float invw = 1.0f / cout[pi].w;
                float sx = vp_cx + cout[pi].x * invw * vp_hw;
                float sy = vp_cy - cout[pi].y * invw * vp_hh;
#ifdef RELIC_FIXED_POINT_3D
                /* Clamp screen coords before they reach sw_draw_tri, which
                 * (under RELIC_FIXED_POINT_3D) converts sx/sy to 16.16
                 * fixed point via FLOAT_TO_FP. That format only safely
                 * represents magnitudes up to ~32767; a near-plane-grazing
                 * vertex (w close to ZNEAR) can project to screen
                 * coordinates in the tens of thousands, silently
                 * overflowing the 32-bit accumulator and corrupting the
                 * whole scanline walk (garbage span bounds / UV / colour -
                 * seen as a stretched, wrong-coloured smear). The actual
                 * viewport is only a few hundred pixels wide, so clamping
                 * to +/-30000 is always far off-screen and changes nothing
                 * for any triangle that would otherwise render correctly.
                 * Gated to the fixed-point build only: the float path has
                 * no such overflow (IEEE float easily covers this range),
                 * so systems with an FPU get unclamped full precision. */
                if (sx > 30000.0f) sx = 30000.0f; else if (sx < -30000.0f) sx = -30000.0f;
                if (sy > 30000.0f) sy = 30000.0f; else if (sy < -30000.0f) sy = -30000.0f;
#endif
                pr_iw[pi] = invw;
                pr_sx[pi] = sx;
                pr_sy[pi] = sy;
            }
        }

        /* Back-face cull on first triangle of the clipped polygon.
         * When cam_mirror is active the horizontal projection flip reverses
         * winding, so the cull sign is inverted. */
        {
            float crs = (pr_sx[1] - pr_sx[0]) * (pr_sy[2] - pr_sy[0])
                      - (pr_sy[1] - pr_sy[0]) * (pr_sx[2] - pr_sx[0]);
            if (s->cam_mirror) {
                if (crs <= -0.5f) continue;
            } else {
                if (crs >= 0.5f) continue;
            }
        }

        /* Fan-triangulate and rasterise. */
        int k;
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
                /* Store u/w*tex_w and v/w*tex_h for perspective recovery in sw_draw_tri. */
                sv[fi].tu = cv->tu * invw;
                sv[fi].tv = cv->tv * invw;
            }
            sw_draw_tri(s, dst, sv[0], sv[1], sv[2], tex);
            if (g_relic_gfx_prof) g_prof.tris_rasterised++;
        }
    }
    if (g_relic_gfx_prof) {
        g_prof.mesh_calls++;
        g_prof.tri_ms += g_millis_counter - prof_tri0;
    }
    if (vx != vx_stack) free(vx);
#undef VXF_STACK
    s->frame_dirty = true;
}

/* -------------------------------------------------------------------------
 * Input functions
 *
 * Cross-platform ABI for Relic input (see win32 / opengl-win32 backends):
 *   - @key_pressed()  returns a Win32 VK code (e.g. 'W'=87, ESC=27, SPACE=32,
 *                     arrows = VK_LEFT/UP/RIGHT/DOWN = 37/38/39/40), 0 if no key.
 *   - @key_held(vk)   takes the same VK code and returns 1 if currently down.
 *   - @mouse(ptr)     writes {x, y, buttons, wheel}; buttons = bit0:L bit1:R bit2:M;
 *                     x/y are in *virtual surface* coords (i.e. with the same
 *                     centering offset as the 2D drawing primitives applied).
 *
 * Allegro on DOS reports `key[]` and `readkey()` using its own scancode
 * scheme (KEY_A=1..KEY_Z=26, KEY_0=27..KEY_9=36, KEY_ESC=59, KEY_SPACE=75,
 * KEY_LEFT=82..KEY_DOWN=85, KEY_F1=47..KEY_F12=58, etc.), so we translate
 * to VK codes here. Without this, samples (TASTER.tgx, maze.tgx,
 * fly.tgx, ...) that check @key_held(87) for 'W' would never trigger on
 * DOS even though the rest of the platform works fine.
 * ---------------------------------------------------------------------- */

/* Allegro KEY_* scancode -> Win32 VK code. Indexed [0..127]. 0 = no mapping. */
static const u8 alg_scancode_to_vk[128] = {
        /* 0 */ 0,
        /* 1..26  KEY_A..KEY_Z */
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
        'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
        /* 27..36 KEY_0..KEY_9 */
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
        /* 37..46 KEY_0_PAD..KEY_9_PAD -> VK_NUMPAD0..VK_NUMPAD9 (0x60..0x69) */
        0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
        /* 47..58 KEY_F1..KEY_F12 -> VK_F1..VK_F12 (0x70..0x7B) */
        0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B,
        /* 59 KEY_ESC */         27,
        /* 60 KEY_TILDE */       0xC0,
        /* 61 KEY_MINUS */       0xBD,
        /* 62 KEY_EQUALS */      0xBB,
        /* 63 KEY_BACKSPACE */   8,
        /* 64 KEY_TAB */         9,
        /* 65 KEY_OPENBRACE */   0xDB,
        /* 66 KEY_CLOSEBRACE */  0xDD,
        /* 67 KEY_ENTER */       13,
        /* 68 KEY_COLON */       0xBA,   /* ; */
        /* 69 KEY_QUOTE */       0xDE,
        /* 70 KEY_BACKSLASH */   0xDC,
        /* 71 KEY_BACKSLASH2 */  0xDC,
        /* 72 KEY_COMMA */       0xBC,
        /* 73 KEY_STOP */        0xBE,   /* . */
        /* 74 KEY_SLASH */       0xBF,
        /* 75 KEY_SPACE */       32,
        /* 76 KEY_INSERT */      0x2D,
        /* 77 KEY_DEL */         0x2E,
        /* 78 KEY_HOME */        0x24,
        /* 79 KEY_END */         0x23,
        /* 80 KEY_PGUP */        0x21,
        /* 81 KEY_PGDN */        0x22,
        /* 82 KEY_LEFT */        37,
        /* 83 KEY_RIGHT */       39,
        /* 84 KEY_UP */          38,
        /* 85 KEY_DOWN */        40,
        /* 86 KEY_SLASH_PAD */   0x6F,
        /* 87 KEY_ASTERISK */    0x6A,
        /* 88 KEY_MINUS_PAD */   0x6D,
        /* 89 KEY_PLUS_PAD */    0x6B,
        /* 90 KEY_DEL_PAD */     0x2E,
        /* 91 KEY_ENTER_PAD */   13,
        /* 92 KEY_PRTSCR */      0x2C,
        /* 93 KEY_PAUSE */       0x13,
        /* rest 0 */
};

static u8 alg_vk_from_scancode(int sc) {
    if (sc < 0 || sc >= 128) return 0;
    return alg_scancode_to_vk[sc];
}

/* Drain Allegro's keypress queue into s->last_key (translated to VK).
 * Called from gfx_flip and inp_key so neither has to be called every
 * frame for keystrokes to be detected. readkey() returns
 * (ascii | scancode<<8); we want the scancode. */
static void allegro_pump_keys(AllegroState *s) {
    if (!s) return;
    while (keypressed()) {
        int k = readkey();
        int sc = (k >> 8) & 0x7F;
        u8 vk = alg_vk_from_scancode(sc);
        if (vk) s->last_key = vk;
    }
}

static void allegro_inp_key(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) {
        vm->r[1] = 0;
        return;
    }

    allegro_pump_keys(s);

    vm->r[1] = (i32) s->last_key;
    s->last_key = 0;
}

static void allegro_inp_keyheld(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) {
        vm->r[1] = 0;
        return;
    }

    /* Also drain the buffered keypress queue so it doesn't overflow when a
     * program only ever uses @key_held — Allegro stops reporting new keys
     * if its 16-deep buffer fills up. */
    allegro_pump_keys(s);

    u8 vk = (u8) vm->r[1];
    int held = 0;
    /* Walk the small scancode table; for any scancode mapped to this VK,
     * OR in Allegro's `key[]` state. (Most VKs have one scancode; numpad
     * Enter / Backslash2 / Del-pad share VKs with main-keyboard variants
     * — both should count as held.) */
    for (int sc = 1; sc < 128; sc++) {
        if (alg_scancode_to_vk[sc] == vk && key[sc]) {
            held = 1;
            break;
        }
    }
    vm->r[1] = held;
}

static void allegro_inp_mouse(RelicVM *vm) {
    u32 ptr = (u32) vm->r[1];
    if (ptr + 16u > vm->mem_sz) return;

    AllegroState *s = (AllegroState *) vm->backend_data;
    i32 x = 0, y = 0, b = 0, w = 0;

    if (s) {
        poll_mouse();
        if (s->stretch_enabled && s->width > 0 && s->height > 0) {
            /* Non-uniform stretch: map full physical screen to logical. */
            x = mouse_x * s->logical_w / s->width;
            y = mouse_y * s->logical_h / s->height;
        } else {
            /* Allegro reports physical-screen coords; convert to logical
             * backbuffer coords by subtracting the screen-centre offset. */
            x = mouse_x - s->vp_x;
            y = mouse_y - s->vp_y;
        }
        /* Allegro's mouse_b is already a bitmask: bit0=L, bit1=R, bit2=M
         * — matches the cross-platform ABI exactly. */
        b = mouse_b;
        w = s->mouse_wheel + mouse_z;
        s->mouse_wheel = 0;
        /* Reset Allegro's accumulating wheel counter so the next call
         * returns only the delta since this one. */
        position_mouse_z(0);
    }

    memcpy(vm->mem + ptr + 0, &x, 4);
    memcpy(vm->mem + ptr + 4, &y, 4);
    memcpy(vm->mem + ptr + 8, &b, 4);
    memcpy(vm->mem + ptr + 12, &w, 4);
}

/* -------------------------------------------------------------------------
 * System functions
 * ---------------------------------------------------------------------- */
static void allegro_sys_rand(RelicVM *vm) {
    vm->r[1] = rand() & 0xFFFF;
}

static void allegro_sys_millis(RelicVM *vm) {
    vm->r[1] = (i32) g_millis_counter;
}

/* Opens TIGRYX.LOG lazily on first @console write so programs that never
 * use @console don't create an empty file. The log is the always-visible
 * debug channel on DOS: stdout writes go to the text-mode video buffer,
 * which is invisible while a VESA graphics mode is active and is lost on
 * crash; the log file survives both. */
static void allegro_console_open_log(void) {
    if (g_console_log) return;
    /* Open in append mode so earlier writes to TIGRYX.LOG (e.g. the
     * [jit] status line emitted by relic-ri before VM init) survive.
     * Opening with "w" here would truncate those diagnostics. */
    g_console_log = fopen("TIGRYX.LOG", "a");
    if (g_console_log && g_vm && g_vm->verbose) {
        fputs("--- Relic DOS console log ---\n", g_console_log);
        fflush(g_console_log);
    }
}

static void allegro_sys_console(RelicVM *vm) {
    u32 ptr = (u32) vm->r[1];
    allegro_console_open_log();

    /* Always write to stdout. In DOS this writes to the text-mode video
     * buffer; while Allegro holds a graphics mode the text isn't visible,
     * but it is restored on the screen when Allegro exits and returns the
     * card to text mode — useful for post-mortem debug. The log file
     * (TIGRYX.LOG) is the primary, always-visible debug channel. */
    while (ptr < vm->mem_sz) {
        u8 c = vm->mem[ptr++];
        if (c == 0) break;
        fputc((int) c, stdout);
        if (g_console_log) fputc((int) c, g_console_log);
    }
    fflush(stdout);
    if (g_console_log) fflush(g_console_log);
}

#define BACKEND_MAX_FILES 16
typedef struct {
    bool is_asset;
    FILE *fp;
    const u8 *asset_data;
    u32 asset_size;
    u32 asset_pos;
} BackendFileHandle;
static BackendFileHandle allegro_handles[BACKEND_MAX_FILES];

static i32 allegro_handle_alloc_asset(const u8 *d, u32 sz) {
    for (int i = 1; i < BACKEND_MAX_FILES; i++) {
        if (!allegro_handles[i].fp && !allegro_handles[i].asset_data) {
            allegro_handles[i].is_asset = true;
            allegro_handles[i].fp = NULL;
            allegro_handles[i].asset_data = d;
            allegro_handles[i].asset_size = sz;
            allegro_handles[i].asset_pos = 0;
            return (i32) i;
        }
    }
    return -1;
}

static i32 allegro_handle_alloc_file(FILE *f) {
    for (int i = 1; i < BACKEND_MAX_FILES; i++) {
        if (!allegro_handles[i].fp && !allegro_handles[i].asset_data) {
            allegro_handles[i].is_asset = false;
            allegro_handles[i].fp = f;
            allegro_handles[i].asset_data = NULL;
            return (i32) i;
        }
    }
    return -1;
}

static BackendFileHandle *allegro_handle_get(i32 idx) {
    if (idx < 1 || idx >= BACKEND_MAX_FILES) return NULL;
    if (!allegro_handles[idx].fp && !allegro_handles[idx].asset_data) return NULL;
    return &allegro_handles[idx];
}

static void allegro_handle_free(i32 idx) {
    if (idx < 1 || idx >= BACKEND_MAX_FILES) return;
    if (allegro_handles[idx].fp) fclose(allegro_handles[idx].fp);
    memset(&allegro_handles[idx], 0, sizeof(allegro_handles[idx]));
}

static void allegro_sys_fopen(RelicVM *vm) {
    u32 ptr = (u32) vm->r[1];
    i32 len = vm->r[2];
    i32 mode = vm->r[3];     /* 0 = read, 1 = write */
    char path[512] = {0};
    /* The compiler only knows the path length for string literals; for
     * dynamic paths it passes 0. In that case derive the length from the
     * NUL terminator so runtime-built filenames (e.g. from @args) work. */
    if (len <= 0) {
        len = 0;
        while (len < 511 && ptr + (u32) len < vm->mem_sz && vm->mem[ptr + len] != 0) len++;
    }
    int n = len < 511 ? (int) len : 511;
    if (ptr + (u32) n <= vm->mem_sz) memcpy(path, vm->mem + ptr, n);
    if (mode == 1) {
        /* Write mode: bypass asset lookup; assets are read-only blobs. */
        FILE *f = fopen(path, "wb");
        vm->r[1] = f ? allegro_handle_alloc_file(f) : -1;
        if (f && vm->r[1] == -1) fclose(f);
    } else if (mode == 2) {
        /* FILE_MODE_READ_DISK: filesystem only, never assets. */
        FILE *f = fopen(path, "rb");
        vm->r[1] = f ? allegro_handle_alloc_file(f) : -1;
        if (f && vm->r[1] == -1) fclose(f);
    } else {
        u32 asset_sz = 0;
        const u8 *asset = relic_assets_find(vm->assets, path, &asset_sz);
        if (asset) {
            vm->r[1] = allegro_handle_alloc_asset(asset, asset_sz);
        } else {
            FILE *f = fopen(path, "rb");
            vm->r[1] = f ? allegro_handle_alloc_file(f) : -1;
            if (f && vm->r[1] == -1) fclose(f);
        }
    }
}

static void allegro_sys_fread(RelicVM *vm) {
    BackendFileHandle *h = allegro_handle_get(vm->r[1]);
    u32 buf = (u32) vm->r[2];
    u32 n = (u32) vm->r[3];
    if (!h || buf + n > vm->mem_sz) {
        vm->r[1] = 0;
        return;
    }

    if (h->is_asset) {
        u32 avail = h->asset_size - h->asset_pos;
        u32 rd = n < avail ? n : avail;
        if (rd) memcpy(vm->mem + buf, h->asset_data + h->asset_pos, rd);
        h->asset_pos += rd;
        vm->r[1] = (i32) rd;
    } else {
        vm->r[1] = (i32) fread(vm->mem + buf, 1, n, h->fp);
    }
}

static void allegro_sys_fclose(RelicVM *vm) {
    allegro_handle_free(vm->r[1]);
}

static void allegro_sys_fseek(RelicVM *vm) {
    BackendFileHandle *h = allegro_handle_get(vm->r[1]);
    i32 offset = vm->r[2], whence = vm->r[3];
    if(!h) { vm->r[1] = -1; return; }
    if(h->is_asset) {
        u32 np;
        switch(whence) {
            case 0: np = (u32)offset; break;
            case 1: np = h->asset_pos + (u32)offset; break;
            case 2: np = h->asset_size + (u32)offset; break;
            default: vm->r[1] = -1; return;
        }
        if(np > h->asset_size) np = h->asset_size;
        h->asset_pos = np;
        vm->r[1] = (i32)np;
    } else {
        int w = (whence==0)?SEEK_SET:(whence==1)?SEEK_CUR:(whence==2)?SEEK_END:SEEK_SET;
        if(fseek(h->fp, (long)offset, w) != 0) { vm->r[1] = -1; return; }
        vm->r[1] = (i32)ftell(h->fp);
    }
}

static void allegro_sys_flen(RelicVM *vm) {
    BackendFileHandle *h = allegro_handle_get(vm->r[1]);
    if(!h) { vm->r[1] = -1; return; }
    if(h->is_asset) {
        vm->r[1] = (i32)h->asset_size;
    } else {
        long cur = ftell(h->fp);
        if(cur < 0 || fseek(h->fp, 0, SEEK_END) != 0) { vm->r[1] = -1; return; }
        long end = ftell(h->fp);
        fseek(h->fp, cur, SEEK_SET);
        vm->r[1] = (i32)end;
    }
}

static void allegro_sys_fwrite(RelicVM *vm) {
    BackendFileHandle *h = allegro_handle_get(vm->r[1]);
    u32 buf = (u32) vm->r[2];
    u32 n = (u32) vm->r[3];
    if (!h || buf + n > vm->mem_sz) {
        vm->r[1] = 0;
        return;
    }
    if (h->is_asset) {
        /* Assets are read-only. */
        vm->r[1] = 0;
    } else {
        vm->r[1] = (i32) fwrite(vm->mem + buf, 1, n, h->fp);
    }
}

/* PlatformInfo layout (VM-side, pointers are u32 vm->mem offsets):
 *   offset  size  field
 *      0     4    *u8  os_platform     ("DOS" / ...)
 *      4     4    *u8  os_version      ("7", ...)
 *      8     4    *u8  cpu_arch        ("x86" / ...)
 *     12     4    *u8  cpu_class       ("386" / "486" / "Pentium" / ...)
 *     16     4    *u8  display_mode    ("Graphics" / "Text" / "None")
 *     20     2    u16  screen_width
 *     22     2    u16  screen_height
 *     24     4    u32  screen_colours
 *     28     4    u32  sound_flags    (bitmap: tones|samples|MUS|MIDI)
 *     32     4    *u8  file_support    ("Read-write" / "Read-only" / "None")
 *     36     4    *u8  fpu_support     ("Hardware" / "Emulated" / "None")
 *     40     4    *u8  architecture    ("Allegro-DOS" / ...)
 *     44     4    *u8  gpu_support     ("Software" / "None")
 *     48     4    *u8  opengl_version  ("None")
 *     52     4    i32  window_x
 *     56     4    i32  window_y
 *     60     4    i32  window_width
 *     64     4    i32  window_height
 *     68     4    *u8  jit_status
 *     72     4    u32  cpu_speed      (MHz, 0=unknown)
 *     76     4    u32  cpu_cores      (logical core count)
 *     80     4    *u8  processor      (CPU brand string)
 *   ---- total 84 bytes ---- */
#define PLATFORM_INFO_SIZE 116u

static u32 allegro_platstr(RelicVM *vm, u32 *cursor, const char *s) {
    if (!vm || !cursor || !s) return 0;
    u32 base = vm->platform_strings_base;
    u32 sz = vm->platform_strings_size;
    if (sz == 0) return 0;
    u32 n = (u32) strlen(s) + 1u;
    if (*cursor + n > sz) return 0;
    u32 off = base + *cursor;
    memcpy(vm->mem + off, s, n);
    *cursor += n;
    return off;
}

static void allegro_putptr(u8 *mem, int off, u32 ptr) {
    mem[off + 0] = ptr & 0xFF;
    mem[off + 1] = (ptr >> 8) & 0xFF;
    mem[off + 2] = (ptr >> 16) & 0xFF;
    mem[off + 3] = (ptr >> 24) & 0xFF;
}

static void allegro_sys_platform(RelicVM *vm) {
    u32 ptr = (u32) vm->r[1];
    if (ptr + PLATFORM_INFO_SIZE > vm->mem_sz) return;
    u8 *mem = vm->mem + ptr;
    memset(mem, 0, PLATFORM_INFO_SIZE);

    u32 cursor = 0;

    /* os_platform */
    allegro_putptr(mem, 0, allegro_platstr(vm, &cursor, "DOS"));

    /* os_version: use Allegro's os_version integer as a string */
    {
        char ver_buf[8];
        snprintf(ver_buf, sizeof(ver_buf), "%d", (int) os_version);
        allegro_putptr(mem, 4, allegro_platstr(vm, &cursor, ver_buf));
    }

    /* cpu_arch / cpu_class: DJGPP targets x86; use compile-time march defines
     * since cpu_type is not reliably declared by this Allegro build's headers. */
    allegro_putptr(mem, 8, allegro_platstr(vm, &cursor, "x86"));
    {
#if defined(__tune_pentium__) || defined(__pentium__) || defined(__i686__) || defined(__i586__)
        const char *cls = "Pentium";
#elif defined(__i486__)
        const char *cls = "486";
#else
        const char *cls = "386";
#endif
        allegro_putptr(mem, 12, allegro_platstr(vm, &cursor, cls));
    }

    /* display_mode */
    allegro_putptr(mem, 16, allegro_platstr(vm, &cursor, "Graphics"));

    /* screen_width / screen_height / screen_colours */
    {
        /* Before the first @window call s->width/height are just the init
         * defaults (320x200), not probed display info — reporting them as
         * screen size makes guests that clamp their window request to the
         * "display" open at 320x200 (KSINK did exactly this). Report 0 until
         * a mode is actually set; the API contract treats 0 as "unknown". */
        AllegroState *s = (AllegroState *) vm->backend_data;
        bool have_mode = s && s->gfx_mode_set;
        u16 sw = have_mode ? (u16)(s->stretch_enabled ? s->logical_w : s->width) : 0;
        u16 sh = have_mode ? (u16)(s->stretch_enabled ? s->logical_h : s->height) : 0;
        mem[20] = sw & 0xFF;
        mem[21] = (sw >> 8) & 0xFF;
        mem[22] = sh & 0xFF;
        mem[23] = (sh >> 8) & 0xFF;
        u32 colours = 65536; /* default 16bpp */
        if (s) {
            if (s->color_depth == 8) colours = 256;
            else if (s->color_depth == 16) colours = 65536;
            else colours = 16777216;
        }
        mem[24] = colours & 0xFF;
        mem[25] = (colours >> 8) & 0xFF;
        mem[26] = (colours >> 16) & 0xFF;
        mem[27] = (colours >> 24) & 0xFF;
    }

    /* sound capability flags */
    {
        u32 sound_flags = (digi_driver && digi_driver->id != DIGI_NONE)
                ? 0x0F : 0;  /* tones | samples | MUS | MIDI */
        mem[28] =  sound_flags        & 0xFF;
        mem[29] = (sound_flags >>  8) & 0xFF;
        mem[30] = (sound_flags >> 16) & 0xFF;
        mem[31] = (sound_flags >> 24) & 0xFF;
    }

    allegro_putptr(mem, 32, allegro_platstr(vm, &cursor, "Read-write")); /* file_support  */
    allegro_putptr(mem, 36, allegro_platstr(vm, &cursor, "Hardware"));   /* fpu_support   */
    allegro_putptr(mem, 40, allegro_platstr(vm, &cursor, "Allegro-DOS"));/* architecture  */
    allegro_putptr(mem, 44, allegro_platstr(vm, &cursor, "Software"));   /* gpu_support   */
    allegro_putptr(mem, 48, allegro_platstr(vm, &cursor, "None"));       /* opengl_version*/

    /* window position + size */
    {
        AllegroState *s = (AllegroState *) vm->backend_data;
        u32 ww = s ? (u32) s->virt_w : 0;
        u32 wh = s ? (u32) s->virt_h : 0;
        mem[52] = 0;
        mem[53] = 0;
        mem[54] = 0;
        mem[55] = 0; /* window_x */
        mem[56] = 0;
        mem[57] = 0;
        mem[58] = 0;
        mem[59] = 0; /* window_y */
        mem[60] = ww & 0xFF;
        mem[61] = (ww >> 8) & 0xFF;
        mem[62] = (ww >> 16) & 0xFF;
        mem[63] = (ww >> 24) & 0xFF;
        mem[64] = wh & 0xFF;
        mem[65] = (wh >> 8) & 0xFF;
        mem[66] = (wh >> 16) & 0xFF;
        mem[67] = (wh >> 24) & 0xFF;
    }

    {
        const char *js;
        if (!vm->jit_name || !vm->jit_name[0] || strcmp(vm->jit_name, "null") == 0)
            js = "Not available";
        else if (!vm->jit_enabled)
            js = "Disabled (--no-jit)";
        else if (strstr(vm->jit_name, "-v2"))
            js = "Enabled (JIT v2)";
        else
            js = "Enabled (JIT v1)";
        allegro_putptr(mem, 68, allegro_platstr(vm, &cursor, js)); /* jit_status */
    }

    /* CPU speed: unknown on DOS, cores: 1 */
    mem[72] = 0; mem[73] = 0; mem[74] = 0; mem[75] = 0;
    mem[76] = 1; mem[77] = 0; mem[78] = 0; mem[79] = 0;
    allegro_putptr(mem, 80, allegro_platstr(vm, &cursor, "Unknown"));

    /* --- New fields (offsets 84-107) --- */

    /* touch: no touch hardware on DOS */
    {
        u32 touch = 0;
        mem[84] =  touch        & 0xFF;
        mem[85] = (touch >>  8) & 0xFF;
        mem[86] = (touch >> 16) & 0xFF;
        mem[87] = (touch >> 24) & 0xFF;
    }

    /* network: 1 if the lwIP TCP/IP stack is linked, else 0 */
    {
        u32 network =
#ifdef HAVE_DOSNET
            1
#else
            0
#endif
            ;
        mem[88] =  network        & 0xFF;
        mem[89] = (network >>  8) & 0xFF;
        mem[90] = (network >> 16) & 0xFF;
        mem[91] = (network >> 24) & 0xFF;
    }

#ifndef RELIC_PLATFORM_ID
#define RELIC_PLATFORM_ID "unknown"
#endif
#ifndef RELIC_BACKEND_ID
#define RELIC_BACKEND_ID "allegro-dos"
#endif
    allegro_putptr(mem, 92,  allegro_platstr(vm, &cursor, RELIC_PLATFORM_ID));
    allegro_putptr(mem, 96,  allegro_platstr(vm, &cursor, RELIC_BACKEND_ID));
    allegro_putptr(mem, 100, allegro_platstr(vm, &cursor, RELIC_VERSION));
    allegro_putptr(mem, 104, allegro_platstr(vm, &cursor, __DATE__));

    /* Adaptive Build tail: detected tier + physical RAM (u32 each). */
    allegro_putptr(mem, 108, (u32)relic_tier_detect());
    allegro_putptr(mem, 112, (u32)relic_probe_memory_kb());
}

/* ==========================================================================
 * Subprocess management (@process_spawn/poll/exit_code/read_stdout/
 * read_stderr/kill/close)
 *
 * DOS is single-tasking: there is no concurrent child process. system()
 * swaps the parent image out to disk/XMS, runs the child to completion,
 * then reloads the parent — so a spawn call BLOCKS and the slot is already
 * "exited" by the time it returns. poll() therefore always reports 1 and
 * kill() is a no-op; what the API actually provides on DOS is the ability to
 * run a command and capture its output.
 *
 * stdout/stderr are captured by dup()'ing the real handles, dup2()'ing two
 * temp files onto fds 1/2, running the child (which inherits them), then
 * restoring. COMMAND.COM cannot redirect stderr ("2>" is not a DOS feature),
 * so the fd-level redirect is the only way to capture it.
 *
 * If a graphics mode is active we first drop to BIOS text mode 3 using the
 * same sequence allegro_shutdown() uses (the only mode transition known safe
 * on FreeDOS+JEMM386), giving the child a sane console. After the child exits
 * we re-enter the window by calling allegro_gfx_window() again with the
 * logical size and flags the script originally requested — gfx_driver was
 * cleared, so this is a fresh mode probe rather than an Allegro mode-change
 * (which is exactly the teardown path that crashes on real hardware).
 * ========================================================================== */

/* Build a capture-file path: <TMPDIR|TEMP|TMP|.\>/TPRC<slot><O|E>.TMP
 * (8.3-safe). Slot-scoped names mean a leaked file is overwritten on the
 * next spawn into the same slot rather than accumulating. */
static void dos_proc_tmp_path(char *out, int cap, int slot, char which) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = getenv("TEMP");
    if (!dir || !dir[0]) dir = getenv("TMP");
    if (!dir || !dir[0]) dir = ".";
    snprintf(out, (size_t)cap, "%s/TPRC%d%c.TMP", dir, slot, which);
    out[cap - 1] = 0;
}

/* @process_spawn(cmd: *u8, cwd: *u8) -> i32 handle (>=0) or -1 */
static void allegro_sys_proc_spawn(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }

    u32 cmd_ptr = (u32) vm->r[1];
    u32 cwd_ptr = (u32) vm->r[2];

    if (cmd_ptr == 0 || cmd_ptr >= vm->mem_sz) { vm->r[1] = -1; return; }

    /* Copy command line out of VM memory. */
    char cmdline[1024];
    int cl = 0;
    while (cl < (int)sizeof(cmdline) - 1 && cmd_ptr + (u32)cl < vm->mem_sz
           && vm->mem[cmd_ptr + cl] != 0) {
        cmdline[cl] = (char) vm->mem[cmd_ptr + cl]; cl++;
    }
    cmdline[cl] = '\0';
    if (cl == 0) { vm->r[1] = -1; return; }

    /* Copy cwd out of VM memory (optional). */
    char cwd[512];
    char *cwd_arg = NULL;
    if (cwd_ptr != 0 && cwd_ptr < vm->mem_sz) {
        int wl = 0;
        while (wl < (int)sizeof(cwd) - 1 && cwd_ptr + (u32)wl < vm->mem_sz
               && vm->mem[cwd_ptr + wl] != 0) {
            cwd[wl] = (char) vm->mem[cwd_ptr + wl]; wl++;
        }
        cwd[wl] = '\0';
        if (wl > 0) cwd_arg = cwd;
    }

    /* Find a free slot. */
    int slot = -1;
    for (int i = 0; i < RELIC_MAX_PROC; i++) {
        if (!s->proc[i].in_use) { slot = i; break; }
    }
    if (slot < 0) { vm->r[1] = -1; return; }
    DosProcSlot *p = &s->proc[slot];

    dos_proc_tmp_path(p->out_path, sizeof p->out_path, slot, 'O');
    dos_proc_tmp_path(p->err_path, sizeof p->err_path, slot, 'E');
    p->out_fp = NULL;
    p->err_fp = NULL;

    /* Redirect our stdout/stderr to the capture files; the child inherits
     * the redirected handles. */
    fflush(NULL);
    int saved_out = dup(STDOUT_FILENO);
    int saved_err = dup(STDERR_FILENO);
    int fd_out = open(p->out_path,
                      O_WRONLY | O_CREAT | O_TRUNC | O_TEXT, 0666);
    int fd_err = open(p->err_path,
                      O_WRONLY | O_CREAT | O_TRUNC | O_TEXT, 0666);
    if (saved_out < 0 || saved_err < 0 || fd_out < 0 || fd_err < 0) {
        if (saved_out >= 0) close(saved_out);
        if (saved_err >= 0) close(saved_err);
        if (fd_out >= 0) close(fd_out);
        if (fd_err >= 0) close(fd_err);
        vm->r[1] = -1; return;
    }
    dup2(fd_out, STDOUT_FILENO);
    dup2(fd_err, STDERR_FILENO);
    close(fd_out);
    close(fd_err);

    /* If a graphics mode is up, drop to a text console for the child — the
     * same BIOS-mode-3 sequence shutdown uses (Allegro's own teardown hangs
     * FreeDOS+JEMM386, so don't call set_gfx_mode(GFX_TEXT) here). */
    int was_gfx = s->gfx_mode_set;
    if (was_gfx) {
        show_mouse(NULL);
        remove_int(relic_millis_tick);

        /* The abandoned video-mode bitmaps are about to go stale: clear our
         * refs without destroying them, exactly as the repeat-@window path
         * does before re-entering set_gfx_mode. */
        if (s->page_flip) {
            s->vpage[0] = NULL;
            s->vpage[1] = NULL;
            s->backbuf = NULL;
        }
        s->page_flip = false;
        s->planar_backbuf = false;
        s->vpage_active = 0;

        __dpmi_regs r;
        memset(&r, 0, sizeof r);
        r.x.ax = 0x0003;   /* mode 3: text 80x25 */
        __dpmi_int(0x10, &r);

        __dpmi_regs sr;
        memset(&sr, 0, sizeof sr);
        sr.x.ax = 0x0600;  /* scroll-up whole window = clear */
        sr.x.bx = 0x0700;
        sr.x.cx = 0x0000;
        sr.x.dx = 0x184F;
        __dpmi_int(0x10, &sr);

        /* Allegro must not think a graphics mode is still active: with
         * gfx_driver NULL, the restore below is a fresh probe rather than
         * a driver-exit teardown (the crashing path). */
        gfx_driver = NULL;
        s->gfx_mode_set = false;
    }

    /* Run the child. system() execs programs directly when possible (no
     * COMMAND.COM length limits) and falls back to %COMSPEC% for batch
     * files and shell builtins. */
    char saved_cwd[512];
    int had_cwd = (getcwd(saved_cwd, sizeof saved_cwd) != NULL);
    if (cwd_arg) chdir(cwd_arg);

    errno = 0;
    int rc = system(cmdline);

    if (cwd_arg && had_cwd) chdir(saved_cwd);

    /* Restore our real stdout/stderr. */
    fflush(NULL);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);

    /* Re-enter the graphics mode the script was using. r[3..5] temporarily
     * carry the saved logical size and flags; the original spawn args in
     * r[1..5] are restored afterwards. */
    if (was_gfx) {
        install_int_ex(relic_millis_tick, BPS_TO_TIMER(1000));

        i32 save1 = vm->r[1], save2 = vm->r[2], save3 = vm->r[3],
            save4 = vm->r[4], save5 = vm->r[5];
        vm->r[3] = s->logical_w;
        vm->r[4] = s->logical_h;
        vm->r[5] = (i32) s->win_flags;
        allegro_gfx_window(vm);
        vm->r[1] = save1; vm->r[2] = save2; vm->r[3] = save3;
        vm->r[4] = save4; vm->r[5] = save5;
    }

    if (rc == -1) {
        /* The command never ran. Remove the empty capture files. */
        remove(p->out_path);
        remove(p->err_path);
        vm->r[1] = -1;
        return;
    }

    p->in_use = true;
    p->exit_code = rc;   /* child has already exited: this is final */
    vm->r[1] = slot;
}

/* @process_poll(handle: i32) -> i32 (0=running, 1=exited, -1=invalid) */
static void allegro_sys_proc_poll(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    int handle = vm->r[1];
    if (handle < 0 || handle >= RELIC_MAX_PROC || !s->proc[handle].in_use) {
        vm->r[1] = -1; return;
    }
    /* spawn blocks until the child exits: every valid handle is exited. */
    vm->r[1] = 1;
}

/* @process_exit_code(handle: i32) -> i32 exit code */
static void allegro_sys_proc_exit_code(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    int handle = vm->r[1];
    if (handle < 0 || handle >= RELIC_MAX_PROC || !s->proc[handle].in_use) {
        vm->r[1] = -1; return;
    }
    vm->r[1] = s->proc[handle].exit_code;
}

/* Helper: lazily open a capture file and stream up to cap bytes into VM
 * memory. Returns bytes read (0 at EOF or when the file is absent). */
static int dos_proc_read_file(FILE **fpp, const char *path,
                              u8 *mem, u32 buf_ptr, i32 cap, u32 mem_sz) {
    if (cap <= 0 || buf_ptr == 0 || buf_ptr >= mem_sz) return -1;
    if (cap > (i32) (mem_sz - buf_ptr)) cap = (i32) (mem_sz - buf_ptr);
    if (!*fpp) {
        *fpp = fopen(path, "rb");
        if (!*fpp) return 0;
    }
    return (int) fread(mem + buf_ptr, 1, (size_t) cap, *fpp);
}

/* @process_read_stdout(handle, buf: *u8, cap: i32) -> i32 bytes (0=none) */
static void allegro_sys_proc_read_out(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    int handle  = vm->r[1];
    u32 buf_ptr = (u32) vm->r[2];
    i32 cap     = vm->r[3];
    if (handle < 0 || handle >= RELIC_MAX_PROC || !s->proc[handle].in_use) {
        vm->r[1] = -1; return;
    }
    vm->r[1] = dos_proc_read_file(&s->proc[handle].out_fp,
                                  s->proc[handle].out_path,
                                  vm->mem, buf_ptr, cap, vm->mem_sz);
}

/* @process_read_stderr(handle, buf: *u8, cap: i32) -> i32 bytes (0=none) */
static void allegro_sys_proc_read_err(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    int handle  = vm->r[1];
    u32 buf_ptr = (u32) vm->r[2];
    i32 cap     = vm->r[3];
    if (handle < 0 || handle >= RELIC_MAX_PROC || !s->proc[handle].in_use) {
        vm->r[1] = -1; return;
    }
    vm->r[1] = dos_proc_read_file(&s->proc[handle].err_fp,
                                  s->proc[handle].err_path,
                                  vm->mem, buf_ptr, cap, vm->mem_sz);
}

/* @process_kill(handle: i32) -> i32 (1=killed, 0=already exited) */
static void allegro_sys_proc_kill(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    int handle = vm->r[1];
    if (handle < 0 || handle >= RELIC_MAX_PROC || !s->proc[handle].in_use) {
        vm->r[1] = -1; return;
    }
    vm->r[1] = 0;   /* always already exited */
}

/* @process_close(handle: i32) -> i32 (1=ok, releases slot) */
static void allegro_sys_proc_close(RelicVM *vm) {
    AllegroState *s = (AllegroState *) vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    int handle = vm->r[1];
    if (handle < 0 || handle >= RELIC_MAX_PROC || !s->proc[handle].in_use) {
        vm->r[1] = -1; return;
    }
    DosProcSlot *p = &s->proc[handle];
    if (p->out_fp) fclose(p->out_fp);
    if (p->err_fp) fclose(p->err_fp);
    remove(p->out_path);
    remove(p->err_path);
    memset(p, 0, sizeof *p);
    vm->r[1] = 1;
}

/* Called from allegro_shutdown to release capture files left open. */
static void allegro_proc_shutdown(AllegroState *s) {
    int i;
    for (i = 0; i < RELIC_MAX_PROC; i++) {
        DosProcSlot *p = &s->proc[i];
        if (!p->in_use) continue;
        if (p->out_fp) fclose(p->out_fp);
        if (p->err_fp) fclose(p->err_fp);
        remove(p->out_path);
        remove(p->err_path);
        memset(p, 0, sizeof *p);
    }
}

/* -------------------------------------------------------------------------
 * Sound functions
 * ---------------------------------------------------------------------- */
#define SND_SAMPLE_RATE  11025
#define SND_AMPLITUDE    110
#define SND_SILENCE      128

/* Render a frequency-swept square tone with attack/release envelope.
 * Returns the number of samples written. */
static int allegro_snd_fill_sweep(u8 *buf, int buf_sz,
        int freq_start, int freq_end, int ms, int volume) {
    int samples = (SND_SAMPLE_RATE * ms) / 1000;
    if (samples > buf_sz) samples = buf_sz;
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    int amplitude = (SND_AMPLITUDE * volume) / 255;

    static float filter_state = 0.0f;
    const float alpha = 0.15f;

    for (int i = 0; i < samples; i++) {
        float t = (samples > 1) ? (float) i / (float) (samples - 1) : 0.0f;
        float freq = (float) freq_start + t * (float) (freq_end - freq_start);
        if (freq < 20.0f) freq = 20.0f;
        float half = (float) SND_SAMPLE_RATE / (2.0f * freq);
        int phase = (int) ((float) i / half) & 1;
        float env = 1.0f;
        float ms_f = (float) ms;
        float t_ms = (float) i * 1000.0f / (float) SND_SAMPLE_RATE;
        if (t_ms < 5.0f) env = t_ms / 5.0f;
        else if (t_ms > ms_f - 5.0f) env = (ms_f - t_ms) / 5.0f;
        if (env < 0.0f) env = 0.0f;
        int val = SND_SILENCE + (phase ? 1 : -1) * (int) (amplitude * env);

        float input = (float) val;
        float output = alpha * input + (1.0f - alpha) * filter_state;
        filter_state = output;
        buf[i] = (u8)((int) (output + 0.5f));
    }
    return samples;
}

/* Simple tone segment descriptor for preset sounds. */
typedef struct {
    int f0, f1, ms;
} AllegroSndSeg;

/* Play a sequence of tone segments via Allegro's play_sample. */
static void allegro_snd_play_segs(const AllegroSndSeg *segs, int n, int volume) {
    if (!digi_driver || digi_driver->id == DIGI_NONE) return;
    int total = 0;
    for (int i = 0; i < n; i++)
        total += (SND_SAMPLE_RATE * segs[i].ms) / 1000 + 1;
    u8 *buf = (u8 *) malloc(total);
    if (!buf) return;
    int pos = 0;
    for (int i = 0; i < n; i++)
        pos += allegro_snd_fill_sweep(buf + pos, total - pos,
                segs[i].f0, segs[i].f1, segs[i].ms, volume);
    SAMPLE *smp = create_sample(8, 0, SND_SAMPLE_RATE, pos);
    if (!smp) {
        free(buf);
        return;
    }
    memcpy(smp->data, buf, pos);
    free(buf);
    if (g_snd_prev) {
        stop_sample(g_snd_prev);
        destroy_sample(g_snd_prev);
    }
    play_sample(smp, 255, 128, 1000, FALSE);
    g_snd_prev = smp;
}

/* @tone(freq, ms, volume) — play a tone at a specific frequency. */
static void allegro_snd_tone_freq(RelicVM *vm) {
    if (!digi_driver || digi_driver->id == DIGI_NONE) return;
    int freq = (int) (u16) vm->r[1];
    int ms = (int) (u16) vm->r[2];
    int volume = (int) (u8) vm->r[3];
    if (freq < 20) freq = 20;
    if (ms < 1) ms = 1;
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    int samples = (SND_SAMPLE_RATE * ms) / 1000 + 1;
    u8 *buf = (u8 *) malloc(samples);
    if (!buf) return;
    int len = allegro_snd_fill_sweep(buf, samples, freq, freq, ms, volume);
    SAMPLE *smp = create_sample(8, 0, SND_SAMPLE_RATE, len);
    if (!smp) {
        free(buf);
        return;
    }
    memcpy(smp->data, buf, len);
    free(buf);
    if (g_snd_prev) {
        stop_sample(g_snd_prev);
        destroy_sample(g_snd_prev);
    }
    play_sample(smp, (int)(255 * g_sfx_vol * g_master_vol), 128, 1000, FALSE);
    g_snd_prev = smp;
}

/* @tone_id(id, volume) — play a preset sound effect by ID. */
static void allegro_snd_tone_id(RelicVM *vm) {
    u8 id = (u8) vm->r[1];
    int volume = (int) (u8) vm->r[2];
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;

    switch (id) {
        case 0: {
            AllegroSndSeg s[] = {{300, 120, 60}, {120, 60, 80}, {60, 40, 100}, {40, 30, 120}};
            allegro_snd_play_segs(s, 4, volume);
            break;
        }
        case 1: {
            AllegroSndSeg s[] = {{1600, 200, 120}};
            allegro_snd_play_segs(s, 1, volume);
            break;
        }
        case 2: {
            AllegroSndSeg s[] = {{2000, 1600, 25}, {1600, 1200, 20}};
            allegro_snd_play_segs(s, 2, volume);
            break;
        }
        case 3: {
            AllegroSndSeg s[] = {{1000, 600, 40}, {600, 400, 30}};
            allegro_snd_play_segs(s, 2, volume);
            break;
        }
        case 4: {
            AllegroSndSeg s[] = {{500, 200, 50}, {200, 100, 70}};
            allegro_snd_play_segs(s, 2, volume);
            break;
        }
        case 5: {
            AllegroSndSeg s[] = {{330, 330, 60}, {440, 440, 60}, {550, 550, 60}, {660, 660, 60}, {880, 880, 90}};
            allegro_snd_play_segs(s, 5, volume);
            break;
        }
        case 6: {
            AllegroSndSeg s[] = {{250, 900, 110}};
            allegro_snd_play_segs(s, 1, volume);
            break;
        }
        case 7: {
            AllegroSndSeg s[] = {{880, 660, 80}, {660, 494, 80}, {494, 330, 90}, {330, 220, 100}, {220, 110, 130}};
            allegro_snd_play_segs(s, 5, volume);
            break;
        }
        case 8: {
            AllegroSndSeg s[] = {{988, 988, 55}, {1319, 1319, 110}};
            allegro_snd_play_segs(s, 2, volume);
            break;
        }
        case 9: {
            AllegroSndSeg s[] = {{523, 523, 80}, {659, 659, 80}, {784, 784, 80}, {1047, 1047, 180}};
            allegro_snd_play_segs(s, 4, volume);
            break;
        }
        case 10: {
            AllegroSndSeg s[] = {{300, 150, 40}, {150, 80, 60}, {80, 50, 60}};
            allegro_snd_play_segs(s, 3, volume);
            break;
        }
        case 11: {
            AllegroSndSeg s[] = {{400, 1600, 100}, {1600, 400, 100}};
            allegro_snd_play_segs(s, 2, volume);
            break;
        }
        default:
            break;
    }
}

/* @play(pcm_ptr, len, volume) — play raw unsigned 8-bit mono PCM at
 * SND_SAMPLE_RATE (11025 Hz). Format matches the win32 backend so the
 * same Relic content plays consistently across platforms.
 *
 * We wrap the VM PCM bytes in an Allegro SAMPLE backed by our own malloc'd
 * buffer (Allegro frees SAMPLE->data via free() in destroy_sample, so the
 * buffer must come from malloc). Volume 0..255 maps directly to Allegro's
 * play_sample volume argument. Pan = 128 (centre), freq = 1000 (play at
 * sample's native rate). The sample is leaked-on-end-of-play; Allegro 4
 * doesn't notify on completion without polling, and the cost of a small
 * leak on a one-shot DOS program is acceptable. To keep memory bounded
 * we hold one previous SAMPLE* and destroy it on the next call. */
static void allegro_snd_play(RelicVM *vm) {
    u32 ptr = (u32) vm->r[1];
    u32 len = (u32) vm->r[2];
    int volume = (int) (u8) vm->r[3];

    if (digi_driver == NULL || digi_driver->id == DIGI_NONE) return;
    if (len == 0) return;
    if ((uint64_t) ptr + (uint64_t) len > (uint64_t) vm->mem_sz) return;
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;

    SAMPLE *smp = create_sample(8, 0, SND_SAMPLE_RATE, (int) len);
    if (!smp) return;
    memcpy(smp->data, vm->mem + ptr, len);

    /* Release the previous one-shot sample (if any) before starting a new
     * one, so memory use stays bounded. */
    if (g_snd_prev) {
        stop_sample(g_snd_prev);
        destroy_sample(g_snd_prev);
        g_snd_prev = NULL;
    }

    play_sample(smp, (int)(volume * g_sfx_vol * g_master_vol), 128, 1000, FALSE);
    g_snd_prev = smp;
}

/* -------------------------------------------------------------------------
 * OPL2 FM synthesizer for MUS/MIDI music
 *
 * Ported from the Windows opengl-win32 backend.  On DOS we synthesize the
 * MUS/GENMIDI score in real time, filling small PCM chunks of an Allegro
 * AUDIOSTREAM on demand (pumped each frame from gfx_flip).
 * ---------------------------------------------------------------------- */
#define MUS_MAX_CHANNELS  16
#define MUS_MAX_VOICES    24
#define MUS_TICK_RATE     140
#define MUS_PERC_CHANNEL  15

/* Real-time stream chunk size. Allegro double-buffers the AUDIOSTREAM, so
 * each half holds MUS_STREAM_LEN samples (~186 ms at 11025 Hz). That is a
 * comfortable margin against frame-rate dips while keeping per-frame synth
 * work small. */
#define MUS_STREAM_LEN 2048

/* ----- OPL waveform table ----- */
static float opl_sin_table[1024];
static int opl_tables_ready = 0;

static void opl_build_tables(void) {
    if (opl_tables_ready) return;
    for (int i = 0; i < 1024; i++)
        opl_sin_table[i] = sinf((float) i * 6.283185307f / 1024.0f);
    opl_tables_ready = 1;
}

static float opl_wave(int waveform, u32 phase) {
    int idx = (int) ((phase >> 22) & 0x3FF);
    switch (waveform & 7) {
        case 0:
            return opl_sin_table[idx];
        case 1:
            return (idx < 512) ? opl_sin_table[idx] : 0.0f;
        case 2:
            return (opl_sin_table[idx] < 0) ? -opl_sin_table[idx] : opl_sin_table[idx];
        case 3:
            if (idx < 256) return opl_sin_table[idx];
            if (idx < 512) return 0.0f;
            if (idx < 768) return opl_sin_table[idx - 512];
            return 0.0f;
        case 4:
            return (idx < 512) ? opl_sin_table[(idx * 2) & 0x3FF] : 0.0f;
        case 5: {
            if (idx >= 512) return 0.0f;
            float s = opl_sin_table[(idx * 2) & 0x3FF];
            return (s < 0) ? -s : s;
        }
        case 6:
            return (idx < 512) ? 1.0f : -1.0f;
        case 7: {
            float t = (float) idx * (1.0f / 1024.0f);
            return 2.0f * t - 1.0f;
        }
    }
    return 0.0f;
}

/* ----- OPL ADSR envelope ----- */
typedef struct {
    float level;
    int stage;
    float atk_inc;
    float dec_mul;
    float sus_level;
    float rel_mul;
    int eg_flag;
} OplEnv;

/* pow(2.0, x) -> exp2f throughout this OPL/MIDI section: these were the
 * only reason it pulled in double-precision arithmetic (see softfp_math.c
 * for exp2f's accuracy, ~1e-7 relative -- far tighter than audible pitch
 * discrimination needs).
 *
 * DJGPP's libm.a does not provide exp2f (C99), so on non-soft-float builds
 * we fall back to pow(2.0, x).  Under RELIC_SOFTFP our soft-float runtime
 * supplies exp2f and no double arithmetic is emitted. */
#ifdef RELIC_SOFTFP
#define OPL_EXP2F(x)   exp2f(x)
#else
#define OPL_EXP2F(x)   ((float)pow(2.0, (double)(x)))
#endif
static float opl_decay_mul(int rate) {
    if (rate <= 0) return 1.0f;
    if (rate > 15) rate = 15;
    float f = -7.4493f * (1.0f / 39.28064f) / (float) SND_SAMPLE_RATE;
    return OPL_EXP2F(f * OPL_EXP2F((float) rate));
}

static float opl_attack_inc(int rate) {
    if (rate <= 0) return 0.0f;
    if (rate >= 15) return 1.0f;
    float inc = OPL_EXP2F((float) rate) / 32768.0f;
    inc *= 22050.0f / (float) SND_SAMPLE_RATE;
    if (inc > 1.0f) inc = 1.0f;
    return inc;
}

static void opl_env_init(OplEnv *e, int atk, int dec, int sus, int rel, int eg) {
    e->level = 0.0f;
    e->stage = 0;
    e->atk_inc = opl_attack_inc(atk);
    e->dec_mul = opl_decay_mul(dec);
    e->sus_level = OPL_EXP2F(-0.5f * (float) sus);
    e->rel_mul = opl_decay_mul(rel > 0 ? rel : 1);
    e->eg_flag = eg;
}

#define OPL_ENV_SILENCE 0.0005f

static float opl_env_tick(OplEnv *e) {
    switch (e->stage) {
        case 0:
            e->level += e->atk_inc;
            if (e->level >= 1.0f) {
                e->level = 1.0f;
                e->stage = 1;
            }
            break;
        case 1:
            e->level *= e->dec_mul;
            if (e->level <= e->sus_level) {
                e->level = e->sus_level;
                e->stage = e->eg_flag ? 2 : 3;
            }
            break;
        case 2:
            break;
        case 3:
            e->level *= e->rel_mul;
            if (e->level <= OPL_ENV_SILENCE) {
                e->level = 0.0f;
                e->stage = 4;
            }
            break;
        case 4:
            e->level = 0.0f;
            break;
    }
    return e->level;
}

/* ----- GENMIDI instrument tables ----- */
typedef struct {
    u8 mult;
    int ksr, eg, vib, am, volume, ksl, attack, decay, sustain, release, wave;
} OplOpDef;

typedef struct {
    OplOpDef mod, car;
    int feedback, additive, note_offset;
} OplVoiceDef;

typedef struct {
    int valid, dual, fixed, fixed_note;
    OplVoiceDef v[2];
} OplInstrument;

static OplInstrument g_genmidi_melodic[128];
static OplInstrument g_genmidi_perc[47];
static int g_genmidi_loaded = 0;

static void opl_parse_op(const u8 *d, OplOpDef *op) {
    op->am = (d[0] >> 7) & 1;
    op->vib = (d[0] >> 6) & 1;
    op->eg = (d[0] >> 5) & 1;
    op->ksr = (d[0] >> 4) & 1;
    op->mult = d[0] & 0x0F;
    op->attack = (d[1] >> 4) & 0x0F;
    op->decay = d[1] & 0x0F;
    op->sustain = (d[2] >> 4) & 0x0F;
    op->release = d[2] & 0x0F;
    op->wave = d[3] & 7;
    op->ksl = (d[4] >> 6) & 3;
    op->volume = d[5] & 0x3F;
}

static void opl_parse_voice_def(const u8 *v, OplOpDef *mod, OplOpDef *car,
        int *feedback, int *additive, int *note_offset) {
    opl_parse_op(v + 0, mod);
    *feedback = (v[6] >> 1) & 7;
    *additive = v[6] & 1;
    opl_parse_op(v + 7, car);
    *note_offset = (i16)(v[14] | (v[15] << 8));
}

static void genmidi_load(const u8 *lump, int lump_len) {
    if (!lump || lump_len < 8 + 175 * 36) return;
    if (memcmp(lump, "#OPL_II#", 8) != 0) return;
    const u8 *p = lump + 8;
    for (int i = 0; i < 175; i++, p += 36) {
        OplInstrument *inst = (i < 128) ? &g_genmidi_melodic[i]
                : &g_genmidi_perc[i - 128];
        u16 flags = p[0] | (p[1] << 8);
        inst->dual = (flags & 0x04) ? 1 : 0;
        inst->fixed = (flags & 0x01) ? 1 : 0;
        inst->fixed_note = p[3];
        inst->valid = 1;
        opl_parse_voice_def(p + 4, &inst->v[0].mod, &inst->v[0].car,
                &inst->v[0].feedback, &inst->v[0].additive,
                &inst->v[0].note_offset);
        opl_parse_voice_def(p + 20, &inst->v[1].mod, &inst->v[1].car,
                &inst->v[1].feedback, &inst->v[1].additive,
                &inst->v[1].note_offset);
    }
    g_genmidi_loaded = 1;
}

/* ----- FM voice types and helpers ----- */
static const float opl_mult_table[16] = {
        0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f,
        8.0f, 9.0f, 10.0f, 10.0f, 12.0f, 12.0f, 15.0f, 15.0f
};

typedef struct {
    u32 phase, step;
    OplEnv env;
    int wave;
    float out_level;
} OplOp;

typedef struct {
    int active, channel, note, released;
    float vol;
    OplOp mod, car;
    int feedback, additive;
    float fb_out[2];
} FmVoice;

typedef struct {
    int program, volume, last_velocity;
} MusChannel;

typedef struct {
    int playing;
    u8 *score;
    int score_len, score_start, pos, delay_ticks;
    float samples_to_tick, master;
    MusChannel ch[MUS_MAX_CHANNELS];
    FmVoice voices[MUS_MAX_VOICES];
} MusPlayer;

static MusPlayer g_mus = {0};
static int g_mus_no_cymbals = 0;
static int g_mus_single_voice = 0;

static int mus_note_is_cymbal(int note) {
    switch (note) {
        case 42:
        case 44:
        case 46:
        case 49:
        case 51:
        case 52:
        case 53:
        case 55:
        case 57:
        case 59:
            return 1;
        default:
            return 0;
    }
}

static float dmx_note_freq(int note) {
    return 440.0f * OPL_EXP2F(((float) note - 57.0f) / 12.0f);
}

static void fm_voice_setup_op(OplOp *op, const OplOpDef *def, float freq, float mult_factor) {
    float op_freq = freq * mult_factor;
    /* Phase step for a 32-bit fixed-point phase accumulator. 4294967296.0f
     * (2^32) is exactly representable in float, so the only rounding is in
     * the division and the final multiply -- at worst a few ULPs out of a
     * ~4 billion-count accumulator, i.e. a relative frequency error far
     * below 1 cent. A phase accumulator has no accuracy requirement finer
     * than that, so float here costs nothing audible; this was the only
     * reason this function needed double. */
    op->step = (u32)(op_freq * (4294967296.0f / (float) SND_SAMPLE_RATE));
    op->phase = 0;
    op->wave = def->wave;
    op->out_level = OPL_EXP2F(-0.125f * (float) def->volume);
    opl_env_init(&op->env, def->attack, def->decay, def->sustain, def->release, def->eg);
}

static FmVoice *mus_alloc_voice(void) {
    FmVoice *slot = NULL;
    for (int i = 0; i < MUS_MAX_VOICES; i++) {
        if (!g_mus.voices[i].active) {
            slot = &g_mus.voices[i];
            break;
        }
    }
    if (!slot) {
        float lowest = 1e9f;
        for (int i = 0; i < MUS_MAX_VOICES; i++) {
            if (g_mus.voices[i].released) {
                float e = g_mus.voices[i].car.env.level;
                if (e < lowest) {
                    lowest = e;
                    slot = &g_mus.voices[i];
                }
            }
        }
    }
    if (!slot) {
        float lowest = 1e9f;
        for (int i = 0; i < MUS_MAX_VOICES; i++) {
            float e = g_mus.voices[i].car.env.level;
            if (e < lowest) {
                lowest = e;
                slot = &g_mus.voices[i];
            }
        }
    }
    return slot;
}

static void mus_setup_voice(FmVoice *slot, const OplInstrument *inst, int vidx,
        int channel, int note, float gain, int perc) {
    const OplVoiceDef *vd = &inst->v[vidx];
    int play_note;
    if (inst->fixed) {
        play_note = inst->fixed_note;
    } else if (perc) {
        play_note = 60;
    } else {
        play_note = note + vd->note_offset;
    }
    while (play_note < 0) play_note += 12;
    while (play_note > 127) play_note -= 12;
    float freq = dmx_note_freq(play_note);
    memset(slot, 0, sizeof(*slot));
    slot->active = 1;
    slot->channel = channel;
    slot->note = note;
    slot->vol = gain;
    slot->feedback = vd->feedback;
    slot->additive = vd->additive;
    fm_voice_setup_op(&slot->mod, &vd->mod, freq, opl_mult_table[vd->mod.mult & 0xF]);
    fm_voice_setup_op(&slot->car, &vd->car, freq, opl_mult_table[vd->car.mult & 0xF]);
}

static void mus_note_on(int channel, int note, int velocity) {
    int perc = (channel == MUS_PERC_CHANNEL) ? 1 : 0;
    if (perc && g_mus_no_cymbals && mus_note_is_cymbal(note)) return;

    OplInstrument *inst = NULL;
    if (perc) {
        int pi = note - 35;
        if (pi >= 0 && pi < 47 && g_genmidi_perc[pi].valid)
            inst = &g_genmidi_perc[pi];
    } else {
        int prog = g_mus.ch[channel].program;
        if (prog >= 0 && prog < 128 && g_genmidi_melodic[prog].valid)
            inst = &g_genmidi_melodic[prog];
    }
    if (!inst && g_genmidi_loaded) inst = &g_genmidi_melodic[0];

    int cvol = g_mus.ch[channel].volume;
    if (cvol <= 0) cvol = 127;
    float gain = ((float) velocity / 127.0f) * ((float) cvol / 127.0f);

    if (inst) {
        int play_dual = inst->dual && !g_mus_single_voice;
        float vgain = play_dual ? gain * 0.5f : gain;
        FmVoice *slot = mus_alloc_voice();
        if (!slot) return;
        mus_setup_voice(slot, inst, 0, channel, note, vgain, perc);
        if (play_dual) {
            FmVoice *slot2 = mus_alloc_voice();
            if (slot2) mus_setup_voice(slot2, inst, 1, channel, note, vgain, perc);
        }
        return;
    }

    /* Fallback: basic FM patch when no GENMIDI bank is loaded. */
    FmVoice *slot = mus_alloc_voice();
    if (!slot) return;
    int play_note = perc ? 60 : note;
    while (play_note < 0) play_note += 12;
    while (play_note > 127) play_note -= 12;
    float freq = dmx_note_freq(play_note);
    /* Same phase-step computation, and same precision reasoning, as
     * fm_voice_setup_op above. */
    u32 step = (u32)(freq * (4294967296.0f / (float) SND_SAMPLE_RATE));
    memset(slot, 0, sizeof(*slot));
    slot->active = 1;
    slot->channel = channel;
    slot->note = note;
    slot->vol = gain;
    slot->mod.step = step;
    slot->mod.phase = 0;
    slot->mod.wave = 0;
    slot->mod.out_level = 0.5f;
    opl_env_init(&slot->mod.env, 12, 6, 8, 6, 1);
    slot->car.step = step;
    slot->car.phase = 0;
    slot->car.wave = 0;
    slot->car.out_level = 1.0f;
    opl_env_init(&slot->car.env, 12, 4, 10, 5, 1);
    slot->feedback = 3;
    slot->additive = 0;
}

static void mus_note_off(int channel, int note) {
    for (int i = 0; i < MUS_MAX_VOICES; i++) {
        FmVoice *v = &g_mus.voices[i];
        if (v->active && v->channel == channel && v->note == note && !v->released) {
            v->released = 1;
            v->mod.env.stage = 3;
            v->car.env.stage = 3;
        }
    }
}

static void mus_channel_silence(int channel) {
    for (int i = 0; i < MUS_MAX_VOICES; i++) {
        FmVoice *v = &g_mus.voices[i];
        if (v->active && v->channel == channel && !v->released) {
            v->released = 1;
            v->mod.env.stage = 3;
            v->car.env.stage = 3;
        }
    }
}

static int mus_read_varlen(void) {
    int value = 0;
    while (g_mus.pos < g_mus.score_len) {
        u8 b = g_mus.score[g_mus.pos++];
        value = (value << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return value;
}

static void mus_do_tick(void) {
    if (!g_mus.playing || !g_mus.score) return;
    if (g_mus.delay_ticks > 0) {
        g_mus.delay_ticks--;
        return;
    }

    while (g_mus.playing) {
        if (g_mus.pos >= g_mus.score_len) {
            g_mus.pos = g_mus.score_start;
            g_mus.delay_ticks = 0;
            return;
        }
        u8 event = g_mus.score[g_mus.pos++];
        int type = (event >> 4) & 7;
        int channel = event & 0x0F;
        int last = event & 0x80;

        if (type == 0) {
            if (g_mus.pos < g_mus.score_len) {
                int note = g_mus.score[g_mus.pos++] & 0x7F;
                mus_note_off(channel, note);
            }
        } else if (type == 1) {
            int note = 0;
            int vel = g_mus.ch[channel].last_velocity;
            if (g_mus.pos < g_mus.score_len) {
                u8 nb = g_mus.score[g_mus.pos++];
                note = nb & 0x7F;
                if (nb & 0x80 && g_mus.pos < g_mus.score_len) {
                    vel = g_mus.score[g_mus.pos++] & 0x7F;
                    g_mus.ch[channel].last_velocity = vel;
                }
            }
            mus_note_on(channel, note, vel);
        } else if (type == 2) {
            if (g_mus.pos < g_mus.score_len) g_mus.pos++;
        } else if (type == 3) {
            if (g_mus.pos < g_mus.score_len) {
                int ctrl = g_mus.score[g_mus.pos++] & 0x7F;
                if (ctrl == 10 || ctrl == 11) mus_channel_silence(channel);
            }
        } else if (type == 4) {
            int ctrl = 0, val = 0;
            if (g_mus.pos < g_mus.score_len) ctrl = g_mus.score[g_mus.pos++] & 0x7F;
            if (g_mus.pos < g_mus.score_len) val = g_mus.score[g_mus.pos++] & 0x7F;
            if (ctrl == 0) g_mus.ch[channel].program = val;
            else if (ctrl == 3) g_mus.ch[channel].volume = val;
        } else if (type == 6) {
            g_mus.pos = g_mus.score_start;
            g_mus.delay_ticks = 0;
            return;
        }

        if (last) {
            g_mus.delay_ticks = mus_read_varlen();
            if (g_mus.delay_ticks > 0) {
                g_mus.delay_ticks--;
                return;
            }
        }
    }
}

/* ----- FM voice rendering ----- */
/* Every entry is an exact power of two (2^27..2^33, and 2^34 for
 * g_opl_mod_depth below), so float represents all of them exactly -- a
 * power of two only needs mantissa 1.0, regardless of magnitude. double was
 * never buying any precision here; it was the only reason this file needed
 * double arithmetic at all. */
static const float opl_fb_scale[8] = {
        0.0f, 134217728.0f, 268435456.0f, 536870912.0f,
        1073741824.0f, 2147483648.0f, 4294967296.0f, 8589934592.0f,
};
static float g_opl_mod_depth = (4.0f * 4294967296.0f);

static float fm_voice_sample(FmVoice *v) {
    float mod_env = opl_env_tick(&v->mod.env);
    float car_env = opl_env_tick(&v->car.env);
    float fb = 0.0f;
    if (v->feedback > 0)
        fb = (v->fb_out[0] + v->fb_out[1]) * 0.5f;
    u32 mod_phase = v->mod.phase + (u32)(i64)(fb * opl_fb_scale[v->feedback]);
    float mod_out = opl_wave(v->mod.wave, mod_phase) * mod_env * v->mod.out_level;
    v->fb_out[0] = v->fb_out[1];
    v->fb_out[1] = mod_out;
    v->mod.phase += v->mod.step;

    float result;
    if (v->additive) {
        float car_out = opl_wave(v->car.wave, v->car.phase) * car_env * v->car.out_level;
        result = (mod_out + car_out) * 0.5f;
    } else {
        u32 car_phase = v->car.phase + (u32)(i64)(mod_out * g_opl_mod_depth);
        float car_out = opl_wave(v->car.wave, car_phase) * car_env * v->car.out_level;
        result = car_out;
    }
    v->car.phase += v->car.step;
    if (v->car.env.stage == 4) v->active = 0;
    return result;
}

/* Render `n` samples of music into `dst` (centred at 128). */
static void mus_render(u8 *dst, int n) {
    if (!g_mus.playing || !g_mus.score) return;
    const float samples_per_tick = (float) SND_SAMPLE_RATE / (float) MUS_TICK_RATE;
    const float amp = 32.0f;

    int rendered = 0;
    while (rendered < n) {
        if (g_mus.samples_to_tick <= 0.0f) {
            mus_do_tick();
            g_mus.samples_to_tick += samples_per_tick;
        }
        int block = n - rendered;
        if ((float) block > g_mus.samples_to_tick)
            block = (int) g_mus.samples_to_tick;
        if (block <= 0) block = 1;

        for (int i = 0; i < block; i++) {
            float acc = 0.0f;
            for (int vi = 0; vi < MUS_MAX_VOICES; vi++) {
                FmVoice *v = &g_mus.voices[vi];
                if (!v->active) continue;
                acc += fm_voice_sample(v) * v->vol;
            }
            int idx = rendered + i;
            int mixed = (int) dst[idx] - 128 + (int) (acc * amp * g_mus.master);
            if (mixed > 127) mixed = 127;
            if (mixed < -128) mixed = -128;
            dst[idx] = (u8)(mixed + 128);
        }
        g_mus.samples_to_tick -= (float) block;
        rendered += block;
    }
}

static int mus_start(const u8 *buf, int len) {
    if (!buf || len < 16) return 0;
    if (buf[0] != 'M' || buf[1] != 'U' || buf[2] != 'S' || buf[3] != 0x1A)
        return 0;
    int score_len = buf[4] | (buf[5] << 8);
    int score_start = buf[6] | (buf[7] << 8);
    if (score_start < 16 || score_start >= len) return 0;
    int end = score_start + score_len;
    if (score_len <= 0 || end > len) end = len;

    opl_build_tables();

    u8 *copy = (u8 *) malloc(len);
    if (!copy) return 0;
    memcpy(copy, buf, len);

    if (g_mus.score) free(g_mus.score);
    memset(&g_mus, 0, sizeof(g_mus));
    g_mus.score = copy;
    g_mus.score_len = end;
    g_mus.score_start = score_start;
    g_mus.pos = score_start;
    g_mus.delay_ticks = 0;
    g_mus.samples_to_tick = 0.0f;
    g_mus.master = 1.0f;
    for (int i = 0; i < MUS_MAX_CHANNELS; i++) {
        g_mus.ch[i].volume = 127;
        g_mus.ch[i].last_velocity = 100;
    }
    g_mus.playing = 1;
    return 1;
}

static void mus_stop(void) {
    g_mus.playing = 0;
    for (int i = 0; i < MUS_MAX_VOICES; i++) g_mus.voices[i].active = 0;
    if (g_mus.score) {
        free(g_mus.score);
        g_mus.score = NULL;
    }
}

/* -------------------------------------------------------------------------
 * MUS → MIDI conversion (shared logic with opengl-win32 backend).
 *
 * Converts a MUS music lump into a Standard MIDI File (Format 0) in memory.
 * The caller must free() the returned buffer. Returns NULL on failure.
 * ---------------------------------------------------------------------- */
static u8 *mus_to_midi(const u8 *mus_data, int mus_len, int *out_len) {
    if (!mus_data || mus_len < 16) return NULL;
    if (mus_data[0] != 'M' || mus_data[1] != 'U' || mus_data[2] != 'S' || mus_data[3] != 0x1A) return NULL;

    int score_len = mus_data[4] | (mus_data[5] << 8);
    int score_start = mus_data[6] | (mus_data[7] << 8);
    if (score_start < 12 || score_start >= mus_len) return NULL;
    int score_end = score_start + score_len;
    if (score_len <= 0 || score_end > mus_len) score_end = mus_len;

#define MUS2MIDI_CH(c) ((c)==15 ? 9 : ((c)<9 ? (c) : (c)+1))

#define WRITE_VLQ(val) do { \
        u32 _v = (u32)(val); \
        u8 _vlq[5]; int _n = 0; \
        _vlq[_n++] = _v & 0x7F; \
        while ((_v >>= 7) > 0) { _vlq[_n++] = 0x80 | (_v & 0x7F); } \
        while (_n > 0) { if (wp < cap) mid[wp++] = _vlq[--_n]; } \
    } while(0)

    int cap = mus_len * 3 + 256;
    u8 *mid = (u8 *) malloc(cap);
    if (!mid) return NULL;
    int wp = 0;

    /* MThd */
    memcpy(mid + wp, "MThd", 4);
    wp += 4;
    mid[wp++] = 0;
    mid[wp++] = 0;
    mid[wp++] = 0;
    mid[wp++] = 6;
    mid[wp++] = 0;
    mid[wp++] = 0; /* format 0 */
    mid[wp++] = 0;
    mid[wp++] = 1; /* 1 track */
    mid[wp++] = 0;
    mid[wp++] = 140; /* division = 140 ticks/qn */

    /* MTrk header */
    memcpy(mid + wp, "MTrk", 4);
    wp += 4;
    int trk_len_pos = wp;
    mid[wp++] = 0;
    mid[wp++] = 0;
    mid[wp++] = 0;
    mid[wp++] = 0;
    int trk_start = wp;

    /* Tempo: 1000000 µs/qn (60 BPM). division=140 → 140 ticks/sec = MUS rate. */
    mid[wp++] = 0;
    mid[wp++] = 0xFF;
    mid[wp++] = 0x51;
    mid[wp++] = 0x03;
    mid[wp++] = 0x0F;
    mid[wp++] = 0x42;
    mid[wp++] = 0x40;

    u32 pending_delay = 0;
    int rp = score_start;
    while (rp < score_end && wp + 16 < cap) {
        u8 event = mus_data[rp++];
        int type = (event >> 4) & 7;
        int channel = event & 0x0F;
        int last = event & 0x80;
        int midi_ch = MUS2MIDI_CH(channel);

        if (type == 6) break;

        if (type == 0) {
            if (rp >= score_end) break;
            int note = mus_data[rp++] & 0x7F;
            WRITE_VLQ(pending_delay);
            pending_delay = 0;
            mid[wp++] = 0x80 | midi_ch;
            mid[wp++] = (u8) note;
            mid[wp++] = 64;
        } else if (type == 1) {
            if (rp >= score_end) break;
            u8 nb = mus_data[rp++];
            int note = nb & 0x7F;
            int vel = 127;
            if (nb & 0x80) {
                if (rp >= score_end) break;
                vel = mus_data[rp++] & 0x7F;
            }
            WRITE_VLQ(pending_delay);
            pending_delay = 0;
            mid[wp++] = 0x90 | midi_ch;
            mid[wp++] = (u8) note;
            mid[wp++] = (u8) vel;
        } else if (type == 2) {
            if (rp >= score_end) break;
            int bend_val = mus_data[rp++];
            int midi_bend = bend_val * 64;
            WRITE_VLQ(pending_delay);
            pending_delay = 0;
            mid[wp++] = 0xE0 | midi_ch;
            mid[wp++] = midi_bend & 0x7F;
            mid[wp++] = (midi_bend >> 7) & 0x7F;
        } else if (type == 3) {
            if (rp >= score_end) break;
            int ctrl = mus_data[rp++] & 0x7F;
            WRITE_VLQ(pending_delay);
            pending_delay = 0;
            mid[wp++] = 0xB0 | midi_ch;
            if (ctrl == 10 || ctrl == 11) {
                mid[wp++] = 123;
                mid[wp++] = 0;
            }
            else if (ctrl == 12) {
                mid[wp++] = 120;
                mid[wp++] = 0;
            }
            else if (ctrl == 14) {
                mid[wp++] = 121;
                mid[wp++] = 0;
            }
            else {
                mid[wp++] = 0;
                mid[wp++] = 0;
            }
        } else if (type == 4) {
            if (rp + 1 >= score_end) break;
            int ctrl = mus_data[rp++] & 0x7F;
            int val = mus_data[rp++] & 0x7F;
            WRITE_VLQ(pending_delay);
            pending_delay = 0;
            if (ctrl == 0) {
                mid[wp++] = 0xC0 | midi_ch;
                mid[wp++] = (u8) val;
            } else {
                mid[wp++] = 0xB0 | midi_ch;
                switch (ctrl) {
                    case 1:
                        mid[wp++] = 0;
                        mid[wp++] = (u8) val;
                        break;
                    case 2:
                        mid[wp++] = 1;
                        mid[wp++] = (u8) val;
                        break;
                    case 3:
                        mid[wp++] = 7;
                        mid[wp++] = (u8) val;
                        break;
                    case 4:
                        mid[wp++] = 10;
                        mid[wp++] = (u8) val;
                        break;
                    case 5:
                        mid[wp++] = 11;
                        mid[wp++] = (u8) val;
                        break;
                    case 6:
                        mid[wp++] = 91;
                        mid[wp++] = (u8) val;
                        break;
                    case 7:
                        mid[wp++] = 93;
                        mid[wp++] = (u8) val;
                        break;
                    case 8:
                        mid[wp++] = 64;
                        mid[wp++] = (u8) val;
                        break;
                    case 9:
                        mid[wp++] = 67;
                        mid[wp++] = (u8) val;
                        break;
                    default:
                        mid[wp++] = 0;
                        mid[wp++] = 0;
                        break;
                }
            }
        }

        if (last) {
            u32 delay = 0;
            while (rp < score_end) {
                u8 db = mus_data[rp++];
                delay = (delay << 7) | (db & 0x7F);
                if (!(db & 0x80)) break;
            }
            pending_delay += delay;
        }
    }

    /* End-of-track meta event */
    WRITE_VLQ(pending_delay);
    mid[wp++] = 0xFF;
    mid[wp++] = 0x2F;
    mid[wp++] = 0x00;

    /* Patch MTrk length */
    int trk_len = wp - trk_start;
    mid[trk_len_pos + 0] = (trk_len >> 24) & 0xFF;
    mid[trk_len_pos + 1] = (trk_len >> 16) & 0xFF;
    mid[trk_len_pos + 2] = (trk_len >> 8) & 0xFF;
    mid[trk_len_pos + 3] = trk_len & 0xFF;

#undef WRITE_VLQ
#undef MUS2MIDI_CH

    *out_len = wp;
    return mid;
}

/* Build an Allegro MIDI struct from raw MIDI file bytes in memory.
 * The returned MIDI* must be freed with destroy_midi(). Returns NULL on error. */
static MIDI *midi_from_memory(const u8 *buf, int len) {
    if (!buf || len < 14) return NULL;
    if (buf[0] != 'M' || buf[1] != 'T' || buf[2] != 'h' || buf[3] != 'd') return NULL;

    int hdr_len = (buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7];
    int format = (buf[8] << 8) | buf[9];
    int ntracks = (buf[10] << 8) | buf[11];
    int division = (buf[12] << 8) | buf[13];
    (void) hdr_len;

    if (format > 1 || ntracks < 1 || ntracks > MIDI_TRACKS || division <= 0)
        return NULL;

    MIDI *midi = (MIDI *) malloc(sizeof(MIDI));
    if (!midi) return NULL;
    memset(midi, 0, sizeof(MIDI));
    midi->divisions = division;

    int pos = 8 + hdr_len;
    for (int t = 0; t < ntracks && pos + 8 <= len; t++) {
        if (buf[pos] != 'M' || buf[pos + 1] != 'T' || buf[pos + 2] != 'r' || buf[pos + 3] != 'k') {
            pos += 8;
            continue;
        }
        int tlen = (buf[pos + 4] << 24) | (buf[pos + 5] << 16) | (buf[pos + 6] << 8) | buf[pos + 7];
        pos += 8;
        if (pos + tlen > len) tlen = len - pos;

        u8 *track_data = (u8 *) malloc(tlen);
        if (!track_data) {
            destroy_midi(midi);
            return NULL;
        }
        memcpy(track_data, buf + pos, tlen);
        midi->track[t].data = track_data;
        midi->track[t].len = tlen;
        pos += tlen;
    }
    return midi;
}

/* Stop any currently playing music (MIDI or pre-rendered MUS sample). */
static void allegro_music_stop_all(void) {
    if (g_midi_playing) {
        stop_midi();
        destroy_midi(g_midi_playing);
        g_midi_playing = NULL;
    }
    if (g_mus_sample) {
        stop_sample(g_mus_sample);
        destroy_sample(g_mus_sample);
        g_mus_sample = NULL;
    }
    if (g_mus_stream) {
        stop_audio_stream(g_mus_stream);
        g_mus_stream = NULL;
    }
    mus_stop();
}

/* Play music via Allegro's native MIDI driver. Returns 1 on success. */
static int allegro_midi_play(const u8 *buf, int len) {
    MIDI *m = midi_from_memory(buf, len);
    if (!m) return 0;
    if (play_midi(m, TRUE) != 0) {
        destroy_midi(m);
        return 0;
    }
    g_midi_playing = m;
    return 1;
}

/* Start real-time OPL2 FM playback of a MUS score. Returns immediately;
 * the audio is synthesized on demand by mus_stream_poll() (called from
 * gfx_flip each frame), so there is no long pre-render delay at startup. */
static void allegro_fm_stream_start(const u8 *buf, int len) {
    if (!digi_driver || digi_driver->id == DIGI_NONE) return;
    if (!mus_start(buf, len)) return;

    g_mus_stream = play_audio_stream(MUS_STREAM_LEN, 8, FALSE,
            SND_SAMPLE_RATE, 255, 128);
    if (!g_mus_stream) {
        mus_stop();
        return;
    }
}

/* Fill one pending AUDIOSTREAM buffer with freshly synthesized FM audio.
 * Called every frame from gfx_flip; a no-op when no music is streaming or
 * when Allegro has no buffer ready to refill yet. */
static void mus_stream_poll(void) {
    if (!g_mus_stream || !g_mus.playing) return;
    void *mem = get_audio_stream_buffer(g_mus_stream);
    if (!mem) return;
    u8 *p = (u8 *) mem;
    memset(p, 128, MUS_STREAM_LEN);
    mus_render(p, MUS_STREAM_LEN);
    if (g_master_vol < 1.0f) {
        for (int i = 0; i < MUS_STREAM_LEN; i++) {
            int s = (int)p[i] - 128;
            s = (int)(s * g_master_vol);
            p[i] = (u8)(s + 128);
        }
    }
    free_audio_stream_buffer(g_mus_stream);
}

/* @music(ptr, len, cmd)
 *   cmd=0: stop all music
 *   cmd=1: play via OPL2 FM synth (real-time stream); falls back to native MIDI
 *   cmd=2: play via native Allegro MIDI (real OPL hardware); falls back to FM
 *   cmd=3: load a GENMIDI bank into the software OPL2 FM synth
 */
static void allegro_snd_music(RelicVM *vm) {
    u32 ptr = (u32) vm->r[1];
    u32 len = (u32) vm->r[2];
    int cmd = vm->r[3];

    if (cmd == 0) {
        allegro_music_stop_all();
        return;
    }

    if (ptr + len > vm->mem_sz) return;

    /* cmd=3: load GENMIDI bank into the software OPL2 FM synth. */
    if (cmd == 3) {
        genmidi_load(vm->mem + ptr, (int) len);
        return;
    }

    /* Stop any currently playing music. */
    allegro_music_stop_all();

    const u8 *buf = vm->mem + ptr;
    int is_midi = (len >= 4 && buf[0] == 'M' && buf[1] == 'T' && buf[2] == 'h' && buf[3] == 'd');
    int is_mus = (len >= 4 && buf[0] == 'M' && buf[1] == 'U' && buf[2] == 'S' && buf[3] == 0x1A);

    g_mus_no_cymbals = 0;
    g_mus_single_voice = 0;
    g_opl_mod_depth = (4.0f * 4294967296.0f);

    if (cmd == 1) {
        /* Best path: OPL2 FM synth real-time stream (sounds closest to the classic game's). */
        if (is_mus) {
            allegro_fm_stream_start(buf, (int) len);
            return;
        }
        /* For raw MIDI, try native Allegro MIDI. */
        if (is_midi && midi_driver && midi_driver->id != MIDI_NONE) {
            if (allegro_midi_play(buf, (int) len)) return;
        }
        return;
    }

    if (cmd == 2) {
        /* Native Allegro MIDI (real OPL hardware on DOS). */
        if (midi_driver && midi_driver->id != MIDI_NONE) {
            if (is_midi) {
                if (allegro_midi_play(buf, (int) len)) return;
            } else if (is_mus) {
                int midi_len = 0;
                u8 *midi_buf = mus_to_midi(buf, (int) len, &midi_len);
                if (midi_buf) {
                    int ok = allegro_midi_play(midi_buf, midi_len);
                    free(midi_buf);
                    if (ok) return;
                }
            }
        }
        /* Fallback: real-time FM synth stream. */
        if (is_mus) allegro_fm_stream_start(buf, (int) len);
        return;
    }
}

static void allegro_snd_mixer(RelicVM *vm) {
    u8 cmd   = (u8)vm->r[1];
    u8 value = (u8)vm->r[2];
    float f  = (float)value / 255.0f;

    switch (cmd) {
        case 0: g_master_vol = f; break;
        case 1: g_mus.master = f; break;
        case 2: g_sfx_vol = f; break;
        case 3:
            if (value == 0) {
                if (g_muted_master < 0.0f) {
                    g_muted_master = g_master_vol;
                    g_master_vol = 0.0f;
                }
            } else {
                if (g_muted_master >= 0.0f) {
                    g_master_vol = g_muted_master;
                    g_muted_master = -1.0f;
                }
            }
            break;
        default: break;
    }
}

/* ------------------------------------------------------------------------- */
/* Web / HTTP                                                                */
/* ------------------------------------------------------------------------- */
#ifdef HAVE_DOSNET
static void allegro_sys_http_request(RelicVM *vm) {
    AllegroState *s = (AllegroState *)vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    dos_http_request(vm, s->http);
}
static void allegro_sys_http_poll(RelicVM *vm) {
    AllegroState *s = (AllegroState *)vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    dos_http_poll(vm, s->http);
}
static void allegro_sys_http_status(RelicVM *vm) {
    AllegroState *s = (AllegroState *)vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    dos_http_status(vm, s->http);
}
static void allegro_sys_http_read(RelicVM *vm) {
    AllegroState *s = (AllegroState *)vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    dos_http_read(vm, s->http);
}
static void allegro_sys_http_close(RelicVM *vm) {
    AllegroState *s = (AllegroState *)vm->backend_data;
    if (!s) return;
    dos_http_close(vm, s->http);
}
#else
static void allegro_sys_http_request(RelicVM *vm) { vm->r[1] = -1; }
static void allegro_sys_http_poll(RelicVM *vm)    { vm->r[1] = -1; }
static void allegro_sys_http_status(RelicVM *vm)  { vm->r[1] = -1; }
static void allegro_sys_http_read(RelicVM *vm)    { vm->r[1] = -1; }
static void allegro_sys_http_close(RelicVM *vm)   { (void)vm; }
#endif

/* =========================================================================
 * Sector-based world renderer (@wr_*)
 *
 * The renderer engine itself lives in wr_render.h (shared across every
 * backend that supports @wr_*). This section provides only the small set
 * of DOS/Allegro-specific hooks the engine needs: raw pixel-row access
 * (WR_ROW/WR_PITCH), hardware palette upload, the planar-page-flip-
 * backbuffer fixup, and backbuffer clearing. See wr_render.h's top
 * comment for the full portability contract.
 * ---------------------------------------------------------------------- */
#define WrHostState AllegroState

#define WR_HAS_BUF(s)   ((s)->backbuf)
#define WR_ROW(s, y)    ((unsigned char *) (s)->backbuf->line[(y)])
#define WR_PITCH(s, y)  ((int) ((unsigned char *) (s)->backbuf->line[(y) + 1] - (unsigned char *) (s)->backbuf->line[(y)]))
#define WR_BUF_W(s)     ((s)->backbuf->w)
#define WR_BUF_H(s)     ((s)->backbuf->h)
#define WR_MILLIS()     (g_millis_counter)

static void wr_host_clear_backbuf(AllegroState *s, int ox, int oy, int w, int h) {
    if (ox == 0 && oy == 0 && w == s->backbuf->w && h == s->backbuf->h) {
        if (bitmap_color_depth(s->backbuf) == 8 && !is_video_bitmap(s->backbuf)) {
            memset(s->backbuf->dat, 0, (size_t) s->backbuf->w * (size_t) s->backbuf->h);
        } else {
            clear_to_color(s->backbuf, 0);
        }
    } else {
        if (bitmap_color_depth(s->backbuf) == 8 && !is_video_bitmap(s->backbuf)) {
            int y;
            for (y = 0; y < h; y++) {
                unsigned char *row = (unsigned char *) s->backbuf->line[y + oy];
                memset(row + ox, 0, (size_t) w);
            }
        } else {
            set_clip_rect(s->backbuf, ox, oy, ox + w - 1, oy + h - 1);
            clear_to_color(s->backbuf, 0);
            set_clip_rect(s->backbuf, 0, 0, s->backbuf->w - 1, s->backbuf->h - 1);
        }
    }
}

static void wr_host_apply_palette(AllegroState *s) {
    if (s->color_depth != 8) return;
    PALETTE pal;
    memset(pal, 0, sizeof(pal));
    int i;
    for (i = 0; i < 256; i++) {
        pal[i].r = (unsigned char) (s->wr_playpal[i * 3 + 0] >> 2);
        pal[i].g = (unsigned char) (s->wr_playpal[i * 3 + 1] >> 2);
        pal[i].b = (unsigned char) (s->wr_playpal[i * 3 + 2] >> 2);
    }
    set_palette(pal);
}

static void wr_host_on_map_uploaded(AllegroState *s) {
    if (s->wr_map_set && s->page_flip && s->planar_backbuf) {
        int rw = s->backbuf ? s->backbuf->w : 0;
        int rh = s->backbuf ? s->backbuf->h : 0;
        if (rw > 0 && rh > 0) {
            BITMAP *ram = create_bitmap_ex(s->color_depth, rw, rh);
            if (ram) {
                if (s->planar_stage) { free(s->planar_stage); s->planar_stage = NULL; }
                if (s->planar_dirty) { free(s->planar_dirty); s->planar_dirty = NULL; }
                s->backbuf = ram;
                s->planar_backbuf = false;
            }
        }
    }
}

static void wr_host_present(AllegroState *s, int x, int y, int w, int h) {
    (void) s; (void) x; (void) y; (void) w; (void) h;
}

#include "../common/wr/wr_render.h"

/* -------------------------------------------------------------------------
 * Backend descriptor
 * ---------------------------------------------------------------------- */
/* Present the sdfill MODE 2 image over the logical 320x224 view: each
 * MODE 2 pixel is 2x1 logical pixels, nearest-scaled through putpixel
 * (any colour depth). The HUD band below is cleared black. */
static void allegro_sdf_present(void *ctx, int true_mode) {
    AllegroState *s = (AllegroState*)ctx;
    BITMAP *dst = DRAW_TGT(s);
    int ox, oy, vw = s->logical_w, vh = s->logical_h;
    int vh224;
    if (!dst || vw <= 0 || vh <= 0) return;
    vh224 = vh * SDF_VROWS / 256;
    for (oy = 0; oy < vh; oy++) {
        int row = oy < vh224 ? oy * SDF_VROWS / vh224 : 0;
        for (ox = 0; ox < vw; ox++) {
            u32 c;
            if (oy >= vh224) { putpixel(dst, ox, oy, makecol(0, 0, 0)); continue; }
            if (true_mode) {
                c = sdt_px[oy * SDF_VROWS * 2 / vh224][ox * SDT_W / vw];
            } else {
                c = l3f_bbc[sdf_px(ox * 160 / vw, row)];
            }
            putpixel(dst, ox, oy, makecol(c & 255, (c >> 8) & 255, (c >> 16) & 255));
        }
    }
}

static void allegro_gfx_lines3d(RelicVM *vm) {
    AllegroState *s = (AllegroState*)vm->backend_data;
    i32 r;
    if (!s) { vm->r[1] = -1; return; }
    r = relic_l3_fill(vm, allegro_sdf_present, s);
    if (r != L3F_NOTFILL) { vm->r[1] = r; return; }
    relic_lines3d(vm, s->logical_w, s->logical_h);
}

RelicBackend relic_backend = {
        .init         = allegro_init_backend,
        .shutdown     = allegro_shutdown,
        .gfx_window   = allegro_gfx_window,
        .gfx_clear    = allegro_gfx_clear,
        .gfx_sprite   = allegro_gfx_sprite,
        .gfx_plot     = allegro_gfx_plot,
        .gfx_flip     = allegro_gfx_flip,
        .gfx_line     = allegro_gfx_line,
        .gfx_polygon  = allegro_gfx_polygon,
        .gfx_polyline = allegro_gfx_polyline,
        .gfx_camera   = allegro_gfx_camera,
        .gfx_viewport = allegro_gfx_viewport,
        .gfx_texture  = allegro_gfx_texture,
        .gfx_palette_upload = allegro_gfx_palette_upload,
        .wr_palette   = wr_op_palette,
        .wr_texture   = wr_op_texture,
        .wr_flat      = wr_op_flat,
        .wr_test      = wr_op_test,
        .wr_map       = wr_op_map,
        .wr_render    = wr_op_render,
        .wr_texmap    = wr_op_texmap,
        .wr_sky       = wr_op_sky,
        .wr_flatshade = wr_op_flatshade,
        .wr_patch     = wr_op_patch,
        .wr_sprite   = wr_op_sprite,
        .gfx_light    = allegro_gfx_light,
        .gfx_text     = allegro_gfx_text,
        .gfx_text_measure = allegro_gfx_text_measure,
        .gfx_clip     = allegro_gfx_clip,
        .gfx_draw_list = allegro_gfx_draw_list,
        .gfx_lines3d   = allegro_gfx_lines3d,
        .gfx_ellipse              = allegro_gfx_ellipse,
        .gfx_ellipse_filled      = allegro_gfx_ellipse_filled,
        .gfx_rounded_rect         = allegro_gfx_rounded_rect,
        .gfx_rounded_rect_filled = allegro_gfx_rounded_rect_filled,
        .gfx_mesh     = allegro_gfx_mesh,
        .inp_key      = allegro_inp_key,
        .inp_keyheld  = allegro_inp_keyheld,
        .inp_mouse    = allegro_inp_mouse,
        .sys_rand     = allegro_sys_rand,
        .sys_millis   = allegro_sys_millis,
        .sys_console  = allegro_sys_console,
        .sys_fopen    = allegro_sys_fopen,
        .sys_fread    = allegro_sys_fread,
        .sys_fwrite   = allegro_sys_fwrite,
        .sys_fclose   = allegro_sys_fclose,
        .sys_fseek    = allegro_sys_fseek,
        .sys_flen     = allegro_sys_flen,
        .sys_platform = allegro_sys_platform,
        .sys_clip_set = local_clip_set,
        .sys_clip_get = local_clip_get,
        .sys_http_request = allegro_sys_http_request,
        .sys_http_poll    = allegro_sys_http_poll,
        .sys_http_status  = allegro_sys_http_status,
        .sys_http_read    = allegro_sys_http_read,
        .sys_http_close   = allegro_sys_http_close,
#if defined(HAVE_DOSNET) && defined(TIGRYX_EXPERIMENTAL_SERVER)
        .sys_serve        = dos_sys_serve,
        .sys_serve_reply  = dos_sys_serve_reply,
#endif
        .sys_proc_spawn     = allegro_sys_proc_spawn,
        .sys_proc_poll      = allegro_sys_proc_poll,
        .sys_proc_exit_code = allegro_sys_proc_exit_code,
        .sys_proc_read_out  = allegro_sys_proc_read_out,
        .sys_proc_read_err  = allegro_sys_proc_read_err,
        .sys_proc_kill      = allegro_sys_proc_kill,
        .sys_proc_close     = allegro_sys_proc_close,
        .snd_tone_freq = allegro_snd_tone_freq,
        .snd_tone_id   = allegro_snd_tone_id,
        .snd_play      = allegro_snd_play,
        .snd_music     = allegro_snd_music,
        .snd_mixer     = allegro_snd_mixer,
};
