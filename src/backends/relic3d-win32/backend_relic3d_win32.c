/*
 * backend_relic3d_win32.c
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
 * @file backend_relic3d_win32.c
 * @brief Relic engine backend for Windows using software Relic3D rendering
 *        and WinMM audio.
 *
 * Provides windowing through the Win32 API with a GDI DIBSection-backed
 * software framebuffer, the Relic3D software rasteriser for 2D and 3D
 * graphics, audio output through the WinMM waveOut and midiOut APIs, and
 * TrueType text rendering through the Win32 GDI font engine.  No OpenGL
 * dependency; designed for Windows 95/98/ME/XP machines where hardware
 * OpenGL is unavailable or too slow.
 *
 * Supported backend APIs and their engines:
 *
 *   Graphics 2D    Software framebuffer: clear, sprites, plot, lines,
 *                  polygons, polylines; presented via GDI StretchDIBits.
 *   Graphics 3D    Relic3D software rasteriser: camera, textures, lighting
 *                  and meshes with 16-bit z-buffer, perspective-correct
 *                  texturing and Lambert shading (Stage 2).
 *   Text           Win32 GDI TrueType rendering via CreateFont/TextOut
 *                  on the memory DC backing the framebuffer.
 *   Input          Win32 message-pump keyboard and mouse state.
 *   System         RNG, millisecond timer, console output, file and asset
 *                  I/O, and a platform-info query.
 *   Sound effects  @tone and @play: a software voice mixer running over the
 *                  WinMM waveOut device.
 *   Music          @music selects one of two engines:
 *                    - an OPL2 FM synthesiser for MUS lumps;
 *                    - the native Windows General MIDI synthesiser via
 *                      midiOut, with MUS-to-MIDI conversion where needed.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <wininet.h>
#ifdef TIGRYX_EXPERIMENTAL_SERVER
/* pulls winsock2.h - WIN32_LEAN_AND_MEAN above kept winsock.h out */
#include "../common/nsnf_server.h"
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__)
#include <cpuid.h>
#if !defined(__get_cpuid)
#undef __cpuid
static void __cpuid(int cpui[4], int eax) {
    __asm__ __volatile__ (
        "cpuid"
        : "=a"(cpui[0]), "=b"(cpui[1]), "=c"(cpui[2]), "=d"(cpui[3])
        : "a"(eax), "c"(0)
        : "memory"
    );
}
#endif
#endif
#include "tigryx.h"
#include "tigryx_tier.h"
#include "tigryx_win_softcursor.h"
#include "../common/win32_clip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <float.h>

/* ---- XInput (dynamically loaded) ---- */
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

static void r3d_xinput_load(void) {
    if (g_xinput_tried) return;
    g_xinput_tried = true;
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

static i32 r3d_xinput_map_buttons(WORD xb) {
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

#ifndef MAX_PATH
#define MAX_PATH 1024
#endif
#define WIN_W  640
#define WIN_H  480
/* Texture slot table sizing. The table grows on demand (see
 * r3d_tex_reserve) instead of being a fixed array, so a small program
 * pays for 256 slots while a WAD-scale one can hold thousands. A fixed
 * 512 used to be the hard limit, and because @texture has no way to
 * report "table full" beyond returning 0 - which callers treat as "use
 * the fallback texture" - overflowing it showed up as a scene that
 * silently rendered untextured rather than as any kind of error.
 *
 * RELIC_MAX_TEXTURES is now the ceiling rather than the allocation:
 * @texture yields a u16 slot id, so 65535 is the highest addressable. */
#define RELIC_TEX_CAP_INIT 256
#define RELIC_MAX_TEXTURES 65535
#define RELIC_MAX_HTTP 16     /* max concurrent in-flight @http_request handles */

/* Sector-based world renderer (@wr_*): shared portable engine. See
 * wr_render.h for the full rationale; this pulls in the on-disk WAD map
 * structs (WrVertex etc.), WR_MAX_TEXTURES/WR_MAX_FLATS, and the
 * wr_plane_buf storage that R3DState's fields below and r3d_shutdown()
 * need. The renderer functions themselves are pulled in later, via
 * wr_render.h, once R3DState is fully defined. */
#include "../common/wr/wr_types.h"
#include "../common/drawlist/drawlist.h"
#include "../common/lines3d.h"
#include "../common/sdfill.h"
#include "../common/sdtrue.h"
#include "../common/l3fill.h"

#define RELIC_WINFLAG_STRETCH 0x00000001u
#define RELIC_WINFLAG_NO_VSYNC 0x00000004u
#define RELIC_WINFLAG_NORESIZE 0x00000010u
#define RELIC_WINFLAG_CENTER  0x00000020u
#define RELIC_WINFLAG_FULLCONTENT 0x00000040u
#define RELIC_WINFLAG_AUTO_TITLEBAR 0x00000800u
#define RELIC_WINFLAG_DARK_TITLEBAR 0x00004000u
#define RELIC_WINFLAG_CLOSE_REQUEST 0x00001000u
#define RELIC_WINFLAG_FULLSCREEN  0x00002000u
/* Top-edge drag strip for FULLCONTENT windows, minus a trailing region left
 * clickable for the script's own window buttons; see the opengl-win32 backend
 * and WINDOW_DRAG_H / WINDOW_BUTTONS_W in artifact/system/flags.tgx. */
#define RELIC_DRAG_STRIP_H 28
#define RELIC_BUTTONS_W    78
#define FONT_MAX_FACES 32

typedef union { i32 i; float f; } R3DFBits;

typedef struct {
    u8  *pixels;
    int  w, h;
    int  src_w, src_h;
    int  w_mask, h_mask;
    int  has_alpha;
} SwTexture;

/* Web / HTTP request slot (see docs/web.md). WinINet's calls block, so each
 * request runs on a worker thread that fills the slot and flips the `done`
 * flag; the single-threaded VM only polls. The WinINet handles live in the
 * slot so http_close can abort an in-flight transfer by closing them from
 * the VM thread. */
typedef struct {
    bool     in_use;
    int      generation;    /* bumped on each alloc/close; matched by the worker */
    volatile int done;      /* 0 = pending, 1 = ready, <0 = error */
    int      status;        /* HTTP status code, valid once done == 1 */
    unsigned char *data;   /* malloc'd response body (may be NULL) */
    long     data_len;
    long     read_pos;      /* @http_read cursor */
    HINTERNET hSession;    /* root handle; closing it aborts child transfers */
    HINTERNET hConnect;
    HINTERNET hRequest;
} HttpReq;

typedef struct {
    HWND          hwnd;
    HDC           hdc;
    HDC           mem_dc;
    HBITMAP       dib;
    u8           *fb;
    int           fb_w, fb_h, fb_stride;
    int           width, height;
    int           pixel_w, pixel_h;
    int           reported_width, reported_height;
    float         backing_scale;
    u8            last_key;
    bool          keys[256];
    i32           mouse_x, mouse_y, mouse_buttons, mouse_wheel;
    volatile bool quit;
    bool close_requested;  /* WINFLAG_CLOSE_REQUEST: WM_CLOSE sets this; @key_held(27) reads+clears it (one-shot) */
    bool close_pending;    /* WINFLAG_CLOSE_REQUEST: dedicated channel, @key_held(0) reads+clears it */
    bool close_chan0;      /* the script has polled @key_held(0): legacy 27 signal disabled */
    struct { char name[64]; char family[LF_FACESIZE]; char path[MAX_PATH]; HFONT hfont; bool is_bold; bool is_system; bool init; HANDLE font_mem_handle; unsigned char *font_buffer; } faces[FONT_MAX_FACES];
    int           nfaces;
    int           default_face;
    /* Grown on demand by r3d_tex_reserve; tex_cap is the number of
     * slots currently allocated. Slot 0 is reserved as "no texture". */
    SwTexture    *textures;
    int           tex_cap;
    int           ntextures;
    bool          frame_dirty;
    u32           win_flags;
    bool          stretch_enabled;
    bool          vsync_throttle;
    DWORD         last_flip_ms;
    bool          soft_cursor, cursor_was_on, cursor_saved;
    int           cursor_x, cursor_y;
    u32           cursor_save[RELIC_SOFTCUR_W * RELIC_SOFTCUR_H];
    int           logical_w, logical_h;
    int           vp_x, vp_y, vp_w, vp_h;
    int           vp_3d_x, vp_3d_y, vp_3d_w, vp_3d_h;
    float         proj[16], view[16], mvp[16];
    float         znear, zfar;
    bool          have_camera, cam_mirror;
    float         light_dir[3], light_rgb[3], light_amb[3];
    bool          light_on;
    u16          *sw_zbuf;
    int           sw_zbuf_w, sw_zbuf_h;
    bool          clip_enabled;
    int           clip_x, clip_y, clip_w, clip_h;
    char          temp_font_paths[FONT_MAX_FACES][MAX_PATH];
    int           ntemp_fonts;

    /* Web / HTTP request slots (see docs/web.md). */
    HttpReq       http[RELIC_MAX_HTTP];

    /* ---- Sector-based world renderer (@wr_*) ----
     * See src/backends/common/wr/wr_render.h for the renderer itself;
     * these are the WrHostState fields it requires, plus (after them)
     * the presentation state used to expand the 8bpp scratch surface
     * into this backend's 32bpp DIBSection framebuffer (see
     * wr_host_present below). Field names and layout mirror the other
     * backends' wr_* fields exactly, since wr_render.h is shared
     * verbatim. */
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
    WrVertex    *wr_vertices;   int wr_n_vertices;
    WrLinedef   *wr_linedefs;   int wr_n_linedefs;
    WrSidedef   *wr_sidedefs;   int wr_n_sidedefs;
    WrSector    *wr_sectors;    int wr_n_sectors;
    WrSeg       *wr_segs;       int wr_n_segs;
    WrSubsector *wr_subsectors; int wr_n_subsectors;
    WrNode      *wr_nodes;      int wr_n_nodes;
    i32 *wr_sd_tex;
    i32 *wr_sec_tex;
    bool          wr_texmap_set;
    i32           wr_sky_tex_id;
    bool          wr_flatshade;
    unsigned char wr_tex_avg_color[WR_MAX_TEXTURES];
    bool          wr_tex_avg_valid[WR_MAX_TEXTURES];
    unsigned char wr_flat_avg_color[WR_MAX_FLATS];
    bool          wr_flat_avg_valid[WR_MAX_FLATS];
    int           wr_area_x, wr_area_y, wr_area_w, wr_area_h;

    /* Presentation state: a private 8bpp scratch surface the shared
     * renderer draws into (sized to the window's logical width/height),
     * and a 256-entry BGRX lookup table rebuilt whenever @wr_palette
     * runs. There is no persistent 8bpp hardware framebuffer on this
     * backend (the DIBSection is 32bpp BGRA), so wr_host_present
     * expands the scratch surface through wr_rgba_lut into s->fb. */
    unsigned char *wr_surf;
    int           wr_surf_w, wr_surf_h, wr_surf_pitch;
    u32           wr_rgba_lut[256];
} R3DState;

static R3DState *g_state = NULL;
static RelicVM  *g_vm    = NULL;

typedef HANDLE (WINAPI *PFN_AddFontMemResourceEx)(PVOID, DWORD, PVOID, DWORD*);
typedef WINBOOL (WINAPI *PFN_RemoveFontMemResourceEx)(HANDLE);
static PFN_AddFontMemResourceEx   g_AddFontMemResourceEx   = NULL;
static PFN_RemoveFontMemResourceEx g_RemoveFontMemResourceEx = NULL;
static bool g_font_mem_funcs_checked = false;

static void init_font_mem_funcs(void) {
    if (g_font_mem_funcs_checked) return;
    g_font_mem_funcs_checked = true;
    HMODULE gdi32 = GetModuleHandleA("gdi32.dll");
    if (gdi32) {
        g_AddFontMemResourceEx   = (PFN_AddFontMemResourceEx)GetProcAddress(gdi32, "AddFontMemResourceEx");
        g_RemoveFontMemResourceEx = (PFN_RemoveFontMemResourceEx)GetProcAddress(gdi32, "RemoveFontMemResourceEx");
    }
}

static void r3d_shutdown(RelicVM *vm);
static void r3d_http_shutdown(R3DState *s);

/* Ctrl-C / close handler: with AddFontMemResourceEx (Windows 2000+) font
 * resources are per-process and auto-cleaned by the OS on exit. On Win9x
 * where AddFontMemResourceEx is unavailable, we fall back to AddFontResourceA
 * and this handler still helps clean up. */
static BOOL WINAPI r3d_ctrl_handler(DWORD ctrl_type) {
    if (g_vm && g_vm->backend_data) {
        r3d_shutdown(g_vm);
    }
    return FALSE;
}

static void r3d_pump_events(R3DState *s);
static void r3d_sync_size(R3DState *s);
static void r3d_snd_init(void);
static void r3d_snd_shutdown(void);

/* Include the sound/music subsystem from a separate compilation unit style file */
#include "backend_relic3d_snd.h"

static inline u32 fb_pack(int r, int g, int b) {
    return (u32)b | ((u32)g << 8) | ((u32)r << 16);
}

static inline void fb_put(R3DState *s, int x, int y, u32 col) {
    if ((unsigned)x >= (unsigned)s->fb_w || (unsigned)y >= (unsigned)s->fb_h) return;
    if (s->clip_enabled && (x < s->clip_x || y < s->clip_y ||
        x >= s->clip_x + s->clip_w || y >= s->clip_y + s->clip_h)) return;
    ((u32*)s->fb)[y * (s->fb_stride >> 2) + x] = col;
}

static inline u32 fb_get(R3DState *s, int x, int y) {
    if ((unsigned)x >= (unsigned)s->fb_w || (unsigned)y >= (unsigned)s->fb_h) return 0;
    return ((u32*)s->fb)[y * (s->fb_stride >> 2) + x];
}

static inline void fb_alpha_put(R3DState *s, int x, int y, int r, int g, int b, int a) {
    if (a <= 0) return;
    if (s->clip_enabled && (x < s->clip_x || y < s->clip_y ||
        x >= s->clip_x + s->clip_w || y >= s->clip_y + s->clip_h)) return;
    if (a >= 255) { fb_put(s, x, y, fb_pack(r, g, b)); return; }
    u32 dst = fb_get(s, x, y);
    int dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    int inv = 255 - a;
    fb_put(s, x, y, fb_pack((r*a+dr*inv)/255, (g*a+dg*inv)/255, (b*a+db*inv)/255));
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    R3DState *s = g_state;
    switch (msg) {
        case WM_CLOSE:
            if (s && (s->win_flags & RELIC_WINFLAG_CLOSE_REQUEST)) {
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
            if (s) { s->last_key = (u8)(wp & 0xFF); if (wp < 256) s->keys[wp] = true; }
            return 0;
        case WM_KEYUP:
            if (s && wp < 256) s->keys[wp] = false;
            return 0;
        case WM_MOUSEMOVE:
            if (s) {
                int px = (int)(i16)LOWORD(lp), py = (int)(i16)HIWORD(lp);
                if (s->stretch_enabled && s->vp_w > 0 && s->vp_h > 0) {
                    int lx = (px - s->vp_x) * s->logical_w / s->vp_w;
                    int ly = (py - s->vp_y) * s->logical_h / s->vp_h;
                    if (lx < 0) lx = 0; if (ly < 0) ly = 0;
                    if (lx >= s->logical_w) lx = s->logical_w - 1;
                    if (ly >= s->logical_h) ly = s->logical_h - 1;
                    s->mouse_x = (i32)lx; s->mouse_y = (i32)ly;
                } else {
                    s->mouse_x = (i32)(px / (s->backing_scale > 0 ? s->backing_scale : 1.0f));
                    s->mouse_y = (i32)(py / (s->backing_scale > 0 ? s->backing_scale : 1.0f));
                }
                s->mouse_buttons = ((wp & MK_LBUTTON)?1:0)|((wp & MK_RBUTTON)?2:0)|((wp & MK_MBUTTON)?4:0);
            }
            return 0;
        case WM_LBUTTONDOWN: if (s) s->mouse_buttons |= 1; return 0;
        case WM_LBUTTONUP:   if (s) s->mouse_buttons &= ~1; return 0;
        case WM_RBUTTONDOWN: if (s) s->mouse_buttons |= 2; return 0;
        case WM_RBUTTONUP:   if (s) s->mouse_buttons &= ~2; return 0;
        case WM_MBUTTONDOWN: if (s) s->mouse_buttons |= 4; return 0;
        case WM_MBUTTONUP:   if (s) s->mouse_buttons &= ~4; return 0;
        case WM_MOUSEWHEEL:
            /* Raw-delta accumulator: smooth-scroll devices emit sub-notch
             * events which per-event /WHEEL_DELTA truncates to 0, leaving
             * the wheel dead. Whole notches are emitted at the read. */
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
        case WM_NCHITTEST:
            /* FULLCONTENT has no caption to drag by; treat the top strip of
             * the client area as one, letting the resize borders win first. */
            if (s && (s->win_flags & RELIC_WINFLAG_FULLCONTENT)) {
                LRESULT ht = DefWindowProcA(hwnd, msg, wp, lp);
                if (ht != HTCLIENT) return ht;
                POINT pt;
                RECT  rc;
                pt.x = (LONG)(i16)LOWORD(lp);
                pt.y = (LONG)(i16)HIWORD(lp);
                ScreenToClient(hwnd, &pt);
                GetClientRect(hwnd, &rc);
                if (pt.y >= 0 && pt.y < RELIC_DRAG_STRIP_H &&
                    pt.x < rc.right - RELIC_BUTTONS_W) return HTCAPTION;
                return HTCLIENT;
            }
            return DefWindowProcA(hwnd, msg, wp, lp);
        case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd,&ps); EndPaint(hwnd,&ps); } return 0;
        case WM_ERASEBKGND: return 1;
        default: return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

/* Mask all x87 FP exceptions. Windows 9x leaves some FP exceptions unmasked,
 * so a divide-by-zero/invalid-op in the software rasteriser raises a hardware
 * exception that the CRT handler retries forever (100% CPU freeze). NT/XP boot
 * the FPU with all exceptions masked, yielding Inf/NaN instead. We replicate
 * that here so the software 3D / vector paths behave identically on Win9x. */
static void r3d_mask_fpu_exceptions(void) {
    unsigned short cw;
    __asm__ __volatile__ ("fnstcw %0" : "=m"(cw));
    cw |= 0x003F; /* set IM,DM,ZM,OM,UM,PM mask bits */
    __asm__ __volatile__ ("fldcw %0" : : "m"(cw));
}

static void r3d_init(RelicVM *vm) {
    r3d_mask_fpu_exceptions();
#if _WIN32_WINNT >= 0x0500
    if (AttachConsole(ATTACH_PARENT_PROCESS)) { freopen("CONOUT$","w",stdout); freopen("CONOUT$","w",stderr); }
#endif
    R3DState *s = calloc(1, sizeof(R3DState));
    s->width = WIN_W; s->height = WIN_H;
    s->reported_width = WIN_W; s->reported_height = WIN_H;
    s->backing_scale = 1.0f;
    g_state = s; g_vm = vm; vm->backend_data = s;
    timeBeginPeriod(1);
    r3d_snd_init();
    SetConsoleCtrlHandler(r3d_ctrl_handler, TRUE);
}

static void r3d_create_framebuffer(R3DState *s, int w, int h) {
    if (s->dib) { DeleteObject(s->dib); s->dib = NULL; }
    if (s->mem_dc) { DeleteDC(s->mem_dc); s->mem_dc = NULL; }
    s->mem_dc = CreateCompatibleDC(s->hdc);
    BITMAPINFO bmi; memset(&bmi,0,sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w; bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    s->dib = CreateDIBSection(s->mem_dc, &bmi, DIB_RGB_COLORS, (void**)&s->fb, NULL, 0);
    SelectObject(s->mem_dc, s->dib);
    s->fb_w = w; s->fb_h = h; s->fb_stride = w * 4;
}

static int ascii_tolower(int c) { return (c>='A'&&c<='Z')?(c+32):c; }

static bool ci_contains(const char *h, const char *n) {
    if (!n||!*n) return true; if (!h) return false;
    size_t nl=strlen(n), hl=strlen(h);
    if (nl>hl) return false;
    for (size_t i=0;i+nl<=hl;i++) {
        size_t j=0;
        for (;j<nl;j++) if (ascii_tolower((unsigned char)h[i+j])!=ascii_tolower((unsigned char)n[j])) break;
        if (j==nl) return true;
    }
    return false;
}

static bool ci_streq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *b) {
        if (ascii_tolower((unsigned char)*a) != ascii_tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
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

static void get_executable_path(char *out, int out_sz) {
    out[0]='\0'; DWORD len=GetModuleFileNameA(NULL,out,out_sz); if(len>0) out[len]='\0';
}

static int find_fonts_dir(char *out, int out_sz) {
    const char *env = getenv("TIGRYX_ARTIFACTS");
    if (env && *env) {
        char *copy=malloc(strlen(env)+1); if(copy){strcpy(copy,env);
        char *tok=strtok(copy,";"); while(tok){
            snprintf(out,out_sz,"%s",tok); DWORD a=GetFileAttributesA(out);
            if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY)){free(copy);return 1;}
            snprintf(out,out_sz,"%s/assets",tok); a=GetFileAttributesA(out);
            if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY)){free(copy);return 1;}
            snprintf(out,out_sz,"%s/fonts",tok); a=GetFileAttributesA(out);
            if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY)){free(copy);return 1;}
            tok=strtok(NULL,";");} free(copy);}
    }
    char ep[MAX_PATH]; get_executable_path(ep,MAX_PATH);
    if(ep[0]!='\0'){char*ls=strrchr(ep,'/');char*lb=strrchr(ep,'\\');
        if(lb&&(!ls||lb>ls))ls=lb; if(ls){*ls='\0';
        snprintf(out,out_sz,"%s/assets",ep); DWORD a=GetFileAttributesA(out);
        if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY))return 1;
        snprintf(out,out_sz,"%s/fonts",ep); a=GetFileAttributesA(out);
        if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY))return 1;
        snprintf(out,out_sz,"%s",ep); a=GetFileAttributesA(out);
        if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY))return 1;}}
    snprintf(out,out_sz,"assets"); DWORD a=GetFileAttributesA(out);
    if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY))return 1;
    snprintf(out,out_sz,"fonts"); a=GetFileAttributesA(out);
    if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY))return 1;
    out[0]='\0'; return 0;
}

