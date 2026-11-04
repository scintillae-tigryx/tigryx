/*
 * tigryx_pragma.h
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

/* tigryx_pragma.h — source-level tool arguments via `#pragma arg`.
 *
 * A program can pin the tool settings it needs to run correctly, instead of
 * relying on whoever launches it to remember the right command line:
 *
 *     #pragma arg heap 2M          // VM memory size
 *     #pragma arg heap 0           // smallest heap this program can run in
 *     #pragma arg no_console       // boolean flag, no value
 *     #pragma arg colour_depth 8   // DOS backend screen depth
 *     #pragma arg assets "data/x.dat"  // extra path/glob for `bc --pack-auto`
 *
 * tigryxpp validates the directive and copies it verbatim into the generated
 * .tp, where the core lexer skips it as a comment (see lexer.c: any `#...`
 * that is not `#line` is treated as a line comment). Each consuming tool
 * (tigryxri, tigryxrc) rescans its input for these lines before it acts on
 * its own argv.
 *
 * Precedence: an explicit command-line flag always overrides the pragma, so
 * a one-off run can still be tuned without editing the source.
 *
 * Names are deliberately identifier-safe (`no_console`, not `no-console`)
 * because the preprocessor tokenises the directive; the underscore form is
 * mapped to the real flag name by the consuming tool.
 *
 * `assets` is repeatable (one line per path/glob) and is the escape hatch
 * for `tigryx bc --pack-auto` (see pack_auto in tigryx_main.c): that scanner
 * only sees asset paths that appear as string literals in @file_open /
 * @file_load_png / @text calls, so anything built at runtime (snprintf,
 * table lookups, loops) needs an explicit `#pragma arg assets` line to be
 * picked up. Quote the value so a glob containing a slash-star
 * sequence can't be mistaken for a block comment by the lexer.
 */
#ifndef TIGRYX_PRAGMA_H
#define TIGRYX_PRAGMA_H

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define RELIC_PRAGMA_MAX_VALUE  128
#define RELIC_PRAGMA_MAX_ASSETS 32

/* Argument kinds understood by `#pragma arg`. Anything not listed here is
 * rejected by the preprocessor, so a source file cannot inject arbitrary
 * flags (output paths, tool selection) into a build. */
typedef enum {
    RELIC_PRAGMA_NONE = 0,
    RELIC_PRAGMA_HEAP,          /* value: size string, "0" = minimum */
    RELIC_PRAGMA_NO_CONSOLE,    /* no value                          */
    RELIC_PRAGMA_COLOUR_DEPTH,  /* value: 8/15/16/24/32              */
    RELIC_PRAGMA_ASSETS,        /* value: quoted path or glob, repeatable */
    RELIC_PRAGMA_PACK_FONTS,    /* no value: tigryxrc packs all .ttf in fonts/ */
    RELIC_PRAGMA_TIER           /* value: low|medium|high|super (Adaptive
                                 * Build; tigryxpp also applies it to the
                                 * TGX_TIER_* defines and emits the resolved
                                 * tier back into the .tp) */
} RelicPragmaKind;

typedef struct {
    const char     *name;       /* as written in source */
    RelicPragmaKind kind;
    int             takes_value;
} RelicPragmaSpec;

static const RelicPragmaSpec RELIC_PRAGMA_SPECS[] = {
    { "heap",         RELIC_PRAGMA_HEAP,         1 },
    { "no_console",   RELIC_PRAGMA_NO_CONSOLE,   0 },
    { "colour_depth", RELIC_PRAGMA_COLOUR_DEPTH, 1 },
    { "color_depth",  RELIC_PRAGMA_COLOUR_DEPTH, 1 },
    { "assets",       RELIC_PRAGMA_ASSETS,       1 },
    { "pack_fonts",   RELIC_PRAGMA_PACK_FONTS,   0 },
    { "tier",         RELIC_PRAGMA_TIER,         1 },
};
#define RELIC_PRAGMA_NSPECS \
    ((int)(sizeof(RELIC_PRAGMA_SPECS) / sizeof(RELIC_PRAGMA_SPECS[0])))

static inline const RelicPragmaSpec *relic_pragma_lookup(const char *name) {
    for (int i = 0; i < RELIC_PRAGMA_NSPECS; i++)
        if (strcmp(RELIC_PRAGMA_SPECS[i].name, name) == 0)
            return &RELIC_PRAGMA_SPECS[i];
    return NULL;
}

/* Collected results of scanning a file. */
typedef struct {
    int  has_heap;
    char heap[RELIC_PRAGMA_MAX_VALUE];
    int  has_no_console;
    int  has_colour_depth;
    int  colour_depth;
    int  nassets;
    char assets[RELIC_PRAGMA_MAX_ASSETS][RELIC_PRAGMA_MAX_VALUE];
    int  has_pack_fonts;
    int  has_tier;
    int  tier;              /* 0..3 (low..super), -1 if the name was bad */
} RelicPragmaArgs;

/* Parse one already-stripped directive body, i.e. the text following
 * "#pragma arg". Returns 1 if it was understood, 0 otherwise; on failure
 * *err_name receives the offending token for diagnostics. */
