/*
 * tigryx_pack_auto.h
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
 * tigryx_pack_auto.h - --pack-auto asset scanner, shared between the
 * `tigryx` dispatcher (tigryx_main.c's `bc --pack-auto`) and `tigryxcc`
 * (tigryxce.c's own --pack-auto/--assets). Header-only (static inline),
 * matching tigryx_pragma.h's convention, so both translation units get
 * their own copy with no new link-time dependency.
 *
 * Needs: <stdio.h> <stdlib.h> <string.h> <ctype.h> and tigryx.h (for
 * RelicAssets / relic_assets_add_file / relic_assets_add_glob) plus
 * tigryx_pragma.h (for RelicPragmaArgs / relic_pragma_scan_buf), all
 * already included by both callers before this header.
 */
#ifndef TIGRYX_PACK_AUTO_H
#define TIGRYX_PACK_AUTO_H

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

/* ---- pack-auto: best-effort scan for assets a program provably needs ----
 *
 * Finds, by scanning the flattened (#import-resolved) source text:
 *   - @file_open("literal", mode) / @file_load_png("literal", ...) calls,
 *     where the path is a string literal and the mode (when present)
 *     isn't a write mode. Resolved relative to the entry script's own
 *     directory, matching this project's existing convention of loading
 *     assets relative to cwd == script dir.
 *   - for every .rlm found that way, the textures named in its v2/v3
 *     texture table (see modules/rlm_loader.tgx for the on-disk format),
 *     resolved relative to the .rlm's own directory - mirroring
 *     rlm_loader.tgx's own rlm_dirname() rule.
 *   - font faces the program can name: the font-name literal at each
 *     @text/@text_measure call site, plus every string literal of >= 4
 *     chars that is a substring of a bundled face name (this catches
 *     names carried in variables like ui.font/theme.font). Faces are
 *     resolved against the same places the backends look for fonts at
 *     runtime: the tigryx executable's own directory (where bundled
 *     fonts ship, see the .ttf files under bin/<platform>), then the
 *     project's assets/ and fonts/ subdirectories. `--pack-fonts` /
 *     `#pragma arg pack_fonts` skips this selection and packs every
 *     found face - the escape hatch for programs that select faces at
 *     runtime (e.g. ui.font) where no literal can resolve them.
 *   - #pragma arg assets "<path-or-glob>" lines (see tigryx_pragma.h) -
 *     the escape hatch for anything built at runtime that this scan
 *     can't see, e.g. snprintf'd paths or table-driven filenames.
 *
 * This is static text scanning, not a real parse: paths built at runtime
 * are invisible to it by construction. That is why it's opt-in
 * (--pack-auto) and additive to explicit --assets globs, not a
 * replacement for either.
 */
#ifdef __ia16__
/* Real-mode DOS: PACK_AUTO_PATH must fit DOS paths (<=80) and the
 * files[] + rlm_paths[] arrays must stay under gcc-ia16's ~32K object
 * limit.  On-DOS packing is marginal anyway - .tlx assets are normally
 * packed host-side. */
#define PACK_AUTO_MAX      32
#define PACK_AUTO_PATH     96
#define PACK_AUTO_MAX_ARGS 8
#else
#define PACK_AUTO_MAX      512
#define PACK_AUTO_PATH     1024
#define PACK_AUTO_MAX_ARGS 8
#endif

typedef struct {
    char host[PACK_AUTO_PATH];  /* file to actually read off disk */
    char rel[PACK_AUTO_PATH];   /* name to pack it under / dedupe key */
} PackAutoFile;

static inline int pack_auto_find(PackAutoFile *list, int n, const char *rel) {
    for(int i = 0; i < n; i++) if(strcmp(list[i].rel, rel) == 0) return i;
    return -1;
}

/* Adds (host, rel) if rel isn't already present and there's room. Returns
 * 1 if a new entry was added, 0 otherwise (duplicate or full). */
static inline int pack_auto_add(PackAutoFile *list, int *n, int max, const char *host, const char *rel) {
    if(pack_auto_find(list, *n, rel) >= 0) return 0;
    if(*n >= max) return 0;
    snprintf(list[*n].host, PACK_AUTO_PATH, "%s", host);
    snprintf(list[*n].rel,  PACK_AUTO_PATH, "%s", rel);
    (*n)++;
    return 1;
}