static const char *font_family_name(const char *filename) {
    if (ci_contains(filename, "MaterialIconsOutlined") || ci_contains(filename, "Material Icons Outlined")) return "Material Icons Outlined";
    if (ci_contains(filename, "MaterialIcons") || ci_contains(filename, "Material Icons")) return "Material Icons";
    if (ci_contains(filename, "JetBrains")) return "JetBrains Mono";
    if (ci_contains(filename, "Roboto")) return "Roboto";
    return filename;
}

static unsigned r3d_be16(const unsigned char *p) {
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}
static unsigned long r3d_be32(const unsigned char *p) {
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
}

/* Read the typeface family name GDI matches against out of the font's own
 * `name` table (nameID 1).
 *
 * CreateFontA matches on this name, not on the filename, and it does not
 * fail on an unknown family - GDI quietly substitutes the closest face it
 * can find. The substitute's metrics (tmHeight / tmAscent / external leading
 * and the glyph ink box) are off by a pixel or two from the intended font,
 * which shows up as slightly-too-large line gaps and text sitting low inside
 * vertically centred controls. font_family_name() above is only a filename
 * guess and has no entry for Inter, whose files are Inter-Regular.ttf /
 * Inter-Bold.ttf but whose family is plain "Inter" - so every Win11-theme
 * label was being drawn and measured in a substituted face.
 *
 * Only the `name` table is read (seeked to directly rather than loading the
 * whole font), so this stays cheap on the low-end machines this backend
 * targets. It also works for CFF/PostScript .otf files, which have a `name`
 * table like any other sfnt. Microsoft-platform records are preferred, with
 * the Mac record as a fallback; both are reduced to their ASCII subset,
 * which covers every family name shipped.
 *
 * @return 1 and fills `out` on success, 0 if the table is missing/unusable.
 */
static int ttf_family_from_file(const char *path, char *out, int out_sz) {
    if (!path || !*path || !out || out_sz < 2) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    int ok = 0;
    unsigned char hdr[12];
    if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr)) {
        int ntables = (int)r3d_be16(hdr + 4);
        unsigned long name_off = 0, name_len = 0;
        for (int i = 0; i < ntables && i < 512; i++) {
            unsigned char rec[16];
            if (fseek(f, 12 + i * 16, SEEK_SET) != 0) break;
            if (fread(rec, 1, sizeof(rec), f) != sizeof(rec)) break;
            if (memcmp(rec, "name", 4) == 0) {
                name_off = r3d_be32(rec + 8);
                name_len = r3d_be32(rec + 12);
                break;
            }
        }
        /* 1 MB cap: real name tables are a few KB, so anything larger means
         * a corrupt header rather than a font we should try to allocate for. */
        if (name_off && name_len >= 6 && name_len <= (1UL << 20)) {
            unsigned char *tbl = (unsigned char *)malloc((size_t)name_len);
            if (tbl) {
                if (fseek(f, (long)name_off, SEEK_SET) == 0 &&
                    fread(tbl, 1, (size_t)name_len, f) == (size_t)name_len) {
                    int count = (int)r3d_be16(tbl + 2);
                    unsigned long str_base = r3d_be16(tbl + 4);
                    int best = -1, best_score = -1;
                    for (int i = 0; i < count; i++) {
                        unsigned long ro = 6 + (unsigned long)i * 12;
                        if (ro + 12 > name_len) break;
                        unsigned plat = r3d_be16(tbl + ro);
                        unsigned enc  = r3d_be16(tbl + ro + 2);
                        if (r3d_be16(tbl + ro + 6) != 1) continue;   /* nameID 1 */
                        int score = -1;
                        if (plat == 3 && enc == 1) score = 2;        /* MS Unicode BMP */
                        else if (plat == 3)        score = 1;        /* MS, other enc  */
                        else if (plat == 1)        score = 0;        /* Mac Roman      */
                        if (score > best_score) { best_score = score; best = i; }
                    }
                    if (best >= 0) {
                        unsigned long ro = 6 + (unsigned long)best * 12;
                        unsigned plat = r3d_be16(tbl + ro);
                        unsigned slen = r3d_be16(tbl + ro + 8);
                        unsigned long s0 = str_base + r3d_be16(tbl + ro + 10);
                        if (s0 + slen <= name_len) {
                            const unsigned char *sp = tbl + s0;
                            int n = 0;
                            if (plat == 1) {
                                for (unsigned k = 0; k < slen && n < out_sz - 1; k++)
                                    if (sp[k] >= 32 && sp[k] <= 126) out[n++] = (char)sp[k];
                            } else {
                                /* UTF-16BE: keep the ASCII plane only. */
                                for (unsigned k = 0; k + 1 < slen && n < out_sz - 1; k += 2)
                                    if (sp[k] == 0 && sp[k + 1] >= 32 && sp[k + 1] <= 126)
                                        out[n++] = (char)sp[k + 1];
                            }
                            out[n] = '\0';
                            ok = (n > 0);
                        }
                    }
                }
                free(tbl);
            }
        }
    }
    fclose(f);
    return ok;
}

/**
 * @brief Registers a system-provided font face (no file on disk). GDI knows
 *        these by family name (e.g. "MS Sans Serif", "Tahoma") and provides
 *        the raster/outline data natively, so no file I/O or
 *        AddFontResourceExA is needed. The family name is set directly.
 * @return 1 on success, 0 if the face table is full.
 */
static int register_system_font_face(R3DState *s, const char *name,
                                     const char *family) {
    if (s->nfaces >= FONT_MAX_FACES) return 0;
    int idx = s->nfaces;
    strncpy(s->faces[idx].name, name, sizeof(s->faces[idx].name) - 1);
    s->faces[idx].name[sizeof(s->faces[idx].name) - 1] = '\0';
    strncpy(s->faces[idx].family, family, sizeof(s->faces[idx].family) - 1);
    s->faces[idx].family[sizeof(s->faces[idx].family) - 1] = '\0';
    s->faces[idx].path[0]         = '\0';
    s->faces[idx].is_bold         = ci_contains(name, "Bold");
    s->faces[idx].is_system       = true;
    s->faces[idx].init            = true;
    s->faces[idx].hfont           = NULL;
    s->faces[idx].font_mem_handle = NULL;
    s->faces[idx].font_buffer     = NULL;
    s->nfaces++;
    return 1;
}

/**
 * @brief Checks whether a font family is actually installed on the system by
 *        asking GDI to enumerate it. CreateFontA silently substitutes a
 *        different face for an unknown family, so this is the only reliable
 *        way to tell whether a system font (e.g. Tahoma) is really present
 *        before handing its name to CreateFontA.
 * @return true if at least one font with the given family name is installed.
 */
static int CALLBACK r3d_font_enum_proc(const LOGFONTA *lf,
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
    strncpy(lf.lfFaceName, family, LF_FACESIZE - 1);
    lf.lfCharSet = DEFAULT_CHARSET;
    volatile int found = 0;
    EnumFontFamiliesExA(dc, &lf, r3d_font_enum_proc, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found != 0;
}

static int load_font_face(R3DState *s, const char *path, const char *basename) {
    if (s->nfaces >= FONT_MAX_FACES) return 0;
    int idx=s->nfaces;
    s->faces[idx].font_mem_handle = NULL;
    s->faces[idx].font_buffer = NULL;
    init_font_mem_funcs();
    if (g_AddFontMemResourceEx) {
        FILE *f = fopen(path, "rb");
        if (!f) return 0;
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsize <= 0) { fclose(f); return 0; }
        unsigned char *fbuf = (unsigned char*)malloc((size_t)fsize);
        if (!fbuf) { fclose(f); return 0; }
        if (fread(fbuf, 1, (size_t)fsize, f) != (size_t)fsize) {
            free(fbuf); fclose(f); return 0;
        }
        fclose(f);
        DWORD num_fonts = 0;
        s->faces[idx].font_mem_handle = g_AddFontMemResourceEx(fbuf, (DWORD)fsize, NULL, &num_fonts);
        if (!s->faces[idx].font_mem_handle) {
            fprintf(stderr, "relic3d-win32: AddFontMemResourceEx failed for %s\n", path);
            free(fbuf);
            return 0;
        }
        s->faces[idx].font_buffer = fbuf;
    } else {
        if (AddFontResourceA(path) == 0) return 0;
    }
    char name[64]; size_t L=strlen(basename); if(L>=sizeof(name))L=sizeof(name)-1;
    memcpy(name,basename,L); name[L]='\0';
    if(L>=4){char*dot=name+L-4;
        if(ascii_tolower((unsigned char)dot[0])=='.'&&ascii_tolower((unsigned char)dot[2])=='t'
         &&ascii_tolower((unsigned char)dot[3])=='f'
         &&(ascii_tolower((unsigned char)dot[1])=='t'||ascii_tolower((unsigned char)dot[1])=='o')) *dot='\0';}
    strncpy(s->faces[idx].name,name,sizeof(s->faces[idx].name)-1);
    s->faces[idx].name[sizeof(s->faces[idx].name)-1]='\0';
    strncpy(s->faces[idx].path,path,sizeof(s->faces[idx].path)-1);
    s->faces[idx].path[sizeof(s->faces[idx].path)-1]='\0';
    s->faces[idx].is_bold=ci_contains(s->faces[idx].name,"Bold");
    s->faces[idx].is_system=false;
    /* Read the real GDI family out of the font's own `name` table rather
     * than guessing from the filename: CreateFontA matches on this name and
     * silently substitutes a different face on a miss (e.g. every
     * "Inter-*.ttf" face, which font_family_name() has no rule for), and the
     * substitute's metrics/ink-box are visibly off from the intended font. */
    if (!ttf_family_from_file(path, s->faces[idx].family, (int)sizeof(s->faces[idx].family))) {
        snprintf(s->faces[idx].family, sizeof(s->faces[idx].family), "%.*s",
                 (int)sizeof(s->faces[idx].family) - 1, font_family_name(s->faces[idx].name));
    }
    s->faces[idx].hfont=NULL; s->faces[idx].init=true; s->nfaces++; return 1;
}

static int r3d_font_init(R3DState *s) {
    s->nfaces=0; s->default_face=-1;
    char dir[MAX_PATH]; if(!find_fonts_dir(dir,MAX_PATH)){dir[0]='\0';
        if(g_vm&&g_vm->verbose) fprintf(stderr,"relic3d-win32: no fonts directory found; trying packed assets\n");}
    char pat[MAX_PATH]; WIN32_FIND_DATAA fd; HANDLE h; bool found_any=false;
    snprintf(pat,sizeof(pat),"%s/*.ttf",dir);
    h=FindFirstFileA(pat,&fd);
    if(h!=INVALID_HANDLE_VALUE){found_any=true;
    do{if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
        char p[MAX_PATH]; snprintf(p,sizeof(p),"%s/%s",dir,fd.cFileName);
        if(!load_font_face(s,p,fd.cFileName)) if(g_vm&&g_vm->verbose) fprintf(stderr,"relic3d-win32: skipped font %s\n",fd.cFileName);
    }while(FindNextFileA(h,&fd)); FindClose(h);}
    snprintf(pat,sizeof(pat),"%s/*.otf",dir);
    h=FindFirstFileA(pat,&fd);
    if(h!=INVALID_HANDLE_VALUE){found_any=true;
    do{if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
        /* Prefer the TrueType (.ttf) icon font. GDI's CFF (.otf) rasterizer
         * leaks session font state on repeated runs and eventually makes
         * icons vanish until a Windows reboot; the TrueType path never does.
         * The .ttf scan above runs first, so if a MaterialIcons TTF face was
         * already loaded, skip the CFF .otf entirely. */
        if(ci_contains(fd.cFileName,"MaterialIcons")){
            bool have_icon_ttf=false;
            for(int i=0;i<s->nfaces;i++) if(ci_contains(s->faces[i].name,"MaterialIcons")){have_icon_ttf=true;break;}
            if(have_icon_ttf) continue;
        }
        char p[MAX_PATH]; snprintf(p,sizeof(p),"%s/%s",dir,fd.cFileName);
        if(!load_font_face(s,p,fd.cFileName)) if(g_vm&&g_vm->verbose) fprintf(stderr,"relic3d-win32: skipped font %s\n",fd.cFileName);
    }while(FindNextFileA(h,&fd)); FindClose(h);}
    if(!found_any){
        if(g_vm&&g_vm->verbose) fprintf(stderr,"relic3d-win32: no .ttf/.otf files in %s\n",dir);
        if(g_vm&&g_vm->assets){
            RelicAssets*a=g_vm->assets;
            /* Prefer the TrueType (.ttf) icon font over the CFF (.otf): the
             * GDI CFF rasterizer leaks session font state and makes icons
             * vanish until reboot. If any MaterialIcons .ttf asset exists,
             * skip every MaterialIcons .otf (asset order is not fixed). */
            bool have_icon_ttf_asset=false;
            for(int i=0;i<a->count;i++){
                const char*n=a->entries[i].name;size_t nl=strlen(n);if(nl<4)continue;
                const char*ext=n+nl-4;
                if(!(ext[0]=='.'&&ascii_tolower((unsigned char)ext[1])=='t'&&ascii_tolower((unsigned char)ext[2])=='t'&&ascii_tolower((unsigned char)ext[3])=='f'))continue;
                const char*bn=strrchr(n,'/');if(!bn)bn=strrchr(n,'\\');bn=bn?bn+1:n;
                if(ci_contains(bn,"MaterialIcons")){have_icon_ttf_asset=true;break;}
            }
            for(int i=0;i<a->count&&s->nfaces<FONT_MAX_FACES;i++){
                const char*n=a->entries[i].name;size_t nl=strlen(n);
                if(nl<4)continue;const char*ext=n+nl-4;
                bool is_ttf=(ext[0]=='.'&&ascii_tolower((unsigned char)ext[1])=='t'&&ascii_tolower((unsigned char)ext[2])=='t'&&ascii_tolower((unsigned char)ext[3])=='f');
                bool is_otf=(ext[0]=='.'&&ascii_tolower((unsigned char)ext[1])=='o'&&ascii_tolower((unsigned char)ext[2])=='t'&&ascii_tolower((unsigned char)ext[3])=='f');
                if(!is_ttf&&!is_otf)continue;
                const char*bn=strrchr(n,'/');if(!bn)bn=strrchr(n,'\\');bn=bn?bn+1:n;
                if(is_otf&&have_icon_ttf_asset&&ci_contains(bn,"MaterialIcons"))continue;
                char tmp[MAX_PATH];GetTempPathA(MAX_PATH,tmp);char tpath[MAX_PATH];
                snprintf(tpath,sizeof(tpath),"%srelic_%s",tmp,bn);
                FILE*tf=fopen(tpath,"wb");if(!tf)continue;
                fwrite(a->data+a->entries[i].blob_offset,1,a->entries[i].blob_size,tf);fclose(tf);
                if(load_font_face(s,tpath,bn)){
                    if(s->ntemp_fonts<FONT_MAX_FACES){
                        strncpy(s->temp_font_paths[s->ntemp_fonts],tpath,MAX_PATH-1);
                        s->temp_font_paths[s->ntemp_fonts][MAX_PATH-1]='\0';
                        s->ntemp_fonts++;
                    }
                } else {
                    DeleteFileA(tpath);
                }
            }
        }
        if(s->nfaces==0)return 0;
    } else if(s->nfaces==0){
        if(g_vm&&g_vm->verbose) fprintf(stderr,"relic3d-win32: no fonts loaded from %s\n",dir);return 0;
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
     * gdi_family_available() verifies each family is actually installed
     * before marking it usable, so a missing font falls back cleanly. */
    if (gdi_family_available("MS Sans Serif")) {
        register_system_font_face(s, "MS Sans Serif", "MS Sans Serif");
        register_system_font_face(s, "MS Sans Serif Bold", "MS Sans Serif");
    }
    if (gdi_family_available("Tahoma")) {
        register_system_font_face(s, "Tahoma", "Tahoma");
        register_system_font_face(s, "Tahoma Bold", "Tahoma");
    }
    /* "Segoe UI": the Windows UI font since Vista, requested by the Win11
     * theme. Absent on real Win9x - the family check skips it there and
     * select_face() falls back cleanly. */
    if (gdi_family_available("Segoe UI")) {
        register_system_font_face(s, "Segoe UI", "Segoe UI");
        register_system_font_face(s, "Segoe UI Bold", "Segoe UI");
    }
    for(int i=0;i<s->nfaces;i++) if(ci_contains(s->faces[i].name,"JetBrains")&&!s->faces[i].is_bold){s->default_face=i;break;}
    if(s->default_face<0) for(int i=0;i<s->nfaces;i++) if(ci_contains(s->faces[i].name,"JetBrains")){s->default_face=i;break;}
    if(s->default_face<0) s->default_face=0;
    return 1;
}

static void r3d_font_shutdown(R3DState *s) {
    if(!s)return;
    init_font_mem_funcs();
    for(int i=0;i<s->nfaces;i++){if(s->faces[i].hfont){DeleteObject(s->faces[i].hfont);s->faces[i].hfont=NULL;}
        if(s->faces[i].font_mem_handle){
            if(g_RemoveFontMemResourceEx) g_RemoveFontMemResourceEx(s->faces[i].font_mem_handle);
            s->faces[i].font_mem_handle=NULL;
        } else if(s->faces[i].path[0]){RemoveFontResourceA(s->faces[i].path);s->faces[i].path[0]='\0';}
        if(s->faces[i].font_buffer){free(s->faces[i].font_buffer);s->faces[i].font_buffer=NULL;}
        s->faces[i].init=false;}
    s->nfaces=0; s->default_face=-1;
    for(int i=0;i<s->ntemp_fonts;i++){if(s->temp_font_paths[i][0]){DeleteFileA(s->temp_font_paths[i]);s->temp_font_paths[i][0]='\0';}}
    s->ntemp_fonts=0;
}

static int select_face(R3DState *s, const char *name, int weight) {
    if(s->nfaces<=0)return -1; bool wb=(weight>=600);
    const char *n=(name&&*name)?name:NULL;
    if(!n){if(wb){for(int i=0;i<s->nfaces;i++)if(ci_contains(s->faces[i].name,"JetBrains")&&s->faces[i].is_bold)return i;}return s->default_face;}
    int am=-1; for(int i=0;i<s->nfaces;i++){if(!ci_contains(s->faces[i].name,n))continue;if(s->faces[i].is_bold==wb)return i;if(am<0)am=i;}
    if(am>=0)return am;
    /* If the requested font was Tahoma (the Win95 theme's preferred font,
     * which is not present on real Windows 95), fall back to MS Sans Serif
     * before trying the default face. */
    if (ci_streq(n, "Tahoma")) {
        for (int i = 0; i < s->nfaces; i++) {
            if (!ci_contains(s->faces[i].name, "MS Sans Serif")) continue;
            if (s->faces[i].is_bold == wb) return i;
        }
        for (int i = 0; i < s->nfaces; i++)
            if (ci_contains(s->faces[i].name, "MS Sans Serif")) return i;
    }
    return s->default_face;
}

/* DWM (Desktop Window Manager) ships on Vista+ but not on Win95/98/ME, so
 * dwmapi.dll is loaded dynamically. DWMWA_WINDOW_CORNER_PREFERENCE (33) is a
 * Windows 11 attribute; on older versions the call fails harmlessly and the
 * window keeps its default (square) corners. Requesting DWMWCP_ROUND (2)
 * makes Windows 11 draw the standard 8px rounded corners the OS uses for
 * top-level windows, instead of leaving them square -- which happens when
 * the heuristics the OS uses to decide corner shape do not classify our
 * window as a "normal" captioned window (e.g. WS_POPUP FULLCONTENT windows). */
#define RELIC_DWMWA_WINDOW_CORNER_PREFERENCE 33
#define RELIC_DWMWCP_ROUND                   2
typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
static PFN_DwmSetWindowAttribute g_DwmSetWindowAttribute = NULL;
static int g_dwm_funcs_checked = 0;

static void r3d_apply_rounded_corners(HWND hwnd) {
    if (!g_dwm_funcs_checked) {
        g_dwm_funcs_checked = 1;
        HMODULE dwmapi = LoadLibraryA("dwmapi.dll");
        if (dwmapi)
            g_DwmSetWindowAttribute = (PFN_DwmSetWindowAttribute)
                GetProcAddress(dwmapi, "DwmSetWindowAttribute");
    }
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
 * attribute and ignored. See opengl_apply_system_titlebar_theme() in
 * backend_opengl_win32.c for the fuller rationale - duplicated here rather
 * than shared because the two backends do not share a translation unit. */
#define RELIC_DWMWA_USE_IMMERSIVE_DARK_MODE     20
#define RELIC_DWMWA_USE_IMMERSIVE_DARK_MODE_1809 19

static void r3d_apply_system_titlebar_theme(HWND hwnd) {
    if (!g_dwm_funcs_checked) {
        g_dwm_funcs_checked = 1;
        HMODULE dwmapi = LoadLibraryA("dwmapi.dll");
        if (dwmapi)
            g_DwmSetWindowAttribute = (PFN_DwmSetWindowAttribute)
                GetProcAddress(dwmapi, "DwmSetWindowAttribute");
    }
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
static void r3d_force_dark_titlebar(HWND hwnd) {
    if (!g_dwm_funcs_checked) {
        g_dwm_funcs_checked = 1;
        HMODULE dwmapi = LoadLibraryA("dwmapi.dll");
        if (dwmapi)
            g_DwmSetWindowAttribute = (PFN_DwmSetWindowAttribute)
                GetProcAddress(dwmapi, "DwmSetWindowAttribute");
    }
    if (!g_DwmSetWindowAttribute) return;
    BOOL use_dark = TRUE;
    g_DwmSetWindowAttribute(hwnd, RELIC_DWMWA_USE_IMMERSIVE_DARK_MODE,
                            &use_dark, sizeof(use_dark));
    g_DwmSetWindowAttribute(hwnd, RELIC_DWMWA_USE_IMMERSIVE_DARK_MODE_1809,
                            &use_dark, sizeof(use_dark));
}

/* MonitorFromPoint/GetMonitorInfo are Windows 98/2000 additions and the
 * win95 build compiles at WINVER 0x0400, where HMONITOR and MONITORINFO
 * do not exist - so both are resolved dynamically and the struct is
 * spelled out locally with the same layout. On win95 the primary screen
 * metrics stand in for a single-monitor machine. rcMonitor (not rcWork)
 * is returned on purpose: fullscreen wants the whole panel, taskbar area
 * included. */
#define RELIC_MONITOR_DEFAULTTONEAREST 2
typedef struct {
    DWORD cbSize;
    RECT  rcMonitor;
    RECT  rcWork;
    DWORD dwFlags;
} R3DMonitorInfo;
typedef void* (WINAPI *PFN_R3DMonitorFromPoint)(POINT, DWORD);
typedef BOOL  (WINAPI *PFN_R3DGetMonitorInfo)(void*, R3DMonitorInfo*);
static PFN_R3DMonitorFromPoint g_MonitorFromPoint = NULL;
static PFN_R3DGetMonitorInfo   g_GetMonitorInfo   = NULL;
static int g_mon_funcs_checked = 0;

static void r3d_monitor_rect(int x, int y, RECT *out) {
    if (!g_mon_funcs_checked) {
        g_mon_funcs_checked = 1;
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (user32) {
            g_MonitorFromPoint = (PFN_R3DMonitorFromPoint)
                GetProcAddress(user32, "MonitorFromPoint");
            g_GetMonitorInfo = (PFN_R3DGetMonitorInfo)
                GetProcAddress(user32, "GetMonitorInfoA");
        }
    }
    if (g_MonitorFromPoint && g_GetMonitorInfo) {
        POINT pt; pt.x=x; pt.y=y;
        void *mon = g_MonitorFromPoint(pt, RELIC_MONITOR_DEFAULTTONEAREST);
        R3DMonitorInfo mi; mi.cbSize=sizeof(mi);
        if (mon && g_GetMonitorInfo(mon,&mi)) { *out=mi.rcMonitor; return; }
    }
    out->left=0;out->top=0;
    out->right=GetSystemMetrics(SM_CXSCREEN);
    out->bottom=GetSystemMetrics(SM_CYSCREEN);
}

/* Forward declaration: read_vm_cstr is defined later in this file. */
static int read_vm_cstr(RelicVM *vm, u32 ptr, char *out, int out_sz);

static void r3d_gfx_window(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s)return;
    /* Second call: resize the existing window (see opengl-win32 for rationale). */
    if(s->hwnd){
        int w=vm->r[3],h=vm->r[4]; if(w<=0)w=s->width; if(h<=0)h=s->height;
        /* FULLSCREEN pins the window to the monitor's bounds, so the call
         * cannot move or resize it. Only the logical render size still
         * updates (the stretch letterbox); without stretch the monitor's
         * own size is kept. */
        int fs=(s->win_flags&RELIC_WINFLAG_FULLSCREEN)!=0;
        if(fs&&!s->stretch_enabled){w=s->width;h=s->height;}
        s->width=w;s->height=h;s->logical_w=w;s->logical_h=h;
        s->reported_width=w;s->reported_height=h;
        if(!fs){
            DWORD ws=GetWindowLongA(s->hwnd,GWL_STYLE);
            DWORD ex=GetWindowLongA(s->hwnd,GWL_EXSTYLE);
            RECT r={0,0,w,h};AdjustWindowRect(&r,ws,FALSE);
            int ww=r.right-r.left,wh=r.bottom-r.top;
            SetWindowPos(s->hwnd,NULL,0,0,ww,wh,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOMOVE);
        }
        r3d_sync_size(s);
        /* Update title if a 6th arg was passed. */
        u32 title_ptr=(u32)vm->r[6];
        if(title_ptr && title_ptr<vm->mem_sz){
            char tb[256]; read_vm_cstr(vm,title_ptr,tb,sizeof(tb));
            if(tb[0]) SetWindowTextA(s->hwnd,tb);
        }
        return;
    }
    int x=vm->r[1],y=vm->r[2],w=vm->r[3],h=vm->r[4]; u32 flags=(u32)vm->r[5];
    if(w<=0)w=WIN_W; if(h<=0)h=WIN_H;
    /* FULLSCREEN: a borderless WS_POPUP window covering the whole monitor
     * (the "fullscreen windowed" model - no display-mode switch, the
     * compositor stays in charge). x,y pick the monitor; w,h remain the
     * logical render size - with STRETCH they are letterboxed onto the
     * panel, without it the logical space is the monitor's own (this
     * backend has no DPI scaling, so logical == device pixels). */
    int fs=(flags&RELIC_WINFLAG_FULLSCREEN)!=0;
    RECT mon={0,0,0,0};
    if(fs) r3d_monitor_rect(x,y,&mon);
    int mon_w=mon.right-mon.left,mon_h=mon.bottom-mon.top;
    s->width=w;s->height=h;s->pixel_w=w;s->pixel_h=h;s->reported_width=w;s->reported_height=h;
    s->win_flags=flags;s->stretch_enabled=(flags&RELIC_WINFLAG_STRETCH)!=0;
    if(fs){
        s->pixel_w=mon_w;s->pixel_h=mon_h;
        if(!s->stretch_enabled){
            s->width=mon_w;s->height=mon_h;
            s->logical_w=mon_w;s->logical_h=mon_h;
            s->reported_width=mon_w;s->reported_height=mon_h;
        }
    }
    /* The software renderer has no swap-interval mechanism, so @flip presents
     * as fast as the script's loop runs. On Windows 95/98 the display driver
     * removes and redraws the mouse pointer around every screen update (it is
     * a software cursor on most 9x-era drivers), so an unthrottled present
     * rate makes the pointer visibly flicker whenever it is over the window.
     * Pace presentation to the display refresh rate instead, which also stops
     * the loop burning a whole core. NO_VSYNC opts out (benchmarks). */
    s->vsync_throttle=(flags&RELIC_WINFLAG_NO_VSYNC)==0;
    s->last_flip_ms=0;
    /* Pacing alone does not stop the 9x driver's pointer exclusion, so on
     * those hosts also draw the pointer into the frame (see @ref
     * tigryx_win_softcursor.h). */
    s->soft_cursor=relic_win_softcursor_enabled()?true:false;
    s->cursor_was_on=false;s->cursor_saved=false;
    if(!fs||s->stretch_enabled){s->logical_w=w;s->logical_h=h;}
    s->vp_x=0;s->vp_y=0;s->vp_w=w;s->vp_h=h;
    WNDCLASSEXA wc={0};wc.cbSize=sizeof(wc);wc.style=CS_HREDRAW|CS_VREDRAW|CS_OWNDC;
    wc.lpfnWndProc=wnd_proc;wc.hInstance=GetModuleHandleA(NULL);wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    wc.hIcon=LoadIconA(wc.hInstance,MAKEINTRESOURCEA(1));
    wc.hIconSm=(HICON)LoadImageA(wc.hInstance,MAKEINTRESOURCEA(1),IMAGE_ICON,16,16,0);
    wc.lpszClassName="RelicR3DWindow";RegisterClassExA(&wc);
    /* FULLCONTENT: WS_POPUP drops the caption so the client area covers the
     * whole window, WS_THICKFRAME keeps the sizing border. The system caption
     * buttons go with the caption; such scripts draw their own. */
    DWORD win_style=WS_OVERLAPPEDWINDOW|WS_VISIBLE;
    DWORD win_ex_style=0;
    if(flags&RELIC_WINFLAG_FULLCONTENT){
        win_style=WS_POPUP|WS_VISIBLE|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX;
        win_ex_style=WS_EX_APPWINDOW;
    }
    if(flags&RELIC_WINFLAG_NORESIZE) win_style&=~(WS_THICKFRAME|WS_MAXIMIZEBOX);
    /* Fullscreen wins over every other style bit: plain WS_POPUP so the
     * client rect IS the monitor rect, with no sizing border to inflate
     * it past the panel. WS_EX_APPWINDOW keeps a taskbar entry. */
    if(fs){win_style=WS_POPUP|WS_VISIBLE;win_ex_style=WS_EX_APPWINDOW;}
    RECT r={0,0,s->pixel_w,s->pixel_h};AdjustWindowRect(&r,win_style,FALSE);
    int ww=r.right-r.left,wh=r.bottom-r.top;
    /* Honour x,y literally; WINFLAG_CENTER centres the outer rect inside
     * the primary monitor's work area (excludes the taskbar). FULLSCREEN
     * pins the window to the monitor's own bounds instead. */
    int px=x,py=y;
    if(fs){px=mon.left;py=mon.top;}
    else if(flags&RELIC_WINFLAG_CENTER){
        RECT work;
        if(SystemParametersInfoA(SPI_GETWORKAREA,0,&work,0)){
            int work_w=work.right-work.left,work_h=work.bottom-work.top;
            px=work.left+(work_w-ww)/2;
            py=work.top +(work_h-wh)/2;
            if(px<work.left)px=work.left;
            if(py<work.top) py=work.top;
        }
    }
    /* Window title: read from the optional 6th @window argument (r[6]).
     * Falls back to "Tigryx" if not provided. */
    char title_buf[256]="Tigryx";
    {
        u32 title_ptr=(u32)vm->r[6];
        if(title_ptr && title_ptr<vm->mem_sz){
            read_vm_cstr(vm,title_ptr,title_buf,sizeof(title_buf));
            if(!title_buf[0]){title_buf[0]='R';title_buf[1]='e';title_buf[2]='l';title_buf[3]='y';title_buf[4]='x';title_buf[5]=0;}
        }
    }
    s->hwnd=CreateWindowExA(win_ex_style,"RelicR3DWindow",title_buf,win_style,px,py,ww,wh,NULL,NULL,GetModuleHandleA(NULL),NULL);
    if(!s->hwnd){fprintf(stderr,"CreateWindowExA failed\n");return;}
    if(!fs){
        r3d_apply_rounded_corners(s->hwnd);
        if(flags&RELIC_WINFLAG_DARK_TITLEBAR) r3d_force_dark_titlebar(s->hwnd);
        else if(flags&RELIC_WINFLAG_AUTO_TITLEBAR) r3d_apply_system_titlebar_theme(s->hwnd);
    }
    s->hdc=GetDC(s->hwnd);
    /* Stretch keeps the framebuffer at the logical size and scales it into
     * the letterbox viewport at present; everything else matches the
     * client size (the monitor's, in fullscreen). */
    r3d_create_framebuffer(s,s->stretch_enabled?w:s->width,
                           s->stretch_enabled?h:s->height);
    MSG msg; while(PeekMessageA(&msg,NULL,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageA(&msg);}
    s->last_key=0;
    if(!r3d_font_init(s)) fprintf(stderr,"Warning: Font initialization failed\n");
}

static void r3d_shutdown(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s)return;
    r3d_snd_shutdown(); r3d_font_shutdown(s);
    r3d_http_shutdown(s);
#ifdef TIGRYX_EXPERIMENTAL_SERVER
    nsnf_serve_shutdown(vm);
#endif
    if(s->textures){
        for(int i=0;i<s->tex_cap;i++) if(s->textures[i].pixels){free(s->textures[i].pixels);s->textures[i].pixels=NULL;}
        free(s->textures);s->textures=NULL;s->tex_cap=0;
    }
    if(s->sw_zbuf){free(s->sw_zbuf);s->sw_zbuf=NULL;}
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
        if (s->wr_surf)       { free(s->wr_surf);       s->wr_surf = NULL; }
        if (wr_plane_buf)     { free(wr_plane_buf);     wr_plane_buf = NULL; wr_plane_buf_w = 0; }
    }
    if(s->dib){DeleteObject(s->dib);s->dib=NULL;}
    if(s->mem_dc){DeleteDC(s->mem_dc);s->mem_dc=NULL;}
    if(s->hdc&&s->hwnd){ReleaseDC(s->hwnd,s->hdc);s->hdc=NULL;}
    if(s->hwnd){DestroyWindow(s->hwnd);s->hwnd=NULL;}
    timeEndPeriod(1); free(s); vm->backend_data=NULL; g_state=NULL; g_vm=NULL;
}

static void r3d_gfx_clear(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->hwnd||!s->fb)return;
    s->frame_dirty=true;
    u32 rgba_ptr=(u32)vm->r[1]; int r=0,g=0,b=0;
    if(rgba_ptr+4<=vm->mem_sz){r=vm->mem[rgba_ptr];g=vm->mem[rgba_ptr+1];b=vm->mem[rgba_ptr+2];}
    u32 col=fb_pack(r,g,b); u32 *px=(u32*)s->fb; int cnt=s->fb_w*s->fb_h;
    for(int i=0;i<cnt;i++) px[i]=col;
    if(s->sw_zbuf) memset(s->sw_zbuf,0,(size_t)s->sw_zbuf_w*(size_t)s->sw_zbuf_h*sizeof(u16));
}

