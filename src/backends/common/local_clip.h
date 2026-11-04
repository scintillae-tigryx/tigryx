/*
 * local_clip.h
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

/* local_clip.h - process-local clipboard for platforms with no OS-level
 * clipboard (DOS). Implements the RelicBackend sys_clip_set/sys_clip_get
 * hooks with the same call contract as the Win32/X11/macOS versions:
 * text is stored in a static buffer owned by the backend, so
 * @clipboard_set/@clipboard_get round-trip within the running app.
 * There is no external clipboard on DOS - the data is per-process and
 * does not persist or transfer between programs.
 *
 * This is a HEADER, not a .c file, and is meant to be #included into
 * each backend .c file that wants it - the same pattern as
 * common/wr/wr_render.h. All functions stay `static` in the backend's
 * own translation unit, and no build-system changes are needed.
 *
 * Requires: tigryx.h (RelicVM) included first.
 */

#define LOCAL_CLIP_CAP 65536 /* 64KB - ample for text snippets */

static u8  local_clip_buf[LOCAL_CLIP_CAP];
static i32 local_clip_len = 0;

static void local_clip_set(RelicVM *vm) {
    u32 buf_ptr = (u32)vm->r[1];
    i32 len     = (i32)vm->r[2];
    if (len <= 0 || buf_ptr + (u32)len > vm->mem_sz) { vm->r[1] = 0; return; }
    if (len > LOCAL_CLIP_CAP) len = LOCAL_CLIP_CAP;
    memcpy(local_clip_buf, vm->mem + buf_ptr, (size_t)len);
    local_clip_len = len;
    vm->r[1] = 1;
}

static void local_clip_get(RelicVM *vm) {
    u32 buf_ptr = (u32)vm->r[1];
    i32 cap     = (i32)vm->r[2];
    i32 n;
    if (cap <= 0 || buf_ptr + (u32)cap > vm->mem_sz) { vm->r[1] = -1; return; }
    if (local_clip_len == 0) { vm->mem[buf_ptr] = 0; vm->r[1] = 0; return; }
    n = local_clip_len < cap - 1 ? local_clip_len : cap - 1;
    memcpy(vm->mem + buf_ptr, local_clip_buf, (size_t)n);
    vm->mem[buf_ptr + n] = 0;
    vm->r[1] = n;
}
