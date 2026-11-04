/*
 * fbdump.h
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
 * fbdump.h - headless frame capture for the GL backends.
 *
 * Writes the presented frame to a PNG when $TGXDUMP is set, so a build
 * machine (or an agent with no screen-recording permission) can see what
 * a program actually drew. The videl-tos backend has had this since the
 * Falcon port ($TIGVDUMP + tools/atari/raw2png.py); this is the same idea
 * for the GL backends, emitting PNG directly so no converter is needed.
 *
 * Usage from a backend's flip hook, AFTER the frame is rendered but
 * BEFORE the buffer swap (glReadPixels must see the back buffer):
 *
 *     relic_fbdump_maybe(0, 0, s->pixel_w, s->pixel_h, &s->fbdump_n);
 *
 * Environment:
 *   TGXDUMP=<path>     write frames to <path>. A '%d' in the path is
 *                      replaced by the frame counter, otherwise the same
 *                      file is overwritten each frame (so the file always
 *                      holds the most recent frame - what you usually
 *                      want when capturing a steady-state screen).
 *   TGXDUMP_FRAME=N    only dump frame N (0-based). Default: every frame,
 *                      which with a non-'%d' path means "last frame
 *                      wins". Dumping every frame of a real game to
 *                      separate files fills a disk quickly - pair '%d'
 *                      with TGXDUMP_EVERY.
 *   TGXDUMP_EVERY=N    dump every Nth frame (default 1).
 *
 * The PNG is written with stored (uncompressed) deflate blocks, so this
 * header needs no zlib/miniz and no stb_image_write: ~3 bytes of overhead
 * per 64 KB plus the 5-byte block headers. Files are large but they are
 * debug artifacts, and correctness beats size here. miniz is deliberately
 * not used: the runtime archives build it with MINIZ_NO_DEFLATE_APIS, so
 * compressing here would break those targets.
 */
#ifndef RELIC_FBDUMP_H
#define RELIC_FBDUMP_H


/* PNG writer shared with the non-GL backends (metal-common). */
#include "fbpng.h"


/* Read back a rect of the current GL back buffer and dump it if $TGXDUMP
 * asks. (x,y,w,h) are in backing pixels, bottom-left origin - pass the
 * rect that holds the presented image. `frame` is the backend's own
 * monotonically increasing frame counter, incremented here. Requires an
 * active GL context. */
static void relic_fbdump_maybe(int x, int y, int w, int h, unsigned long *frame) {
    const char *path = getenv("TGXDUMP");
    unsigned long n = (*frame)++;
    if (!path || !path[0] || w <= 0 || h <= 0) return;

    const char *only = getenv("TGXDUMP_FRAME");
    if (only && only[0]) {
        if (n != strtoul(only, NULL, 10)) return;
    } else {
        const char *every = getenv("TGXDUMP_EVERY");
        unsigned long e = (every && every[0]) ? strtoul(every, NULL, 10) : 1;
        if (e > 1 && (n % e) != 0) return;
    }

    uint8_t *buf = (uint8_t*)malloc((size_t)w * (size_t)h * 3);
    if (!buf) return;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, w, h, GL_RGB, GL_UNSIGNED_BYTE, buf);

    /* GL returns bottom-row-first; PNG wants top-row-first. */
    uint8_t *flip = (uint8_t*)malloc((size_t)w * (size_t)h * 3);
    if (!flip) { free(buf); return; }
    size_t rb = (size_t)w * 3;
    for (int y = 0; y < h; y++)
        memcpy(flip + rb * (size_t)y, buf + rb * (size_t)(h - 1 - y), rb);

    char out[1024];
    const char *pct = strstr(path, "%d");
    if (pct) snprintf(out, sizeof out, path, n);
    else     snprintf(out, sizeof out, "%s", path);

    if (relic_fbdump_write_png(out, flip, w, h))
        fprintf(stderr, "tgxdump: frame %lu -> %s (%dx%d)\n", n, out, w, h);
    else
        fprintf(stderr, "tgxdump: failed to write %s\n", out);

    free(flip);
    free(buf);
}

#endif /* RELIC_FBDUMP_H */