static void r3d_gfx_plot(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->hwnd||!s->fb)return;
    s->frame_dirty=true;
    int x=vm->r[1],y=vm->r[2]; u32 rgba_ptr=(u32)vm->r[3];
    if(x<0||x>=s->fb_w||y<0||y>=s->fb_h)return; if(rgba_ptr+4>vm->mem_sz)return;
    fb_alpha_put(s,x,y,vm->mem[rgba_ptr],vm->mem[rgba_ptr+1],vm->mem[rgba_ptr+2],vm->mem[rgba_ptr+3]);
}

static void fb_line(R3DState *s, int x0, int y0, int x1, int y1, int r, int g, int b, int a) {
    int dx=abs(x1-x0),dy=abs(y1-y0),sx=(x0<x1)?1:-1,sy=(y0<y1)?1:-1,err=dx-dy;
    while(1){fb_alpha_put(s,x0,y0,r,g,b,a);if(x0==x1&&y0==y1)break;int e2=2*err;if(e2>-dy){err-=dy;x0+=sx;}if(e2<dx){err+=dx;y0+=sy;}}
}

/* Like fb_line but excludes the final pixel, matching OpenGL's GL_LINES
 * diamond-exit rasterization rule. This keeps @line endpoints aligned
 * across the relic3d and opengl backends: bevel corners drawn as @line
 * calls would otherwise extend one pixel past the joining edge in
 * relic3d, producing a visible cross at the bottom-right of buttons. */
static void fb_line_open(R3DState *s, int x0, int y0, int x1, int y1, int r, int g, int b, int a) {
    if(x0==x1&&y0==y1){fb_alpha_put(s,x0,y0,r,g,b,a);return;}
    int dx=abs(x1-x0),dy=abs(y1-y0),sx=(x0<x1)?1:-1,sy=(y0<y1)?1:-1,err=dx-dy;
    while(1){if(x0==x1&&y0==y1)break;fb_alpha_put(s,x0,y0,r,g,b,a);int e2=2*err;if(e2>-dy){err-=dy;x0+=sx;}if(e2<dx){err+=dx;y0+=sy;}}
}

static void fb_circlefill(R3DState *s, int cx, int cy, int rad, int r, int g, int b, int a) {
    if(rad<=0){fb_alpha_put(s,cx,cy,r,g,b,a);return;}
    int x=0,y=rad,d=1-rad;
    while(x<=y){
        for(int i=cx-x;i<=cx+x;i++){fb_alpha_put(s,i,cy+y,r,g,b,a);fb_alpha_put(s,i,cy-y,r,g,b,a);}
        for(int i=cx-y;i<=cx+y;i++){fb_alpha_put(s,i,cy+x,r,g,b,a);fb_alpha_put(s,i,cy-x,r,g,b,a);}
        x++; if(d<0) d+=2*x+1; else{y--;d+=2*(x-y)+1;}
    }
}

static void fb_polygon_fill(R3DState *s, int *pts, int npts, int r, int g, int b, int a) {
    if(npts<3)return;
    int miny=pts[1],maxy=pts[1];
    for(int i=1;i<npts;i++){if(pts[i*2+1]<miny)miny=pts[i*2+1];if(pts[i*2+1]>maxy)maxy=pts[i*2+1];}
    if(miny<0)miny=0; if(maxy>=s->fb_h)maxy=s->fb_h-1;
    for(int y=miny;y<=maxy;y++){
        int xmn=s->fb_w,xmx=-1;
        for(int i=0;i<npts;i++){int j=(i+1)%npts;int y0=pts[i*2+1],y1=pts[j*2+1],x0=pts[i*2],x1=pts[j*2];
            if((y0<=y&&y1>y)||(y1<=y&&y0>y)){int ix=x0+(y-y0)*(x1-x0)/(y1-y0);if(ix<xmn)xmn=ix;if(ix>xmx)xmx=ix;}}
        if(xmn>xmx)continue; if(xmn<0)xmn=0; if(xmx>=s->fb_w)xmx=s->fb_w-1;
        for(int x=xmn;x<=xmx;x++) fb_alpha_put(s,x,y,r,g,b,a);
    }
}

static void fb_thick_line(R3DState *s,int x1,int y1,int x2,int y2,int w,int r,int g,int b,int a) {
    if(w<=1){fb_line_open(s,x1,y1,x2,y2,r,g,b,a);return;}
    float dx=(float)(x2-x1),dy=(float)(y2-y1),len=sqrtf(dx*dx+dy*dy);
    if(len<1e-4f){int h=(w-1)/2;for(int py=y1-h;py<=y1+(w-1-h);py++)for(int px=x1-h;px<=x1+(w-1-h);px++)fb_alpha_put(s,px,py,r,g,b,a);return;}
    float hw=(float)(w-1)*0.5f,px=(-dy/len)*hw,py=(dx/len)*hw;
    int ox=(int)(px+(px>=0?0.5f:-0.5f)),oy=(int)(py+(py>=0?0.5f:-0.5f));
    int q[8]={x1+ox,y1+oy,x2+ox,y2+oy,x2-ox,y2-oy,x1-ox,y1-oy};
    fb_polygon_fill(s,q,4,r,g,b,a);
    fb_line(s,x1+ox,y1+oy,x2+ox,y2+oy,r,g,b,a);
    fb_line(s,x1-ox,y1-oy,x2-ox,y2-oy,r,g,b,a);
    int cr=(w-1)/2; if(cr>0){fb_circlefill(s,x1,y1,cr,r,g,b,a);fb_circlefill(s,x2,y2,cr,r,g,b,a);}
}

static void r3d_gfx_line(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->hwnd||!s->fb)return;
    s->frame_dirty=true;
    int x1=vm->r[1],y1=vm->r[2],x2=vm->r[3],y2=vm->r[4];
    u32 rp=(u32)vm->r[5]; int w=vm->r[6]; if(w<1)w=1; if(rp+4>vm->mem_sz)return;
    fb_thick_line(s,x1,y1,x2,y2,w,vm->mem[rp],vm->mem[rp+1],vm->mem[rp+2],vm->mem[rp+3]);
}

static void r3d_gfx_polygon(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->hwnd||!s->fb)return;
    s->frame_dirty=true;
    u32 pp=(u32)vm->r[1]; i32 cnt=vm->r[2]; u32 rp=(u32)vm->r[3];
    if(cnt<3||pp+cnt*4>vm->mem_sz||rp+4>vm->mem_sz)return;
    int r=vm->mem[rp],g=vm->mem[rp+1],b=vm->mem[rp+2],a=vm->mem[rp+3];
    int *pts=malloc(cnt*2*sizeof(int)); if(!pts)return;
    for(int i=0;i<cnt;i++){u32 o=pp+i*4;pts[i*2]=(i16)(vm->mem[o]|(vm->mem[o+1]<<8));pts[i*2+1]=(i16)(vm->mem[o+2]|(vm->mem[o+3]<<8));}
    fb_polygon_fill(s,pts,cnt,r,g,b,a); free(pts);
}

static void r3d_gfx_polyline(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->hwnd||!s->fb)return;
    s->frame_dirty=true;
    u32 pp=(u32)vm->r[1]; i32 cnt=vm->r[2]; u32 rp=(u32)vm->r[3]; int w=vm->r[4]; if(w<1)w=1;
    if(cnt<2||pp+cnt*4>vm->mem_sz||rp+4>vm->mem_sz)return;
    int r=vm->mem[rp],g=vm->mem[rp+1],b=vm->mem[rp+2],a=vm->mem[rp+3];
    for(int i=0;i<cnt-1;i++){u32 o1=pp+i*4,o2=pp+(i+1)*4;
        i16 x1=(i16)(vm->mem[o1]|(vm->mem[o1+1]<<8)),y1=(i16)(vm->mem[o1+2]|(vm->mem[o1+3]<<8));
        i16 x2=(i16)(vm->mem[o2]|(vm->mem[o2+1]<<8)),y2=(i16)(vm->mem[o2+2]|(vm->mem[o2+3]<<8));
        fb_thick_line(s,x1,y1,x2,y2,w,r,g,b,a);}
}

static void r3d_gfx_clip(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s) return;
    /* @clip(x,y,w,h): four i32 registers, matching opengl_apply_clip.
     * w<=0 or h<=0 disables clipping. */
    i32 x=vm->r[1],y=vm->r[2],w=vm->r[3],h=vm->r[4];
    if(w<=0||h<=0){s->clip_enabled=false;return;}
    s->clip_enabled=true; s->clip_x=x; s->clip_y=y; s->clip_w=w; s->clip_h=h;
}

