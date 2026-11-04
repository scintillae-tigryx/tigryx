/*
 * pp_tmp.c
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

/* pp_tmp.c - temp file path helper for tigryxpp output.
 *
 * tigryx / tigryxri / tigryxrc all run tigryxpp as a separate process to
 * preprocess .tgx to .tp.  Writing the .tp next to the input would risk clobbering a user's
 * real .tp and leave stray files on crash, so relic_pp_tmp_path() builds
 * a path in the OS temp dir instead, tagged
 * with the PID so concurrent invocations don't collide. */
#include "tigryx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <process.h>
#elif defined(RELIC_DOS)
#  include <process.h>
#  include <unistd.h>     /* getpid() */
#  include <sys/stat.h>   /* stat() for temp dir check */
#else
#  include <unistd.h>
#endif

/* ------------------------------------------------------------------------- */
/* Temp directory resolution                                                  */
/* ------------------------------------------------------------------------- */
static int get_temp_dir(char *out, size_t out_sz) {
#if defined(_WIN32)
    DWORD n = GetTempPathA((DWORD)out_sz, out);
    return n > 0 && n < out_sz;
#elif defined(RELIC_DOS)
    /* FreeDOS/MS-DOS: TEMP and TMP env vars may point at a temp folder.
     * DOSBox typically doesn't set them. If neither is set, try \TEMP and
     * \TMP on the current drive, then fall back to the current directory
     * so the temp .tp is always writable. */
    {
        const char *t = getenv("TEMP");
        if (!t || !*t) t = getenv("TMP");
        if (t && *t) {
            snprintf(out, out_sz, "%s", t);
            return 1;
        }
        struct stat st;
        if (stat("\\TEMP", &st) == 0 && (st.st_mode & S_IFDIR)) {
            snprintf(out, out_sz, "\\TEMP");
            return 1;
        }
        if (stat("\\TMP", &st) == 0 && (st.st_mode & S_IFDIR)) {
            snprintf(out, out_sz, "\\TMP");
            return 1;
        }
        /* Last resort: current directory. */
        snprintf(out, out_sz, ".");
        return 1;
    }
#elif defined(RELIC_RISCOS)
    /* Wimp$ScrapDir is the canonical scrap area on a RISC OS desktop.
     * Fall back to @ (the CSD) so the temp file is always writable. */
    {
        const char *t = getenv("Wimp$ScrapDir");
        if (t && *t) {
            snprintf(out, out_sz, "%s", t);
            return 1;
        }
        snprintf(out, out_sz, "@");
        return 1;
    }
#else
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    snprintf(out, out_sz, "%s", t);
    return 1;
#endif
}

/* ------------------------------------------------------------------------- */
/* Basename extraction (strip directory + .tgx extension)                     */
/* ------------------------------------------------------------------------- */
static void base_name_no_ext(const char *path, char *out, size_t out_sz) {
#if defined(RELIC_RISCOS)
    /* '.' is the RISC OS directory separator. */
    const char *base = strrchr(path, '.');
#else
    const char *base = strrchr(path, '/');
#endif
#if defined(_WIN32) || defined(RELIC_DOS)
    {
        const char *bs = strrchr(path, '\\');
        if (bs && (!base || bs > base)) base = bs;
    }
#endif
    if (base) base++; else base = path;

    size_t len = strlen(base);
    /* Strip a trailing .tgx (case-insensitive) - or /tgx under RISC OS,
     * where leafnames carry the extension after a slash. */
    if (len >= 4) {
#if defined(RELIC_RISCOS)
        const char *dot = strrchr(base, '/');
#else
        const char *dot = strrchr(base, '.');
#endif
        if (dot && (dot[1] == 't' || dot[1] == 'T')
                && (dot[2] == 'g' || dot[2] == 'G')
                && (dot[3] == 'x' || dot[3] == 'X')
                && dot[4] == '\0')
            len = (size_t)(dot - base);
    }
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, base, len);
    out[len] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */
int relic_pp_tmp_path(const char *input_rx_path, char *out, size_t out_sz) {
    if (!input_rx_path || !out || out_sz < 16) return 0;

    char tmpdir[1024];
    if (!get_temp_dir(tmpdir, sizeof tmpdir)) {
        /* Fall back to the input file's own directory. */
        snprintf(tmpdir, sizeof tmpdir, "%s", input_rx_path);
        char *sep = strrchr(tmpdir, '/');
#if defined(_WIN32) || defined(RELIC_DOS)
        char *bs = strrchr(tmpdir, '\\');
        if (bs && (!sep || bs > sep)) sep = bs;
#endif
        if (sep) *(sep + 1) = '\0';
        else tmpdir[0] = '\0';
    }

    int pid =
#if defined(_WIN32)
        _getpid();
#else
        (int)getpid();
#endif

    /* Ensure exactly one separator between dir and filename. */
    size_t dlen = strlen(tmpdir);
    const char *sep = "";
#if defined(RELIC_RISCOS)
    if (dlen > 0 && tmpdir[dlen - 1] != '.')
        sep = ".";
#else
    if (dlen > 0 && tmpdir[dlen - 1] != '/' && tmpdir[dlen - 1] != '\\')
        sep = "/";
#endif

#if defined(RELIC_RISCOS)
    /* FileCore leafnames are limited to 10 chars and can't hold '.';
     * a PID-derived leaf ('R' + hex pid <= 9 chars) stays legal on
     * real FileCore media as well as HostFS.  The .tp is re-opened by
     * path, not redetected by extension. */
    int written = snprintf(out, out_sz, "%s%sR%X", tmpdir, sep, pid);
#elif defined(RELIC_DOS)
    /* DOS only supports 8.3 filenames.  Build a short name from the PID:
     *   <tmpdir>\R<pidhex>.TP
     * The hex PID fits in 7 chars for a 32-bit PID, so "R" + 7 = 8 chars,
     * plus ".TP" extension = valid 8.3.  This avoids the long
     * "tigryx-<base>-<pid>.tp" format used on other platforms. */
    int written = snprintf(out, out_sz, "%s%sR%X.TP", tmpdir, sep, pid);
#else
    char base[256];
    base_name_no_ext(input_rx_path, base, sizeof base);
    if (base[0] == '\0') snprintf(base, sizeof base, "input");

    /* Build: <tmpdir>[sep]tigryx-<base>-<pid>.tp */
    int written = snprintf(out, out_sz, "%s%stigryx-%s-%d.tp",
                           tmpdir, sep, base, pid);
#endif
    if (written < 0 || (size_t)written >= out_sz) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}
