/*
 * syswin32.h
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
 * syswin32.h - Win32 system services shared by backends.
 *
 * These are the parts of the Win32 backend that have nothing to do with
 * graphics: @platform probing, the Win32 clipboard (CF_TEXT), and the
 * WinINet worker-thread HTTP layer (see docs/web.md).
 *
 * backend_opengl_win32 delegates its sys_* hooks here; a console-only
 * runtime backend can publish them directly through relic_syswin_backend.
 */
#ifndef RELIC_SYSWIN32_H
#define RELIC_SYSWIN32_H

#include "tigryx.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>

#define SYSWIN32_MAX_HTTP 16   /* max concurrent in-flight @http_request handles */

/* One in-flight (or completed) HTTP request. See docs/web.md.
 *
 * WinINet runs the HTTP/TLS handshake and body transfer on a worker thread
 * (blocking calls). The worker fills these fields under the module lock
 * while the VM thread polls/reads them. `generation` guards against a
 * late worker writing into a slot that was already closed and reused: the
 * worker captures the generation at request time and only writes if it
 * still matches. The WinINet handles live in the slot so http_close can
 * abort an in-flight transfer by closing them from the VM thread. */
typedef struct {
    bool     in_use;
    int      generation;    /* bumped on each alloc/close; matched by the worker */
    volatile int done;      /* 0 = pending, 1 = ready, <0 = error */
    int      status;        /* HTTP status code, valid once done == 1 */
    unsigned char *data;    /* malloc'd response body (may be NULL) */
    long     data_len;
    long     read_pos;      /* @http_read cursor */
    HINTERNET hSession;     /* root handle; closing it aborts child transfers */
    HINTERNET hConnect;
    HINTERNET hRequest;
} SysWin32HttpReq;

/* HTTP layer. `tbl` is the caller's slot array (SYSWIN32_MAX_HTTP
 * entries); NULL or out-of-range handles degrade to -1 / no-op. */
void syswin32_sys_http_request(RelicVM *vm, SysWin32HttpReq *tbl);
void syswin32_sys_http_poll   (RelicVM *vm, SysWin32HttpReq *tbl);
void syswin32_sys_http_status (RelicVM *vm, SysWin32HttpReq *tbl);
void syswin32_sys_http_read   (RelicVM *vm, SysWin32HttpReq *tbl);
void syswin32_sys_http_close  (RelicVM *vm, SysWin32HttpReq *tbl);
/* Frees data and closes handles on every live slot. Call from backend
 * shutdown. */
void syswin32_http_shutdown(SysWin32HttpReq *tbl);

/* Win32 clipboard (CF_TEXT). */
void syswin32_sys_clip_set(RelicVM *vm);
void syswin32_sys_clip_get(RelicVM *vm);

/* Fields of @platform's PlatformInfo that depend on the display backend.
 * A console program has no window, GL context or audio device, so the
 * graphics fields report their "none" values honestly. */
typedef struct {
    const char *display_mode;   /* "Graphics" (windowed) or "Text" (console) */
    const char *gfx_arch;       /* "OpenGL", or "Null" when no gfx backend */
    const char *gpu_str;        /* renderer string, or NULL -> "None" */
    const char *gl_version;     /* parsed "major.minor", or NULL -> "0.0" */
    u32         sound_flags;    /* capability bitmap, 0 when no audio */
    i32         win_w, win_h;   /* window size; 0,0 when no window */
    i32         win_x, win_y;   /* window position in screen pixels */
    const char *backend_name;   /* RELIC_BACKEND_ID of the owning backend */
} SysWin32PlatformView;

/* @platform(info): fills the 116-byte PlatformInfo struct at r1. */
void syswin32_sys_platform(RelicVM *vm, const SysWin32PlatformView *view);

/* Console-only backend object for tigryx_rt_console: sys_platform,
 * clipboard and HTTP hooks populated; everything else NULL (vm.c
 * degrades every unset hook safely). */
extern RelicBackend relic_syswin_backend;

#endif /* RELIC_SYSWIN32_H */