static void r3d_gfx_sprite(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->hwnd||!s->fb)return;
    i32 x=vm->r[1],y=vm->r[2],slot=vm->r[3],src_x=vm->r[4],src_y=vm->r[5],src_w=vm->r[6],src_h=vm->r[7];
    i32 dst_w=vm->r[8],dst_h=vm->r[9];
    float angle=((R3DFBits){.i=vm->r[10]}).f; u32 rp=(u32)vm->r[11];
    if(slot<1||slot>=s->tex_cap)return;
    SwTexture *tex=&s->textures[slot]; if(!tex->pixels)return;
    int tw=tex->src_w,th=tex->src_h; if(tw<=0||th<=0)return;
    if(src_w<=0)src_w=tw; if(src_h<=0)src_h=th;
    if(dst_w<=0)dst_w=src_w; if(dst_h<=0)dst_h=src_h;
    int tr=255,tg=255,tb=255,ta=255;
    if(rp+4<=vm->mem_sz){tr=vm->mem[rp];tg=vm->mem[rp+1];tb=vm->mem[rp+2];ta=vm->mem[rp+3];}
    int tint=(tr<255||tg<255||tb<255||ta<255);
    if(angle==0.0f){
        const u8*tp=tex->pixels; int tpi=tex->w,ak=tex->has_alpha;
        for(int py=0;py<dst_h;py++){int sy=src_y+(py*src_h)/(dst_h>0?dst_h:1);if(sy<0||sy>=th)continue;int ro=sy*tpi;
            for(int px=0;px<dst_w;px++){int sx=src_x+(px*src_w)/(dst_w>0?dst_w:1);if(sx<0||sx>=tw)continue;
                const u8*p=tp+((ro+sx)<<2);if(ak&&p[3]<128)continue;
                int rr=p[0],gg=p[1],bb=p[2];if(tint){rr=(rr*tr)>>8;gg=(gg*tg)>>8;bb=(bb*tb)>>8;}
                fb_put(s,x+px,y+py,fb_pack(rr,gg,bb));}}
    } else {
        float cosA=cosf(angle),sinA=sinf(angle),hx=dst_w*0.5f,hy=dst_h*0.5f;
        float rx[4]={-hx,hx,hx,-hx},ry[4]={-hy,-hy,hy,hy};
        float mnx=1e9f,mxx=-1e9f,mny=1e9f,mxy=-1e9f;
        for(int i=0;i<4;i++){float rrx=rx[i]*cosA-ry[i]*sinA,rry=rx[i]*sinA+ry[i]*cosA;
            if(rrx<mnx)mnx=rrx;if(rrx>mxx)mxx=rrx;if(rry<mny)mny=rry;if(rry>mxy)mxy=rry;}
        int bx0=(int)(x+hx+mnx),by0=(int)(y+hy+mny),bx1=(int)(x+hx+mxx+1),by1=(int)(y+hy+mxy+1);
        if(bx0<0)bx0=0;if(by0<0)by0=0;if(bx1>s->fb_w)bx1=s->fb_w;if(by1>s->fb_h)by1=s->fb_h;
        const u8*tp=tex->pixels;int tpi=tex->w,ak=tex->has_alpha;
        for(int py=by0;py<by1;py++) for(int px=bx0;px<bx1;px++){
            float dx=(float)(px-x)-hx,dy=(float)(py-y)-hy;
            float rdx=dx*cosA+dy*sinA,rdy=-dx*sinA+dy*cosA;
            int sx=src_x+(int)((hx+rdx)*(float)src_w/(float)dst_w),sy=src_y+(int)((hy+rdy)*(float)src_h/(float)dst_h);
            if(sx<src_x||sy<src_y||sx>=src_x+src_w||sy>=src_y+src_h)continue;
            const u8*p=tp+(((sy*tpi)+sx)<<2);if(ak&&p[3]<128)continue;
            int rr=p[0],gg=p[1],bb=p[2];if(tint){rr=(rr*tr)>>8;gg=(gg*tg)>>8;bb=(bb*tb)>>8;}
            fb_put(s,px,py,fb_pack(rr,gg,bb));}
    }
    s->frame_dirty=true;
}

/* Pace presentation to the display's refresh rate (see vsync_throttle in
 * r3d_gfx_window). GetDeviceCaps(VREFRESH) reports 0 or 1 for "device
 * default", in which case 60Hz is assumed. timeBeginPeriod(1) is already in
 * effect, so Sleep() has millisecond granularity. */
static void r3d_wait_for_frame(R3DState *s) {
    if(!s->vsync_throttle)return;
    int hz=s->hdc?GetDeviceCaps(s->hdc,VREFRESH):0;
    if(hz<=1)hz=60;
    DWORD interval=(DWORD)(1000/hz); if(interval<1)interval=1;
    DWORD now=timeGetTime();
    if(s->last_flip_ms){
        DWORD elapsed=now-s->last_flip_ms;
        if(elapsed<interval){Sleep(interval-elapsed);now=timeGetTime();}
    }
    s->last_flip_ms=now;
}

/* Stamps the software pointer into the framebuffer at (cx,cy), remembering the
 * pixels it covers. Writes the framebuffer directly instead of going through
 * fb_put: the pointer is not part of the script's drawing, so the script's clip
 * rectangle must not apply to it. */
static void r3d_cursor_stamp(R3DState *s, int cx, int cy) {
    s->cursor_saved=false;
    if(!s->fb)return;
    u32 *px=(u32*)s->fb; int stride=s->fb_stride>>2;
    for(int row=0;row<RELIC_SOFTCUR_H;row++){
        int y=cy+row; if(y<0||y>=s->fb_h)continue;
        const char *art=relic_softcur_arrow[row];
        for(int col=0;col<RELIC_SOFTCUR_W;col++){
            char c=art[col]; if(c==' ')continue;
            int x=cx+col; if(x<0||x>=s->fb_w)continue;
            s->cursor_save[row*RELIC_SOFTCUR_W+col]=px[y*stride+x];
            px[y*stride+x]=(c=='X')?fb_pack(0,0,0):fb_pack(255,255,255);
        }
    }
    s->cursor_x=cx; s->cursor_y=cy; s->cursor_saved=true;
}

/* Puts back the pixels the pointer covered, so the script's framebuffer is
 * handed back exactly as it drew it: scripts that redraw only part of the frame
 * would otherwise accumulate a trail of pointers. */
static void r3d_cursor_unstamp(R3DState *s) {
    if(!s->cursor_saved||!s->fb){s->cursor_saved=false;return;}
    u32 *px=(u32*)s->fb; int stride=s->fb_stride>>2;
    for(int row=0;row<RELIC_SOFTCUR_H;row++){
        int y=s->cursor_y+row; if(y<0||y>=s->fb_h)continue;
        const char *art=relic_softcur_arrow[row];
        for(int col=0;col<RELIC_SOFTCUR_W;col++){
            if(art[col]==' ')continue;
            int x=s->cursor_x+col; if(x<0||x>=s->fb_w)continue;
            px[y*stride+x]=s->cursor_save[row*RELIC_SOFTCUR_W+col];
        }
    }
    s->cursor_saved=false;
}

static void r3d_gfx_flip(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->hwnd)return;
    r3d_pump_events(s);
    /* The software pointer only reaches the screen as part of a present, so a
     * pointer that moved (or entered/left the client area) has to force one
     * even when the script drew nothing new this frame - otherwise it freezes
     * until the script's next visible change. */
    bool cursor_on=false;
    if(s->soft_cursor){
        cursor_on=relic_win_softcursor_visible(s->hwnd)?true:false;
        if(cursor_on!=s->cursor_was_on||
           (cursor_on&&(s->mouse_x!=s->cursor_x||s->mouse_y!=s->cursor_y)))
            s->frame_dirty=true;
        s->cursor_was_on=cursor_on;
    }
    r3d_wait_for_frame(s);
    if(s->fb&&s->hdc&&s->frame_dirty){
        /* Flush any pending GDI ops (e.g. @text) into the DIB bits, then present
         * straight from the bit array. Reading the raw bits (rather than BitBlt
         * from the DIBSection-backed mem_dc) guarantees CPU-written pixels show
         * on Win9x, where GDI may otherwise present a stale cached surface. */
        GdiFlush();
        if(cursor_on) r3d_cursor_stamp(s,s->mouse_x,s->mouse_y);
        BITMAPINFO bmi; memset(&bmi,0,sizeof(bmi));
        bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth=s->fb_w; bmi.bmiHeader.biHeight=-s->fb_h;
        bmi.bmiHeader.biPlanes=1; bmi.bmiHeader.biBitCount=32;
        bmi.bmiHeader.biCompression=BI_RGB;
        if(s->stretch_enabled){
            int old=SetStretchBltMode(s->hdc,COLORONCOLOR);
            StretchDIBits(s->hdc,s->vp_x,s->vp_y,s->vp_w,s->vp_h,
                          0,0,s->fb_w,s->fb_h,s->fb,&bmi,DIB_RGB_COLORS,SRCCOPY);
            SetStretchBltMode(s->hdc,old);
        } else {
            SetDIBitsToDevice(s->hdc,0,0,s->fb_w,s->fb_h,0,0,0,s->fb_h,
                              s->fb,&bmi,DIB_RGB_COLORS);
        }
        r3d_cursor_unstamp(s);
        s->frame_dirty=false;
    }
}

static int read_vm_cstr(RelicVM *vm, u32 ptr, char *out, int out_sz) {
    int n=0; if(out_sz<=0)return 0;
    while(ptr<vm->mem_sz&&n<out_sz-1){u8 c=vm->mem[ptr++];if(c==0)break;out[n++]=(char)c;}
    out[n]='\0'; return n;
}

/**
 * @brief Measures a string's raw GDI cell size and its tight ink-box extent.
 *
 * `ts_out` is GDI's cell size (GetTextExtentPoint32W): includes internal
 * leading above the cap-height, so it is typically 1-2px taller than the
 * glyphs actually drawn. `ink_top_out`/`ink_height_out` describe the visible
 * glyph rows within that cell, found by rendering into a scratch DIB and
 * scanning for lit pixels - the same technique win95-opengl uses so both
 * backends report/position identically.
 *
 * `ink_left_out`/`ink_width_out` describe the visible glyph columns within
 * the cell. GDI's advance width includes side bearings that are often
 * asymmetric (especially for icon fonts whose glyph sits on the left of
 * the cell), so centring on `ts.cx` puts the visible ink off-centre. The
 * OpenGL backend trims its text texture to the tight ink box and centres
 * on that; these outputs let the relic3d backend do the same.
 *
 * `ref_dc` only supplies the device to create the scratch DC/DIB from; `hf`
 * must already be selectable (not yet selected into any DC).
 */
static void r3d_measure_text_ink(HDC ref_dc, HFONT hf, const wchar_t *wbuf, int wlen,
                                 SIZE *ts_out, int *ink_top_out, int *ink_height_out,
                                 int *ink_left_out, int *ink_width_out) {
    SIZE ts = {0, 0};
    int ink_top = 0, ink_height = 0;
    int ink_left = 0, ink_width = 0;

    HDC scan_dc = CreateCompatibleDC(ref_dc);
    if (scan_dc) {
        HFONT old_font = (HFONT)SelectObject(scan_dc, hf);
        if (GetTextExtentPoint32W(scan_dc, wbuf, wlen, &ts) && ts.cx > 0 && ts.cy > 0) {
            ink_height = ts.cy; /* fallback if the scan below finds nothing */
            ink_width  = ts.cx;
            BITMAPINFO bmi; memset(&bmi, 0, sizeof(bmi));
            bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth       = ts.cx;
            bmi.bmiHeader.biHeight      = -(LONG)ts.cy;
            bmi.bmiHeader.biPlanes      = 1;
            bmi.bmiHeader.biBitCount    = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            void *bits = NULL;
            HBITMAP dib = CreateDIBSection(scan_dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (dib && bits) {
                HBITMAP old_bmp = (HBITMAP)SelectObject(scan_dc, dib);
                memset(bits, 0, (size_t)ts.cx * (size_t)ts.cy * 4);
                SetTextColor(scan_dc, RGB(255, 255, 255));
                SetBkMode(scan_dc, TRANSPARENT);
                TextOutW(scan_dc, 0, 0, wbuf, wlen);
                GdiFlush();
                unsigned char *src = (unsigned char*)bits;
                /* Vertical ink extents (top/bottom of visible glyph rows). */
                int top = 0, bottom = ts.cy - 1, found = 0;
                for (top = 0; top < ts.cy; top++) {
                    for (int gx = 0; gx < ts.cx; gx++) {
                        if ((src[(top * ts.cx + gx) * 4] | src[(top * ts.cx + gx) * 4 + 1] | src[(top * ts.cx + gx) * 4 + 2]) > 0) { found = 1; break; }
                    }
                    if (found) break;
                }
                if (found) {
                    found = 0;
                    for (bottom = ts.cy - 1; bottom >= 0; bottom--) {
                        for (int gx = 0; gx < ts.cx; gx++) {
                            if ((src[(bottom * ts.cx + gx) * 4] | src[(bottom * ts.cx + gx) * 4 + 1] | src[(bottom * ts.cx + gx) * 4 + 2]) > 0) { found = 1; break; }
                        }
                        if (found) break;
                    }
                    ink_top    = top;
                    ink_height = bottom - top + 1;
                }
                /* Horizontal ink extents (left/right of visible glyph cols).
                 * Icon fonts often have a large left side bearing; without
                 * this scan, centring on ts.cx puts the glyph off-centre. */
                int left = 0, right = ts.cx - 1, hfound = 0;
                for (left = 0; left < ts.cx; left++) {
                    for (int gy = 0; gy < ts.cy; gy++) {
                        if ((src[(gy * ts.cx + left) * 4] | src[(gy * ts.cx + left) * 4 + 1] | src[(gy * ts.cx + left) * 4 + 2]) > 0) { hfound = 1; break; }
                    }
                    if (hfound) break;
                }
                if (hfound) {
                    hfound = 0;
                    for (right = ts.cx - 1; right >= 0; right--) {
                        for (int gy = 0; gy < ts.cy; gy++) {
                            if ((src[(gy * ts.cx + right) * 4] | src[(gy * ts.cx + right) * 4 + 1] | src[(gy * ts.cx + right) * 4 + 2]) > 0) { hfound = 1; break; }
                        }
                        if (hfound) break;
                    }
                    ink_left  = left;
                    ink_width = right - left + 1;
                }
                SelectObject(scan_dc, old_bmp);
                DeleteObject(dib);
            }
        }
        SelectObject(scan_dc, old_font);
        DeleteDC(scan_dc);
    }

    *ts_out         = ts;
    *ink_top_out    = ink_top;
    *ink_height_out = ink_height;
    *ink_left_out   = ink_left;
    *ink_width_out  = ink_width;
}

/* Draw a string with explicit args. Shared by @text and draw-list
 * DL_TEXT records; x/y are logical top-left coords. */
static void r3d_text_draw(RelicVM *vm, u32 tp, i32 x, i32 y, i32 sz,
                          i32 wt, u32 fp, i32 just, int cr, int cg,
                          int cb) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->hwnd||!s->mem_dc)return;
    s->frame_dirty=true;
    if(s->nfaces<=0||tp>=vm->mem_sz)return;
    char tb[1024]; read_vm_cstr(vm,tp,tb,sizeof(tb));
    char fb[64]=""; if(fp&&fp<vm->mem_sz) read_vm_cstr(vm,fp,fb,sizeof(fb));
    int fi=select_face(s,fb,wt); if(fi<0||!s->faces[fi].init)return;
    int ps=(sz>0)?sz:16,fw=(wt>=600)?FW_BOLD:FW_NORMAL;
    /* System fonts (e.g. MS Sans Serif, Tahoma) are raster/TrueType fonts
     * that GDI provides natively. OUT_DEFAULT_PRECIS lets GDI use the raster
     * face directly; OUT_TT_ONLY_PRECIS would force it to find a TrueType
     * substitute, which defeats the purpose. For bundled TrueType fonts,
     * OUT_TT_ONLY_PRECIS keeps the mapper away from raster faces. */
    DWORD out_precis = s->faces[fi].is_system ? OUT_DEFAULT_PRECIS : OUT_TT_ONLY_PRECIS;
    /* ClearType composites subpixel coverage straight into the framebuffer,
     * matching native Windows text. Win9x predates ClearType, and a
     * stretch-mode present would scale the colour fringes into smears, so
     * both keep plain antialiasing. */
    DWORD quality = ANTIALIASED_QUALITY;
#ifndef RELIC_WIN95
    if (!s->stretch_enabled) quality = CLEARTYPE_QUALITY;
#endif
    const char *fam = s->faces[fi].family[0] ? s->faces[fi].family : font_family_name(s->faces[fi].name);
    HFONT hf=CreateFontA(-ps,0,0,0,fw,FALSE,FALSE,FALSE,DEFAULT_CHARSET,out_precis,
        CLIP_DEFAULT_PRECIS,quality,DEFAULT_PITCH|FF_DONTCARE,fam);
    if(!hf)return;
    wchar_t wbuf[1024];
#ifdef RELIC_WIN95
    int wlen=utf8_to_wide(tb,wbuf,1024);
#else
    int wlen=MultiByteToWideChar(CP_UTF8,0,tb,-1,wbuf,1024)-1;
    if(wlen<0)wlen=0;
#endif
    /* Position using the same tight ink box @text_measure reports (see
     * r3d_measure_text_ink), not GDI's raw cell height: drawing centred on
     * ts.cy while @text_measure reports a shorter ink height put every
     * centred/anchored label 1-3px lower than a caller laying out with
     * @text_measure would expect, and stacked the same amount of extra gap
     * onto every line of wrapped text. (Vertical centring still uses the
     * ink box; TOP/BOTTOM anchoring uses the font cell - see dy below.)
     *
     * Horizontal positioning also uses the tight ink box: GDI's advance
     * width includes side bearings that are often asymmetric (icon fonts
     * in particular have a large left bearing), so centring on ts.cx puts
     * the visible glyph off-centre. The OpenGL backend trims its text
     * texture to the ink box and centres on that; we do the same here. */
    SIZE ts; int ink_top = 0, ink_h = 0, ink_left = 0, ink_w = 0;
    r3d_measure_text_ink(s->hdc, hf, wbuf, wlen, &ts, &ink_top, &ink_h, &ink_left, &ink_w);
    if (ink_h <= 0) { ink_top = 0; ink_h = ts.cy; }
    if (ink_w <= 0) { ink_left = 0; ink_w = ts.cx; }
    int j=(just>=0&&just<=8)?just:0,hm=(j<=2)?0:(j<=5?1:2),vm2;
    switch(j){case 0:case 3:case 6:vm2=0;break;case 1:case 4:case 7:vm2=1;break;default:vm2=2;}
    /* dx is the GDI cell origin; the visible ink starts at dx+ink_left and
     * spans ink_w. CENTRE and RIGHT anchor the ink box on x (the glyph's
     * visible edge lands on x). LEFT puts the pen at x instead, so the
     * left side bearing is preserved and a run drawn at an x accumulated
     * from @text_measure advance widths lands where the next run expects
     * it - matching the OpenGL backend's pen-at-x convention. */
    int dx;
    if(hm==0) dx = x - ink_left - ink_w / 2;
    else if(hm==1) dx = x;
    else dx = x - ink_left - ink_w;
    /* vm2: 0=vertical-centre, 1=bottom-anchor (text sits above y),
     * 2=top-anchor (text sits below y). Centring uses the ink box - a lone
     * icon glyph wants its visible shape centred, and that is what
     * @text_measure reports. TOP/BOTTOM anchor the font cell instead
     * (ts.cy = tmHeight = ascent+descent, content-independent): anchoring
     * by ink height gives every run its own baseline - "." lands at
     * ascender height and "_" floats up - which breaks any line drawn as
     * several runs (syntax highlighting). */
    int dy;
    if(vm2==0) dy = y - ink_top - ink_h/2;
    else if(vm2==1) dy = y - ts.cy;
    else dy = y;
    HFONT of=(HFONT)SelectObject(s->mem_dc,hf);
    SetTextColor(s->mem_dc,RGB(cr,cg,cb)); SetBkMode(s->mem_dc,TRANSPARENT);
    TextOutW(s->mem_dc,dx,dy,wbuf,wlen);
    SelectObject(s->mem_dc,of); DeleteObject(hf);
    /* Commit GDI text into the DIB bits now. On Win9x, GDI keeps a cached view
     * of a DIBSection after drawing into it; without this flush a later direct
     * CPU write or a raw-bit present can race the unflushed GDI batch, leaving
     * a stale/black frame. */
    GdiFlush();
}

static void r3d_gfx_text(RelicVM *vm) {
    u32 cp=(u32)vm->r[8];
    int cr=255,cg=255,cb=255;
    if(cp&&cp+4<=vm->mem_sz){cr=vm->mem[cp];cg=vm->mem[cp+1];cb=vm->mem[cp+2];}
    r3d_text_draw(vm,(u32)vm->r[1],vm->r[2],vm->r[3],vm->r[4],vm->r[5],
                  (u32)vm->r[6],vm->r[7],cr,cg,cb);
}

/* @draw_list(buf, len) - submit a retained primitive buffer. The shared
 * tessellator (common/drawlist/drawlist.h) turns it into triangles plus
 * ordered text/clip ops; triangles go through the same software
 * scanline filler as @polygon. */
static void r3d_gfx_draw_list(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data;
    u32 ptr=(u32)vm->r[1]; i32 len=vm->r[2];
    DlBatch b; int i;
    if(!s||!s->hwnd||!s->fb||len<=0)return;
    if((uint64_t)ptr+(uint64_t)len>(uint64_t)vm->mem_sz)return;

    dl_batch_init(&b);
    /* no y-flip: the software framebuffer is already top-left y-down */
    if(!dl_tessellate(vm->mem+ptr,len,0.0f,&b)){dl_batch_free(&b);return;}
    if(b.no==0){dl_batch_free(&b);return;}
    s->frame_dirty=true;

    for(i=0;i<b.no;i++){
        DlOp *o=&b.o[i];
        if(o->kind==DLO_TRIS){
            if(o->count<=0||!b.v)continue;
            for(int k=0;k+2<o->count;k+=3){
                const DlVert *v=&b.v[o->start+k];
                int pts[6];
                pts[0]=(int)v[0].x; pts[1]=(int)v[0].y;
                pts[2]=(int)v[1].x; pts[3]=(int)v[1].y;
                pts[4]=(int)v[2].x; pts[5]=(int)v[2].y;
                fb_polygon_fill(s,pts,3,v->r,v->g,v->b,v->a);
            }
        } else if(o->kind==DLO_CLIP){
            if(o->cw<=0||o->ch<=0){s->clip_enabled=false;}
            else {s->clip_enabled=true;s->clip_x=o->cx;s->clip_y=o->cy;
                  s->clip_w=o->cw;s->clip_h=o->ch;}
        } else if(o->kind==DLO_TEXT){
            r3d_text_draw(vm,o->text_ptr,o->tx,o->ty,o->tsize,o->tweight,
                          o->font_ptr,o->tjust,o->tr,o->tg,o->tb);
        }
    }
    dl_batch_free(&b);
}

static void r3d_gfx_text_measure(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->mem_dc)return;
    u32 tp=(u32)vm->r[1]; i32 sz=vm->r[2]; i32 wt=vm->r[3]; u32 fp=(u32)vm->r[4]; u32 out=(u32)vm->r[5];
    if(tp>=vm->mem_sz||out+20>vm->mem_sz)return;
    char tb[1024]; read_vm_cstr(vm,tp,tb,sizeof(tb));
    char fb[64]=""; if(fp&&fp<vm->mem_sz) read_vm_cstr(vm,fp,fb,sizeof(fb));
    int fi=select_face(s,fb,wt); if(fi<0||!s->faces[fi].init){vm->r[1]=0;vm->r[2]=0;return;}
    int ps=(sz>0)?sz:16,fww=(wt>=600)?FW_BOLD:FW_NORMAL;
    DWORD out_precis2 = s->faces[fi].is_system ? OUT_DEFAULT_PRECIS : OUT_TT_ONLY_PRECIS;
    const char *fam2 = s->faces[fi].family[0] ? s->faces[fi].family : font_family_name(s->faces[fi].name);
    /* Same quality choice as r3d_gfx_text so the reported extents match the
     * drawn ink. */
    DWORD quality2 = ANTIALIASED_QUALITY;
