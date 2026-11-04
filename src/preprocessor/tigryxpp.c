/*
 * tigryxpp.c
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
 * tigryxpp.c — Tigryx Preprocessor (token-stream architecture)
 *
 * Pipeline (single pass):
 *   1. Tokenize the input file recursively (inlining #import).  Strings,
 *      char literals, and comments are first-class tokens.  Directives
 *      (#define/#if/#pragma) are processed during this stage; they affect
 *      which tokens are kept and which #define values get substituted
 *      into IDENT tokens.
 *   2. Emit tokens back to the output file.
 *
 * `class` declarations, `new C()`/`x.m()` method calls and `[e0,e1,...]`
 * array literals are native language syntax - they are handled by the
 * compiler's parser/typechecker and pass through here verbatim.  The
 * `--native-class` flag is kept as an accepted no-op for compatibility
 * with build scripts from the transition period.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#ifdef _WIN32
#include <sys/stat.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#endif

#include "tigryx_pragma.h"
#include "tigryx_pp.h"
#include "tigryx_tier.h"

#define RELIC_VERSION   "0.1.0"
#define RELIC_COPYRIGHT "Copyright (c) 2026 Scintillae Limited"
#ifndef RELIC_PLATFORM_ID
#  define RELIC_PLATFORM_ID "unknown"
#endif
#ifndef RELIC_BACKEND_ID
#  define RELIC_BACKEND_ID "unknown"
#endif

/* Both DOS toolchains (DJGPP and gcc-ia16 real mode) share argv[0]-based
 * path resolution, backslash separators and 8.3 filename limits. */
#if defined(RELIC_DOS) || defined(__ia16__)
#  define RELIC_DOS 1
#endif

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__) && defined(__GLIBC__)
#include <linux/limits.h>
#endif

#ifdef __ia16__
/* 16-bit DOS: static arrays must each stay <32K (gcc-ia16 limit), and DOS
 * paths are short anyway.  These caps are ample for a preprocessor that
 * runs host-side tooling in practice. */
#define MAX_DEFINES        160
#define MAX_INCLUDE_DEPTH  16
#define MAX_COND_DEPTH     32
#undef MAX_PATH
#define MAX_PATH           256
#define MAX_IMPORTED       256
#define MAX_IDENT          128
#define MAX_ARTIFACT_ROOTS 4
#else
#define MAX_DEFINES        1024
#define MAX_INCLUDE_DEPTH  16
#define MAX_COND_DEPTH     32
#undef MAX_PATH
#define MAX_PATH           4096
#define MAX_IMPORTED       1024
#define MAX_IDENT          128
#define MAX_ARTIFACT_ROOTS 8
#endif
static int file_exists(const char *path);

static char g_artifact_roots[MAX_ARTIFACT_ROOTS][MAX_PATH];
static int  g_nartifact_roots = -1;   /* -1 = not yet initialised */
#if defined(RELIC_DOS)
static char g_argv0[MAX_PATH] = {0};
#endif
static int g_verbose = 0;
#define vlog(...) do { if(g_verbose) { fprintf(stderr, __VA_ARGS__); fflush(stderr); } } while(0)
typedef enum {
    T_EOF, T_IDENT, T_NUM, T_STR, T_CHR,
    T_OP, T_LCMT, T_BCMT, T_NL
} Tk;

typedef struct {
    Tk    kind;
    char *text;
    int   line;
    int   file_id;  /* index into g_pp_files[]; -1 = synthetic/unknown.
                     * Set for real tokens right after tokenize() (see
                     * process_file_recursive); copied by ta_push_dup(). */
} Tok;

typedef struct {
    Tok *data;
    int  len;
    int  cap;
} TokArr;

static void ta_init(TokArr *a) { a->data = NULL; a->len = 0; a->cap = 0; }

/* Release a TokArr's backing storage, including each token's duplicated
 * text buffer. The original process-once CLI never called this (the OS
 * reclaims it at exit); tigryx_pp_run() does, since it may run many times in
 * the same process (repeated native invocations in future tooling, and the
 * in-browser tigryxwc compiler). */
static void ta_free(TokArr *a) {
    for(int i = 0; i < a->len; i++) free(a->data[i].text);
    free(a->data);
    a->data = NULL; a->len = 0; a->cap = 0;
}

static void ta_reserve(TokArr *a, int extra) {
    if(a->len + extra <= a->cap) return;
    int nc = a->cap ? a->cap : 256;
    while(nc < a->len + extra) nc *= 2;
    a->data = realloc(a->data, sizeof(Tok) * (size_t)nc);
    a->cap = nc;
}

static char *xstrndup_local(const char *s, int n) {
    char *p = malloc((size_t)n + 1);
    memcpy(p, s, (size_t)n);
    p[n] = '\0';
    return p;
}

static void ta_push(TokArr *a, Tk k, const char *s, int n, int line) {
    ta_reserve(a, 1);
    Tok *t = &a->data[a->len++];
    t->kind    = k;
    t->line    = line;
    t->file_id = -1;
    t->text    = xstrndup_local(s, n);
}

static void ta_push_text(TokArr *a, Tk k, const char *s, int line) {
    ta_push(a, k, s, (int)strlen(s), line);
}

/* Duplicate `src` into `dst`, including its file_id - this is how file
 * identity survives the token pipeline (mostly ta_push_dup() of
 * passthrough tokens). */
static void ta_push_dup(TokArr *a, const Tok *src) {
    ta_push_text(a, src->kind, src->text, src->line);
    a->data[a->len - 1].file_id = src->file_id;
}

/* ============================================================ */
/* Artifacts path resolution                                    */
/* ============================================================ */

static void get_executable_path(char *out, int out_sz) {
    out[0] = '\0';
#if defined(_WIN32)
    DWORD len = GetModuleFileName(NULL, out, out_sz);
    if(len > 0) out[len] = '\0';
#elif defined(RELIC_DOS)
    /* DJGPP: use argv[0] captured in main() */
    if(g_argv0[0] != '\0') {
        snprintf(out, out_sz, "%s", g_argv0);
    }
#elif defined(__APPLE__)
    uint32_t len = out_sz;
    if(_NSGetExecutablePath(out, &len) != 0) out[0] = '\0';
#elif defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", out, out_sz - 1);
    if(len > 0) out[len] = '\0';
#elif defined(__sun)
    /* Solaris: /proc/self/path/a.out is the exe path symlink (Solaris 8+).
     * On Solaris 7 fall back to getexecname() + realpath(). */
    ssize_t len = readlink("/proc/self/path/a.out", out, out_sz - 1);
    if(len <= 0) {
        const char *e = getexecname();
        if(e && *e) {
            if(!realpath(e, out)) snprintf(out, out_sz, "%s", e);
            len = (ssize_t)strlen(out);
        }
    }
    if(len > 0) out[len] = '\0';
#endif
}

/* Separator used between entries of the TIGRYX_ARTIFACTS path list.
 * Mirrors the backends: ';' on Windows/DOS (where ':' follows a drive
 * letter), ':' everywhere else. */
#if defined(_WIN32) || defined(RELIC_DOS)
#  define ARTIFACT_PATH_SEP ';'
#else
#  define ARTIFACT_PATH_SEP ':'
#endif

static void add_artifact_root(const char *dir, int len) {
    while(len > 0 && (dir[len-1] == '/' || dir[len-1] == '\\')) len--;
    if(len <= 0 || len >= MAX_PATH) return;
    if(g_nartifact_roots >= MAX_ARTIFACT_ROOTS) return;
    memcpy(g_artifact_roots[g_nartifact_roots], dir, (size_t)len);
    g_artifact_roots[g_nartifact_roots][len] = '\0';
    vlog("[tigryxpp] artifact root[%d]='%s'\n",
         g_nartifact_roots, g_artifact_roots[g_nartifact_roots]);
    g_nartifact_roots++;
}

/* Append "<base>/<name>" as a root. The directory name "artifact" is
 * 8.3-compliant (8 chars), so the same name is used on all platforms
 * including DOS/FAT16; "artifact-working" only ever appears on hosts
 * that run the dev tree, where long filenames are guaranteed. */
static void add_artifact_named_subdir(const char *base, const char *name) {
#if __GNUC__ >= 5
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
    char root[MAX_PATH];
#ifdef RELIC_DOS
    snprintf(root, MAX_PATH, "%s\\%s", base, name);
#else
    snprintf(root, MAX_PATH, "%s/%s", base, name);
#endif
    add_artifact_root(root, (int)strlen(root));
#if __GNUC__ >= 5
#pragma GCC diagnostic pop
#endif
}

static void add_artifact_subdir(const char *base) {
    add_artifact_named_subdir(base, "artifact");
}

/* Chop the last path component off `p` in place. Returns 0 if there is
 * no separator left to chop. */
