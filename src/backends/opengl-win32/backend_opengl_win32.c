/*
 * backend_opengl_win32.c
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

/**
 * @file backend_opengl_win32.c
 * @brief Relic engine backend for Windows (32-bit and 64-bit) using OpenGL
 *        and WinMM.
 *
 * Provides windowing and an OpenGL rendering context through the Win32 and
 * WGL APIs, audio output through the WinMM waveOut and midiOut APIs, and
 * TrueType text rendering through stb_truetype. The primary toolchain is
 * MinGW/GCC across x86, x64 and Windows 95; Clang on Windows is also
 * supported. The OpenGL 1.1 code paths keep the backend compatible with
 * software and legacy GL implementations.
 *
 * Supported backend APIs and their engines:
 *
 *   Graphics 2D    OpenGL fixed-function immediate mode: clear, EGA @char
 *                  text, sprites, plot, lines, polygons and polylines.
 *   Graphics 3D    OpenGL fixed-function camera, textures, lighting and
 *                  meshes, with power-of-two padding to support OpenGL 1.1
 *                  non-power-of-two textures.
 *   Text           stb_truetype glyph baking into per-face atlases.
 *   Input          Win32 message-pump keyboard and mouse state.
 *   System         RNG, millisecond timer, console output, file and asset
 *                  I/O, and a platform-info query.
 *   Sound effects  @tone and @play: a software voice mixer running over the
 *                  WinMM waveOut device.
 *   Music          @music selects one of three engines:
 *                    - an OPL2 FM synthesiser for MUS lumps;
 *                    - a GENMIDI FM synthesiser that drives the OPL2 core
 *                      from MIDI files using the embedded Freedoom bank;
 *                    - the native Windows General MIDI synthesiser via
 *                      midiOut, with MUS-to-MIDI conversion where needed.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <wininet.h>
#if defined(_MSC_VER)
#pragma comment(lib, "wininet.lib")
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <GL/gl.h>

#ifdef TIGRYX_EXPERIMENTAL_SERVER
/* pulls winsock2.h - safe here: WIN32_LEAN_AND_MEAN is set above, so
 * windows.h never pulled in the legacy winsock.h */
#include "../common/nsnf_server.h"
#endif

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif

#ifndef GL_LINE_SMOOTH
#define GL_LINE_SMOOTH 0x0B20
#endif
#ifndef GL_POLYGON_SMOOTH
#define GL_POLYGON_SMOOTH 0x0B41
#endif
#ifndef GL_LINE_SMOOTH_HINT
#define GL_LINE_SMOOTH_HINT 0x0C52
#endif
#ifndef GL_POLYGON_SMOOTH_HINT
#define GL_POLYGON_SMOOTH_HINT 0x0C53
#endif

/* WGL_ARB_pixel_format / WGL_ARB_multisample constants used to request an
 * MSAA OpenGL context on Windows. */
#ifndef WGL_SAMPLE_BUFFERS_ARB
#define WGL_SAMPLE_BUFFERS_ARB 0x2041
#endif
#ifndef WGL_SAMPLES_ARB
#define WGL_SAMPLES_ARB 0x2042
#endif
#ifndef WGL_DRAW_TO_WINDOW_ARB
#define WGL_DRAW_TO_WINDOW_ARB 0x2001
#endif
#ifndef WGL_SUPPORT_OPENGL_ARB
#define WGL_SUPPORT_OPENGL_ARB 0x2010
#endif
#ifndef WGL_DOUBLE_BUFFER_ARB
#define WGL_DOUBLE_BUFFER_ARB 0x2011
#endif
#ifndef WGL_PIXEL_TYPE_ARB
#define WGL_PIXEL_TYPE_ARB 0x2013
#endif
#ifndef WGL_COLOR_BITS_ARB
#define WGL_COLOR_BITS_ARB 0x2014
#endif
#ifndef WGL_DEPTH_BITS_ARB
#define WGL_DEPTH_BITS_ARB 0x2022
#endif
#ifndef WGL_STENCIL_BITS_ARB
#define WGL_STENCIL_BITS_ARB 0x2023
#endif
#ifndef WGL_TYPE_RGBA_ARB
#define WGL_TYPE_RGBA_ARB 0x202B
#endif
#include "tigryx.h"
#include "tigryx_win_softcursor.h"
#include "../syswin32/syswin32.h"

#ifndef RELIC_BACKEND_ID
#define RELIC_BACKEND_ID "opengl-win32"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>

/* Sector-based world renderer (@wr_*): on-disk WAD map structs and
 * texture/flat slot counts, shared with every other backend that
 * supports @wr_*. See src/backends/common/wr/wr_render.h (included
 * further down, once OpenGLState is fully defined) for the renderer
 * itself. */
#include "../common/wr/wr_types.h"
#include "../common/drawlist/drawlist.h"
#include "../common/lines3d.h"
#include "../common/sdfill.h"
#include "../common/sdtrue.h"
#include "../common/fbdump.h"
#include "../common/musdec.h"

/* ---- XInput (dynamically loaded) ---- */
/* We define the types locally and load xinput1_4.dll / xinput9_1_0.dll at
 * runtime so there is no link-time dependency. This works on MinGW (which
 * may not ship xinput.lib) and on old Windows where the DLL doesn't exist. */

#define RELIC_XINPUT_MAX_USERS 4

typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} RELIC_XINPUT_GAMEPAD;

typedef struct {
    DWORD dwPacketNumber;
    RELIC_XINPUT_GAMEPAD Gamepad;
} RELIC_XINPUT_STATE;

#define RELIC_XINPUT_GAMEPAD_DPAD_UP         0x0001
#define RELIC_XINPUT_GAMEPAD_DPAD_DOWN       0x0002
#define RELIC_XINPUT_GAMEPAD_DPAD_LEFT       0x0004
#define RELIC_XINPUT_GAMEPAD_DPAD_RIGHT      0x0008
#define RELIC_XINPUT_GAMEPAD_START           0x0010
#define RELIC_XINPUT_GAMEPAD_BACK            0x0020
#define RELIC_XINPUT_GAMEPAD_LEFT_THUMB      0x0040
#define RELIC_XINPUT_GAMEPAD_RIGHT_THUMB     0x0080
#define RELIC_XINPUT_GAMEPAD_LEFT_SHOULDER   0x0100
#define RELIC_XINPUT_GAMEPAD_RIGHT_SHOULDER  0x0200
#define RELIC_XINPUT_GAMEPAD_A               0x1000
#define RELIC_XINPUT_GAMEPAD_B               0x2000
#define RELIC_XINPUT_GAMEPAD_X               0x4000
#define RELIC_XINPUT_GAMEPAD_Y               0x8000

typedef DWORD (WINAPI *RELIC_XINPUT_GET_STATE)(DWORD dwUserIndex, RELIC_XINPUT_STATE *pState);

static RELIC_XINPUT_GET_STATE g_xinput_get_state = NULL;
static HMODULE g_xinput_dll = NULL;
static bool g_xinput_tried = false;

static void opengl_xinput_load(void) {
    if (g_xinput_tried) return;
    g_xinput_tried = true;
    /* Try xinput1_4.dll first (Windows 8+), then xinput9_1_0.dll (Windows 7+). */
    g_xinput_dll = LoadLibraryA("xinput1_4.dll");
    if (!g_xinput_dll) g_xinput_dll = LoadLibraryA("xinput9_1_0.dll");
    if (!g_xinput_dll) return;
    g_xinput_get_state = (RELIC_XINPUT_GET_STATE)
        GetProcAddress(g_xinput_dll, "XInputGetState");
    if (!g_xinput_get_state) {
        FreeLibrary(g_xinput_dll);
        g_xinput_dll = NULL;
    }
}

/* Map XInput button bitmask to Relic's gamepad button bitmask.
 * Relic layout:
 *   bit 0=A, 1=B, 2=X, 3=Y, 4=LB, 5=RB, 6=Back, 7=Start,
 *   bit 8=L3, 9=R3, 10=Up, 11=Down, 12=Left, 13=Right */
static i32 opengl_xinput_map_buttons(WORD xb) {
    i32 b = 0;
    if (xb & RELIC_XINPUT_GAMEPAD_A)              b |= (1 << 0);
    if (xb & RELIC_XINPUT_GAMEPAD_B)              b |= (1 << 1);
    if (xb & RELIC_XINPUT_GAMEPAD_X)              b |= (1 << 2);
    if (xb & RELIC_XINPUT_GAMEPAD_Y)              b |= (1 << 3);
    if (xb & RELIC_XINPUT_GAMEPAD_LEFT_SHOULDER)  b |= (1 << 4);
    if (xb & RELIC_XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= (1 << 5);
    if (xb & RELIC_XINPUT_GAMEPAD_BACK)           b |= (1 << 6);
    if (xb & RELIC_XINPUT_GAMEPAD_START)          b |= (1 << 7);
    if (xb & RELIC_XINPUT_GAMEPAD_LEFT_THUMB)     b |= (1 << 8);
    if (xb & RELIC_XINPUT_GAMEPAD_RIGHT_THUMB)    b |= (1 << 9);
    if (xb & RELIC_XINPUT_GAMEPAD_DPAD_UP)        b |= (1 << 10);
    if (xb & RELIC_XINPUT_GAMEPAD_DPAD_DOWN)      b |= (1 << 11);
    if (xb & RELIC_XINPUT_GAMEPAD_DPAD_LEFT)      b |= (1 << 12);
    if (xb & RELIC_XINPUT_GAMEPAD_DPAD_RIGHT)     b |= (1 << 13);
    return b;
}

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#ifndef MAX_PATH
#define MAX_PATH 1024
#endif
#define WIN_W  640
#define WIN_H  480
#define FONT_MAX_FACES 32

/* Texture slot table sizing. The table grows on demand (see
 * opengl_tex_reserve) instead of being a fixed array, so a small program
 * pays for 256 slots (~5 KB) while a WAD-scale one can hold thousands.
 * A fixed 512 used to be the hard limit, and because @texture has no way
 * to report "table full" beyond returning 0 - which every caller treats
 * as "use the fallback texture" - overflowing it showed up as a scene
 * that silently rendered untextured rather than as any kind of error.
 *
 * RELIC_MAX_TEXTURES is now the ceiling rather than the allocation:
 * @texture yields a u16 slot id, so 65535 is the highest addressable. */
#define RELIC_TEX_CAP_INIT 256
#define RELIC_MAX_TEXTURES 65535

#define RELIC_MAX_PROC 8      /* max concurrent spawned subprocess handles */

#define RELIC_WINFLAG_STRETCH     0x00000001u
#define RELIC_WINFLAG_NO_VSYNC    0x00000004u
#define RELIC_WINFLAG_NORESIZE    0x00000010u
#define RELIC_WINFLAG_CENTER      0x00000020u
#define RELIC_WINFLAG_FULLCONTENT 0x00000040u
#define RELIC_WINFLAG_NODPI       0x00000080u
#define RELIC_WINFLAG_AUTO_TITLEBAR 0x00000800u
#define RELIC_WINFLAG_DARK_TITLEBAR 0x00004000u
#define RELIC_WINFLAG_CLOSE_REQUEST 0x00001000u
#define RELIC_WINFLAG_FULLSCREEN  0x00002000u

/* Window-drag strip along the top edge of a RELIC_WINFLAG_FULLCONTENT window.
 * Pressing inside it reports HTCAPTION so the window moves, which means the
 * script never sees those clicks. The trailing RELIC_BUTTONS_W pixels are
 * excluded from the strip and passed through as HTCLIENT: with the system
 * caption buttons gone, that is where the script draws its own close /
 * minimise controls, and they have to be clickable. Matches WINDOW_DRAG_H
 * and WINDOW_BUTTONS_W in artifact/system/flags.tgx. */
#define RELIC_DRAG_STRIP_H 28
#define RELIC_BUTTONS_W    78

/* Subprocess slot for @process_spawn/poll/read/kill/close.
 * stdout_pipe_r / stderr_pipe_r are the read ends (owned by us);
 * the write ends are given to the child and closed by us after CreateProcess.
 * We buffer whatever's available from the pipes each poll; ReadFile is
 * non-blocking because we PeekNamedPipe first to get avail bytes. */
typedef struct {
    bool       in_use;
    HANDLE     hProcess;       /* child process handle */
    HANDLE     hThread;        /* main thread handle (closed after create) */
    HANDLE     stdout_pipe_r;  /* our read end of stdout pipe */
    HANDLE     stderr_pipe_r;  /* our read end of stderr pipe */
    int        exit_code;      /* valid once exited == 1 */
    bool       exited;
} ProcSlot;

/**
 * @brief Per-VM backend state: window, GL context, input, fonts, textures
 *        and frame-presentation bookkeeping.
 *
 * One instance is allocated per VM and stored in vm->backend_data. The
 * global g_state mirrors the active instance so the window procedure can
 * reach it. Text is rasterised through GDI (CreateFontA) at draw time, not
 * a baked atlas; font faces are only discovered (path + name) at startup
 * and are not opened, parsed, or registered with GDI until a script first
 * asks to draw with one (see discover_font_face / ensure_face_registered).
 * The stretch fields implement the fixed logical resolution letterbox mode
 * requested by RELIC_WINFLAG_STRETCH.
 */

/* @text rasterisation cache. The GDI path rasterises each string into a
 * one-off texture; caching that texture turns a repeat draw of the same
 * string at the same size into a textured quad instead of a
 * CreateFont/CreateDIBSection/TextOut/glTexImage2D round trip (~100us per
 * call, which dominates text-heavy frames - a schematic redraws thousands
 * of identical labels). Keyed only on what changes the bitmap - face,
 * device-pixel size, ClearType mode and the string; position, colour and
 * justification are applied at draw time. */
#define TEXT_CACHE_SIZE  2048   /* power of two */
#define TEXT_CACHE_PROBE 32     /* open-addressing probe window */

typedef struct {
    u32    hash;                /* 0 = empty slot */
    char  *text;                /* owned copy of the key string */
    int    face_idx;
    int    ps_dev;              /* device-pixel size (folds DPI scale) */
    int    use_ct;              /* ClearType RGB texture vs alpha texture */
    GLuint tex;
    int    dib_w, tex_h;        /* ink box, device pixels */
    int    ink_x, ink_y;        /* ink offset inside the font cell */
    int    cell_w, cell_h;      /* font cell dims (advance, line height) */
    float  u_max, v_max;        /* used sub-rect when PoT-padded */
    u32    last_used;           /* LRU clock */
} TextCacheEntry;

typedef struct {
    HWND          hwnd;
    HDC           hdc;
    HGLRC         hglrc;
    int           width;          /* logical size the script draws into */
    int           height;
    int           pixel_w;        /* device (physical) size for glViewport */
    int           pixel_h;
    unsigned long fbdump_n;       /* $TGXDUMP frame counter (common/fbdump.h) */
    GLuint        sdf_tex;        /* BBC-exact fill view (common/sdfill.h) */
    float         backing_scale;  /* device pixels per logical pixel */
    int           dpi;            /* effective DPI of the current monitor */
    bool          dpi_scaling;    /* false => legacy 1:1 pixels (win95, NODPI) */
    int           reported_width;
    int           reported_height;
    u8            key_queue[32]; /* FIFO of WM_CHAR codes for @key_pressed;
                                  * a bare last_key slot loses characters
                                  * typed faster than the guest polls */
    int           key_qh;
    int           key_qt;
    bool          keys[256];
    i32           mouse_x;
    i32           mouse_y;
    i32           mouse_buttons;
    i32           mouse_wheel;
    volatile bool quit;
    bool close_requested;  /* WINFLAG_CLOSE_REQUEST: WM_CLOSE sets this; @key_held(27) reads+clears it (one-shot) */
    bool close_pending;    /* WINFLAG_CLOSE_REQUEST: dedicated channel, @key_held(0) reads+clears it */
    bool close_chan0;      /* the script has polled @key_held(0): legacy 27 signal disabled */
    struct {
        char             name[64];
        /* The typeface name GDI actually knows this font by, read from the
         * font's own `name` table. CreateFontA matches on this, so guessing
         * it from the filename silently yields a substituted font. Left
         * empty until ensure_face_registered() runs. */
        char             family[LF_FACESIZE];
        char             path[MAX_PATH];
        bool             is_bold;
        bool             is_system;    /* system font (no file; GDI knows it natively) */
        bool             init;         /* slot discovered (path/name valid) */
        bool             registered;   /* ensure_face_registered() has run */
        bool             font_res_added;   /* registered via AddFontResourceExA */
        HANDLE           font_mem_handle;
    } faces[FONT_MAX_FACES];
    int           nfaces;
    int           default_face;
    TextCacheEntry *text_cache;     /* lazily allocated TEXT_CACHE_SIZE slots */
    u32           text_cache_tick;
    bool          in_3d;
    bool          light_on;
    /* Grown on demand by opengl_tex_reserve; tex_cap is the number of
     * slots currently allocated. Slot 0 is reserved as "no texture". */
    GLuint       *textures;
    int          *tex_width;
    int          *tex_height;
    int           ntextures;
    float        *tex_u_scale;
    float        *tex_v_scale;
    int           tex_cap;
    int           gl_version;
    /* GL_RENDERER == "GDI Generic": Microsoft's software rasteriser, the
     * only OpenGL on Win9x-era systems without an ICD. Its vertex-array
     * path crashes inside glDrawArrays/glDrawElements (the triangle-setup
     * proc dereferences a per-vertex attribute pointer the array fetch
     * leaves NULL), so @mesh must use glBegin/glEnd there instead. */
    bool          gl_ms_generic;
    bool          frame_dirty;
    u32           win_flags;
    bool          stretch_enabled;
    bool          vsync_throttle;
    DWORD         last_flip_ms;
    bool          soft_cursor, cursor_was_on;
    int           cursor_x, cursor_y;
    int           logical_w;
    int           logical_h;
    int           vp_x, vp_y, vp_w, vp_h;
    GLuint        stretch_tex;
    int           stretch_pot_w, stretch_pot_h;
    float         stretch_uscale, stretch_vscale;
    int           vp_3d_x, vp_3d_y, vp_3d_w, vp_3d_h;
    /* @view(x,y,w,h): logical render-surface rect. While set, the 3D
     * projection maps into it (glViewport) instead of the full window -
     * used by embedding hosts (TIDE) to place a guest's @window-sized
     * scene inside a panel. All zeros = full window (default). */
    int           view_x, view_y, view_w, view_h;
    /* Retained guest frame: the view rect is snapshotted into view_tex
     * by @view_capture (called at the guest's @flip) and drawn scaled by
     * @view_present, so the host composites a completed frame rather
     * than the guest's mid-frame draws. */
    GLuint        view_tex;
    int           view_tex_w, view_tex_h;   /* POT texture dims */
    float         view_uscale, view_vscale; /* content fraction of tex */
    bool          view_has_frame;
    /* Panel pixel rect (set by @view_present, used by wr_host_present
     * to scale the 8bpp wr_surf to fill the panel on the next frame).
     * Zero on the first frame, so wr_host_present falls back to 1:1. */
    int           present_dst_x, present_dst_y, present_dst_w, present_dst_h;
    char          temp_font_paths[FONT_MAX_FACES][MAX_PATH];
    int           ntemp_fonts;

    /* Web / HTTP request slots (see docs/web.md). */
    SysWin32HttpReq http[SYSWIN32_MAX_HTTP];

    /* Subprocess slots for @process_spawn/poll/read/kill/close. */
    ProcSlot      proc[RELIC_MAX_PROC];

    /* ---- Sector-based world renderer (@wr_*) ----
     * See src/backends/common/wr/wr_render.h for the renderer itself;
     * these are the WrHostState fields it requires, plus (after them)
     * the GL-specific scratch surface / palette LUT / texture used to
     * present the 8bpp result (see wr_host_present below). Field names
     * and layout mirror AllegroState's wr_* fields exactly, since
     * wr_render.h is shared verbatim between backends. */
    bool          wr_palette_set;
    unsigned char wr_playpal[768];
    unsigned char *wr_colormap;
    int           wr_num_colormaps;
    struct {
        unsigned char *data;
        int w, h;
    } wr_textures[WR_MAX_TEXTURES];
    unsigned char *wr_flats[WR_MAX_FLATS];
    bool          wr_map_set;
    WrVertex      *wr_vertices;   int wr_n_vertices;
    WrLinedef     *wr_linedefs;   int wr_n_linedefs;
    WrSidedef     *wr_sidedefs;   int wr_n_sidedefs;
    WrSector      *wr_sectors;    int wr_n_sectors;
    WrSeg         *wr_segs;       int wr_n_segs;
    WrSubsector   *wr_subsectors; int wr_n_subsectors;
    WrNode        *wr_nodes;      int wr_n_nodes;
    i32           *wr_sd_tex;
    i32           *wr_sec_tex;
    bool          wr_texmap_set;
    i32           wr_sky_tex_id;
    bool          wr_flatshade;
    unsigned char wr_tex_avg_color[WR_MAX_TEXTURES];
    bool          wr_tex_avg_valid[WR_MAX_TEXTURES];
    unsigned char wr_flat_avg_color[WR_MAX_FLATS];
    bool          wr_flat_avg_valid[WR_MAX_FLATS];
    int           wr_area_x, wr_area_y, wr_area_w, wr_area_h;

    /* GL-only presentation state: a private 8bpp scratch surface the
     * shared renderer draws into (sized to the window's logical width/
     * height - see wr_host_present), a 256-entry RGBA lookup table
     * rebuilt whenever @wr_palette runs, and the GL texture used to
     * upload/redraw it. There is no persistent hardware framebuffer to
     * write raw palette-index bytes into on this backend, unlike DOS's
     * VGA/VRAM backbuffer. */
    unsigned char *wr_surf;
    int           wr_surf_w, wr_surf_h, wr_surf_pitch;
    u32           wr_rgba_lut[256];
    GLuint        wr_present_tex;
    int           wr_present_pot_w, wr_present_pot_h;

    /* ---- embedded guest slice (gfx_guest_begin/end) ----
     * gst_on != 0 while a guest VM's run slice is executing on this
     * shared context. The guest's logical surface [0,lw)x[0,lh) maps
     * onto content rect (x,y,w,h) in host logical px (top-left origin)
     * via a modelview translate+scale; scissor clamps to the same rect
     * so guest drawing can never escape its window. sav_* hold the
     * enclosing (host) render state restored by guest_end - one level
     * deep since guest slices never nest on one context. */
    int           gst_on;
    int           gst_x, gst_y, gst_w, gst_h;
    int           gst_lw, gst_lh;
    int           sav_in_3d, sav_light_on;
    int           sav_view_x, sav_view_y, sav_view_w, sav_view_h;
    int           sav_vp_x, sav_vp_y, sav_vp_w, sav_vp_h;
} OpenGLState;

static OpenGLState *g_state = NULL;
static RelicVM     *g_vm    = NULL;

/* glBlendColor (GL 1.4) is fetched once a context is current. A NULL pointer
 * disables the ClearType text path, which needs CONSTANT_COLOR blending to
 * composite per-subpixel coverage; text then keeps plain antialiasing. */
typedef void (WINAPI *PFN_glBlendColor_t)(GLclampf, GLclampf, GLclampf, GLclampf);
static PFN_glBlendColor_t g_glBlendColor = NULL;
#ifndef GL_CONSTANT_COLOR
#define GL_CONSTANT_COLOR 0x8001   /* GL 1.4 blend factor */
#endif

/* FR_PRIVATE is gated behind _WIN32_WINNT >= 0x0500 in wingdi.h; the win95
 * builds pin it to 0x0400, so define it here rather than raising the target.
 * CLEARTYPE_QUALITY (XP, 0x0501) is the same story — guarded the same way. */
#ifndef FR_PRIVATE
#define FR_PRIVATE 0x10
#endif
#ifndef CLEARTYPE_QUALITY
#define CLEARTYPE_QUALITY 5
#endif

typedef HANDLE (WINAPI *PFN_AddFontMemResourceEx)(PVOID, DWORD, PVOID, DWORD*);
typedef BOOL (WINAPI *PFN_RemoveFontMemResourceEx)(HANDLE);
typedef int (WINAPI *PFN_AddFontResourceExA)(LPCSTR, DWORD, PVOID);
typedef BOOL (WINAPI *PFN_RemoveFontResourceExA)(LPCSTR, DWORD, PVOID);
static PFN_AddFontMemResourceEx    g_AddFontMemResourceEx    = NULL;
static PFN_RemoveFontMemResourceEx g_RemoveFontMemResourceEx = NULL;
static PFN_AddFontResourceExA      g_AddFontResourceExA      = NULL;
static PFN_RemoveFontResourceExA   g_RemoveFontResourceExA   = NULL;
static bool g_font_mem_funcs_checked = false;

static void init_font_mem_funcs(void) {
    if (g_font_mem_funcs_checked) return;
    g_font_mem_funcs_checked = true;
    /* LoadLibraryA, not GetModuleHandleA: the latter returns NULL when gdi32
     * has not already been pulled into the process, which silently demoted us
     * to the Win9x AddFontResourceA path. gdi32 is never unloaded, so the
     * reference is deliberately left outstanding. */
    HMODULE gdi32 = LoadLibraryA("gdi32.dll");
    if (gdi32) {
        g_AddFontMemResourceEx    = (PFN_AddFontMemResourceEx)GetProcAddress(gdi32, "AddFontMemResourceEx");
        g_RemoveFontMemResourceEx = (PFN_RemoveFontMemResourceEx)GetProcAddress(gdi32, "RemoveFontMemResourceEx");
        g_AddFontResourceExA      = (PFN_AddFontResourceExA)GetProcAddress(gdi32, "AddFontResourceExA");
        g_RemoveFontResourceExA   = (PFN_RemoveFontResourceExA)GetProcAddress(gdi32, "RemoveFontResourceExA");
    }
}

/* ---------------------------------------------------------------------------
 * Per-monitor DPI scaling.
 *
 * Scripts work entirely in logical pixels: @window's w/h and every coordinate
 * passed to a drawing builtin are logical, and the backend multiplies them by
 * the monitor's scale factor (dpi / 96) to get device pixels. glViewport is
 * sized in device pixels while glOrtho keeps the logical extents, so the whole
 * 2D and 3D pipeline scales for free and existing scripts need no changes.
 * Only the few places that speak device pixels directly - GDI glyph
 * rasterisation, glScissor and WM_NCHITTEST - apply the factor by hand. This
 * mirrors the Retina handling in the macOS backend, which is why the state
 * already carries pixel_w/pixel_h and backing_scale.
 *
 * Every entry point below is resolved with GetProcAddress. This same file
 * builds the win95 target at WINVER 0x0400, where none of them exist: there
 * the process stays DPI-unaware, the DPI stays 96 and backing_scale stays
 * 1.0, so win95 renders exactly as it did before. RELIC_WINFLAG_NODPI opts a
 * script out on modern Windows and takes the same 1.0 path.
 */
#define RELIC_DEFAULT_DPI 96

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

/* DPI_AWARENESS_CONTEXT is an opaque handle whose per-monitor-v2 value is the
 * constant -4; PROCESS_PER_MONITOR_DPI_AWARE is 2 and MDT_EFFECTIVE_DPI is 0.
 * Spelled out here rather than taken from headers the win95 target predates. */
#define RELIC_DPI_CTX_PER_MONITOR_V2        ((HANDLE)(LONG_PTR)-4)
#define RELIC_PROCESS_PER_MONITOR_DPI_AWARE 2
#define RELIC_MDT_EFFECTIVE_DPI             0
#define RELIC_MONITOR_DEFAULTTONEAREST      2

/* HMONITOR is gated behind WINVER >= 0x0500, so monitor handles are carried
 * as void* to keep the win95 build compiling. */
typedef BOOL     (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef HRESULT  (WINAPI *PFN_SetProcessDpiAwareness)(int);
typedef BOOL     (WINAPI *PFN_SetProcessDPIAware)(void);
typedef UINT     (WINAPI *PFN_GetDpiForWindow)(HWND);
typedef UINT     (WINAPI *PFN_GetDpiForSystem)(void);
typedef HRESULT  (WINAPI *PFN_GetDpiForMonitor)(void*, int, UINT*, UINT*);
typedef void*    (WINAPI *PFN_MonitorFromPoint)(POINT, DWORD);
typedef BOOL     (WINAPI *PFN_AdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);

/* MONITORINFO is gated behind WINVER >= 0x0500 like HMONITOR, so a local
 * copy with the same layout keeps the win95 build compiling. */
typedef struct {
    DWORD cbSize;
    RECT  rcMonitor;
    RECT  rcWork;
    DWORD dwFlags;
} RelicMonitorInfo;
typedef BOOL     (WINAPI *PFN_GetMonitorInfo)(void*, RelicMonitorInfo*);

static PFN_SetProcessDpiAwarenessContext g_SetProcessDpiAwarenessContext = NULL;
static PFN_SetProcessDpiAwareness        g_SetProcessDpiAwareness        = NULL;
static PFN_SetProcessDPIAware            g_SetProcessDPIAware            = NULL;
static PFN_GetDpiForWindow               g_GetDpiForWindow               = NULL;
static PFN_GetDpiForSystem               g_GetDpiForSystem               = NULL;
static PFN_GetDpiForMonitor              g_GetDpiForMonitor              = NULL;
static PFN_MonitorFromPoint              g_MonitorFromPoint              = NULL;
static PFN_GetMonitorInfo                g_GetMonitorInfo                = NULL;
static PFN_AdjustWindowRectExForDpi      g_AdjustWindowRectExForDpi      = NULL;
static bool g_dpi_funcs_checked = false;

static void init_dpi_funcs(void) {
    if (g_dpi_funcs_checked) return;
    g_dpi_funcs_checked = true;
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        g_SetProcessDpiAwarenessContext = (PFN_SetProcessDpiAwarenessContext)
            GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        g_SetProcessDPIAware = (PFN_SetProcessDPIAware)
            GetProcAddress(user32, "SetProcessDPIAware");
        g_GetDpiForWindow = (PFN_GetDpiForWindow)
            GetProcAddress(user32, "GetDpiForWindow");
        g_GetDpiForSystem = (PFN_GetDpiForSystem)
            GetProcAddress(user32, "GetDpiForSystem");
        g_MonitorFromPoint = (PFN_MonitorFromPoint)
            GetProcAddress(user32, "MonitorFromPoint");
        g_GetMonitorInfo = (PFN_GetMonitorInfo)
            GetProcAddress(user32, "GetMonitorInfoA");
        g_AdjustWindowRectExForDpi = (PFN_AdjustWindowRectExForDpi)
            GetProcAddress(user32, "AdjustWindowRectExForDpi");
    }
    /* shcore.dll is the Windows 8.1 home of the per-monitor APIs and is not
     * loaded by default, so LoadLibrary rather than GetModuleHandle. It is
     * never unloaded, so the reference is deliberately left outstanding. */
    HMODULE shcore = LoadLibraryA("shcore.dll");
    if (shcore) {
        g_SetProcessDpiAwareness = (PFN_SetProcessDpiAwareness)
            GetProcAddress(shcore, "SetProcessDpiAwareness");
        g_GetDpiForMonitor = (PFN_GetDpiForMonitor)
            GetProcAddress(shcore, "GetDpiForMonitor");
    }
}

/**
 * @brief Requests the strongest DPI awareness this Windows version offers.
 *
 * Per-monitor v2 (Windows 10 1703+) is preferred: it stops Windows
 * bitmap-stretching the window when it moves to a display with a different
 * scale factor, delivers WM_DPICHANGED instead, and scales the non-client
 * frame for us. Falls back to per-monitor v1 (8.1) then to system-wide
 * awareness (Vista).
 *
 * @return true when the process ended up per-monitor aware, i.e. when
 *         WM_DPICHANGED will be delivered and no OS stretching happens.
 */
static bool opengl_dpi_set_awareness(void) {
    init_dpi_funcs();
    if (g_SetProcessDpiAwarenessContext &&
        g_SetProcessDpiAwarenessContext(RELIC_DPI_CTX_PER_MONITOR_V2)) return true;
    if (g_SetProcessDpiAwareness &&
        g_SetProcessDpiAwareness(RELIC_PROCESS_PER_MONITOR_DPI_AWARE) >= 0) return true;
    if (g_SetProcessDPIAware) g_SetProcessDPIAware();
    return false;
}

/**
 * @brief Effective DPI of the monitor containing a screen point.
 *
 * Used before the window exists, so the initial client size can be scaled for
 * the display the window is about to appear on. Falls back to the system DPI
 * and then to the desktop DC, and finally to 96 (= no scaling).
 */
static UINT opengl_dpi_at_point(int x, int y) {
    init_dpi_funcs();
    if (g_GetDpiForMonitor && g_MonitorFromPoint) {
        POINT pt;
        pt.x = (LONG)x;
        pt.y = (LONG)y;
        void *mon = g_MonitorFromPoint(pt, RELIC_MONITOR_DEFAULTTONEAREST);
        UINT dx = 0, dy = 0;
        if (mon && g_GetDpiForMonitor(mon, RELIC_MDT_EFFECTIVE_DPI, &dx, &dy) >= 0 && dx)
            return dx;
    }
    if (g_GetDpiForSystem) {
        UINT d = g_GetDpiForSystem();
        if (d) return d;
    }
    {
        HDC dc = GetDC(NULL);
        if (dc) {
            int d = GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC(NULL, dc);
            if (d > 0) return (UINT)d;
        }
    }
    return RELIC_DEFAULT_DPI;
}

/**
 * @brief Effective DPI of the monitor a window is on, or 96 when unknown.
 */
static UINT opengl_dpi_for_window(HWND hwnd) {
    init_dpi_funcs();
    if (hwnd && g_GetDpiForWindow) {
        UINT d = g_GetDpiForWindow(hwnd);
        if (d) return d;
    }
    if (hwnd) {
        RECT rc;
        if (GetWindowRect(hwnd, &rc))
            return opengl_dpi_at_point((int)rc.left, (int)rc.top);
    }
    return opengl_dpi_at_point(0, 0);
}

/**
 * @brief Bounds of the monitor containing a screen point, in device pixels.
 *
 * MonitorFromPoint/GetMonitorInfo are Windows 98/2000 additions, resolved
 * dynamically like the DPI functions; on win95 they are absent and the
 * primary screen metrics stand in for a single-monitor machine. rcMonitor
 * (not rcWork) is returned on purpose: fullscreen wants the whole panel,
 * taskbar area included.
 */
static void opengl_monitor_rect(int x, int y, RECT *out) {
    init_dpi_funcs();
    if (g_MonitorFromPoint && g_GetMonitorInfo) {
        POINT pt;
        pt.x = (LONG)x;
        pt.y = (LONG)y;
        void *mon = g_MonitorFromPoint(pt, RELIC_MONITOR_DEFAULTTONEAREST);
        RelicMonitorInfo mi;
        mi.cbSize = sizeof(mi);
        if (mon && g_GetMonitorInfo(mon, &mi)) {
            *out = mi.rcMonitor;
            return;
        }
    }
    out->left   = 0;
    out->top    = 0;
    out->right  = GetSystemMetrics(SM_CXSCREEN);
    out->bottom = GetSystemMetrics(SM_CYSCREEN);
}

/* DWM (Desktop Window Manager) ships on Vista+ but not on Win95/98/ME, so
 * dwmapi.dll is loaded dynamically like shcore.dll above. The corner
 * preference attribute is Windows 11 only; on older versions the call
 * fails harmlessly and the window keeps its (square) default corners. */
#define RELIC_DWMWA_WINDOW_CORNER_PREFERENCE 33
#define RELIC_DWMWCP_ROUND                   2

typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
static PFN_DwmSetWindowAttribute g_DwmSetWindowAttribute = NULL;
static bool g_dwm_funcs_checked = false;

static void init_dwm_funcs(void) {
    if (g_dwm_funcs_checked) return;
    g_dwm_funcs_checked = true;
    HMODULE dwmapi = LoadLibraryA("dwmapi.dll");
    if (dwmapi) {
        g_DwmSetWindowAttribute = (PFN_DwmSetWindowAttribute)
            GetProcAddress(dwmapi, "DwmSetWindowAttribute");
    }
}

/**
 * @brief Opts the window into Windows 11's rounded corners.
 *
 * Windows 11 decides corner shape from heuristics about the window's chrome,
 * and our windows -- particularly WS_POPUP FULLCONTENT windows that draw
 * their own title bar, but also standard captioned windows under some
 * per-monitor DPI configurations -- can fall into the "square" bucket and
 * end up with corners that do not match the rest of the OS. Explicitly
 * requesting DWMWCP_ROUND makes the DWM draw the standard 8px corners.
 *
 * Called right after CreateWindowExA so the first visible frame is already
 * rounded. No-op on Windows 10 and earlier (the attribute is unknown there)
 * and on Win95 (dwmapi.dll does not exist, so the pointer stays NULL). */
static void opengl_apply_rounded_corners(HWND hwnd) {
    init_dwm_funcs();
    if (g_DwmSetWindowAttribute) {
        int pref = RELIC_DWMWCP_ROUND;
        g_DwmSetWindowAttribute(hwnd, RELIC_DWMWA_WINDOW_CORNER_PREFERENCE,
                                &pref, sizeof(pref));
    }
}

/* DWMWA_USE_IMMERSIVE_DARK_MODE opts a window's title bar and border into
 * the dark chrome Windows itself uses for dark-themed apps. The attribute
 * number changed between the Windows 10 1809 preview (19, withdrawn before
 * general availability) and the 20H1 release (20, the documented value
 * ever since); setting both is the common compatibility trick other
 * applications use, since the wrong one is simply rejected as an unknown
 * attribute and ignored. */
#define RELIC_DWMWA_USE_IMMERSIVE_DARK_MODE     20
#define RELIC_DWMWA_USE_IMMERSIVE_DARK_MODE_1809 19

/**
 * @brief Sets the window's title bar/border to follow the OS light/dark app
 *        theme (Settings > Personalization > Colors > Choose your mode),
 *        read once from the registry at window-creation time.
 *
 * "AppsUseLightTheme" is a per-user DWORD; 0 means the user has chosen dark
 * apps, 1 (or the key/value being absent, e.g. on Windows versions that
 * predate this setting) means light. No-op below Windows 10 1809 (the
 * attribute is unknown there, so the DWM call is rejected harmlessly) and
 * on Win95 (dwmapi.dll does not exist, so the pointer stays NULL). */
static void opengl_apply_system_titlebar_theme(HWND hwnd) {
    init_dwm_funcs();
    if (!g_DwmSetWindowAttribute) return;

    DWORD light = 1;
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD value = 0, size = sizeof(value);
        if (RegQueryValueExA(hKey, "AppsUseLightTheme", NULL, NULL,
                              (LPBYTE)&value, &size) == ERROR_SUCCESS) {
            light = value;
        }
        RegCloseKey(hKey);
    }

    BOOL use_dark = light ? FALSE : TRUE;
    g_DwmSetWindowAttribute(hwnd, RELIC_DWMWA_USE_IMMERSIVE_DARK_MODE,
                            &use_dark, sizeof(use_dark));
    g_DwmSetWindowAttribute(hwnd, RELIC_DWMWA_USE_IMMERSIVE_DARK_MODE_1809,
                            &use_dark, sizeof(use_dark));
}

/* RELIC_WINFLAG_DARK_TITLEBAR: the same immersive-dark attribute, but
 * forced on unconditionally - for apps whose chrome is always dark and
 * must not follow the OS theme. No-op below Windows 10 1809 and on
 * Win95 (dwmapi.dll absent). */
static void opengl_force_dark_titlebar(HWND hwnd) {
    init_dwm_funcs();
    if (!g_DwmSetWindowAttribute) return;
    BOOL use_dark = TRUE;
    g_DwmSetWindowAttribute(hwnd, RELIC_DWMWA_USE_IMMERSIVE_DARK_MODE,
                            &use_dark, sizeof(use_dark));
    g_DwmSetWindowAttribute(hwnd, RELIC_DWMWA_USE_IMMERSIVE_DARK_MODE_1809,
                            &use_dark, sizeof(use_dark));
}

static void opengl_shutdown(RelicVM *vm);
static void text_cache_flush(OpenGLState *s);
static void opengl_apply_clip(OpenGLState *s, i32 x, i32 y, i32 w, i32 h);
static void opengl_guest_2d(OpenGLState *s);

/* Defined by wr_render.h (included further down, once OpenGLState is
 * complete). Forward-declared here so opengl_shutdown can write the final
 * WRPROF.LOG - the periodic in-render flush is deliberately skipped when
 * --fps/NO_VSYNC is active, so without this a benchmark run would produce
 * no profile at all. Same arrangement as the DOS backend. */
static void wr_prof_flush(void);

/* Ctrl-C / close handler: ensures font resources are released even when the
 * process is interrupted. With AddFontMemResourceEx (Windows 2000+) font
 * resources are per-process and auto-cleaned by the OS on exit, so this
 * handler is not strictly needed to prevent font leaks. On Win9x where
 * AddFontMemResourceEx is unavailable, we fall back to AddFontResourceA and
 * this handler still helps clean up. */
static BOOL WINAPI opengl_ctrl_handler(DWORD ctrl_type) {
    if (g_vm && g_vm->backend_data) {
        opengl_shutdown(g_vm);
    }
    return FALSE;  /* let default handler terminate the process */
}

static void opengl_pump_events(OpenGLState *s);
static void opengl_sync_size(OpenGLState *s);
static void opengl_snd_init(void);
static void opengl_snd_shutdown(void);
static int  opengl_font_init(OpenGLState *s);
static void opengl_font_shutdown(OpenGLState *s);
static void opengl_proc_shutdown(OpenGLState *s);

/**
 * @brief Reinterprets the bits of an i32 register as an f32, or vice versa.
 *
 * Relic passes f32 arguments in i32 registers; this union performs the
 * bit-exact reinterpretation without a value-converting cast.
 */
typedef union { i32 i; float f; } RelicFBits;

/**
 * @brief Rounds an integer up to the next power of two.
 * @param n Input value.
 * @return The smallest power of two that is >= n, with a minimum of 1. Used
 *         to pad non-power-of-two textures for OpenGL 1.1 implementations.
 */
static int next_power_of_2(int n) {
    if (n <= 1) return 1;
    int power = 1;
    while (power < n) power <<= 1;
    return power;
}

/**
 * @brief Win32 window procedure: records keyboard, mouse and close events.
 */
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    OpenGLState *s = g_state;
    /* Ignore messages from temporary helper windows (e.g. the MSAA bootstrap
     * dummy window) so they don't mutate input state or set the quit flag. */
    if (s && hwnd != s->hwnd) return DefWindowProcA(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_CLOSE:
            /* WINFLAG_CLOSE_REQUEST: intercept the close button (X) so the
             * script can prompt for unsaved changes before the window is
             * destroyed. Without the flag, fall through to DefWindowProc
             * which destroys the window and kills the VM immediately. */
            if (s && (s->win_flags & RELIC_WINFLAG_CLOSE_REQUEST)) {
                /* Dedicated channel (@key_held(0)) always latches. The
                 * legacy fake-Escape only fires for scripts that never
                 * polled the dedicated channel - otherwise any key scan
                 * reading 27 would eat the request first. */
                s->close_pending = true;
                if (!s->close_chan0) s->close_requested = true;
                return 0;
            }
            return DefWindowProcA(hwnd, msg, wp, lp);
        case WM_DESTROY:
            if (s) s->quit = true;
            if (g_vm) g_vm->running = false;
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            /* WM_SYSKEYDOWN is sent for system keys: Alt, F10, and
             * Alt+key combos. Without handling it here, @key_held()
             * never sees Alt (VK_MENU=18) or F10 (VK_F10=0x79), so
             * a classic DOS FPS's Alt-strafe and F10-quit don't work.
             * We must return 0 (not pass to DefWindowProc) to prevent
             * the default system menu activation on F10/Alt. */
            if (s) {
                if (wp < 256) s->keys[wp] = true;
            }
            return 0;
        case WM_CHAR:
            /* TranslateMessage posts WM_CHAR from WM_KEYDOWN with the
             * actual translated character (lowercase if no shift/caps).
             * Queue it for @key_pressed so printable chars arrive in the
             * right case and none are lost when several land between two
             * polls. Non-printable keys (arrows, function keys) don't
             * generate WM_CHAR - callers that need them use @key_held with
             * the VK code. */
            if (s) {
                int n = (s->key_qt + 1) & 31;
                if (n != s->key_qh) {
                    s->key_queue[s->key_qt] = (u8)(wp & 0xFF);
                    s->key_qt = n;
                }
            }
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (s && wp < 256) s->keys[wp] = false;
            return 0;
        case WM_KILLFOCUS:
            /* A key released while another window had focus delivers its
             * WM_KEYUP to that window, not ours - leaving keys[] stuck true
             * (classic "player runs forward forever after alt-tab"). Drop
             * every tracked key and mouse button on the way out; held queries
             * re-latch naturally on the next WM_KEYDOWN after focus returns. */
            if (s) {
                memset(s->keys, 0, sizeof(s->keys));
                s->mouse_buttons = 0;
            }
            return 0;
        case WM_MOUSEMOVE:
            if (s) {
                int px = (int)(i16)LOWORD(lp);
                int py = (int)(i16)HIWORD(lp);
                if (s->stretch_enabled && s->vp_w > 0 && s->vp_h > 0) {
                    int lx = (px - s->vp_x) * s->logical_w / s->vp_w;
                    int ly = (py - s->vp_y) * s->logical_h / s->vp_h;
                    if (lx < 0) lx = 0;
                    if (ly < 0) ly = 0;
                    if (lx >= s->logical_w) lx = s->logical_w - 1;
                    if (ly >= s->logical_h) ly = s->logical_h - 1;
                    s->mouse_x = (i32)lx;
                    s->mouse_y = (i32)ly;
                } else {
                    s->mouse_x = (i32)(px / (s->backing_scale > 0 ? s->backing_scale : 1.0f));
                    s->mouse_y = (i32)(py / (s->backing_scale > 0 ? s->backing_scale : 1.0f));
                }
                s->mouse_buttons =
                    ((wp & MK_LBUTTON) ? 1 : 0) |
                    ((wp & MK_RBUTTON) ? 2 : 0) |
                    ((wp & MK_MBUTTON) ? 4 : 0);
            }
            return 0;
        case WM_LBUTTONDOWN: if (s) s->mouse_buttons |= 1; return 0;
        case WM_LBUTTONUP:   if (s) s->mouse_buttons &= ~1; return 0;
        case WM_RBUTTONDOWN: if (s) s->mouse_buttons |= 2; return 0;
        case WM_RBUTTONUP:   if (s) s->mouse_buttons &= ~2; return 0;
        case WM_MBUTTONDOWN: if (s) s->mouse_buttons |= 4; return 0;
        case WM_MBUTTONUP:   if (s) s->mouse_buttons &= ~4; return 0;
        case WM_MOUSEWHEEL:
            /* Accumulate RAW deltas: smooth-scroll devices (precision
             * touchpads, high-res wheels) emit many sub-notch events, and
             * dividing each by WHEEL_DELTA individually truncates them
             * all to 0 - the wheel is then dead for the whole session.
             * Whole notches are emitted at the @mouse read instead. */
            if (s) s->mouse_wheel += (i32)((i16)HIWORD(wp));
            return 0;
        case WM_SETCURSOR:
            /* Win9x hosts draw the pointer into the frame themselves (see
             * tigryx_win_softcursor.h), so the system pointer is suppressed
             * over the client area only - the caption and resize borders keep
             * the real arrow and their resize shapes. */
            if (s && s->soft_cursor && LOWORD(lp) == HTCLIENT) {
                SetCursor(NULL);
                return TRUE;
            }
            return DefWindowProcA(hwnd, msg, wp, lp);
        case WM_DPICHANGED:
            /* Sent only to per-monitor-aware processes, i.e. never when
             * RELIC_WINFLAG_NODPI kept us system-aware and never on win95.
             * lParam is the window rect Windows suggests for the new scale
             * factor; honouring it keeps the window the same physical size and
             * under the cursor mid-drag. opengl_sync_size then picks up the new
             * client rect and rebuilds the viewport and projection. */
            if (s && s->dpi_scaling) {
                const RECT *sug = (const RECT*)lp;
                s->dpi = (int)LOWORD(wp);
                if (sug) {
                    SetWindowPos(hwnd, NULL,
                                 (int)sug->left, (int)sug->top,
                                 (int)(sug->right  - sug->left),
                                 (int)(sug->bottom - sug->top),
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
                opengl_sync_size(s);
                return 0;
            }
            return DefWindowProcA(hwnd, msg, wp, lp);
        case WM_NCHITTEST:
            /* RELIC_WINFLAG_FULLCONTENT reclaims the caption strip for the
             * script, which also removes the system move handle. Report the
             * top RELIC_DRAG_STRIP_H pixels of the client area as HTCAPTION
             * so the window stays draggable (and double-click still
             * maximises), minus the trailing RELIC_BUTTONS_W pixels which
             * stay HTCLIENT for the script's own window buttons.
             * DefWindowProc runs first so the WS_THICKFRAME resize borders
             * keep priority over the strip at the corners.
             * The strip constants are logical pixels (the units the script
             * draws its header in) while the client rect is in device pixels,
             * so both are scaled by backing_scale before comparison. */
            if (s && (s->win_flags & RELIC_WINFLAG_FULLCONTENT)) {
                LRESULT ht = DefWindowProcA(hwnd, msg, wp, lp);
                if (ht != HTCLIENT) return ht;
                POINT pt;
                RECT  rc;
                pt.x = (LONG)(i16)LOWORD(lp);
                pt.y = (LONG)(i16)HIWORD(lp);
                ScreenToClient(hwnd, &pt);
                GetClientRect(hwnd, &rc);
                /* In stretch mode the script's logical pixel is one letterbox
                 * pixel, and the content starts at vp_x/vp_y rather than the
                 * client origin. */
                float bs;
                LONG  ox = 0, oy = 0, right = rc.right;
                if (s->stretch_enabled) {
                    bs = (s->logical_h > 0 && s->vp_h > 0)
                       ? (float)s->vp_h / (float)s->logical_h : 1.0f;
                    ox = (LONG)s->vp_x;
                    oy = (LONG)s->vp_y;
                    right = ox + (LONG)s->vp_w;
                } else {
                    bs = (s->backing_scale > 0.0f) ? s->backing_scale : 1.0f;
                }
                LONG strip_h = (LONG)(RELIC_DRAG_STRIP_H * bs);
                LONG btns_w  = (LONG)(RELIC_BUTTONS_W    * bs);
                if (pt.y >= oy && pt.y < oy + strip_h &&
                    pt.x >= ox && pt.x < right - btns_w) return HTCAPTION;
                return HTCLIENT;
            }
            return DefWindowProcA(hwnd, msg, wp, lp);
        default:
            return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

/**
 * @brief Allocates backend state, attaches to a parent console where
 *        available, raises timer resolution and starts the audio thread.
 */
static void opengl_init(RelicVM *vm) {
#if _WIN32_WINNT >= 0x0500
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
#endif

    OpenGLState *s = calloc(1, sizeof(OpenGLState));
    s->width          = WIN_W;
    s->height         = WIN_H;
    s->reported_width  = WIN_W;
    s->reported_height = WIN_H;
    s->backing_scale  = 1.0f;
    s->dpi            = RELIC_DEFAULT_DPI;
    /* The texture table is allocated lazily on the first @texture call;
     * opengl_tex_reserve seeds every new slot's u/v scale to 1.0, which
     * is what the old fixed-array init loop did here for RELIC_WIN95. */
    g_state = s;
    g_vm    = vm;
    vm->backend_data = s;

    timeBeginPeriod(1);
    opengl_snd_init();
    SetConsoleCtrlHandler(opengl_ctrl_handler, TRUE);
}

/**
 * @brief Try to choose an MSAA pixel format using WGL_ARB_multisample.
 *
 * Creates a temporary hidden window + context using a separate window class so
 * it cannot interfere with the real window's input/quit state. Loads
 * wglChoosePixelFormatARB and asks the driver for a 4x multisampled double-
 * buffered RGBA format with stencil. Returns the chosen format index, or 0 on
 * failure. The caller can then fall back to ChoosePixelFormat().
 */
static int opengl_choose_msaa_pixel_format(HINSTANCE hinst) {
    typedef BOOL (WINAPI *PFNWGLCHOOSEPIXELFORMATARBPROC)(HDC hdc,
            const int *piAttribIList, const FLOAT *pfAttribFList,
            UINT nMaxFormats, int *piFormats, UINT *nNumFormats);
    PFNWGLCHOOSEPIXELFORMATARBPROC wglChoosePixelFormatARB = NULL;
    int result = 0;

    const char *dummy_class = "RelicOpenGLMSAAProbe";
    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = hinst;
    wc.lpszClassName = dummy_class;
    if (!RegisterClassExA(&wc)) return 0;

    HWND dummy = CreateWindowExA(0, dummy_class, "", WS_OVERLAPPEDWINDOW,
                                 0, 0, 32, 32, NULL, NULL, hinst, NULL);
    if (!dummy) return 0;

    HDC dc = GetDC(dummy);

    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 32;
    pfd.cDepthBits   = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType   = PFD_MAIN_PLANE;

    int pf = ChoosePixelFormat(dc, &pfd);
    if (pf && SetPixelFormat(dc, pf, &pfd)) {
        HGLRC rc = wglCreateContext(dc);
        if (rc && wglMakeCurrent(dc, rc)) {
            wglChoosePixelFormatARB = (PFNWGLCHOOSEPIXELFORMATARBPROC)
                    wglGetProcAddress("wglChoosePixelFormatARB");
            if (wglChoosePixelFormatARB) {
                int attribs[] = {
                    WGL_DRAW_TO_WINDOW_ARB, 1,
                    WGL_SUPPORT_OPENGL_ARB, 1,
                    WGL_DOUBLE_BUFFER_ARB,  1,
                    WGL_PIXEL_TYPE_ARB,     WGL_TYPE_RGBA_ARB,
                    WGL_COLOR_BITS_ARB,     32,
                    WGL_DEPTH_BITS_ARB,     24,
                    WGL_STENCIL_BITS_ARB,   8,
                    WGL_SAMPLE_BUFFERS_ARB, 1,
                    WGL_SAMPLES_ARB,        4,
                    0, 0
                };
                int formats[16];
                UINT count = 0;
                if (wglChoosePixelFormatARB(dc, attribs, NULL, 16,
                                            formats, &count) && count > 0) {
                    result = formats[0];
                    if (g_vm && g_vm->verbose) fprintf(stderr, "[opengl] MSAA 4x pixel format: %d\n", result);
                }
                if (!result) {
                    /* Try 2x samples as a fallback. */
                    attribs[17] = 2;
                    if (wglChoosePixelFormatARB(dc, attribs, NULL, 16,
                                                formats, &count) && count > 0) {
                        result = formats[0];
                        if (g_vm && g_vm->verbose) fprintf(stderr, "[opengl] MSAA 2x pixel format: %d\n", result);
                    }
                }
                if (!result) {
                    if (g_vm && g_vm->verbose) fprintf(stderr, "[opengl] No MSAA pixel format found; falling back.\n");
                }
            } else {
                if (g_vm && g_vm->verbose) fprintf(stderr, "[opengl] wglChoosePixelFormatARB unavailable; falling back.\n");
            }
            wglMakeCurrent(NULL, NULL);
            wglDeleteContext(rc);
        } else {
            if (g_vm && g_vm->verbose) fprintf(stderr, "[opengl] MSAA bootstrap context failed; falling back.\n");
        }
    } else {
        if (g_vm && g_vm->verbose) fprintf(stderr, "[opengl] MSAA bootstrap pixel format failed; falling back.\n");
    }

    ReleaseDC(dummy, dc);
    DestroyWindow(dummy);
    UnregisterClassA(dummy_class, hinst);
    return result;
}

/**
 * @brief Implements @window: creates the Win32 window and WGL OpenGL context.
 *
 * Honours the optional RELIC_WINFLAG_STRETCH flag, which pins the script to a
 * fixed logical resolution and letterboxes it to the client area.
 */

/* Forward declaration: read_vm_cstr is defined later in this file but is
 * needed by opengl_gfx_window (title argument) and opengl_apply_icon_file. */
static int read_vm_cstr(RelicVM *vm, u32 ptr, char *out, int out_sz);

/* Check for an icon.ico file alongside the executable or script and, if
 * found, override the window's icon via WM_SETICON. Search locations
 * (first match wins):
 *   <exe_dir>/icon.ico
 *   <exe_dir>/assets/icon.ico
 * This lets a compiled program or an interpreted script ship its own
 * icon without recompiling the runtime. */
static void opengl_apply_icon_file(HWND hwnd) {
    if (!hwnd) return;

    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;

    /* Strip the executable name to get the directory. */
    char *slash = strrchr(path, '\\');
    if (!slash) slash = strrchr(path, '/');
    if (!slash) return;
    *slash = 0;

    char ico_path[MAX_PATH];
    const char *candidates[2];
    candidates[0] = "\\icon.ico";
    candidates[1] = "\\assets\\icon.ico";
    const char *found = NULL;
    for (int i = 0; i < 2; i++) {
        if (strlen(path) + strlen(candidates[i]) < MAX_PATH) {
            char test[MAX_PATH];
            strcpy(test, path);
            strcat(test, candidates[i]);
            DWORD attr = GetFileAttributesA(test);
            if (attr != INVALID_FILE_ATTRIBUTES &&
                !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                found = candidates[i];
                break;
            }
        }
    }
    if (!found) return;

    strcpy(ico_path, path);
    strcat(ico_path, found);

    /* Load big (system-size) and small (16x16) icons from the file. */
    HICON hicon_big = (HICON)LoadImageA(NULL, ico_path, IMAGE_ICON, 0, 0,
                                        LR_LOADFROMFILE);
    HICON hicon_small = (HICON)LoadImageA(NULL, ico_path, IMAGE_ICON, 16, 16,
                                          LR_LOADFROMFILE);
    if (hicon_big) {
        SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hicon_big);
    }
    if (hicon_small) {
        SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hicon_small);
    }
}

static void opengl_gfx_window(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s) return;

    /* A second @window call with an already-open window resizes it rather
     * than being a no-op. This lets scripts (e.g. KSINK-win.tgx) shrink the
     * window when switching to a more compact GUI style. Only w/h are
     * honoured; flags and position are kept from the original call since
     * changing window style (FULLCONTENT, NORESIZE, etc.) requires
     * destroying and recreating the window. */
    if (s->hwnd) {
        int w = vm->r[3];
        int h = vm->r[4];
        if (w <= 0) w = s->width;
        if (h <= 0) h = s->height;

        /* RELIC_WINFLAG_FULLSCREEN pins the window to the monitor's bounds,
         * so the pixel geometry and the SetWindowPos below do not apply.
         * The logical render size still updates: in stretch mode that
         * retargets the letterbox, and without stretch opengl_sync_size
         * puts the reported size back to the monitor's on the next pump. */
        bool fs_resize = (s->win_flags & RELIC_WINFLAG_FULLSCREEN) != 0;

        s->width   = w;
        s->height  = h;
        s->logical_w = w;
        s->logical_h = h;
        if (!fs_resize || s->stretch_enabled) {
            s->reported_width  = w;
            s->reported_height = h;
        }
        if (!fs_resize) {
            s->pixel_w = MulDiv(w, s->dpi, RELIC_DEFAULT_DPI);
            s->pixel_h = MulDiv(h, s->dpi, RELIC_DEFAULT_DPI);
            if (s->pixel_w < 1) s->pixel_w = 1;
            if (s->pixel_h < 1) s->pixel_h = 1;
            s->backing_scale = (float)s->pixel_w / (float)w;
        }

        /* Stretch mode: recreate the stretch texture for the new logical
         * resolution, then let opengl_sync_size recompute the letterbox. */
        if (s->stretch_enabled && s->stretch_tex) {
            glDeleteTextures(1, &s->stretch_tex);
            s->stretch_pot_w = next_power_of_2(w);
            s->stretch_pot_h = next_power_of_2(h);
            s->stretch_uscale = (float)w / (float)s->stretch_pot_w;
            s->stretch_vscale = (float)h / (float)s->stretch_pot_h;
            glGenTextures(1, &s->stretch_tex);
            glBindTexture(GL_TEXTURE_2D, s->stretch_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                         s->stretch_pot_w, s->stretch_pot_h, 0,
                         GL_RGB, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
            glBindTexture(GL_TEXTURE_2D, 0);
            glScissor(0, 0, w, h);
        }

        /* Calculate the outer window size for the new client rect. */
        if (!fs_resize) {
            DWORD win_style    = GetWindowLongA(s->hwnd, GWL_STYLE);
            DWORD win_ex_style = GetWindowLongA(s->hwnd, GWL_EXSTYLE);
            RECT r = {0, 0, s->pixel_w, s->pixel_h};
            if (s->dpi_scaling && g_AdjustWindowRectExForDpi) {
                g_AdjustWindowRectExForDpi(&r, win_style, FALSE, win_ex_style,
                                           (UINT)s->dpi);
            } else {
                AdjustWindowRect(&r, win_style, FALSE);
            }
            int ww = r.right - r.left;
            int wh = r.bottom - r.top;

            SetWindowPos(s->hwnd, NULL, 0, 0, ww, wh,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        }

        /* Update the GL viewport and projection to the new size immediately.
         * opengl_sync_size() below has an early-out when the cached sizes
         * already match (which they do, since we just set them above), so
         * without this the viewport stays at the old (smaller) dimensions
         * and drawing only fills the bottom-left corner after a grow. */
        if (!fs_resize) {
            glViewport(0, 0, s->pixel_w, s->pixel_h);
            if (!s->in_3d) {
                glMatrixMode(GL_PROJECTION);
                glLoadIdentity();
                glOrtho(0, w, 0, h, -1, 1);
                glMatrixMode(GL_MODELVIEW);
                glLoadIdentity();
            }
        }
        opengl_sync_size(s);

        /* If a title string was passed (6th arg), update the window title
         * at runtime. This lets scripts rename the window after creation. */
        u32 title_ptr = (u32)vm->r[6];
        if (title_ptr && title_ptr < vm->mem_sz) {
            char title_buf[256];
            read_vm_cstr(vm, title_ptr, title_buf, sizeof(title_buf));
            if (title_buf[0]) SetWindowTextA(s->hwnd, title_buf);
        }
        return;
    }

    int x = vm->r[1];
    int y = vm->r[2];
    int w = vm->r[3];
    int h = vm->r[4];
    u32 flags = (u32)vm->r[5];
    if (w <= 0) w = WIN_W;
    if (h <= 0) h = WIN_H;

    /* Optional 6th argument: window title (C string pointer). Falls back
     * to "Tigryx" if absent (0) or invalid. */
    char title_buf[256] = "Tigryx";
    u32 title_ptr = (u32)vm->r[6];
    if (title_ptr && title_ptr < vm->mem_sz) {
        read_vm_cstr(vm, title_ptr, title_buf, sizeof(title_buf));
        if (!title_buf[0]) {
            /* Empty string -- restore default. */
            title_buf[0] = 'R'; title_buf[1] = 'e'; title_buf[2] = 'l';
            title_buf[3] = 'y'; title_buf[4] = 'x'; title_buf[5] = 0;
        }
    }

    /* Ask for per-monitor DPI awareness, then scale the requested client size
     * by the target monitor's factor: w,h stay the logical size the script
     * draws into while the window is created at w*scale x h*scale real pixels.
     * RELIC_WINFLAG_NODPI keeps the historical behaviour - system-wide
     * awareness and a literal pixel-for-pixel client area - for scripts that
     * want exact control over physical pixels. Neither path does anything on
     * win95, where none of these APIs exist. */
    if (flags & RELIC_WINFLAG_NODPI) {
        init_dpi_funcs();
        if (g_SetProcessDPIAware) g_SetProcessDPIAware();
        s->dpi_scaling = false;
        s->dpi         = RELIC_DEFAULT_DPI;
    } else {
        s->dpi_scaling = opengl_dpi_set_awareness();
        s->dpi = (int)opengl_dpi_at_point(
            (flags & RELIC_WINFLAG_CENTER) ? 0 : x,
            (flags & RELIC_WINFLAG_CENTER) ? 0 : y);
        if (s->dpi <= 0) s->dpi = RELIC_DEFAULT_DPI;
    }

    /* RELIC_WINFLAG_FULLSCREEN: a borderless WS_POPUP window covering the
     * whole monitor - the "fullscreen windowed" model, so no display-mode
     * switch and no exclusive access; the compositor stays in charge and
     * alt-tab is instant. x,y pick the monitor (the point they name, else
     * the primary); w,h remain the logical render size the script draws
     * in - with STRETCH that fixed resolution is letterboxed onto the
     * panel, without it the logical space is the monitor's own, so
     * pixel-accurate scripts get every physical pixel. */
    RECT mon_rc = {0, 0, 0, 0};
    bool fs = (flags & RELIC_WINFLAG_FULLSCREEN) != 0;
    if (fs) opengl_monitor_rect(x, y, &mon_rc);

    s->width   = w;
    s->height  = h;
    s->pixel_w = MulDiv(w, s->dpi, RELIC_DEFAULT_DPI);
    s->pixel_h = MulDiv(h, s->dpi, RELIC_DEFAULT_DPI);
    {
        /* TGX_WIN_PIXELS=WxH - create the window at a fixed client size
         * (device pixels) for capture/testing. */
        const char *wp = getenv("TGX_WIN_PIXELS");
        int pw = 0, ph = 0;
        if (wp && wp[0]) {
            int a = 0, b = 0;
            if (sscanf(wp, "%d%*[,xX]%d", &a, &b) == 2) { pw = a; ph = b; }
        }
        if (pw > 0 && ph > 0) { s->pixel_w = pw; s->pixel_h = ph; }
    }
    if (vm->verbose) fprintf(stderr, "[dpi] logical=%dx%d pixel=%dx%d\n", w, h, s->pixel_w, s->pixel_h);
    if (s->pixel_w < 1) s->pixel_w = 1;
    if (s->pixel_h < 1) s->pixel_h = 1;
    /* Derived from the rounded device size rather than from dpi/96 directly, so
     * converting a logical coordinate to a device pixel with backing_scale
     * matches what glOrtho + glViewport do to the nearest pixel. Text placement
     * relies on that to stay glyph-crisp at fractional scale factors. */
    s->backing_scale   = (float)s->pixel_w / (float)w;
    s->reported_width  = w;
    s->reported_height = h;

    s->win_flags        = flags;
    s->stretch_enabled  = (flags & RELIC_WINFLAG_STRETCH) != 0;
    s->logical_w        = w;
    s->logical_h        = h;
    s->vp_x = 0; s->vp_y = 0; s->vp_w = s->pixel_w; s->vp_h = s->pixel_h;

    if (fs) {
        s->pixel_w = mon_rc.right - mon_rc.left;
        s->pixel_h = mon_rc.bottom - mon_rc.top;
        if (s->pixel_w < 1) s->pixel_w = 1;
        if (s->pixel_h < 1) s->pixel_h = 1;
        s->vp_w = s->pixel_w;
        s->vp_h = s->pixel_h;
        if (s->stretch_enabled) {
            /* The letterbox target is the whole panel; the logical space
             * and reported size stay w x h. Input mapping uses the display
             * scale, matching opengl_sync_size's stretch path. */
            s->backing_scale = (float)s->dpi / (float)RELIC_DEFAULT_DPI;
        } else {
            /* The logical space is the monitor's logical size: the script
             * draws at native scale and @platform reports the same space. */
            s->width  = MulDiv(s->pixel_w, RELIC_DEFAULT_DPI, s->dpi);
            s->height = MulDiv(s->pixel_h, RELIC_DEFAULT_DPI, s->dpi);
            if (s->width < 1)  s->width = 1;
            if (s->height < 1) s->height = 1;
            s->logical_w       = s->width;
            s->logical_h       = s->height;
            s->reported_width  = s->width;
            s->reported_height = s->height;
            s->backing_scale   = (float)s->pixel_w / (float)s->width;
        }
    }

    WNDCLASSEXA wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    /* Load the application icon from the executable's resource section
     * (resource ID 1, embedded via tigryx_resource.o / tigryx.rc). This
     * makes the titlebar and taskbar show the Tigryx logo rather than
     * the generic Windows default. Both tigryx.exe and compiled user
     * executables (e.g. tide.exe) embed this resource. A runtime
     * icon.ico file can override this via WM_SETICON after creation. */
    wc.hIcon         = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    wc.hIconSm       = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(1),
                                         IMAGE_ICON, 16, 16, 0);
    wc.hbrBackground = NULL;
    wc.lpszClassName = "RelicOpenGLWindow";
    RegisterClassExA(&wc);

    /* RELIC_WINFLAG_FULLCONTENT drops the caption so the client area covers
     * the full window and the script can draw where the title bar would be.
     * WS_POPUP removes the caption while WS_THICKFRAME keeps the sizing
     * border (and its resize cursors) intact; WS_MINIMIZEBOX/WS_MAXIMIZEBOX
     * are kept for taskbar restore and Aero snap even though the buttons are
     * not drawn. The system caption buttons go away with the caption --
     * Windows offers no way to keep them over a reclaimed strip -- so such
     * scripts draw their own. WS_EX_APPWINDOW forces a taskbar button, which
     * a WS_POPUP window would not otherwise be guaranteed. */
    DWORD win_style = WS_OVERLAPPEDWINDOW;
    DWORD win_ex_style = 0;
    if (flags & RELIC_WINFLAG_FULLCONTENT) {
        win_style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
        win_ex_style = WS_EX_APPWINDOW;
    }
    if (flags & RELIC_WINFLAG_NORESIZE)
        win_style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    /* Fullscreen wins over every other style bit: plain WS_POPUP so the
     * client rect IS the monitor rect, with no sizing border to inflate
     * it past the panel. WS_EX_APPWINDOW keeps a taskbar entry. */
    if (fs) {
        win_style    = WS_POPUP;
        win_ex_style = WS_EX_APPWINDOW;
    }

    /* The client rect is requested in device pixels. AdjustWindowRectExForDpi
     * (Windows 10 1607+) sizes the frame for the target monitor's DPI; plain
     * AdjustWindowRect only knows the DPI the process was told about at
     * startup, which under per-monitor awareness is the wrong one for any
     * monitor that is not scaled like the primary. */
    RECT r = {0, 0, s->pixel_w, s->pixel_h};
    if (s->dpi_scaling && g_AdjustWindowRectExForDpi) {
        g_AdjustWindowRectExForDpi(&r, win_style, FALSE, win_ex_style, (UINT)s->dpi);
    } else {
        AdjustWindowRect(&r, win_style, FALSE);
    }
    int ww = r.right - r.left;
    int wh = r.bottom - r.top;

    /* Window placement. x,y are honoured literally as the outer top-left
     * corner. WINFLAG_CENTER overrides them and centres the outer rect
     * inside the primary monitor's work area (the region excluding the
     * taskbar), queried via SystemParametersInfoA(SPI_GETWORKAREA). */
    int posx = x;
    int posy = y;
    if (fs) {
        posx = mon_rc.left;
        posy = mon_rc.top;
    } else if (flags & RELIC_WINFLAG_CENTER) {
        RECT work;
        if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
            int work_w = work.right - work.left;
            int work_h = work.bottom - work.top;
            posx = work.left + (work_w - ww) / 2;
            posy = work.top  + (work_h - wh) / 2;
            if (posx < work.left) posx = work.left;
            if (posy < work.top)  posy = work.top;
        }
    }

    s->hwnd = CreateWindowExA(win_ex_style, "RelicOpenGLWindow", title_buf,
        win_style,
        posx, posy, ww, wh,
        NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!s->hwnd) {
        fprintf(stderr, "CreateWindowExA failed\n");
        return;
    }
    if (!fs) {
        opengl_apply_rounded_corners(s->hwnd);
        if (flags & RELIC_WINFLAG_DARK_TITLEBAR) opengl_force_dark_titlebar(s->hwnd);
        else if (flags & RELIC_WINFLAG_AUTO_TITLEBAR) opengl_apply_system_titlebar_theme(s->hwnd);
    }

    s->hdc = GetDC(s->hwnd);

    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER | PFD_SWAP_COPY;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 32;
    pfd.cDepthBits   = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType   = PFD_MAIN_PLANE;

    int pf = 0;
    bool msaa = false;

    /* Try to request a multisampled pixel format for anti-aliased primitives.
     * Stretch mode copies the logical framebuffer to a texture via
     * glCopyTexSubImage2D, which is not valid for a multisampled default
     * framebuffer, so MSAA is disabled in stretch mode. */
    int msaa_pf = 0;
    if (!s->stretch_enabled) {
        msaa_pf = opengl_choose_msaa_pixel_format(GetModuleHandleA(NULL));
    }
    if (msaa_pf && SetPixelFormat(s->hdc, msaa_pf, &pfd)) {
        pf = msaa_pf;
        msaa = true;
    }

    if (!pf) {
        pf = ChoosePixelFormat(s->hdc, &pfd);
        if (!pf || !SetPixelFormat(s->hdc, pf, &pfd)) {
            fprintf(stderr, "ChoosePixelFormat/SetPixelFormat failed\n");
            return;
        }
        if (vm->verbose) fprintf(stderr, "[opengl] Using non-MSAA pixel format: %d\n", pf);
    }

    s->hglrc = wglCreateContext(s->hdc);
    if ((!s->hglrc || !wglMakeCurrent(s->hdc, s->hglrc)) && msaa) {
        fprintf(stderr, "[opengl] MSAA context creation failed; recreating window without MSAA.\n");
        HWND old_hwnd = s->hwnd;
        s->hwnd = NULL;  /* prevent WM_DESTROY from setting quit flags. */
        ReleaseDC(old_hwnd, s->hdc);
        DestroyWindow(old_hwnd);
        s->hwnd = CreateWindowExA(win_ex_style, "RelicOpenGLWindow", title_buf,
            win_style, posx, posy, ww, wh,
            NULL, NULL, GetModuleHandleA(NULL), NULL);
        if (!s->hwnd) {
            fprintf(stderr, "CreateWindowExA failed on MSAA fallback\n");
            return;
        }
        opengl_apply_rounded_corners(s->hwnd);
        if (flags & RELIC_WINFLAG_DARK_TITLEBAR) opengl_force_dark_titlebar(s->hwnd);
        else if (flags & RELIC_WINFLAG_AUTO_TITLEBAR) opengl_apply_system_titlebar_theme(s->hwnd);
        s->hdc = GetDC(s->hwnd);
        msaa = false;
        pf = ChoosePixelFormat(s->hdc, &pfd);
        if (!pf || !SetPixelFormat(s->hdc, pf, &pfd)) {
            fprintf(stderr, "ChoosePixelFormat/SetPixelFormat failed on MSAA fallback\n");
            return;
        }
        if (vm->verbose) fprintf(stderr, "[opengl] Using non-MSAA pixel format: %d\n", pf);
        s->hglrc = wglCreateContext(s->hdc);
    }
    if (!s->hglrc || !wglMakeCurrent(s->hdc, s->hglrc)) {
        fprintf(stderr, "wglCreateContext/wglMakeCurrent failed\n");
        return;
    }
    if (vm->verbose) fprintf(stderr, "[opengl] OpenGL context created (MSAA=%d).\n", (int)msaa);

    ShowWindow(s->hwnd, SW_SHOW);
    UpdateWindow(s->hwnd);

    /* Override the window icon if an icon.ico file is found alongside
     * the executable. This lets scripts ship custom icons. */
    opengl_apply_icon_file(s->hwnd);

    {
        const char *vendor_str   = (const char*)glGetString(GL_VENDOR);
        const char *renderer_str = (const char*)glGetString(GL_RENDERER);
        const char *version_str  = (const char*)glGetString(GL_VERSION);
        if (vm->verbose) fprintf(stderr, "[opengl] GL_VENDOR   = %s\n", vendor_str   ? vendor_str   : "(null)");
        if (vm->verbose) fprintf(stderr, "[opengl] GL_RENDERER = %s\n", renderer_str ? renderer_str : "(null)");
        if (vm->verbose) fprintf(stderr, "[opengl] GL_VERSION  = %s\n", version_str  ? version_str  : "(null)");
        if (renderer_str && strstr(renderer_str, "GDI Generic"))
            s->gl_ms_generic = true;
        if (vm->verbose && s->gl_ms_generic)
            fprintf(stderr, "[opengl] software rasteriser; @mesh will use immediate mode\n");
        if (version_str) {
            int major = 0, minor = 0;
            if (sscanf(version_str, "%d.%d", &major, &minor) == 2) {
                s->gl_version = major * 100 + minor * 10;
            }
        }
    }

    {
        typedef BOOL (WINAPI *PFNWGLSWAPINTERVALEXTPROC)(int);
        PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT =
            (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
        g_glBlendColor = (PFN_glBlendColor_t)wglGetProcAddress("glBlendColor");
        int want_vsync = (s->win_flags & RELIC_WINFLAG_NO_VSYNC) ? 0 : 1;
        BOOL got_vsync = FALSE;
        if (wglSwapIntervalEXT) got_vsync = wglSwapIntervalEXT(want_vsync);
        /* WGL_EXT_swap_control is absent on Windows 95/98-era drivers and on
         * the generic Microsoft software rasteriser, so SwapBuffers returns
         * immediately and @flip presents as fast as the script's loop runs.
         * The 9x display drivers use a software mouse pointer, which the
         * driver removes and redraws around each screen update, so an
         * unthrottled present rate makes the pointer visibly flicker over the
         * window. Fall back to pacing the loop in software. */
        s->vsync_throttle = (want_vsync && !got_vsync);
        s->last_flip_ms = 0;
        /* Pacing alone does not stop the pointer exclusion, so on 9x hosts
         * also draw the pointer into the frame (see tigryx_win_softcursor.h). */
        s->soft_cursor   = relic_win_softcursor_enabled() ? true : false;
        s->cursor_was_on = false;
        if (vm->verbose && s->vsync_throttle)
            fprintf(stderr, "[opengl] no WGL_EXT_swap_control; pacing frames in software\n");
    }

    /* Viewport in device pixels, ortho in logical ones: the script keeps its
     * w x h coordinate space on any DPI and the GPU does the scaling. Stretch
     * mode instead renders 1:1 into a logical-sized corner of the back buffer
     * and letterboxes at flip time, so it keeps a logical viewport. */
    if (s->stretch_enabled) {
        glViewport(0, 0, w, h);
    } else {
        glViewport(0, 0, s->pixel_w, s->pixel_h);
    }
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, s->width, 0, s->height, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (msaa) glEnable(GL_MULTISAMPLE);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    /* We start in 2D, and opengl_ensure_2d early-returns while in_3d is
     * false, so the initial depth write mask has to be set here. */
    glDepthMask(GL_FALSE);

    if (s->stretch_enabled) {
        s->stretch_pot_w = next_power_of_2(w);
        s->stretch_pot_h = next_power_of_2(h);
        s->stretch_uscale = (float)w / (float)s->stretch_pot_w;
        s->stretch_vscale = (float)h / (float)s->stretch_pot_h;
        glGenTextures(1, &s->stretch_tex);
        glBindTexture(GL_TEXTURE_2D, s->stretch_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                     s->stretch_pot_w, s->stretch_pot_h, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        glBindTexture(GL_TEXTURE_2D, 0);

        glScissor(0, 0, w, h);
        glEnable(GL_SCISSOR_TEST);
    }

    SwapBuffers(s->hdc);

    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    s->key_qh = s->key_qt = 0;

    if (!opengl_font_init(s)) {
        fprintf(stderr, "Warning: Font initialization failed, text rendering may not work\n");
    }
    (void)vm;
}

/**
 * @brief Releases the GL context, window, audio, fonts and timer resolution.
 */
static void opengl_shutdown(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s) return;

    opengl_snd_shutdown();

    /* Cancel any in-flight HTTP requests. */
    syswin32_http_shutdown(s->http);

#ifdef TIGRYX_EXPERIMENTAL_SERVER
    /* Tear down any NSNF listeners/connections. */
    nsnf_serve_shutdown(vm);
#endif

    /* Kill and clean up any spawned subprocesses. */
    opengl_proc_shutdown(s);

    opengl_font_shutdown(s);

    if (s->view_tex) { glDeleteTextures(1, &s->view_tex); s->view_tex = 0; }

    if (s->hglrc) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(s->hglrc);
        s->hglrc = NULL;
    }
    if (s->hdc && s->hwnd) {
        ReleaseDC(s->hwnd, s->hdc);
        s->hdc = NULL;
    }
    if (s->hwnd) {
        DestroyWindow(s->hwnd);
        s->hwnd = NULL;
    }

    timeEndPeriod(1);

    /* Sector-based world renderer (@wr_*): final profile flush, then
     * release resources. The flush must happen here as well as
     * periodically in-render, because the periodic one is suppressed
     * while --fps (NO_VSYNC) is active to keep disk I/O out of the
     * benchmark it is measuring. */
    wr_prof_flush();

    /* Release the grow-on-demand texture slot table. The GL objects
     * themselves die with the context, which has already been destroyed
     * above, so only the host-side arrays need freeing here. */
    if (s->textures) {
        free(s->textures);    s->textures    = NULL;
        free(s->tex_width);   s->tex_width   = NULL;
        free(s->tex_height);  s->tex_height  = NULL;
        free(s->tex_u_scale); s->tex_u_scale = NULL;
        free(s->tex_v_scale); s->tex_v_scale = NULL;
        s->tex_cap = 0;
    }

    {
        int i;
        if (s->wr_colormap) { free(s->wr_colormap); s->wr_colormap = NULL; }
        for (i = 0; i < WR_MAX_TEXTURES; i++) {
            if (s->wr_textures[i].data) { free(s->wr_textures[i].data); s->wr_textures[i].data = NULL; }
        }
        for (i = 0; i < WR_MAX_FLATS; i++) {
            if (s->wr_flats[i]) { free(s->wr_flats[i]); s->wr_flats[i] = NULL; }
        }
        if (s->wr_vertices)   free(s->wr_vertices);
        if (s->wr_linedefs)   free(s->wr_linedefs);
        if (s->wr_sidedefs)   free(s->wr_sidedefs);
        if (s->wr_sectors)    free(s->wr_sectors);
        if (s->wr_segs)       free(s->wr_segs);
        if (s->wr_subsectors) free(s->wr_subsectors);
        if (s->wr_nodes)      free(s->wr_nodes);
        if (s->wr_sd_tex)     free(s->wr_sd_tex);
        if (s->wr_sec_tex)    free(s->wr_sec_tex);
        if (s->wr_surf)       free(s->wr_surf);
        if (wr_plane_buf)     { free(wr_plane_buf); wr_plane_buf = NULL; wr_plane_buf_w = 0; }
    }

    free(s);
    vm->backend_data = NULL;
    g_state = NULL;
    g_vm    = NULL;
}

/**
 * @brief Returns the running executable's full path.
 */
static void get_executable_path(char *out, int out_sz) {
    out[0] = '\0';
    DWORD len = GetModuleFileNameA(NULL, out, out_sz);
    if (len > 0) out[len] = '\0';
}

/**
 * @brief Lowercases an ASCII character.
 */
static int ascii_tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

#ifdef RELIC_WIN95
/**
 * @brief Converts a NUL-terminated UTF-8 string to UTF-16LE (wide) in `out`,
 *        returning the number of wide characters (excluding the NUL).
 *
 * Windows 95/98/ME do not support CP_UTF8 (65001) in MultiByteToWideChar -
 * the call fails with ERROR_INVALID_PARAMETER, so every @text/@text_measure
 * call silently produced zero glyphs and no text was drawn. This hand-rolled
 * decoder covers the full UTF-8 range (1-4 bytes) and is only used on the
 * RELIC_WIN95 build; the main Windows build keeps MultiByteToWideChar which
 * is faster and handles edge cases the OS already knows about.
 *
 * `out` is NUL-terminated. Returns at most `out_max - 1` characters.
 */
static int utf8_to_wide(const char *src, wchar_t *out, int out_max) {
    if (!src || !out || out_max <= 0) return 0;
    int n = 0;
    const unsigned char *p = (const unsigned char*)src;
    while (*p && n < out_max - 1) {
        unsigned int cp;
        int adv;
        if (p[0] < 0x80) {
            cp = p[0]; adv = 1;
        } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((unsigned int)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
            adv = 2;
        } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((unsigned int)(p[0] & 0x0F) << 12) | ((unsigned int)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            adv = 3;
        } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            cp = ((unsigned int)(p[0] & 0x07) << 18) | ((unsigned int)(p[1] & 0x3F) << 12) | ((unsigned int)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            adv = 4;
        } else {
            /* Invalid/continuation byte - emit replacement and advance by 1. */
            out[n++] = (wchar_t)0xFFFD;
            p++;
            continue;
        }
        if (cp <= 0xFFFF) {
            out[n++] = (wchar_t)cp;
        } else {
            /* Encode as a UTF-16 surrogate pair (needs two slots). */
            if (n >= out_max - 2) break;
            cp -= 0x10000;
            out[n++] = (wchar_t)(0xD800 + (cp >> 10));
            out[n++] = (wchar_t)(0xDC00 + (cp & 0x3FF));
        }
        p += adv;
    }
    out[n] = L'\0';
    return n;
}
#endif /* RELIC_WIN95 */

/**
 * @brief Case-insensitive substring test; an empty needle always matches.
 */
static bool ci_contains(const char *haystack, const char *needle) {
    if (!needle || !*needle) return true;
    if (!haystack) return false;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        for (; j < nlen; j++) {
            if (ascii_tolower((unsigned char)haystack[i+j]) !=
                ascii_tolower((unsigned char)needle[j])) break;
        }
        if (j == nlen) return true;
    }
    return false;
}

/* Last-resort guess at the Windows family name from a font's filename. Only
 * used when the font's own `name` table cannot be read - a wrong guess makes
 * CreateFontA silently substitute a different typeface. */
static const char *font_family_name(const char *filename) {
    if (ci_contains(filename, "MaterialIconsOutlined") || ci_contains(filename, "Material Icons Outlined")) return "Material Icons Outlined";
    if (ci_contains(filename, "MaterialIcons") || ci_contains(filename, "Material Icons")) return "Material Icons";
    if (ci_contains(filename, "JetBrains")) return "JetBrains Mono";
    if (ci_contains(filename, "Roboto")) return "Roboto";
    return filename;
}

/* Read the family name GDI will match against out of the font's own `name`
 * table (Microsoft platform, name ID 1). The string is UTF-16BE; only the
 * ASCII subset is kept, which covers every family name we ship. Returns
 * false if the table is missing or holds nothing usable. */
static bool font_family_from_name_table(const stbtt_fontinfo *info,
                                        char *out, int out_sz) {
    int len = 0;
    const char *raw = stbtt_GetFontNameString(
        info, &len,
        STBTT_PLATFORM_ID_MICROSOFT,
        STBTT_MS_EID_UNICODE_BMP,
        STBTT_MS_LANG_ENGLISH,
        1 /* nameID 1 = font family */);
    if (!raw || len < 2) return false;

    int n = 0;
    for (int i = 0; i + 1 < len && n < out_sz - 1; i += 2) {
        unsigned hi = (unsigned char)raw[i];
        unsigned lo = (unsigned char)raw[i + 1];
        if (hi != 0) continue;           /* non-ASCII, skip */
        if (lo < 32 || lo > 126) continue;
        out[n++] = (char)lo;
    }
    out[n] = '\0';
    return n > 0;
}

/**
 * @brief Case-insensitive full-string comparison.
 */
static bool ci_streq(const char *a, const char *b) {
    while (*a && *b) {
        if (ascii_tolower((unsigned char)*a) != ascii_tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

/**
 * @brief Derives a face name from a font filename by dropping the .ttf/.otf
 *        extension.
 */
static void font_face_name(const char *filename, char *out, int out_sz) {
    int L = (int)strlen(filename);
    if (L >= 4) {
        const char *dot = filename + L - 4;
        if (dot[0] == '.' &&
            ascii_tolower((unsigned char)dot[2]) == 't' &&
            ascii_tolower((unsigned char)dot[3]) == 'f' &&
            (ascii_tolower((unsigned char)dot[1]) == 't' ||
             ascii_tolower((unsigned char)dot[1]) == 'o')) L -= 4;
    }
    if (L >= out_sz) L = out_sz - 1;
    memcpy(out, filename, (size_t)L);
    out[L] = '\0';
}

/* Font directories are scanned in priority order and a face is only taken
 * from the first directory that provides it, so the same font present both
 * in an `assets` subfolder and in the executable's own folder loads once. */
#define FONT_MAX_DIRS 16

/**
 * @brief Appends `path` to `dirs` if it is an existing directory not already
 *        listed. Paths are canonicalised first so that e.g. the executable
 *        directory and "." are recognised as the same place.
 */
static void add_font_dir(char dirs[][MAX_PATH], int *n, const char *path) {
    if (*n >= FONT_MAX_DIRS || !path || !path[0]) return;

    char full[MAX_PATH];
    if (GetFullPathNameA(path, MAX_PATH, full, NULL) == 0)
        snprintf(full, MAX_PATH, "%s", path);

    DWORD a = GetFileAttributesA(full);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return;

    for (int i = 0; i < *n; i++) if (ci_streq(dirs[i], full)) return;

    snprintf(dirs[*n], MAX_PATH, "%s", full);
    (*n)++;
}

/**
 * @brief Builds the ordered list of directories to scan for fonts: the
 *        TIGRYX_ARTIFACTS roots, then the executable's `assets` subfolder and
 *        the executable's own folder, then the same relative to the CWD.
 * @return The number of directories written to `dirs`.
 */
static int collect_font_dirs(char dirs[][MAX_PATH]) {
    int n = 0;
    /* Oversized so that appending a subfolder to a full-length root cannot
     * truncate; add_font_dir rejects anything that is not a real directory
     * anyway. */
    char buf[MAX_PATH + 16];

    const char *env = getenv("TIGRYX_ARTIFACTS");
    if (env && *env) {
        char *copy = malloc(strlen(env) + 1);
        if (copy) {
            strcpy(copy, env);
            const char *sep = ";";
            char *token = strtok(copy, sep);
            while (token) {
                add_font_dir(dirs, &n, token);
                snprintf(buf, sizeof(buf), "%s/assets", token);
                add_font_dir(dirs, &n, buf);
                snprintf(buf, sizeof(buf), "%s/fonts", token);
                add_font_dir(dirs, &n, buf);
                token = strtok(NULL, sep);
            }
            free(copy);
        }
    }

    char exe_path[MAX_PATH];
    get_executable_path(exe_path, MAX_PATH);
    if (exe_path[0] != '\0') {
        char *last_slash = strrchr(exe_path, '/');
        char *last_bslash = strrchr(exe_path, '\\');
        if (last_bslash && (!last_slash || last_bslash > last_slash)) last_slash = last_bslash;
        if (last_slash) {
            *last_slash = '\0';
            snprintf(buf, sizeof(buf), "%s/assets", exe_path);
            add_font_dir(dirs, &n, buf);
            snprintf(buf, sizeof(buf), "%s/fonts", exe_path);
            add_font_dir(dirs, &n, buf);
            add_font_dir(dirs, &n, exe_path);
        }
    }

    add_font_dir(dirs, &n, "assets");
    add_font_dir(dirs, &n, "fonts");
    add_font_dir(dirs, &n, ".");

    return n;
}

/**
 * @brief Records a font file's path and derived name in the next free face
 *        slot without opening it.
 *
 * This is the only work done for every .ttf/.otf found at startup: no file
 * I/O, no stb_truetype parsing, no GDI registration. The file is only ever
 * opened, parsed for its real GDI family name, and registered the first
 * time a script actually asks to draw with the face - see
 * ensure_face_registered() - so a large fonts/ directory costs nothing
 * beyond the directory scan until each font is actually used.
 * @return 1 on success, 0 if the face table is full.
 */
static int discover_font_face(OpenGLState *s, const char *path, const char *basename) {
    if (s->nfaces >= FONT_MAX_FACES) return 0;
    int idx = s->nfaces;

    char name[64];
    font_face_name(basename, name, (int)sizeof(name));
    snprintf(s->faces[idx].name, sizeof(s->faces[idx].name), "%.*s", (int)sizeof(s->faces[idx].name) - 1, name);
    snprintf(s->faces[idx].path, sizeof(s->faces[idx].path), "%.*s", (int)sizeof(s->faces[idx].path) - 1, path);
    s->faces[idx].family[0]      = '\0';
    s->faces[idx].is_bold        = ci_contains(s->faces[idx].name, "Bold");
    s->faces[idx].is_system      = false;
    s->faces[idx].init           = true;
    s->faces[idx].registered     = false;
    s->faces[idx].font_res_added = false;
    s->faces[idx].font_mem_handle = NULL;
    s->nfaces++;
    return 1;
}

/**
 * @brief Registers a system-provided font face (no file on disk). GDI knows
 *        these by family name (e.g. "MS Sans Serif", "Microsoft Sans Serif")
 *        and provides the raster/outline data natively, so no file I/O or
 *        AddFontResourceExA is needed. The family name is set directly.
 * @return 1 on success, 0 if the face table is full.
 */
static int register_system_font_face(OpenGLState *s, const char *name,
                                     const char *family) {
    if (s->nfaces >= FONT_MAX_FACES) return 0;
    int idx = s->nfaces;
    snprintf(s->faces[idx].name, sizeof(s->faces[idx].name), "%.*s", (int)sizeof(s->faces[idx].name) - 1, name);
    snprintf(s->faces[idx].family, sizeof(s->faces[idx].family), "%.*s", (int)sizeof(s->faces[idx].family) - 1, family);
    s->faces[idx].path[0]         = '\0';
    s->faces[idx].is_bold         = ci_contains(name, "Bold");
    s->faces[idx].is_system       = true;
    s->faces[idx].init            = true;
    s->faces[idx].registered      = false;
    s->faces[idx].font_res_added  = false;
    s->faces[idx].font_mem_handle = NULL;
    s->nfaces++;
    return 1;
}

/**
 * @brief Opens, parses and registers a discovered face with GDI the first
 *        time it is needed. Idempotent: later calls just report the result
 *        of the first attempt.
 * @return true if the face's family name is registered and usable with
 *         CreateFontA.
 */
/**
 * @brief Checks whether a font family is actually installed on the system by
 *        asking GDI to enumerate it. CreateFontA silently substitutes a
 *        different face for an unknown family, so this is the only reliable
 *        way to tell whether a system font (e.g. Tahoma) is really present
 *        before handing its name to CreateFontA.
 * @return true if at least one font with the given family name is installed.
 */
static int CALLBACK font_enum_proc(const LOGFONTA *lf,
                                   const TEXTMETRICA *tm,
                                   DWORD font_type, LPARAM lparam) {
    (void)lf; (void)tm; (void)font_type;
    *((int*)lparam) = 1;
    return 0;  /* stop enumeration after the first match */
}

static bool gdi_family_available(const char *family) {
    HDC dc = GetDC(NULL);
    if (!dc) return false;
    LOGFONTA lf;
    memset(&lf, 0, sizeof(lf));
    snprintf(lf.lfFaceName, sizeof(lf.lfFaceName), "%.*s", LF_FACESIZE - 1, family);
    lf.lfCharSet = DEFAULT_CHARSET;
    volatile int found = 0;
    EnumFontFamiliesExA(dc, &lf, font_enum_proc, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found != 0;
}

static bool ensure_face_registered(OpenGLState *s, int idx) {
    if (idx < 0 || idx >= s->nfaces) return false;
    if (s->faces[idx].registered)
        return s->faces[idx].font_res_added || s->faces[idx].font_mem_handle;
    s->faces[idx].registered = true;
    s->faces[idx].font_mem_handle = NULL;

    /* System fonts (e.g. MS Sans Serif, Tahoma) have no file on disk - GDI
     * provides them natively. Verify the family is actually installed before
     * marking it usable: CreateFontA silently substitutes a different face
     * for an unknown family, so without this check a missing system font
     * (e.g. Tahoma on real Windows 95) would render as whatever GDI's mapper
     * picks instead of falling back to MS Sans Serif. */
    if (s->faces[idx].is_system) {
        if (!gdi_family_available(s->faces[idx].family)) {
            if (g_vm && g_vm->verbose)
                fprintf(stderr, "opengl-win32: system font '%s' not installed\n",
                        s->faces[idx].family);
            return false;
        }
        s->faces[idx].font_res_added = true;
        return true;
    }

    const char *path = s->faces[idx].path;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "opengl-win32: could not open font %s\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return false; }

    unsigned char *buf = (unsigned char*)malloc((size_t)size);
    if (!buf) { fclose(f); return false; }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf); fclose(f); return false;
    }
    fclose(f);

    /* Try stb_truetype init — works for TrueType-outline fonts (.ttf and
     * TrueType-based .otf), just to read the name table. CFF/PostScript-
     * outline OTFs will fail here, but the win32 backend renders via GDI
     * (CreateFontA), not stb, so we can still register the face for GDI use
     * with a guessed family name. */
    stbtt_fontinfo info;
    bool stb_ok = stbtt_InitFont(&info, buf, stbtt_GetFontOffsetForIndex(buf, 0));

    /* Resolve the name CreateFontA has to be given. Reading it from the font
     * itself is the only reliable option: font_family_name() is a guess based
     * on the filename, and a wrong guess does not fail - GDI just substitutes
     * another typeface, which renders as blocks (and as notdef boxes for the
     * icon font's private-use codepoints). */
    if (!stb_ok ||
        !font_family_from_name_table(&info, s->faces[idx].family,
                                     (int)sizeof(s->faces[idx].family))) {
        /* Explicit precision: a LOGFONT face name is capped at LF_FACESIZE
         * anyway, so anything longer could never be matched by CreateFontA. */
        snprintf(s->faces[idx].family, sizeof(s->faces[idx].family), "%.*s",
                 (int)sizeof(s->faces[idx].family) - 1, font_family_name(s->faces[idx].name));
        if (g_vm && g_vm->verbose)
            fprintf(stderr, "opengl-win32: %s: no usable name table, guessing family '%s'\n",
                    s->faces[idx].name, s->faces[idx].family);
    }

    init_font_mem_funcs();

    /* AddFontResourceExA(FR_PRIVATE) registers by path, keeps the font out
     * of the system-wide list, and resolves by name correctly on XP as well
     * as Vista+. Every caller already has the font on disk (packed assets
     * are extracted to a temp file first). */
    if (g_AddFontResourceExA) {
        if (g_AddFontResourceExA(path, FR_PRIVATE, NULL) > 0) {
            s->faces[idx].font_res_added = true;
        } else if (g_vm && g_vm->verbose) {
            fprintf(stderr, "opengl-win32: AddFontResourceExA failed for %s (err %lu)\n",
                    path, (unsigned long)GetLastError());
        }
    }
    if (!s->faces[idx].font_res_added) {
        /* Pre-Windows 2000, or the private registration was refused. */
        if (AddFontResourceA(path) > 0) {
            s->faces[idx].font_res_added = true;
        } else if (g_vm && g_vm->verbose) {
            fprintf(stderr, "opengl-win32: AddFontResourceA failed for %s\n", path);
        }
    }
    /* Belt and braces: also add from memory. Fonts with restrictive
     * embedding permissions (the OS/2 `fsType` bits) can be rasterised
     * differently by GDI - e.g. without ClearType - depending on which API
     * registered the matching family, so both registrations are kept
     * exactly as before to avoid a lazily-loaded face rendering with
     * different antialiasing than it did when registration ran eagerly.
     * This also keeps the face present if the file is removed underneath us
     * mid-run. */
    if (g_AddFontMemResourceEx) {
        DWORD num_fonts = 0;
        s->faces[idx].font_mem_handle = g_AddFontMemResourceEx(buf, (DWORD)size, NULL, &num_fonts);
    }
    free(buf);

    bool usable = s->faces[idx].font_res_added || s->faces[idx].font_mem_handle;
    if (!usable) {
        fprintf(stderr, "opengl-win32: could not register font %s with GDI; "
                        "falling back to another face\n", path);
    } else if (g_vm && g_vm->verbose) {
        fprintf(stderr, "opengl-win32: registered '%s' as family '%s' (%s)\n",
                s->faces[idx].name, s->faces[idx].family,
                s->faces[idx].font_res_added ? "file" : "memory");
    }
    return usable;
}

/**
 * @brief Reports whether a face with this filename's name is already loaded.
 */
static bool face_already_loaded(OpenGLState *s, const char *filename) {
    char name[64];
    font_face_name(filename, name, (int)sizeof(name));
    for (int i = 0; i < s->nfaces; i++) if (ci_streq(s->faces[i].name, name)) return true;
    return false;
}

/**
 * @brief Discovers every font with extension `ext` found in `dir`.
 * @return true if the directory contained at least one matching file.
 */
static bool scan_font_dir(OpenGLState *s, const char *dir, const char *ext) {
    char pattern[MAX_PATH + 16];
    snprintf(pattern, sizeof(pattern), "%s/*.%s", dir, ext);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char path[MAX_PATH];
        /* Bound the filename so dir + separator + name can't exceed MAX_PATH.
         * cFileName is MAX_PATH itself, so without a precision the compiler
         * can't prove the destination is safe. */
        snprintf(path, sizeof(path), "%s/%.*s", dir,
                 (int)(sizeof(path) - strlen(dir) - 2), fd.cFileName);
        /* Same-named faces dedup by name (extension stripped), and every
         * directory is scanned for .ttf before any is scanned for .otf (see
         * opengl_font_init), so a TrueType face - including the Material
         * Icons font - always claims the slot ahead of a CFF one of the same
         * name: GDI rasterizes TrueType glyf outlines through a robust path,
         * whereas the CFF/OpenType-PostScript path leaks session font state
         * on repeated runs and eventually makes icons fail to render until a
         * Windows reboot. */
        if (face_already_loaded(s, fd.cFileName)) continue;
        if (!discover_font_face(s, path, fd.cFileName)) {
            if (g_vm && g_vm->verbose) fprintf(stderr, "opengl-win32: skipped font %s (face table full)\n", path);
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return true;
}

/**
 * @brief Scans every font directory, discovers the fonts found (without
 *        opening or registering them - see discover_font_face) and selects
 *        the default face.
 * @return 1 if at least one face was discovered, 0 otherwise.
 */
static int opengl_font_init(OpenGLState *s) {
    s->nfaces = 0;
    s->default_face = -1;

    char dirs[FONT_MAX_DIRS][MAX_PATH];
    int ndirs = collect_font_dirs(dirs);
    if (ndirs == 0 && g_vm && g_vm->verbose)
        fprintf(stderr, "opengl-win32: no font directory found; trying packed assets\n");

    /* .ttf across every directory first, then .otf, so that a TrueType face
     * always takes precedence over a CFF one of the same name. */
    bool found_any = false;
    for (int i = 0; i < ndirs; i++) found_any |= scan_font_dir(s, dirs[i], "ttf");
    for (int i = 0; i < ndirs; i++) found_any |= scan_font_dir(s, dirs[i], "otf");

    if (!found_any) {
        if (g_vm && g_vm->verbose) {
            for (int i = 0; i < ndirs; i++)
                fprintf(stderr, "opengl-win32: no .ttf/.otf files in %s\n", dirs[i]);
        }
        /* Fallback: extract fonts from packed VM assets to temp files.
         * Discovery still needs a real file on disk for later GDI
         * registration, so the extraction itself cannot be deferred - but it
         * is just a memory-to-disk copy, not font parsing or GDI work. */
        if (g_vm && g_vm->assets) {
            RelicAssets *a = g_vm->assets;
            /* Tracks the winning Material Icons candidate so far, since asset
             * order is not fixed (unlike the disk scan's .ttf-then-.otf
             * passes): a .ttf entry always wins over a .otf one. */
            char icon_tpath[MAX_PATH] = "";
            bool icon_slot_is_ttf = false;
            for (int i = 0; i < a->count && s->nfaces < FONT_MAX_FACES; i++) {
                const char *n = a->entries[i].name;
                size_t nl = strlen(n);
                if (nl < 4) continue;
                const char *ext = n + nl - 4;
                bool is_ttf, is_otf;
                if (ascii_tolower((unsigned char)ext[1]) == 't' &&
                    ascii_tolower((unsigned char)ext[2]) == 't' &&
                    ascii_tolower((unsigned char)ext[3]) == 'f' &&
                    ext[0] == '.') {
                    is_ttf = true; is_otf = false;
                } else if (ascii_tolower((unsigned char)ext[1]) == 'o' &&
                           ascii_tolower((unsigned char)ext[2]) == 't' &&
                           ascii_tolower((unsigned char)ext[3]) == 'f' &&
                           ext[0] == '.') {
                    is_ttf = false; is_otf = true;
                } else continue;
                (void)is_otf;
                const char *bn = strrchr(n, '/');
                if (!bn) bn = strrchr(n, '\\');
                bn = bn ? bn + 1 : n;

                char tmp[MAX_PATH];
                GetTempPathA(MAX_PATH, tmp);
                char tpath[MAX_PATH];
                snprintf(tpath, sizeof(tpath), "%.*srelic_%.180s", (int)sizeof(tpath) - 190, tmp, bn);

                if (ci_contains(bn, "MaterialIcons")) {
                    /* Once a .ttf has claimed the icon slot, never let an
                     * .otf overwrite it; among equals, first one wins. */
                    if (!is_ttf && icon_slot_is_ttf) continue;
                    if (!is_ttf && icon_tpath[0]) continue;
                    FILE *tf = fopen(tpath, "wb");
                    if (!tf) continue;
                    fwrite(a->data + a->entries[i].blob_offset, 1, a->entries[i].blob_size, tf);
                    fclose(tf);
                    if (icon_tpath[0] && !ci_streq(icon_tpath, tpath))
                        DeleteFileA(icon_tpath); /* drop the superseded .otf */
                    snprintf(icon_tpath, sizeof(icon_tpath), "%.*s", MAX_PATH - 1, tpath);
                    icon_slot_is_ttf = is_ttf;
                    continue;
                }

                FILE *tf = fopen(tpath, "wb");
                if (!tf) continue;
                fwrite(a->data + a->entries[i].blob_offset, 1, a->entries[i].blob_size, tf);
                fclose(tf);
                if (discover_font_face(s, tpath, bn)) {
                    if (s->ntemp_fonts < FONT_MAX_FACES) {
                        snprintf(s->temp_font_paths[s->ntemp_fonts], MAX_PATH, "%.*s", MAX_PATH - 1, tpath);
                        s->ntemp_fonts++;
                    }
                } else {
                    DeleteFileA(tpath);
                }
            }
            if (icon_tpath[0]) {
                const char *bn = strrchr(icon_tpath, '/');
                if (!bn) bn = strrchr(icon_tpath, '\\');
                bn = bn ? bn + 1 : icon_tpath;
                if (discover_font_face(s, icon_tpath, bn)) {
                    if (s->ntemp_fonts < FONT_MAX_FACES) {
                        snprintf(s->temp_font_paths[s->ntemp_fonts], MAX_PATH, "%.*s", MAX_PATH - 1, icon_tpath);
                        s->ntemp_fonts++;
                    }
                } else {
                    DeleteFileA(icon_tpath);
                }
            }
        }
        if (s->nfaces == 0) return 0;
    } else if (s->nfaces == 0) {
        fprintf(stderr, "opengl-win32: failed to load any fonts from %s\n", dirs[0]);
        return 0;
    }

    /* Register system fonts that GDI provides natively (no file on disk).
     * These are used by the Win95 theme:
     *   - "Tahoma": the Windows 98/ME/2000/XP UI font, present on every
     *     modern Windows install. Renders cleanly at small sizes and is
     *     noticeably more readable than the MS Sans Serif bitmap font.
     *   - "MS Sans Serif": the actual Windows 95 system raster font. On
     *     modern Windows, GDI substitutes "Microsoft Sans Serif" (the TrueType
     *     replacement) automatically. Used as the fallback when Tahoma is not
     *     present (e.g. on real Windows 95 hardware).
     * ensure_face_registered() verifies each family is actually installed
     * before marking it usable, so a missing font falls back cleanly. */
    register_system_font_face(s, "MS Sans Serif", "MS Sans Serif");
    register_system_font_face(s, "MS Sans Serif Bold", "MS Sans Serif");
    register_system_font_face(s, "Tahoma", "Tahoma");
    register_system_font_face(s, "Tahoma Bold", "Tahoma");
    /* "Segoe UI": the Windows UI font since Vista, present on every install
     * the Win11 theme targets. The theme requests it by name; on a system
     * without it (Wine without corefonts, a stripped-down Windows) the
     * ensure_face_registered() family check fails and select_face() falls
     * back cleanly to a bundled font. */
    register_system_font_face(s, "Segoe UI", "Segoe UI");
    register_system_font_face(s, "Segoe UI Bold", "Segoe UI");

    for (int i = 0; i < s->nfaces; i++) {
        if (ci_contains(s->faces[i].name, "JetBrains") && !s->faces[i].is_bold) {
            s->default_face = i; break;
        }
    }
    if (s->default_face < 0) {
        for (int i = 0; i < s->nfaces; i++) {
            if (ci_contains(s->faces[i].name, "JetBrains")) { s->default_face = i; break; }
        }
    }
    if (s->default_face < 0) s->default_face = 0;

    return 1;
}

/**
 * @brief Unregisters every face still registered with GDI and cleans up
 *        temp files extracted from packed assets.
 */
static void opengl_font_shutdown(OpenGLState *s) {
    if (!s) return;
    init_font_mem_funcs();
    for (int i = 0; i < s->nfaces; i++) {
        /* A face can hold both registrations (see ensure_face_registered),
         * so these are independent checks rather than an either/or. */
        if (s->faces[i].font_mem_handle) {
            if (g_RemoveFontMemResourceEx)
                g_RemoveFontMemResourceEx(s->faces[i].font_mem_handle);
            s->faces[i].font_mem_handle = NULL;
        }
        if (s->faces[i].font_res_added && s->faces[i].path[0]) {
            if (g_RemoveFontResourceExA)
                g_RemoveFontResourceExA(s->faces[i].path, FR_PRIVATE, NULL);
            else
                RemoveFontResourceA(s->faces[i].path);
            s->faces[i].font_res_added = false;
        }
        s->faces[i].path[0]    = '\0';
        s->faces[i].registered = false;
        s->faces[i].init       = false;
    }
    text_cache_flush(s);
    s->nfaces = 0;
    s->default_face = -1;
    for (int i = 0; i < s->ntemp_fonts; i++) {
        if (s->temp_font_paths[i][0]) {
            DeleteFileA(s->temp_font_paths[i]);
            s->temp_font_paths[i][0] = '\0';
        }
    }
    s->ntemp_fonts = 0;
}

/**
 * @brief Finds a discovered face by name (e.g. "Roboto"), preferring the
 *        requested weight but returning any match if that weight is absent.
 * @return A face index, or -1 if no face with this name was discovered.
 */
static int find_face_by_name(OpenGLState *s, const char *name, bool want_bold) {
    int any = -1;
    for (int i = 0; i < s->nfaces; i++) {
        if (!ci_contains(s->faces[i].name, name)) continue;
        if (s->faces[i].is_bold == want_bold) return i;
        if (any < 0) any = i;
    }
    return any;
}

/**
 * @brief Rate-limited font-directory rescan, then a retry lookup.
 *
 * Font faces are normally discovered once at opengl_font_init(). Hosts
 * that embed a VM (TIDE's embedded relvm) can drop additional font files
 * into a scanned directory AFTER init - e.g. fonts extracted from a
 * guest program's packed assets - and this rescan lets a missing face
 * become visible without a restart. The 1.5s rate limit keeps a
 * permanently-missing name from rescanning the directories every frame.
 */
static int font_rescan_for(OpenGLState *s, const char *needle, bool want_bold) {
    static u32 last_ms = 0;
    u32 now = GetTickCount();
    if (last_ms && (u32)(now - last_ms) < 1500u) return -1;
    last_ms = now;
    char dirs[FONT_MAX_DIRS][MAX_PATH];
    int ndirs = collect_font_dirs(dirs);
    int found = 0;
    for (int i = 0; i < ndirs; i++) {
        found |= scan_font_dir(s, dirs[i], "ttf");
        found |= scan_font_dir(s, dirs[i], "otf");
    }
    if (!found) return -1;
    if (g_vm && g_vm->verbose)
        fprintf(stderr, "opengl-win32: font rescan for '%s'\n", needle);
    return find_face_by_name(s, needle, want_bold);
}

/**
 * @brief Chooses the best face for a name + weight and registers it with GDI
 *        (see ensure_face_registered) on first use. If the chosen face - or
 *        the default face - fails to register (e.g. a corrupt or unreadable
 *        font file), falls back to Roboto, then to any face that does
 *        register.
 * @return A face index ready for CreateFontA, or -1 if nothing usable was
 *         found at all.
 */
static int select_face(OpenGLState *s, const char *name, int weight) {
    if (s->nfaces <= 0) return -1;
    bool want_bold = (weight >= 600);

    int idx = -1;
    const char *needle = (name && *name) ? name : NULL;
    if (!needle) {
        if (want_bold) {
            for (int i = 0; i < s->nfaces; i++) {
                if (ci_contains(s->faces[i].name, "JetBrains") && s->faces[i].is_bold) { idx = i; break; }
            }
        }
        if (idx < 0) idx = s->default_face;
    } else {
        int any_match = -1;
        for (int i = 0; i < s->nfaces; i++) {
            if (!ci_contains(s->faces[i].name, needle)) continue;
            if (s->faces[i].is_bold == want_bold) { idx = i; break; }
            if (any_match < 0) any_match = i;
        }
        if (idx < 0) idx = any_match;
        /* Missed entirely: a font dropped into a scanned dir after
         * font_init (e.g. guest fonts extracted by an embedded relvm)
         * can still be found via a rate-limited rescan. */
        if (idx < 0) idx = font_rescan_for(s, needle, want_bold);
        if (idx < 0) idx = s->default_face;
    }

    if (idx >= 0 && ensure_face_registered(s, idx)) {
        if (g_vm && g_vm->verbose && idx < FONT_MAX_FACES) {
            static bool logged[FONT_MAX_FACES];
            if (!logged[idx]) {
                logged[idx] = true;
                fprintf(stderr, "opengl-win32: select_face '%s' (weight=%d, want_bold=%d) -> face %d '%s' family '%s'\n",
                        needle ? needle : "(null)", weight, (int)want_bold,
                        idx, s->faces[idx].name, s->faces[idx].family);
            }
        }
        return idx;
    }

    /* Registration failed. If the requested font was Tahoma (the Win95
     * theme's preferred font, which is not present on real Windows 95),
     * fall back to MS Sans Serif before trying the generic Roboto chain. */
    if (needle && ci_streq(needle, "Tahoma")) {
        int ms = find_face_by_name(s, "MS Sans Serif", want_bold);
        if (ms >= 0 && ms != idx && ensure_face_registered(s, ms)) {
            if (g_vm && g_vm->verbose)
                fprintf(stderr, "opengl-win32: Tahoma unavailable, fell back to MS Sans Serif face %d\n", ms);
            return ms;
        }
    }

    /* Fall back to Roboto, then to whatever else will register at all, so
     * a single bad font file never breaks all text. */
    int fb = find_face_by_name(s, "Roboto", want_bold);
    if (fb >= 0 && fb != idx && ensure_face_registered(s, fb)) {
        if (g_vm && g_vm->verbose)
            fprintf(stderr, "opengl-win32: fell back to Roboto face %d for '%s'\n",
                    fb, needle ? needle : "(null)");
        return fb;
    }
    for (int i = 0; i < s->nfaces; i++) {
        if (i == idx || i == fb) continue;
        if (ensure_face_registered(s, i)) {
            if (g_vm && g_vm->verbose)
                fprintf(stderr, "opengl-win32: fell back to face %d '%s' for '%s'\n",
                        i, s->faces[i].name, needle ? needle : "(null)");
            return i;
        }
    }
    return -1;
}

/**
 * @brief Restores 2D orthographic render state after a 3D pass in the frame.
 */
static void opengl_ensure_2d(OpenGLState *s) {
    if (!s->in_3d) return;
    glDisable(GL_DEPTH_TEST);
    /* 2D primitives must not write depth. Depth testing is off here, but the
     * write mask is independent: with it enabled every 2D primitive stamps
     * z=0.5 (the ortho -1..1 midpoint) into the depth buffer, which then
     * occludes any later 3D pass in the same frame. */
    glDepthMask(GL_FALSE);
    glDisable(GL_LIGHTING);
    glDisable(GL_LIGHT0);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);
    glDisable(GL_COLOR_MATERIAL);
    glDisable(GL_ALPHA_TEST);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    s->in_3d    = false;
    s->light_on = false;
    if (s->gst_on) {
        /* Embedded guest: a 3D->2D transition inside one slice must not
         * tear down into host coordinates - re-assert the guest's own
         * transform + content-rect clip instead. */
        opengl_guest_2d(s);
        return;
    }
    /* Leaving 3D: 2D ortho covers the whole window again, so restore the
     * GL viewport if @view had redirected it to a guest surface rect. */
    if (s->view_w > 0 && s->view_h > 0)
        glViewport(0, 0, s->pixel_w, s->pixel_h);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, s->width, 0, s->height, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glDisable(GL_SCISSOR_TEST);
}

/**
 * @brief Implements @clear: fills the framebuffer with an RGBA colour.
 */
static void opengl_gfx_clear(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;
    s->frame_dirty = true;
    opengl_ensure_2d(s);

    u32 rgba_ptr = (u32)vm->r[1];
    float r = 0, g = 0, b = 0, a = 1;
    if (rgba_ptr + 4 <= vm->mem_sz) {
        r = vm->mem[rgba_ptr]     / 255.0f;
        g = vm->mem[rgba_ptr + 1] / 255.0f;
        b = vm->mem[rgba_ptr + 2] / 255.0f;
        a = vm->mem[rgba_ptr + 3] / 255.0f;
    }
    glClearColor(r, g, b, a);
    /* glClear honours the depth write mask, which opengl_ensure_2d leaves
     * disabled, so it has to be re-enabled for the clear itself and then
     * restored for subsequent 2D drawing. */
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!s->in_3d) glDepthMask(GL_FALSE);
}

/**
 * @brief Implements @sprite: draws a textured, rotatable, tinted quad.
 *
 * src_x/src_y/src_w/src_h crop the source texture in its own pixel space
 * (src_w/src_h <= 0 means "whole texture"). dst_w/dst_h independently set
 * the on-screen quad size in pixels (dst_w/dst_h <= 0 means "same as
 * src_w/src_h"). Decoupling these lets a texture be sampled at its native
 * resolution while drawn at a different logical size, e.g. an HD-upscaled
 * texture drawn at its original low-res display size.
 */
static void opengl_gfx_sprite(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;
    opengl_ensure_2d(s);

    i32   x        = vm->r[1];
    i32   y        = vm->r[2];
    i32   slot     = vm->r[3];
    i32   src_x    = vm->r[4];
    i32   src_y    = vm->r[5];
    i32   src_w    = vm->r[6];
    i32   src_h    = vm->r[7];
    i32   dst_w    = vm->r[8];
    i32   dst_h    = vm->r[9];
    float angle    = ((RelicFBits){ .i = vm->r[10] }).f;
    u32   rgba_ptr = (u32)vm->r[11];

    if (slot < 1 || slot >= s->tex_cap) return;
    GLuint tex = s->textures[slot];
    if (!tex) return;

    int tex_w = s->tex_width[slot];
    int tex_h = s->tex_height[slot];
    if (tex_w <= 0 || tex_h <= 0) return;
    if (src_w <= 0) src_w = tex_w;
    if (src_h <= 0) src_h = tex_h;
    if (dst_w <= 0) dst_w = src_w;
    if (dst_h <= 0) dst_h = src_h;

    /* On OpenGL 1.1 (Win95/98), non-power-of-two images are padded to the
     * next POT size. tex_u_scale/tex_v_scale map the original image extent
     * into the padded texture so UV 1.0 hits the image edge, not the POT
     * edge (which would show transparent padding). */
    float us = s->tex_u_scale[slot];
    float vs = s->tex_v_scale[slot];
    float u0 = (float)src_x           / (float)tex_w * us;
    float u1 = (float)(src_x + src_w) / (float)tex_w * us;
    float v0 = (float)src_y           / (float)tex_h * vs;
    float v1 = (float)(src_y + src_h) / (float)tex_h * vs;

    float cx_s = (float)x + dst_w * 0.5f;
    float cy_s = (float)y + dst_h * 0.5f;
    float cosA = cosf(angle);
    float sinA = sinf(angle);
    float hx = dst_w * 0.5f;
    float hy = dst_h * 0.5f;
    float rx[4] = { -hx,  hx,  hx, -hx };
    float ry[4] = { -hy, -hy,  hy,  hy };

    float vx[4], vy[4];
    for (int i = 0; i < 4; i++) {
        float rx_rot = rx[i] * cosA - ry[i] * sinA;
        float ry_rot = rx[i] * sinA + ry[i] * cosA;
        vx[i] = cx_s + rx_rot;
        vy[i] = (float)s->height - (cy_s + ry_rot);
    }

    float tr = 1.0f, tg = 1.0f, tb = 1.0f, ta = 1.0f;
    if (rgba_ptr + 4 <= vm->mem_sz) {
        tr = vm->mem[rgba_ptr]     / 255.0f;
        tg = vm->mem[rgba_ptr + 1] / 255.0f;
        tb = vm->mem[rgba_ptr + 2] / 255.0f;
        ta = vm->mem[rgba_ptr + 3] / 255.0f;
    }

    if (u1 > 1.0f) u1 = 1.0f;
    if (v1 > 1.0f) v1 = 1.0f;

    /* Always clamp, never repeat. A sprite runs to u/v = 1.0 (or, on a
     * padded NPOT texture, to the image/pad boundary); under GL_REPEAT the
     * GL_LINEAR kernel at that edge wraps around and blends in the
     * *opposite* edge of the image, leaving a stray hairline down the right
     * and bottom of every sprite. The textures default to GL_REPEAT for
     * @mesh's benefit, so set clamping here and restore it after the quad.
     * GL_CLAMP_TO_EDGE is 1.2+; 1.1 drivers reject the enum and would
     * silently stay on GL_REPEAT, so fall back to GL_CLAMP there. */
    GLint clamp_mode = (s->gl_version >= 120) ? GL_CLAMP_TO_EDGE : GL_CLAMP;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp_mode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp_mode);
    glColor4f(tr, tg, tb, ta);

    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(vx[0], vy[0]);
    glTexCoord2f(u1, v0); glVertex2f(vx[1], vy[1]);
    glTexCoord2f(u1, v1); glVertex2f(vx[2], vy[2]);
    glTexCoord2f(u0, v1); glVertex2f(vx[3], vy[3]);
    glEnd();

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    s->frame_dirty = true;
}

/**
 * @brief Implements @plot: draws a single coloured pixel.
 */
static void opengl_gfx_plot(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;
    s->frame_dirty = true;
    opengl_ensure_2d(s);

    int x = vm->r[1];
    int y = vm->r[2];
    u32 rgba_ptr = (u32)vm->r[3];
    if (x < 0 || x >= s->width || y < 0 || y >= s->height) return;
    if (rgba_ptr + 4 > vm->mem_sz) return;

    y = s->height - 1 - y;

    float r = vm->mem[rgba_ptr]     / 255.0f;
    float g = vm->mem[rgba_ptr + 1] / 255.0f;
    float b = vm->mem[rgba_ptr + 2] / 255.0f;
    float a = vm->mem[rgba_ptr + 3] / 255.0f;

    glColor4f(r, g, b, a);
    glPointSize(s->backing_scale);
    glBegin(GL_POINTS);
    glVertex2i(x, y);
    glEnd();
}

/**
 * @brief Paces presentation to the display refresh rate when the driver has no
 *        swap-interval support (see vsync_throttle). GetDeviceCaps(VREFRESH)
 *        reports 0 or 1 for "device default", in which case 60Hz is assumed.
 */
static void opengl_wait_for_frame(OpenGLState *s) {
    if (!s->vsync_throttle) return;
    int hz = s->hdc ? GetDeviceCaps(s->hdc, VREFRESH) : 0;
    if (hz <= 1) hz = 60;
    DWORD interval = (DWORD)(1000 / hz);
    if (interval < 1) interval = 1;
    DWORD now = timeGetTime();
    if (s->last_flip_ms) {
        DWORD elapsed = now - s->last_flip_ms;
        if (elapsed < interval) {
            Sleep(interval - elapsed);
            now = timeGetTime();
        }
    }
    s->last_flip_ms = now;
}

/**
 * @brief Implements @flip: presents the frame, applying letterbox scaling in
 *        stretch mode, and skips redundant swaps to avoid flicker.
 */
/**
 * @brief Draws the software mouse pointer into the back buffer at (cx,cy) in
 *        logical pixels, as horizontal runs of black outline and white fill.
 *
 * Called with the 2D ortho state active, so it shares the script's coordinate
 * space (y down, flipped into GL's y up here as everywhere else). Any clip
 * rectangle the script left set is ignored: the pointer is not part of the
 * script's drawing and must not be clipped by it.
 */
static void opengl_draw_soft_cursor(OpenGLState *s, int cx, int cy) {
    GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (had_scissor) glDisable(GL_SCISSOR_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);

    float top = (float)s->height;
    glBegin(GL_QUADS);
    for (int row = 0; row < RELIC_SOFTCUR_H; row++) {
        const char *art = relic_softcur_arrow[row];
        int col = 0;
        while (col < RELIC_SOFTCUR_W) {
            char c = art[col];
            if (c == ' ') { col++; continue; }
            int run = 1;
            while (col + run < RELIC_SOFTCUR_W && art[col + run] == c) run++;
            if (c == 'X') glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
            else          glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            float x0 = (float)(cx + col), x1 = x0 + (float)run;
            float y0 = top - (float)(cy + row), y1 = y0 - 1.0f;
            glVertex2f(x0, y0);
            glVertex2f(x1, y0);
            glVertex2f(x1, y1);
            glVertex2f(x0, y1);
            col += run;
        }
    }
    glEnd();

    glEnable(GL_BLEND);
    if (had_scissor) glEnable(GL_SCISSOR_TEST);
}

static void opengl_gfx_flip(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;

    opengl_pump_events(s);

    /* Return to 2D state *before* presenting. A frame that ends with a 3D
     * pass and no trailing 2D draw (e.g. tdoom with --no-hud, where nothing
     * is drawn over the world) would otherwise reach the stretch blit below
     * with the 3D matrices and per-fragment tests still active, and present
     * a black screen. Scripts that do draw 2D last were transitioning here
     * implicitly via their first 2D call, which is why this only ever showed
     * up when the HUD was disabled. */
    opengl_ensure_2d(s);

    /* The software pointer only reaches the screen as part of a present, so a
     * pointer that moved (or entered/left the client area) has to force one
     * even when the script drew nothing new this frame - otherwise it freezes
     * until the script's next visible change. Presenting a frame the script did
     * not redraw relies on the back buffer surviving the previous swap, which
     * holds for the swap-by-copy 9x drivers this path is limited to. */
    bool cursor_on = false;
    if (s->soft_cursor) {
        cursor_on = relic_win_softcursor_visible(s->hwnd) ? true : false;
        if (cursor_on != s->cursor_was_on ||
            (cursor_on && (s->mouse_x != s->cursor_x || s->mouse_y != s->cursor_y)))
            s->frame_dirty = true;
        s->cursor_was_on = cursor_on;
    }

    if (s->hdc && s->frame_dirty) {
        if (cursor_on) {
            opengl_draw_soft_cursor(s, s->mouse_x, s->mouse_y);
            s->cursor_x = s->mouse_x;
            s->cursor_y = s->mouse_y;
        }
        if (s->stretch_enabled) {
            glDisable(GL_SCISSOR_TEST);

            glBindTexture(GL_TEXTURE_2D, s->stretch_tex);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
                                s->logical_w, s->logical_h);

            glViewport(0, 0, s->pixel_w, s->pixel_h);
            glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
            glOrtho(0, s->pixel_w, 0, s->pixel_h, -1, 1);
            glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();

            glDisable(GL_DEPTH_TEST);
            glDisable(GL_LIGHTING);
            glDisable(GL_BLEND);
            glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);

            glEnable(GL_TEXTURE_2D);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
            float u1 = s->stretch_uscale;
            float v1 = s->stretch_vscale;
            int x0 = s->vp_x,           y0 = s->vp_y;
            int x1 = s->vp_x + s->vp_w, y1 = s->vp_y + s->vp_h;
            glBegin(GL_QUADS);
                glTexCoord2f(0.0f, 0.0f); glVertex2i(x0, y0);
                glTexCoord2f(u1,   0.0f); glVertex2i(x1, y0);
                glTexCoord2f(u1,   v1  ); glVertex2i(x1, y1);
                glTexCoord2f(0.0f, v1  ); glVertex2i(x0, y1);
            glEnd();
            glDisable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, 0);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

            glMatrixMode(GL_PROJECTION); glPopMatrix();
            glMatrixMode(GL_MODELVIEW);  glPopMatrix();

            glViewport(0, 0, s->logical_w, s->logical_h);
            glScissor(0, 0, s->logical_w, s->logical_h);
            glEnable(GL_SCISSOR_TEST);
            glEnable(GL_BLEND);
        }
        /* $TGXDUMP capture before the swap (glReadPixels reads the back
         * buffer): stretch mode presents into the letterbox rect vp_*,
         * otherwise the frame spans the whole backing store. */
        if (s->stretch_enabled)
            relic_fbdump_maybe(s->vp_x, s->vp_y, s->vp_w, s->vp_h, &s->fbdump_n);
        else
            relic_fbdump_maybe(0, 0, s->pixel_w, s->pixel_h, &s->fbdump_n);
        /* glFlush() before SwapBuffers() causes CPU-GPU sync stalls on old
         * hardware (e.g. S3 Trio on Win95). SwapBuffers() handles flushing
         * internally, so the explicit call is unnecessary and harmful. */
        opengl_wait_for_frame(s);
        SwapBuffers(s->hdc);
        s->frame_dirty = false;
    }

    opengl_ensure_2d(s);
}

/**
 * @brief Draws a thick line as a rounded-cap "pill" via a triangle fan.
 */
static void opengl_draw_thick_line_round(float x1, float y1,
                                         float x2, float y2, float hw) {
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 0.001f) len = 0.001f;
    float ang0 = atan2f(dy, dx) + 1.5707963267948966f;
    int N = (int)(hw * 2.0f);
    if (N < 6)  N = 6;
    if (N > 32) N = 32;
    const float PI = 3.14159265358979f;
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(0.5f * (x1 + x2), 0.5f * (y1 + y2));
    for (int i = 0; i <= N; i++) {
        float t = ang0 + PI * (float)i / (float)N;
        glVertex2f(x1 + cosf(t) * hw, y1 + sinf(t) * hw);
    }
    for (int i = 0; i <= N; i++) {
        float t = ang0 + PI + PI * (float)i / (float)N;
        glVertex2f(x2 + cosf(t) * hw, y2 + sinf(t) * hw);
    }
    glVertex2f(x1 + cosf(ang0) * hw, y1 + sinf(ang0) * hw);
    glEnd();
}

/**
 * @brief Implements @line: draws a coloured line, thin or thick.
 */
static void opengl_gfx_line(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;
    s->frame_dirty = true;
    opengl_ensure_2d(s);

    int x1 = vm->r[1], y1 = vm->r[2];
    int x2 = vm->r[3], y2 = vm->r[4];
    u32 rgba_ptr = (u32)vm->r[5];
    int width = vm->r[6];
    if (width < 1) width = 1;
    if (rgba_ptr + 4 > vm->mem_sz) return;

    float r = vm->mem[rgba_ptr]     / 255.0f;
    float g = vm->mem[rgba_ptr + 1] / 255.0f;
    float b = vm->mem[rgba_ptr + 2] / 255.0f;
    float a = vm->mem[rgba_ptr + 3] / 255.0f;

    y1 = s->height - 1 - y1;
    y2 = s->height - 1 - y2;

    glColor4f(r, g, b, a);
    if (width <= 1) {
        /* glLineWidth is in device pixels, so scale it to keep width=1 meaning
         * one logical pixel (= backing_scale device pixels when DPI-scaled). */
        glLineWidth(s->backing_scale);
        glBegin(GL_LINES);
        glVertex2i(x1, y1);
        glVertex2i(x2, y2);
        glEnd();
    } else {
        /* Thick strokes are geometry emitted in the logical ortho space, which
         * the viewport already scales, so hw must stay in logical units -
         * multiplying by backing_scale here would double the stroke. */
        float hw = (float)width * 0.5f;
        opengl_draw_thick_line_round((float)x1, (float)y1,
                                     (float)x2, (float)y2, hw);
    }
}

/**
 * @brief Implements @polygon: fills a convex polygon from a point list.
 */
static void opengl_gfx_polygon(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;
    s->frame_dirty = true;
    opengl_ensure_2d(s);

    u32 points_ptr = (u32)vm->r[1];
    i32 count = vm->r[2];
    u32 rgba_ptr = (u32)vm->r[3];

    if (count < 3 || points_ptr + count * 4 > vm->mem_sz) return;
    if (rgba_ptr + 4 > vm->mem_sz) return;

    float r = vm->mem[rgba_ptr]     / 255.0f;
    float g = vm->mem[rgba_ptr + 1] / 255.0f;
    float b = vm->mem[rgba_ptr + 2] / 255.0f;
    float a = vm->mem[rgba_ptr + 3] / 255.0f;

    glColor4f(r, g, b, a);
    glBegin(GL_POLYGON);
    for (int i = 0; i < count; i++) {
        u32 offset = points_ptr + i * 4;
        i16 px = (i16)(vm->mem[offset]     | (vm->mem[offset + 1] << 8));
        i16 py = (i16)(vm->mem[offset + 2] | (vm->mem[offset + 3] << 8));
        /* height - y like the draw_list/scissor convention: a fill at
         * y=0 must cover the framebuffer's top row (the "-1" used by the
         * point/line primitives is pixel-centring for strokes). */
        py = s->height - py;
        glVertex2i(px, py);
    }
    glEnd();
}

/**
 * @brief Implements @polyline: draws a connected line strip, thin or thick.
 */
static void opengl_gfx_polyline(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;
    s->frame_dirty = true;
    opengl_ensure_2d(s);

    u32 points_ptr = (u32)vm->r[1];
    i32 count = vm->r[2];
    u32 rgba_ptr = (u32)vm->r[3];
    int width = vm->r[4];
    if (width < 1) width = 1;

    if (count < 2 || points_ptr + count * 4 > vm->mem_sz) return;
    if (rgba_ptr + 4 > vm->mem_sz) return;

    float r = vm->mem[rgba_ptr]     / 255.0f;
    float g = vm->mem[rgba_ptr + 1] / 255.0f;
    float b = vm->mem[rgba_ptr + 2] / 255.0f;
    float a = vm->mem[rgba_ptr + 3] / 255.0f;

    glColor4f(r, g, b, a);
    if (width <= 1) {
        glLineWidth(s->backing_scale);
        glBegin(GL_LINE_STRIP);
        for (int i = 0; i < count; i++) {
            u32 offset = points_ptr + i * 4;
            i16 px = (i16)(vm->mem[offset]     | (vm->mem[offset + 1] << 8));
            i16 py = (i16)(vm->mem[offset + 2] | (vm->mem[offset + 3] << 8));
            py = s->height - 1 - py;
            glVertex2i(px, py);
        }
        glEnd();
    } else {
        float hw = (float)width * 0.5f;

        glEnable(GL_STENCIL_TEST);
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        glStencilMask(0xFF);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDisable(GL_BLEND);

        for (int i = 0; i < count - 1; i++) {
            u32 o1 = points_ptr + i * 4;
            u32 o2 = points_ptr + (i + 1) * 4;
            i16 px1 = (i16)(vm->mem[o1]     | (vm->mem[o1 + 1] << 8));
            i16 py1 = (i16)(vm->mem[o1 + 2] | (vm->mem[o1 + 3] << 8));
            i16 px2 = (i16)(vm->mem[o2]     | (vm->mem[o2 + 1] << 8));
            i16 py2 = (i16)(vm->mem[o2 + 2] | (vm->mem[o2 + 3] << 8));
            py1 = s->height - 1 - py1;
            py2 = s->height - 1 - py2;
            opengl_draw_thick_line_round((float)px1, (float)py1,
                                         (float)px2, (float)py2, hw);
        }

        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_BLEND);
        glStencilFunc(GL_EQUAL, 1, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilMask(0x00);

        glColor4f(r, g, b, a);
        glBegin(GL_QUADS);
        glVertex2i(0,         0);
        glVertex2i(s->width,  0);
        glVertex2i(s->width,  s->height);
        glVertex2i(0,         s->height);
        glEnd();

        glStencilMask(0xFF);
        glDisable(GL_STENCIL_TEST);
    }
}

/* -------------------------------------------------------------------------
 * Ellipse and rounded-rectangle primitives
 * ---------------------------------------------------------------------- */

#define OPENGL_ELLIPSE_SEGS 100

static const float OPENGL_PI = 3.14159265358979f;

static bool opengl_read_rgba(RelicVM *vm, u32 rgba_ptr,
                              float *r, float *g, float *b, float *a) {
    if (rgba_ptr + 4 > vm->mem_sz) return false;
    *r = vm->mem[rgba_ptr]     / 255.0f;
    *g = vm->mem[rgba_ptr + 1] / 255.0f;
    *b = vm->mem[rgba_ptr + 2] / 255.0f;
    *a = vm->mem[rgba_ptr + 3] / 255.0f;
    return true;
}

static void opengl_fill_ellipse_arc(float cx, float cy, float rx, float ry,
                                    float start_deg, float finish_deg) {
    float span = finish_deg - start_deg;
    int N = (int)(fabsf(span) / 360.0f * (float)OPENGL_ELLIPSE_SEGS);
    if (N < 4) N = 4;
    if (N > OPENGL_ELLIPSE_SEGS) N = OPENGL_ELLIPSE_SEGS;
    float step = span / (float)N;
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int i = 0; i <= N; i++) {
        float ang = (start_deg + step * (float)i) * OPENGL_PI / 180.0f;
        glVertex2f(cx + cosf(ang) * rx, cy + sinf(ang) * ry);
    }
    glEnd();
}

static void opengl_stroke_ellipse_arc(OpenGLState *s,
                                      float cx, float cy, float rx, float ry,
                                      float start_deg, float finish_deg, float hw,
                                      float cr, float cg, float cb, float ca) {
    float span = finish_deg - start_deg;
    int N = (int)(fabsf(span) / 360.0f * (float)OPENGL_ELLIPSE_SEGS);
    if (N < 4) N = 4;
    if (N > OPENGL_ELLIPSE_SEGS) N = OPENGL_ELLIPSE_SEGS;
    float step = span / (float)N;

    glEnable(GL_STENCIL_TEST);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(0xFF);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDisable(GL_BLEND);

    for (int i = 0; i < N; i++) {
        float ang1 = (start_deg + step * (float)i)       * OPENGL_PI / 180.0f;
        float ang2 = (start_deg + step * (float)(i + 1)) * OPENGL_PI / 180.0f;
        float x1 = cx + cosf(ang1) * rx;
        float y1 = cy + sinf(ang1) * ry;
        float x2 = cx + cosf(ang2) * rx;
        float y2 = cy + sinf(ang2) * ry;
        opengl_draw_thick_line_round(x1, y1, x2, y2, hw);
    }

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilMask(0x00);

    glColor4f(cr, cg, cb, ca);
    glBegin(GL_QUADS);
    glVertex2i(0,        0);
    glVertex2i(s->width, 0);
    glVertex2i(s->width, s->height);
    glVertex2i(0,        s->height);
    glEnd();

    glStencilMask(0xFF);
    glDisable(GL_STENCIL_TEST);
}

static void opengl_gfx_ellipse(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;
    s->frame_dirty = true;
    opengl_ensure_2d(s);

    int cx       = vm->r[1];
    int cy       = vm->r[2];
    int w        = vm->r[3];
    int h        = vm->r[4];
    int sw       = vm->r[5];
    u32 sc_ptr   = (u32)vm->r[6];
    int start    = vm->r[7];
    int finish   = vm->r[8];

    float rx = (float)w * 0.5f;
    float ry = (float)h * 0.5f;
    float fcx = (float)cx;
    float fcy = (float)(s->height - cy);

    float sr, sg, sb, sa;
    if (sw > 0 && opengl_read_rgba(vm, sc_ptr, &sr, &sg, &sb, &sa)) {
        float hw = (float)sw * 0.5f;
        glColor4f(sr, sg, sb, sa);
        opengl_stroke_ellipse_arc(s, fcx, fcy, rx, ry,
                                  (float)start, (float)finish, hw,
                                  sr, sg, sb, sa);
    }
}

static void opengl_gfx_ellipse_filled(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;
    s->frame_dirty = true;
    opengl_ensure_2d(s);

    int cx       = vm->r[1];
    int cy       = vm->r[2];
    int w        = vm->r[3];
    int h        = vm->r[4];
    int sw       = vm->r[5];
    u32 sc_ptr   = (u32)vm->r[6];
    int start    = vm->r[7];
    int finish   = vm->r[8];
    u32 fc_ptr   = (u32)vm->r[9];

    float rx = (float)w * 0.5f;
    float ry = (float)h * 0.5f;
    float fcx = (float)cx;
    float fcy = (float)(s->height - cy);

    float fr, fg, fb, fa;
    if (opengl_read_rgba(vm, fc_ptr, &fr, &fg, &fb, &fa)) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(fr, fg, fb, fa);
        opengl_fill_ellipse_arc(fcx, fcy, rx, ry, (float)start, (float)finish);
    }

    float sr, sg, sb, sa;
    if (sw > 0 && opengl_read_rgba(vm, sc_ptr, &sr, &sg, &sb, &sa)) {
        float hw = (float)sw * 0.5f;
        opengl_stroke_ellipse_arc(s, fcx, fcy, rx, ry,
                                  (float)start, (float)finish, hw,
                                  sr, sg, sb, sa);
    }
}

static int opengl_build_rrect(float *pts, float x1, float y1, float x2, float y2,
                               float radius, int height) {
    float r = radius;
    float half_w = (x2 - x1) * 0.5f;
    float half_h = (y2 - y1) * 0.5f;
    if (r > half_w) r = half_w;
    if (r > half_h) r = half_h;
    if (r < 0.0f)   r = 0.0f;

    int segs = 32;
    if (r < 4.0f) segs = 8;
    else if (r > 32.0f) segs = 48;
    int n = 0;

    float cx[4] = { x1 + r, x2 - r, x2 - r, x1 + r };
    float cy[4] = { y1 + r, y1 + r, y2 - r, y2 - r };
    float start_deg[4] = { 180.0f, 270.0f, 0.0f, 90.0f };

    for (int c = 0; c < 4; c++) {
        for (int i = 0; i <= segs; i++) {
            float ang = (start_deg[c] + 90.0f * (float)i / (float)segs)
                        * OPENGL_PI / 180.0f;
            float px = cx[c] + cosf(ang) * r;
            float py = cy[c] + sinf(ang) * r;
            pts[n * 2]     = px;
            pts[n * 2 + 1] = (float)height - py;
            n++;
        }
    }
    return n;
}

static void opengl_gfx_rounded_rect(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;
    s->frame_dirty = true;
    opengl_ensure_2d(s);

    int x1     = vm->r[1];
    int y1     = vm->r[2];
    int x2     = vm->r[3];
    int y2     = vm->r[4];
    int sw     = vm->r[5];
    u32 sc_ptr = (u32)vm->r[6];
    int radius = vm->r[7];

    if (sw <= 0) return;
    float sr, sg, sb, sa;
    if (!opengl_read_rgba(vm, sc_ptr, &sr, &sg, &sb, &sa)) return;

    float pts[(48 * 4 + 8) * 2];  /* max 48 segments per corner */
    int n = opengl_build_rrect(pts, (float)x1, (float)y1, (float)x2, (float)y2,
                               (float)radius, s->height);
    if (n < 2) return;

    float hw = (float)sw * 0.5f;

    if (hw <= 0.5f) {
        glColor4f(sr, sg, sb, sa);
        glLineWidth(s->backing_scale);
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < n; i++)
            glVertex2f(pts[i * 2], pts[i * 2 + 1]);
        glEnd();
        return;
    }

    glEnable(GL_STENCIL_TEST);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(0xFF);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDisable(GL_BLEND);

    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        opengl_draw_thick_line_round(pts[i*2], pts[i*2+1],
                                     pts[j*2], pts[j*2+1], hw);
    }

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilMask(0x00);

    glColor4f(sr, sg, sb, sa);
    glBegin(GL_QUADS);
    glVertex2i(0,        0);
    glVertex2i(s->width, 0);
    glVertex2i(s->width, s->height);
    glVertex2i(0,        s->height);
    glEnd();

    glStencilMask(0xFF);
    glDisable(GL_STENCIL_TEST);
}

static void opengl_gfx_rounded_rect_filled(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;
    s->frame_dirty = true;
    opengl_ensure_2d(s);

    int x1        = vm->r[1];
    int y1        = vm->r[2];
    int x2        = vm->r[3];
    int y2        = vm->r[4];
    int sw        = vm->r[5];
    u32 sc_ptr    = (u32)vm->r[6];
    int radius    = vm->r[7];
    u32 fc_ptr    = (u32)vm->r[8];
    int elevation = vm->r[9];

    float pts[(48 * 4 + 8) * 2];  /* max 48 segments per corner */

    /* Optional drop-shadow/elevation for contained-button style. */
    if (elevation > 0) {
        int shadow_n = opengl_build_rrect(pts, (float)(x1 + elevation), (float)(y1 + elevation),
                                          (float)(x2 + elevation), (float)(y2 + elevation),
                                          (float)radius, s->height);
        if (shadow_n >= 3) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(0.0f, 0.0f, 0.0f, 0.25f);
            glBegin(GL_POLYGON);
            for (int i = 0; i < shadow_n; i++)
                glVertex2f(pts[i * 2], pts[i * 2 + 1]);
            glEnd();
        }
    }

    int n = opengl_build_rrect(pts, (float)x1, (float)y1, (float)x2, (float)y2,
                               (float)radius, s->height);
    if (n < 3) return;

    float fr, fg, fb, fa;
    if (opengl_read_rgba(vm, fc_ptr, &fr, &fg, &fb, &fa)) {
        if (fa < 1.0f) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        glColor4f(fr, fg, fb, fa);
        glBegin(GL_POLYGON);
        for (int i = 0; i < n; i++)
            glVertex2f(pts[i * 2], pts[i * 2 + 1]);
        glEnd();
    }

    if (sw <= 0) return;
    float sr, sg, sb, sa;
    if (!opengl_read_rgba(vm, sc_ptr, &sr, &sg, &sb, &sa)) return;

    float hw = (float)sw * 0.5f;

    if (hw <= 0.5f) {
        glColor4f(sr, sg, sb, sa);
        glLineWidth(s->backing_scale);
        glBegin(GL_LINE_LOOP);
        for (int i = 0; i < n; i++)
            glVertex2f(pts[i * 2], pts[i * 2 + 1]);
        glEnd();
        return;
    }

    glEnable(GL_STENCIL_TEST);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(0xFF);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDisable(GL_BLEND);

    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        opengl_draw_thick_line_round(pts[i*2], pts[i*2+1],
                                     pts[j*2], pts[j*2+1], hw);
    }

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilMask(0x00);

    glColor4f(sr, sg, sb, sa);
    glBegin(GL_QUADS);
    glVertex2i(0,        0);
    glVertex2i(s->width, 0);
    glVertex2i(s->width, s->height);
    glVertex2i(0,        s->height);
    glEnd();

    glStencilMask(0xFF);
    glDisable(GL_STENCIL_TEST);
}

/**
 * @brief Reads a 3-component float vector from VM memory.
 * @return true on success, false if the pointer is out of bounds.
 */
static bool read_vec3(RelicVM *vm, u32 ptr, float out[3]) {
    if (ptr + 12 > vm->mem_sz) { out[0] = out[1] = out[2] = 0; return false; }
    memcpy(out, vm->mem + ptr, 12);
    return true;
}

/**
 * @brief Switches to 3D render state (depth test, culling, alpha-test cut-out).
 */
/* ------------------------------------------------------------------ */
/* Embedded guest slices (gfx_guest_begin/end)                          */
/*                                                                      */
/* tigryxos runs several guest VMs on one OpenGLState: their drawing    */
/* shares this window's backbuffer under a per-slice transform+clip.    */
/* ops keep flipping Y with s->height (the desktop), so the guest's     */
/* logical surface is placed by a modelview translate+scale rather      */
/* than a private projection - the same math the host installed         */
/* manually before this hook existed.                                   */
/* ------------------------------------------------------------------ */

/* Device-pixel scale for glViewport/glScissor (stretch mode renders
 * 1:1 into the logical framebuffer, so no scaling applies there). */
static float opengl_dev_scale(const OpenGLState *s) {
    return s->stretch_enabled ? 1.0f
         : (s->backing_scale > 0.0f ? s->backing_scale : 1.0f);
}

/* Translate a guest-logical rect into host logical px (top-left). */
static void opengl_guest_rect(const OpenGLState *s,
                              i32 *x, i32 *y, i32 *w, i32 *h) {
    if (s->gst_lw <= 0 || s->gst_lh <= 0) return;
    *x = s->gst_x + (int)((i64)(*x) * s->gst_w / s->gst_lw);
    *y = s->gst_y + (int)((i64)(*y) * s->gst_h / s->gst_lh);
    *w = (int)((i64)(*w) * s->gst_w / s->gst_lw);
    *h = (int)((i64)(*h) * s->gst_h / s->gst_lh);
}

/* Intersect a host-logical rect with the guest's content rect, so a
 * guest-supplied @view/@viewport/@clip can never draw outside it. */
static void opengl_guest_clamp(const OpenGLState *s,
                               i32 *x, i32 *y, i32 *w, i32 *h) {
    int x1 = *x + *w, y1 = *y + *h;
    int cx1 = s->gst_x + s->gst_w, cy1 = s->gst_y + s->gst_h;
    if (*x < s->gst_x) *x = s->gst_x;
    if (*y < s->gst_y) *y = s->gst_y;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    *w = x1 - *x; if (*w < 0) *w = 0;
    *h = y1 - *y; if (*h < 0) *h = 0;
}

/* Install the guest's 2D transform + clip on top of the usual
 * full-window ortho. Re-asserted by opengl_ensure_2d so a guest may
 * freely mix 3D and 2D inside one frame without losing its mapping. */
static void opengl_guest_2d(OpenGLState *s) {
    float sx, sy, bs;
    int x0, y0, x1, y1;
    if (s->gst_w <= 0 || s->gst_h <= 0 ||
        s->gst_lw <= 0 || s->gst_lh <= 0) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 0, 0);
        return;
    }
    sx = (float)s->gst_w / (float)s->gst_lw;
    sy = (float)s->gst_h / (float)s->gst_lh;
    glViewport(0, 0, s->pixel_w, s->pixel_h);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, s->width, 0, s->height, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glTranslatef((float)s->gst_x,
                (1.0f - sy) * (float)(s->height - 1) - (float)s->gst_y,
                0.0f);
    glScalef(sx, sy, 1.0f);
    x0 = s->gst_x < 0 ? 0 : (s->gst_x > s->width ? s->width : s->gst_x);
    y0 = s->gst_y < 0 ? 0 : (s->gst_y > s->height ? s->height : s->gst_y);
    x1 = s->gst_x + s->gst_w; if (x1 < 0) x1 = 0; if (x1 > s->width)  x1 = s->width;
    y1 = s->gst_y + s->gst_h; if (y1 < 0) y1 = 0; if (y1 > s->height) y1 = s->height;
    bs = opengl_dev_scale(s);
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)(x0 * bs), (int)((s->height - y1) * bs),
              (int)((x1 - x0) * bs), (int)((y1 - y0) * bs));
}

static void opengl_guest_begin(RelicVM *vm, int x, int y, int w, int h,
                               int lw, int lh) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;

    s->sav_in_3d    = s->in_3d;
    s->sav_light_on = s->light_on;
    s->sav_view_x   = s->view_x;  s->sav_view_y = s->view_y;
    s->sav_view_w   = s->view_w;  s->sav_view_h = s->view_h;
    s->sav_vp_x     = s->vp_3d_x; s->sav_vp_y   = s->vp_3d_y;
    s->sav_vp_w     = s->vp_3d_w; s->sav_vp_h   = s->vp_3d_h;
    s->in_3d = false; s->light_on = false;
    s->view_x  = s->view_y  = s->view_w  = s->view_h  = 0;
    s->vp_3d_x = s->vp_3d_y = s->vp_3d_w = s->vp_3d_h = 0;

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION); glPushMatrix();
    glMatrixMode(GL_MODELVIEW);  glPushMatrix();

    s->gst_on = 1;
    s->gst_x = x; s->gst_y = y; s->gst_w = w; s->gst_h = h;
    s->gst_lw = lw; s->gst_lh = lh;
    opengl_guest_2d(s);
}

static void opengl_guest_end(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd || !s->gst_on) return;
    s->gst_on = 0;

    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glPopAttrib();

    s->in_3d    = s->sav_in_3d;
    s->light_on = s->sav_light_on;
    s->view_x  = s->sav_view_x; s->view_y  = s->sav_view_y;
    s->view_w  = s->sav_view_w; s->view_h  = s->sav_view_h;
    s->vp_3d_x = s->sav_vp_x;   s->vp_3d_y = s->sav_vp_y;
    s->vp_3d_w = s->sav_vp_w;   s->vp_3d_h = s->sav_vp_h;
}

/* Wipe depth inside the active slice's scissor - called once per guest
 * frame by hosts that share one depth buffer across slices (see
 * RelicBackend.gfx_guest_depth_clear). */
static void opengl_guest_depth_clear(RelicVM *vm) {
    (void)vm;
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
}

/* Retained guest frame (RelicBackend.gfx_guest_snap_*): the slice rect
 * copied out of the backbuffer into a POT texture, so a host can
 * re-show a guest's last complete frame in frames where it drew none. */
typedef struct {
    GLuint tex;
    int    pw, ph;     /* texture size (POT) */
    int    w, h;       /* captured device px */
} OpenGLGuestSnap;

/* Active slice rect clipped to the window, host logical px. */
static int opengl_guest_vis(const OpenGLState *s,
                            int *x0, int *y0, int *x1, int *y1) {
    if (!s->gst_on || s->gst_w <= 0 || s->gst_h <= 0) return 0;
    *x0 = s->gst_x < 0 ? 0 : s->gst_x;
    *y0 = s->gst_y < 0 ? 0 : s->gst_y;
    *x1 = s->gst_x + s->gst_w; if (*x1 > s->width)  *x1 = s->width;
    *y1 = s->gst_y + s->gst_h; if (*y1 > s->height) *y1 = s->height;
    return *x1 > *x0 && *y1 > *y0;
}

static void *opengl_guest_snap_capture(RelicVM *vm, void *snap) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    OpenGLGuestSnap *g = (OpenGLGuestSnap*)snap;
    int x0, y0, x1, y1, w, h, pw = 1, ph = 1;
    GLint prev = 0;
    float bs;
    if (!s || !s->hwnd || !opengl_guest_vis(s, &x0, &y0, &x1, &y1))
        return snap;
    bs = opengl_dev_scale(s);
    w = (int)((x1 - x0) * bs);
    h = (int)((y1 - y0) * bs);
    if (w < 1 || h < 1) return snap;
    while (pw < w) pw <<= 1;
    while (ph < h) ph <<= 1;
    if (!g) {
        g = (OpenGLGuestSnap*)calloc(1, sizeof *g);
        if (!g) return NULL;
    }
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    if (!g->tex || g->pw != pw || g->ph != ph) {
        if (g->tex) glDeleteTextures(1, &g->tex);
        glGenTextures(1, &g->tex);
        glBindTexture(GL_TEXTURE_2D, g->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, pw, ph, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        g->pw = pw; g->ph = ph;
    }
    glBindTexture(GL_TEXTURE_2D, g->tex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        (int)(x0 * bs), (int)((s->height - y1) * bs), w, h);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    g->w = w; g->h = h;
    return g;
}

/* Draws in host logical space over the slice rect. GL state is free to
 * trash here: guest_end pops the attrib/matrix stacks guest_begin
 * pushed, and the next guest_begin resets the backend's state flags. */
static void opengl_guest_snap_present(RelicVM *vm, void *snap) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    OpenGLGuestSnap *g = (OpenGLGuestSnap*)snap;
    int x0, y0, x1, y1;
    float u, v;
    if (!s || !s->hwnd || !g || !g->tex) return;
    if (!opengl_guest_vis(s, &x0, &y0, &x1, &y1)) return;
    u = (float)g->w / (float)g->pw;
    v = (float)g->h / (float)g->ph;
    glViewport(0, 0, s->pixel_w, s->pixel_h);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, s->width, 0, s->height, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g->tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f((float)x0, (float)(s->height - y1));
        glTexCoord2f(u,    0.0f); glVertex2f((float)x1, (float)(s->height - y1));
        glTexCoord2f(u,    v);    glVertex2f((float)x1, (float)(s->height - y0));
        glTexCoord2f(0.0f, v);    glVertex2f((float)x0, (float)(s->height - y0));
    glEnd();
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void opengl_guest_snap_free(void *snap) {
    OpenGLGuestSnap *g = (OpenGLGuestSnap*)snap;
    if (!g) return;
    if (g->tex) glDeleteTextures(1, &g->tex);
    free(g);
}

static void opengl_ensure_3d(OpenGLState *s) {
    if (s->in_3d) return;
    s->in_3d = true;
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);

    /* glScissor takes render-target (device) pixels with a bottom-left origin,
     * so the logical viewport rect is scaled by backing_scale. In stretch mode
     * the script renders 1:1 into the logical sub-rect, so no scaling applies. */
    /* @view redirects the projection surface: NDC maps into the view
     * rect (logical coords, top-left origin) rather than the window. */
    if (s->view_w > 0 && s->view_h > 0) {
        float vbs = opengl_dev_scale(s);
        int vgl_y = s->height - s->view_y - s->view_h;
        glViewport((int)(s->view_x * vbs), (int)(vgl_y * vbs),
                   (int)(s->view_w * vbs), (int)(s->view_h * vbs));
    } else if (s->gst_on) {
        /* Guest with no @view: the 3D projection maps into its content
         * rect, not the whole window. */
        float vbs = opengl_dev_scale(s);
        int vgl_y = s->height - s->gst_y - s->gst_h;
        glViewport((int)(s->gst_x * vbs), (int)(vgl_y * vbs),
                   (int)(s->gst_w * vbs), (int)(s->gst_h * vbs));
    }

    if (s->vp_3d_w > 0 && s->vp_3d_h > 0) {
        float bs = opengl_dev_scale(s);
        int gl_y = s->height - s->vp_3d_y - s->vp_3d_h;
        glEnable(GL_SCISSOR_TEST);
        glScissor((int)(s->vp_3d_x * bs), (int)(gl_y * bs),
                  (int)(s->vp_3d_w * bs), (int)(s->vp_3d_h * bs));
    } else if (s->gst_on) {
        /* Never drop the clip inside a guest slice: no @viewport means
         * the content rect itself is the bound. */
        float bs = opengl_dev_scale(s);
        int gl_y = s->height - s->gst_y - s->gst_h;
        glEnable(GL_SCISSOR_TEST);
        glScissor((int)(s->gst_x * bs), (int)(gl_y * bs),
                  (int)(s->gst_w * bs), (int)(s->gst_h * bs));
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

/**
 * @brief Implements @view: sets the logical render-surface rect, or
 *        resets it (w/h <= 0) back to the whole window.
 *
 * While set, the 3D projection (glViewport) maps into this rect; 2D
 * drawing is unaffected - embedded hosts translate guest coords at the
 * relvm layer. Applied immediately when mid-3D so a guest resize or a
 * first call after ensure_3d still takes effect this frame.
 */
static void opengl_gfx_view(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;

    i32 x = vm->r[1];
    i32 y = vm->r[2];
    i32 w = vm->r[3];
    i32 h = vm->r[4];

    if (w <= 0 || h <= 0) {
        s->view_x = s->view_y = s->view_w = s->view_h = 0;
        /* Disabling the view drops the retained guest frame too, so the
         * next run can't present a stale capture from the previous one. */
        s->view_has_frame = false;
        if (s->in_3d)
            glViewport(0, 0, s->pixel_w, s->pixel_h);
        return;
    }

    /* Inside a guest slice the coords are in the guest's own logical
     * surface - translate into host space and keep inside its content
     * rect. */
    if (s->gst_on) {
        opengl_guest_rect(s, &x, &y, &w, &h);
        opengl_guest_clamp(s, &x, &y, &w, &h);
        if (w <= 0 || h <= 0) return;
    }

    s->view_x = x; s->view_y = y; s->view_w = w; s->view_h = h;
    if (s->in_3d) {
        float bs = s->stretch_enabled ? 1.0f
                 : (s->backing_scale > 0.0f ? s->backing_scale : 1.0f);
        int gl_y = s->height - y - h;
        glViewport((int)(x * bs), (int)(gl_y * bs),
                   (int)(w * bs), (int)(h * bs));
    }
}

/**
 * @brief Implements @view_capture: snapshots the view rect of the
 *        framebuffer into the retained texture.
 *
 * Called at an embedded guest's @flip, while the backbuffer still holds
 * that guest frame - reading the framebuffer mid-frame (before
 * SwapBuffers) is valid here since both sides share the same host frame.
 * The texture is (re)allocated only when the view size changes.
 */
static void opengl_gfx_view_capture(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;

    i32 rx = vm->r[1];
    i32 ry = vm->r[2];
    i32 rw = vm->r[3];
    i32 rh = vm->r[4];
    if (rw <= 0 || rh <= 0) return;

    float bs = s->stretch_enabled ? 1.0f
             : (s->backing_scale > 0.0f ? s->backing_scale : 1.0f);
    int cw = (int)(rw * bs);
    int ch = (int)(rh * bs);
    if (cw < 1 || ch < 1) return;
    /* Never read past the framebuffer edge if the rect overhangs the
     * window (guest window larger than the panel). */
    int cx = (int)(rx * bs);
    int cy = (int)((s->height - ry - rh) * bs);
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx + cw > s->pixel_w) cw = s->pixel_w - cx;
    if (cy + ch > s->pixel_h) ch = s->pixel_h - cy;
    if (cw < 1 || ch < 1) return;

    int pot_w = next_power_of_2(cw);
    int pot_h = next_power_of_2(ch);
    if (!s->view_tex || pot_w != s->view_tex_w || pot_h != s->view_tex_h) {
        if (s->view_tex) glDeleteTextures(1, &s->view_tex);
        glGenTextures(1, &s->view_tex);
        glBindTexture(GL_TEXTURE_2D, s->view_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, pot_w, pot_h, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        s->view_tex_w = pot_w;
        s->view_tex_h = pot_h;
        s->view_has_frame = false;
    }

    glBindTexture(GL_TEXTURE_2D, s->view_tex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cx, cy, cw, ch);
    glBindTexture(GL_TEXTURE_2D, 0);
    s->view_uscale = (float)cw / (float)pot_w;
    s->view_vscale = (float)ch / (float)pot_h;
    s->view_has_frame = true;
}

/**
 * @brief Implements @view_present: draws the retained guest frame scaled
 *        into the given logical rect. No-op until the first capture, so
 *        a guest that has not flipped yet just shows what the host drew.
 */
static void opengl_gfx_view_present(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;
    if (!s->view_tex || !s->view_has_frame) return;

    i32 x = vm->r[1];
    i32 y = vm->r[2];
    i32 w = vm->r[3];
    i32 h = vm->r[4];
    if (w <= 0 || h <= 0) return;

    /* Remember the panel rect so wr_host_present can scale the 8bpp
     * wr_surf to fill it on the next frame (wr_host_present runs during
     * the guest's frame, before @view_present, so it uses the previous
     * frame's rect). */
    s->present_dst_x = x; s->present_dst_y = y;
    s->present_dst_w = w; s->present_dst_h = h;

    opengl_ensure_2d(s);

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, s->view_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    float u1 = s->view_uscale;
    float v1 = s->view_vscale;
    float x0 = (float)x,       y0 = (float)(s->height - (y + h));
    float x1 = (float)(x + w), y1 = (float)(s->height - y);
    glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(x0, y0);
        glTexCoord2f(u1,   0.0f); glVertex2f(x1, y0);
        glTexCoord2f(u1,   v1  ); glVertex2f(x1, y1);
        glTexCoord2f(0.0f, v1  ); glVertex2f(x0, y1);
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    s->frame_dirty = true;
}

/**
 * @brief Implements @viewport: sets the 3D scissor rect, or resets it.
 */
static void opengl_gfx_viewport(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;

    i32 x = vm->r[1];
    i32 y = vm->r[2];
    i32 w = vm->r[3];
    i32 h = vm->r[4];

    if (w <= 0 || h <= 0) {
        s->vp_3d_x = 0;
        s->vp_3d_y = 0;
        s->vp_3d_w = 0;
        s->vp_3d_h = 0;
        if (s->gst_on) {
            /* "no 3D viewport" inside a guest means its content rect. */
            float bs = opengl_dev_scale(s);
            int gl_y = s->height - s->gst_y - s->gst_h;
            glEnable(GL_SCISSOR_TEST);
            glScissor((int)(s->gst_x * bs), (int)(gl_y * bs),
                      (int)(s->gst_w * bs), (int)(s->gst_h * bs));
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        return;
    }

    /* Inside a guest slice, translate into host space and clamp to the
     * guest's content rect. */
    if (s->gst_on) {
        opengl_guest_rect(s, &x, &y, &w, &h);
        opengl_guest_clamp(s, &x, &y, &w, &h);
        if (w <= 0 || h <= 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, 0, 0);
            s->vp_3d_x = s->vp_3d_y = s->vp_3d_w = s->vp_3d_h = 0;
            return;
        }
    }

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > s->width)  w = s->width - x;
    if (y + h > s->height) h = s->height - y;

    s->vp_3d_x = x;
    s->vp_3d_y = y;
    s->vp_3d_w = w;
    s->vp_3d_h = h;

    /* Applied immediately, in device pixels (see opengl_ensure_3d). */
    float bs = s->stretch_enabled ? 1.0f
             : (s->backing_scale > 0.0f ? s->backing_scale : 1.0f);
    int gl_y = s->height - y - h;
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)(x * bs), (int)(gl_y * bs), (int)(w * bs), (int)(h * bs));
}

/**
 * @brief Implements @camera: builds the perspective and look-at matrices.
 *
 * A negative FOV selects a horizontally mirrored (left-handed) view.
 */
static void opengl_gfx_camera(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;

    u32 eye_ptr = (u32)vm->r[1];
    u32 tgt_ptr = (u32)vm->r[2];
    u32 up_ptr  = (u32)vm->r[3];
    RelicFBits fb;
    fb.i = vm->r[4]; float fov_deg = fb.f;
    fb.i = vm->r[5]; float znear   = fb.f;
    fb.i = vm->r[6]; float zfar    = fb.f;

    float eye[3], tgt[3], up[3];
    read_vec3(vm, eye_ptr, eye);
    read_vec3(vm, tgt_ptr, tgt);
    read_vec3(vm, up_ptr,  up);

    int cam_mirror = 0;
    if (fov_deg < 0.0f) { cam_mirror = 1; fov_deg = -fov_deg; }
    if (fov_deg <= 0.0f) fov_deg = 60.0f;
    if (znear   <= 0.0f) znear   = 0.1f;
    if (zfar    <= znear) zfar   = znear + 1000.0f;

    opengl_ensure_3d(s);

    /* Projection aspect follows the render surface: the @view rect when
     * set, else the guest's logical surface inside a guest slice (its
     * 3D viewport is the content rect - using the host window size here
     * squashed the scene vertically enough to pull a sky dome's
     * silhouette into view), otherwise the whole window. */
    float asp_w, asp_h;
    if (s->view_w > 0 && s->view_h > 0) {
        asp_w = (float)s->view_w;
        asp_h = (float)s->view_h;
        if (s->gst_on && s->gst_w > 0 && s->gst_h > 0) {
            /* view_* was translated into host px - recover the guest's
             * own logical aspect so 3D distorts with the 2D surface. */
            asp_w = asp_w * (float)s->gst_lw / (float)s->gst_w;
            asp_h = asp_h * (float)s->gst_lh / (float)s->gst_h;
        }
    } else if (s->gst_on) {
        asp_w = (float)s->gst_lw;
        asp_h = (float)s->gst_lh;
    } else {
        asp_w = (float)s->width;
        asp_h = (float)s->height;
    }
    float aspect = (asp_h > 0) ? asp_w / asp_h : 1.0f;
    float fov_rad = fov_deg * (3.14159265358979f / 180.0f);
    float f = 1.0f / tanf(fov_rad * 0.5f);
    float proj[16] = {0};
    proj[0]  = f / aspect;
    if (cam_mirror) proj[0] = -proj[0];
    proj[5]  = f;
    proj[10] = (zfar + znear) / (znear - zfar);
    proj[11] = -1.0f;
    proj[14] = (2.0f * zfar * znear) / (znear - zfar);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(proj);

    float fx = tgt[0]-eye[0], fy = tgt[1]-eye[1], fz = tgt[2]-eye[2];
    float fl = sqrtf(fx*fx + fy*fy + fz*fz);
    if (fl < 1e-6f) fl = 1.0f;
    fx/=fl; fy/=fl; fz/=fl;
    float sx = fy*up[2] - fz*up[1];
    float sy = fz*up[0] - fx*up[2];
    float sz = fx*up[1] - fy*up[0];
    float sl = sqrtf(sx*sx + sy*sy + sz*sz);
    if (sl < 1e-6f) sl = 1.0f;
    sx/=sl; sy/=sl; sz/=sl;
    float ux = sy*fz - sz*fy;
    float uy = sz*fx - sx*fz;
    float uz = sx*fy - sy*fx;
    float view[16] = {
        sx,  ux, -fx, 0,
        sy,  uy, -fy, 0,
        sz,  uz, -fz, 0,
        -(sx*eye[0] + sy*eye[1] + sz*eye[2]),
        -(ux*eye[0] + uy*eye[1] + uz*eye[2]),
          fx*eye[0] + fy*eye[1] + fz*eye[2],
        1
    };
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(view);

    glFrontFace(cam_mirror ? GL_CW : GL_CCW);
}

/**
 * @brief Magnification filter for script-supplied textures.
 *
 * Textures are addressed in logical pixels, so on a DPI-scaled display they
 * are magnified by the viewport. GL_NEAREST is kept whenever the scale factor
 * is a whole number (including the unscaled 1.0 case) so pixel art stays
 * pixel-exact; at fractional factors like 125% or 150% nearest sampling
 * duplicates some source rows and not others, which reads as a wobble, so
 * those fall back to GL_LINEAR.
 */
static GLint opengl_tex_mag_filter(const OpenGLState *s) {
    float bs = (s && !s->stretch_enabled && s->backing_scale > 0.0f)
             ? s->backing_scale : 1.0f;
    float frac = bs - (float)(int)bs;
    bool integral = (frac < 0.01f) || (frac > 0.99f);
    return integral ? GL_NEAREST : GL_LINEAR;
}

/**
 * @brief Re-applies the magnification filter to every live texture.
 *
 * Called when the scale factor changes (the window moved to a display with a
 * different DPI), since the right filter depends on it.
 */
/* Grow the texture slot table to hold at least `need` slots, doubling so
 * repeated uploads stay amortised O(1). New slots are zeroed and their
 * u/v scales seeded to 1.0 (the identity used by non-POT-padded
 * textures). Returns false only on allocation failure or when `need`
 * exceeds what a u16 slot id can address, leaving the old table intact
 * and usable - a partial grow is safe because tex_cap is not advanced
 * until every array has been resized. */
static bool opengl_tex_reserve(OpenGLState *s, int need) {
    if (need <= s->tex_cap) return true;
    if (need > RELIC_MAX_TEXTURES) return false;

    int cap = s->tex_cap ? s->tex_cap : RELIC_TEX_CAP_INIT;
    while (cap < need) {
        if (cap > RELIC_MAX_TEXTURES / 2) { cap = RELIC_MAX_TEXTURES; break; }
        cap *= 2;
    }

    GLuint *nt = (GLuint*)realloc(s->textures,    (size_t)cap * sizeof *nt);
    if (!nt) return false;
    s->textures = nt;
    int *nw = (int*)realloc(s->tex_width,         (size_t)cap * sizeof *nw);
    if (!nw) return false;
    s->tex_width = nw;
    int *nh = (int*)realloc(s->tex_height,        (size_t)cap * sizeof *nh);
    if (!nh) return false;
    s->tex_height = nh;
    float *nu = (float*)realloc(s->tex_u_scale,   (size_t)cap * sizeof *nu);
    if (!nu) return false;
    s->tex_u_scale = nu;
    float *nv = (float*)realloc(s->tex_v_scale,   (size_t)cap * sizeof *nv);
    if (!nv) return false;
    s->tex_v_scale = nv;

    for (int i = s->tex_cap; i < cap; i++) {
        s->textures[i]    = 0;
        s->tex_width[i]   = 0;
        s->tex_height[i]  = 0;
        s->tex_u_scale[i] = 1.0f;
        s->tex_v_scale[i] = 1.0f;
    }
    s->tex_cap = cap;
    return true;
}

static void opengl_refresh_tex_filters(const OpenGLState *s) {
    GLint mag = opengl_tex_mag_filter(s);
    for (int i = 1; i < s->tex_cap; i++) {
        if (!s->textures[i]) continue;
        glBindTexture(GL_TEXTURE_2D, s->textures[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

/**
 * @brief Implements @texture: uploads RGBA pixels and returns a texture slot.
 *
 * Pads non-power-of-two images to the next power of two on OpenGL 1.1.
 * @return The new texture slot in vm->r[1], or 0 on failure.
 */
static void opengl_gfx_texture(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) { vm->r[1] = 0; return; }

    u32 pixels_ptr = (u32)vm->r[1];
    i32 w = vm->r[2];
    i32 h = vm->r[3];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) { vm->r[1] = 0; return; }
    uint64_t need = (uint64_t)w * (uint64_t)h * 4ull;
    if ((uint64_t)pixels_ptr + need > (uint64_t)vm->mem_sz) { vm->r[1] = 0; return; }

    int slot = 0;
    for (int i = 1; i < s->tex_cap; i++) {
        if (s->textures[i] == 0) { slot = i; break; }
    }
    if (slot == 0) {
        /* Every slot is taken (or the table has not been allocated yet):
         * grow it and claim the first of the new slots. Only a genuine
         * out-of-memory or hitting the u16 slot-id ceiling fails here. */
        int prev = s->tex_cap;
        if (!opengl_tex_reserve(s, prev ? prev * 2 : RELIC_TEX_CAP_INIT)) {
            vm->r[1] = 0;
            return;
        }
        slot = prev < 1 ? 1 : prev;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) { vm->r[1] = 0; return; }
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (s->gl_version < 120) {
        int pot_w = next_power_of_2(w);
        int pot_h = next_power_of_2(h);

        if (pot_w != w || pot_h != h) {
            uint64_t padded_size = (uint64_t)pot_w * (uint64_t)pot_h * 4ull;
            u8 *padded_pixels = calloc(1, (size_t)padded_size);
            if (!padded_pixels) { glDeleteTextures(1, &tex); vm->r[1] = 0; return; }

            for (int row = 0; row < h; row++) {
                memcpy(padded_pixels + row * pot_w * 4,
                       vm->mem + pixels_ptr + row * w * 4,
                       (size_t)w * 4);
            }

            /* Replicate the last image column/row into the first column/row
             * of padding. @sprite samples up to exactly u = w/pot_w, which
             * sits on the boundary between image and padding, so GL_LINEAR
             * blends the edge texel with its neighbour in the pad. Leaving
             * that neighbour as calloc'd (0,0,0,0) mixes RGB *black* into
             * the edge - a dark hairline down the right side of every NPOT
             * sprite - because these are straight, not premultiplied, alpha
             * (a zero alpha does not imply a harmless zero RGB here). */
            if (pot_w > w) {
                for (int row = 0; row < h; row++) {
                    u8 *edge = padded_pixels + (size_t)row * pot_w * 4 + (size_t)(w - 1) * 4;
                    memcpy(edge + 4, edge, 4);
                }
            }
            if (pot_h > h) {
                int copy_w = (pot_w > w) ? w + 1 : w;
                memcpy(padded_pixels + (size_t)h * pot_w * 4,
                       padded_pixels + (size_t)(h - 1) * pot_w * 4,
                       (size_t)copy_w * 4);
            }

            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pot_w, pot_h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, padded_pixels);

            s->tex_u_scale[slot] = (float)w / (float)pot_w;
            s->tex_v_scale[slot] = (float)h / (float)pot_h;

            free(padded_pixels);
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, vm->mem + pixels_ptr);
            s->tex_u_scale[slot] = 1.0f;
            s->tex_v_scale[slot] = 1.0f;
        }
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, vm->mem + pixels_ptr);
        s->tex_u_scale[slot] = 1.0f;
        s->tex_v_scale[slot] = 1.0f;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, opengl_tex_mag_filter(s));
    /* GL_REPEAT is the right default here because @mesh binds these same
     * slots and tiles them with UVs > 1.0. @sprite, which always wants
     * clamping, overrides the wrap mode per-draw and restores it. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);

    s->textures[slot]   = tex;
    s->tex_width[slot]  = w;
    s->tex_height[slot] = h;
    vm->r[1] = (i32)slot;
}

/**
 * @brief Implements @light: configures a single directional light, or off.
 */
static void opengl_gfx_light(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;
    opengl_ensure_3d(s);

    u32 dir_ptr = (u32)vm->r[1];
    u32 rgb_ptr = (u32)vm->r[2];
    u32 amb_ptr = (u32)vm->r[3];

    if (dir_ptr == 0) {
        glDisable(GL_LIGHTING);
        glDisable(GL_LIGHT0);
        s->light_on = false;
        return;
    }

    float dir[3]; read_vec3(vm, dir_ptr, dir);
    float dl = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
    if (dl < 1e-6f) dl = 1.0f;
    GLfloat pos[4] = { dir[0]/dl, dir[1]/dl, dir[2]/dl, 0.0f };

    GLfloat diffuse[4] = {1,1,1,1}, ambient[4] = {0.1f,0.1f,0.1f,1};
    if (rgb_ptr + 3 <= vm->mem_sz) {
        diffuse[0] = vm->mem[rgb_ptr]   / 255.0f;
        diffuse[1] = vm->mem[rgb_ptr+1] / 255.0f;
        diffuse[2] = vm->mem[rgb_ptr+2] / 255.0f;
    }
    if (amb_ptr + 3 <= vm->mem_sz) {
        ambient[0] = vm->mem[amb_ptr]   / 255.0f;
        ambient[1] = vm->mem[amb_ptr+1] / 255.0f;
        ambient[2] = vm->mem[amb_ptr+2] / 255.0f;
    }

    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE,  diffuse);
    glLightfv(GL_LIGHT0, GL_AMBIENT,  ambient);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
    s->light_on = true;
}

static void opengl_gfx_fog(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;
    opengl_ensure_3d(s);

    i32 mode = (i32)vm->r[1];
    if (mode == 0) {
        glDisable(GL_FOG);
        return;
    }
    RelicFBits fs, fe;
    fs.i = (i32)vm->r[2];
    fe.i = (i32)vm->r[3];
    float fog_start = fs.f;
    float fog_end   = fe.f;
    u32 col_ptr = (u32)vm->r[4];

    GLfloat fog_color[4] = {0, 0, 0, 1};
    if (col_ptr && col_ptr + 4 <= vm->mem_sz) {
        fog_color[0] = vm->mem[col_ptr]   / 255.0f;
        fog_color[1] = vm->mem[col_ptr+1] / 255.0f;
        fog_color[2] = vm->mem[col_ptr+2] / 255.0f;
        fog_color[3] = vm->mem[col_ptr+3] / 255.0f;
    }

    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, (mode == 2) ? GL_EXP : (mode == 3) ? GL_EXP2 : GL_LINEAR);
    if (mode >= 2) {
        // For EXP/EXP2 modes, `near` (fog_start) is the density; START/END
        // are ignored by GL for those modes.
        glFogf(GL_FOG_DENSITY, fog_start);
    }
    glFogf(GL_FOG_START, fog_start);
    glFogf(GL_FOG_END, fog_end);
    glFogfv(GL_FOG_COLOR, fog_color);
    glHint(GL_FOG_HINT, GL_FASTEST);
}

/**
 * @brief Copies a NUL-terminated VM string into a host buffer.
 * @return The number of bytes copied, excluding the terminator.
 */
static int read_vm_cstr(RelicVM *vm, u32 ptr, char *out, int out_sz) {
    int n = 0;
    if (out_sz <= 0) return 0;
    while (ptr < vm->mem_sz && n < out_sz - 1) {
        u8 c = vm->mem[ptr++];
        if (c == 0) break;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return n;
}

/**
 * @brief Device pixels per logical pixel for GDI glyph rasterisation.
 *
 * GDI rasterises at whatever size it is handed, so @text asks it for the
 * device size and then draws the result into a logical-sized quad: glyphs end
 * up sampled 1:1 with the display and stay sharp at any DPI, which is the
 * whole reason text goes through GDI rather than a fixed atlas. Stretch mode
 * renders 1:1 into a fixed logical buffer, so it always rasterises at the
 * logical size.
 */
static float opengl_text_scale(const OpenGLState *s) {
    if (!s || s->stretch_enabled) return 1.0f;
    return (s->backing_scale > 0.0f) ? s->backing_scale : 1.0f;
}

/* FNV-1a over the string, mixed with the scalar key parts. Zero is the
 * empty-slot sentinel, so any real hash is forced nonzero. */
static u32 text_key_hash(const char *s, int face_idx, int ps_dev, int use_ct) {
    u32 h = 2166136261u;
    while (*s) { h ^= (u8)*s++; h *= 16777619u; }
    h ^= (u32)face_idx * 0x9E3779B1u + (u32)ps_dev * 4u + (u32)use_ct;
    h *= 16777619u;
    return h ? h : 1u;
}

/* Looks the key up; on a miss returns a fresh slot with tex==0 for the
 * caller to rasterise into, NULL if the cache could not be allocated.
 * Eviction picks the globally oldest entry. Removing a live entry leaves
 * an empty slot mid-chain, which can strand entries that probed past it;
 * the result is only a false miss (that key re-rasterises and re-inserts
 * earlier in its chain) - never wrong data, so no tombstones. */
static TextCacheEntry *text_cache_get(OpenGLState *s, const char *text,
        int face_idx, int ps_dev, int use_ct) {
    if (!s->text_cache) {
        s->text_cache = (TextCacheEntry*)calloc(TEXT_CACHE_SIZE,
                                                sizeof(TextCacheEntry));
        if (!s->text_cache) return NULL;
    }
    u32 hash = text_key_hash(text, face_idx, ps_dev, use_ct);
    u32 base = hash & (TEXT_CACHE_SIZE - 1);
    TextCacheEntry *empty = NULL;
    for (int n = 0; n < TEXT_CACHE_PROBE; n++) {
        TextCacheEntry *e =
            &s->text_cache[(base + (u32)n) & (TEXT_CACHE_SIZE - 1)];
        if (e->hash == hash && e->face_idx == face_idx &&
            e->ps_dev == ps_dev && e->use_ct == use_ct &&
            strcmp(e->text, text) == 0) {
            e->last_used = ++s->text_cache_tick;
            return e;
        }
        if (!e->hash && !empty) empty = e;
    }
    TextCacheEntry *e = empty;
    if (!e) {
        u32 oldest = 0xFFFFFFFFu;
        int oi = (int)base;
        for (int k = 0; k < TEXT_CACHE_SIZE; k++) {
            if (s->text_cache[k].last_used < oldest) {
                oldest = s->text_cache[k].last_used;
                oi = k;
            }
        }
        e = &s->text_cache[oi];
        if (e->tex) glDeleteTextures(1, &e->tex);
        free(e->text);
    }
    memset(e, 0, sizeof(*e));
    e->text = (char*)malloc(strlen(text) + 1);
    if (!e->text) return NULL;
    strcpy(e->text, text);
    e->hash     = hash;
    e->face_idx = face_idx;
    e->ps_dev   = ps_dev;
    e->use_ct   = use_ct;
    e->last_used = ++s->text_cache_tick;
    return e;
}

/* Drops a slot text_cache_get just reserved when rasterisation then fails.
 * Safe because the slot was empty before the reservation, so clearing it
 * restores the probe topology exactly. */
static void text_cache_release(OpenGLState *s, TextCacheEntry *e) {
    if (!s || !e) return;
    free(e->text);
    memset(e, 0, sizeof(*e));
}

/* Deletes every cached texture and frees the table. Called from
 * opengl_font_shutdown while the GL context is still current. */
static void text_cache_flush(OpenGLState *s) {
    if (!s || !s->text_cache) return;
    for (int i = 0; i < TEXT_CACHE_SIZE; i++) {
        TextCacheEntry *e = &s->text_cache[i];
        if (e->tex) glDeleteTextures(1, &e->tex);
        free(e->text);
    }
    free(s->text_cache);
    s->text_cache = NULL;
    s->text_cache_tick = 0;
}

/**
 * @brief Implements @text: draws justified, scaled TrueType text.
 */
/* @text_measure(text, size, weight, font_name, out)
 *   Measures text without drawing and writes a TextMetrics struct (5 i32):
 *   +0 width, +4 height, +8 ascent, +12 descent, +16 line_h (pixels). */
static void opengl_gfx_text_measure(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;

    u32 text_ptr  = (u32)vm->r[1];
    i32 size      = vm->r[2];
    i32 weight    = vm->r[3];
    u32 fname_ptr = (u32)vm->r[4];
    u32 out_ptr   = (u32)vm->r[5];

    if (out_ptr + 20u > vm->mem_sz) return;

    i32 width = 0, height = 0, ascent = 0, descent = 0, line_h = 0;

    /* Measured at the device size that @text will rasterise, then divided back
     * down, so the numbers a script lays out with match what gets drawn. */
    float sc = opengl_text_scale(s);

    if (s && s->nfaces > 0 && text_ptr < vm->mem_sz) {
        char text_buf[1024];
        read_vm_cstr(vm, text_ptr, text_buf, sizeof(text_buf));
        char fname_buf[64] = "";
        if (fname_ptr && fname_ptr < vm->mem_sz) {
            read_vm_cstr(vm, fname_ptr, fname_buf, sizeof(fname_buf));
        }

        int face_idx = select_face(s, fname_buf, weight);
        if (face_idx >= 0 && s->faces[face_idx].init) {
            int ps = (size > 0) ? size : 16;
            int ps_dev = (int)((float)ps * sc + 0.5f);
            if (ps_dev < 1) ps_dev = 1;
            int fw = (weight >= 600) ? FW_BOLD : FW_NORMAL;
            DWORD quality = ANTIALIASED_QUALITY;
            DWORD out_precis = s->faces[face_idx].is_system
                ? OUT_DEFAULT_PRECIS : OUT_TT_ONLY_PRECIS;
            HFONT hf = CreateFontA(-ps_dev, 0, 0, 0, fw, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, out_precis,
                                   CLIP_DEFAULT_PRECIS, quality,
                                   DEFAULT_PITCH | FF_DONTCARE,
                                   s->faces[face_idx].family);
            if (hf) {
                HDC hdc = GetDC(s->hwnd);
                HDC mem_dc = CreateCompatibleDC(hdc);
                ReleaseDC(s->hwnd, hdc);
                if (mem_dc) {
                    HFONT old = (HFONT)SelectObject(mem_dc, hf);
                    wchar_t wbuf[1024];
#ifdef RELIC_WIN95
                    int wlen = utf8_to_wide(text_buf, wbuf, 1024);
#else
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, text_buf, -1, wbuf, 1024) - 1;
                    if (wlen < 0) wlen = 0;
#endif
                    SIZE ts;
                    if (GetTextExtentPoint32W(mem_dc, wbuf, wlen, &ts) && ts.cx > 0 && ts.cy > 0) {
                        width  = ts.cx;
                        TEXTMETRICA tm;
                        if (GetTextMetricsA(mem_dc, &tm)) {
                            ascent  = tm.tmAscent;
                            descent = tm.tmDescent;
                            line_h  = tm.tmHeight + tm.tmExternalLeading;
                        }

                        /* Render text into a DIB and scan for the actual
                         * visible glyph rows to get a tight ink height. */
                        int dib_w = ts.cx;
                        int dib_h = ts.cy;
                        BITMAPINFO bmi;
                        memset(&bmi, 0, sizeof(bmi));
                        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                        bmi.bmiHeader.biWidth       = dib_w;
                        bmi.bmiHeader.biHeight      = -(LONG)dib_h;
                        bmi.bmiHeader.biPlanes      = 1;
                        bmi.bmiHeader.biBitCount    = 32;
                        bmi.bmiHeader.biCompression = BI_RGB;

                        void *bits = NULL;
                        HBITMAP dib = CreateDIBSection(mem_dc, &bmi,
                                                        DIB_RGB_COLORS,
                                                        &bits, NULL, 0);
                        if (dib && bits) {
                            HBITMAP old_bmp = (HBITMAP)SelectObject(mem_dc, dib);
                            memset(bits, 0, (size_t)dib_w * (size_t)dib_h * 4);
                            SetTextColor(mem_dc, RGB(255, 255, 255));
                            SetBkMode(mem_dc, TRANSPARENT);
                            TextOutW(mem_dc, 0, 0, wbuf, wlen);
                            GdiFlush();

                            unsigned char *src = (unsigned char*)bits;
                            int glyph_top = 0, glyph_bottom = dib_h - 1;
                            int found = 0;
                            for (glyph_top = 0; glyph_top < dib_h; glyph_top++) {
                                for (int gx = 0; gx < dib_w; gx++) {
                                    if (src[(glyph_top * dib_w + gx) * 4 + 2] > 0) {
                                        found = 1; break;
                                    }
                                }
                                if (found) break;
                            }
                            if (found) {
                                found = 0;
                                for (glyph_bottom = dib_h - 1; glyph_bottom >= 0; glyph_bottom--) {
                                    for (int gx = 0; gx < dib_w; gx++) {
                                        if (src[(glyph_bottom * dib_w + gx) * 4 + 2] > 0) {
                                            found = 1; break;
                                        }
                                    }
                                    if (found) break;
                                }
                                height = glyph_bottom - glyph_top + 1;
                            } else {
                                height = ts.cy;
                            }

                            SelectObject(mem_dc, old_bmp);
                            DeleteObject(dib);
                        } else {
                            height = ts.cy;
                        }
                    }
                    SelectObject(mem_dc, old);
                    DeleteDC(mem_dc);
                }
                DeleteObject(hf);
            }
        }
    }

    if (sc != 1.0f) {
        width   = (i32)((float)width   / sc + 0.5f);
        height  = (i32)((float)height  / sc + 0.5f);
        ascent  = (i32)((float)ascent  / sc + 0.5f);
        descent = (i32)((float)descent / sc + 0.5f);
        line_h  = (i32)((float)line_h  / sc + 0.5f);
    }

    memcpy(vm->mem + out_ptr + 0,  &width,   4);
    memcpy(vm->mem + out_ptr + 4,  &height,  4);
    memcpy(vm->mem + out_ptr + 8,  &ascent,  4);
    memcpy(vm->mem + out_ptr + 12, &descent, 4);
    memcpy(vm->mem + out_ptr + 16, &line_h,  4);
}

/* Render text with GDI at the exact requested size and upload the result
 * as a 1:1 texture. The bitmap fields of `out` are filled with everything
 * the draw side needs: tex, dib_w/tex_h = ink box in device pixels,
 * ink_x/ink_y = ink offset inside the font cell, cell_w/cell_h = the cell
 * itself, u_max/v_max = used sub-rect after any power-of-two padding.
 * The caller owns out->tex. Returns 1 on success, 0 on failure. */
static int text_rasterise_gdi(OpenGLState *s, int face_idx,
        const wchar_t *wbuf, int wlen, int ps_dev, int fw, int use_ct,
        TextCacheEntry *out) {
    HDC hdc = GetDC(s->hwnd);
    HDC mem_dc = CreateCompatibleDC(hdc);
    ReleaseDC(s->hwnd, hdc);
    if (!mem_dc) return 0;

    DWORD quality = use_ct ? CLEARTYPE_QUALITY : ANTIALIASED_QUALITY;
    /* System fonts (e.g. MS Sans Serif) are raster fonts that GDI provides
     * natively. OUT_DEFAULT_PRECIS lets GDI use the raster face directly;
     * OUT_TT_ONLY_PRECIS would force it to find a TrueType substitute, which
     * defeats the purpose. For bundled TrueType fonts, OUT_TT_ONLY_PRECIS
     * keeps the mapper away from the raster faces it otherwise falls back to
     * on XP, which stretch into solid blocks at these pixel sizes. */
    DWORD out_precis = s->faces[face_idx].is_system
        ? OUT_DEFAULT_PRECIS : OUT_TT_ONLY_PRECIS;
    HFONT hf = CreateFontA(-ps_dev, 0, 0, 0, fw, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, out_precis,
                           CLIP_DEFAULT_PRECIS, quality,
                           DEFAULT_PITCH | FF_DONTCARE,
                           s->faces[face_idx].family);
    if (!hf) { DeleteDC(mem_dc); return 0; }

    HFONT old_font = (HFONT)SelectObject(mem_dc, hf);

    /* CreateFontA never fails on an unknown family - it quietly hands back
     * whatever the font mapper considered closest. Ask the DC what actually
     * got selected so a substitution shows up in the log instead of only as
     * unreadable blocks on screen. Warned about once per face. */
    if (g_vm && g_vm->verbose && face_idx < FONT_MAX_FACES) {
        static bool warned[FONT_MAX_FACES];
        char actual[LF_FACESIZE] = "";
        if (!warned[face_idx] &&
            GetTextFaceA(mem_dc, (int)sizeof(actual), actual) > 0 &&
            !ci_streq(actual, s->faces[face_idx].family)) {
            warned[face_idx] = true;
            fprintf(stderr, "opengl-win32: GDI substituted '%s' for requested "
                            "family '%s' - text will not render correctly\n",
                    actual, s->faces[face_idx].family);
        }
    }

    SIZE ts;
    if (!GetTextExtentPoint32W(mem_dc, wbuf, wlen, &ts) || ts.cx <= 0 || ts.cy <= 0) {
        SelectObject(mem_dc, old_font);
        DeleteObject(hf);
        DeleteDC(mem_dc);
        return 0;
    }

    int dib_w = ts.cx;
    int dib_h = ts.cy;

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = dib_w;
    bmi.bmiHeader.biHeight = -dib_h; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP dib = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib || !bits) {
        SelectObject(mem_dc, old_font);
        DeleteObject(hf);
        DeleteDC(mem_dc);
        return 0;
    }

    HBITMAP old_bmp = (HBITMAP)SelectObject(mem_dc, dib);

    /* Clear to black and draw white text; we extract the red channel as alpha. */
    memset(bits, 0, (size_t)dib_w * (size_t)dib_h * 4);
    SetTextColor(mem_dc, RGB(255, 255, 255));
    SetBkMode(mem_dc, TRANSPARENT);
    TextOutW(mem_dc, 0, 0, wbuf, wlen);
    GdiFlush();

    /* bpp=1: single coverage channel for SRC_ALPHA blending.
     * bpp=4: RGB per-subpixel coverage for the ClearType blend. */
    int bpp = use_ct ? 4 : 1;
    unsigned char *alpha = (unsigned char*)malloc((size_t)dib_w * (size_t)dib_h * bpp);
    if (!alpha) {
        SelectObject(mem_dc, old_bmp);
        SelectObject(mem_dc, old_font);
        DeleteObject(dib);
        DeleteObject(hf);
        DeleteDC(mem_dc);
        return 0;
    }
    {
        unsigned char *src = (unsigned char*)bits;
        for (int i = 0; i < dib_w * dib_h; i++) {
            if (use_ct) {
                /* DIB is BGRA; texture wants RGB coverage. */
                alpha[i * 4 + 0] = src[i * 4 + 2];
                alpha[i * 4 + 1] = src[i * 4 + 1];
                alpha[i * 4 + 2] = src[i * 4 + 0];
                alpha[i * 4 + 3] = 255;
            } else {
                alpha[i] = src[i * 4 + 2]; /* R in BGRA */
            }
        }
    }
    /* A lit pixel is any coverage above zero; for ClearType the fringes can
     * light only one or two of the three channels, so test all of them. */
#define TEXT_PX_LIT(i) (alpha[(i) * bpp] > 0 || \
                        (bpp == 4 && (alpha[(i) * bpp + 1] > 0 || \
                                      alpha[(i) * bpp + 2] > 0)))

    /* Find the visible glyph extents (tight ink bounds) and trim the
     * alpha buffer so the rendered quad matches the actual glyph ink box
     * rather than the full font cell. This makes @text rendering
     * consistent with @text_measure on all platforms and fixes off-centre
     * icon glyphs (e.g. Material Icons) whose visible shape sits on the
     * left side of the GDI cell. */
    int glyph_top = 0, glyph_bottom = dib_h - 1;
    for (glyph_top = 0; glyph_top < dib_h; glyph_top++) {
        int row_visible = 0;
        for (int gx = 0; gx < dib_w; gx++) {
            if (TEXT_PX_LIT(glyph_top * dib_w + gx)) { row_visible = 1; break; }
        }
        if (row_visible) break;
    }
    for (glyph_bottom = dib_h - 1; glyph_bottom >= 0; glyph_bottom--) {
        int row_visible = 0;
        for (int gx = 0; gx < dib_w; gx++) {
            if (TEXT_PX_LIT(glyph_bottom * dib_w + gx)) { row_visible = 1; break; }
        }
        if (row_visible) break;
    }
    int any_visible = (glyph_top <= glyph_bottom);

    int glyph_left = 0, glyph_right = dib_w - 1;
    for (glyph_left = 0; glyph_left < dib_w; glyph_left++) {
        int col_visible = 0;
        for (int gy = 0; gy < dib_h; gy++) {
            if (TEXT_PX_LIT(gy * dib_w + glyph_left)) { col_visible = 1; break; }
        }
        if (col_visible) break;
    }
    for (glyph_right = dib_w - 1; glyph_right >= 0; glyph_right--) {
        int col_visible = 0;
        for (int gy = 0; gy < dib_h; gy++) {
            if (TEXT_PX_LIT(gy * dib_w + glyph_right)) { col_visible = 1; break; }
        }
        if (col_visible) break;
    }
#undef TEXT_PX_LIT

    int ink_h = any_visible ? (glyph_bottom - glyph_top + 1) : dib_h;
    int ink_y = any_visible ? glyph_top : 0;
    int tex_w = (glyph_left <= glyph_right)
              ? (glyph_right - glyph_left + 1) : dib_w;
    int ink_x = (glyph_left <= glyph_right) ? glyph_left : 0;

    /* Build tightly-trimmed alpha (tex_w x ink_h) from the visible ink. */
    unsigned char *alpha_trim = (unsigned char*)malloc((size_t)tex_w * (size_t)ink_h * bpp);
    if (alpha_trim) {
        for (int row = 0; row < ink_h; row++) {
            memcpy(alpha_trim + (size_t)row * tex_w * bpp,
                   alpha + ((size_t)(ink_y + row) * dib_w + ink_x) * bpp,
                   (size_t)tex_w * bpp);
        }
        free(alpha);
        alpha = alpha_trim;
    }
    int tex_h = ink_h;
    int cell_w = dib_w;   /* advance width  (what @text_measure reports) */
    int cell_h = dib_h;   /* tmHeight: ascent + descent, content-independent */
    dib_w = tex_w;   /* The rendered quad is now the tight ink width. */

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    /* This texture is sized to the string's ink box, so it is almost never a
     * power of two. OpenGL 1.1 - which is what the generic Microsoft software
     * renderer on Windows XP reports - rejects such a texture outright, and
     * sampling the resulting incomplete texture behaves as though texturing
     * were disabled: the quad below then fills solid in the current colour,
     * i.e. a text-coloured block the exact size of the string.
     *
     * Pad up to a power of two and sample only the used sub-rectangle, the
     * same way @image does above. Gated on the version so modern drivers skip
     * the extra allocation and upload entirely. Glyph pixels stay 1:1 either
     * way, so this costs nothing in text quality. */
    float u_max = 1.0f, v_max = 1.0f;
    unsigned char *upload = alpha;
    unsigned char *padded = NULL;
    int up_w = dib_w, up_h = tex_h;

    if (s->gl_version < 120 || (use_ct && s->gl_version < 200)) {
        int pot_w = next_power_of_2(dib_w);
        int pot_h = next_power_of_2(tex_h);
        if (pot_w != dib_w || pot_h != tex_h) {
            padded = (unsigned char*)calloc(1, (size_t)pot_w * (size_t)pot_h * bpp);
            if (padded) {
                for (int row = 0; row < tex_h; row++) {
                    memcpy(padded + (size_t)row * pot_w * bpp,
                           alpha + (size_t)row * dib_w * bpp,
                           (size_t)dib_w * bpp);
                }
                upload = padded;
                up_w   = pot_w;
                up_h   = pot_h;
                u_max  = (float)dib_w / (float)pot_w;
                v_max  = (float)tex_h / (float)pot_h;
            }
        }
    }

    GLenum tex_fmt = use_ct ? GL_RGBA : GL_ALPHA;
    glTexImage2D(GL_TEXTURE_2D, 0, tex_fmt, up_w, up_h, 0, tex_fmt, GL_UNSIGNED_BYTE, upload);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(padded);
    free(alpha);

    /* The texture is the only product the draw side needs; every GDI
     * object is released here. */
    SelectObject(mem_dc, old_bmp);
    SelectObject(mem_dc, old_font);
    DeleteObject(dib);
    DeleteObject(hf);
    DeleteDC(mem_dc);

    out->tex    = tex;
    out->dib_w  = dib_w;   out->tex_h  = tex_h;
    out->ink_x  = ink_x;   out->ink_y  = ink_y;
    out->cell_w = cell_w;  out->cell_h = cell_h;
    out->u_max  = u_max;   out->v_max  = v_max;
    return 1;
}

/* Draw a string with explicit args. Shared by @text and draw-list
 * DL_TEXT records; position is in logical top-left coords and goes
 * through the same anchoring/ink-box rules documented below. */
static void opengl_text_draw(RelicVM *vm, u32 text_ptr, i32 x, i32 y,
                             i32 size, i32 weight, u32 fname_ptr,
                             i32 justify, int cr, int cg, int cb, int ca) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;
    s->frame_dirty = true;

    if (s->nfaces <= 0) return;
    if (text_ptr >= vm->mem_sz) return;

    char text_buf[1024];
    read_vm_cstr(vm, text_ptr, text_buf, sizeof(text_buf));
    char fname_buf[64] = "";
    if (fname_ptr && fname_ptr < vm->mem_sz) {
        read_vm_cstr(vm, fname_ptr, fname_buf, sizeof(fname_buf));
    }

    int face_idx = select_face(s, fname_buf, weight);
    if (face_idx < 0 || !s->faces[face_idx].init) return;

    wchar_t wbuf[1024];
#ifdef RELIC_WIN95
    int wlen = utf8_to_wide(text_buf, wbuf, 1024);
#else
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text_buf, -1, wbuf, 1024) - 1;
    if (wlen < 0) wlen = 0;
#endif
    if (wlen == 0) return;

    /* size is logical; GDI rasterises at the device size so the glyph bitmap
     * lands 1:1 on the display (see opengl_text_scale). */
    float sc = opengl_text_scale(s);
    int ps = (size > 0) ? size : 16;
    int ps_dev = (int)((float)ps * sc + 0.5f);
    if (ps_dev < 1) ps_dev = 1;
    int fw = (weight >= 600) ? FW_BOLD : FW_NORMAL;

    /* ClearType (subpixel AA) when we can composite it: GDI writes
     * per-channel coverage into the DIB, the texture keeps that RGB mask,
     * and the quad is blended as cov*textCol + dst*(1-cov) - the exact
     * ClearType composite, so text matches native Windows rendering.
     * Translucent text keeps the grayscale path (ClearType coverage has no
     * alpha to attenuate), as does anything below GL 1.4/2.0 (glBlendColor
     * and NPOT textures respectively) and Win9x, which has no ClearType. */
    int use_ct = 0;
#ifndef RELIC_WIN95
    /* Stretch mode blits the logical buffer to the window afterwards, which
     * would scale the subpixel fringes into colour smears, so stay gray. */
    if (g_glBlendColor && s->gl_version >= 200 && ca == 255 &&
        !s->stretch_enabled) use_ct = 1;
#endif

    /* A hit supplies the rasterised result; a miss rasterises into the
     * reserved slot. NULL falls back to a scratch entry, which degrades to
     * the old rasterise-draw-delete path. */
    TextCacheEntry local;
    TextCacheEntry *ce =
        text_cache_get(s, text_buf, face_idx, ps_dev, use_ct);
    int cached = (ce != NULL);
    if (!ce) { memset(&local, 0, sizeof(local)); ce = &local; }

    if (!ce->tex &&
        !text_rasterise_gdi(s, face_idx, wbuf, wlen, ps_dev, fw, use_ct,
                            ce)) {
        if (cached) text_cache_release(s, ce);
        return;
    }

    GLuint tex  = ce->tex;
    int dib_w   = ce->dib_w,   tex_h  = ce->tex_h;
    int ink_x   = ce->ink_x,   ink_y  = ce->ink_y;
    int cell_w  = ce->cell_w,  cell_h = ce->cell_h;
    float u_max = ce->u_max,   v_max  = ce->v_max;

    int j = (justify >= 0 && justify <= 8) ? justify : 0;
    int h_mode = (j <= 2) ? 0 : (j <= 5 ? 1 : 2);
    int v_mode;
    switch (j) {
        case 0: case 3: case 6: v_mode = 0; break;
        case 1: case 4: case 7: v_mode = 1; break;
        case 2: case 5: case 8: v_mode = 2; break;
        default: v_mode = 0;
    }

    /* dib_w and tex_h are the ink box in device pixels, so the anchor is
     * converted to device pixels and the quad is positioned there. Snapping to
     * whole device pixels is what keeps the GL_NEAREST sampling exactly 1:1
     * with the glyph bitmap at fractional scale factors such as 125% or 150%. */
    int xp = (int)((float)x * sc + (x >= 0 ? 0.5f : -0.5f));
    int yp = (int)((float)y * sc + (y >= 0 ? 0.5f : -0.5f));

    /* Two different anchor conventions are in play here, and using the
     * wrong one is what made multi-run text wobble:
     *
     *   - The ink box (dib_w x tex_h) is what you want when centring a
     *     lone symbol in a box - notably Material Icons glyphs, whose
     *     visible shape sits off-centre in the GDI cell. The CENTRE
     *     modes keep using it, so icon placement is unchanged.
     *
     *   - The font cell (cell_w x cell_h, plus the ink's offset inside
     *     it) is what you want for text, because it does not depend on
     *     which glyphs the string happens to contain. Anchoring text by
     *     its ink box gives every run its own baseline: "." is drawn at
     *     ascender height and "_" floats up, since neither has an
     *     ascender to push its ink box down. That is fatal for anything
     *     that draws a line in several runs (syntax highlighting), so
     *     the TOP/BOTTOM modes - which are defined as "text sits
     *     above/below y" - anchor the cell and then add the ink offset.
     *     Left alignment likewise starts the run at the pen position,
     *     matching the advance widths @text_measure reports. */
    int draw_x, draw_y;
    if (h_mode == 0)      draw_x = xp - dib_w / 2;      /* ink-centred   */
    else if (h_mode == 1) draw_x = xp + ink_x;          /* pen position  */
    else                  draw_x = xp - dib_w;          /* ink right     */

    if (v_mode == 0)      draw_y = yp - tex_h / 2;          /* ink-centred */
    else if (v_mode == 1) draw_y = yp - cell_h + ink_y;     /* above y     */
    else                  draw_y = yp + ink_y;              /* below y     */
    (void)cell_w;

    opengl_ensure_2d(s);
    glEnable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (use_ct) {
        /* Per-subpixel ClearType composite:
         *   out.rgb = cov.rgb * textCol + dst.rgb * (1 - cov.rgb)
         * GL_CONSTANT_COLOR multiplies the coverage texture by the text
         * colour; ONE_MINUS_SRC_COLOR applies (1-cov) to the background.
         * glColor must stay white so modulate passes coverage through. */
        g_glBlendColor(cr / 255.0f, cg / 255.0f, cb / 255.0f, 1.0f);
        glBlendFunc(GL_CONSTANT_COLOR, GL_ONE_MINUS_SRC_COLOR);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    } else {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(cr / 255.0f, cg / 255.0f, cb / 255.0f, ca / 255.0f);
    }

    /* Back to logical coordinates: that is what the ortho projection expects.
     * At 96 DPI sc is 1.0 and these are the device pixels unchanged. */
    float sx = (float)draw_x / sc, ex = sx + (float)dib_w / sc;
    float sy = (float)draw_y / sc, ey = sy + (float)tex_h / sc;
    float win_h = (float)s->height;
    float y0 = win_h - sy; /* top    in OpenGL coords */
    float y1 = win_h - ey; /* bottom in OpenGL coords */

    glBegin(GL_QUADS);
    glTexCoord2f(0.0f,  0.0f);  glVertex2f(sx, y0);
    glTexCoord2f(u_max, 0.0f);  glVertex2f(ex, y0);
    glTexCoord2f(u_max, v_max); glVertex2f(ex, y1);
    glTexCoord2f(0.0f,  v_max); glVertex2f(sx, y1);
    glEnd();

    if (use_ct) glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);

    /* A cached texture lives on for the next hit; only an uncached one
     * (scratch entry) is thrown away as before. */
    if (!cached) glDeleteTextures(1, &tex);
}

static void opengl_gfx_text_gdi(RelicVM *vm) {
    u32 color_ptr = (u32)vm->r[8];
    int cr = 255, cg = 255, cb = 255, ca = 255;
    if (color_ptr && color_ptr + 4 <= vm->mem_sz) {
        cr = vm->mem[color_ptr];
        cg = vm->mem[color_ptr + 1];
        cb = vm->mem[color_ptr + 2];
        ca = vm->mem[color_ptr + 3];
    }
    opengl_text_draw(vm, (u32)vm->r[1], vm->r[2], vm->r[3], vm->r[4],
                     vm->r[5], (u32)vm->r[6], vm->r[7], cr, cg, cb, ca);
}

/* @draw_list(buf, len) - submit a retained primitive buffer. The shared
 * tessellator (common/drawlist/drawlist.h) turns it into triangles plus
 * ordered text/clip ops; geometry goes through client-side vertex
 * arrays (GL 1.1) with an immediate-mode fallback for the Microsoft
 * software rasteriser, which faults inside glDrawArrays. */
static void opengl_gfx_draw_list(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    u32 ptr = (u32)vm->r[1];
    i32 len = vm->r[2];
    DlBatch b;
    int i, use_arrays;
    if (!s || !s->hwnd || len <= 0) return;
    if ((uint64_t)ptr + (uint64_t)len > (uint64_t)vm->mem_sz) return;

    dl_batch_init(&b);
    /* y-flip at emit: buffer coords are top-left logical, GL is y-up. */
    if (!dl_tessellate(vm->mem + ptr, len, (float)s->height, &b)) {
        dl_batch_free(&b);
        return;
    }
    if (b.no == 0) { dl_batch_free(&b); return; }
    s->frame_dirty = true;
    opengl_ensure_2d(s);
    glEnable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);

    use_arrays = !s->gl_ms_generic && b.v != NULL;
    if (use_arrays) {
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(2, GL_FLOAT, (GLsizei)sizeof(DlVert), &b.v[0].x);
        glColorPointer(4, GL_UNSIGNED_BYTE, (GLsizei)sizeof(DlVert),
                       &b.v[0].r);
    }
    for (i = 0; i < b.no; i++) {
        DlOp *o = &b.o[i];
        if (o->kind == DLO_TRIS) {
            int k;
            if (o->count <= 0) continue;
            glDisable(GL_TEXTURE_2D);
            if (use_arrays) {
                glDrawArrays(GL_TRIANGLES, o->start, o->count);
            } else {
                glBegin(GL_TRIANGLES);
                for (k = 0; k < o->count; k++) {
                    const DlVert *v = &b.v[o->start + k];
                    glColor4ub(v->r, v->g, v->b, v->a);
                    glVertex2f(v->x, v->y);
                }
                glEnd();
            }
        } else if (o->kind == DLO_CLIP) {
            opengl_apply_clip(s, o->cx, o->cy, o->cw, o->ch);
        } else if (o->kind == DLO_TEXT) {
            opengl_text_draw(vm, o->text_ptr, o->tx, o->ty, o->tsize,
                             o->tweight, o->font_ptr, o->tjust,
                             o->tr, o->tg, o->tb, o->ta);
        }
    }
    if (use_arrays) {
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
    }
    glDisable(GL_TEXTURE_2D);
    dl_batch_free(&b);
}

/* @clip(x, y, w, h) - 2D scissor in logical top-left coords. w<=0||h<=0
 * disables. Script-managed (IMGUI style). */
/* Scissor in logical top-left coords; w<=0||h<=0 disables. Shared by
 * @clip and draw-list DL_CLIP records. */
static void opengl_apply_clip(OpenGLState *s, i32 x, i32 y, i32 w, i32 h) {
    if (s->gst_on) {
        /* Guest clip rects are in guest-logical space; translate into
         * host space and never widen past the content rect. A guest
         * "clip off" just means the whole content rect again. */
        if (w <= 0 || h <= 0) {
            x = s->gst_x; y = s->gst_y; w = s->gst_w; h = s->gst_h;
        } else {
            opengl_guest_rect(s, &x, &y, &w, &h);
            opengl_guest_clamp(s, &x, &y, &w, &h);
        }
        if (w <= 0 || h <= 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, 0, 0);
            return;
        }
    } else if (w <= 0 || h <= 0) {
        glDisable(GL_SCISSOR_TEST); return;
    }

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > s->width)  w = s->width  - x;
    if (y + h > s->height) h = s->height - y;
    if (w <= 0 || h <= 0) { glDisable(GL_SCISSOR_TEST); return; }

    float bs = s->stretch_enabled ? 1.0f
             : (s->backing_scale > 0.0f ? s->backing_scale : 1.0f);
    int sx = (int)(x * bs);
    int sy = (int)((s->height - y - h) * bs);
    int sw = (int)(w * bs);
    int sh = (int)(h * bs);
    glEnable(GL_SCISSOR_TEST);
    glScissor(sx, sy, sw, sh);
}

static void opengl_gfx_clip(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) return;
    opengl_apply_clip(s, vm->r[1], vm->r[2], vm->r[3], vm->r[4]);
}

static void opengl_gfx_text(RelicVM *vm) {
    opengl_gfx_text_gdi(vm);
}

/**
 * @brief Implements @mesh: draws an indexed or sequential triangle mesh.
 */
static void opengl_gfx_mesh(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;

    u32 verts_ptr = (u32)vm->r[1];
    i32 vcount    = vm->r[2];
    u32 idx_ptr   = (u32)vm->r[3];
    i32 icount    = vm->r[4];
    i32 tex_id    = vm->r[5];
    u32 rgba_ptr  = (u32)vm->r[6];

    /* Bit 31 of icount signals u32 index mode (RLM v3). */
    int idx_u32 = (icount < 0);
    if (idx_u32) icount = icount & 0x7FFFFFFF;

    if (!s || !s->hwnd) return;
    if (vcount <= 0) return;
    s->frame_dirty = true;
    const size_t VSIZE = 8 * sizeof(float);
    size_t idx_stride = idx_u32 ? 4 : 2;
    if ((uint64_t)verts_ptr + (uint64_t)vcount * VSIZE > (uint64_t)vm->mem_sz) return;
    if (idx_ptr && (uint64_t)idx_ptr + (uint64_t)icount * idx_stride > (uint64_t)vm->mem_sz) return;

    opengl_ensure_3d(s);

    GLfloat col[4] = {1,1,1,1};
    if (rgba_ptr && rgba_ptr + 4 <= vm->mem_sz) {
        col[0] = vm->mem[rgba_ptr]   / 255.0f;
        col[1] = vm->mem[rgba_ptr+1] / 255.0f;
        col[2] = vm->mem[rgba_ptr+2] / 255.0f;
        col[3] = vm->mem[rgba_ptr+3] / 255.0f;
    }
    glColor4f(col[0], col[1], col[2], col[3]);

    if (tex_id > 0 && tex_id < s->tex_cap && s->textures[tex_id]) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, s->textures[tex_id]);
        if (s->gl_version < 120) {
            glMatrixMode(GL_TEXTURE);
            glLoadIdentity();
            glScalef(s->tex_u_scale[tex_id], s->tex_v_scale[tex_id], 1.0f);
            glMatrixMode(GL_MODELVIEW);
        }
    } else {
        glDisable(GL_TEXTURE_2D);
    }

    const GLubyte *base = vm->mem + verts_ptr;

    if (s->gl_ms_generic) {
        /* "GDI Generic" (Microsoft's software rasteriser) crashes inside
         * glDrawArrays/glDrawElements: its picked triangle-setup proc
         * dereferences a per-vertex attribute pointer that the vertex-array
         * fetch path can leave NULL (fault: fsub [ecx] with ecx=0 in the
         * interpolated-triangle setup). Immediate mode is unaffected - it
         * is the same glBegin/glVertex path every other @gfx call uses. */
        const float *v = (const float *)base;
        i32 i;
        glBegin(GL_TRIANGLES);
        if (idx_ptr && icount > 0) {
            for (i = 0; i < icount; i++) {
                u32 j = idx_u32 ? ((const u32 *)(vm->mem + idx_ptr))[i]
                                : ((const u16 *)(vm->mem + idx_ptr))[i];
                const float *p;
                if (j >= (u32)vcount) continue;
                p = v + (size_t)j * 8;
                glNormal3f(p[3], p[4], p[5]);
                glTexCoord2f(p[6], p[7]);
                glVertex3f(p[0], p[1], p[2]);
            }
        } else {
            for (i = 0; i < vcount; i++) {
                const float *p = v + (size_t)i * 8;
                glNormal3f(p[3], p[4], p[5]);
                glTexCoord2f(p[6], p[7]);
                glVertex3f(p[0], p[1], p[2]);
            }
        }
        glEnd();
    } else {
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_NORMAL_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer  (3, GL_FLOAT, (GLsizei)VSIZE, base + 0);
        glNormalPointer  (   GL_FLOAT, (GLsizei)VSIZE, base + 12);
        glTexCoordPointer(2, GL_FLOAT, (GLsizei)VSIZE, base + 24);

        if (idx_ptr && icount > 0) {
            GLenum idx_type = idx_u32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
            glDrawElements(GL_TRIANGLES, icount, idx_type, vm->mem + idx_ptr);
        } else {
            glDrawArrays(GL_TRIANGLES, 0, vcount);
        }
    }

    if (tex_id > 0 && tex_id < s->tex_cap && s->textures[tex_id]) {
        if (s->gl_version < 120) {
            glMatrixMode(GL_TEXTURE);
            glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);
        }
    }
    if (tex_id > 0) glBindTexture(GL_TEXTURE_2D, 0);
}

/**
 * @brief Syncs backend size to the client rect, updating viewport or letterbox.
 *
 * The client rect is in device pixels; dividing by the monitor's scale factor
 * gives the logical size the script sees and draws into. glViewport therefore
 * takes device pixels while glOrtho takes logical ones, so a script that never
 * mentions DPI still fills the window at the right physical size.
 */
static void opengl_sync_size(OpenGLState *s) {
    if (!s || !s->hwnd) return;
    RECT rc;
    if (!GetClientRect(s->hwnd, &rc)) return;
    int pw = rc.right - rc.left;
    int ph = rc.bottom - rc.top;
    if (pw <= 0 || ph <= 0) return;

    /* Track the live DPI. WM_DPICHANGED already updates it when the window
     * moves between differently-scaled monitors, but polling here also covers
     * the user changing the scale factor in Settings while we run. */
    int dpi = s->dpi_scaling ? (int)opengl_dpi_for_window(s->hwnd)
                             : RELIC_DEFAULT_DPI;
    if (dpi <= 0) dpi = RELIC_DEFAULT_DPI;
    s->dpi = dpi;

    if (s->stretch_enabled) {
        /* The script renders 1:1 into a fixed logical_w x logical_h sub-rect,
         * so DPI only makes the letterbox target bigger; backing_scale still
         * describes device pixels per logical pixel for input mapping. */
        s->pixel_w = pw;
        s->pixel_h = ph;
        s->backing_scale = (float)dpi / (float)RELIC_DEFAULT_DPI;
        if (s->logical_w <= 0 || s->logical_h <= 0) return;
        float sx = (float)pw / (float)s->logical_w;
        float sy = (float)ph / (float)s->logical_h;
        float scale = (sx < sy) ? sx : sy;
        int vw = (int)((float)s->logical_w * scale);
        int vh = (int)((float)s->logical_h * scale);
        if (vw < 1) vw = 1;
        if (vh < 1) vh = 1;
        s->vp_w = vw;
        s->vp_h = vh;
        s->vp_x = (pw - vw) / 2;
        s->vp_y = (ph - vh) / 2;
        return;
    }

    /* Logical size of the client area. At 96 DPI (or under
     * RELIC_WINFLAG_NODPI, and on win95) this is the device size unchanged. */
    int lw = MulDiv(pw, RELIC_DEFAULT_DPI, dpi);
    int lh = MulDiv(ph, RELIC_DEFAULT_DPI, dpi);
    if (lw < 1) lw = 1;
    if (lh < 1) lh = 1;

    /* @platform reports the logical size: it is the coordinate space the
     * script lays out in, so scripts that recompute layout from it stay
     * consistent with what they draw. Matches the macOS backend. */
    s->reported_width  = lw;
    s->reported_height = lh;

    if (pw == s->pixel_w && ph == s->pixel_h &&
        lw == s->width   && lh == s->height) return;

    float prev_scale = s->backing_scale;
    s->width   = lw;
    s->height  = lh;
    s->pixel_w = pw;
    s->pixel_h = ph;
    s->backing_scale = (float)pw / (float)lw;
    if (s->backing_scale != prev_scale) opengl_refresh_tex_filters(s);

    glViewport(0, 0, pw, ph);
    if (!s->in_3d) {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, lw, 0, lh, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
    }
}

/**
 * @brief Drains the Win32 message queue and syncs window size.
 */
static void opengl_pump_events(OpenGLState *s) {
    if (!s) return;
    opengl_sync_size(s);
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        if (msg.message == WM_QUIT) {
            s->quit = true;
            if (g_vm) g_vm->running = false;
        }
    }
}

/**
 * @brief Implements @key: pops the oldest queued key press.
 */
static void opengl_inp_key(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) { vm->r[1] = 0; return; }
    opengl_pump_events(s);
    vm->r[1] = 0;
    if (s->key_qh != s->key_qt) {
        vm->r[1] = (i32)s->key_queue[s->key_qh];
        s->key_qh = (s->key_qh + 1) & 31;
    }
}

/**
 * @brief Implements @keyheld: reports whether a given key is currently down.
 */
static void opengl_inp_keyheld(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) { vm->r[1] = 0; return; }
    opengl_pump_events(s);
    u8 k = (u8)vm->r[1];
    /* WINFLAG_CLOSE_REQUEST dedicated channel: VK 0 is never a real key,
     * so @key_held(0) reports the window close button (X / Alt+F4) and
     * nothing else. The first poll switches the legacy Escape signal
     * off for this window - from then on the X can't be mistaken for,
     * or eaten as, a keypress by a key-scanning widget. */
    if (k == 0) {
        s->close_chan0 = true;
        s->close_requested = false;
        vm->r[1] = s->close_pending ? 1 : 0;
        s->close_pending = false;
        return;
    }
    /* WINFLAG_CLOSE_REQUEST: deliver a one-shot Escape (27) signal when the
     * window close button was clicked. The signal is cleared after reading
     * so the script's save-changes dialog isn't re-triggered every frame. */
    if (k == 27 && s->close_requested) {
        s->close_requested = false;
        vm->r[1] = 1;
        return;
    }
    /* Self-heal: keys[] is fed by WM_KEYDOWN/WM_KEYUP, and any keyup that
     * misses this window (focus steal, modal loop) leaves the entry latched
     * forever. If the table claims a key is down but the physical key is up,
     * the release was lost - clear it. Only ever clears; entries are still
     * set exclusively by real keydown messages, so phantom keys can't
     * appear. */
    if (s->keys[k] && !(GetAsyncKeyState((int)k) & 0x8000)) {
        s->keys[k] = false;
    }
    /* Generic modifier VKs (Shift 16, Ctrl 17, Alt 18) are never delivered
     * to keys[]: WM_KEYDOWN's wParam carries the side-specific code
     * (VK_LSHIFT 160..VK_RMENU 165). GetAsyncKeyState accepts the generic
     * VK and reports either side - without this, @key_held(16/17/18)
     * could never fire on Windows. */
    if (k == 16 || k == 17 || k == 18) {
        vm->r[1] = (GetAsyncKeyState((int)k) & 0x8000) ? 1 : 0;
        return;
    }
    vm->r[1] = s->keys[k] ? 1 : 0;
}

/* WINFLAG_CLOSE_REQUEST poll for embedded hosts: reports the pending
 * window-close click once, independent of the physical Escape state.
 * mvm uses it to keep the request on the host VM while a focused child
 * owns @key_held. */
static int opengl_inp_close_requested(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s) return 0;
    /* Drain either channel - the key-0 latch too, so a child's
     * inp_close_requested peek can still rescue a pending close to the
     * host via mvm's g_close_pending even after chan0 took over. */
    if (s->close_requested) { s->close_requested = false; return 1; }
    if (s->close_pending)   { s->close_pending   = false; return 1; }
    return 0;
}

/**
 * @brief Implements @mouse: writes x, y, buttons and wheel delta to VM memory.
 */
static void opengl_inp_mouse(RelicVM *vm) {
    u32 ptr = (u32)vm->r[1];
    if (ptr + 16u > vm->mem_sz) return;
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    i32 x = 0, y = 0, b = 0, w = 0;
    if (s && s->hwnd) {
        opengl_pump_events(s);
        x = s->mouse_x;
        y = s->mouse_y;
        b = s->mouse_buttons;
        /* mouse_wheel is a raw-delta accumulator (see WM_MOUSEWHEEL):
         * hand out whole notches and keep the sub-notch remainder so it
         * can complete a notch across events and across reads. */
        w = s->mouse_wheel / WHEEL_DELTA;
        s->mouse_wheel -= w * WHEEL_DELTA;
    }
    memcpy(vm->mem + ptr + 0,  &x, 4);
    memcpy(vm->mem + ptr + 4,  &y, 4);
    memcpy(vm->mem + ptr + 8,  &b, 4);
    memcpy(vm->mem + ptr + 12, &w, 4);
}

/**
 * @brief Implements @gamepad_count: returns number of connected controllers.
 */
static void opengl_inp_gamepad_count(RelicVM *vm) {
    opengl_xinput_load();
    if (!g_xinput_get_state) { vm->r[1] = 0; return; }
    int count = 0;
    RELIC_XINPUT_STATE st;
    for (int i = 0; i < RELIC_XINPUT_MAX_USERS; i++) {
        if (g_xinput_get_state((DWORD)i, &st) == ERROR_SUCCESS)
            count++;
    }
    vm->r[1] = count;
}

/**
 * @brief Implements @gamepad: reads controller state into a 28-byte struct.
 * @gamepad(idx: i32, out: ref Gamepad) -> i32 (1=connected)
 * Gamepad struct: i32 buttons, i32 lx, i32 ly, i32 rx, i32 ry, i32 lt, i32 rt
 */
static void opengl_inp_gamepad(RelicVM *vm) {
    i32 idx = vm->r[1];
    u32 ptr = (u32)vm->r[2];
    if (ptr + 28u > vm->mem_sz) { vm->r[1] = 0; return; }

    opengl_xinput_load();
    if (!g_xinput_get_state || idx < 0 || idx >= RELIC_XINPUT_MAX_USERS) {
        vm->r[1] = 0;
        memset(vm->mem + ptr, 0, 28);
        return;
    }

    RELIC_XINPUT_STATE st;
    if (g_xinput_get_state((DWORD)idx, &st) != ERROR_SUCCESS) {
        vm->r[1] = 0;
        memset(vm->mem + ptr, 0, 28);
        return;
    }

    i32 buttons = opengl_xinput_map_buttons(st.Gamepad.wButtons);
    i32 lx = (i32)st.Gamepad.sThumbLX;
    i32 ly = (i32)st.Gamepad.sThumbLY;
    i32 rx = (i32)st.Gamepad.sThumbRX;
    i32 ry = (i32)st.Gamepad.sThumbRY;
    /* Scale triggers from 0-255 to 0-32767 */
    i32 lt = (i32)st.Gamepad.bLeftTrigger  * 129;
    i32 rt = (i32)st.Gamepad.bRightTrigger * 129;

    memcpy(vm->mem + ptr + 0,  &buttons, 4);
    memcpy(vm->mem + ptr + 4,  &lx, 4);
    memcpy(vm->mem + ptr + 8,  &ly, 4);
    memcpy(vm->mem + ptr + 12, &rx, 4);
    memcpy(vm->mem + ptr + 16, &ry, 4);
    memcpy(vm->mem + ptr + 20, &lt, 4);
    memcpy(vm->mem + ptr + 24, &rt, 4);
    vm->r[1] = 1;
}

/**
 * @brief Implements @rand: returns a 16-bit pseudo-random value.
 */
static void opengl_sys_rand(RelicVM *vm)   { vm->r[1] = rand() & 0xFFFF; }

/**
 * @brief Implements @millis: returns a millisecond tick count.
 */
static void opengl_sys_millis(RelicVM *vm) {
    /* Process-relative, not system uptime: raw timeGetTime() goes
     * negative in the i32 result once the host has been up ~24.8 days,
     * which inverts every "now < deadline" timer check in scripts (weapon
     * cooldowns refuse to fire, HUD flashes stick). The u32 delta is
     * wrap-safe. */
    static DWORD t0 = 0;
    DWORD now = timeGetTime();
    if (t0 == 0) t0 = now;
    vm->r[1] = (i32)(now - t0);
}

/**
 * @brief Implements @console: writes a NUL-terminated VM string to stdout.
 */
static void opengl_sys_console(RelicVM *vm) {
    /* Un-hide the console if it was hidden as an Explorer launch (see
     * tigryx.h); a script calling @console is exactly the case that should
     * make it visible. */
    relic_console_ensure();
    u32 ptr = (u32)vm->r[1];
    while (ptr < vm->mem_sz) {
        u8 c = vm->mem[ptr++];
        if (c == 0) break;
        fputc((int)c, stdout);
    }
    fflush(stdout);
}

#define BACKEND_MAX_FILES 16

/**
 * @brief Open file or embedded-asset handle used by the file I/O API.
 */
typedef struct {
    bool      is_asset;
    FILE     *fp;
    const u8 *asset_data;
    u32       asset_size;
    u32       asset_pos;
} BackendFileHandle;
static BackendFileHandle opengl_handles[BACKEND_MAX_FILES];

/**
 * @brief Allocates a handle slot backed by an in-memory asset.
 */
static i32 opengl_handle_alloc_asset(const u8 *d, u32 sz) {
    for(int i=1;i<BACKEND_MAX_FILES;i++) {
        if(!opengl_handles[i].fp && !opengl_handles[i].asset_data) {
            opengl_handles[i].is_asset=true; opengl_handles[i].fp=NULL;
            opengl_handles[i].asset_data=d; opengl_handles[i].asset_size=sz;
            opengl_handles[i].asset_pos=0; return (i32)i;
        }
    }
    return -1;
}
/**
 * @brief Allocates a handle slot backed by an open FILE*.
 */
static i32 opengl_handle_alloc_file(FILE *f) {
    for(int i=1;i<BACKEND_MAX_FILES;i++) {
        if(!opengl_handles[i].fp && !opengl_handles[i].asset_data) {
            opengl_handles[i].is_asset=false; opengl_handles[i].fp=f;
            opengl_handles[i].asset_data=NULL; return (i32)i;
        }
    }
    return -1;
}
/**
 * @brief Resolves a handle index to its slot, or NULL if invalid.
 */
static BackendFileHandle *opengl_handle_get(i32 idx) {
    if(idx<1||idx>=BACKEND_MAX_FILES) return NULL;
    if(!opengl_handles[idx].fp && !opengl_handles[idx].asset_data) return NULL;
    return &opengl_handles[idx];
}
/**
 * @brief Closes and clears a handle slot.
 */
static void opengl_handle_free(i32 idx) {
    if(idx<1||idx>=BACKEND_MAX_FILES) return;
    if(opengl_handles[idx].fp) fclose(opengl_handles[idx].fp);
    memset(&opengl_handles[idx],0,sizeof(opengl_handles[idx]));
}

/**
 * @brief Implements @fopen: opens a file or asset for read, or a file for write.
 * @return A handle index in vm->r[1], or -1 on failure.
 */
static void opengl_sys_fopen(RelicVM *vm) {
    u32 ptr = (u32)vm->r[1];
    i32 len = vm->r[2];
    i32 mode = vm->r[3];
    char path[512] = {0};
    if(len <= 0) {
        len = 0;
        while(len < 511 && ptr + (u32)len < vm->mem_sz && vm->mem[ptr + len] != 0) len++;
    }
    int n = len < 511 ? (int)len : 511;
    if(ptr + (u32)n <= vm->mem_sz) memcpy(path, vm->mem + ptr, n);
    if(mode == 1) {
        FILE *f = fopen(path, "wb");
        vm->r[1] = f ? opengl_handle_alloc_file(f) : -1;
        if(f && vm->r[1]==-1) fclose(f);
    } else if(mode == 2) {
        /* FILE_MODE_READ_DISK: filesystem only, never assets - for state
         * files the program itself writes. */
        FILE *f = fopen(path, "rb");
        vm->r[1] = f ? opengl_handle_alloc_file(f) : -1;
        if(f && vm->r[1]==-1) fclose(f);
    } else {
        u32 asset_sz = 0;
        const u8 *asset = relic_assets_find(vm->assets, path, &asset_sz);
        if(asset) {
            vm->r[1] = opengl_handle_alloc_asset(asset, asset_sz);
        } else {
            FILE *f = fopen(path, "rb");
            vm->r[1] = f ? opengl_handle_alloc_file(f) : -1;
            if(f && vm->r[1]==-1) fclose(f);
        }
    }
}

/**
 * @brief Implements @fread: reads bytes from a file or asset into VM memory.
 */
static void opengl_sys_fread(RelicVM *vm) {
    BackendFileHandle *h = opengl_handle_get(vm->r[1]);
    u32 buf = (u32)vm->r[2];
    u32 n   = (u32)vm->r[3];
    if(!h || buf + n > vm->mem_sz) { vm->r[1] = 0; return; }
    if(h->is_asset) {
        u32 avail = h->asset_size - h->asset_pos;
        u32 rd = n < avail ? n : avail;
        if(rd) memcpy(vm->mem + buf, h->asset_data + h->asset_pos, rd);
        h->asset_pos += rd;
        vm->r[1] = (i32)rd;
    } else {
        vm->r[1] = (i32)fread(vm->mem + buf, 1, n, h->fp);
    }
}

/**
 * @brief Implements @fwrite: writes VM memory to a file (assets are read-only).
 */
static void opengl_sys_fwrite(RelicVM *vm) {
    BackendFileHandle *h = opengl_handle_get(vm->r[1]);
    u32 buf = (u32)vm->r[2];
    u32 n   = (u32)vm->r[3];
    if(!h || buf + n > vm->mem_sz) { vm->r[1] = 0; return; }
    if(h->is_asset) {
        vm->r[1] = 0;
    } else {
        vm->r[1] = (i32)fwrite(vm->mem + buf, 1, n, h->fp);
    }
}

/**
 * @brief Implements @fclose: releases a file or asset handle.
 */
static void opengl_sys_fclose(RelicVM *vm) {
    opengl_handle_free(vm->r[1]);
}

static void opengl_sys_fseek(RelicVM *vm) {
    BackendFileHandle *h = opengl_handle_get(vm->r[1]);
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

static void opengl_sys_flen(RelicVM *vm) {
    BackendFileHandle *h = opengl_handle_get(vm->r[1]);
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

/* -------------------------------------------------------------------------
 * Web / HTTP — shared WinINet + worker-thread implementation lives in
 * ../syswin32/syswin32.c (see docs/web.md). The hooks below delegate to it
 * with this backend's slot table.
 * ---------------------------------------------------------------------- */

static void opengl_sys_http_request(RelicVM *vm) {
    OpenGLState *s = (OpenGLState *)vm->backend_data;
    syswin32_sys_http_request(vm, s ? s->http : NULL);
}
static void opengl_sys_http_poll(RelicVM *vm) {
    OpenGLState *s = (OpenGLState *)vm->backend_data;
    syswin32_sys_http_poll(vm, s ? s->http : NULL);
}
static void opengl_sys_http_status(RelicVM *vm) {
    OpenGLState *s = (OpenGLState *)vm->backend_data;
    syswin32_sys_http_status(vm, s ? s->http : NULL);
}
static void opengl_sys_http_read(RelicVM *vm) {
    OpenGLState *s = (OpenGLState *)vm->backend_data;
    syswin32_sys_http_read(vm, s ? s->http : NULL);
}
static void opengl_sys_http_close(RelicVM *vm) {
    OpenGLState *s = (OpenGLState *)vm->backend_data;
    syswin32_sys_http_close(vm, s ? s->http : NULL);
}

/* ==========================================================================
 * Subprocess management (@process_spawn/poll/exit_code/read_stdout/
 * read_stderr/kill/close)
 *
 * Uses CreateProcess with anonymous pipes for stdout/stderr capture.
 * Reads are non-blocking: PeekNamedPipe checks how many bytes are
 * available, then ReadFile reads only that many (which returns
 * immediately). This keeps the VM's poll loop non-blocking, matching
 * the HTTP async pattern.
 * ========================================================================== */

/* @process_spawn(cmd: *u8, cwd: *u8) -> i32 handle (>=0) or -1 */
static void opengl_sys_proc_spawn(RelicVM *vm) {
    OpenGLState *s = (OpenGLState *)vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }

    u32 cmd_ptr = (u32)vm->r[1];
    u32 cwd_ptr = (u32)vm->r[2];

    if (cmd_ptr == 0 || cmd_ptr >= vm->mem_sz) { vm->r[1] = -1; return; }

    /* Copy command line out of VM memory. */
    char cmdline[1024];
    int cl = 0;
    while (cl < (int)sizeof(cmdline) - 1 && cmd_ptr + (u32)cl < vm->mem_sz
           && vm->mem[cmd_ptr + cl] != 0) {
        cmdline[cl] = (char)vm->mem[cmd_ptr + cl]; cl++;
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
            cwd[wl] = (char)vm->mem[cwd_ptr + wl]; wl++;
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

    /* Create anonymous pipes for stdout and stderr.
     * The read ends are inherited by us; the write ends go to the child.
     * We set the read ends to non-blocking by NOT inheriting them and
     * using PeekNamedPipe before ReadFile. */
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE stdout_w = NULL, stderr_w = NULL;
    HANDLE stdout_r = NULL, stderr_r = NULL;

    if (!CreatePipe(&stdout_r, &stdout_w, &sa, 0)) { vm->r[1] = -1; return; }
    if (!CreatePipe(&stderr_r, &stderr_w, &sa, 0)) {
        CloseHandle(stdout_r); CloseHandle(stdout_w);
        vm->r[1] = -1; return;
    }

    /* Ensure our read ends are NOT inherited by the child. */
    SetHandleInformation(stdout_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_w;
    si.hStdError  = stderr_w;
    si.hStdInput  = NULL;
    memset(&pi, 0, sizeof(pi));

    /* CreateProcess may modify cmdline (it's non-const). */
    char cmd_buf[1024];
    snprintf(cmd_buf, sizeof(cmd_buf), "%.*s", (int)sizeof(cmd_buf) - 1, cmdline);

    BOOL ok = CreateProcessA(
        NULL,           /* lpApplicationName - let it parse from cmdline */
        cmd_buf,        /* lpCommandLine */
        NULL, NULL,     /* process/thread security attrs */
        TRUE,           /* bInheritHandles - inherit pipe write ends */
        CREATE_NO_WINDOW, /* dwCreationFlags - no console window for the child */
        NULL,           /* lpEnvironment - inherit parent's */
        cwd_arg,        /* lpCurrentDirectory */
        &si, &pi
    );

    /* Close the write ends in the parent so ReadFile returns 0 on EOF. */
    CloseHandle(stdout_w);
    CloseHandle(stderr_w);

    if (!ok) {
        CloseHandle(stdout_r);
        CloseHandle(stderr_r);
        vm->r[1] = -1;
        return;
    }

    /* Close the thread handle - we don't need it. */
    CloseHandle(pi.hThread);

    s->proc[slot].in_use       = true;
    s->proc[slot].hProcess     = pi.hProcess;
    s->proc[slot].hThread       = NULL;
    s->proc[slot].stdout_pipe_r = stdout_r;
    s->proc[slot].stderr_pipe_r = stderr_r;
    s->proc[slot].exit_code     = 0;
    s->proc[slot].exited        = false;

    vm->r[1] = slot;
}

/* @process_poll(handle: i32) -> i32 (0=running, 1=exited, -1=invalid) */
static void opengl_sys_proc_poll(RelicVM *vm) {
    OpenGLState *s = (OpenGLState *)vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    int handle = vm->r[1];
    if (handle < 0 || handle >= RELIC_MAX_PROC) { vm->r[1] = -1; return; }
    if (!s->proc[handle].in_use) { vm->r[1] = -1; return; }

    if (s->proc[handle].exited) { vm->r[1] = 1; return; }

    DWORD code;
    if (GetExitCodeProcess(s->proc[handle].hProcess, &code)) {
        if (code != STILL_ACTIVE) {
            s->proc[handle].exited    = true;
            s->proc[handle].exit_code = (int)code;
            vm->r[1] = 1;
            return;
        }
    }
    vm->r[1] = 0;
}

/* @process_exit_code(handle: i32) -> i32 exit code (valid once exited) */
static void opengl_sys_proc_exit_code(RelicVM *vm) {
    OpenGLState *s = (OpenGLState *)vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    int handle = vm->r[1];
    if (handle < 0 || handle >= RELIC_MAX_PROC) { vm->r[1] = -1; return; }
    if (!s->proc[handle].in_use) { vm->r[1] = -1; return; }

    /* If not yet marked exited, check now. */
    if (!s->proc[handle].exited) {
        DWORD code;
        if (GetExitCodeProcess(s->proc[handle].hProcess, &code)) {
            if (code != STILL_ACTIVE) {
                s->proc[handle].exited    = true;
                s->proc[handle].exit_code = (int)code;
            }
        }
    }

    if (s->proc[handle].exited) { vm->r[1] = s->proc[handle].exit_code; return; }
    vm->r[1] = -1;
}

/* Helper: read available bytes from a pipe into VM memory.
 * Returns the number of bytes copied (0 = none available or EOF). */
static int proc_read_pipe(HANDLE pipe_r, u8 *vm_mem, u32 buf_ptr, i32 cap, u32 mem_sz) {
    if (cap <= 0 || buf_ptr == 0 || buf_ptr >= mem_sz) return -1;

    /* Check how many bytes are available without blocking. */
    DWORD avail = 0;
    if (!PeekNamedPipe(pipe_r, NULL, 0, NULL, &avail, NULL)) {
        /* Pipe closed / broken = EOF. */
        return 0;
    }
    if (avail == 0) return 0;

    DWORD to_read = avail;
    if ((i32)to_read > cap) to_read = (DWORD)cap;
    if (buf_ptr + to_read > mem_sz) to_read = mem_sz - buf_ptr;

    DWORD bytes_read = 0;
    if (!ReadFile(pipe_r, vm_mem + buf_ptr, to_read, &bytes_read, NULL)) {
        return 0;
    }
    return (int)bytes_read;
}

/* @process_read_stdout(handle, buf: *u8, cap: i32) -> i32 bytes (0=none) */
static void opengl_sys_proc_read_out(RelicVM *vm) {
    OpenGLState *s = (OpenGLState *)vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    int handle  = vm->r[1];
    u32 buf_ptr = (u32)vm->r[2];
    i32 cap     = vm->r[3];
    if (handle < 0 || handle >= RELIC_MAX_PROC || !s->proc[handle].in_use) {
        vm->r[1] = -1; return;
    }
    vm->r[1] = proc_read_pipe(s->proc[handle].stdout_pipe_r,
                              vm->mem, buf_ptr, cap, vm->mem_sz);
}

/* @process_read_stderr(handle, buf: *u8, cap: i32) -> i32 bytes (0=none) */
static void opengl_sys_proc_read_err(RelicVM *vm) {
    OpenGLState *s = (OpenGLState *)vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    int handle  = vm->r[1];
    u32 buf_ptr = (u32)vm->r[2];
    i32 cap     = vm->r[3];
    if (handle < 0 || handle >= RELIC_MAX_PROC || !s->proc[handle].in_use) {
        vm->r[1] = -1; return;
    }
    vm->r[1] = proc_read_pipe(s->proc[handle].stderr_pipe_r,
                              vm->mem, buf_ptr, cap, vm->mem_sz);
}

/* @process_kill(handle: i32) -> i32 (1=killed, 0=already exited) */
static void opengl_sys_proc_kill(RelicVM *vm) {
    OpenGLState *s = (OpenGLState *)vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    int handle = vm->r[1];
    if (handle < 0 || handle >= RELIC_MAX_PROC || !s->proc[handle].in_use) {
        vm->r[1] = -1; return;
    }

    /* Check if already exited. */
    if (!s->proc[handle].exited) {
        DWORD code;
        if (GetExitCodeProcess(s->proc[handle].hProcess, &code)) {
            if (code != STILL_ACTIVE) {
                s->proc[handle].exited    = true;
                s->proc[handle].exit_code = (int)code;
            }
        }
    }

    if (s->proc[handle].exited) { vm->r[1] = 0; return; }

    if (TerminateProcess(s->proc[handle].hProcess, 1)) {
        s->proc[handle].exited    = true;
        s->proc[handle].exit_code = 1;
        vm->r[1] = 1;
    } else {
        vm->r[1] = 0;
    }
}

/* @process_close(handle: i32) -> i32 (1=ok, releases slot; kills if running) */
static void opengl_sys_proc_close(RelicVM *vm) {
    OpenGLState *s = (OpenGLState *)vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    int handle = vm->r[1];
    if (handle < 0 || handle >= RELIC_MAX_PROC || !s->proc[handle].in_use) {
        vm->r[1] = -1; return;
    }

    /* Kill if still running. */
    if (!s->proc[handle].exited) {
        DWORD code;
        if (GetExitCodeProcess(s->proc[handle].hProcess, &code)) {
            if (code == STILL_ACTIVE) {
                TerminateProcess(s->proc[handle].hProcess, 1);
            }
        }
    }

    if (s->proc[handle].hProcess)     CloseHandle(s->proc[handle].hProcess);
    if (s->proc[handle].stdout_pipe_r) CloseHandle(s->proc[handle].stdout_pipe_r);
    if (s->proc[handle].stderr_pipe_r) CloseHandle(s->proc[handle].stderr_pipe_r);

    memset(&s->proc[handle], 0, sizeof(ProcSlot));
    vm->r[1] = 1;
}

/* Called from opengl_shutdown to clean up all spawned processes. */
static void opengl_proc_shutdown(OpenGLState *s) {
    for (int i = 0; i < RELIC_MAX_PROC; i++) {
        if (!s->proc[i].in_use) continue;
        if (s->proc[i].hProcess) {
            DWORD code;
            if (GetExitCodeProcess(s->proc[i].hProcess, &code)) {
                if (code == STILL_ACTIVE) TerminateProcess(s->proc[i].hProcess, 1);
            }
            CloseHandle(s->proc[i].hProcess);
        }
        if (s->proc[i].stdout_pipe_r) CloseHandle(s->proc[i].stdout_pipe_r);
        if (s->proc[i].stderr_pipe_r) CloseHandle(s->proc[i].stderr_pipe_r);
        memset(&s->proc[i], 0, sizeof(ProcSlot));
    }
}

/**
 * @brief Implements @platform: fills a PlatformInfo struct in VM memory.
 * The shared probe in syswin32.c handles the OS/CPU fields; this wrapper
 * supplies the OpenGL-specific view (GPU name, GL version, window size).
 */
static void opengl_sys_platform(RelicVM *vm) {
    SysWin32PlatformView v;
    memset(&v, 0, sizeof(v));
    v.display_mode = "Graphics";
    v.gfx_arch     = "OpenGL";
    v.sound_flags  = 0x0F;  /* tones | samples | MUS | MIDI */
    v.backend_name = RELIC_BACKEND_ID;
    v.gpu_str      = "Hardware";
    v.gl_version   = "0.0";

    if (g_state && g_state->hglrc) {
        const char *renderer = (const char*)glGetString(GL_RENDERER);
        if (renderer && renderer[0])
            v.gpu_str = renderer;

        const char *version_str = (const char*)glGetString(GL_VERSION);
        if (version_str) {
            int major = 0, minor = 0;
            static char gl_buf[16];
            if (sscanf(version_str, "%d.%d", &major, &minor) == 2) {
                snprintf(gl_buf, sizeof(gl_buf), "%d.%d", major, minor);
                v.gl_version = gl_buf;
            }
        }
    }

    if (g_state) {
        opengl_sync_size(g_state);
        v.win_w = g_state->reported_width;
        v.win_h = g_state->reported_height;
        if (g_state->hwnd) {
            RECT wrc;
            if (GetWindowRect(g_state->hwnd, &wrc)) {
                v.win_x = (i32)wrc.left;
                v.win_y = (i32)wrc.top;
            }
        }
    }

    syswin32_sys_platform(vm, &v);
}

/* --- System clipboard (@clipboard_set / @clipboard_get): the shared Win32
 * CF_TEXT implementation in syswin32.c needs no window, so these are plain
 * delegations. */
static void opengl_sys_clip_set(RelicVM *vm) { syswin32_sys_clip_set(vm); }
static void opengl_sys_clip_get(RelicVM *vm) { syswin32_sys_clip_get(vm); }

#define SND_SAMPLE_RATE  22050
#define SND_AMPLITUDE    110
#define SND_SILENCE      128
#define SND_CHUNK_SAMPLES 512
#define SND_STREAM_BUFFERS 3
#define SND_MAX_VOICES   16

/**
 * @brief One playing PCM voice in the software mixer.
 */
typedef struct {
    u8 *data;
    int len;
    int pos;
    int volume;
    int active;
} OpenGLSndVoice;

/**
 * @brief Audio subsystem state: waveOut device, streaming buffers and voices.
 */
typedef struct {
    HWAVEOUT         hwo;
    WAVEFORMATEX     wfx;
    CRITICAL_SECTION lock;
    HANDLE           thread;
    int              running;
    u8               bufs[SND_STREAM_BUFFERS][SND_CHUNK_SAMPLES];
    WAVEHDR          hdrs[SND_STREAM_BUFFERS];
    OpenGLSndVoice   voices[SND_MAX_VOICES];
} OpenGLSndState;

static OpenGLSndState g_snd = {0};

static float g_master_vol = 1.0f;
static float g_sfx_vol = 1.0f;
static float g_muted_master = -1.0f;

/**
 * @brief Renders a frequency-swept square tone with attack/release envelope.
 * @return The number of samples written.
 */
static int opengl_snd_fill_sweep(u8 *buf, int buf_sz,
                                 int freq_start, int freq_end, int ms, int volume)
{
    int samples = (SND_SAMPLE_RATE * ms) / 1000;
    if (samples > buf_sz) samples = buf_sz;
    if (volume < 0)   volume = 0;
    if (volume > 255) volume = 255;
    int amplitude = (SND_AMPLITUDE * volume) / 255;

    static float filter_state = 0.0f;
    const float alpha = 0.15f;

    for (int i = 0; i < samples; i++) {
        float t    = (samples > 1) ? (float)i / (float)(samples - 1) : 0.0f;
        float freq = (float)freq_start + t * (float)(freq_end - freq_start);
        if (freq < 20.0f) freq = 20.0f;
        float half = (float)SND_SAMPLE_RATE / (2.0f * freq);
        int phase = (int)((float)i / half) & 1;
        float env = 1.0f;
        float ms_f = (float)ms;
        float t_ms = (float)i * 1000.0f / (float)SND_SAMPLE_RATE;
        if (t_ms < 5.0f)         env = t_ms / 5.0f;
        else if (t_ms > ms_f-5.0f) env = (ms_f - t_ms) / 5.0f;
        if (env < 0.0f) env = 0.0f;
        int val = SND_SILENCE + (phase ? 1 : -1) * (int)(amplitude * env);

        float input = (float)val;
        float output = alpha * input + (1.0f - alpha) * filter_state;
        filter_state = output;
        buf[i] = (u8)((int)(output + 0.5f));
    }
    return samples;
}

static void mus_render(u8 *dst, int n);

/* Streamed (Ogg/MP3/FLAC/WAV) music track; guarded by g_snd.lock. */
static float mus_master_vol(void);   /* @mixer music volume (g_mus.master) */
#define MUSDEC_LOCK()   EnterCriticalSection(&g_snd.lock)
#define MUSDEC_UNLOCK() LeaveCriticalSection(&g_snd.lock)
#define MUSDEC_VOLUME() mus_master_vol()
#define MUSDEC_OUT_RATE SND_SAMPLE_RATE
#include "../common/musdec_backend.h"

/**
 * @brief Mixes music and all active voices into the output buffer.
 * @return The number of samples mixed.
 */
static int opengl_snd_mixer_fill(u8 *dst, int dst_samples)
{
    memset(dst, SND_SILENCE, dst_samples);

    EnterCriticalSection(&g_snd.lock);
    mus_render(dst, dst_samples);
    musdec_mix(dst, dst_samples);
    for (int v = 0; v < SND_MAX_VOICES; v++) {
        if (!g_snd.voices[v].active) continue;
        OpenGLSndVoice *voice = &g_snd.voices[v];
        int vol = voice->volume;
        for (int i = 0; i < dst_samples && voice->pos < voice->len; i++) {
            int sample = (int)voice->data[voice->pos] - 128;
            int mixed  = (int)dst[i] - 128 + (sample * vol) / 255;
            if (mixed > 127)  mixed = 127;
            if (mixed < -128) mixed = -128;
            dst[i] = (u8)(mixed + 128);
            voice->pos++;
        }
        if (voice->pos >= voice->len) {
            voice->active = 0;
            free(voice->data);
            voice->data = NULL;
        }
    }
    LeaveCriticalSection(&g_snd.lock);

    if (g_master_vol < 1.0f) {
        for (int i = 0; i < dst_samples; i++) {
            int s = (int)dst[i] - 128;
            s = (int)(s * g_master_vol);
            dst[i] = (u8)(s + 128);
        }
    }
    return dst_samples;
}

/**
 * @brief Copies a PCM buffer into a free voice slot and starts playback.
 * @return 1 if a voice was started, 0 otherwise.
 */
static int opengl_snd_start_voice(const u8 *buf, int len, int volume)
{
    if (!g_snd.running || len <= 0) return 0;
    u8 *copy = (u8*)malloc(len);
    if (!copy) return 0;
    memcpy(copy, buf, len);

    EnterCriticalSection(&g_snd.lock);
    int started = 0;
    for (int i = 0; i < SND_MAX_VOICES; i++) {
        if (!g_snd.voices[i].active) {
            g_snd.voices[i].data   = copy;
            g_snd.voices[i].len    = len;
            g_snd.voices[i].pos    = 0;
            g_snd.voices[i].volume = volume;
            g_snd.voices[i].active = 1;
            started = 1;
            break;
        }
    }
    if (!started) free(copy);
    LeaveCriticalSection(&g_snd.lock);
    return started;
}

/**
 * @brief Audio worker thread: refills and resubmits completed wave buffers.
 */
static DWORD WINAPI opengl_snd_thread_proc(LPVOID param)
{
    (void)param;

    for (int i = 0; i < SND_STREAM_BUFFERS; i++) {
        opengl_snd_mixer_fill(g_snd.bufs[i], SND_CHUNK_SAMPLES);
        waveOutWrite(g_snd.hwo, &g_snd.hdrs[i], sizeof(WAVEHDR));
    }

    while (g_snd.running) {
        int refilled = 0;
        for (int i = 0; i < SND_STREAM_BUFFERS; i++) {
            if (g_snd.hdrs[i].dwFlags & WHDR_DONE) {
                waveOutUnprepareHeader(g_snd.hwo, &g_snd.hdrs[i], sizeof(WAVEHDR));
                opengl_snd_mixer_fill(g_snd.bufs[i], SND_CHUNK_SAMPLES);
                waveOutPrepareHeader(g_snd.hwo, &g_snd.hdrs[i], sizeof(WAVEHDR));
                waveOutWrite(g_snd.hwo, &g_snd.hdrs[i], sizeof(WAVEHDR));
                refilled++;
            }
        }
        if (!refilled) Sleep(5);
    }

    waveOutReset(g_snd.hwo);
    return 0;
}

/**
 * @brief Opens the waveOut device and starts the audio worker thread.
 */
static void opengl_snd_init(void)
{
    InitializeCriticalSection(&g_snd.lock);
    g_snd.running = 1;

    g_snd.wfx.wFormatTag      = WAVE_FORMAT_PCM;
    g_snd.wfx.nChannels       = 1;
    g_snd.wfx.nSamplesPerSec  = SND_SAMPLE_RATE;
    g_snd.wfx.wBitsPerSample  = 8;
    g_snd.wfx.nBlockAlign     = 1;
    g_snd.wfx.nAvgBytesPerSec = SND_SAMPLE_RATE;

    if (waveOutOpen(&g_snd.hwo, WAVE_MAPPER, &g_snd.wfx,
                    0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_snd.hwo = NULL;
        return;
    }

    for (int i = 0; i < SND_STREAM_BUFFERS; i++) {
        memset(&g_snd.hdrs[i], 0, sizeof(WAVEHDR));
        g_snd.hdrs[i].lpData         = (LPSTR)g_snd.bufs[i];
        g_snd.hdrs[i].dwBufferLength = SND_CHUNK_SAMPLES;
        waveOutPrepareHeader(g_snd.hwo, &g_snd.hdrs[i], sizeof(WAVEHDR));
    }

    g_snd.thread = CreateThread(NULL, 0, opengl_snd_thread_proc, NULL, 0, NULL);
}

static void midi_play_stop(void);

/**
 * @brief Stops audio, frees voices and closes the waveOut device.
 */
static void opengl_snd_shutdown(void)
{
    midi_play_stop();
    g_snd.running = 0;
    if (g_snd.thread) {
        WaitForSingleObject(g_snd.thread, 2000);
        CloseHandle(g_snd.thread);
        g_snd.thread = NULL;
    }
    if (g_snd.hwo) {
        waveOutReset(g_snd.hwo);
        for (int i = 0; i < SND_STREAM_BUFFERS; i++) {
            waveOutUnprepareHeader(g_snd.hwo, &g_snd.hdrs[i], sizeof(WAVEHDR));
        }
        waveOutClose(g_snd.hwo);
        g_snd.hwo = NULL;
    }
    EnterCriticalSection(&g_snd.lock);
    for (int i = 0; i < SND_MAX_VOICES; i++) {
        if (g_snd.voices[i].data) {
            free(g_snd.voices[i].data);
            g_snd.voices[i].data = NULL;
        }
        g_snd.voices[i].active = 0;
    }
    LeaveCriticalSection(&g_snd.lock);
    musdec_stop();
    DeleteCriticalSection(&g_snd.lock);
}

#define MUS_MAX_CHANNELS  16
#define MUS_MAX_VOICES    24
#define MUS_TICK_RATE     140
#define MUS_PERC_CHANNEL  15

static float opl_sin_table[1024];
static int   opl_tables_ready = 0;

/** @brief Build the OPL waveform lookup table. */
static void opl_build_tables(void) {
    if (opl_tables_ready) return;
    for (int i = 0; i < 1024; i++)
        opl_sin_table[i] = sinf((float)i * 6.283185307f / 1024.0f);
    opl_tables_ready = 1;
}

/** @brief Return a sample from the selected OPL waveform.
 *  @param waveform Waveform selector (0..7).
 *  @param phase    32-bit phase accumulator.
 *  @return Sample in the range [-1, +1]. */
static float opl_wave(int waveform, u32 phase) {
    int idx = (int)((phase >> 22) & 0x3FF);
    switch (waveform & 7) {
        case 0:
            return opl_sin_table[idx];
        case 1:
            return (idx < 512) ? opl_sin_table[idx] : 0.0f;
        case 2:
            return (opl_sin_table[idx] < 0) ? -opl_sin_table[idx] : opl_sin_table[idx];
        case 3:
            if (idx < 256)  return opl_sin_table[idx];
            if (idx < 512)  return 0.0f;
            if (idx < 768)  return opl_sin_table[idx - 512];
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
            float t = (float)idx * (1.0f / 1024.0f);
            return 2.0f * t - 1.0f;
        }
    }
    return 0.0f;
}

/** @brief Per-operator ADSR envelope state. */
typedef struct {
    float level;
    int   stage;
    float atk_inc;
    float dec_mul;
    float sus_level;
    float rel_mul;
    int   eg_flag;
} OplEnv;

/** @brief Compute the exponential decay multiplier for an OPL envelope rate.
 *  @param rate OPL2-style decay/release rate (0..15).
 *  @return Multiplier to apply per sample. */
static float opl_decay_mul(int rate) {
    if (rate <= 0) return 1.0f;
    if (rate > 15) rate = 15;
    double f = -7.4493 * (1.0 / 39.28064) / (double)SND_SAMPLE_RATE;
    return (float)pow(2.0, f * pow(2.0, (double)rate));
}

/** @brief Compute the linear attack increment for an OPL envelope rate.
 *  @param rate OPL2-style attack rate (0..15).
 *  @return Increment to apply per sample. */
static float opl_attack_inc(int rate) {
    if (rate <= 0) return 0.0f;
    if (rate >= 15) return 1.0f;
    float inc = (float)pow(2.0, (double)rate) / 32768.0f;
    inc *= 22050.0f / (float)SND_SAMPLE_RATE;
    if (inc > 1.0f) inc = 1.0f;
    return inc;
}

/** @brief Initialise an OPL envelope from operator parameters.
 *  @param e   Envelope state to initialise.
 *  @param atk Attack rate (0..15).
 *  @param dec Decay rate (0..15).
 *  @param sus Sustain level (0..15).
 *  @param rel Release rate (0..15).
 *  @param eg  Envelope type (1 = sustained, 0 = percussive). */
static void opl_env_init(OplEnv *e, int atk, int dec, int sus, int rel, int eg) {
    e->level     = 0.0f;
    e->stage     = 0;
    e->atk_inc   = opl_attack_inc(atk);
    e->dec_mul   = opl_decay_mul(dec);
    e->sus_level = (float)pow(2.0, -0.5 * (double)sus);
    e->rel_mul   = opl_decay_mul(rel > 0 ? rel : 1);
    e->eg_flag   = eg;
}

#define OPL_ENV_SILENCE 0.0005f

/** @brief Advance an OPL envelope by one sample and return its level.
 *  @param e Envelope state.
 *  @return Current envelope level in the range [0, 1]. */
static float opl_env_tick(OplEnv *e) {
    switch (e->stage) {
        case 0:
            e->level += e->atk_inc;
            if (e->level >= 1.0f) { e->level = 1.0f; e->stage = 1; }
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
            if (e->level <= OPL_ENV_SILENCE) { e->level = 0.0f; e->stage = 4; }
            break;
        case 4:
            e->level = 0.0f;
            break;
    }
    return e->level;
}

/** @brief Single operator definition parsed from a GENMIDI record. */
typedef struct {
    u8 mult;
    int ksr;
    int eg;
    int vib;
    int am;
    int volume;
    int ksl;
    int attack;
    int decay;
    int sustain;
    int release;
    int wave;
} OplOpDef;

/** @brief Two-operator voice definition from a GENMIDI record. */
typedef struct {
    OplOpDef mod;
    OplOpDef car;
    int      feedback;
    int      additive;
    int      note_offset;
} OplVoiceDef;

/** @brief Complete GENMIDI instrument (up to two voices). */
typedef struct {
    int         valid;
    int         dual;
    int         fixed;
    int         fixed_note;
    OplVoiceDef v[2];
} OplInstrument;

static OplInstrument g_genmidi_melodic[128];
static OplInstrument g_genmidi_perc[47];
static int           g_genmidi_loaded = 0;

/** @brief Parse a 16-byte GENMIDI operator definition.
 *  @param d  Raw 16-byte voice definition.
 *  @param op Output operator definition. */
static void opl_parse_op(const u8 *d, OplOpDef *op) {
    op->am      = (d[0] >> 7) & 1;
    op->vib     = (d[0] >> 6) & 1;
    op->eg      = (d[0] >> 5) & 1;
    op->ksr     = (d[0] >> 4) & 1;
    op->mult    = d[0] & 0x0F;
    op->attack  = (d[1] >> 4) & 0x0F;
    op->decay   = d[1] & 0x0F;
    op->sustain = (d[2] >> 4) & 0x0F;
    op->release = d[2] & 0x0F;
    op->wave    = d[3] & 7;
    op->ksl     = (d[4] >> 6) & 3;
    op->volume  = d[5] & 0x3F;
}

/** @brief Parse a 16-byte GENMIDI voice definition into two operators.
 *  @param v           Raw voice data.
 *  @param mod           Output modulator operator.
 *  @param car           Output carrier operator.
 *  @param feedback      Output feedback level.
 *  @param additive      Output connection type.
 *  @param note_offset   Output note transpose. */
static void opl_parse_voice_def(const u8 *v, OplOpDef *mod, OplOpDef *car,
                                 int *feedback, int *additive, int *note_offset) {
    opl_parse_op(v + 0, mod);
    *feedback = (v[6] >> 1) & 7;
    *additive = v[6] & 1;
    opl_parse_op(v + 7, car);
    *note_offset = (i16)(v[14] | (v[15] << 8));
}

/** @brief Load a GENMIDI lump into the instrument tables.
 *  @param lump     Pointer to the raw GENMIDI data.
 *  @param lump_len Size of the lump in bytes. */
static void genmidi_load(const u8 *lump, int lump_len) {
    if (!lump || lump_len < 8 + 175 * 36) return;
    if (memcmp(lump, "#OPL_II#", 8) != 0) return;

    const u8 *p = lump + 8;
    for (int i = 0; i < 175; i++, p += 36) {
        OplInstrument *inst = (i < 128) ? &g_genmidi_melodic[i]
                                        : &g_genmidi_perc[i - 128];
        u16 flags = p[0] | (p[1] << 8);
        inst->dual  = (flags & 0x04) ? 1 : 0;
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

static const float opl_mult_table[16] = {
    0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f,
    8.0f, 9.0f, 10.0f, 10.0f, 12.0f, 12.0f, 15.0f, 15.0f
};

/** @brief Running state for a single FM operator. */
typedef struct {
    u32   phase;
    u32   step;
    OplEnv env;
    int   wave;
    float out_level;
} OplOp;

/** @brief Active FM synthesizer voice. */
typedef struct {
    int   active;
    int   channel;
    int   note;
    float vol;
    int   released;
    OplOp mod;
    OplOp car;
    int   feedback;
    int   additive;
    float fb_out[2];
} FmVoice;

/** @brief Per-channel state for the MUS sequencer. */
typedef struct {
    int program;
    int volume;
    int last_velocity;
} MusChannel;

/** @brief MUS sequencer and FM synthesizer player state. */
typedef struct {
    int        playing;
    int        external;
    u8        *score;
    int        score_len;
    int        score_start;
    int        pos;
    int        delay_ticks;
    float      samples_to_tick;
    float      master;
    MusChannel ch[MUS_MAX_CHANNELS];
    FmVoice    voices[MUS_MAX_VOICES];
} MusPlayer;

/* master starts at full volume: mus_start keeps the @mixer(1) setting across
 * tracks, so a program that never calls @mixer(1) must still get audible
 * music (a {0} init left MUS/OPL output at 0 and MIDI at velocity 1). */
static MusPlayer g_mus = { .master = 1.0f };
static float mus_master_vol(void) { return g_mus.master; }

static int g_mus_no_cymbals = 0;
static int g_mus_single_voice = 0;

/** @brief Check whether a GM percussion note is a cymbal or hi-hat.
 *  @param note MIDI note number.
 *  @return Non-zero if the note is metallic percussion. */
static int mus_note_is_cymbal(int note) {
    switch (note) {
        case 42: case 44: case 46:
        case 49: case 51: case 52: case 53:
        case 55: case 57: case 59:
            return 1;
        default:
            return 0;
    }
}

/** @brief Convert a MIDI note number to an acoustic frequency (Hz).
 *  @param note MIDI note number.
 *  @return Frequency in Hz. */
static float dmx_note_freq(int note) {
    return (float)(440.0 * pow(2.0, ((double)note - 57.0) / 12.0));
}

/** @brief Configure an FM operator from a GENMIDI definition.
 *  @param op          Operator state to initialise.
 *  @param def         GENMIDI operator definition.
 *  @param freq        Base note frequency (Hz).
 *  @param mult_factor Frequency multiplier. */
static void fm_voice_setup_op(OplOp *op, const OplOpDef *def, float freq, float mult_factor) {
    float op_freq = freq * mult_factor;
    op->step = (u32)((double)op_freq * 4294967296.0 / (double)SND_SAMPLE_RATE);
    op->phase = 0;
    op->wave = def->wave;
    op->out_level = (float)pow(2.0, -0.125 * (double)def->volume);
    opl_env_init(&op->env, def->attack, def->decay, def->sustain, def->release, def->eg);
}

/** @brief Allocate an FM voice slot, stealing the quietest active voice if needed.
 *  @return Pointer to the allocated voice, or NULL if none available.
 *
 *  Caller must hold g_snd.lock. */
static FmVoice *mus_alloc_voice(void) {
    FmVoice *slot = NULL;
    for (int i = 0; i < MUS_MAX_VOICES; i++) {
        if (!g_mus.voices[i].active) { slot = &g_mus.voices[i]; break; }
    }
    if (!slot) {
        float lowest = 1e9f;
        for (int i = 0; i < MUS_MAX_VOICES; i++) {
            if (g_mus.voices[i].released) {
                float e = g_mus.voices[i].car.env.level;
                if (e < lowest) { lowest = e; slot = &g_mus.voices[i]; }
            }
        }
    }
    if (!slot) {
        float lowest = 1e9f;
        for (int i = 0; i < MUS_MAX_VOICES; i++) {
            float e = g_mus.voices[i].car.env.level;
            if (e < lowest) { lowest = e; slot = &g_mus.voices[i]; }
        }
    }
    return slot;
}

/** @brief Configure an FM voice from a GENMIDI instrument definition.
 *  @param slot    Voice slot to configure.
 *  @param inst    GENMIDI instrument.
 *  @param vidx    Voice index (0 or 1).
 *  @param channel MUS channel number.
 *  @param note    MIDI note number.
 *  @param gain    Velocity × channel volume gain.
 *  @param perc    Non-zero if this is a percussion channel. */
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
    while (play_note < 0)   play_note += 12;
    while (play_note > 127) play_note -= 12;

    float freq = dmx_note_freq(play_note);

    memset(slot, 0, sizeof(*slot));
    slot->active   = 1;
    slot->channel  = channel;
    slot->note     = note;
    slot->vol      = gain;
    slot->feedback = vd->feedback;
    slot->additive = vd->additive;
    fm_voice_setup_op(&slot->mod, &vd->mod, freq, opl_mult_table[vd->mod.mult & 0xF]);
    fm_voice_setup_op(&slot->car, &vd->car, freq, opl_mult_table[vd->car.mult & 0xF]);
}

/** @brief Trigger a note on the FM synthesizer.
 *  @param channel MUS channel number.
 *  @param note    MIDI note number.
 *  @param velocity Note velocity (0..127).
 *
 *  Caller must hold g_snd.lock. */
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
    if (!inst && g_genmidi_loaded) {
        inst = &g_genmidi_melodic[0];
    }

    int cvol = g_mus.ch[channel].volume;
    if (cvol <= 0) cvol = 127;
    float gain = ((float)velocity / 127.0f) * ((float)cvol / 127.0f);

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

    FmVoice *slot = mus_alloc_voice();
    if (!slot) return;
    int play_note = perc ? 60 : note;
    while (play_note < 0)   play_note += 12;
    while (play_note > 127) play_note -= 12;
    float freq = dmx_note_freq(play_note);
    u32 step = (u32)((double)freq * 4294967296.0 / (double)SND_SAMPLE_RATE);
    memset(slot, 0, sizeof(*slot));
    slot->active  = 1;
    slot->channel = channel;
    slot->note    = note;
    slot->vol     = gain;
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

/** @brief Release all matching notes on the FM synthesizer.
 *  @param channel MUS channel number.
 *  @param note    MIDI note number.
 *
 *  Caller must hold g_snd.lock. */
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

/** @brief Silence every active voice on a MUS channel.
 *  @param channel MUS channel number.
 *
 *  Caller must hold g_snd.lock. */
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

/** @brief Read a MUS variable-length quantity from the score.
 *  @return Decoded integer value. */
static int mus_read_varlen(void) {
    int value = 0;
    while (g_mus.pos < g_mus.score_len) {
        u8 b = g_mus.score[g_mus.pos++];
        value = (value << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return value;
}

/** @brief Advance the MUS sequencer by one tick.
 *
 *  Caller must hold g_snd.lock. */
static void mus_do_tick(void) {
    if (!g_mus.playing || !g_mus.score) return;
    if (g_mus.delay_ticks > 0) { g_mus.delay_ticks--; return; }

    while (g_mus.playing) {
        if (g_mus.pos >= g_mus.score_len) {
            g_mus.pos = g_mus.score_start;
            g_mus.delay_ticks = 0;
            return;
        }
        u8 event = g_mus.score[g_mus.pos++];
        int type    = (event >> 4) & 7;
        int channel = event & 0x0F;
        int last    = event & 0x80;

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
            if (g_mus.pos < g_mus.score_len) val  = g_mus.score[g_mus.pos++] & 0x7F;
            if (ctrl == 0)      g_mus.ch[channel].program = val;
            else if (ctrl == 3) g_mus.ch[channel].volume  = val;
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

static const double opl_fb_scale[8] = {
    0.0,
    134217728.0,
    268435456.0,
    536870912.0,
    1073741824.0,
    2147483648.0,
    4294967296.0,
    8589934592.0,
};

static double g_opl_mod_depth = (4.0 * 4294967296.0);

/** @brief Generate one sample for an active FM voice.
 *  @param v Voice state.
 *  @return Sample in the range [-1, +1]. */
static float fm_voice_sample(FmVoice *v) {
    float mod_env = opl_env_tick(&v->mod.env);
    float car_env = opl_env_tick(&v->car.env);

    float fb = 0.0f;
    if (v->feedback > 0) {
        fb = (v->fb_out[0] + v->fb_out[1]) * 0.5f;
    }
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

/** @brief Mix active FM voices into a PCM buffer.
 *  @param dst Output buffer (8-bit unsigned, centered at 128).
 *  @param n   Number of samples to render.
 *
 *  Caller must hold g_snd.lock. */
static void mus_render(u8 *dst, int n) {
    if (!g_mus.playing) return;
    const float amp = 32.0f;

    if (g_mus.external) {
        for (int i = 0; i < n; i++) {
            float acc = 0.0f;
            for (int vi = 0; vi < MUS_MAX_VOICES; vi++) {
                FmVoice *v = &g_mus.voices[vi];
                if (!v->active) continue;
                acc += fm_voice_sample(v) * v->vol;
            }
            int mixed = (int)dst[i] - 128 + (int)(acc * amp * g_mus.master);
            if (mixed > 127)  mixed = 127;
            if (mixed < -128) mixed = -128;
            dst[i] = (u8)(mixed + 128);
        }
        return;
    }

    if (!g_mus.score) return;
    const float samples_per_tick = (float)SND_SAMPLE_RATE / (float)MUS_TICK_RATE;

    int rendered = 0;
    while (rendered < n) {
        if (g_mus.samples_to_tick <= 0.0f) {
            mus_do_tick();
            g_mus.samples_to_tick += samples_per_tick;
        }
        int block = n - rendered;
        if ((float)block > g_mus.samples_to_tick)
            block = (int)g_mus.samples_to_tick;
        if (block <= 0) block = 1;

        for (int i = 0; i < block; i++) {
            float acc = 0.0f;
            for (int vi = 0; vi < MUS_MAX_VOICES; vi++) {
                FmVoice *v = &g_mus.voices[vi];
                if (!v->active) continue;
                acc += fm_voice_sample(v) * v->vol;
            }
            int idx = rendered + i;
            int mixed = (int)dst[idx] - 128 + (int)(acc * amp * g_mus.master);
            if (mixed > 127)  mixed = 127;
            if (mixed < -128) mixed = -128;
            dst[idx] = (u8)(mixed + 128);
        }
        g_mus.samples_to_tick -= (float)block;
        rendered += block;
    }
}

/** @brief Start playing a MUS score from a memory buffer.
 *  @param buf Pointer to the MUS lump.
 *  @param len Size of the lump in bytes.
 *  @return 1 on success, 0 on failure. */
static int mus_start(const u8 *buf, int len) {
    if (!buf || len < 16) return 0;
    if (buf[0] != 'M' || buf[1] != 'U' || buf[2] != 'S' || buf[3] != 0x1A)
        return 0;
    int score_len   = buf[4] | (buf[5] << 8);
    int score_start = buf[6] | (buf[7] << 8);
    if (score_start < 16 || score_start >= len) return 0;
    int end = score_start + score_len;
    if (score_len <= 0 || end > len) end = len;

    opl_build_tables();

    u8 *copy = (u8*)malloc(len);
    if (!copy) return 0;
    memcpy(copy, buf, len);

    EnterCriticalSection(&g_snd.lock);
    if (g_mus.score) free(g_mus.score);
    float saved_master = g_mus.master;
    memset(&g_mus, 0, sizeof(g_mus));
    g_mus.score        = copy;
    g_mus.score_len    = end;
    g_mus.score_start  = score_start;
    g_mus.pos          = score_start;
    g_mus.delay_ticks  = 0;
    g_mus.samples_to_tick = 0.0f;
    g_mus.master       = saved_master;
    for (int i = 0; i < MUS_MAX_CHANNELS; i++) {
        g_mus.ch[i].volume = 127;
        g_mus.ch[i].last_velocity = 100;
    }
    g_mus.playing      = 1;
    LeaveCriticalSection(&g_snd.lock);
    return 1;
}

/** @brief Stop the MUS player and free its score buffer. */
static void mus_stop(void) {
    EnterCriticalSection(&g_snd.lock);
    g_mus.playing = 0;
    g_mus.external = 0;
    for (int i = 0; i < MUS_MAX_VOICES; i++) g_mus.voices[i].active = 0;
    if (g_mus.score) { free(g_mus.score); g_mus.score = NULL; }
    LeaveCriticalSection(&g_snd.lock);
}

#define MIDI_MAX_EVENTS  32768

/** @brief Single parsed MIDI event. */
typedef struct {
    u32 tick;
    u8  status;
    u8  data1;
    u8  data2;
} MidiEvent;

/** @brief Native MIDI playback state. */
typedef struct {
    int        playing;
    HMIDIOUT   hmo;
    HANDLE     thread;
    MidiEvent *events;
    int        n_events;
    int        divisions;
    int        tempo;
} MidiPlayer;

static MidiPlayer g_midi = {0};

/** @brief Read a MIDI variable-length quantity from a buffer.
 *  @param buf Source buffer.
 *  @param len Buffer size.
 *  @param pos In/out read position.
 *  @return Decoded value. */
static u32 midi_read_vlq(const u8 *buf, int len, int *pos) {
    u32 val = 0;
    while (*pos < len) {
        u8 b = buf[(*pos)++];
        val = (val << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return val;
}

/** @brief Parse one MIDI track and append channel events to the global list.
 *  @param data Raw track data.
 *  @param tlen Track length in bytes. */
static void midi_parse_track(const u8 *data, int tlen) {
    int pos = 0;
    u32 abs_tick = 0;
    u8 running = 0;

    while (pos < tlen && g_midi.n_events < MIDI_MAX_EVENTS) {
        u32 delta = midi_read_vlq(data, tlen, &pos);
        abs_tick += delta;
        if (pos >= tlen) break;

        u8 b = data[pos];
        if (b == 0xFF) {
            pos++;
            if (pos >= tlen) break;
            u8 mtype = data[pos++];
            u32 mlen = midi_read_vlq(data, tlen, &pos);
            if (mtype == 0x51 && mlen == 3 && pos + 3 <= tlen) {
                g_midi.tempo = ((int)data[pos] << 16) |
                               ((int)data[pos+1] << 8) |
                               (int)data[pos+2];
            }
            pos += (int)mlen;
        } else if (b == 0xF0 || b == 0xF7) {
            pos++;
            u32 slen = midi_read_vlq(data, tlen, &pos);
            pos += (int)slen;
        } else {
            u8 status, d1, d2 = 0;
            if (b & 0x80) {
                status = b;
                running = b;
                pos++;
            } else {
                status = running;
            }
            if (pos >= tlen) break;
            d1 = data[pos++];
            u8 hi = status & 0xF0;
            if (hi != 0xC0 && hi != 0xD0) {
                if (pos >= tlen) break;
                d2 = data[pos++];
            }
            MidiEvent *ev = &g_midi.events[g_midi.n_events++];
            ev->tick   = abs_tick;
            ev->status = status;
            ev->data1  = d1;
            ev->data2  = d2;
        }
    }
}

/** @brief qsort comparison for MidiEvent (by tick).
 *  @param a First event.
 *  @param b Second event.
 *  @return -1, 0, or 1. */
static int midi_event_cmp(const void *a, const void *b) {
    const MidiEvent *ea = (const MidiEvent *)a;
    const MidiEvent *eb = (const MidiEvent *)b;
    if (ea->tick < eb->tick) return -1;
    if (ea->tick > eb->tick) return 1;
    return 0;
}

/** @brief Background thread: plays merged MIDI events via midiOut.
 *  @param param Unused.
 *  @return Thread exit code. */
static DWORD WINAPI midi_thread_proc(LPVOID param) {
    (void)param;
    while (g_midi.playing) {
        u32 last_tick = 0;
        for (int i = 0; i < g_midi.n_events && g_midi.playing; i++) {
            MidiEvent *ev = &g_midi.events[i];
            if (ev->tick > last_tick) {
                u32 dt = ev->tick - last_tick;
                double ms = (double)dt * ((double)g_midi.tempo / (double)g_midi.divisions) / 1000.0;
                int sleep_ms = (int)(ms + 0.5);
                if (sleep_ms > 0) {
                    while (sleep_ms > 0 && g_midi.playing) {
                        int chunk = sleep_ms > 20 ? 20 : sleep_ms;
                        Sleep(chunk);
                        sleep_ms -= chunk;
                    }
                }
                last_tick = ev->tick;
            }
            if (!g_midi.playing) break;
            u8 hi = ev->status & 0xF0;
            DWORD msg;
            if (hi == 0xC0 || hi == 0xD0) {
                msg = (DWORD)ev->status | ((DWORD)ev->data1 << 8);
            } else {
                u8 d2 = ev->data2;
                /* Scale Note On velocity by music volume.
                 * midiOutSetVolume doesn't work with the MS GS Wavetable
                 * Synth on Vista+, so we scale velocities instead. */
                if (hi == 0x90 && d2 > 0) {
                    d2 = (u8)(d2 * g_mus.master);
                    if (d2 == 0) d2 = 1;
                }
                /* Scale Channel Volume CC (controller 7) by music volume. */
                if (hi == 0xB0 && ev->data1 == 7) {
                    d2 = (u8)(d2 * g_mus.master);
                }
                msg = (DWORD)ev->status | ((DWORD)ev->data1 << 8) | ((DWORD)d2 << 16);
            }
            midiOutShortMsg(g_midi.hmo, msg);
        }
    }
    return 0;
}

/** @brief Convert a MUS music lump into an in-memory Standard MIDI File.
 *  @param mus     Pointer to the MUS lump.
 *  @param mus_len Size of the lump.
 *  @param out_len Output length of the generated MIDI buffer.
 *  @return Allocated MIDI buffer, or NULL on failure. */
static u8 *mus_to_midi(const u8 *mus, int mus_len, int *out_len) {
    if (!mus || mus_len < 16) return NULL;
    if (mus[0]!='M'||mus[1]!='U'||mus[2]!='S'||mus[3]!=0x1A) return NULL;

    int score_len   = mus[4] | (mus[5] << 8);
    int score_start = mus[6] | (mus[7] << 8);
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
    u8 *mid = (u8*)malloc(cap);
    if (!mid) return NULL;
    int wp = 0;

    memcpy(mid + wp, "MThd", 4); wp += 4;
    mid[wp++]=0; mid[wp++]=0; mid[wp++]=0; mid[wp++]=6;
    mid[wp++]=0; mid[wp++]=0;
    mid[wp++]=0; mid[wp++]=1;
    mid[wp++]=0; mid[wp++]=140;

    memcpy(mid + wp, "MTrk", 4); wp += 4;
    int trk_len_pos = wp;
    mid[wp++]=0; mid[wp++]=0; mid[wp++]=0; mid[wp++]=0;
    int trk_start = wp;

    mid[wp++]=0;
    mid[wp++]=0xFF; mid[wp++]=0x51; mid[wp++]=0x03;
    mid[wp++]=0x0F; mid[wp++]=0x42; mid[wp++]=0x40;

    u32 pending_delay = 0;
    int rp = score_start;
    while (rp < score_end && wp + 16 < cap) {
        u8 event = mus[rp++];
        int type    = (event >> 4) & 7;
        int channel = event & 0x0F;
        int last    = event & 0x80;
        int midi_ch = MUS2MIDI_CH(channel);

        if (type == 6) break;

        if (type == 0) {
            if (rp >= score_end) break;
            int note = mus[rp++] & 0x7F;
            WRITE_VLQ(pending_delay); pending_delay = 0;
            mid[wp++] = 0x80 | midi_ch;
            mid[wp++] = (u8)note;
            mid[wp++] = 64;
        } else if (type == 1) {
            if (rp >= score_end) break;
            u8 nb = mus[rp++];
            int note = nb & 0x7F;
            int vel = 127;
            if (nb & 0x80) {
                if (rp >= score_end) break;
                vel = mus[rp++] & 0x7F;
            }
            WRITE_VLQ(pending_delay); pending_delay = 0;
            mid[wp++] = 0x90 | midi_ch;
            mid[wp++] = (u8)note;
            mid[wp++] = (u8)vel;
        } else if (type == 2) {
            if (rp >= score_end) break;
            int bend_val = mus[rp++];
            int midi_bend = bend_val * 64;
            WRITE_VLQ(pending_delay); pending_delay = 0;
            mid[wp++] = 0xE0 | midi_ch;
            mid[wp++] = midi_bend & 0x7F;
            mid[wp++] = (midi_bend >> 7) & 0x7F;
        } else if (type == 3) {
            if (rp >= score_end) break;
            int ctrl = mus[rp++] & 0x7F;
            WRITE_VLQ(pending_delay); pending_delay = 0;
            mid[wp++] = 0xB0 | midi_ch;
            if (ctrl == 10 || ctrl == 11) { mid[wp++]=123; mid[wp++]=0; }
            else if (ctrl == 12)          { mid[wp++]=120; mid[wp++]=0; }
            else if (ctrl == 14)          { mid[wp++]=121; mid[wp++]=0; }
            else                          { mid[wp++]=0;   mid[wp++]=0; }
        } else if (type == 4) {
            if (rp + 1 >= score_end) break;
            int ctrl = mus[rp++] & 0x7F;
            int val  = mus[rp++] & 0x7F;
            WRITE_VLQ(pending_delay); pending_delay = 0;
            if (ctrl == 0) {
                mid[wp++] = 0xC0 | midi_ch;
                mid[wp++] = (u8)val;
            } else {
                mid[wp++] = 0xB0 | midi_ch;
                switch (ctrl) {
                    case 1: mid[wp++]=0;  mid[wp++]=(u8)val; break;
                    case 2: mid[wp++]=1;  mid[wp++]=(u8)val; break;
                    case 3: mid[wp++]=7;  mid[wp++]=(u8)val; break;
                    case 4: mid[wp++]=10; mid[wp++]=(u8)val; break;
                    case 5: mid[wp++]=11; mid[wp++]=(u8)val; break;
                    case 6: mid[wp++]=91; mid[wp++]=(u8)val; break;
                    case 7: mid[wp++]=93; mid[wp++]=(u8)val; break;
                    case 8: mid[wp++]=64; mid[wp++]=(u8)val; break;
                    case 9: mid[wp++]=67; mid[wp++]=(u8)val; break;
                    default: mid[wp++]=0; mid[wp++]=0; break;
                }
            }
        }

        if (last) {
            u32 delay = 0;
            while (rp < score_end) {
                u8 db = mus[rp++];
                delay = (delay << 7) | (db & 0x7F);
                if (!(db & 0x80)) break;
            }
            pending_delay += delay;
        }
    }

    WRITE_VLQ(pending_delay);
    mid[wp++] = 0xFF; mid[wp++] = 0x2F; mid[wp++] = 0x00;

    int trk_len = wp - trk_start;
    mid[trk_len_pos+0] = (trk_len >> 24) & 0xFF;
    mid[trk_len_pos+1] = (trk_len >> 16) & 0xFF;
    mid[trk_len_pos+2] = (trk_len >> 8)  & 0xFF;
    mid[trk_len_pos+3] = trk_len & 0xFF;

    #undef WRITE_VLQ
    #undef MUS2MIDI_CH

    *out_len = wp;
    return mid;
}

/** @brief Stop MIDI playback and release resources. */
static void midi_play_stop(void) {
    if (!g_midi.playing && !g_midi.hmo) return;
    g_midi.playing = 0;
    if (g_midi.thread) {
        WaitForSingleObject(g_midi.thread, 3000);
        CloseHandle(g_midi.thread);
        g_midi.thread = NULL;
    }
    if (g_midi.hmo) {
        midiOutReset(g_midi.hmo);
        midiOutClose(g_midi.hmo);
        g_midi.hmo = NULL;
    }
    if (g_midi.events) {
        free(g_midi.events);
        g_midi.events = NULL;
    }
    g_midi.n_events = 0;
}

/** @brief Start MIDI playback via the native Windows GM synthesizer.
 *  @param buf Pointer to a Standard MIDI File.
 *  @param len File size in bytes.
 *  @return 1 on success, 0 on failure. */
static int midi_play_start(const u8 *buf, int len) {
    if (!buf || len < 14) return 0;
    if (buf[0] != 'M' || buf[1] != 'T' || buf[2] != 'h' || buf[3] != 'd') return 0;

    int hdr_len = (buf[4]<<24)|(buf[5]<<16)|(buf[6]<<8)|buf[7];
    int format  = (buf[8]<<8)|buf[9];
    int ntracks = (buf[10]<<8)|buf[11];
    int division = (buf[12]<<8)|buf[13];
    (void)hdr_len;

    if (format > 1 || ntracks < 1 || division <= 0) return 0;

    g_midi.events = (MidiEvent*)malloc(MIDI_MAX_EVENTS * sizeof(MidiEvent));
    if (!g_midi.events) return 0;
    g_midi.n_events = 0;
    g_midi.divisions = division;
    g_midi.tempo = 500000;

    int pos = 8 + hdr_len;
    for (int t = 0; t < ntracks && pos + 8 <= len; t++) {
        if (buf[pos]!='M'||buf[pos+1]!='T'||buf[pos+2]!='r'||buf[pos+3]!='k') {
            pos += 8; continue;
        }
        int tlen = (buf[pos+4]<<24)|(buf[pos+5]<<16)|(buf[pos+6]<<8)|buf[pos+7];
        pos += 8;
        if (pos + tlen > len) tlen = len - pos;
        midi_parse_track(buf + pos, tlen);
        pos += tlen;
    }

    if (g_midi.n_events == 0) { free(g_midi.events); g_midi.events = NULL; return 0; }

    qsort(g_midi.events, g_midi.n_events, sizeof(MidiEvent), midi_event_cmp);

    if (midiOutOpen(&g_midi.hmo, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        free(g_midi.events); g_midi.events = NULL;
        return 0;
    }

    g_midi.playing = 1;
    g_midi.thread = CreateThread(NULL, 0, midi_thread_proc, NULL, 0, NULL);
    if (!g_midi.thread) {
        g_midi.playing = 0;
        midiOutClose(g_midi.hmo); g_midi.hmo = NULL;
        free(g_midi.events); g_midi.events = NULL;
        return 0;
    }
    return 1;
}

/** @brief Simple tone segment descriptor for preset sounds. */
typedef struct { int f0, f1, ms; } OpenGLSndSeg;

/** @brief Play a sequence of tone segments asynchronously.
 *  @param segs  Array of segment descriptors.
 *  @param n     Number of segments.
 *  @param volume Master volume (0..255). */
static void opengl_snd_play_segs_async(const OpenGLSndSeg *segs, int n, int volume)
{
    int total = 0;
    for (int i = 0; i < n; i++)
        total += (SND_SAMPLE_RATE * segs[i].ms) / 1000 + 1;
    u8 *buf = (u8*)malloc(total);
    if (!buf) return;
    int pos = 0;
    for (int i = 0; i < n; i++)
        pos += opengl_snd_fill_sweep(buf + pos, total - pos,
                                     segs[i].f0, segs[i].f1, segs[i].ms, volume);
    opengl_snd_start_voice(buf, pos, 255);
    free(buf);
}

/** @brief VM @tone: play a tone at a specific frequency and duration.
 *  @param vm VM state (r[1]=freq, r[2]=ms, r[3]=volume). */
static void opengl_snd_tone_freq(RelicVM *vm) {
    int freq   = (int)(u16)vm->r[1];
    int ms     = (int)(u16)vm->r[2];
    int volume = (int)(u8) vm->r[3];
    if (freq < 20) freq = 20;
    if (ms   < 1)  ms   = 1;
    if (volume < 0)   volume = 0;
    if (volume > 255) volume = 255;
    int samples = (SND_SAMPLE_RATE * ms) / 1000 + 1;
    u8 *buf = (u8*)malloc(samples);
    if (!buf) return;
    int len = opengl_snd_fill_sweep(buf, samples, freq, freq, ms, volume);
    int scaled = (int)(255 * g_sfx_vol);
    if (scaled > 255) scaled = 255;
    opengl_snd_start_voice(buf, len, scaled);
    free(buf);
}

/** @brief VM @tone_id: play a preset sound effect by ID.
 *  @param vm VM state (r[1]=id, r[2]=volume). */
static void opengl_snd_tone_id(RelicVM *vm) {
    u8 id = (u8)vm->r[1];
    int volume = (int)(u8)vm->r[2];
    if (volume < 0)   volume = 0;
    if (volume > 255) volume = 255;

    switch (id) {
        case 0: { OpenGLSndSeg s[] = { {300,120,60},{120,60,80},{60,40,100},{40,30,120} };
                  opengl_snd_play_segs_async(s, 4, (int)(volume * g_sfx_vol)); break; }
        case 1: { OpenGLSndSeg s[] = { {1600,200,120} };
                  opengl_snd_play_segs_async(s, 1, (int)(volume * g_sfx_vol)); break; }
        case 2: { OpenGLSndSeg s[] = { {2000,1600,25},{1600,1200,20} };
                  opengl_snd_play_segs_async(s, 2, (int)(volume * g_sfx_vol)); break; }
        case 3: { OpenGLSndSeg s[] = { {1000,600,40},{600,400,30} };
                  opengl_snd_play_segs_async(s, 2, (int)(volume * g_sfx_vol)); break; }
        case 4: { OpenGLSndSeg s[] = { {500,200,50},{200,100,70} };
                  opengl_snd_play_segs_async(s, 2, (int)(volume * g_sfx_vol)); break; }
        case 5: { OpenGLSndSeg s[] = { {330,330,60},{440,440,60},{550,550,60},{660,660,60},{880,880,90} };
                  opengl_snd_play_segs_async(s, 5, (int)(volume * g_sfx_vol)); break; }
        case 6: { OpenGLSndSeg s[] = { {250,900,110} };
                  opengl_snd_play_segs_async(s, 1, (int)(volume * g_sfx_vol)); break; }
        case 7: { OpenGLSndSeg s[] = { {880,660,80},{660,494,80},{494,330,90},{330,220,100},{220,110,130} };
                  opengl_snd_play_segs_async(s, 5, (int)(volume * g_sfx_vol)); break; }
        case 8: { OpenGLSndSeg s[] = { {988,988,55},{1319,1319,110} };
                  opengl_snd_play_segs_async(s, 2, (int)(volume * g_sfx_vol)); break; }
        case 9: { OpenGLSndSeg s[] = { {523,523,80},{659,659,80},{784,784,80},{1047,1047,180} };
                  opengl_snd_play_segs_async(s, 4, (int)(volume * g_sfx_vol)); break; }
        case 10: { OpenGLSndSeg s[] = { {300,150,40},{150,80,60},{80,50,60} };
                   opengl_snd_play_segs_async(s, 3, (int)(volume * g_sfx_vol)); break; }
        case 11: { OpenGLSndSeg s[] = { {400,1600,100},{1600,400,100} };
                   opengl_snd_play_segs_async(s, 2, (int)(volume * g_sfx_vol)); break; }
        default: break;
    }
}

/** @brief VM @play: play a raw PCM sample from VM memory.
 *  @param vm VM state (r[1]=ptr, r[2]=len, r[3]=volume). */
static void opengl_snd_play(RelicVM *vm) {
    u32 ptr = (u32)vm->r[1];
    u32 len = (u32)vm->r[2];
    int volume = (int)(u8)vm->r[3];
    if (ptr + len > vm->mem_sz) return;
    if (volume < 0)   volume = 0;
    if (volume > 255) volume = 255;

    int eff_vol = (int)(volume * g_sfx_vol);
    if (eff_vol > 255) eff_vol = 255;
    if (eff_vol < 255) {
        u8 *scaled = (u8*)malloc(len);
        if (!scaled) {
            opengl_snd_start_voice(vm->mem + ptr, (int)len, eff_vol);
            return;
        }
        for (u32 i = 0; i < len; i++) {
            int sample = (int)vm->mem[ptr + i];
            int offset = sample - 128;
            offset = (offset * eff_vol) / 255;
            scaled[i] = (u8)(128 + offset);
        }
        opengl_snd_start_voice(scaled, (int)len, 255);
        free(scaled);
    } else {
        opengl_snd_start_voice(vm->mem + ptr, (int)len, 255);
    }
}

/** @brief VM @music: control music playback.
 *  @param vm VM state (r[1]=ptr, r[2]=len, r[3]=cmd). */
static void opengl_snd_music(RelicVM *vm) {
    u32 ptr = (u32)vm->r[1];
    u32 len = (u32)vm->r[2];
    int cmd = vm->r[3];
    if (cmd == 0) {
        mus_stop();
        midi_play_stop();
        musdec_stop();
        return;
    }
    if (ptr + len > vm->mem_sz) return;

    const u8 *buf = vm->mem + ptr;

    if (cmd == 3) {
        genmidi_load(buf, (int)len);
        return;
    }

    mus_stop();
    midi_play_stop();
    /* Ogg Vorbis / MP3 / FLAC / WAV: decoded and streamed into the mixer
     * (looping); replaces any previous stream on success. */
    if (musdec_start(buf, (int)len)) return;
    musdec_stop();

    int is_midi = (len >= 4 && buf[0]=='M' && buf[1]=='T' && buf[2]=='h' && buf[3]=='d');
    int is_mus  = (len >= 4 && buf[0]=='M' && buf[1]=='U' && buf[2]=='S' && buf[3]==0x1A);

    g_mus_no_cymbals = 0;
    g_mus_single_voice = 0;
    g_opl_mod_depth = (4.0 * 4294967296.0);

    if (cmd == 2) {
        if (is_midi) {
            midi_play_start(buf, (int)len);
        } else if (is_mus) {
            int midi_len = 0;
            u8 *midi_buf = mus_to_midi(buf, (int)len, &midi_len);
            if (midi_buf) {
                if (!midi_play_start(midi_buf, midi_len))
                    mus_start(buf, (int)len);
                free(midi_buf);
            } else {
                mus_start(buf, (int)len);
            }
        }
        return;
    }

    if (is_mus) {
        mus_start(buf, (int)len);
    } else if (is_midi) {
        midi_play_start(buf, (int)len);
    }
}

/** @brief VM @mixer: control the sound mixer.
 *  @param vm VM state (r[1]=cmd, r[2]=value).
 *  cmd=0: set master volume (0..255)
 *  cmd=1: set music volume (0..255)
 *  cmd=2: set SFX volume (0..255)
 *  cmd=3: mute (value=0) / unmute (value!=0) */
static void opengl_snd_mixer(RelicVM *vm) {
    u8 cmd   = (u8)vm->r[1];
    u8 value = (u8)vm->r[2];
    float f  = (float)value / 255.0f;

    switch (cmd) {
        case 0: g_master_vol = f; break;
        case 1:
            EnterCriticalSection(&g_snd.lock);
            g_mus.master = f;
            LeaveCriticalSection(&g_snd.lock);
            break;
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

/* =========================================================================
 * Sector-based world renderer (@wr_*)
 *
 * The renderer engine itself lives in wr_render.h (shared verbatim with
 * every other backend that supports @wr_*, including allegro-fixed-dos on
 * real 486-era hardware). This section provides only what this backend
 * has that DOS doesn't: no persistent 8bpp hardware framebuffer to write
 * palette-index bytes into, so wr_render/wr_patch/wr_test instead draw
 * into a private heap-allocated 8bpp scratch surface (wr_surf), which is
 * then expanded through a 256-entry RGBA lookup table and presented
 * immediately as an ordinary GL textured quad - see wr_host_present.
 * Drawing immediately (rather than deferring to @flip) means it composes
 * correctly with any other @gfx_* calls in the same frame under this
 * backend's normal immediate-mode draw order, exactly like @sprite. See
 * wr_render.h's top comment for the full portability contract.
 * ---------------------------------------------------------------------- */
#define WrHostState OpenGLState

/* Lazily (re)allocates wr_surf to the window's current logical size.
 * Used as WR_HAS_BUF(s) below, so every renderer entry point that checks
 * "is there a buffer?" also ensures it exists and is correctly sized -
 * unlike DOS, where the backbuffer already exists before any @wr_* call
 * can run. Idempotent: a no-op re-check when the size hasn't changed. */
static bool wr_gl_ensure_surface(OpenGLState *s) {
    /* When an embedded host (e.g. TIDE) has redirected the guest's logical
     * window into a sub-rectangle via @view, size the 8bpp scratch surface
     * to the guest's logical dimensions (view_w x view_h), not the host
     * window. The wr_* renderer works in the guest's coordinate space
     * (e.g. 320x200 for adoom); wr_host_present then blits the result to
     * the viewport rect. Without this, the renderer uses the host window
     * size (e.g. 1200x800) for projection math, producing a wrong aspect
     * ratio / FOV and a ceiling that fills the screen. */
    int w = s->width, h = s->height;
    if (s->view_w > 0 && s->view_h > 0) { w = s->view_w; h = s->view_h; }
    /* A multi-VM child slice (gst_on) draws in its own logical surface
     * (gst_lw x gst_lh); opengl_guest_begin clears view_w, so without
     * this a docked child would render at the host window's size. */
    if (s->gst_on && s->gst_lw > 0 && s->gst_lh > 0) {
        w = s->gst_lw; h = s->gst_lh;
    }
    if (w <= 0 || h <= 0) return false;
    if (s->wr_surf && s->wr_surf_w == w && s->wr_surf_h == h) return true;
    unsigned char *buf = (unsigned char *) calloc((size_t) w * (size_t) h, 1);
    if (!buf) return false;
    if (s->wr_surf) free(s->wr_surf);
    s->wr_surf = buf;
    s->wr_surf_w = w;
    s->wr_surf_h = h;
    s->wr_surf_pitch = w;
    return true;
}

#define WR_HAS_BUF(s)   wr_gl_ensure_surface(s)
#define WR_ROW(s, y)    ((s)->wr_surf + (size_t) (y) * (size_t) (s)->wr_surf_pitch)
#define WR_PITCH(s, y)  ((s)->wr_surf_pitch)
#define WR_BUF_W(s)     ((s)->wr_surf_w)
#define WR_BUF_H(s)     ((s)->wr_surf_h)
#define WR_MILLIS()     ((int) timeGetTime())

/* Clears the sub-rectangle [ox,oy,ox+w,oy+h) of the scratch surface to
 * palette index 0. wr_surf is always a plain linear heap buffer (never a
 * video/planar bitmap, unlike DOS), so a per-row memset always applies -
 * no video-bitmap fallback needed. */
static void wr_host_clear_backbuf(OpenGLState *s, int ox, int oy, int w, int h) {
    int y;
    for (y = 0; y < h; y++) {
        memset(WR_ROW(s, y + oy) + ox, 0, (size_t) w);
    }
}

/* Called once after @wr_palette copies a fresh PLAYPAL into
 * s->wr_playpal. There is no hardware palette on this backend, so
 * instead this rebuilds the RGBA lookup table wr_host_present uses to
 * expand palette indices to real colour when it uploads the scratch
 * surface as a GL texture. */
static void wr_host_apply_palette(OpenGLState *s) {
    int i;
    for (i = 0; i < 256; i++) {
        u32 r = s->wr_playpal[i * 3 + 0];
        u32 g = s->wr_playpal[i * 3 + 1];
        u32 b = s->wr_playpal[i * 3 + 2];
        s->wr_rgba_lut[i] = r | (g << 8) | (b << 16) | (0xFFu << 24);
    }
}

/* No backend-specific fixup needed after a @wr_map upload - wr_surf is
 * always a plain linear buffer, so there is no DOS-style planar/page-
 * flip backbuffer swap to do. */
static void wr_host_on_map_uploaded(OpenGLState *s) {
    (void)s;
}

/* Expands the sub-rectangle [x,y,w,h) of the 8bpp scratch surface
 * through wr_rgba_lut and draws it as one textured quad, immediately -
 * see the file-level comment above for why immediate (not deferred to
 * @flip) is what makes @wr_render/@wr_patch composite correctly with
 * ordinary @gfx_* calls in the same frame. Uses GL_NEAREST filtering to
 * keep the chunky low-res pixels crisp when the window is larger than
 * the logical resolution, matching every other backend's presentation of
 * this renderer at native (unscaled) pixels. */
static void wr_host_present(OpenGLState *s, int x, int y, int w, int h) {
    if (!s->hwnd || !s->wr_surf || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->wr_surf_w) w = s->wr_surf_w - x;
    if (y + h > s->wr_surf_h) h = s->wr_surf_h - y;
    if (w <= 0 || h <= 0) return;

    opengl_ensure_2d(s);

    /* Grow-only POT texture, reused across every present call this
     * session (world render and every HUD/weapon/text patch alike) -
     * avoids a glGenTextures/glDeleteTextures churn on backends where
     * @wr_patch can run dozens of times per frame. */
    int pot_w = next_power_of_2(w > s->wr_present_pot_w ? w : s->wr_present_pot_w);
    int pot_h = next_power_of_2(h > s->wr_present_pot_h ? h : s->wr_present_pot_h);
    if (!s->wr_present_tex || pot_w != s->wr_present_pot_w || pot_h != s->wr_present_pot_h) {
        if (s->wr_present_tex) glDeleteTextures(1, &s->wr_present_tex);
        glGenTextures(1, &s->wr_present_tex);
        glBindTexture(GL_TEXTURE_2D, s->wr_present_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pot_w, pot_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        s->wr_present_pot_w = pot_w;
        s->wr_present_pot_h = pot_h;
    } else {
        glBindTexture(GL_TEXTURE_2D, s->wr_present_tex);
    }

    /* Expand palette indices to RGBA into a tightly-packed scratch
     * buffer, row by row from the (possibly strided) source rect, then
     * upload in one glTexSubImage2D call. */
    u32 *rgba = (u32 *) malloc((size_t) w * (size_t) h * 4u);
    if (!rgba) return;
    int row;
    for (row = 0; row < h; row++) {
        const unsigned char *src = WR_ROW(s, y + row) + x;
        u32 *dst = rgba + (size_t) row * (size_t) w;
        int col;
        for (col = 0; col < w; col++) dst[col] = s->wr_rgba_lut[src[col]];
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);

    float u1 = (float) w / (float) pot_w;
    float v1 = (float) h / (float) pot_h;
    /* When an embedded host (e.g. TIDE) has redirected the guest's
     * logical window into a sub-rectangle of the host window, scale
     * the wr_* output (which is in the guest's coordinate space, e.g.
     * 320x200) to fill the panel's pixel rect. present_dst_* is set
     * by @view_present on the previous frame; on the first frame it
     * is 0 and we fall back to 1:1 at the @view rect. */
    float x0, x1, y0, y1;
    if (s->gst_on) {
        /* Multi-VM child: the guest-surface modelview already maps the
         * child's logical coords onto its pane, so present 1:1 in them. */
        x0 = (float) x; x1 = (float) (x + w);
        y0 = (float) (s->height - y);
        y1 = (float) (s->height - (y + h));
    } else if (s->present_dst_w > 0 && s->present_dst_h > 0) {
        /* Map the source rect (x,y,w,h) in the guest's coordinate
         * space to the panel's pixel rect. The full wr_surf is
         * view_w x view_h (e.g. 320x200); the source rect is a
         * sub-rectangle of that. Scale proportionally. */
        float sx_scale = (float) s->present_dst_w / (float) s->wr_surf_w;
        float sy_scale = (float) s->present_dst_h / (float) s->wr_surf_h;
        float dx0 = (float) x * sx_scale + (float) s->present_dst_x;
        float dx1 = (float) (x + w) * sx_scale + (float) s->present_dst_x;
        /* Flip vertically: wr_surf row 0 is the top, GL is bottom-up.
         * Match the 1:1 pattern: y0 (quad bottom) = height - y, y1
         * (quad top) = height - (y+h). Scaling multiplies the image
         * coords by sy_scale and offsets by present_dst_y. */
        float dy0 = (float) (s->height - y * sy_scale - (float) s->present_dst_y);
        float dy1 = (float) (s->height - (y + h) * sy_scale - (float) s->present_dst_y);
        x0 = dx0; x1 = dx1; y0 = dy0; y1 = dy1;
    } else if (s->view_w > 0 && s->view_h > 0) {
        /* First frame: 1:1 at the @view rect (no panel rect yet). */
        float vx = (float) s->view_x;
        float vy = (float) s->view_y;
        x0 = (float) x + vx;
        x1 = (float) (x + w) + vx;
        y0 = (float) (s->height - y - vy);
        y1 = (float) (s->height - (y + h) - vy);
    } else {
        /* Standalone: 1:1 at host window origin. */
        x0 = (float) x; x1 = (float) (x + w);
        y0 = (float) (s->height - y);
        y1 = (float) (s->height - (y + h));
    }

    GLint clamp_mode = (s->gl_version >= 120) ? GL_CLAMP_TO_EDGE : GL_CLAMP;
    glEnable(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp_mode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp_mode);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x0, y0);
    glTexCoord2f(u1,   0.0f); glVertex2f(x1, y0);
    glTexCoord2f(u1,   v1);   glVertex2f(x1, y1);
    glTexCoord2f(0.0f, v1);   glVertex2f(x0, y1);
    glEnd();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    s->frame_dirty = true;
}

#include "../common/wr/wr_render.h"

/* @palette_upload: no hardware palette here - only recorded so the
 * BBC-exact fill renderer maps palette indices to MODE 2 colours the
 * way the BBC backend does. */
static void opengl_gfx_palette_upload(RelicVM *vm) {
    u32 ptr = (u32)vm->r[1];
    i32 n = vm->r[2];
    if (!ptr || n <= 0) return;
    if (n > 16) n = 16;
    if ((u64)ptr + (u64)n * 4u > vm->mem_sz) return;
    sdf_palette(vm->mem + ptr, n);
}

/* Present the sdfill MODE 2 image over the logical 320x224 view: each
 * MODE 2 pixel is 2x1 logical pixels, nearest-filtered. The HUD band
 * below is cleared black for the app to redraw. */
/* Hires renderer: 4-px clip columns (80 vs the BBC's 20 at 16 px), the
 * exact wall order and a 128-wall stage cap, plus an FPS readout in the
 * title. The app asks for it per frame with @lines3d cap bit 26 (TROOMS
 * sets it on every tier above LOW); TGX_SDF_HIRES=1/0 forces it on/off.
 * The parts override individually: TGX_SDF_COLW=<4..32>,
 * TGX_SDF_SORT=<0|1|2>, TGX_SDF_NST=<1..128>, TGX_SDF_FPS=1. */
static int g_sdf_hires;
static void opengl_sdf_config(int want_hires) {
    static int init = 0;
    static int e_hi = -1, e_colw = 0, e_sort = -1, e_nst = 0;
    int hi;
    if (!init) {
        const char *e;
        init = 1;
        e = getenv("TGX_SDF_HIRES"); if (e && e[0]) e_hi = atoi(e) != 0;
        e = getenv("TGX_SDF_COLW");  if (e && e[0]) e_colw = atoi(e);
        e = getenv("TGX_SDF_SORT");  if (e && e[0]) e_sort = atoi(e);
        e = getenv("TGX_SDF_NST");   if (e && e[0]) e_nst = atoi(e);
    }
    hi = e_hi >= 0 ? e_hi : want_hires;
    g_sdf_hires = hi;
    sdf_set_colw(e_colw ? e_colw : hi ? 4 : SDF_COLW);
    sdf_sort = e_sort >= 0 ? e_sort : hi ? 2 : 0;
    sdf_nstcap = e_nst ? e_nst : hi ? SDF_NSTMAX : SDF_NST;
    if (sdf_nstcap < 1) sdf_nstcap = 1;
    if (sdf_nstcap > SDF_NSTMAX) sdf_nstcap = SDF_NSTMAX;
}
static int opengl_sdf_fps(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TGX_SDF_FPS");
        on = (e && e[0] == '1');
    }
    return on || g_sdf_hires;
}

static void opengl_sdf_present(OpenGLState *s, int true_mode) {
    static const u32 bbc[16] = {
        0xFF000000, 0xFF0000FF, 0xFF00FF00, 0xFF00FFFF,
        0xFFFF0000, 0xFFFF00FF, 0xFFFFFF00, 0xFFFFFFFF,
        0xFF000000, 0xFF0000FF, 0xFF00FF00, 0xFF00FFFF,
        0xFFFF0000, 0xFFFF00FF, 0xFFFFFF00, 0xFFFFFFFF
    };
    static u32 px[SDF_VROWS * 160];
    static GLuint true_tex = 0;
    int x, y;
    float sx = (float)s->width / 320.0f, sy = (float)s->height / 256.0f;
    /* sdtrue.h frames are the 320x256 design at 2x (640 x 448 view) */
    int tw = true_mode ? SDT_W : 160, th = true_mode ? SDF_VROWS * 2 : SDF_VROWS;
    int pw = true_mode ? 1024 : 256, ph = true_mode ? 512 : 256;
    GLuint *tex = true_mode ? &true_tex : &s->sdf_tex;
    if (!true_mode)
        for (y = 0; y < SDF_VROWS; y++)
            for (x = 0; x < 160; x++) px[y * 160 + x] = bbc[sdf_px(x, y)];
    s->frame_dirty = true;
    if (opengl_sdf_fps() && s->hwnd) {
        static DWORD t0 = 0; static int fr = 0;
        DWORD now = GetTickCount();
        fr++;
        if (now - t0 >= 1000) {
            char tbuf[64];
            sprintf(tbuf, "Tigryx - %d fps", fr);
            SetWindowTextA(s->hwnd, tbuf);
            fr = 0; t0 = now;
        }
    }
    opengl_ensure_2d(s);
    if (!*tex) {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pw, ph, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
    } else {
        glBindTexture(GL_TEXTURE_2D, *tex);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tw, th, GL_RGBA, GL_UNSIGNED_BYTE,
                    true_mode ? (const void *)sdt_px : (const void *)px);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    {
        float u1 = (float)tw / pw, v1 = (float)th / ph;
        float x1 = 320.0f * sx, ytop = (float)s->height,
              ybot = (float)s->height - SDF_VROWS * sy;
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, ytop);
        glTexCoord2f(u1,   0.0f); glVertex2f(x1,   ytop);
        glTexCoord2f(u1,   v1);   glVertex2f(x1,   ybot);
        glTexCoord2f(0.0f, v1);   glVertex2f(0.0f, ybot);
        glEnd();
    }
    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(0.0f, (float)s->height - SDF_VROWS * sy);
    glVertex2f((float)s->width, (float)s->height - SDF_VROWS * sy);
    glVertex2f((float)s->width, 0.0f);
    glVertex2f(0.0f, 0.0f);
    glEnd();
    glEnable(GL_BLEND);
}

static void opengl_gfx_lines3d(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    i32 mode;
    if (!s) { vm->r[1] = -1; return; }
    mode = vm->r[10];
    /* TROOMS fill mode (indexed blob + span fill): the BBC-exact
     * renderer draws the view itself and reports "already drawn" (bit
     * 30), exactly as the BBC backend does. TGX_SDF_LEGACY=1 keeps the
     * old span-record path; TGX_SDF_SORT=1 a full depth sort. */
    if ((mode & 128) && (mode & 32) && !getenv("TGX_SDF_LEGACY")) {
        i32 n, cap = vm->r[9];
        int style = (cap >> 27) & 7;
        opengl_sdf_config((cap >> 26) & 1);
        /* Texture styles 3/4 (tiers above LOW): the true-texture
         * renderer, sdtrue.h. The app passes its loaded SDXTEX.bin in
         * the span-output argument (r8), unused by the fill path. */
        if (style >= 3 && vm->r[8] > 0 && (u32)vm->r[8] < vm->mem_sz) {
            u32 tp = (u32)vm->r[8], w = (u32)vm->r[1];
            if (sdt_load(vm->mem + tp, vm->mem_sz - tp) &&
                (u64)w + 2 <= vm->mem_sz) {
                int nv = (i16)(vm->mem[w] | (vm->mem[w + 1] << 8));
                u32 wt = w + 2 + (u32)nv * 4;
                if (nv >= 0 && (u64)wt + (u64)vm->r[2] * 12 <= vm->mem_sz) {
                    n = sdt_render((const i16 *)(vm->mem + wt), vm->r[2],
                                   vm->r[3], vm->r[4], vm->r[5], vm->r[6],
                                   vm->r[7],
                                   sdf_roommask(vm->mem, vm->mem_sz, w,
                                                vm->r[2], vm->r[3], vm->r[4],
                                                vm->r[5], vm->r[6],
                                                (cap >> 16) & 255),
                                   ((mode >> 28) & 7) * 32, (cap >> 24) & 1,
                                   style);
                    if (n >= 0) {
                        /* TGX_SDT_RAW=<path>: dump the RGBA frame
                         * (SDT_H rows x SDT_W x 4) on commit
                         * TGX_SDF_RAW_FRAME (default 30). */
                        static u32 sdt_cmt;
                        const char *rp = getenv("TGX_SDT_RAW");
                        if (rp && rp[0]) {
                            const char *rf = getenv("TGX_SDF_RAW_FRAME");
                            u32 want = (rf && rf[0]) ? (u32)strtoul(rf, NULL, 10)
                                                     : 30u;
                            if (sdt_cmt == want + 1u) {
                                FILE *f = fopen(rp, "wb");
                                if (f) { fwrite(sdt_px, 4, SDT_W * SDT_H, f);
                                         fclose(f); }
                            }
                        }
                        sdt_cmt++;
                        opengl_sdf_present(s, 1);
                        vm->r[1] = n | 0x40000000;
                        return;
                    }
                }
            }
        }
        n = sdf_render(vm->mem, vm->mem_sz, (u32)vm->r[1], vm->r[2],
                       vm->r[3], vm->r[4], vm->r[5], vm->r[6], vm->r[7],
                       vm->r[9], mode);
        if (n < 0) { vm->r[1] = -1; return; }
        {
            /* TGX_SDF_RAW=<path>: the MODE 2 bytes (256 rows x 80,
             * row-major) of frame TGX_SDF_RAW_FRAME (default 30), for
             * byte comparison against a BBC screen dump. */
            const char *rp = getenv("TGX_SDF_RAW");
            if (rp && rp[0]) {
                const char *rf = getenv("TGX_SDF_RAW_FRAME");
                u32 want = (rf && rf[0]) ? (u32)strtoul(rf, NULL, 10) : 30u;
                if (sdf_cmt == want + 1u) {
                    FILE *f = fopen(rp, "wb");
                    if (f) { fwrite(sdf_fb, 1, sizeof sdf_fb, f); fclose(f); }
                }
            }
        }
        opengl_sdf_present(s, 0);
        vm->r[1] = n | 0x40000000;
        return;
    }
    relic_lines3d(vm, s->logical_w, s->logical_h);
}

/* @register RELIC_REG_WIN_* - script-owned window chrome controls for
 * WINFLAG_FULLCONTENT builds, where the caption and its system buttons
 * are gone and the script draws its own in the trailing WINDOW_BUTTONS_W
 * of the drag strip. */
static void opengl_win_ctl(RelicVM *vm) {
    OpenGLState *s = (OpenGLState*)vm->backend_data;
    if (!s || !s->hwnd) { vm->r[1] = RELIC_REG_ERR_UNSUP; return; }
    switch ((int)vm->r[1]) {
    case RELIC_REG_WIN_MINIMIZE:
        ShowWindow(s->hwnd, SW_MINIMIZE);
        vm->r[1] = RELIC_REG_OK;
        break;
    case RELIC_REG_WIN_MAXIMIZE:
        ShowWindow(s->hwnd, IsZoomed(s->hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        vm->r[1] = RELIC_REG_OK;
        break;
    case RELIC_REG_WIN_STATE:
        vm->r[1] = IsZoomed(s->hwnd) ? 1 : 0;
        break;
    default:
        vm->r[1] = RELIC_REG_ERR_UNSUP;
        break;
    }
}

RelicBackend relic_backend = {
    .init         = opengl_init,
    .shutdown     = opengl_shutdown,
    .gfx_clear    = opengl_gfx_clear,
    .gfx_sprite   = opengl_gfx_sprite,
    .gfx_plot     = opengl_gfx_plot,
    .gfx_flip     = opengl_gfx_flip,
    .gfx_line     = opengl_gfx_line,
    .gfx_window   = opengl_gfx_window,
    .gfx_polygon  = opengl_gfx_polygon,
    .gfx_polyline = opengl_gfx_polyline,
    .gfx_camera   = opengl_gfx_camera,
    .gfx_texture  = opengl_gfx_texture,
    .gfx_palette_upload = opengl_gfx_palette_upload,
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
    .gfx_light    = opengl_gfx_light,
    .gfx_fog      = opengl_gfx_fog,
    .gfx_mesh     = opengl_gfx_mesh,
    .gfx_text     = opengl_gfx_text,
    .gfx_viewport = opengl_gfx_viewport,
    .gfx_text_measure = opengl_gfx_text_measure,
    .gfx_clip     = opengl_gfx_clip,
    .gfx_draw_list = opengl_gfx_draw_list,
    .gfx_lines3d   = opengl_gfx_lines3d,
.gfx_view     = opengl_gfx_view,
.gfx_view_capture = opengl_gfx_view_capture,
.gfx_view_present = opengl_gfx_view_present,
    .gfx_ellipse              = opengl_gfx_ellipse,
    .gfx_ellipse_filled       = opengl_gfx_ellipse_filled,
    .gfx_rounded_rect         = opengl_gfx_rounded_rect,
    .gfx_rounded_rect_filled  = opengl_gfx_rounded_rect_filled,
    .inp_key      = opengl_inp_key,
    .inp_keyheld  = opengl_inp_keyheld,
    .win_ctl      = opengl_win_ctl,
    .inp_mouse    = opengl_inp_mouse,
    .inp_gamepad_count = opengl_inp_gamepad_count,
    .inp_gamepad       = opengl_inp_gamepad,
    .sys_rand     = opengl_sys_rand,
    .sys_millis   = opengl_sys_millis,
    .sys_console  = opengl_sys_console,
    .sys_fopen    = opengl_sys_fopen,
    .sys_fread    = opengl_sys_fread,
    .sys_fwrite   = opengl_sys_fwrite,
    .sys_fclose   = opengl_sys_fclose,
    .sys_fseek    = opengl_sys_fseek,
    .sys_flen     = opengl_sys_flen,
    .sys_platform = opengl_sys_platform,
    .sys_http_request = opengl_sys_http_request,
    .sys_http_poll    = opengl_sys_http_poll,
    .sys_http_status  = opengl_sys_http_status,
    .sys_http_read    = opengl_sys_http_read,
    .sys_http_close   = opengl_sys_http_close,
#ifdef TIGRYX_EXPERIMENTAL_SERVER
    .sys_serve        = nsnf_sys_serve,
    .sys_serve_reply  = nsnf_sys_serve_reply,
#endif
    .sys_clip_set     = opengl_sys_clip_set,
    .sys_clip_get     = opengl_sys_clip_get,
    .sys_proc_spawn     = opengl_sys_proc_spawn,
    .sys_proc_poll      = opengl_sys_proc_poll,
    .sys_proc_exit_code = opengl_sys_proc_exit_code,
    .sys_proc_read_out  = opengl_sys_proc_read_out,
    .sys_proc_read_err  = opengl_sys_proc_read_err,
    .sys_proc_kill      = opengl_sys_proc_kill,
    .sys_proc_close     = opengl_sys_proc_close,
    .snd_tone_freq = opengl_snd_tone_freq,
    .snd_tone_id   = opengl_snd_tone_id,
    .snd_play      = opengl_snd_play,
    .snd_music     = opengl_snd_music,
    .snd_mixer     = opengl_snd_mixer,
    /* Embedded-guest slice isolation for tigryxos: one GL context hosts
     * many VMs, so per-surface state is saved/restored per slice. */
    .gfx_guest_begin = opengl_guest_begin,
    .gfx_guest_end   = opengl_guest_end,
    .gfx_guest_depth_clear = opengl_guest_depth_clear,
    .gfx_guest_snap_capture = opengl_guest_snap_capture,
    .gfx_guest_snap_present = opengl_guest_snap_present,
    .gfx_guest_snap_free    = opengl_guest_snap_free,
    .inp_close_requested    = opengl_inp_close_requested,
};