#ifndef RELIC_WIN95
    if (!s->stretch_enabled) quality2 = CLEARTYPE_QUALITY;
#endif
    HFONT hf=CreateFontA(-ps,0,0,0,fww,FALSE,FALSE,FALSE,DEFAULT_CHARSET,out_precis2,
        CLIP_DEFAULT_PRECIS,quality2,DEFAULT_PITCH|FF_DONTCARE,fam2);
    if(!hf){vm->r[1]=0;vm->r[2]=0;return;}
    wchar_t wbuf2[1024];
#ifdef RELIC_WIN95
    int wlen2=utf8_to_wide(tb,wbuf2,1024);
#else
    int wlen2=MultiByteToWideChar(CP_UTF8,0,tb,-1,wbuf2,1024)-1;
    if(wlen2<0)wlen2=0;
#endif
    /* Write the full TextMetrics struct (5 x i32 = 20 bytes) to match the
     * OpenGL backend: width, height, ascent, descent, line_h. Previously
     * only width+height were written, leaving line_h as 0 — the script's
     * fallback (21px) made wrapped text overflow on Win95 where the actual
     * line height for an 11px font is ~16px. */
    HFONT of=(HFONT)SelectObject(s->mem_dc,hf);
    TEXTMETRICA tm;
    int ascent = 0, descent = 0, line_h = 0;
    if (GetTextMetricsA(s->mem_dc, &tm)) {
        ascent  = tm.tmAscent;
        descent = tm.tmDescent;
        line_h  = tm.tmHeight + tm.tmExternalLeading;
    }
    SelectObject(s->mem_dc,of);
    /* `height` uses a tight ink bounding box (actual visible glyph rows, via
     * r3d_measure_text_ink), matching the OpenGL backend's
     * opengl_gfx_text_measure and, since the fix above, matching what
     * r3d_gfx_text itself now draws and positions with. */
    SIZE ts; int ink_top = 0, ink_h = 0, ink_left = 0, ink_w = 0;
    r3d_measure_text_ink(s->hdc, hf, wbuf2, wlen2, &ts, &ink_top, &ink_h, &ink_left, &ink_w);
    LONG height = (ink_h > 0) ? ink_h : ts.cy;
    DeleteObject(hf);
    i32 width32 = ts.cx, height32 = (i32)height;
    memcpy(vm->mem+out+0,&width32,4);
    memcpy(vm->mem+out+4,&height32,4);
    memcpy(vm->mem+out+8,&ascent,4);
    memcpy(vm->mem+out+12,&descent,4);
    memcpy(vm->mem+out+16,&line_h,4);
}

/* Ellipse / rounded-rectangle helpers ---------------------------------------------------------- */

static void r3d_ellipse_arc_points(float *pts, int *n, int cx, int cy, int rx, int ry,
                                   int start_deg, int finish_deg) {
    float span = (float)(finish_deg - start_deg);
    int N = (int)(fabsf(span) / 360.0f * 64.0f);
    if (N < 4) N = 4; if (N > 64) N = 64;
    float step = span / (float)N;
    const float PI = 3.14159265358979f;
    *n = 0;
    for (int i = 0; i <= N; i++) {
        float ang = (start_deg + step * (float)i) * PI / 180.0f;
        pts[(*n) * 2]     = (float)cx + cosf(ang) * (float)rx;
        pts[(*n) * 2 + 1] = (float)cy + sinf(ang) * (float)ry;
        (*n)++;
    }
}

static void r3d_gfx_ellipse(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->hwnd||!s->fb)return;
    s->frame_dirty=true;
    int cx=vm->r[1],cy=vm->r[2],w=vm->r[3],h=vm->r[4],sw=vm->r[5];
    u32 sc_ptr=(u32)vm->r[6]; int start=vm->r[7],finish=vm->r[8];
    if(sw<=0||sc_ptr+4>vm->mem_sz)return;
    int r=vm->mem[sc_ptr],g=vm->mem[sc_ptr+1],b=vm->mem[sc_ptr+2],a=vm->mem[sc_ptr+3];
    int rx=w/2,ry=h/2;
    float pts[(64+1)*2]; int n=0;
    r3d_ellipse_arc_points(pts,&n,cx,cy,rx,ry,start,finish);
    if(n<2)return;
    for(int i=0;i<n-1;i++){
        int x1=(int)pts[i*2],y1=(int)pts[i*2+1];
        int x2=(int)pts[(i+1)*2],y2=(int)pts[(i+1)*2+1];
        fb_thick_line(s,x1,y1,x2,y2,sw,r,g,b,a);
    }
}

static void r3d_gfx_ellipse_filled(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->hwnd||!s->fb)return;
    s->frame_dirty=true;
    int cx=vm->r[1],cy=vm->r[2],w=vm->r[3],h=vm->r[4],sw=vm->r[5];
    u32 sc_ptr=(u32)vm->r[6]; int start=vm->r[7],finish=vm->r[8]; u32 fc_ptr=(u32)vm->r[9];
    int rx=w/2,ry=h/2;
    float pts[(64+1)*2]; int n=0;
    r3d_ellipse_arc_points(pts,&n,cx,cy,rx,ry,start,finish);
    if(n<2)return;
    if(fc_ptr+4<=vm->mem_sz){
        int fr=vm->mem[fc_ptr],fg=vm->mem[fc_ptr+1],fb2=vm->mem[fc_ptr+2],fa=vm->mem[fc_ptr+3];
        int *poly=malloc((n+1)*2*sizeof(int)); if(!poly)return;
        poly[0]=cx; poly[1]=cy;
        for(int i=0;i<n;i++){poly[(i+1)*2]=(int)pts[i*2];poly[(i+1)*2+1]=(int)pts[i*2+1];}
        fb_polygon_fill(s,poly,n+1,fr,fg,fb2,fa); free(poly);
    }
    if(sw>0&&sc_ptr+4<=vm->mem_sz){
        int r=vm->mem[sc_ptr],g=vm->mem[sc_ptr+1],b=vm->mem[sc_ptr+2],a=vm->mem[sc_ptr+3];
        for(int i=0;i<n-1;i++){
            int x1=(int)pts[i*2],y1=(int)pts[i*2+1];
            int x2=(int)pts[(i+1)*2],y2=(int)pts[(i+1)*2+1];
            fb_thick_line(s,x1,y1,x2,y2,sw,r,g,b,a);
        }
    }
}

static int r3d_build_rrect(float *pts, int x1, int y1, int x2, int y2, int radius) {
    float r = (float)radius;
    float half_w = (x2 - x1) * 0.5f, half_h = (y2 - y1) * 0.5f;
    if (r > half_w) r = half_w; if (r > half_h) r = half_h; if (r < 0) r = 0;
    int segs = 32; if (r < 4) segs = 16; if (r > 32) segs = 48;
    float cx[4] = {x1+r,x2-r,x2-r,x1+r};
    float cy[4] = {y1+r,y1+r,y2-r,y2-r};
    float sd[4] = {180.0f,270.0f,0.0f,90.0f};
    const float PI = 3.14159265358979f;
    int n = 0;
    for(int c=0;c<4;c++) for(int i=0;i<=segs;i++){
        float ang=(sd[c]+90.0f*(float)i/(float)segs)*PI/180.0f;
        pts[n*2]=cx[c]+cosf(ang)*r; pts[n*2+1]=cy[c]+sinf(ang)*r; n++;
    }
    return n;
}

static void r3d_gfx_rounded_rect(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->hwnd||!s->fb)return;
    s->frame_dirty=true;
    int x1=vm->r[1],y1=vm->r[2],x2=vm->r[3],y2=vm->r[4],sw=vm->r[5];
    u32 sc_ptr=(u32)vm->r[6]; int radius=vm->r[7];
    if(sw<=0||sc_ptr+4>vm->mem_sz)return;
    int r=vm->mem[sc_ptr],g=vm->mem[sc_ptr+1],b=vm->mem[sc_ptr+2],a=vm->mem[sc_ptr+3];
    float pts[(48*4+8)*2]; int n=r3d_build_rrect(pts,x1,y1,x2,y2,radius);
    if(n<2)return;
    for(int i=0;i<n;i++){
        int j=(i+1)%n;
        int x1p=(int)pts[i*2],y1p=(int)pts[i*2+1];
        int x2p=(int)pts[j*2],y2p=(int)pts[j*2+1];
        fb_thick_line(s,x1p,y1p,x2p,y2p,sw,r,g,b,a);
    }
}

static void r3d_gfx_rounded_rect_filled(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->hwnd||!s->fb)return;
    s->frame_dirty=true;
    int x1=vm->r[1],y1=vm->r[2],x2=vm->r[3],y2=vm->r[4],sw=vm->r[5];
    u32 sc_ptr=(u32)vm->r[6]; int radius=vm->r[7]; u32 fc_ptr=(u32)vm->r[8];
    float pts[(48*4+8)*2]; int n=r3d_build_rrect(pts,x1,y1,x2,y2,radius);
    if(n<3)return;
    if(fc_ptr+4<=vm->mem_sz){
        int fr=vm->mem[fc_ptr],fg=vm->mem[fc_ptr+1],fb2=vm->mem[fc_ptr+2],fa=vm->mem[fc_ptr+3];
        int *poly=malloc(n*2*sizeof(int)); if(!poly)return;
        for(int i=0;i<n;i++){poly[i*2]=(int)pts[i*2];poly[i*2+1]=(int)pts[i*2+1];}
        fb_polygon_fill(s,poly,n,fr,fg,fb2,fa); free(poly);
    }
    if(sw>0&&sc_ptr+4<=vm->mem_sz){
        int r=vm->mem[sc_ptr],g=vm->mem[sc_ptr+1],b=vm->mem[sc_ptr+2],a=vm->mem[sc_ptr+3];
        for(int i=0;i<n;i++){
            int j=(i+1)%n;
            int x1p=(int)pts[i*2],y1p=(int)pts[i*2+1];
            int x2p=(int)pts[j*2],y2p=(int)pts[j*2+1];
            fb_thick_line(s,x1p,y1p,x2p,y2p,sw,r,g,b,a);
        }
    }
}

static void r3d_gfx_viewport(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->hwnd)return;
    i32 x=vm->r[1],y=vm->r[2],w=vm->r[3],h=vm->r[4];
    if(w<=0||h<=0){s->vp_3d_x=s->vp_3d_y=s->vp_3d_w=s->vp_3d_h=0;return;}
    if(x<0)x=0;if(y<0)y=0;if(x+w>s->width)w=s->width-x;if(y+h>s->height)h=s->height-y;
    s->vp_3d_x=x;s->vp_3d_y=y;s->vp_3d_w=w;s->vp_3d_h=h;s->frame_dirty=true;
}

/* Grow the texture slot table to at least `want` slots (or fail: the
 * request exceeded RELIC_MAX_TEXTURES / ran out of memory). New slots
 * are zeroed so the scan below sees them as free. Lazily allocates the
 * table on first use. */
static bool r3d_tex_reserve(R3DState *s, int want) {
    if(!s->textures){
        s->textures=(SwTexture*)calloc(RELIC_TEX_CAP_INIT,sizeof(SwTexture));
        if(!s->textures) return false;
        s->tex_cap=RELIC_TEX_CAP_INIT;
    }
    if(want>s->tex_cap){
        int nc=s->tex_cap;
        while(nc<want) nc<<=1;
        if(nc>RELIC_MAX_TEXTURES) nc=RELIC_MAX_TEXTURES;
        if(want>nc) return false;
        SwTexture *nt=(SwTexture*)realloc(s->textures,(size_t)nc*sizeof(SwTexture));
        if(!nt) return false;
        memset(nt+s->tex_cap,0,(size_t)(nc-s->tex_cap)*sizeof(SwTexture));
        s->textures=nt; s->tex_cap=nc;
    }
    return true;
}

static void r3d_gfx_texture(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->hwnd){vm->r[1]=0;return;}
    u32 pp=(u32)vm->r[1]; i32 w=vm->r[2],h=vm->r[3];
    if(w<=0||h<=0||w>4096||h>4096){vm->r[1]=0;return;}
    uint64_t need=(uint64_t)w*(uint64_t)h*4ull;
    if((uint64_t)pp+need>(uint64_t)vm->mem_sz){vm->r[1]=0;return;}
    if(!r3d_tex_reserve(s,RELIC_TEX_CAP_INIT)){vm->r[1]=0;return;}
    int slot=0; for(int i=1;i<s->tex_cap;i++) if(!s->textures[i].pixels){slot=i;break;}
    if(!slot&&r3d_tex_reserve(s,s->tex_cap+1)){
        for(int i=1;i<s->tex_cap;i++) if(!s->textures[i].pixels){slot=i;break;}
    }
    if(!slot){vm->r[1]=0;return;}
    /* Round up to POT for bitmask wrap. */
    int pw=1; while(pw<w) pw<<=1;
    int ph=1; while(ph<h) ph<<=1;
    u8*buf=(u8*)malloc((size_t)pw*(size_t)ph*4); if(!buf){vm->r[1]=0;return;}
    const u8*src=vm->mem+pp; int ha=0;
    for(int y=0;y<ph;y++){int sy=y%h;for(int x=0;x<pw;x++){int sx=x%w;
        int os=(sy*w+sx)*4,od=(y*pw+x)*4;
        buf[od]=src[os];buf[od+1]=src[os+1];buf[od+2]=src[os+2];buf[od+3]=src[os+3];
        if(src[os+3]<255) ha=1;}}
    s->textures[slot].pixels=buf;s->textures[slot].w=pw;s->textures[slot].h=ph;
    s->textures[slot].src_w=w;s->textures[slot].src_h=h;
    s->textures[slot].w_mask=pw-1;s->textures[slot].h_mask=ph-1;
    s->textures[slot].has_alpha=ha;
    vm->r[1]=(i32)slot;
}

/* =========================================================================
 * Software 3D rasteriser (ported from allegro-dos backend)
 * =========================================================================
 * Vertex format: float pos[3], normal[3], uv[2]  (VSIZE = 32 bytes)
 * ========================================================================= */

static void r3d_read_vec3(RelicVM *vm, u32 ptr, float out[3]) {
    if (ptr + 12 > vm->mem_sz) { out[0] = out[1] = out[2] = 0; return; }
    memcpy(out, vm->mem + ptr, 12);
}

static void r3d_mat4_mul(const float a[16], const float b[16], float out[16]) {
    float r[16]; int c, row, k;
    for (c = 0; c < 4; c++)
        for (row = 0; row < 4; row++) {
            float v = 0;
            for (k = 0; k < 4; k++) v += a[k*4+row] * b[c*4+k];
            r[c*4+row] = v;
        }
    memcpy(out, r, sizeof(r));
}

static void r3d_mat4_xform(const float m[16], const float p[3], float out[4]) {
    out[0] = m[0]*p[0] + m[4]*p[1] + m[8]*p[2]  + m[12];
    out[1] = m[1]*p[0] + m[5]*p[1] + m[9]*p[2]  + m[13];
    out[2] = m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14];
    out[3] = m[3]*p[0] + m[7]*p[1] + m[11]*p[2] + m[15];
}

/* Per-vertex rasteriser data */
typedef struct { float sx, sy, invw, r, g, b, tu, tv; } R3DSwVert;

/* Sub-span width for perspective-correct UV restart */
#define R3D_SW_SPAN 8

/* 16.16 fixed-point helpers */
#define R3D_FP_SHIFT 16
#define R3D_FP_ONE   (1 << R3D_FP_SHIFT)
#define R3D_FLOAT_TO_FP(x) ((int)((x) * (float)R3D_FP_ONE))

/* Inline pixel write to 32-bit BGRA framebuffer */
static inline void r3d_sw_put(u8 *fb, int stride, int x, int y, int r, int g, int b) {
    u32 *row = (u32*)(fb + y * stride);
    row[x] = (u32)b | ((u32)g << 8) | ((u32)r << 16);
}

static void r3d_sw_draw_tri(R3DState *s,
                            R3DSwVert v0, R3DSwVert v1, R3DSwVert v2,
                            const SwTexture *tex)
{
    R3DSwVert *a = &v0, *b = &v1, *c = &v2, *tmp;
    int W = s->sw_zbuf_w, H = s->sw_zbuf_h;
    float z_scale = 65535.0f * (s->znear > 0.0f ? s->znear : 1.0f);

    /* Sort top→bottom. */
    if (a->sy > b->sy) { tmp=a; a=b; b=tmp; }
    if (b->sy > c->sy) { tmp=b; b=c; c=tmp; }
    if (a->sy > b->sy) { tmp=a; a=b; b=tmp; }

    int y_top = (int)(a->sy + 0.5f);
    int y_mid = (int)(b->sy + 0.5f);
    int y_bot = (int)(c->sy + 0.5f);
    if (y_top == y_bot || y_bot <= 0 || y_top >= H) return;

#define R3D_DLTA(F,P,Q,D) (((Q)->F - (P)->F) / (float)(D))

    float dy_ac = (float)(y_bot - y_top);
    if (dy_ac < 1e-4f) return;

    /* Long-edge (a→c) deltas per scanline. */
    float ac_dx=R3D_DLTA(sx,a,c,dy_ac), ac_dw=R3D_DLTA(invw,a,c,dy_ac);
    float ac_dr=R3D_DLTA(r,a,c,dy_ac),  ac_dg=R3D_DLTA(g,a,c,dy_ac),  ac_db=R3D_DLTA(b,a,c,dy_ac);
    float ac_du=R3D_DLTA(tu,a,c,dy_ac), ac_dv=R3D_DLTA(tv,a,c,dy_ac);

    float pre0 = (float)y_top - a->sy + 0.5f;
    float ac_x=a->sx+ac_dx*pre0, ac_w=a->invw+ac_dw*pre0;
    float ac_r=a->r+ac_dr*pre0,  ac_g=a->g+ac_dg*pre0,  ac_b=a->b+ac_db*pre0;
    float ac_u=a->tu+ac_du*pre0, ac_v=a->tv+ac_dv*pre0;

    /* Cache texture fields */
    int wm=0, hm=0, tw=0, tw_shift=0, akey=0; const u8 *tpx=NULL;
    if (tex) {
        wm=tex->w_mask; hm=tex->h_mask; tw=tex->w; tpx=tex->pixels;
        akey=tex->has_alpha;
        int tmp_w = tw; tw_shift = 0;
        while (tmp_w > 1) { tw_shift++; tmp_w >>= 1; }
    }

    int half;
    for (half = 0; half < 2; half++) {
        R3DSwVert *sv  = (half == 0) ? a : b;
        R3DSwVert *bv  = (half == 0) ? b : c;
        int y0 = (half == 0) ? y_top : y_mid;
        int y1 = (half == 0) ? y_mid : y_bot;
        if (y0 == y1) continue;

        float dys = (float)(y1 - y0);
        float pre2 = (float)y0 - sv->sy + 0.5f;
        float sh_dx=R3D_DLTA(sx,sv,bv,dys), sh_dw=R3D_DLTA(invw,sv,bv,dys);
        float sh_dr=R3D_DLTA(r,sv,bv,dys),  sh_dg=R3D_DLTA(g,sv,bv,dys),  sh_db=R3D_DLTA(b,sv,bv,dys);
        float sh_du=R3D_DLTA(tu,sv,bv,dys), sh_dv=R3D_DLTA(tv,sv,bv,dys);
        float sh_x=sv->sx+sh_dx*pre2, sh_w=sv->invw+sh_dw*pre2;
        float sh_r=sv->r+sh_dr*pre2,  sh_g=sv->g+sh_dg*pre2,  sh_b=sv->b+sh_db*pre2;
        float sh_u=sv->tu+sh_du*pre2, sh_v=sv->tv+sh_dv*pre2;

        int y;
        for (y = y0; y < y1; y++,
             ac_x+=ac_dx, ac_w+=ac_dw, ac_r+=ac_dr, ac_g+=ac_dg, ac_b+=ac_db, ac_u+=ac_du, ac_v+=ac_dv,
             sh_x+=sh_dx, sh_w+=sh_dw, sh_r+=sh_dr, sh_g+=sh_dg, sh_b+=sh_db, sh_u+=sh_du, sh_v+=sh_dv)
        {
            if (y < 0 || y >= H) continue;
            /* Per-scanline clip-rect check (zero per-pixel cost: the
             * for-loop increment still advances the ac_x/sh_x edge
             * interpolants when we continue, so they stay in sync). */
            if (s->clip_enabled && (y < s->clip_y || y >= s->clip_y + s->clip_h)) continue;

            float lx, rx, lw, rw, lr, rr, lg, rg, lb, rb, lu, ru, lv, rv;
            if (ac_x <= sh_x) {
                lx=ac_x; rx=sh_x; lw=ac_w; rw=sh_w;
                lr=ac_r; rr=sh_r; lg=ac_g; rg=sh_g; lb=ac_b; rb=sh_b;
                lu=ac_u; ru=sh_u; lv=ac_v; rv=sh_v;
            } else {
                lx=sh_x; rx=ac_x; lw=sh_w; rw=ac_w;
                lr=sh_r; rr=ac_r; lg=sh_g; rg=ac_g; lb=sh_b; rb=ac_b;
                lu=sh_u; ru=ac_u; lv=sh_v; rv=ac_v;
            }
            int x0s = (int)(lx+0.5f); if (x0s < 0) x0s = 0;
            int x1s = (int)(rx+0.5f); if (x1s > W) x1s = W;
            /* Clamp scanline to the clip rect. fxp is derived from x0s
             * below, so advancing x0s automatically advances c_w/c_u/c_v/
             * icr/icg/icb/iz to the correct values at the clamped start
             * — no per-pixel cost. */
            if (s->clip_enabled) {
                if (x0s < s->clip_x) x0s = s->clip_x;
                if (x1s > s->clip_x + s->clip_w) x1s = s->clip_x + s->clip_w;
            }
            int span = x1s - x0s;
            if (span <= 0) continue;

            float edge_span = rx - lx;
            float inv_sp = (edge_span > 1e-6f) ? (1.0f / edge_span) : (1.0f / (float)span);
            float fxp = (float)x0s - lx + 0.5f;

            float d_w = (rw - lw) * inv_sp;
            float c_w = lw + d_w * fxp;

            int idr = R3D_FLOAT_TO_FP((rr-lr)*inv_sp);
            int idg = R3D_FLOAT_TO_FP((rg-lg)*inv_sp);
            int idb = R3D_FLOAT_TO_FP((rb-lb)*inv_sp);
            int icr = R3D_FLOAT_TO_FP(lr + (rr-lr)*inv_sp*fxp);
            int icg = R3D_FLOAT_TO_FP(lg + (rg-lg)*inv_sp*fxp);
            int icb = R3D_FLOAT_TO_FP(lb + (rb-lb)*inv_sp*fxp);

            float d_u = (ru-lu)*inv_sp, d_v = (rv-lv)*inv_sp;
            float c_u = lu + d_u*fxp,   c_v = lv + d_v*fxp;

            u16 *zrow = s->sw_zbuf + y * W;
            int x = x0s;

            if (tpx) {
                /* Textured path: perspective UV every R3D_SW_SPAN pixels. */
                int iz  = (int)(c_w * z_scale);
                int idz = (int)(d_w * z_scale);
                while (x < x1s) {
                    int xe = x + R3D_SW_SPAN; if (xe > x1s) xe = x1s;
                    int sub = xe - x;
                    int pr = icr >> R3D_FP_SHIFT; if (pr < 0) pr = 0; if (pr > 255) pr = 255;
                    int pg = icg >> R3D_FP_SHIFT; if (pg < 0) pg = 0; if (pg > 255) pg = 255;
                    int pb = icb >> R3D_FP_SHIFT; if (pb < 0) pb = 0; if (pb > 255) pb = 255;
                    icr += idr * sub; icg += idg * sub; icb += idb * sub;
                    {
                        float w0 = (c_w > 1e-9f) ? 1.0f/c_w : 1e9f;
                        if (w0 > 32767.0f) w0 = 32767.0f;
                        float ew = c_w + d_w*(float)sub;
                        float w1 = (ew > 1e-9f) ? 1.0f/ew : 1e9f;
                        if (w1 > 32767.0f) w1 = 32767.0f;
                        int iu0 = R3D_FLOAT_TO_FP(c_u * w0);
                        int iv0 = R3D_FLOAT_TO_FP(c_v * w0);
                        int iu1 = R3D_FLOAT_TO_FP((c_u + d_u*(float)sub) * w1);
                        int iv1 = R3D_FLOAT_TO_FP((c_v + d_v*(float)sub) * w1);
                        int idu = (sub > 1) ? (iu1-iu0)/sub : 0;
                        int idv = (sub > 1) ? (iv1-iv0)/sub : 0;
                        int iu = iu0, iv = iv0;
                        if (pr >= 255 && pg >= 255 && pb >= 255) {
                            /* Full-bright textured — direct u32 write. */
                            u32 *row32 = (u32*)(s->fb + y * s->fb_stride);
                            int xi;
                            for (xi = x; xi < xe; xi++, iu+=idu, iv+=idv, iz+=idz) {
                                u16 nz = (iz >= 65535) ? 65535u : (iz <= 0 ? 0u : (u16)iz);
                                if (nz <= zrow[xi]) continue;
                                int tx = (iu >> R3D_FP_SHIFT) & wm;
                                int ty = (iv >> R3D_FP_SHIFT) & hm;
                                const u8 *px = tpx + (((ty << tw_shift) + tx) << 2);
                                if (akey && px[3] == 0) continue;
                                zrow[xi] = nz;
                                row32[xi] = (u32)px[2] | ((u32)px[1] << 8) | ((u32)px[0] << 16);
                            }
                        } else {
                            u32 *row32 = (u32*)(s->fb + y * s->fb_stride);
                            int xi;
                            for (xi = x; xi < xe; xi++, iu+=idu, iv+=idv, iz+=idz) {
                                u16 nz = (iz >= 65535) ? 65535u : (iz <= 0 ? 0u : (u16)iz);
                                if (nz <= zrow[xi]) continue;
                                int tx = (iu >> R3D_FP_SHIFT) & wm;
                                int ty = (iv >> R3D_FP_SHIFT) & hm;
                                const u8 *px = tpx + (((ty << tw_shift) + tx) << 2);
                                if (akey && px[3] == 0) continue;
                                zrow[xi] = nz;
                                row32[xi] = (u32)((px[2]*pb)>>8) | ((u32)((px[1]*pg)>>8) << 8) | ((u32)((px[0]*pr)>>8) << 16);
                            }
                        }
                    }
                    c_w += d_w * (float)sub;
                    c_u += d_u * (float)sub;
                    c_v += d_v * (float)sub;
                    x = xe;
                }
            } else {
                /* Untextured (Gouraud only) path. */
                int iz  = (int)(c_w * z_scale);
                int idz = (int)(d_w * z_scale);
                u32 *row32 = (u32*)(s->fb + y * s->fb_stride);
                while (x < x1s) {
                    int xe = x + R3D_SW_SPAN; if (xe > x1s) xe = x1s;
                    int sub = xe - x;
                    int pr = icr >> R3D_FP_SHIFT; if (pr < 0) pr = 0; if (pr > 255) pr = 255;
                    int pg = icg >> R3D_FP_SHIFT; if (pg < 0) pg = 0; if (pg > 255) pg = 255;
                    int pb = icb >> R3D_FP_SHIFT; if (pb < 0) pb = 0; if (pb > 255) pb = 255;
                    icr += idr * sub; icg += idg * sub; icb += idb * sub;
                    u32 packed = (u32)pb | ((u32)pg << 8) | ((u32)pr << 16);
                    int xi;
                    for (xi = x; xi < xe; xi++, iz += idz) {
                        u16 nz = (iz >= 65535) ? 65535u : (iz <= 0 ? 0u : (u16)iz);
                        if (nz <= zrow[xi]) continue;
                        zrow[xi] = nz;
                        row32[xi] = packed;
                    }
                    x = xe;
                }
            }
        }
    }
#undef R3D_DLTA
}