static int chop_last_component(char *p) {
    char *last_slash = strrchr(p, '/');
#if defined(_WIN32) || defined(RELIC_DOS)
    char *last_bslash = strrchr(p, '\\');
    if(last_bslash && (!last_slash || last_bslash > last_slash)) last_slash = last_bslash;
#endif
    if(!last_slash) return 0;
    *last_slash = '\0';
    return 1;
}

static void init_artifacts_path(void) {
    if(g_nartifact_roots >= 0) return;
    g_nartifact_roots = 0;

    /* Allow override via TIGRYX_ARTIFACTS env var. It may list
     * several roots separated by ARTIFACT_PATH_SEP, matching the runtime
     * backends' interpretation of the same variable. */
    const char *env_art = getenv("TIGRYX_ARTIFACTS");
    if(env_art && *env_art) {
        const char *start = env_art;
        for(;;) {
            const char *sep = strchr(start, ARTIFACT_PATH_SEP);
            int len = sep ? (int)(sep - start) : (int)strlen(start);
            add_artifact_root(start, len);
            if(!sep) break;
            start = sep + 1;
        }
        if(g_nartifact_roots > 0) return;
    }

    char exe_path[MAX_PATH];
    get_executable_path(exe_path, MAX_PATH);
    vlog("[tigryxpp] exe_path='%s'\n", exe_path);
    if(exe_path[0] == '\0') return;

#if defined(RELIC_DOS)
    /* Bare argv[0]: some DOS environments (shells/emulators that don't
     * preserve the PSP env-block program name, or being spawned by name)
     * hand us just "tigryxpp.exe" with no directory. Search PATH for the
     * exe so the artifact roots still anchor at its real location. */
    if(!strchr(exe_path, '/') && !strchr(exe_path, '\\') &&
       !strchr(exe_path, ':')) {
        const char *path_env = getenv("PATH");
        const char *p = path_env;
        while(p && *p) {
            const char *sep = strchr(p, ';');
            int dlen = sep ? (int)(sep - p) : (int)strlen(p);
            if(dlen > 0) {
                char full[MAX_PATH];
                snprintf(full, sizeof full, "%.*s\\%s", dlen, p, exe_path);
                if(file_exists(full)) {
                    snprintf(exe_path, MAX_PATH, "%s", full);
                    break;
                }
                /* argv[0] may lack the extension */
                size_t fl = strlen(full);
                if(fl + 4 < sizeof full) {
                    snprintf(full + fl, sizeof full - fl, ".EXE");
                    if(file_exists(full)) {
                        snprintf(exe_path, MAX_PATH, "%s", full);
                        break;
                    }
                }
            }
            if(!sep) break;
            p = sep + 1;
        }
        vlog("[tigryxpp] exe_path after PATH search='%s'\n", exe_path);
    }
#if defined(__DJGPP__)
    /* Canonicalize relative argv[0] (e.g. "..\BIN\TIGRYXPP.EXE") to an
     * absolute path so the chops below reach the real bin dir. */
    {
        char fx[MAX_PATH];
        _fixpath(exe_path, fx);
        snprintf(exe_path, MAX_PATH, "%s", fx);
        vlog("[tigryxpp] exe_path fixed='%s'\n", exe_path);
    }
#endif
#endif

    if(!chop_last_component(exe_path)) return;  /* chop off tigryxpp */

    /* Development tree: exe is at <repo>/bin/platform/tigryxpp. The repo
     * root's artifact-working/ tree (the development library) is resolved
     * BEFORE both the staged <exe_dir>/artifact snapshot and the shipped
     * <repo>/artifact tree, so library edits take effect immediately
     * without re-running scripts/stage-bin.ps1. Nonexistent roots are
     * skipped harmlessly at probe time, so this is a no-op in installed
     * layouts where ../.. holds no artifact-working dir. */
    char repo_path[MAX_PATH];
    strncpy(repo_path, exe_path, MAX_PATH - 1);
    repo_path[MAX_PATH - 1] = '\0';
    if(chop_last_component(repo_path) && chop_last_component(repo_path)) {
        add_artifact_named_subdir(repo_path, "artifact-working");
    }

    /* Installed layout: every executable sits in the install root and the
     * artifact library ships alongside them, so <exe_dir>/artifact comes
     * first (after artifact-working in a dev checkout). */
    add_artifact_subdir(exe_path);

    /* Development tree: exe is at bin/platform/tigryxpp, artifact is at
     * ../../artifact. */
    if(!chop_last_component(exe_path)) return;  /* chop off platform dir */
    if(!chop_last_component(exe_path)) return;  /* chop off bin */
    add_artifact_subdir(exe_path);
}

static int file_exists(const char *path) {
    /* Try fopen first as a simple existence check */
    FILE *f = fopen(path, "rb");
    if(f) {
        fclose(f);
        return 1;
    }
    return 0;
}

/* Look for `rel` under each configured artifact root in order. On success
 * `out` receives the full path; on failure it is left empty. */
static void try_artifacts_path(const char *rel, char *out, int out_sz) {
    out[0] = '\0';
    init_artifacts_path();
    size_t rel_len = strlen(rel);
    for(int i = 0; i < g_nartifact_roots; i++) {
        const char *root = g_artifact_roots[i];
        /* Check for potential overflow before snprintf */
        if(strlen(root) + rel_len + 2 >= (size_t)out_sz) continue;
#if __GNUC__ >= 5
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
#ifdef RELIC_DOS
        snprintf(out, out_sz, "%s\\%s", root, rel);
#else
        snprintf(out, out_sz, "%s/%s", root, rel);
#endif
#if __GNUC__ >= 5
#pragma GCC diagnostic pop
#endif
        int ok = file_exists(out);
        vlog("[tigryxpp] try_artifacts_path rel='%s' -> '%s' (exists=%d)\n", rel, out, ok);
        if(ok) return;
    }
    out[0] = '\0';
}

static int is_ident_start(int c) { return isalpha(c) || c == '_'; }
static int is_ident_char(int c)  { return isalnum(c) || c == '_'; }

