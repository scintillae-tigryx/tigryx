/*
 * tigryx_win_softcursor.h
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
 * @file tigryx_win_softcursor.h
 * @brief Shared arrow artwork and host detection for the Windows 9x software
 *        mouse pointer, used by both win32 backends.
 *
 * On Windows 95/98/ME the mouse pointer is a *software* sprite owned by the
 * display driver: every drawing operation that reaches the screen and overlaps
 * the pointer makes the driver erase the pointer, draw, then paint it back.
 * A Tigryx script presents its whole client area on every @flip
 * (SetDIBitsToDevice/StretchDIBits on the relic3d backend, SwapBuffers through
 * the generic software rasteriser on the opengl one), so the pointer is
 * removed and restored tens of times a second and flickers violently over the
 * window - badly enough to be unusable in GUI apps and games alike. Pacing the
 * presentation rate (see the vsync_throttle paths) lowers the frequency but
 * cannot remove the exclusion.
 *
 * The fix is the one every Win9x-era blitter used: hide the system pointer
 * while it is over the client area (WM_SETCURSOR -> SetCursor(NULL)) and draw
 * the pointer ourselves as part of the frame. Once the pointer *is* the frame
 * there is nothing for the driver to exclude, so the flicker disappears
 * entirely and the pointer stays in sync with what the script drew.
 *
 * This is only done on 9x hosts. NT-family Windows draws the pointer with a
 * hardware overlay that is unaffected by our blits, so those builds keep the
 * real system cursor (which also keeps per-control cursor shapes, cursor
 * schemes and accessibility settings working). TIGRYX_SOFT_CURSOR=1/0 in the
 * environment forces the software pointer on or off, which is the only way to
 * exercise this path on a modern machine.
 */
#ifndef TIGRYX_WIN_SOFTCURSOR_H
#define TIGRYX_WIN_SOFTCURSOR_H

#include <windows.h>
#include <stdlib.h>
#include <string.h>

/* Classic arrow, hotspot at (0,0): 'X' = black outline, '.' = white fill,
 * ' ' = transparent. Deliberately the plain 9x-era arrow rather than a
 * shadowed modern one, since it has to composite into 16-bit and palettised
 * framebuffers with no blending. */
#define RELIC_SOFTCUR_W 10
#define RELIC_SOFTCUR_H 17

static const char *const relic_softcur_arrow[RELIC_SOFTCUR_H] = {
    "X         ",
    "XX        ",
    "X.X       ",
    "X..X      ",
    "X...X     ",
    "X....X    ",
    "X.....X   ",
    "X......X  ",
    "X.......X ",
    "X........X",
    "X.....XXXX",
    "X..X..X   ",
    "X.X X..X  ",
    "XX  X..X  ",
    "X    X..X ",
    "     X..X ",
    "      XX  "
};

/**
 * @brief True when this host needs the backend to draw its own mouse pointer.
 *
 * Cached: the answer cannot change during a run.
 */
static int relic_win_softcursor_enabled(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    const char *env = getenv("TIGRYX_SOFT_CURSOR");
    if (env && env[0]) {
        cached = (env[0] != '0');
        return cached;
    }
    OSVERSIONINFOA ovi;
    memset(&ovi, 0, sizeof(ovi));
    ovi.dwOSVersionInfoSize = sizeof(ovi);
    cached = (GetVersionExA(&ovi) &&
              ovi.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS) ? 1 : 0;
    return cached;
}

/**
 * @brief True when the pointer is over `hwnd`'s client area and not covered by
 *        another window, i.e. when the software pointer should be drawn.
 *
 * Asked at present time rather than tracked from WM_MOUSEMOVE: the pointer can
 * leave the window, or another window can be raised over it, without our
 * window receiving anything (Win95 has no WM_MOUSELEAVE), which would strand a
 * drawn pointer at the edge of the frame.
 */
static int relic_win_softcursor_visible(HWND hwnd) {
    POINT pt;
    RECT  rc;
    if (!hwnd || !GetCursorPos(&pt)) return 0;
    if (WindowFromPoint(pt) != hwnd) return 0;
    if (!ScreenToClient(hwnd, &pt) || !GetClientRect(hwnd, &rc)) return 0;
    return pt.x >= 0 && pt.y >= 0 && pt.x < rc.right && pt.y < rc.bottom;
}

#endif /* TIGRYX_WIN_SOFTCURSOR_H */