/* Near-plane (w >= znear) Sutherland-Hodgman clip in clip space. */
#define R3D_SW_MAXCLIP 8
typedef struct { float x,y,z,w, r,g,b, tu,tv; } R3DClipVert;

static int r3d_clip_near(const R3DClipVert *in, int n, R3DClipVert *out, float znear) {
    int m = 0, i;
    for (i = 0; i < n; i++) {
        const R3DClipVert *a = &in[i], *b = &in[(i+1)%n];
        int ai = (a->w >= znear), bi = (b->w >= znear);
        if (ai && m < R3D_SW_MAXCLIP) out[m++] = *a;
        if (ai != bi && m < R3D_SW_MAXCLIP) {
            float da=a->w-znear, db=b->w-znear, denom=da-db;
            float t = (denom != 0.0f) ? da/denom : 0.0f;
            if (t<0) t=0; else if (t>1) t=1;
            R3DClipVert cv;
            cv.x=a->x+(b->x-a->x)*t; cv.y=a->y+(b->y-a->y)*t;
            cv.z=a->z+(b->z-a->z)*t; cv.w=znear;
            cv.r=a->r+(b->r-a->r)*t; cv.g=a->g+(b->g-a->g)*t; cv.b=a->b+(b->b-a->b)*t;
            cv.tu=a->tu+(b->tu-a->tu)*t; cv.tv=a->tv+(b->tv-a->tv)*t;
            out[m++] = cv;
        }
    }
    return m;
}

static void r3d_gfx_camera(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s) return;
    u32 eye_ptr=(u32)vm->r[1], tgt_ptr=(u32)vm->r[2], up_ptr=(u32)vm->r[3];
    R3DFBits fb; fb.i=vm->r[4]; float fov_deg=fb.f;
    fb.i=vm->r[5]; float znear=fb.f; fb.i=vm->r[6]; float zfar=fb.f;
    float eye[3],tgt[3],up[3];
    r3d_read_vec3(vm,eye_ptr,eye); r3d_read_vec3(vm,tgt_ptr,tgt); r3d_read_vec3(vm,up_ptr,up);

    s->cam_mirror=false;
    if(fov_deg<0.0f){s->cam_mirror=true;fov_deg=-fov_deg;}
    if(fov_deg<=0.0f)fov_deg=60.0f;
    if(znear<=0.0f)znear=0.1f;
    if(zfar<=znear)zfar=znear+1000.0f;
    s->znear=znear; s->zfar=zfar;

    /* Aspect ratio from 3D viewport if set, else framebuffer */
    int vp_w,vp_h;
    if(s->vp_3d_w>0&&s->vp_3d_h>0){vp_w=s->vp_3d_w;vp_h=s->vp_3d_h;}
    else{vp_w=s->stretch_enabled?s->logical_w:s->fb_w; vp_h=s->stretch_enabled?s->logical_h:s->fb_h;}
    float aspect=(vp_h>0)?(float)vp_w/(float)vp_h:1.0f;
    float fov_rad=fov_deg*(3.14159265358979f/180.0f);
    float f=1.0f/(float)tan(fov_rad*0.5f);
    memset(s->proj,0,sizeof(s->proj));
    s->proj[0]=s->cam_mirror?-(f/aspect):(f/aspect);
    s->proj[5]=f;
    s->proj[10]=(zfar+znear)/(znear-zfar);
    s->proj[11]=-1.0f;
    s->proj[14]=(2.0f*zfar*znear)/(znear-zfar);

    /* Right-handed look-at */
    float fx=tgt[0]-eye[0],fy=tgt[1]-eye[1],fz=tgt[2]-eye[2];
    float fl=(float)sqrt(fx*fx+fy*fy+fz*fz); if(fl<1e-6f)fl=1.0f;
    fx/=fl;fy/=fl;fz/=fl;
    float sx=fy*up[2]-fz*up[1], sy=fz*up[0]-fx*up[2], sz=fx*up[1]-fy*up[0];
    float sl=(float)sqrt(sx*sx+sy*sy+sz*sz); if(sl<1e-6f)sl=1.0f;
    sx/=sl;sy/=sl;sz/=sl;
    float ux=sy*fz-sz*fy, uy=sz*fx-sx*fz, uz=sx*fy-sy*fx;
    s->view[0]=sx;  s->view[4]=sy;  s->view[8]=sz;   s->view[12]=-(sx*eye[0]+sy*eye[1]+sz*eye[2]);
    s->view[1]=ux;  s->view[5]=uy;  s->view[9]=uz;   s->view[13]=-(ux*eye[0]+uy*eye[1]+uz*eye[2]);
    s->view[2]=-fx; s->view[6]=-fy; s->view[10]=-fz; s->view[14]= (fx*eye[0]+fy*eye[1]+fz*eye[2]);
    s->view[3]=0;   s->view[7]=0;   s->view[11]=0;   s->view[15]=1;

    s->have_camera=true;
    r3d_mat4_mul(s->proj,s->view,s->mvp);
    /* Allocate z-buffer if needed */
    int zw=s->fb_w, zh=s->fb_h;
    if(!s->sw_zbuf||s->sw_zbuf_w!=zw||s->sw_zbuf_h!=zh){
        if(s->sw_zbuf)free(s->sw_zbuf);
        s->sw_zbuf=(u16*)malloc((size_t)zw*(size_t)zh*sizeof(u16));
        s->sw_zbuf_w=zw; s->sw_zbuf_h=zh;
    }
    if(s->sw_zbuf) memset(s->sw_zbuf,0,(size_t)zw*(size_t)zh*sizeof(u16));
    s->frame_dirty=true;
}

static void r3d_gfx_light(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s) return;
    u32 dir_ptr=(u32)vm->r[1], rgb_ptr=(u32)vm->r[2], amb_ptr=(u32)vm->r[3];
    if(dir_ptr==0){s->light_on=false;return;}
    float dir[3]; r3d_read_vec3(vm,dir_ptr,dir);
    float dl=(float)sqrt(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
    if(dl<1e-6f)dl=1.0f;
    s->light_dir[0]=dir[0]/dl; s->light_dir[1]=dir[1]/dl; s->light_dir[2]=dir[2]/dl;
    s->light_rgb[0]=1;s->light_rgb[1]=1;s->light_rgb[2]=1;
    if(rgb_ptr+3<=vm->mem_sz){s->light_rgb[0]=vm->mem[rgb_ptr]/255.0f;s->light_rgb[1]=vm->mem[rgb_ptr+1]/255.0f;s->light_rgb[2]=vm->mem[rgb_ptr+2]/255.0f;}
    s->light_amb[0]=0.1f;s->light_amb[1]=0.1f;s->light_amb[2]=0.1f;
    if(amb_ptr+3<=vm->mem_sz){s->light_amb[0]=vm->mem[amb_ptr]/255.0f;s->light_amb[1]=vm->mem[amb_ptr+1]/255.0f;s->light_amb[2]=vm->mem[amb_ptr+2]/255.0f;}
    s->light_on=true; s->frame_dirty=true;
}

static void r3d_gfx_mesh(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data;
    if(!s||!s->fb||!s->sw_zbuf||!s->have_camera) return;

    u32 verts_ptr=(u32)vm->r[1]; i32 vcount=vm->r[2];
    u32 idx_ptr=(u32)vm->r[3];   i32 icount=vm->r[4];
    i32 tex_id=vm->r[5];         u32 rgba_ptr=(u32)vm->r[6];

    /* Bit 31 of icount signals u32 index mode (RLM v3). */
    int idx_u32=(icount<0);
    if(idx_u32) icount=icount&0x7FFFFFFF;

    if(vcount<=0) return;
    const size_t VSIZE=8*sizeof(float);
    size_t idx_stride=idx_u32?4:2;
    if((uint64_t)verts_ptr+(uint64_t)vcount*VSIZE>(uint64_t)vm->mem_sz) return;
    if(idx_ptr&&(uint64_t)idx_ptr+(uint64_t)icount*idx_stride>(uint64_t)vm->mem_sz) return;

    float cr=1,cg=1,cb=1;
    if(rgba_ptr&&rgba_ptr+4<=vm->mem_sz){cr=vm->mem[rgba_ptr]/255.0f;cg=vm->mem[rgba_ptr+1]/255.0f;cb=vm->mem[rgba_ptr+2]/255.0f;}

    const SwTexture *tex=NULL; int tex_src_w=1,tex_src_h=1;
    if(tex_id>0&&tex_id<s->tex_cap&&s->textures[tex_id].pixels){
        tex=&s->textures[tex_id]; tex_src_w=tex->src_w; tex_src_h=tex->src_h;
    }

    const float *mvp=s->mvp;

    /* Per-vertex clip-space data. Stack for small meshes. */
    typedef struct { float cx,cy,cz,cw, r,g,b, tu,tv; } VXf;
#define R3D_VXF_STACK 256
    VXf vx_stack[R3D_VXF_STACK];
    VXf *vx=(vcount<=R3D_VXF_STACK)?vx_stack:(VXf*)malloc(sizeof(VXf)*(size_t)vcount);
    if(!vx) return;

    /* Viewport centre and half-size */
    float vp_cx,vp_cy,vp_hw,vp_hh;
    if(s->vp_3d_w>0&&s->vp_3d_h>0){
        vp_cx=(float)(s->vp_3d_x+s->vp_3d_w*0.5f); vp_cy=(float)(s->vp_3d_y+s->vp_3d_h*0.5f);
        vp_hw=(float)(s->vp_3d_w*0.5f); vp_hh=(float)(s->vp_3d_h*0.5f);
    } else {
        vp_cx=(float)(s->fb_w*0.5f); vp_cy=(float)(s->fb_h*0.5f);
        vp_hw=(float)(s->fb_w*0.5f); vp_hh=(float)(s->fb_h*0.5f);
    }

    /* Whole-mesh cull: skip if every vertex is behind the camera. */
    {
        const float *V=s->view;
        float rx=V[2],ry=V[6],rz=V[10],rd=V[14];
        int all_behind=1,ci;
        for(ci=0;ci<vcount&&all_behind;ci++){
            const float *v=(const float*)(vm->mem+verts_ptr+(size_t)ci*VSIZE);
            float vz=rx*v[0]+ry*v[1]+rz*v[2]+rd;
            if(vz<=0.0f) all_behind=0;
        }
        if(all_behind){if(vx!=vx_stack)free(vx);return;}
    }

    int i;
    for(i=0;i<vcount;i++){
        const float *v=(const float*)(vm->mem+verts_ptr+(size_t)i*VSIZE);
        float pos[3]={v[0],v[1],v[2]}, nrm[3]={v[3],v[4],v[5]};
        float clip[4]; r3d_mat4_xform(mvp,pos,clip);
        vx[i].cx=clip[0]; vx[i].cy=clip[1]; vx[i].cz=clip[2]; vx[i].cw=clip[3];

        float lr=cr,lg=cg,lb=cb;
        if(s->light_on){
            float nl=nrm[0]*(-s->light_dir[0])+nrm[1]*(-s->light_dir[1])+nrm[2]*(-s->light_dir[2]);
            if(nl<0)nl=0;
            lr=cr*(s->light_amb[0]+s->light_rgb[0]*nl);
            lg=cg*(s->light_amb[1]+s->light_rgb[1]*nl);
            lb=cb*(s->light_amb[2]+s->light_rgb[2]*nl);
            if(lr>1)lr=1; if(lg>1)lg=1; if(lb>1)lb=1;
        }
        vx[i].r=lr*255.0f; vx[i].g=lg*255.0f; vx[i].b=lb*255.0f;
        vx[i].tu=v[6]*(float)tex_src_w;
        vx[i].tv=v[7]*(float)tex_src_h;
    }

    const float ZNEAR=s->znear>0.0f?s->znear:0.05f;
    int max_tris=(idx_ptr&&icount>0)?(icount/3):(vcount/3);
    int t;
    for(t=0;t<max_tris;t++){
        int i0,i1,i2;
        if(idx_ptr&&icount>0){
            if(idx_u32){u32 o=idx_ptr+(u32)t*12;
                i0=vm->mem[o]|(vm->mem[o+1]<<8)|(vm->mem[o+2]<<16)|(vm->mem[o+3]<<24);
                i1=vm->mem[o+4]|(vm->mem[o+5]<<8)|(vm->mem[o+6]<<16)|(vm->mem[o+7]<<24);
                i2=vm->mem[o+8]|(vm->mem[o+9]<<8)|(vm->mem[o+10]<<16)|(vm->mem[o+11]<<24);
            }else{u32 o=idx_ptr+(u32)t*6;
                i0=vm->mem[o]|(vm->mem[o+1]<<8); i1=vm->mem[o+2]|(vm->mem[o+3]<<8); i2=vm->mem[o+4]|(vm->mem[o+5]<<8);
            }
        } else{i0=t*3;i1=t*3+1;i2=t*3+2;}
        if(i0<0||i0>=vcount||i1<0||i1>=vcount||i2<0||i2>=vcount) continue;

        /* Clip-space trivial reject */
        {
            const VXf *p0=&vx[i0],*p1=&vx[i1],*p2=&vx[i2];
            if(p0->cw<ZNEAR&&p1->cw<ZNEAR&&p2->cw<ZNEAR) continue;
            if(p0->cw>0.0f&&p1->cw>0.0f&&p2->cw>0.0f){
                if(p0->cx<-p0->cw&&p1->cx<-p1->cw&&p2->cx<-p2->cw) continue;
                if(p0->cx> p0->cw&&p1->cx> p1->cw&&p2->cx> p2->cw) continue;
                if(p0->cy> p0->cw&&p1->cy> p1->cw&&p2->cy> p2->cw) continue;
                if(p0->cy<-p0->cw&&p1->cy<-p1->cw&&p2->cy<-p2->cw) continue;
            }
        }

        /* Near-plane clip */
        R3DClipVert cin[3], cout[R3D_SW_MAXCLIP]; int j;
        for(j=0;j<3;j++){int idx=(j==0)?i0:(j==1)?i1:i2;
            cin[j].x=vx[idx].cx;cin[j].y=vx[idx].cy;cin[j].z=vx[idx].cz;cin[j].w=vx[idx].cw;
            cin[j].r=vx[idx].r;cin[j].g=vx[idx].g;cin[j].b=vx[idx].b;
            cin[j].tu=vx[idx].tu;cin[j].tv=vx[idx].tv;}
        int nc=r3d_clip_near(cin,3,cout,ZNEAR);
        if(nc<3) continue;

        /* Back-face cull */
        {
            float iw0=1.0f/cout[0].w,iw1=1.0f/cout[1].w,iw2=1.0f/cout[2].w;
            float sx0=vp_cx+cout[0].x*iw0*vp_hw, sy0=vp_cy-cout[0].y*iw0*vp_hh;
            float sx1=vp_cx+cout[1].x*iw1*vp_hw, sy1=vp_cy-cout[1].y*iw1*vp_hh;
            float sx2=vp_cx+cout[2].x*iw2*vp_hw, sy2=vp_cy-cout[2].y*iw2*vp_hh;
            float crs=(sx1-sx0)*(sy2-sy0)-(sy1-sy0)*(sx2-sx0);
            if(s->cam_mirror){if(crs<=-0.5f)continue;}else{if(crs>=0.5f)continue;}
        }

        /* Fan-triangulate and rasterise */
        int k;
        for(k=1;k+1<nc;k++){
            R3DSwVert sv[3]; int fi;
            for(fi=0;fi<3;fi++){
                const R3DClipVert *cv=&cout[(fi==0)?0:(fi==1)?k:k+1];
                float invw=1.0f/cv->w;
                sv[fi].sx=vp_cx+cv->x*invw*vp_hw;
                sv[fi].sy=vp_cy-cv->y*invw*vp_hh;
                sv[fi].invw=invw;
                sv[fi].r=cv->r; sv[fi].g=cv->g; sv[fi].b=cv->b;
                sv[fi].tu=cv->tu*invw; sv[fi].tv=cv->tv*invw;
            }
            r3d_sw_draw_tri(s,sv[0],sv[1],sv[2],tex);
        }
    }
    if(vx!=vx_stack) free(vx);
#undef R3D_VXF_STACK
    s->frame_dirty=true;
}

static void r3d_sync_size(R3DState *s) {
    if(!s||!s->hwnd)return; RECT rc; if(!GetClientRect(s->hwnd,&rc))return;
    int pw=rc.right-rc.left,ph=rc.bottom-rc.top; if(pw<=0||ph<=0)return;
    if(s->stretch_enabled){s->pixel_w=pw;s->pixel_h=ph;
        if(s->logical_w<=0||s->logical_h<=0)return;
        float sx=(float)pw/(float)s->logical_w,sy=(float)ph/(float)s->logical_h,sc=(sx<sy)?sx:sy;
        int vw=(int)((float)s->logical_w*sc),vh=(int)((float)s->logical_h*sc);
        if(vw<1)vw=1;if(vh<1)vh=1;s->vp_w=vw;s->vp_h=vh;s->vp_x=(pw-vw)/2;s->vp_y=(ph-vh)/2;return;}
    s->reported_width=pw;s->reported_height=ph;
    if(pw==s->width&&ph==s->height)return;
    s->width=pw;s->height=ph;s->pixel_w=pw;s->pixel_h=ph;
    r3d_create_framebuffer(s,pw,ph);
}

