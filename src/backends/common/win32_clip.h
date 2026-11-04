/*
 * win32_clip.h
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

/* win32_clip.h - Win32 system clipboard (CF_TEXT) implementing the
 * RelicBackend sys_clip_set/sys_clip_get hooks. OpenClipboard(NULL) does
 * not associate the clipboard with any window, so this works with or
 * without a graphics backend.
 *
 * This is a HEADER, not a .c file, and is meant to be #included into
 * each backend .c file that wants it - the same pattern as
 * common/local_clip.h. All functions stay `static` in the backend's
 * own translation unit, and no build-system changes are needed.
 *
 * Requires: <windows.h> and tigryx.h (RelicVM) included first.
 */

/* OpenClipboard can fail transiently when a clipboard viewer (e.g. the
 * Windows 10/11 clipboard history service) is briefly holding the
 * clipboard open after a previous CloseClipboard. Retry a few times
 * with a short yield so rapid set/get cycles don't fail spuriously. */
static int w32_clip_open_with_retry(void) {
    for (int i = 0; i < 20; i++) {
        if (OpenClipboard(NULL)) return 1;
        Sleep(1);
    }
    return 0;
}

static void w32_clip_set(RelicVM *vm) {
    u32 buf_ptr = (u32)vm->r[1];
    i32 len     = (i32)vm->r[2];
    if (len <= 0 || buf_ptr + (u32)len > vm->mem_sz) { vm->r[1] = 0; return; }

    if (!w32_clip_open_with_retry()) { vm->r[1] = 0; return; }
    EmptyClipboard();

    /* CF_TEXT requires a GlobalAlloc'd GMEM_MOVEABLE buffer with an extra
     * byte for the NUL terminator. */
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(len + 1));
    if (!hg) { CloseClipboard(); vm->r[1] = 0; return; }

    char *p = (char *)GlobalLock(hg);
    if (!p) { GlobalFree(hg); CloseClipboard(); vm->r[1] = 0; return; }
    memcpy(p, vm->mem + buf_ptr, (size_t)len);
    p[len] = 0;
    GlobalUnlock(hg);

    HANDLE h = SetClipboardData(CF_TEXT, hg);
    CloseClipboard();
    /* SetClipboardData takes ownership on success; free only on failure. */
    if (!h) GlobalFree(hg);
    vm->r[1] = h ? 1 : 0;
}

static void w32_clip_get(RelicVM *vm) {
    u32 buf_ptr = (u32)vm->r[1];
    i32 cap     = (i32)vm->r[2];
    if (cap <= 0 || buf_ptr + (u32)cap > vm->mem_sz) { vm->r[1] = -1; return; }

    if (!w32_clip_open_with_retry()) { vm->r[1] = -1; return; }

    HANDLE h = GetClipboardData(CF_TEXT);
    if (!h) { CloseClipboard(); vm->r[1] = -1; return; }

    const char *src = (const char *)GlobalLock(h);
    if (!src) { CloseClipboard(); vm->r[1] = -1; return; }

    /* Copy up to cap-1 bytes, stopping at the source NUL. */
    int n = 0;
    while (n < cap - 1 && src[n] != 0) {
        vm->mem[buf_ptr + n] = (u8)src[n];
        n++;
    }
    vm->mem[buf_ptr + n] = 0;

    GlobalUnlock(h);
    CloseClipboard();
    vm->r[1] = n;
}