static inline int relic_pragma_apply(const char *body, RelicPragmaArgs *out,
                                     char *err_name, int err_cap) {
    while (*body == ' ' || *body == '\t') body++;

    char name[RELIC_PRAGMA_MAX_VALUE];
    int n = 0;
    while (*body && !isspace((unsigned char)*body) && n < (int)sizeof(name) - 1)
        name[n++] = *body++;
    name[n] = '\0';
    if (n == 0) return 0;

    while (*body == ' ' || *body == '\t') body++;

    char value[RELIC_PRAGMA_MAX_VALUE];
    int v = 0;
    while (*body && *body != '\n' && *body != '\r' &&
           v < (int)sizeof(value) - 1)
        value[v++] = *body++;
    while (v > 0 && isspace((unsigned char)value[v - 1])) v--;
    value[v] = '\0';

    const RelicPragmaSpec *spec = relic_pragma_lookup(name);
    if (!spec) {
        if (err_name && err_cap > 0) snprintf(err_name, (size_t)err_cap, "%s", name);
        return 0;
    }
    if (spec->takes_value && v == 0) {
        if (err_name && err_cap > 0) snprintf(err_name, (size_t)err_cap, "%s", name);
        return 0;
    }

    switch (spec->kind) {
        case RELIC_PRAGMA_HEAP:
            out->has_heap = 1;
            snprintf(out->heap, sizeof(out->heap), "%s", value);
            break;
        case RELIC_PRAGMA_NO_CONSOLE:
            out->has_no_console = 1;
            break;
        case RELIC_PRAGMA_COLOUR_DEPTH:
            out->has_colour_depth = 1;
            out->colour_depth = atoi(value);
            break;
        case RELIC_PRAGMA_ASSETS: {
            /* The raw-text scanners below hand us the value verbatim,
             * quotes and all (unlike tigryxpp.c's tokeniser, which strips
             * them); the tokenised path never reaches this function since
             * tigryxpp only records the pragma text, it doesn't call
             * relic_pragma_apply. Strip a matching pair here so both
             * paths end up with the same bare value. */
            const char *v = value;
            int vlen = (int)strlen(v);
            if (vlen >= 2 && v[0] == '"' && v[vlen - 1] == '"') {
                vlen -= 2;
                if (vlen >= (int)sizeof(out->assets[0])) vlen = (int)sizeof(out->assets[0]) - 1;
                if (out->nassets < RELIC_PRAGMA_MAX_ASSETS) {
                    memcpy(out->assets[out->nassets], v + 1, (size_t)vlen);
                    out->assets[out->nassets][vlen] = '\0';
                    out->nassets++;
                }
            } else if (out->nassets < RELIC_PRAGMA_MAX_ASSETS) {
                snprintf(out->assets[out->nassets], sizeof(out->assets[0]), "%s", v);
                out->nassets++;
            }
            break;
        }
        case RELIC_PRAGMA_PACK_FONTS:
            out->has_pack_fonts = 1;
            break;
        case RELIC_PRAGMA_TIER: {
            /* Ordinal only; validation of the *name* happens in tigryxpp
             * (which reports a proper error) — a bad value leaves
             * has_tier set but tier == -1, which consumers ignore. */
            static const char *const tn[4] = { "low", "medium", "high", "super" };
            int t = -1;
            for (int i = 0; i < 4; i++) {
                if (strcmp(value, tn[i]) == 0) { t = i; break; }
            }
            out->has_tier = 1;
            out->tier = t;
            break;
        }
        default:
            return 0;
    }
    return 1;
}

/* Scan an in-memory source buffer for `#pragma arg ...` lines. Unknown
 * or malformed directives are ignored here — tigryxpp is where they are
 * diagnosed — so that a tool handed a hand-written .tp never hard-fails
 * on this. Call this on the same buffer you already read for compilation,
 * to avoid a second disk read. */
static inline void relic_pragma_scan_buf(const char *src, int src_len,
                                         RelicPragmaArgs *out) {
    memset(out, 0, sizeof(*out));
    if (!src || src_len <= 0) return;

    const char *p   = src;
    const char *end = src + src_len;
    while (p < end) {
        /* Find the start of a line. */
        const char *lstart = p;
        while (p < end && *p != '\n') p++;
        const char *lend = p;
        if (p < end) p++;  /* consume '\n' */

        const char *q = lstart;
        while (q < lend && (*q == ' ' || *q == '\t')) q++;
        if (q >= lend || *q != '#') continue;
        q++;
        while (q < lend && (*q == ' ' || *q == '\t')) q++;
        if (lend - q < 6 || strncmp(q, "pragma", 6) != 0) continue;
        q += 6;
        if (q >= lend || !isspace((unsigned char)*q)) continue;
        while (q < lend && (*q == ' ' || *q == '\t')) q++;
        if (lend - q < 3 || strncmp(q, "arg", 3) != 0) continue;
        q += 3;
        if (q >= lend || !isspace((unsigned char)*q)) continue;

        /* Copy the remainder into a NUL-terminated buffer for apply. */
        char body[256];
        int n = (int)(lend - q);
        if (n > (int)sizeof(body) - 1) n = (int)sizeof(body) - 1;
        memcpy(body, q, (size_t)n);
        body[n] = '\0';
        relic_pragma_apply(body, out, NULL, 0);
    }
}

/* Convenience: read a file and scan it. Prefer relic_pragma_scan_buf()
 * on a buffer you have already read, to avoid a redundant disk read. */
static inline void relic_pragma_scan(const char *path, RelicPragmaArgs *out) {
    memset(out, 0, sizeof(*out));
    if (!path) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '#') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "pragma", 6) != 0) continue;
        p += 6;
        if (!isspace((unsigned char)*p)) continue;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "arg", 3) != 0) continue;
        p += 3;
        if (!isspace((unsigned char)*p)) continue;
        relic_pragma_apply(p, out, NULL, 0);
    }
    fclose(f);
}

#endif /* TIGRYX_PRAGMA_H */