static void r3d_pump_events(R3DState *s) {
    if(!s)return; r3d_sync_size(s);
    MSG msg; while(PeekMessageA(&msg,NULL,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageA(&msg);
        if(msg.message==WM_QUIT){s->quit=true;if(g_vm)g_vm->running=false;}}
}

static void r3d_inp_key(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->hwnd){vm->r[1]=0;return;}
    r3d_pump_events(s); vm->r[1]=(i32)s->last_key; s->last_key=0;
}
static void r3d_inp_keyheld(RelicVM *vm) {
    R3DState *s=(R3DState*)vm->backend_data; if(!s||!s->hwnd){vm->r[1]=0;return;}
    r3d_pump_events(s); u8 k=(u8)vm->r[1];
    /* WINFLAG_CLOSE_REQUEST dedicated channel: @key_held(0) reports the
     * window close action and is never a real key. The first poll of it
     * disables the legacy fake-Escape for this window. */
    if (k==0) { s->close_chan0=true; s->close_requested=false;
        vm->r[1]=s->close_pending?1:0; s->close_pending=false; return; }
    if (k==27 && s->close_requested) { s->close_requested=false; vm->r[1]=1; return; }
    vm->r[1]=s->keys[k]?1:0;
}
static void r3d_inp_mouse(RelicVM *vm) {
    u32 ptr=(u32)vm->r[1]; if(ptr+16u>vm->mem_sz)return;
    R3DState *s=(R3DState*)vm->backend_data; i32 x=0,y=0,b=0,w=0;
    if(s&&s->hwnd){r3d_pump_events(s);x=s->mouse_x;y=s->mouse_y;b=s->mouse_buttons;
        /* raw accumulator -> whole notches out, keep the remainder */
        w=s->mouse_wheel/WHEEL_DELTA; s->mouse_wheel-=w*WHEEL_DELTA;}
    memcpy(vm->mem+ptr,&x,4);memcpy(vm->mem+ptr+4,&y,4);memcpy(vm->mem+ptr+8,&b,4);memcpy(vm->mem+ptr+12,&w,4);
}

static void r3d_inp_gamepad_count(RelicVM *vm) {
    r3d_xinput_load();
    if (!g_xinput_get_state) { vm->r[1] = 0; return; }
    int count = 0;
    RELIC_XINPUT_STATE st;
    for (int i = 0; i < RELIC_XINPUT_MAX_USERS; i++) {
        if (g_xinput_get_state((DWORD)i, &st) == ERROR_SUCCESS)
            count++;
    }
    vm->r[1] = count;
}

static void r3d_inp_gamepad(RelicVM *vm) {
    i32 idx = vm->r[1];
    u32 ptr = (u32)vm->r[2];
    if (ptr + 28u > vm->mem_sz) { vm->r[1] = 0; return; }

    r3d_xinput_load();
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

    i32 buttons = r3d_xinput_map_buttons(st.Gamepad.wButtons);
    i32 lx = (i32)st.Gamepad.sThumbLX;
    i32 ly = (i32)st.Gamepad.sThumbLY;
    i32 rx = (i32)st.Gamepad.sThumbRX;
    i32 ry = (i32)st.Gamepad.sThumbRY;
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

static void r3d_sys_rand(RelicVM *vm)    { vm->r[1]=rand()&0xFFFF; }
static void r3d_sys_millis(RelicVM *vm) {
    /* Process-relative, not system uptime: raw timeGetTime() goes
     * negative in the i32 result once the host has been up ~24.8 days,
     * inverting "now < deadline" timer checks in scripts. The u32 delta
     * is wrap-safe. */
    static DWORD t0 = 0;
    DWORD now = timeGetTime();
    if (t0 == 0) t0 = now;
    vm->r[1] = (i32)(now - t0);
}
static void r3d_sys_console(RelicVM *vm) {
    u32 p=(u32)vm->r[1]; while(p<vm->mem_sz){u8 c=vm->mem[p++];if(!c)break;fputc(c,stdout);}fflush(stdout);
}

#define BACKEND_MAX_FILES 16
typedef struct { bool is_asset; FILE *fp; const u8 *asset_data; u32 asset_size,asset_pos; } BFH;
static BFH r3d_h[BACKEND_MAX_FILES];
static i32 r3d_ha(const u8*d,u32 sz){for(int i=1;i<BACKEND_MAX_FILES;i++)if(!r3d_h[i].fp&&!r3d_h[i].asset_data){r3d_h[i].is_asset=true;r3d_h[i].fp=NULL;r3d_h[i].asset_data=d;r3d_h[i].asset_size=sz;r3d_h[i].asset_pos=0;return i;}return -1;}
static i32 r3d_hf(FILE*f){for(int i=1;i<BACKEND_MAX_FILES;i++)if(!r3d_h[i].fp&&!r3d_h[i].asset_data){r3d_h[i].is_asset=false;r3d_h[i].fp=f;r3d_h[i].asset_data=NULL;return i;}return -1;}
static BFH*r3d_hg(i32 i){if(i<1||i>=BACKEND_MAX_FILES)return NULL;if(!r3d_h[i].fp&&!r3d_h[i].asset_data)return NULL;return &r3d_h[i];}
static void r3d_hx(i32 i){if(i<1||i>=BACKEND_MAX_FILES)return;if(r3d_h[i].fp)fclose(r3d_h[i].fp);memset(&r3d_h[i],0,sizeof(r3d_h[i]));}

static void r3d_sys_fopen(RelicVM *vm) {
    u32 p=(u32)vm->r[1];i32 l=vm->r[2],m=vm->r[3];char path[512]={0};
    if(l<=0){l=0;while(l<511&&p+(u32)l<vm->mem_sz&&vm->mem[p+l]!=0)l++;}
    int n=l<511?(int)l:511;if(p+(u32)n<=vm->mem_sz)memcpy(path,vm->mem+p,n);
    if(m==1){FILE*f=fopen(path,"wb");vm->r[1]=f?r3d_hf(f):-1;if(f&&vm->r[1]==-1)fclose(f);}
    else if(m==2){FILE*f=fopen(path,"rb");vm->r[1]=f?r3d_hf(f):-1;if(f&&vm->r[1]==-1)fclose(f);}
    else{u32 as=0;const u8*a=relic_assets_find(vm->assets,path,&as);
        if(a)vm->r[1]=r3d_ha(a,as);else{FILE*f=fopen(path,"rb");vm->r[1]=f?r3d_hf(f):-1;if(f&&vm->r[1]==-1)fclose(f);}}
}
static void r3d_sys_fread(RelicVM *vm) {
    BFH*h=r3d_hg(vm->r[1]);u32 b=(u32)vm->r[2],n=(u32)vm->r[3];if(!h||b+n>vm->mem_sz){vm->r[1]=0;return;}
    if(h->is_asset){u32 av=h->asset_size-h->asset_pos,rd=n<av?n:av;if(rd)memcpy(vm->mem+b,h->asset_data+h->asset_pos,rd);h->asset_pos+=rd;vm->r[1]=(i32)rd;}
    else vm->r[1]=(i32)fread(vm->mem+b,1,n,h->fp);
}
static void r3d_sys_fwrite(RelicVM *vm) {
    BFH*h=r3d_hg(vm->r[1]);u32 b=(u32)vm->r[2],n=(u32)vm->r[3];if(!h||b+n>vm->mem_sz){vm->r[1]=0;return;}
    if(h->is_asset)vm->r[1]=0;else vm->r[1]=(i32)fwrite(vm->mem+b,1,n,h->fp);
}
static void r3d_sys_fclose(RelicVM *vm) { r3d_hx(vm->r[1]); }
static void r3d_sys_fseek(RelicVM *vm) {
    BFH*h=r3d_hg(vm->r[1]);i32 off=vm->r[2],wh=vm->r[3];if(!h){vm->r[1]=-1;return;}
    if(h->is_asset){u32 np;switch(wh){case 0:np=(u32)off;break;case 1:np=h->asset_pos+(u32)off;break;case 2:np=h->asset_size+(u32)off;break;default:vm->r[1]=-1;return;}
        if(np>h->asset_size)np=h->asset_size;h->asset_pos=np;vm->r[1]=(i32)np;}
    else{int w=(wh==0)?SEEK_SET:(wh==1)?SEEK_CUR:(wh==2)?SEEK_END:SEEK_SET;
        if(fseek(h->fp,(long)off,w)!=0){vm->r[1]=-1;return;}vm->r[1]=(i32)ftell(h->fp);}
}
static void r3d_sys_flen(RelicVM *vm) {
    BFH*h=r3d_hg(vm->r[1]);if(!h){vm->r[1]=-1;return;}
    if(h->is_asset)vm->r[1]=(i32)h->asset_size;
    else{long cur=ftell(h->fp);if(cur<0||fseek(h->fp,0,SEEK_END)!=0){vm->r[1]=-1;return;}
        long end=ftell(h->fp);fseek(h->fp,cur,SEEK_SET);vm->r[1]=(i32)end;}
}

#define PLATFORM_INFO_SIZE 116u
static u32 r3d_ps(RelicVM*vm,u32*c,const char*s){if(!vm||!c||!s)return 0;u32 b=vm->platform_strings_base,sz=vm->platform_strings_size;if(!sz)return 0;u32 n=(u32)strlen(s)+1;if(*c+n>sz)return 0;u32 o=b+*c;memcpy(vm->mem+o,s,n);*c+=n;return o;}
static void r3d_pp(u8*m,int o,u32 p){m[o]=p&0xFF;m[o+1]=(p>>8)&0xFF;m[o+2]=(p>>16)&0xFF;m[o+3]=(p>>24)&0xFF;}
static const char*r3d_ccs(u8 a,u8 c){if(a==0){switch(c){case 0:return"8086";case 1:return"286";case 2:return"386";case 3:return"486";case 4:return"Pentium";case 5:return"MMX";case 6:return"SSE";case 7:return"SSE2";default:return"Unknown";}}if(a==1){switch(c){case 0:return"Baseline";case 1:return"SSE3";case 2:return"SSE4.2";case 3:return"AVX1";case 4:return"AVX2";case 5:return"AVX512";default:return"Unknown";}}if(a==2||a==3){switch(c){case 0:return"v6";case 1:return"v7/Cortex-A";case 2:return"v7/Cortex-M";case 3:return"v8/AArch64";default:return"Unknown";}}return"Unknown";}

static void r3d_sys_platform(RelicVM *vm) {
    u32 ptr=(u32)vm->r[1]; if(ptr+PLATFORM_INFO_SIZE>vm->mem_sz)return;
    u8*mem=vm->mem+ptr; u32 cur=0;
    r3d_pp(mem,0,r3d_ps(vm,&cur,"Windows"));
    {const char*vs="Unknown";char vb[16];OSVERSIONINFOEXW ovi;memset(&ovi,0,sizeof(ovi));ovi.dwOSVersionInfoSize=sizeof(ovi);
     HMODULE nt=GetModuleHandleA("ntdll.dll");if(nt){typedef LONG(WINAPI*F)(OSVERSIONINFOEXW*);F fn=(F)GetProcAddress(nt,"RtlGetVersion");
     if(fn&&fn(&ovi)==0){int v=(ovi.dwMajorVersion==10&&ovi.dwBuildNumber>=22000)?11:(int)ovi.dwMajorVersion;snprintf(vb,sizeof(vb),"%d",v);vs=vb;}}
     /* RtlGetVersion is NT-only; fall back to GetVersionExA on Win9x. */
     if(vs[0]=='U'){OSVERSIONINFOA ova;memset(&ova,0,sizeof(ova));ova.dwOSVersionInfoSize=sizeof(ova);
     if(GetVersionExA(&ova)){snprintf(vb,sizeof(vb),"%d",(int)ova.dwMajorVersion);vs=vb;}}
     r3d_pp(mem,4,r3d_ps(vm,&cur,vs));}
    {u8 a=1,c=0;const char*as="x64";
#if defined(_M_ARM64)||defined(__aarch64__)
     a=3;c=3;as="ARM64";
#elif defined(_M_ARM)||defined(__arm__)
     a=2;c=1;as="ARM";
#else
     int ci[4]={0};__cpuid(ci,1);int e1=ci[2];__cpuid(ci,7);int e7=ci[1];
     if(e7&(1<<16)){a=1;c=5;}else if(e7&(1<<5)){a=1;c=4;}else if(e1&(1<<28)){a=1;c=3;}
     else if(e1&(1<<19)){a=1;c=2;}else if(e1&(1<<0)){a=1;c=1;}else{a=1;c=0;}
#endif
     r3d_pp(mem,8,r3d_ps(vm,&cur,as));r3d_pp(mem,12,r3d_ps(vm,&cur,r3d_ccs(a,c)));}
    r3d_pp(mem,16,r3d_ps(vm,&cur,"Graphics"));
    {u16 sw=(u16)GetSystemMetrics(SM_CXSCREEN),sh=(u16)GetSystemMetrics(SM_CYSCREEN);
     mem[20]=sw&0xFF;mem[21]=(sw>>8)&0xFF;mem[22]=sh&0xFF;mem[23]=(sh>>8)&0xFF;
     u32 co=16777216;HDC hdc=GetDC(NULL);if(hdc){int bp=GetDeviceCaps(hdc,BITSPIXEL)*GetDeviceCaps(hdc,PLANES);ReleaseDC(NULL,hdc);if(bp<=8)co=256;else if(bp<=16)co=65536;}
     mem[24]=co&0xFF;mem[25]=(co>>8)&0xFF;mem[26]=(co>>16)&0xFF;mem[27]=(co>>24)&0xFF;}
    /* sound_flags: all capabilities (tones|samples|MUS|MIDI) */
    {u32 sf=0x0F;mem[28]=sf&0xFF;mem[29]=(sf>>8)&0xFF;mem[30]=(sf>>16)&0xFF;mem[31]=(sf>>24)&0xFF;}
    r3d_pp(mem,32,r3d_ps(vm,&cur,"Read-write"));
    r3d_pp(mem,36,r3d_ps(vm,&cur,"Software"));r3d_pp(mem,40,r3d_ps(vm,&cur,"Relic3D"));
    r3d_pp(mem,44,r3d_ps(vm,&cur,"Software"));r3d_pp(mem,48,r3d_ps(vm,&cur,"1.0"));
    {i32 ww=0,wh=0;if(g_state){r3d_sync_size(g_state);ww=g_state->reported_width;wh=g_state->reported_height;}
     u32 ux=0,uy=0,uw=(u32)ww,uh=(u32)wh;
     mem[52]=ux&0xFF;mem[53]=(ux>>8)&0xFF;mem[54]=(ux>>16)&0xFF;mem[55]=(ux>>24)&0xFF;
     mem[56]=uy&0xFF;mem[57]=(uy>>8)&0xFF;mem[58]=(uy>>16)&0xFF;mem[59]=(uy>>24)&0xFF;
     mem[60]=uw&0xFF;mem[61]=(uw>>8)&0xFF;mem[62]=(uw>>16)&0xFF;mem[63]=(uw>>24)&0xFF;
     mem[64]=uh&0xFF;mem[65]=(uh>>8)&0xFF;mem[66]=(uh>>16)&0xFF;mem[67]=(uh>>24)&0xFF;}
    {const char *js;
     if(!vm->jit_name||!vm->jit_name[0]||strcmp(vm->jit_name,"null")==0) js="Not available";
     else if(!vm->jit_enabled) js="Disabled (--no-jit)";
     else if(strstr(vm->jit_name,"-v2")) js="Enabled (JIT v2)";
     else js="Enabled (JIT v1)";
     r3d_pp(mem,68,r3d_ps(vm,&cur,js));}
    /* CPU speed (MHz), core count, and processor name */
    {u32 sp=0,co=0;char pn[128]="Unknown";SYSTEM_INFO si;GetSystemInfo(&si);co=(u32)si.dwNumberOfProcessors;
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0501
     HMODULE hk32=GetModuleHandleA("kernel32.dll");
     if(hk32){typedef BOOL(WINAPI*GLPI_t)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION,PDWORD);
      GLPI_t pGLPI=(GLPI_t)GetProcAddress(hk32,"GetLogicalProcessorInformation");
      if(pGLPI){DWORD bl=0;pGLPI(NULL,&bl);if(bl>0){
       PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buf=(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)HeapAlloc(GetProcessHeap(),0,bl);
       if(buf&&pGLPI(buf,&bl)){DWORD cnt=bl/sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);u32 ph=0;DWORD i;
        for(i=0;i<cnt;i++)if(buf[i].Relationship==RelationProcessorCore)ph++;
        if(ph>0)co=ph;}
       if(buf)HeapFree(GetProcessHeap(),0,buf);}}}
#endif /* _WIN32_WINNT >= 0x0501 */
     HKEY hk;if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",0,KEY_READ,&hk)==ERROR_SUCCESS){
      DWORD mhz=0,sz=sizeof(mhz);if(RegQueryValueExA(hk,"~MHz",NULL,NULL,(LPBYTE)&mhz,&sz)==ERROR_SUCCESS)sp=(u32)mhz;
      char buf[128];sz=sizeof(buf);if(RegQueryValueExA(hk,"ProcessorNameString",NULL,NULL,(LPBYTE)buf,&sz)==ERROR_SUCCESS){buf[sizeof(buf)-1]=0;char*p=buf;while(*p==' ')p++;if(*p){strncpy(pn,p,sizeof(pn)-1);pn[sizeof(pn)-1]=0;}}
      RegCloseKey(hk);}
     mem[72]=sp&0xFF;mem[73]=(sp>>8)&0xFF;mem[74]=(sp>>16)&0xFF;mem[75]=(sp>>24)&0xFF;
     mem[76]=co&0xFF;mem[77]=(co>>8)&0xFF;mem[78]=(co>>16)&0xFF;mem[79]=(co>>24)&0xFF;
     r3d_pp(mem,80,r3d_ps(vm,&cur,pn));}

    /* touch: 0 = no touch on desktop Windows */
    {u32 t=0;mem[84]=t&0xFF;mem[85]=(t>>8)&0xFF;mem[86]=(t>>16)&0xFF;mem[87]=(t>>24)&0xFF;}
    /* network: 1 = available */
    {u32 n=1;mem[88]=n&0xFF;mem[89]=(n>>8)&0xFF;mem[90]=(n>>16)&0xFF;mem[91]=(n>>24)&0xFF;}
#ifndef RELIC_PLATFORM_ID
#define RELIC_PLATFORM_ID "unknown"
#endif
#ifndef RELIC_BACKEND_ID
#define RELIC_BACKEND_ID "relic3d-win32"
#endif
    r3d_pp(mem,92,r3d_ps(vm,&cur,RELIC_PLATFORM_ID));
    r3d_pp(mem,96,r3d_ps(vm,&cur,RELIC_BACKEND_ID));
    r3d_pp(mem,100,r3d_ps(vm,&cur,RELIC_VERSION));
    r3d_pp(mem,104,r3d_ps(vm,&cur,__DATE__));
    /* Adaptive Build tail: detected tier + physical RAM. */
    r3d_pp(mem,108,(u32)relic_tier_detect());
    r3d_pp(mem,112,(u32)relic_probe_memory_kb());
}

/* =========================================================================
 * Web / HTTP — WinINet + CreateThread worker (see docs/web.md)
 *
 * wininet.dll ships on Win95 (with IE3+) through Win11 and performs HTTP and
 * HTTPS (TLS) for us — proxies, redirects and cert handling all in the OS, so
 * no crypto code lands in the VM. WinINet's calls block, so each request runs
 * on a worker thread that fills the slot and flips the `done` flag; the
 * single-threaded VM only polls. This is the same implementation used by the
 * opengl-win32 backend, ported verbatim.
 * ---------------------------------------------------------------------- */

static CRITICAL_SECTION g_http_lock;
static int g_http_lock_ready = 0;

/* Lazily create the HTTP critical section. Only ever called from the VM
 * thread (the single-threaded VM), so no init race is possible. */
static void http_lock_ensure(void) {
    if (!g_http_lock_ready) {
        InitializeCriticalSection(&g_http_lock);
        g_http_lock_ready = 1;
    }
}

static const char *http_method_str(int method) {
    switch (method) {
        case 0:  return "GET";
        case 1:  return "POST";
        case 2:  return "PUT";
        case 3:  return "DELETE";
        default: return "GET";
    }
}

typedef struct {
    R3DState     *s;
    int           slot;
    int           generation;
    char          method[8];
    char          url[2048];
    char         *headers;      /* malloc'd, or NULL */
    int           headers_len;
    unsigned char *body;        /* malloc'd, or NULL */
    int           body_len;
} HttpWorkerCtx;

static void http_worker_ctx_free(HttpWorkerCtx *c) {
    if (!c) return;
    free(c->headers);
    free(c->body);
    free(c);
}

/* Store a freshly-created WinINet handle into the slot iff it is still valid
 * (not closed/reused). Returns 1 on success; 0 means the caller must close
 * the handle itself and abandon the request. */
static int http_store_handle(R3DState *s, int slot, int gen,
                             HINTERNET *field, HINTERNET h) {
    EnterCriticalSection(&g_http_lock);
    int ok = (s->http[slot].in_use && s->http[slot].generation == gen);
    if (ok) *field = h;
    LeaveCriticalSection(&g_http_lock);
    return ok;
}