static int read_file_all(const char *path, char **out_buf, int *out_size) {
    FILE *f = fopen(path, "rb");
    if(!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if(!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);
    *out_buf = buf;
    *out_size = (int)got;
    return 1;
}

static void tokenize(const char *src, TokArr *ta) {
    const char *p = src;
    int line = 1;
    while(*p) {
        unsigned char c = (unsigned char)*p;
        if(c == '\n') { ta_push(ta, T_NL, "\n", 1, line); line++; p++; continue; }
        if(c == ' ' || c == '\t' || c == '\r') { p++; continue; }

        if(c == '/' && p[1] == '/') {
            const char *q = p;
            while(*p && *p != '\n') p++;
            ta_push(ta, T_LCMT, q, (int)(p - q), line);
            continue;
        }
        if(c == '/' && p[1] == '*') {
            const char *q = p;
            int sl = line;
            p += 2;
            while(*p && !(p[0] == '*' && p[1] == '/')) {
                if(*p == '\n') line++;
                p++;
            }
            if(*p) p += 2;
            ta_push(ta, T_BCMT, q, (int)(p - q), sl);
            continue;
        }
        if(c == '"' || c == '\'') {
            char quote = (char)c;
            const char *q = p++;
            int sl = line;
            while(*p && *p != quote) {
                if(*p == '\\' && p[1]) { p += 2; continue; }
                if(*p == '\n') line++;
                p++;
            }
            if(*p == quote) p++;
            ta_push(ta, quote == '"' ? T_STR : T_CHR, q, (int)(p - q), sl);
            continue;
        }
        if(is_ident_start(c)) {
            const char *q = p;
            while(is_ident_char((unsigned char)*p)) p++;
            ta_push(ta, T_IDENT, q, (int)(p - q), line);
            continue;
        }
        if(isdigit(c) || (c == '.' && isdigit((unsigned char)p[1]))) {
            const char *q = p;
            if(c == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                while(isxdigit((unsigned char)*p) || *p == '_') p++;
            } else if(c == '0' && (p[1] == 'b' || p[1] == 'B')) {
                p += 2;
                while(*p == '0' || *p == '1' || *p == '_') p++;
            } else {
                while(isdigit((unsigned char)*p) || *p == '_') p++;
                if(*p == '.' && isdigit((unsigned char)p[1])) {
                    p++;
                    while(isdigit((unsigned char)*p) || *p == '_') p++;
                }
                if(*p == 'e' || *p == 'E') {
                    p++;
                    if(*p == '+' || *p == '-') p++;
                    while(isdigit((unsigned char)*p)) p++;
                }
                /* float suffix: f/F = f32, d/D = f64 (mirrors lexer.c) */
                if(*p == 'f' || *p == 'F' || *p == 'd' || *p == 'D') p++;
            }
            ta_push(ta, T_NUM, q, (int)(p - q), line);
            continue;
        }
        const char *q = p;
        int two = 0, three = 0;
        if((p[0] == '<' && p[1] == '<' && p[2] == '=') ||
           (p[0] == '>' && p[1] == '>' && p[2] == '=')) three = 1;
        else if((p[0] == '-' && p[1] == '>') ||
           (p[0] == '=' && p[1] == '=') ||
           (p[0] == '=' && p[1] == '>') ||
           (p[0] == '!' && p[1] == '=') ||
           (p[0] == '<' && p[1] == '=') ||
           (p[0] == '>' && p[1] == '=') ||
           (p[0] == '&' && p[1] == '&') ||
           (p[0] == '|' && p[1] == '|') ||
           (p[0] == '<' && p[1] == '<') ||
           (p[0] == '>' && p[1] == '>') ||
           (p[0] == '.' && p[1] == '.') ||
           (p[0] == '+' && p[1] == '=') ||
           (p[0] == '-' && p[1] == '=') ||
           (p[0] == '*' && p[1] == '=') ||
           (p[0] == '/' && p[1] == '=') ||
           (p[0] == '%' && p[1] == '=') ||
           (p[0] == '&' && p[1] == '=') ||
           (p[0] == '|' && p[1] == '=') ||
           (p[0] == '^' && p[1] == '=')) two = 1;
        if(three)  { ta_push(ta, T_OP, q, 3, line); p += 3; }
        else if(two) { ta_push(ta, T_OP, q, 2, line); p += 2; }
        else    { ta_push(ta, T_OP, q, 1, line); p++; }
    }
    ta_push(ta, T_EOF, "", 0, line);
}

static int g_errors = 0;
static const char *g_current_file = "<input>";

/* File table for #line "path" emission (see emit_output()). Separate from
 * g_current_file (which is a transient pointer used only for pp's own
 * error() messages and can point at a stack buffer - see final_path in
 * process_file_recursive) - this table copies each path's bytes so ids
 * stay valid for the whole run, including into emit_output() at the very
 * end. Reset in relicpp_reset_state(). */
static char **g_pp_files = NULL;   /* grown like g_imported[] */
static int    g_pp_nfiles = 0;
static int    g_pp_files_cap = 0;

static int pp_intern_file(const char *path) {
    for(int i = 0; i < g_pp_nfiles; i++) {
        if(strcmp(g_pp_files[i], path) == 0)
            return i;
    }
    if(g_pp_nfiles >= g_pp_files_cap) {
        int new_cap = (g_pp_files_cap == 0) ? 16 : g_pp_files_cap * 2;
        char **nf = realloc(g_pp_files, (size_t)new_cap * sizeof(char*));
        if(!nf) return -1;             /* token keeps -1 -> bare "#line N" */
        g_pp_files = nf;
        g_pp_files_cap = new_cap;
    }
    char *copy = malloc((size_t)strlen(path) + 1);
    if(!copy) return -1;
    strcpy(copy, path);
    int id = g_pp_nfiles++;
    g_pp_files[id] = copy;
    return id;
}

static void pp_err(int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d: error: ", g_current_file, line);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
    g_errors++;
}

/* ---- #pragma arg ------------------------------------------------------
 * Source-level tool arguments. Validated here against the whitelist in
 * tigryx_pragma.h, then copied verbatim into the emitted .tp so downstream
 * tools (tigryxri, tigryxrc) can pick them up. The core lexer skips any
 * `#...` that is not `#line`, so the passthrough is inert to the parser. */
#define MAX_PRAGMAS 32
static char g_pragmas[MAX_PRAGMAS][256];
static int  g_npragmas = 0;

static void pp_apply_tier(int t);   /* defined with the tier machinery below */
static int  g_tier_locked;          /* set later; declared here for pragma_record */

static void pragma_record(const Tok *args, int nargs, int line) {
    if(nargs < 1 || args[0].kind != T_IDENT || strcmp(args[0].text, "arg") != 0) {
        pp_err(line, "#pragma expects 'arg' (usage: #pragma arg <name> [value])");
        return;
    }
    if(nargs < 2 || args[1].kind != T_IDENT) {
        pp_err(line, "#pragma arg requires a name");
        return;
    }
    const char *name = args[1].text;

    /* Values are re-joined without separators: the tokenizer splits e.g.
     * "16M" into NUM(16) IDENT(M), and no whitelisted value contains a
     * space. */
    char value[RELIC_PRAGMA_MAX_VALUE];
    int v = 0;
    for(int i = 2; i < nargs; i++) {
        const char *s = args[i].text;
        for(int k = 0; s[k] && v < (int)sizeof(value) - 1; k++) value[v++] = s[k];
    }
    value[v] = '\0';

    /* `#pragma arg tier <name>` is consumed here, not recorded verbatim:
     * it selects the Adaptive Build tier for the rest of the file, and
     * the resolved tier is emitted once into the .tp header (so a source
     * pragma and a --tier flag can't disagree in the same output). A
     * --tier flag or TGX_TIER env var locks the tier and wins. */
    if(strcmp(name, "tier") == 0) {
        if(v == 0) { pp_err(line, "'#pragma arg tier' requires a value"); return; }
        {
            int t = relic_tier_parse(value);
            if(t < 0) {
                pp_err(line, "'#pragma arg tier': unknown tier '%s' "
                             "(low|medium|high|super)", value);
                return;
            }
            if(t > relic_tier_ceiling()) {
                pp_err(line, "'#pragma arg tier %s' is above this build's "
                             "ceiling '%s' (platform %s)",
                             value, relic_tier_name(relic_tier_ceiling()),
                             relic_tier_platform());
                return;
            }
            if(!g_tier_locked) pp_apply_tier(t);
        }
        return;
    }

    const RelicPragmaSpec *spec = relic_pragma_lookup(name);
    if(!spec) {
        pp_err(line, "unknown '#pragma arg' name '%s'", name);
        return;
    }
    if(spec->takes_value && v == 0) {
        pp_err(line, "'#pragma arg %s' requires a value", name);
        return;
    }
    if(!spec->takes_value && v > 0) {
        pp_err(line, "'#pragma arg %s' takes no value", name);
        return;
    }
    if(g_npragmas >= MAX_PRAGMAS) {
        pp_err(line, "too many #pragma arg directives (max %d)", MAX_PRAGMAS);
        return;
    }
    if(v > 0)
        snprintf(g_pragmas[g_npragmas], sizeof(g_pragmas[0]), "#pragma arg %s %s", name, value);
    else
        snprintf(g_pragmas[g_npragmas], sizeof(g_pragmas[0]), "#pragma arg %s", name);
    g_npragmas++;
}

typedef struct {
    char  name[MAX_IDENT];
    Tok  *value;
    int   nvalue;
} Define;

static Define g_defines[MAX_DEFINES];
static int    g_ndefines = 0;

static int define_find(const char *name) {
    for(int i = 0; i < g_ndefines; i++)
        if(strcmp(g_defines[i].name, name) == 0) return i;
    return -1;
}

static void define_set(const char *name, const Tok *tokens, int n) {
    int idx = define_find(name);
    if(idx < 0) {
        if(g_ndefines >= MAX_DEFINES) {
            fprintf(stderr, "tigryxpp: too many #define macros (limit %d) while defining '%s'.\n"
                            "  Raise MAX_DEFINES in src/preprocessor/tigryxpp.c and rebuild.\n",
                    MAX_DEFINES, name);
            exit(1);
        }
        idx = g_ndefines++;
    }
    snprintf(g_defines[idx].name, MAX_IDENT, "%s", name);
    free(g_defines[idx].value);
    g_defines[idx].value = NULL;
    g_defines[idx].nvalue = 0;
    if(n > 0) {
        g_defines[idx].value = malloc(sizeof(Tok) * (size_t)n);
        for(int i = 0; i < n; i++) g_defines[idx].value[i] = tokens[i];
        g_defines[idx].nvalue = n;
    }
}

static void define_set_text(const char *name, const char *text) {
    if(!text || !*text) { define_set(name, NULL, 0); return; }
    TokArr ta; ta_init(&ta);
    tokenize(text, &ta);
    int n = ta.len;
    while(n > 0 && (ta.data[n-1].kind == T_EOF || ta.data[n-1].kind == T_NL)) n--;
    define_set(name, ta.data, n);
}

static void define_undef(const char *name) {
    int idx = define_find(name);
    if(idx < 0) return;
    free(g_defines[idx].value);
    g_defines[idx].value = NULL;
    g_defines[idx].nvalue = 0;
    int last = --g_ndefines;
    if(idx != last) {
        g_defines[idx] = g_defines[last];
        g_defines[last].value = NULL;
        g_defines[last].nvalue = 0;
    }
}

typedef enum { COND_ACTIVE, COND_SKIP, COND_DONE } CondState;
static CondState g_cond[MAX_COND_DEPTH];
static int       g_cond_depth = 0;

static int cond_active(void) {
    for(int i = 0; i < g_cond_depth; i++)
        if(g_cond[i] != COND_ACTIVE) return 0;
    return 1;
}

/* ---- #if expression evaluator ---------------------------------------
 * Integer-only, C precedence. Identifiers expand one level of #define
 * (recursively, depth-limited); undefined identifiers evaluate to 0.
 * `defined X` and `defined(X)` are supported. The result is only
 * computed while the enclosing conditional group is active - a dead
 * `#if` never evaluates, matching the C preprocessor. */
typedef struct {
    const Tok *t;
    int        n;
    int        pos;
    int        line;
    int        depth;
} IfCx;

static long if_or(IfCx *c);

static const Tok *if_next(IfCx *c) {
    return c->pos < c->n ? &c->t[c->pos++] : NULL;
}

static int if_op(IfCx *c, const char *s) {
    const Tok *t = c->pos < c->n ? &c->t[c->pos] : NULL;
    if(t && t->kind == T_OP && strcmp(t->text, s) == 0) { c->pos++; return 1; }
    return 0;
}

static long if_num(const Tok *t) {
    /* T_NUM text: decimal, 0x hex, 0b binary, optional '.'/exponent/suffix,
     * '_' digit separators. #if is integer-only - floats truncate. */
    char buf[72];
    int w = 0;
    for(const char *p = t->text; *p && w < (int)sizeof(buf) - 1; p++)
        if(*p != '_') buf[w++] = *p;
    buf[w] = '\0';
    if(strchr(buf, '.')) return (long)strtod(buf, NULL);
    if(buf[0] == '0' && (buf[1] == 'b' || buf[1] == 'B')) {
        long v = 0;
        for(const char *p = buf + 2; *p == '0' || *p == '1'; p++) v = (v << 1) | (*p - '0');
        return v;
    }
    return strtol(buf, NULL, 0);
}

static long if_primary(IfCx *c) {
    const Tok *t = if_next(c);
    if(!t) { pp_err(c->line, "#if: unexpected end of expression"); return 0; }

    if(t->kind == T_NUM) return if_num(t);

    if(t->kind == T_CHR) {
        /* 'x' or a simple escape - first quoted character. */
        const char *s = t->text;
        if(strlen(s) >= 3) {
            char ch = s[1];
            if(ch == '\\' && strlen(s) >= 4) {
                char e = s[2];
                ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' :
                     e == '0' ? '\0' : e;
            }
            return (unsigned char)ch;
        }
        return 0;
    }

    if(t->kind == T_OP && strcmp(t->text, "(") == 0) {
        long v = if_or(c);
        if(!if_op(c, ")")) pp_err(c->line, "#if: expected ')'");
        return v;
    }

    if(t->kind == T_IDENT) {
        if(strcmp(t->text, "defined") == 0) {
            const Tok *n = if_next(c);
            int d;
            if(!n) { pp_err(c->line, "#if: 'defined' needs a name"); return 0; }
            if(n->kind == T_OP && strcmp(n->text, "(") == 0) {
                n = if_next(c);
                if(!n || n->kind != T_IDENT) {
                    pp_err(c->line, "#if: defined() needs a name"); return 0;
                }
                d = define_find(n->text) >= 0;
                if(!if_op(c, ")")) pp_err(c->line, "#if: expected ')'");
            } else {
                if(n->kind != T_IDENT) {
                    pp_err(c->line, "#if: 'defined' needs a name"); return 0;
                }
                d = define_find(n->text) >= 0;
            }
            return d;
        }
        {
            int idx = define_find(t->text);
            if(idx < 0) return 0;    /* C: undefined identifier -> 0 */
            if(c->depth > 8) {
                pp_err(c->line, "#if: macro expansion too deep"); return 0;
            }
            /* Evaluate the macro's replacement list as an expression, so
             * `#if TGX_TIER >= TGX_TIER_HIGH` works with TGX_TIER_HIGH=2. */
            {
                IfCx sub = { g_defines[idx].value, g_defines[idx].nvalue,
                             0, c->line, c->depth + 1 };
                if(sub.n == 0) return 0;
                return if_or(&sub);
            }
        }
    }
    pp_err(c->line, "#if: unexpected token '%s'", t->text);
    return 0;
}

static long if_unary(IfCx *c) {
    if(if_op(c, "!")) return !if_unary(c);
    if(if_op(c, "~")) return ~if_unary(c);
    if(if_op(c, "-")) return -if_unary(c);
    if(if_op(c, "+")) return +if_unary(c);
    return if_primary(c);
}

static long if_mul(IfCx *c) {
    long v = if_unary(c);
    for(;;) {
        if(if_op(c, "*")) v *= if_unary(c);
        else if(if_op(c, "/")) {
            long d = if_unary(c);
            if(d == 0) { pp_err(c->line, "#if: division by zero"); return 0; }
            v /= d;
        }
        else if(if_op(c, "%")) {
            long d = if_unary(c);
            if(d == 0) { pp_err(c->line, "#if: division by zero"); return 0; }
            v %= d;
        }
        else return v;
    }
}

static long if_add(IfCx *c) {
    long v = if_mul(c);
    for(;;) {
        if(if_op(c, "+")) v += if_mul(c);
        else if(if_op(c, "-")) v -= if_mul(c);
        else return v;
    }
}

static long if_shift(IfCx *c) {
    long v = if_add(c);
    for(;;) {
        if(if_op(c, "<<")) v <<= if_add(c);
        else if(if_op(c, ">>")) v >>= if_add(c);
        else return v;
    }
}

static long if_rel(IfCx *c) {
    long v = if_shift(c);
    for(;;) {
        if(if_op(c, "<=")) v = (v <= if_shift(c));
        else if(if_op(c, ">=")) v = (v >= if_shift(c));
        else if(if_op(c, "<")) v = (v < if_shift(c));
        else if(if_op(c, ">")) v = (v > if_shift(c));
        else return v;
    }
}

static long if_eq(IfCx *c) {
    long v = if_rel(c);
    for(;;) {
        if(if_op(c, "==")) v = (v == if_rel(c));
        else if(if_op(c, "!=")) v = (v != if_rel(c));
        else return v;
    }
}

static long if_band(IfCx *c) {
    long v = if_eq(c);
    while(if_op(c, "&")) v &= if_eq(c);
    return v;
}
static long if_bxor(IfCx *c) {
    long v = if_band(c);
    while(if_op(c, "^")) v ^= if_band(c);
    return v;
}
static long if_bor(IfCx *c) {
    long v = if_bxor(c);
    while(if_op(c, "|")) v |= if_bxor(c);
    return v;
}
static long if_land(IfCx *c) {
    long v = if_bor(c);
    /* NB: the rhs must ALWAYS be parsed, even when v is already 0 - a C
     * short-circuit `v && if_bor(c)` would leave the tokens unconsumed
     * and trip the "trailing token" check. */
    while(if_op(c, "&&")) { long r = if_bor(c); v = (v && r); }
    return v;
}
static long if_or(IfCx *c) {
    long v = if_land(c);
    while(if_op(c, "||")) { long r = if_land(c); v = (v || r); }
    return v;
}

static int pp_if_eval(const Tok *args, int nargs, int line) {
    IfCx c = { args, nargs, 0, line, 0 };
    long v = if_or(&c);
    if(c.pos < c.n)
        pp_err(line, "#if: trailing token '%s'", c.t[c.pos].text);
    return v != 0;
}

/* ---- Adaptive Build tier defines -------------------------------------
 * Mirrors include/tigryx_tier.h; the capability names map 1:1 onto
 * RELIC_CAP_* bits. The define set is:
 *   always numeric:  TGX_TIER (0..3) and TGX_TIER_{LOW,MEDIUM,HIGH,SUPER}
 *                    (ordinals — so `#if TGX_TIER >= TGX_TIER_HIGH` works)
 *   always string:   TGX_PLATFORM, TGX_BACKEND (quoted)
 *   existence, one:  TGX_LOW / TGX_MEDIUM / TGX_HIGH / TGX_SUPER
 *   cumulative:      TGX_TIER_ATLEAST_{MEDIUM,HIGH,SUPER}
 *   per-backend:     TGX_HAS_* / TGX_NO_* / TGX_MEM_64K (build caps —
 *                    these describe the binary, NOT the requested tier)
 *   tier nominal:    TGX_SCREEN_W / TGX_SCREEN_H
 */
static int g_tier = -1;        /* resolved tier for this run            */
static int g_tier_locked = 0;  /* set by --tier / TGX_TIER env: a source
                                * `#pragma arg tier` may not override   */

static void pp_apply_tier(int t) {
    static const char *const ord_names[4] = {
        "TGX_TIER_LOW", "TGX_TIER_MEDIUM", "TGX_TIER_HIGH", "TGX_TIER_SUPER" };
    static const char *const sel_names[4] = {
        "TGX_LOW", "TGX_MEDIUM", "TGX_HIGH", "TGX_SUPER" };
    static const char *const at_names[4] = {
        NULL, "TGX_TIER_ATLEAST_MEDIUM", "TGX_TIER_ATLEAST_HIGH", "TGX_TIER_ATLEAST_SUPER" };
    static const int scr_w[4] = { 320, 320, 640, 1920 };
    static const int scr_h[4] = { 200, 200, 480, 1080 };
    static const struct { unsigned int bit; const char *name; } caps[] = {
        { RELIC_CAP_MESH,     "TGX_HAS_MESH" },
        { RELIC_CAP_TEXTURE,  "TGX_HAS_TEXTURE" },
        { RELIC_CAP_SPRITE,   "TGX_HAS_SPRITE" },
        { RELIC_CAP_GPU,      "TGX_HAS_GPU" },
        { RELIC_CAP_FILES,    "TGX_HAS_FILES" },
        { RELIC_CAP_PCM,      "TGX_HAS_PCM" },
        { RELIC_CAP_TONE,     "TGX_HAS_TONE" },
        { RELIC_CAP_PALETTE8, "TGX_PALETTE_8" },
        { RELIC_CAP_NO_FPU,   "TGX_NO_FPU" },
        { RELIC_CAP_NO_F32,   "TGX_NO_F32" },
        { RELIC_CAP_MEM_64K,  "TGX_MEM_64K" },
        { RELIC_CAP_TOUCH,    "TGX_HAS_TOUCH" },
    };
    char num[16];
    int  i;
    unsigned int c;

    g_tier = t;
    snprintf(num, sizeof num, "%d", t);
    define_set_text("TGX_TIER", num);
    {
        char pid[128], bid[64];
        snprintf(pid, sizeof pid, "\"%s\"", relic_tier_platform());
        snprintf(bid, sizeof bid, "\"%s\"", relic_tier_backend());
        define_set_text("TGX_PLATFORM", pid);
        define_set_text("TGX_BACKEND",  bid);
    }
    for(i = 0; i < 4; i++) {
        snprintf(num, sizeof num, "%d", i);
        define_set_text(ord_names[i], num);
        if(i == t) define_set_text(sel_names[i], "1");
        else       define_undef(sel_names[i]);
        if(at_names[i]) {
            if(i <= t) define_set_text(at_names[i], "1");
            else       define_undef(at_names[i]);
        }
    }
    snprintf(num, sizeof num, "%d", scr_w[t]);
    define_set_text("TGX_SCREEN_W", num);
    snprintf(num, sizeof num, "%d", scr_h[t]);
    define_set_text("TGX_SCREEN_H", num);

    c = relic_backend_caps();
    for(i = 0; i < (int)(sizeof(caps) / sizeof(caps[0])); i++) {
        if(c & caps[i].bit) define_set_text(caps[i].name, "1");
        else                define_undef(caps[i].name);
    }
}

/* Resolve the tier for this run and install the defines. Called at the
 * start of tigryx_pp_run, after state reset. Honour order:
 *   explicit override (--tier / tigryx_pp_set_tier) > TGX_TIER env >
 *   auto-detect (which itself honours nothing user-facing). A later
 *   `#pragma arg tier` may still redefine everything mid-file unless
 *   g_tier_locked. */
static void pp_resolve_tier(void) {
    int locked = relic_tier_override();
    if(locked >= 0) {
        g_tier_locked = 1;
        pp_apply_tier(locked);
        return;
    }
    {
        const char *e = getenv("TGX_TIER");
        if(e && *e) {
            int t = relic_tier_parse(e);
            if(t < 0) {
                fprintf(stderr,
                    "tigryxpp: warning: ignoring bad TGX_TIER='%s' "
                    "(low|medium|high|super)\n", e);
            } else {
                int ceil = relic_tier_ceiling();
                if(t > ceil) {
                    fprintf(stderr,
                        "tigryxpp: warning: TGX_TIER=%s is above this "
                        "platform's ceiling '%s'; clamping\n",
                        e, relic_tier_name(ceil));
                    t = ceil;
                }
                g_tier_locked = 1;
                pp_apply_tier(t);
                return;
            }
        }
    }
    pp_apply_tier(relic_tier_detect());
}

/* ---- -D NAME[=VALUE] command-line defines ------------------------------
 * Stored outside the per-run define table so they survive
 * relicpp_reset_state() and can be set once by an embedding tool
 * (tigryxwc) before several tigryx_pp_run() calls. */
#ifdef __ia16__
/* 64 entries x (128+256) would eat ~24K of DGROUP on its own. 8 is ample:
 * the only -D a 16-bit toolchain realistically passes is a tier/test knob. */
#define MAX_USER_DEFINES 8
#else
#define MAX_USER_DEFINES 64
#endif
static struct {
    char name[MAX_IDENT];
    char value[256];
} g_user_defs[MAX_USER_DEFINES];
static int g_nuser_defs = 0;

int tigryx_pp_define(const char *name, const char *value) {
    if(g_nuser_defs >= MAX_USER_DEFINES) return -1;
    snprintf(g_user_defs[g_nuser_defs].name, MAX_IDENT, "%s", name);
    snprintf(g_user_defs[g_nuser_defs].value,
             sizeof(g_user_defs[0].value), "%s", value ? value : "1");
    g_nuser_defs++;
    return 0;
}

#ifndef RELIC_PP_NO_MAIN
static int pp_add_user_def_spec(const char *spec) {
    /* "NAME" or "NAME=VALUE" */
    char name[MAX_IDENT];
    const char *eq = strchr(spec, '=');
    int n = eq ? (int)(eq - spec) : (int)strlen(spec);
    if(n <= 0 || n >= MAX_IDENT) return -1;
    memcpy(name, spec, (size_t)n);
    name[n] = '\0';
    return tigryx_pp_define(name, eq ? eq + 1 : "1");
}
#endif /* !RELIC_PP_NO_MAIN */

static void pp_apply_user_defs(void) {
    for(int i = 0; i < g_nuser_defs; i++)
        define_set_text(g_user_defs[i].name, g_user_defs[i].value);
}

static char **g_imported = NULL;
static int   g_nimported = 0;
static int   g_imported_cap = 0;

static int imported_seen(const char *path) {
    for(int i = 0; i < g_nimported; i++)
        if(g_imported[i] && strcmp(g_imported[i], path) == 0) return 1;
    return 0;
}

static void imported_mark(const char *path) {
    if(g_nimported >= MAX_IMPORTED) {
        fprintf(stderr, "tigryxpp: too many imported files (limit %d) while importing '%s'.\n"
                        "  Raise MAX_IMPORTED in src/preprocessor/tigryxpp.c and rebuild.\n",
                MAX_IMPORTED, path);
        exit(1);
    }
    if(g_nimported >= g_imported_cap) {
        int new_cap = g_imported_cap ? g_imported_cap * 2 : 8;
        if(new_cap > MAX_IMPORTED) new_cap = MAX_IMPORTED;
        char **new_arr = realloc(g_imported, sizeof(char*) * (size_t)new_cap);
        if(!new_arr) return;
        g_imported = new_arr;
        for(int i = g_imported_cap; i < new_cap; i++) g_imported[i] = NULL;
        g_imported_cap = new_cap;
    }
    if(g_nimported < g_imported_cap) {
        g_imported[g_nimported] = malloc(MAX_PATH);
        if(g_imported[g_nimported]) {
            snprintf(g_imported[g_nimported], MAX_PATH, "%s", path);
            g_nimported++;
        }
    }
}

static void resolve_path(const char *base, const char *rel, char *out, int out_sz) {
    if(rel[0] == '/' || rel[0] == '\\') {
        snprintf(out, (size_t)out_sz, "%s", rel);
        return;
    }
    snprintf(out, (size_t)out_sz, "%s", base);
    char *slash = strrchr(out, '/');
    char *bslash = strrchr(out, '\\');
    if(!slash || (bslash && bslash > slash)) slash = bslash;
    if(slash) *(slash+1) = '\0'; else out[0] = '\0';
    int used = (int)strlen(out);
    snprintf(out + used, (size_t)(out_sz - used), "%s", rel);
}

/* ============================================================ */
/* Stage 1: directive processing                                */
/* ============================================================ */
static void process_file_recursive(const char *path, const char *rel_import, TokArr *dst, int depth);

static int directive_at_line_start(const TokArr *src, int i) {
    int j = i - 1;
    while(j >= 0) {
        Tk k = src->data[j].kind;
        if(k == T_NL) return 1;
        if(k == T_LCMT || k == T_BCMT) { j--; continue; }
        return 0;
    }
    return 1;
}

static void emit_define_value(const Define *d, TokArr *dst) {
    for(int i = 0; i < d->nvalue; i++) ta_push_dup(dst, &d->value[i]);
}

static void process_tokens(const TokArr *src, TokArr *dst, const char *path, int depth) {
    int i = 0;
    while(i < src->len) {
        const Tok *t = &src->data[i];
        if(t->kind == T_EOF) break;

        if(t->kind == T_OP && strcmp(t->text, "#") == 0 &&
           directive_at_line_start(src, i))
        {
            int j = i + 1;
            while(j < src->len && (src->data[j].kind == T_LCMT ||
                                    src->data[j].kind == T_BCMT)) j++;
            if(j >= src->len || src->data[j].kind != T_IDENT) {
                pp_err(t->line, "expected directive name after '#'");
                while(i < src->len && src->data[i].kind != T_NL) i++;
                if(i < src->len) i++;
                continue;
            }
            const char *dname = src->data[j].text;
            int line = src->data[j].line;
            int k = j + 1;
            int end = k;
            while(end < src->len && src->data[end].kind != T_NL && src->data[end].kind != T_EOF) end++;

            Tok args_buf[256];
            int nargs = 0;
            for(int x = k; x < end && nargs < 256; x++) {
                if(src->data[x].kind == T_LCMT || src->data[x].kind == T_BCMT) continue;
                args_buf[nargs++] = src->data[x];
            }

            if(strcmp(dname, "define") == 0) {
                if(cond_active()) {
                    if(nargs < 1 || args_buf[0].kind != T_IDENT) pp_err(line, "#define requires a name");
                    else define_set(args_buf[0].text, &args_buf[1], nargs - 1);
                }
            } else if(strcmp(dname, "undef") == 0) {
                if(cond_active() && nargs >= 1 && args_buf[0].kind == T_IDENT)
                    define_undef(args_buf[0].text);
            } else if(strcmp(dname, "env") == 0) {
                if(cond_active()) {
                    if(nargs < 1 || args_buf[0].kind != T_IDENT) pp_err(line, "#env requires a name");
                    else {
                        const char *val = getenv(args_buf[0].text);
                        define_set_text(args_buf[0].text, val ? val : "");
                    }
                }
            } else if(strcmp(dname, "ifdef") == 0) {
                if(g_cond_depth >= MAX_COND_DEPTH) pp_err(line, "too many nested conditionals");
                else {
                    int parent_active = cond_active();
                    int defined = (nargs >= 1 && args_buf[0].kind == T_IDENT && define_find(args_buf[0].text) >= 0);
                    g_cond[g_cond_depth++] = (parent_active && defined) ? COND_ACTIVE : COND_SKIP;
                }
            } else if(strcmp(dname, "ifndef") == 0) {
                if(g_cond_depth >= MAX_COND_DEPTH) pp_err(line, "too many nested conditionals");
                else {
                    int parent_active = cond_active();
                    int not_defined = !(nargs >= 1 && args_buf[0].kind == T_IDENT && define_find(args_buf[0].text) >= 0);
                    g_cond[g_cond_depth++] = (parent_active && not_defined) ? COND_ACTIVE : COND_SKIP;
                }
            } else if(strcmp(dname, "if") == 0) {
                if(g_cond_depth >= MAX_COND_DEPTH) pp_err(line, "too many nested conditionals");
                else {
                    int parent_active = cond_active();
                    int v = parent_active ? pp_if_eval(args_buf, nargs, line) : 0;
                    g_cond[g_cond_depth++] = (parent_active && v) ? COND_ACTIVE : COND_SKIP;
                }
            } else if(strcmp(dname, "elif") == 0) {
                if(g_cond_depth == 0) pp_err(line, "#elif without #if");
                else {
                    CondState *top = &g_cond[g_cond_depth - 1];
                    if(*top == COND_ACTIVE) *top = COND_DONE;
                    else if(*top == COND_SKIP) {
                        int parent_active = 1;
                        for(int x = 0; x < g_cond_depth - 1; x++)
                            if(g_cond[x] != COND_ACTIVE) { parent_active = 0; break; }
                        if(parent_active && pp_if_eval(args_buf, nargs, line))
                            *top = COND_ACTIVE;
                    }
                }
            } else if(strcmp(dname, "else") == 0) {
                if(g_cond_depth == 0) pp_err(line, "#else without #ifdef");
                else {
                    int parent_active = 1;
                    for(int x = 0; x < g_cond_depth - 1; x++)
                        if(g_cond[x] != COND_ACTIVE) { parent_active = 0; break; }
                    if(parent_active) {
                        CondState *top = &g_cond[g_cond_depth - 1];
                        if(*top == COND_ACTIVE) *top = COND_DONE;
                        else if(*top == COND_SKIP) *top = COND_ACTIVE;
                    }
                }
            } else if(strcmp(dname, "endif") == 0) {
                if(g_cond_depth == 0) pp_err(line, "#endif without #ifdef");
                else g_cond_depth--;
            } else if(strcmp(dname, "import") == 0) {
                if(cond_active()) {
                    if(nargs < 1 || args_buf[0].kind != T_STR) pp_err(line, "#import expects a quoted path");
                    else {
                        const char *raw = args_buf[0].text;
                        int rlen = (int)strlen(raw);
                        if(rlen >= 2 && raw[0] == '"' && raw[rlen-1] == '"') {
                            char rel[MAX_PATH];
                            int n = rlen - 2;
                            if(n >= MAX_PATH) n = MAX_PATH - 1;
                            memcpy(rel, raw + 1, (size_t)n);
                            rel[n] = '\0';
                            char abs_path[MAX_PATH];
                            resolve_path(path, rel, abs_path, MAX_PATH);
                            ta_push_text(dst, T_LCMT, "// --- imported ---", line);
                            ta_push(dst, T_NL, "\n", 1, line);
                            process_file_recursive(abs_path, rel, dst, depth + 1);
                            ta_push_text(dst, T_LCMT, "// --- end import ---", line);
                            ta_push(dst, T_NL, "\n", 1, line);
                        }
                    }
                }
            } else if(strcmp(dname, "pragma") == 0) {
                if(cond_active()) pragma_record(args_buf, nargs, line);
            } else {
                pp_err(line, "unknown directive '#%s'", dname);
            }

            i = end;
            if(i < src->len && src->data[i].kind == T_NL) i++;
            continue;
        }

        if(!cond_active()) {
            if(t->kind == T_NL) ta_push_dup(dst, t);
            i++;
            continue;
        }

        if(t->kind == T_IDENT) {
            int idx = define_find(t->text);
            if(idx >= 0) {
                emit_define_value(&g_defines[idx], dst);
                i++;
                continue;
            }
        }

        ta_push_dup(dst, t);
        i++;
    }
}

static void process_file_recursive(const char *path, const char *rel_import, TokArr *dst, int depth) {
    if(depth > MAX_INCLUDE_DEPTH) {
        fprintf(stderr, "error: maximum #import depth (%d) exceeded\n", MAX_INCLUDE_DEPTH);
        g_errors++; return;
    }
    if(imported_seen(path)) return;
    imported_mark(path);

    char *buf;
    int   sz;
    const char *final_path = path;
    char alt_path[MAX_PATH];
    
    if(!read_file_all(path, &buf, &sz)) {
        /* Try artifact folder as fallback.
         * First try the full relative import path (e.g. "gui/button.tgx")
         * so subdirectories work. Then fall back to basename-only for
         * backward compatibility with flat artifact names. */
        int found = 0;
        if(rel_import && rel_import[0]) {
            try_artifacts_path(rel_import, alt_path, MAX_PATH);
            if(alt_path[0] != '\0' && read_file_all(alt_path, &buf, &sz)) {
                final_path = alt_path;
                found = 1;
            }
        }
        if(!found) {
            const char *basename = strrchr(path, '/');
            const char *basename2 = strrchr(path, '\\');
            if(basename2 && (!basename || basename2 > basename)) basename = basename2;
            if(basename) basename++; else basename = path;
            
            try_artifacts_path(basename, alt_path, MAX_PATH);
            if(alt_path[0] != '\0' && read_file_all(alt_path, &buf, &sz)) {
                final_path = alt_path;
                found = 1;
            }
        }
        if(!found) {
            fprintf(stderr, "error: cannot open '%s'\n", path);
            g_errors++; return;
        }
    }

    TokArr raw; ta_init(&raw);
    tokenize(buf, &raw);

    /* Stamp every raw token with this file's id. tokenize() doesn't know
     * about file identity (it just scans a buffer), so this is the one
     * place a whole file's worth of tokens gets tagged; every later pass
     * propagates it via ta_push_dup(). */
    int file_id = pp_intern_file(final_path);
    for(int i = 0; i < raw.len; i++) raw.data[i].file_id = file_id;

    const char *prev_file = g_current_file;
    g_current_file = final_path;
    process_tokens(&raw, dst, final_path, depth);
    g_current_file = prev_file;
    (void)sz;
}

/* ============================================================ */
/* Emitter                                                      */
/* ============================================================ */
static int needs_space_between(const Tok *a, const Tok *b) {
    if(!a || a->kind == T_NL || b->kind == T_NL) return 0;
    if(b->kind == T_OP) {
        const char *s = b->text;
        if(!strcmp(s, ")") || !strcmp(s, "]") || !strcmp(s, ",") ||
           !strcmp(s, ";") || !strcmp(s, ":") || !strcmp(s, ".")) return 0;
        if(!strcmp(s, "(") || !strcmp(s, "[")) {
            if(a->kind == T_IDENT || a->kind == T_NUM) return 0;
            if(a->kind == T_OP && (!strcmp(a->text, ")") || !strcmp(a->text, "]"))) return 0;
        }
    }
    if(a->kind == T_OP) {
        const char *s = a->text;
        if(!strcmp(s, "(") || !strcmp(s, "[") || !strcmp(s, ".") ||
           !strcmp(s, "@") || !strcmp(s, "&") || !strcmp(s, "*")) return 0;
    }
    if(a->kind == T_LCMT || a->kind == T_BCMT) return 0;
    return 1;
}

/* Emit a `#line N` (bare) or `#line N "path"` directive, per the C-compatible
 * form the lexer understands (see src/core/lexer.c). `file_id < 0` (a
 * synthetic token - see the Tok comment) omits the filename, which leaves
 * the compiler's notion of "current file" unchanged rather than clobbering
 * it with "unknown". */
static void emit_line_directive(FILE *out, int line, int file_id) {
    if(file_id >= 0 && file_id < g_pp_nfiles)
        fprintf(out, "#line %d \"%s\"\n", line, g_pp_files[file_id]);
    else
        fprintf(out, "#line %d\n", line);
}

static void emit_output(FILE *out, const TokArr *toks) {
    const Tok *prev = NULL;

    /* Tool arguments first, so a consumer can find them without reading the
     * whole file. These are comments as far as the lexer is concerned. */
    for(int i = 0; i < g_npragmas; i++) fprintf(out, "%s\n", g_pragmas[i]);

    /* The resolved Adaptive Build tier, always emitted. Downstream tools
     * rescan `#pragma arg` lines (tigryxri for the .tlx stamp, tigryxcc)
     * — this is how the tier chosen here reaches them without any flag
     * forwarding through the spawned processes. A source `#pragma arg
     * tier` is consumed by pragma_record rather than recorded verbatim,
     * so exactly one tier line ever appears. */
    if(g_tier >= 0)
        fprintf(out, "#pragma arg tier %s\n", relic_tier_name(g_tier));

    /* Source line/file of the most recent #line directive we emitted. -1
     * forces an initial directive before the first real token. */
    int last_src_line = -1;
    int last_src_file = -1;
    /* Line/file we want to emit next, deferred until a line boundary so
     * that we never insert #line inside an expression. */
    int pending_line = -1;
    int pending_file = -1;

    for(int i = 0; i < toks->len; i++) {
        const Tok *t = &toks->data[i];
        if(t->kind == T_EOF) break;

        if(t->kind == T_NL) {
            fputc('\n', out);
            prev = t;
            /* At a line boundary, flush any pending #line directive. */
            if(pending_line > 0 && (pending_line != last_src_line || pending_file != last_src_file)) {
                emit_line_directive(out, pending_line, pending_file);
                last_src_line = pending_line;
                if(pending_file >= 0) last_src_file = pending_file;
                prev = NULL;
            }
            pending_line = -1;
            pending_file = -1;
            continue;
        }

        /* Track source line/file changes. If we are at a line boundary
         * (prev is NULL or a newline) we can emit immediately. Otherwise
         * defer until the next newline so we never split an expression
         * with a directive. */
        int line_changed = (t->line > 0 && t->line != last_src_line);
        int file_changed = (t->file_id >= 0 && t->file_id != last_src_file);
        if(line_changed || file_changed) {
            int at_line_start = (!prev || prev->kind == T_NL);
            if(at_line_start) {
                emit_line_directive(out, t->line, t->file_id);
                last_src_line = t->line;
                if(t->file_id >= 0) last_src_file = t->file_id;
                prev = NULL;
                pending_line = -1;
                pending_file = -1;
            } else {
                pending_line = t->line;
                pending_file = t->file_id;
            }
        }

        if(needs_space_between(prev, t)) fputc(' ', out);
        fputs(t->text, out);
        prev = t;
    }
    if(toks->len == 0 || toks->data[toks->len-1].kind != T_NL) fputc('\n', out);
}

/* ============================================================ */
/* Library entry point (tigryx_pp_run)                            */
/* ============================================================ */

/* Reset all preprocessor-internal state that is scoped to a single run.
 * The original process-once CLI never needed this (the OS reclaims
 * everything at exit); tigryx_pp_run() does, since it can be called
 * repeatedly within one process (the in-browser tigryxwc compiler is the
 * motivating case).
 *
 * g_artifact_roots/g_nartifact_roots is deliberately NOT reset here: it is
 * a lazily-computed, process-wide cache of search roots (derived from an
 * env var or the executable's location) that does not vary between runs. */
static void relicpp_reset_state(void) {
    g_errors       = 0;
    g_current_file = "<input>";
    if(g_pp_files) {
        for(int i = 0; i < g_pp_nfiles; i++) free(g_pp_files[i]);
        free(g_pp_files);
        g_pp_files = NULL;
    }
    g_pp_nfiles    = 0;
    g_pp_files_cap = 0;
    g_npragmas     = 0;
    g_tier         = -1;
    g_tier_locked  = 0;
    g_ndefines     = 0;
    g_cond_depth   = 0;
    if(g_imported) {
        for(int i = 0; i < g_nimported; i++) free(g_imported[i]);
        free(g_imported);
        g_imported = NULL;
    }
    g_nimported     = 0;
    g_imported_cap  = 0;
}

int tigryx_pp_run(const char *input_path, const char *output_path) {
    relicpp_reset_state();
    g_current_file = input_path;
    /* Adaptive Build: install the tier + capability defines, then any
     * -D overrides (a -D wins — the user is explicitly asking for it). */
    pp_resolve_tier();
    pp_apply_user_defs();
    vlog("[tigryxpp] input=%s output=%s\n", input_path, output_path ? output_path : "(stdout)");

    /* A single token pass: #import recursion, #define/#if handling and
     * tier pragmas. `class` declarations, method calls and array
     * literals are native language syntax - the parser/typechecker in
     * the compiler handles them, so they pass through verbatim. */
    TokArr stage1; ta_init(&stage1);
    vlog("[tigryxpp] processing files recursively...\n");
    process_file_recursive(input_path, NULL, &stage1, 0);
    vlog("[tigryxpp] stage1 tokens=%d\n", stage1.len);

    if(g_cond_depth > 0)
        fprintf(stderr, "warning: %d unterminated #ifdef block(s)\n", g_cond_depth);

    vlog("[tigryxpp] opening output %s...\n", output_path ? output_path : "(stdout)");
    FILE *out = output_path ? fopen(output_path, "w") : stdout;
    if(!out) {
        fprintf(stderr, "error: cannot open output '%s'\n", output_path);
        ta_free(&stage1);
        return 1;
    }
    vlog("[tigryxpp] emitting output...\n");
    fprintf(out, "// Generated by tigryxpp from %s\n", input_path);
    emit_output(out, &stage1);
    if(output_path) fclose(out);
    ta_free(&stage1);
    vlog("[tigryxpp] emit done (tier=%s)\n",
         g_tier >= 0 ? relic_tier_name(g_tier) : "?");

    if(g_errors) {
        vlog("[tigryxpp] errors occurred, returning 1\n");
        if(output_path) remove(output_path);
        return 1;
    }
    return 0;
}

/* ============================================================ */
/* Main                                                          */
/* ============================================================ */
#ifndef RELIC_PP_NO_MAIN
int main(int argc, char **argv) {
#if defined(RELIC_DOS)
    /* Capture argv[0] for DJGPP executable path resolution */
    if(argc > 0 && argv[0]) {
        snprintf(g_argv0, MAX_PATH, "%s", argv[0]);
    }
#endif

    vlog("[tigryxpp] started argc=%d\n", argc);
    for(int i = 0; i < argc; i++) vlog("[tigryxpp]   argv[%d]=%s\n", i, argv[i]);

    if(argc < 2) {
        fprintf(stderr,
            "\n"
            RELIC_COPYRIGHT "\n"
            "\n"
            "BETA SOFTWARE - this is a beta release. There will be bugs, and the\n"
            "API and behaviour may change between releases without notice.\n"
            "\n"
            "Version:  tigryxpp " RELIC_VERSION " BETA\n"
            "Platform: " RELIC_PLATFORM_ID "\n"
            "\n"
            "Usage:\n"
            "  tigryxpp [options] <input.tgx> [output.tp]     preprocess .tgx to .tp\n"
            "  tigryxpp --version                           print version\n"
            "\n"
            "Options:\n"
            "  --verbose                  enable verbose/diagnostic output\n"
            "  --version                  print version and exit\n"
            "  -o <file>                  output .tp path (overrides [output.tp])\n"
            "  -D NAME[=VALUE]            add a define (applied after the tier\n"
            "                             predefines, so a -D wins)\n"
            "  --tier low|medium|high|super\n"
            "                             pin the Adaptive Build tier (default:\n"
            "                             auto-detect; TGX_TIER env also works)\n"
            "  --platform <id>            cross-compile target platform id\n"
            "                             (ceiling/caps/TGX_PLATFORM; hosted\n"
            "                             compilers pass their target here)\n"
            "  --backend <id>             cross-compile target backend id\n"
            "  --print-tier               print the resolved tier and exit\n"
            "\n"
            "  If [output.tp] is omitted, the preprocessed source is written to stdout.\n"
        );
        return 1;
    }
    if(argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        printf("tigryxpp %s BETA (%s) built %s %s\n", RELIC_VERSION, RELIC_PLATFORM_ID, __DATE__, __TIME__);
        printf("%s\n", RELIC_COPYRIGHT);
        return 0;
    }
    if(argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        fprintf(stderr,
            "\n"
            RELIC_COPYRIGHT "\n"
            "\n"
            "BETA SOFTWARE - this is a beta release. There will be bugs, and the\n"
            "API and behaviour may change between releases without notice.\n"
            "\n"
            "Version:  tigryxpp " RELIC_VERSION " BETA\n"
            "Platform: " RELIC_PLATFORM_ID "\n"
            "\n"
            "Usage:\n"
            "  tigryxpp [options] <input.tgx> [output.tp]     preprocess .tgx to .tp\n"
            "  tigryxpp --version                           print version\n"
            "\n"
            "Options:\n"
            "  --verbose                  enable verbose/diagnostic output\n"
            "  --version                  print version and exit\n"
            "  -o <file>                  output .tp path (overrides [output.tp])\n"
            "  -D NAME[=VALUE]            add a define (applied after the tier\n"
            "                             predefines, so a -D wins)\n"
            "  --tier low|medium|high|super\n"
            "                             pin the Adaptive Build tier (default:\n"
            "                             auto-detect; TGX_TIER env also works)\n"
            "  --platform <id>            cross-compile target platform id\n"
            "                             (ceiling/caps/TGX_PLATFORM; hosted\n"
            "                             compilers pass their target here)\n"
            "  --backend <id>             cross-compile target backend id\n"
            "  --print-tier               print the resolved tier and exit\n"
            "\n"
            "  If [output.tp] is omitted, the preprocessed source is written to stdout.\n"
        );
        return 0;
    }

    /* Scan for options, collect positional args. `-D NAME[=V]` adds a
     * define (applied after the tier predefines, so a -D wins); `--tier`
     * pins the Adaptive Build tier; `-o` is an explicit output path —
     * the historical `tigryxpp in.tgx -o out.tp` footgun (which wrote
     * a file literally named "-o") is now handled properly, and any
     * other unknown '-' option is an error rather than a positional. */
    const char *positional[8];
    const char *cli_output = NULL;
    const char *cli_tier = NULL, *cli_platform = NULL, *cli_backend = NULL;
    int npos = 0;
    int print_tier = 0;
    for(int i = 1; i < argc && npos < 8; i++) {
        if(strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if(strcmp(argv[i], "--print-tier") == 0) {
            print_tier = 1;
        } else if(strcmp(argv[i], "--native-class") == 0) {
            /* Backward compatibility only: classes and array literals
             * are always native syntax now - nothing to switch on. */
        } else if(strcmp(argv[i], "--tier") == 0) {
            if(i + 1 >= argc) {
                fprintf(stderr, "tigryxpp: --tier requires a value "
                                "(low|medium|high|super)\n");
                return 1;
            }
            cli_tier = argv[++i];
        } else if(strcmp(argv[i], "--platform") == 0) {
            /* Cross-compile: pretend to be this platform for ceiling,
             * capability and TGX_PLATFORM/TGX_BACKEND purposes. A hosted
             * compiler (e.g. TOS tigryxcc spawning a Windows pp) passes
             * its target's ids here. */
            if(i + 1 >= argc) {
                fprintf(stderr, "tigryxpp: --platform requires an id\n");
                return 1;
            }
            cli_platform = argv[++i];
        } else if(strcmp(argv[i], "--backend") == 0) {
            if(i + 1 >= argc) {
                fprintf(stderr, "tigryxpp: --backend requires an id\n");
                return 1;
            }
            cli_backend = argv[++i];
        } else if(strcmp(argv[i], "-D") == 0) {
            if(i + 1 >= argc) {
                fprintf(stderr, "tigryxpp: -D requires NAME[=VALUE]\n");
                return 1;
            }
            if(pp_add_user_def_spec(argv[++i]) != 0) {
                fprintf(stderr, "tigryxpp: bad -D '%s'\n", argv[i]);
                return 1;
            }
        } else if(strncmp(argv[i], "-D", 2) == 0 && argv[i][2] != '\0') {
            if(pp_add_user_def_spec(argv[i] + 2) != 0) {
                fprintf(stderr, "tigryxpp: bad -D '%s'\n", argv[i] + 2);
                return 1;
            }
        } else if(strcmp(argv[i], "-o") == 0) {
            if(i + 1 >= argc) {
                fprintf(stderr, "tigryxpp: -o requires a filename\n");
                return 1;
            }
            cli_output = argv[++i];
        } else if(argv[i][0] == '-') {
            fprintf(stderr, "tigryxpp: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            positional[npos++] = argv[i];
        }
    }

    /* Apply the cross-compile identity BEFORE the tier check so --tier
     * is validated against the *target's* ceiling regardless of the
     * order the flags appeared in. */
    if(cli_platform || cli_backend)
        relic_tier_set_platform(cli_platform, cli_backend);
    if(cli_tier) {
        char err[160];
        if(relic_tier_set_override_name(cli_tier, err, sizeof err) != 0) {
            fprintf(stderr, "tigryxpp: %s\n", err);
            return 1;
        }
    }

    if(print_tier) {
        /* Route through the real resolver so the honour order (override >
         * env > detect), the env-value warnings and the ceiling clamp are
         * exactly what a preprocessing run would use. */
        pp_resolve_tier();
        fprintf(stderr,
            "tigryxpp: tier=%s (ceiling=%s, platform=%s, backend=%s)\n",
            relic_tier_name(g_tier), relic_tier_name(relic_tier_ceiling()),
            relic_tier_platform(), relic_tier_backend());
        if(npos < 1) return 0;
    }

    if(npos < 1) {
        fprintf(stderr,
            "\n"
            RELIC_COPYRIGHT "\n"
            "\n"
            "BETA SOFTWARE - this is a beta release. There will be bugs, and the\n"
            "API and behaviour may change between releases without notice.\n"
            "\n"
            "Version:  tigryxpp " RELIC_VERSION " BETA\n"
            "Platform: " RELIC_PLATFORM_ID "\n"
            "\n"
            "Usage:\n"
            "  tigryxpp [options] <input.tgx> [output.tp]     preprocess .tgx to .tp\n"
            "  tigryxpp --version                           print version\n"
            "\n"
            "Options:\n"
            "  --verbose                  enable verbose/diagnostic output\n"
            "  --version                  print version and exit\n"
            "  -o <file>                  output .tp path (overrides [output.tp])\n"
            "  -D NAME[=VALUE]            add a define (applied after the tier\n"
            "                             predefines, so a -D wins)\n"
            "  --tier low|medium|high|super\n"
            "                             pin the Adaptive Build tier (default:\n"
            "                             auto-detect; TGX_TIER env also works)\n"
            "  --platform <id>            cross-compile target platform id\n"
            "                             (ceiling/caps/TGX_PLATFORM; hosted\n"
            "                             compilers pass their target here)\n"
            "  --backend <id>             cross-compile target backend id\n"
            "  --print-tier               print the resolved tier and exit\n"
            "\n"
            "  If [output.tp] is omitted, the preprocessed source is written to stdout.\n"
        );
        return 1;
    }

    const char *input_path  = positional[0];
    const char *output_path = cli_output ? cli_output
                             : (npos >= 2) ? positional[1] : NULL;
    return tigryx_pp_run(input_path, output_path);
}
#endif /* !RELIC_PP_NO_MAIN */