static inline int pack_auto_isident(int c) { return isalnum((unsigned char)c) || c == '_'; }

/* Case-insensitive "is needle a substring of hay" (both plain ASCII). */
static inline int pack_auto_ci_contains(const char *hay, const char *needle) {
    size_t hn = strlen(hay), nn = strlen(needle);
    if(nn == 0) return 1;
    if(nn > hn) return 0;
    for(size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        for(; j < nn; j++) {
            char a = hay[i+j], b = needle[j];
            if(a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if(b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if(a != b) break;
        }
        if(j == nn) return 1;
    }
    return 0;
}

/* Splits the argument list of a call whose '(' sits at src[open_paren]
 * into top-level (depth-0) comma-separated spans, skipping the contents
 * of "..." string literals so a comma/paren inside one doesn't confuse
 * the split. Returns the argument count (capped at max_args) and, via
 * *after, the source index just past the matching ')'. */
static inline int pack_auto_split_args(const char *src, long open_paren, long len,
                                const char *arg_ptr[], int arg_len[], int max_args,
                                long *after) {
    int nargs = 0;
    long i = open_paren + 1;
    long start = i;
    int depth = 0;
    while(i < len) {
        char c = src[i];
        if(c == '"') {
            i++;
            while(i < len && src[i] != '"') { if(src[i] == '\\' && i + 1 < len) i++; i++; }
            if(i < len) i++;
            continue;
        }
        if(c == '(') { depth++; i++; continue; }
        if(c == ')') {
            if(depth == 0) {
                if(nargs < max_args) { arg_ptr[nargs] = src + start; arg_len[nargs] = (int)(i - start); nargs++; }
                *after = i + 1;
                return nargs;
            }
            depth--; i++; continue;
        }
        if(c == ',' && depth == 0) {
            if(nargs < max_args) { arg_ptr[nargs] = src + start; arg_len[nargs] = (int)(i - start); nargs++; }
            start = i + 1; i++; continue;
        }
        i++;
    }
    *after = len;
    return nargs;
}

/* If the (whitespace-trimmed) argument span is a quoted string literal,
 * copies its contents (handling \\ and \") into out and returns 1. */
static inline int pack_auto_arg_str(const char *arg, int arg_len, char *out, int out_sz) {
    int i = 0;
    while(i < arg_len && isspace((unsigned char)arg[i])) i++;
    if(i >= arg_len || arg[i] != '"') return 0;
    i++;
    int o = 0;
    while(i < arg_len && arg[i] != '"') {
        char c = arg[i];
        if(c == '\\' && i + 1 < arg_len) { i++; c = arg[i]; }
        if(o < out_sz - 1) out[o++] = c;
        i++;
    }
    out[o < out_sz ? o : out_sz - 1] = '\0';
    return 1;
}

/* Returns a malloc'd copy of `src` with the contents of every line and
 * block comment blanked out to spaces (same length, so all offsets
 * into the original stay valid). String literals are respected - a
 * comment-opening sequence inside one doesn't start a comment - so this
 * has to track quote state exactly like the split/arg scanners above.
 * Both the call scanner and the generic literal-path scanner below run
 * over this masked copy, so neither one can be fooled by a commented-out
 * call or a documentation snippet like `// #import "foo.tgx"`. */
static inline char *pack_auto_mask_comments(const char *src, long len) {
    char *out = (char*)malloc((size_t)len + 1);
    if(!out) return NULL;
    memcpy(out, src, (size_t)len);
    out[len] = '\0';
    long i = 0;
    while(i < len) {
        if(src[i] == '"') {
            i++;
            while(i < len && src[i] != '"') { if(src[i] == '\\' && i + 1 < len) i++; i++; }
            if(i < len) i++;
            continue;
        }
        if(i + 1 < len && src[i] == '/' && src[i+1] == '/') {
            while(i < len && src[i] != '\n') { out[i] = ' '; i++; }
            continue;
        }
        if(i + 1 < len && src[i] == '/' && src[i+1] == '*') {
            out[i] = ' '; out[i+1] = ' '; i += 2;
            while(i + 1 < len && !(src[i] == '*' && src[i+1] == '/')) { out[i] = ' '; i++; }
            if(i + 1 < len) { out[i] = ' '; out[i+1] = ' '; i += 2; }
            continue;
        }
        i++;
    }
    return out;
}

/* tigryxpp's own `#line N "file"` attribution lines are metadata, not
 * program text. The literal-path scan would otherwise read each quoted
 * path as a candidate asset: the paths are relative to the file that
 * imported them, so they only resolve when the entry script sits in the
 * build cwd - packing the program's own .tgx sources into the .tlx
 * (where their basenames can then shadow files the program opens).
 * Blank every such directive line in the masked copy. */
static inline void pack_auto_mask_line_directives(char *src, long len) {
    long i = 0;
    while(i < len) {
        long ls = i;
        while(i < len && src[i] != '\n') i++;
        long p = ls;
        while(p < i && (src[p] == ' ' || src[p] == '\t')) p++;
        if(p + 5 <= i && src[p] == '#' &&
           memcmp(src + p + 1, "line", 4) == 0 &&
           (p + 5 == i || src[p + 5] == ' ' || src[p + 5] == '\t')) {
            for(long k = ls; k < i; k++) src[k] = ' ';
        }
        if(i < len) i++;
    }
}

/* The generic asset-detection pass: every "..." string literal in the
 * (comment-masked) source is a candidate path, tried relative to
 * base_dir (or as-is if it looks absolute); it's only kept if it
 * resolves to a real file. This is what makes wrapper functions "just
 * work" without knowing their names - e.g. feet3d.tgx's own
 * rlm_load(&pitch_model, "assets/pitch.rlm") is a call to a project
 * helper, not to @file_open directly (rlm_loader.tgx's rlm_load() takes
 * the path as a *parameter* and only @file_open()s a variable) - but the
 * literal at the call site is exactly the string this pass is looking
 * for. The existence check is what keeps this from matching arbitrary
 * quoted text (UI strings, format strings, error messages): those
 * essentially never happen to name a real file relative to the project. */
static inline void pack_auto_scan_literal_paths(const char *src, long len, const char *base_dir,
                                         PackAutoFile *files, int *nfiles, int max,
                                         char rlm_paths[][PACK_AUTO_PATH], int *nrlm, int rlm_cap) {
    long i = 0;
    while(i < len) {
        if(src[i] != '"') { i++; continue; }
        long start = i + 1;
        long j = start;
        while(j < len && src[j] != '"') { if(src[j] == '\\' && j + 1 < len) j++; j++; }

        char rel[PACK_AUTO_PATH];
        int o = 0;
        for(long k = start; k < j && o < (int)sizeof(rel) - 1; k++) {
            char c = src[k];
            if(c == '\\' && k + 1 < j) { k++; c = src[k]; }
            rel[o++] = c;
        }
        rel[o] = '\0';
        i = (j < len) ? j + 1 : j;

        if(o < 2 || strcmp(rel, "..") == 0 || strcmp(rel, ".") == 0) continue;

        int is_abs = (rel[0] == '/' || rel[0] == '\\' || (o >= 2 && rel[1] == ':'));
        char host[PACK_AUTO_PATH];
        if(is_abs) snprintf(host, sizeof host, "%s", rel);
        else       snprintf(host, sizeof host, "%.511s/%.511s", base_dir, rel);

        FILE *f = fopen(host, "rb");
        if(!f) continue;
        fclose(f);
        if(pack_auto_add(files, nfiles, max, host, rel)) {
            size_t rlen = strlen(rel);
            if(rlen >= 4 && strcmp(rel + rlen - 4, ".rlm") == 0 && *nrlm < rlm_cap)
                snprintf(rlm_paths[(*nrlm)++], PACK_AUTO_PATH, "%s", rel);
        }
    }
}

#define PACK_AUTO_MAX_FONT_NAMES 64
typedef struct {
    char names[PACK_AUTO_MAX_FONT_NAMES][64];
    int  n;
} PackAutoFonts;

static inline void pack_auto_add_font_name(PackAutoFonts *f, const char *name) {
    if(name[0] == '\0') return;  /* empty = backend default face; not resolved */
    for(int i = 0; i < f->n; i++) if(strcmp(f->names[i], name) == 0) return;
    if(f->n < PACK_AUTO_MAX_FONT_NAMES) { snprintf(f->names[f->n], sizeof f->names[0], "%s", name); f->n++; }
}

typedef struct { PackAutoFonts *fonts; int name_arg_idx; int *used; } PackAutoTextCtx;

/* Callback for @text (font name is arg 5) / @text_measure (arg 3). */
static inline void pack_auto_on_text_call(const char *arg_ptr[], const int arg_len[], int nargs, void *ctxp) {
    PackAutoTextCtx *ctx = (PackAutoTextCtx*)ctxp;
    if(ctx->used) *ctx->used = 1;
    if(nargs <= ctx->name_arg_idx) return;
    char name[64];
    if(!pack_auto_arg_str(arg_ptr[ctx->name_arg_idx], arg_len[ctx->name_arg_idx], name, sizeof name)) return;
    pack_auto_add_font_name(ctx->fonts, name);
}

/* Callback that only records the call was seen - used to detect any
 * @draw_list usage (DL_TEXT records carry a font name resolved through
 * select_face() at playback, without any @text call). */
static inline void pack_auto_on_any_call(const char *arg_ptr[], const int arg_len[], int nargs, void *ctxp) {
    (void)arg_ptr; (void)arg_len; (void)nargs;
    *(int*)ctxp = 1;
}

/* Scans `src` for calls to `name(`, i.e. `name` not preceded by an
 * identifier character (so "@text" doesn't match inside "@text_measure"),
 * and invokes cb(args...) for each. */
typedef void (*PackAutoCallCb)(const char *arg_ptr[], const int arg_len[], int nargs, void *ctx);
static inline void pack_auto_scan_calls(const char *src, long len, const char *name,
                                 PackAutoCallCb cb, void *ctx) {
    int name_len = (int)strlen(name);
    long i = 0;
    while(i + name_len <= len) {
        if(memcmp(src + i, name, (size_t)name_len) == 0 &&
           (i == 0 || !pack_auto_isident((unsigned char)src[i - 1]))) {
            long j = i + name_len;
            while(j < len && isspace((unsigned char)src[j])) j++;
            if(j < len && src[j] == '(') {
                const char *arg_ptr[PACK_AUTO_MAX_ARGS]; int arg_len[PACK_AUTO_MAX_ARGS];
                long after;
                int nargs = pack_auto_split_args(src, j, len, arg_ptr, arg_len, PACK_AUTO_MAX_ARGS, &after);
                cb(arg_ptr, arg_len, nargs, ctx);
                i = after;
                continue;
            }
        }
        i++;
    }
}

/* RLM texture-table follower: parses the v2/v3 header of `rlm_host` (a
 * .rlm file already confirmed to exist) and, for each texture name found,
 * adds it resolved against `rlm_rel`'s own directory - the same rule
 * rlm_loader.tgx's rlm_dirname() uses at runtime. v1 files have no texture
 * table and are silently skipped, as are any header sizes/counts that
 * don't check out (best-effort: this only ever adds files, never breaks
 * the surrounding scan). */
static inline void pack_auto_follow_rlm(const char *base_dir, const char *rlm_host, const char *rlm_rel,
                                 PackAutoFile *files, int *nfiles, int max, int verbose) {
    FILE *f = fopen(rlm_host, "rb");
    if(!f) return;
    unsigned char hdr[32] = {0};
    if(fread(hdr, 1, 16, f) != 16 || hdr[0] != 'R' || hdr[1] != 'L' || hdr[2] != 'M') { fclose(f); return; }
    int version = hdr[3];
    if(version != 1 && version != 2 && version != 3) { fclose(f); return; }
    if(version == 1) { fclose(f); return; }  /* no texture table */
    if(fread(hdr + 16, 1, 16, f) != 16) { fclose(f); return; }

    long vcount, icount, nsub, ntex, idx_size;
    if(version == 3) {
        vcount = (long)(hdr[4] | (hdr[5]<<8) | (hdr[6]<<16) | ((unsigned long)hdr[7]<<24));
        icount = (long)(hdr[8] | (hdr[9]<<8) | (hdr[10]<<16) | ((unsigned long)hdr[11]<<24));
        nsub   = hdr[14] | (hdr[15]<<8);
        ntex   = hdr[16] | (hdr[17]<<8);
        idx_size = 4;
    } else {
        vcount = hdr[4] | (hdr[5]<<8);
        icount = (long)(hdr[6] | (hdr[7]<<8) | (hdr[8]<<16) | ((unsigned long)hdr[9]<<24));
        nsub   = hdr[12] | (hdr[13]<<8);
        ntex   = hdr[14] | (hdr[15]<<8);
        idx_size = 2;
    }
    if(vcount <= 0 || icount <= 0 || nsub < 0 || nsub > 32 || ntex <= 0 || ntex > 16) { fclose(f); return; }

    long tex_table_off = 32 + vcount * 32 + icount * idx_size + nsub * 16;
    if(fseek(f, tex_table_off, SEEK_SET) != 0) { fclose(f); return; }

    /* dirname(rlm_rel): everything up to and including the last slash. */
    char dir[PACK_AUTO_PATH] = "";
    const char *slash = strrchr(rlm_rel, '/');
    const char *bslash = strrchr(rlm_rel, '\\');
    if(bslash && (!slash || bslash > slash)) slash = bslash;
    if(slash) snprintf(dir, sizeof dir, "%.*s", (int)(slash - rlm_rel + 1), rlm_rel);

    for(long t = 0; t < ntex; t++) {
        char tname[65] = {0};
        if(fread(tname, 1, 64, f) != 64) break;
        tname[64] = '\0';
        if(tname[0] == '\0') continue;
        char rel[PACK_AUTO_PATH], host[PACK_AUTO_PATH];
        snprintf(rel, sizeof rel, "%.959s%.64s", dir, tname);
        snprintf(host, sizeof host, "%.511s/%.511s", base_dir, rel);
        FILE *tf = fopen(host, "rb");
        if(!tf) {
            if(verbose) fprintf(stderr, "tigryx: --pack-auto: %s references texture '%s', not found at '%s'\n",
                                rlm_rel, tname, rel);
            continue;
        }
        fclose(tf);
        pack_auto_add(files, nfiles, max, host, rel);
    }
    fclose(f);
}

/* ---- font detection ----
 *
 * A "face" is one .ttf/.otf file found in the search path, named by its
 * filename minus extension - the same derivation the backends use
 * (backend_opengl_win32.c's font_face_name(), the DOS backends'
 * relic_font_make_name()). At runtime a requested name selects a face
 * by a case-insensitive substring test (select_face()/
 * face_name_matches(): the face name must contain the request), so a
 * face is wanted whenever a source string is a substring of its name.
 *
 * Wanted names are found two ways, both over the comment-masked source:
 *   - the font-name argument of every @text/@text_measure call
 *     (@text arg 5, @text_measure arg 3) - precise, any length;
 *   - EVERY "..." string literal of >= PACK_AUTO_FONT_MIN_LIT chars.
 *     Font names are routinely carried in variables rather than at the
 *     call site - ui.font, theme.font, helper-fn parameters like
 *     fonttest's draw_row(y, "Roboto") - so only a whole-source literal
 *     scan sees them. Coincidental matches (a UI label like "Bold" or
 *     "Mono") can over-include a face: bounded, benign, and far safer
 *     than missing a name the program can form.
 *
 * `#pragma arg pack_fonts` / --pack-fonts keeps its "I can't be scanned"
 * role as the unconditional escape hatch: it packs EVERY face in the
 * search path, for programs that select faces at runtime (or read them
 * from a file) where no literal can resolve them. When it is not given
 * and a program draws text but nothing resolved, a verbose-mode hint
 * points at the pragma. */
typedef struct {
    char host[PACK_AUTO_PATH];
    char rel[PACK_AUTO_PATH];  /* "fonts/<file>": dedupe key + packed name */
    char face[64];             /* filename minus .ttf/.otf extension */
    int  wanted;
} PackAutoFace;

static inline void pack_auto_add_face(PackAutoFace *faces, int *nfaces, int max,
                                      const char *host, const char *rel, const char *face) {
    for(int i = 0; i < *nfaces; i++)
        if(strcmp(faces[i].rel, rel) == 0) return;  /* same rel: first dir wins */
    if(*nfaces >= max) return;
    PackAutoFace *f = &faces[(*nfaces)++];
    snprintf(f->host, PACK_AUTO_PATH, "%s", host);
    snprintf(f->rel,  PACK_AUTO_PATH, "%s", rel);
    snprintf(f->face, sizeof f->face, "%s", face);
    f->wanted = 0;
}

#if defined(_WIN32)
static inline void pack_auto_collect_font_faces(const char *dir, PackAutoFace *faces,
                                                int *nfaces, int max) {
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if(h == INVALID_HANDLE_VALUE) return;
    do {
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char *dot = strrchr(fd.cFileName, '.');
        if(!dot) continue;
        int is_font = (_stricmp(dot, ".ttf") == 0 || _stricmp(dot, ".otf") == 0);
        if(!is_font) continue;
        char face[64], host[1024], rel[1024];
        int blen = (int)(dot - fd.cFileName);
        if(blen >= (int)sizeof face) blen = (int)sizeof face - 1;
        memcpy(face, fd.cFileName, (size_t)blen); face[blen] = '\0';
        snprintf(host, sizeof host, "%s\\%s", dir, fd.cFileName);
        snprintf(rel, sizeof rel, "fonts/%s", fd.cFileName);
        pack_auto_add_face(faces, nfaces, max, host, rel, face);
    } while(FindNextFileA(h, &fd));
    FindClose(h);
}
#elif !defined(__DJGPP__)
#include <dirent.h>
#include <strings.h>   /* strcasecmp (POSIX; hidden under strict -std=c11) */
static inline void pack_auto_collect_font_faces(const char *dir, PackAutoFace *faces,
                                                int *nfaces, int max) {
    DIR *d = opendir(dir);
    if(!d) return;
    struct dirent *de;
    while((de = readdir(d)) != NULL) {
        const char *dot = strrchr(de->d_name, '.');
        if(!dot) continue;
        int is_font = (strcasecmp(dot, ".ttf") == 0 || strcasecmp(dot, ".otf") == 0);
        if(!is_font) continue;
        char face[64], host[1024], rel[1024];
        int blen = (int)(dot - de->d_name);
        if(blen >= (int)sizeof face) blen = (int)sizeof face - 1;
        memcpy(face, de->d_name, (size_t)blen); face[blen] = '\0';
        snprintf(host, sizeof host, "%s/%s", dir, de->d_name);
        snprintf(rel, sizeof rel, "fonts/%s", de->d_name);
        pack_auto_add_face(faces, nfaces, max, host, rel, face);
    }
    closedir(d);
}
#else
/* DJGPP/DOS: no directory-listing helper here; font packing is skipped
 * (the DOS backend's own font is a fixed-size bitmap TrueType shipped
 * alongside the binaries, not something scripts typically select by
 * name across multiple files). */
static inline void pack_auto_collect_font_faces(const char *dir, PackAutoFace *faces,
                                                int *nfaces, int max) {
    (void)dir; (void)faces; (void)nfaces; (void)max;
}
#endif

/* Marks every face whose name contains `req` (case-insensitive) - the
 * substring rule the backends apply when a script asks for a font. */
static inline void pack_auto_mark_faces(PackAutoFace *faces, int nfaces, const char *req) {
    if(!req || !req[0]) return;
    for(int i = 0; i < nfaces; i++)
        if(pack_auto_ci_contains(faces[i].face, req)) faces[i].wanted = 1;
}

/* The whole-source literal pass: every "..." literal of >=
 * PACK_AUTO_FONT_MIN_LIT chars marks the faces whose names contain it.
 * A literal >= 64 chars can never be contained in a face name (< 64)
 * and is skipped without being copied. */
#define PACK_AUTO_FONT_MIN_LIT 4
static inline void pack_auto_mark_faces_from_literals(const char *src, long len,
                                                      PackAutoFace *faces, int nfaces) {
    if(nfaces <= 0) return;
    long i = 0;
    while(i < len) {
        if(src[i] != '"') { i++; continue; }
        long start = i + 1;
        long j = start;
        while(j < len && src[j] != '"') { if(src[j] == '\\' && j + 1 < len) j++; j++; }
        if(j - start >= PACK_AUTO_FONT_MIN_LIT && j - start < 64) {
            char lit[64]; int o = 0;
            for(long k = start; k < j; k++) {
                char c = src[k];
                if(c == '\\' && k + 1 < j) { k++; c = src[k]; }
                lit[o++] = c;
            }
            lit[o] = '\0';
            pack_auto_mark_faces(faces, nfaces, lit);
        }
        i = (j < len) ? j + 1 : j;
    }
}

#if defined(_WIN32)
static inline int pack_auto_chdir_save(char *save, size_t save_sz) {
    return GetCurrentDirectoryA((DWORD)save_sz, save) != 0;
}
static inline int pack_auto_chdir(const char *new_dir) { return SetCurrentDirectoryA(new_dir) != 0; }
#else
#include <unistd.h>
static inline int pack_auto_chdir_save(char *save, size_t save_sz) {
    return getcwd(save, save_sz) != NULL;
}
static inline int pack_auto_chdir(const char *new_dir) { return chdir(new_dir) == 0; }
#endif

/* Runs the whole pack-auto scan over `r_path` (the flattened, #import-
 * resolved source tigryxpp produced) and appends every resolved, existing
 * file into `assets`. `base_dir` is the entry script's own directory (the
 * convention this project's samples already load assets relative to);
 * `exe_dir` is this tigryx binary's own directory, where bundled fonts
 * ship. Returns the number of files packed. */
static inline int pack_auto_run(RelicAssets *assets, const char *r_path,
                         const char *base_dir, const char *exe_dir, int verbose,
                         int force_fonts) {
    FILE *f = fopen(r_path, "rb");
    if(!f) { fprintf(stderr, "tigryx: --pack-auto: cannot open '%s'\n", r_path); return 0; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *src = (char*)malloc((size_t)sz + 1);
    if(!src) { fclose(f); return 0; }
    long n = (long)fread(src, 1, (size_t)sz, f);
    fclose(f);
    src[n] = '\0';

    /* #pragma arg assets "..." escape hatch - scanned from the untouched
     * source (it's a real directive, not a comment, so masking is moot,
     * and this way it works even if pragma lines are somehow comment-
     * adjacent). */
    RelicPragmaArgs pargs;
    relic_pragma_scan_buf(src, (int)n, &pargs);

    /* Everything else scans the comment-masked copy, so a commented-out
     * call or a documentation snippet with an example path/call in it
     * can't be mistaken for real usage. */
    char *masked = pack_auto_mask_comments(src, n);
    free(src);
    if(!masked) return 0;
    pack_auto_mask_line_directives(masked, n);

    /* files/rlm_paths/faces are ~2MB combined on hosted targets - far
     * past the default stack reserve - so they live on the heap. */
    PackAutoFile *files = (PackAutoFile*)malloc(sizeof(PackAutoFile) * PACK_AUTO_MAX);
    char *rlm_paths = (char*)malloc(64 * PACK_AUTO_PATH);
    PackAutoFace *faces = (PackAutoFace*)malloc(sizeof(PackAutoFace) * PACK_AUTO_MAX);
    if(!files || !rlm_paths || !faces) {
        free(files); free(rlm_paths); free(faces); free(masked);
        return 0;
    }
    int nfiles = 0;
    int nrlm = 0;

    pack_auto_scan_literal_paths(masked, n, base_dir, files, &nfiles, PACK_AUTO_MAX,
                                 (char(*)[PACK_AUTO_PATH])rlm_paths, &nrlm, 64);

    PackAutoFonts fonts = {0};
    int text_used = 0;
    PackAutoTextCtx tctx1 = { &fonts, 5, &text_used };  /* @text(...,font,...)   */
    PackAutoTextCtx tctx2 = { &fonts, 3, &text_used };  /* @text_measure(...,font,...) */
    pack_auto_scan_calls(masked, n, "@text",         pack_auto_on_text_call, &tctx1);
    pack_auto_scan_calls(masked, n, "@text_measure", pack_auto_on_text_call, &tctx2);
    pack_auto_scan_calls(masked, n, "@draw_list",    pack_auto_on_any_call,  &text_used);

    /* Every font face in the search path: exe_dir itself (some platform
     * bins ship *.ttf loose next to the executables) and its fonts/
     * subdir (the pro bins' layout), then the project's own assets/ and
     * fonts/. Dedupe is on the packed `fonts/<name>` rel path, so
     * overlaps cost nothing. */
    int nfaces = 0;
    {
        char dir[1024];
        if(exe_dir && exe_dir[0]) {
            pack_auto_collect_font_faces(exe_dir, faces, &nfaces, PACK_AUTO_MAX);
            snprintf(dir, sizeof dir, "%.1017s/fonts", exe_dir);
            pack_auto_collect_font_faces(dir, faces, &nfaces, PACK_AUTO_MAX);
        }
        snprintf(dir, sizeof dir, "%.1016s/assets", base_dir);
        pack_auto_collect_font_faces(dir, faces, &nfaces, PACK_AUTO_MAX);
        snprintf(dir, sizeof dir, "%.1017s/fonts", base_dir);
        pack_auto_collect_font_faces(dir, faces, &nfaces, PACK_AUTO_MAX);
    }

    /* Mark the faces the program can name: call-site font literals first
     * (any length - the position is precise), then the whole-source
     * literal pass that catches names carried in variables. */
    for(int i = 0; i < fonts.n; i++)
        pack_auto_mark_faces(faces, nfaces, fonts.names[i]);
    pack_auto_mark_faces_from_literals(masked, n, faces, nfaces);
    free(masked);

    for(int i = 0; i < nrlm; i++) {
        int idx = pack_auto_find(files, nfiles, rlm_paths + i * PACK_AUTO_PATH);
        if(idx < 0) continue;
        pack_auto_follow_rlm(base_dir, files[idx].host, files[idx].rel, files, &nfiles, PACK_AUTO_MAX, verbose);
    }

    /* Pack the wanted faces. `--pack-fonts`/`#pragma arg pack_fonts`
     * remains the explicit escape hatch: it packs every face found,
     * for programs that select faces at runtime (e.g. ui.font) where
     * no literal can resolve them. */
    int nwanted = 0;
    for(int i = 0; i < nfaces; i++) if(faces[i].wanted) nwanted++;
    int pack_all = force_fonts || pargs.has_pack_fonts;
    if(!pack_all && text_used && nwanted == 0 && nfaces > 0 && verbose)
        fprintf(stderr, "tigryx: --pack-auto: program draws text but no font names resolved; "
                "use #pragma arg pack_fonts to pack all %d face(s)\n", nfaces);
    for(int i = 0; i < nfaces; i++)
        if(faces[i].wanted || pack_all)
            pack_auto_add(files, &nfiles, PACK_AUTO_MAX, faces[i].host, faces[i].rel);

    int packed = 0;
    for(int i = 0; i < nfiles; i++) {
        if(relic_assets_add_file(assets, files[i].host, files[i].rel)) {
            if(verbose) fprintf(stderr, "tigryx: --pack-auto: packed '%s'\n", files[i].rel);
            packed++;
        }
    }
    /* Remember cwd: a wildcard pragma glob is expanded via
     * relic_assets_add_glob(), which derives the packed asset *name* from
     * the pattern's own directory prefix (see relic_assets_add_glob in
     * ir.c) - passing it a base_dir-prefixed pattern would bake the
     * project's absolute path into every packed name. chdir'ing into
     * base_dir first lets the pattern stay project-relative, exactly like
     * running `tigryx pack --assets <pattern>` from the project directory
     * would. */
    char saved_cwd[PACK_AUTO_PATH] = "";
    int have_saved_cwd = pack_auto_chdir_save(saved_cwd, sizeof saved_cwd);

    for(int i = 0; i < pargs.nassets; i++) {
        const char *spec = pargs.assets[i];
        int has_wildcard = (strchr(spec, '*') != NULL || strchr(spec, '?') != NULL);
        int n_added;
        if(has_wildcard) {
            n_added = (have_saved_cwd && pack_auto_chdir(base_dir)) ? relic_assets_add_glob(assets, spec) : -1;
            if(have_saved_cwd) pack_auto_chdir(saved_cwd);
        } else {
            char host[PACK_AUTO_PATH];
            snprintf(host, sizeof host, "%.511s/%.511s", base_dir, spec);
            n_added = relic_assets_add_file(assets, host, spec) ? 1 : 0;
        }
        if(n_added > 0) {
            if(verbose) fprintf(stderr, "tigryx: --pack-auto: packed %d file(s) from #pragma arg assets \"%s\"\n", n_added, spec);
            packed += n_added;
        } else if(verbose) {
            fprintf(stderr, "tigryx: --pack-auto: #pragma arg assets \"%s\" matched nothing\n", spec);
        }
    }
    free(files); free(rlm_paths); free(faces);
    return packed;
}

#endif /* TIGRYX_PACK_AUTO_H */