static DWORD WINAPI http_worker(LPVOID arg) {
    HttpWorkerCtx *c = (HttpWorkerCtx *)arg;
    R3DState *s = c->s;
    int slot = c->slot, gen = c->generation;
    int err = 0, status = 0;
    unsigned char *body = NULL;
    long body_len = 0;

    /* Split the URL into host / port / path (+ query). */
    URL_COMPONENTSA uc;
    char host[256] = {0}, path[2048] = {0}, extra[2048] = {0};
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize     = sizeof(uc);
    uc.lpszHostName     = host;  uc.dwHostNameLength   = sizeof(host);
    uc.lpszUrlPath      = path;  uc.dwUrlPathLength    = sizeof(path);
    uc.lpszExtraInfo    = extra; uc.dwExtraInfoLength  = sizeof(extra);
    if (!InternetCrackUrlA(c->url, 0, 0, &uc)) { err = 1; goto finish; }

    int is_https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    INTERNET_PORT port = uc.nPort;
    char fullpath[4096];
    snprintf(fullpath, sizeof(fullpath), "%s%s",
             path[0] ? path : "/", extra);
    fullpath[sizeof(fullpath) - 1] = '\0';

    HINTERNET hs = InternetOpenA("Relic/1.0", INTERNET_OPEN_TYPE_PRECONFIG,
                                 NULL, NULL, 0);
    if (!hs) { err = 1; goto finish; }
    if (!http_store_handle(s, slot, gen, &s->http[slot].hSession, hs)) {
        InternetCloseHandle(hs); goto aborted;
    }

    HINTERNET hc = InternetConnectA(hs, host, port, NULL, NULL,
                                    INTERNET_SERVICE_HTTP, 0, 0);
    if (!hc) { err = 1; goto finish; }
    if (!http_store_handle(s, slot, gen, &s->http[slot].hConnect, hc)) {
        InternetCloseHandle(hc); goto aborted;
    }

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE
                | INTERNET_FLAG_KEEP_CONNECTION;
    if (is_https) flags |= INTERNET_FLAG_SECURE;
    HINTERNET hr = HttpOpenRequestA(hc, c->method, fullpath, NULL, NULL,
                                    NULL, flags, 0);
    if (!hr) { err = 1; goto finish; }
    if (!http_store_handle(s, slot, gen, &s->http[slot].hRequest, hr)) {
        InternetCloseHandle(hr); goto aborted;
    }

    BOOL ok = HttpSendRequestA(
        hr,
        c->headers_len > 0 ? c->headers : NULL,
        c->headers_len > 0 ? (DWORD)c->headers_len : 0,
        c->body_len > 0 ? c->body : NULL,
        c->body_len > 0 ? (DWORD)c->body_len : 0);
    if (!ok) { err = 1; goto finish; }

    /* HTTP status code. */
    {
        DWORD sc = 0, sz = sizeof(sc), idx = 0;
        if (HttpQueryInfoA(hr, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                           &sc, &sz, &idx)) {
            status = (int)sc;
        }
    }

    /* Drain the response body into a growing heap buffer. */
    {
        long cap = 0;
        for (;;) {
            if (body_len + 8192 > cap) {
                cap = body_len + 8192;
                unsigned char *nb = (unsigned char *)realloc(body, (size_t)cap);
                if (!nb) { err = 1; break; }
                body = nb;
            }
            DWORD got = 0;
            if (!InternetReadFile(hr, body + body_len, 8192, &got)) { err = 1; break; }
            if (got == 0) break;
            body_len += (long)got;
        }
    }

finish:
    EnterCriticalSection(&g_http_lock);
    if (s->http[slot].in_use && s->http[slot].generation == gen) {
        s->http[slot].data     = body;
        s->http[slot].data_len = body_len;
        s->http[slot].status   = status;
        s->http[slot].done     = err ? -1 : 1;
        /* Take ownership of the handles so we can close them after unlocking;
         * http_close will see NULL and skip them. */
        HINTERNET h1 = s->http[slot].hRequest;
        HINTERNET h2 = s->http[slot].hConnect;
        HINTERNET h3 = s->http[slot].hSession;
        s->http[slot].hRequest = NULL;
        s->http[slot].hConnect = NULL;
        s->http[slot].hSession = NULL;
        LeaveCriticalSection(&g_http_lock);
        if (h1) InternetCloseHandle(h1);
        if (h2) InternetCloseHandle(h2);
        if (h3) InternetCloseHandle(h3);
    } else {
        /* Slot was closed/reused while we ran: http_close already released the
         * stored handles. Just drop our body buffer. */
        LeaveCriticalSection(&g_http_lock);
        free(body);
    }
    http_worker_ctx_free(c);
    return 0;

aborted:
    /* Slot was closed during setup; handles we stored are owned by http_close,
     * and the un-stored handle was closed at the goto site. */
    free(body);
    http_worker_ctx_free(c);
    return 0;
}

/* @http_request(method, url, headers, body, body_len) -> i32 handle */
static void r3d_sys_http_request(RelicVM *vm) {
    R3DState *s = (R3DState *)vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    http_lock_ensure();

    int method   = vm->r[1];
    u32 url_ptr  = (u32)vm->r[2];
    u32 hdr_ptr  = (u32)vm->r[3];
    u32 body_ptr = (u32)vm->r[4];
    i32 body_len = vm->r[5];

    if (url_ptr == 0 || url_ptr >= vm->mem_sz) { vm->r[1] = -1; return; }

    HttpWorkerCtx *c = (HttpWorkerCtx *)calloc(1, sizeof(HttpWorkerCtx));
    if (!c) { vm->r[1] = -1; return; }

    /* Copy URL out of VM memory. */
    int ul = 0;
    while (ul < (int)sizeof(c->url) - 1 && url_ptr + (u32)ul < vm->mem_sz
           && vm->mem[url_ptr + ul] != 0) {
        c->url[ul] = (char)vm->mem[url_ptr + ul]; ul++;
    }
    c->url[ul] = '\0';
    if (ul == 0) { free(c); vm->r[1] = -1; return; }

    /* Copy headers (NUL-terminated block) out of VM memory. */
    if (hdr_ptr != 0 && hdr_ptr < vm->mem_sz) {
        int hl = 0;
        while (hdr_ptr + (u32)hl < vm->mem_sz && vm->mem[hdr_ptr + hl] != 0) hl++;
        if (hl > 0) {
            c->headers = (char *)malloc((size_t)hl);
            if (c->headers) {
                memcpy(c->headers, vm->mem + hdr_ptr, (size_t)hl);
                c->headers_len = hl;
            }
        }
    }

    /* Copy request body out of VM memory. */
    if (body_ptr != 0 && body_len > 0 && body_ptr + (u32)body_len <= vm->mem_sz) {
        c->body = (unsigned char *)malloc((size_t)body_len);
        if (c->body) {
            memcpy(c->body, vm->mem + body_ptr, (size_t)body_len);
            c->body_len = body_len;
        }
    }

    strncpy(c->method, http_method_str(method), sizeof(c->method) - 1);
    c->method[sizeof(c->method) - 1] = '\0';
    c->s = s;

    /* Allocate a slot. */
    int slot = -1;
    EnterCriticalSection(&g_http_lock);
    for (int i = 0; i < RELIC_MAX_HTTP; i++) {
        if (!s->http[i].in_use) {
            slot = i;
            s->http[i].in_use     = true;
            s->http[i].generation++;
            s->http[i].done       = 0;
            s->http[i].status     = 0;
            s->http[i].data       = NULL;
            s->http[i].data_len   = 0;
            s->http[i].read_pos   = 0;
            s->http[i].hSession   = NULL;
            s->http[i].hConnect   = NULL;
            s->http[i].hRequest   = NULL;
            break;
        }
    }
    if (slot >= 0) {
        c->slot = slot;
        c->generation = s->http[slot].generation;
    }
    LeaveCriticalSection(&g_http_lock);
    if (slot < 0) { http_worker_ctx_free(c); vm->r[1] = -1; return; }

    HANDLE th = CreateThread(NULL, 0, http_worker, c, 0, NULL);
    if (!th) {
        EnterCriticalSection(&g_http_lock);
        s->http[slot].in_use = false;
        LeaveCriticalSection(&g_http_lock);
        http_worker_ctx_free(c);
        vm->r[1] = -1;
        return;
    }
    CloseHandle(th);  /* detach; worker frees its own ctx */
    vm->r[1] = slot;
}

/* @http_poll(handle) -> i32 (0=pending, 1=ready, <0=error) */
static void r3d_sys_http_poll(RelicVM *vm) {
    R3DState *s = (R3DState *)vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    int handle = vm->r[1];
    if (handle < 0 || handle >= RELIC_MAX_HTTP) { vm->r[1] = -1; return; }
    http_lock_ensure();
    EnterCriticalSection(&g_http_lock);
    int done = s->http[handle].in_use ? s->http[handle].done : -1;
    LeaveCriticalSection(&g_http_lock);
    vm->r[1] = done;
}

/* @http_status(handle) -> i32 HTTP status code */
static void r3d_sys_http_status(RelicVM *vm) {
    R3DState *s = (R3DState *)vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    int handle = vm->r[1];
    if (handle < 0 || handle >= RELIC_MAX_HTTP) { vm->r[1] = -1; return; }
    http_lock_ensure();
    EnterCriticalSection(&g_http_lock);
    int status = s->http[handle].in_use ? s->http[handle].status : -1;
    LeaveCriticalSection(&g_http_lock);
    vm->r[1] = status;
}

/* @http_read(handle, buf, cap) -> i32 bytes copied (0=EOF) */
static void r3d_sys_http_read(RelicVM *vm) {
    R3DState *s = (R3DState *)vm->backend_data;
    if (!s) { vm->r[1] = -1; return; }
    int handle  = vm->r[1];
    u32 buf_ptr = (u32)vm->r[2];
    i32 cap     = vm->r[3];
    if (handle < 0 || handle >= RELIC_MAX_HTTP || cap <= 0
        || buf_ptr == 0 || buf_ptr >= vm->mem_sz) { vm->r[1] = -1; return; }
    http_lock_ensure();

    EnterCriticalSection(&g_http_lock);
    if (!s->http[handle].in_use) { LeaveCriticalSection(&g_http_lock); vm->r[1] = -1; return; }
    long avail = s->http[handle].data_len - s->http[handle].read_pos;
    if (avail <= 0) { LeaveCriticalSection(&g_http_lock); vm->r[1] = 0; return; }
    long to_copy = avail < cap ? avail : cap;
    if (buf_ptr + (u32)to_copy > vm->mem_sz) to_copy = vm->mem_sz - buf_ptr;
    memcpy(vm->mem + buf_ptr, s->http[handle].data + s->http[handle].read_pos, (size_t)to_copy);
    s->http[handle].read_pos += to_copy;
    LeaveCriticalSection(&g_http_lock);
    vm->r[1] = (i32)to_copy;
}

/* @http_close(handle) */
static void r3d_sys_http_close(RelicVM *vm) {
    R3DState *s = (R3DState *)vm->backend_data;
    if (!s) return;
    int handle = vm->r[1];
    if (handle < 0 || handle >= RELIC_MAX_HTTP) return;
    http_lock_ensure();

    EnterCriticalSection(&g_http_lock);
    if (!s->http[handle].in_use) { LeaveCriticalSection(&g_http_lock); return; }
    s->http[handle].in_use = false;
    s->http[handle].generation++;   /* invalidate any in-flight worker */
    HINTERNET h1 = s->http[handle].hRequest;
    HINTERNET h2 = s->http[handle].hConnect;
    HINTERNET h3 = s->http[handle].hSession;
    s->http[handle].hRequest = NULL;
    s->http[handle].hConnect = NULL;
    s->http[handle].hSession = NULL;
    unsigned char *data = s->http[handle].data;
    s->http[handle].data     = NULL;
    s->http[handle].data_len = 0;
    s->http[handle].read_pos = 0;
    LeaveCriticalSection(&g_http_lock);

    /* Close child-first so closing the session root never double-closes a
     * child. Closing an in-flight handle aborts the worker's blocking call. */
    if (h1) InternetCloseHandle(h1);
    if (h2) InternetCloseHandle(h2);
    if (h3) InternetCloseHandle(h3);
    free(data);
}

/* Called from r3d_shutdown to clean up all in-flight requests. */
static void r3d_http_shutdown(R3DState *s) {
    if (!g_http_lock_ready) return;
    for (int i = 0; i < RELIC_MAX_HTTP; i++) {
        EnterCriticalSection(&g_http_lock);
        HINTERNET h1 = NULL, h2 = NULL, h3 = NULL;
        unsigned char *data = NULL;
        if (s->http[i].in_use) {
            s->http[i].in_use = false;
            s->http[i].generation++;
            h1 = s->http[i].hRequest; h2 = s->http[i].hConnect; h3 = s->http[i].hSession;
            s->http[i].hRequest = NULL; s->http[i].hConnect = NULL; s->http[i].hSession = NULL;
            data = s->http[i].data;
            s->http[i].data = NULL;
        }
        LeaveCriticalSection(&g_http_lock);
        if (h1) InternetCloseHandle(h1);
        if (h2) InternetCloseHandle(h2);
        if (h3) InternetCloseHandle(h3);
        free(data);
    }
}

/* =========================================================================
 * Sector-based world renderer (@wr_*)
 *
 * The renderer engine itself lives in wr_render.h (shared across every
 * backend that supports @wr_*). This backend has no persistent 8bpp
 * hardware framebuffer (the DIBSection is 32bpp BGRA), so wr_render/
 * wr_patch/wr_test instead draw into a private heap-allocated 8bpp
 * scratch surface (wr_surf), which is then expanded through a 256-entry
 * BGRX lookup table and written directly into s->fb by wr_host_present.
 * Drawing immediately (rather than deferring to @flip) means it composes
 * correctly with any other @gfx_* calls in the same frame, exactly like
 * @sprite. See wr_render.h's top comment for the full portability
 * contract.
 * ---------------------------------------------------------------------- */
#define WrHostState R3DState

/* Lazily (re)allocates wr_surf to the window's current logical size.
 * Used as WR_HAS_BUF(s) below, so every renderer entry point that checks
 * "is there a buffer?" also ensures it exists and is correctly sized.
 * Idempotent: a no-op re-check when the size hasn't changed. */
static bool wr_r3d_ensure_surface(R3DState *s) {
    int w = s->width, h = s->height;
    if (w <= 0 || h <= 0) return false;
    if (s->wr_surf && s->wr_surf_w == w && s->wr_surf_h == h) return true;
    if (s->wr_surf) { free(s->wr_surf); s->wr_surf = NULL; }
    s->wr_surf = (unsigned char *) malloc((size_t) w * (size_t) h);
    if (!s->wr_surf) { s->wr_surf_w = 0; s->wr_surf_h = 0; return false; }
    s->wr_surf_w = w;
    s->wr_surf_h = h;
    s->wr_surf_pitch = w;
    return true;
}

#define WR_HAS_BUF(s)   wr_r3d_ensure_surface(s)
#define WR_ROW(s, y)    ((s)->wr_surf + (size_t) (y) * (size_t) (s)->wr_surf_pitch)
#define WR_PITCH(s, y)  ((s)->wr_surf_pitch)
#define WR_BUF_W(s)     ((s)->wr_surf_w)
#define WR_BUF_H(s)     ((s)->wr_surf_h)
#define WR_MILLIS()     ((int) timeGetTime())

/* Clears the sub-rectangle [ox,oy,ox+w,oy+h) of the scratch surface to
 * palette index 0. wr_surf is always a plain linear heap buffer, so a
 * per-row memset always applies. */
static void wr_host_clear_backbuf(R3DState *s, int ox, int oy, int w, int h) {
    int y;
    for (y = 0; y < h; y++) {
        memset(WR_ROW(s, y + oy) + ox, 0, (size_t) w);
    }
}

/* Called once after @wr_palette copies a fresh PLAYPAL into s->wr_playpal.
 * There is no hardware palette on this backend, so instead this rebuilds
 * the BGRX lookup table wr_host_present uses to expand palette indices
 * to real colour when it writes the scratch surface into s->fb. Uses
 * fb_pack's byte order (b | g<<8 | r<<16) to match the 32bpp DIBSection
 * layout. */
static void wr_host_apply_palette(R3DState *s) {
    int i;
    for (i = 0; i < 256; i++) {
        s->wr_rgba_lut[i] = fb_pack(s->wr_playpal[i * 3 + 0],
                                    s->wr_playpal[i * 3 + 1],
                                    s->wr_playpal[i * 3 + 2]);
    }
}

/* No backend-specific fixup needed after a @wr_map upload - wr_surf is
 * always a plain linear buffer, so there is no planar/page-flip
 * backbuffer swap to do. */
static void wr_host_on_map_uploaded(R3DState *s) {
    (void) s;
}

/* Expands the sub-rectangle [x,y,w,h) of the 8bpp scratch surface
 * through wr_rgba_lut and writes it directly into the 32bpp DIBSection
 * framebuffer (s->fb), then sets frame_dirty so @flip presents it.
 * Immediate (not deferred to @flip) so @wr_render/@wr_patch composite
 * correctly with ordinary @gfx_* calls in the same frame, exactly like
 * @sprite. */
static void wr_host_present(R3DState *s, int x, int y, int w, int h) {
    if (!s->fb || !s->wr_surf || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->wr_surf_w) w = s->wr_surf_w - x;
    if (y + h > s->wr_surf_h) h = s->wr_surf_h - y;
    if (w <= 0 || h <= 0) return;

    int fb_stride_u32 = s->fb_stride >> 2;
    u32 *fb_base = (u32 *) s->fb;
    int row;
    for (row = 0; row < h; row++) {
        const unsigned char *src = WR_ROW(s, y + row) + x;
        u32 *dst = fb_base + (size_t) (y + row) * (size_t) fb_stride_u32 + (size_t) x;
        int col;
        for (col = 0; col < w; col++) dst[col] = s->wr_rgba_lut[src[col]];
    }
    s->frame_dirty = true;
}

#include "../common/wr/wr_render.h"

/* @palette_upload: no hardware palette here - only recorded so the
 * BBC-exact fill renderer maps palette indices to MODE 2 colours the
 * way the BBC backend does. */
static void r3d_gfx_palette_upload(RelicVM *vm) {
    u32 ptr = (u32)vm->r[1];
    i32 n = vm->r[2];
    if (!ptr || n <= 0) return;
    if (n > 16) n = 16;
    if ((u64)ptr + (u64)n * 4u > vm->mem_sz) return;
    sdf_palette(vm->mem + ptr, n);
}

/* Present the sdfill MODE 2 image over the logical 320x224 view: each
 * MODE 2 pixel is 2x1 logical pixels, nearest-scaled into the u32
 * framebuffer. The HUD band below is cleared black. */
static void r3d_sdf_present(void *ctx, int true_mode) {
    R3DState *s = (R3DState*)ctx;
    int ox, oy, vw = s->logical_w, vh = s->logical_h;
    int vh224;
    if (!s->fb || vw <= 0 || vh <= 0) return;
    vh224 = vh * SDF_VROWS / 256;
    for (oy = 0; oy < vh; oy++) {
        int row = oy < vh224 ? oy * SDF_VROWS / vh224 : 0;
        for (ox = 0; ox < vw; ox++) {
            u32 c;
            if (oy >= vh224) { fb_put(s, ox, oy, fb_pack(0, 0, 0)); continue; }
            if (true_mode) {
                c = sdt_px[oy * SDF_VROWS * 2 / vh224][ox * SDT_W / vw];
            } else {
                c = l3f_bbc[sdf_px(ox * 160 / vw, row)];
            }
            fb_put(s, ox, oy, fb_pack(c & 255, (c >> 8) & 255, (c >> 16) & 255));
        }
    }
}

static void r3d_gfx_lines3d(RelicVM *vm) {
    R3DState *s = (R3DState*)vm->backend_data;
    i32 r;
    if (!s) { vm->r[1] = -1; return; }
    r = relic_l3_fill(vm, r3d_sdf_present, s);
    if (r != L3F_NOTFILL) { vm->r[1] = r; return; }
    relic_lines3d(vm, s->logical_w, s->logical_h);
}

/* @register RELIC_REG_WIN_* - script-owned window chrome controls for
 * WINFLAG_FULLCONTENT builds (see opengl-win32 backend for the shared
 * contract). */
static void r3d_win_ctl(RelicVM *vm) {
    R3DState *s = (R3DState*)vm->backend_data;
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
    .init         = r3d_init,
    .shutdown     = r3d_shutdown,
    .gfx_clear    = r3d_gfx_clear,
    .gfx_sprite   = r3d_gfx_sprite,
    .gfx_plot     = r3d_gfx_plot,
    .gfx_flip     = r3d_gfx_flip,
    .gfx_line     = r3d_gfx_line,
    .gfx_window   = r3d_gfx_window,
    .gfx_polygon  = r3d_gfx_polygon,
    .gfx_polyline = r3d_gfx_polyline,
    .gfx_camera   = r3d_gfx_camera,
    .gfx_texture  = r3d_gfx_texture,
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
    .gfx_light    = r3d_gfx_light,
    .gfx_mesh     = r3d_gfx_mesh,
    .gfx_text          = r3d_gfx_text,
    .gfx_draw_list     = r3d_gfx_draw_list,
    .gfx_lines3d       = r3d_gfx_lines3d,
    .gfx_palette_upload = r3d_gfx_palette_upload,
    .gfx_text_measure  = r3d_gfx_text_measure,
    .gfx_clip          = r3d_gfx_clip,
    .gfx_ellipse       = r3d_gfx_ellipse,
    .gfx_ellipse_filled= r3d_gfx_ellipse_filled,
    .gfx_rounded_rect  = r3d_gfx_rounded_rect,
    .gfx_rounded_rect_filled = r3d_gfx_rounded_rect_filled,
    .gfx_viewport = r3d_gfx_viewport,
    .inp_key      = r3d_inp_key,
    .inp_keyheld  = r3d_inp_keyheld,
    .win_ctl      = r3d_win_ctl,
    .inp_mouse    = r3d_inp_mouse,
    .inp_gamepad_count = r3d_inp_gamepad_count,
    .inp_gamepad       = r3d_inp_gamepad,
    .sys_rand     = r3d_sys_rand,
    .sys_millis   = r3d_sys_millis,
    .sys_console  = r3d_sys_console,
    .sys_fopen    = r3d_sys_fopen,
    .sys_fread    = r3d_sys_fread,
    .sys_fwrite   = r3d_sys_fwrite,
    .sys_fclose   = r3d_sys_fclose,
    .sys_fseek    = r3d_sys_fseek,
    .sys_flen     = r3d_sys_flen,
    .sys_platform = r3d_sys_platform,
    .sys_clip_set     = w32_clip_set,
    .sys_clip_get     = w32_clip_get,
    .sys_http_request = r3d_sys_http_request,
    .sys_http_poll    = r3d_sys_http_poll,
    .sys_http_status  = r3d_sys_http_status,
    .sys_http_read    = r3d_sys_http_read,
    .sys_http_close   = r3d_sys_http_close,
#ifdef TIGRYX_EXPERIMENTAL_SERVER
    .sys_serve        = nsnf_sys_serve,
    .sys_serve_reply  = nsnf_sys_serve_reply,
#endif
    .snd_tone_freq = r3d_snd_tone_freq,
    .snd_tone_id   = r3d_snd_tone_id,
    .snd_play      = r3d_snd_play,
    .snd_music     = r3d_snd_music,
    .snd_mixer     = r3d_snd_mixer,
};
