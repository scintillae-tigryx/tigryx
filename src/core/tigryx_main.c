/*
 * tigryx_main.c
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
 * tigryx_main.c - Tigryx dispatcher (the public-facing 'tigryx' command)
 *
 * Usage:
 *   tigryx run   <file.tp|.tgx|.tla|.tlx> [args...]  -- interpret / run
 *   tigryx bc    <file.tp|.tgx> [--pack-auto|--pack-fonts] [args...]  -- compile to .tlx bytecode
 *   tigryx asm   <file.tp|.tgx>            [args...]  -- compile to .tla assembly
 *   tigryx bind  <file.tp|.tgx|.tlx>       [args...]  -- produce bound executable (VM + .tlx)
 *   tigryx build <file.tp|.tgx|.tlx>       [args...]  -- produce native executable
 *   tigryx pp    <file.tgx>               [args...]  -- preprocess only, emit .tp
 *   tigryx version                                  -- print version
 *
 * Internal binaries (siblings of this executable):
 *   tigryxri   interpreter / bytecode compiler
 *   tigryxrc   C-emitting native compiler
 *              (dispatched by `tigryx build`, uses libtcc to link)
 *   tigryxpp   preprocessor
 *   tigryxvm   VM runtime for bound executables
 *
 * File extensions:
 *   .tp    Tigryx source
 *   .tgx   Tigryx extended source
 *   .tla  Tigryx assembly listing
 *   .tlx  Tigryx portable bytecode (distributable)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include "../../include/tigryx.h"
#include "../../include/tigryx_pragma.h"
#include "../../include/tigryx_tier.h"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define PATH_SEP '\\'
#  define EXE_EXT  ".exe"
#elif defined(RELIC_DOS)
#  include <dos.h>
#  include <process.h>
#  include <errno.h>
#  define PATH_SEP '\\'
#  define EXE_EXT  ".exe"
#else
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <errno.h>
#  define PATH_SEP '/'
#  define EXE_EXT  ""
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>
#  endif
#endif

#ifndef RELIC_VERSION
#  define RELIC_VERSION  "0.1.0"
#endif
#ifndef RELIC_COPYRIGHT
#  define RELIC_COPYRIGHT "Copyright (c) 2026 Scintillae Limited"
#endif

#ifndef RELIC_PLATFORM_ID
#  define RELIC_PLATFORM_ID "unknown"
#endif
#ifndef RELIC_BACKEND_ID
#  define RELIC_BACKEND_ID "unknown"
#endif

static int g_verbose = 0;

#define vlog(...) do { if(g_verbose) { fprintf(stderr, __VA_ARGS__); fflush(stderr); } } while(0)

/* -------------------------------------------------------------------------
 * Self-path resolution
 * ---------------------------------------------------------------------- */
static char g_argv0[2048] = {0};
static int self_dir(char *out, size_t out_sz) {
    char buf[2048] = {0};
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)(sizeof buf - 1));
    if (n == 0 || n >= sizeof buf - 1) return 0;
#elif defined(RELIC_DOS)
    /* DJGPP/DOS: use argv[0] captured in main() */
    if (g_argv0[0] == '\0') return 0;
    snprintf(buf, sizeof buf, "%s", g_argv0);
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)(sizeof buf - 1);
    if (_NSGetExecutablePath(buf, &sz) != 0) return 0;
#elif defined(__linux__)
    ssize_t r = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (r <= 0) return 0;
    buf[r] = '\0';
#elif defined(__sun)
    /* Solaris: path/a.out needs Solaris 8+; getexecname()+realpath
     * covers Solaris 7 (see relic_sun_exepath in tigryx.h). */
    if (!relic_sun_exepath(buf, sizeof buf)) return 0;
#else
    return 0;
#endif
    char *slash = strrchr(buf, PATH_SEP);
#if defined(_WIN32) || defined(RELIC_DOS)
    if (!slash) slash = strrchr(buf, '/');
#endif
    if (slash) {
        size_t dir_len = (size_t)(slash - buf + 1);
        if (dir_len >= out_sz) return 0;
        memcpy(out, buf, dir_len);
        out[dir_len] = '\0';
    } else {
        out[0] = '\0';
    }
    return 1;
}

/* Build a full path to a sibling binary next to this executable. */
static void sibling_path(const char *dir, const char *name, char *out, size_t out_sz) {
    size_t dir_len = strlen(dir);
    /* Check if dir already ends with a separator */
    if (dir_len > 0 && (dir[dir_len-1] == '/' || dir[dir_len-1] == '\\')) {
        snprintf(out, out_sz, "%s%s%s", dir, name, EXE_EXT);
    } else {
        snprintf(out, out_sz, "%s%c%s%s", dir, PATH_SEP, name, EXE_EXT);
    }
}

/* Build a full path to a child binary, preferring its GUI-subsystem twin
 * (the same name with a 'w' appended) when no console may be shown.
 *
 * A console-subsystem child is given a console window by Windows whenever the
 * parent has none, which is exactly the Explorer / file-association case. On
 * NT that can be suppressed with CREATE_NO_WINDOW, but Windows 95/98/ME
 * ignore that flag and DETACHED_PROCESS is documented-buggy there, so the
 * child's PE subsystem is the only reliable guarantee. The twins are built for
 * the win95 targets; elsewhere they don't exist and we fall back to the
 * console-subsystem name (where CREATE_NO_WINDOW does the job). */
static void child_path(const char *dir, const char *name, int prefer_gui,
                       char *out, size_t out_sz) {
    if (prefer_gui) {
        char gui_name[64];
        snprintf(gui_name, sizeof gui_name, "%sw", name);
        sibling_path(dir, gui_name, out, out_sz);
        FILE *probe = fopen(out, "rb");
        if (probe) { fclose(probe); return; }
    }
    sibling_path(dir, name, out, out_sz);
}

/* -------------------------------------------------------------------------
 * Extension helpers
 * ---------------------------------------------------------------------- */
static const char *file_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot ? dot : "";
}

/* Extension equality. DOS (DJGPP) always uppercases filenames, and users
 * on other platforms may type uppercase extensions too, so we compare
 * case-insensitively on ALL platforms. */
static int ext_ieq(const char *ext, const char *want) {
    while (*ext && *want) {
        char a = *ext++, b = *want++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return *ext == 0 && *want == 0;
}

/* Replace extension on path. Writes into out[out_sz]. */
static void replace_ext(const char *path, const char *new_ext, char *out, size_t out_sz) {
    const char *dot = strrchr(path, '.');
    size_t base_len = dot ? (size_t)(dot - path) : strlen(path);
    snprintf(out, out_sz, "%.*s%s", (int)base_len, path, new_ext);
}

/* -------------------------------------------------------------------------
 * Process execution
 * ---------------------------------------------------------------------- */

#if defined(_WIN32)
/* True when this process has a console of its own.
 *
 * GetConsoleWindow() is Windows 2000+ and isn't even declared by the vintage
 * MinGW headers the win95 target builds against, so on Win9x we probe the
 * standard handles instead: GetConsoleMode() succeeds only on a real console
 * handle. All three are checked because stdout/stderr may be redirected to a
 * file or pipe while a console is still attached. */
static int have_console(void) {
#if defined(RELIC_WIN95)
    static const DWORD ids[3] = { STD_OUTPUT_HANDLE, STD_ERROR_HANDLE, STD_INPUT_HANDLE };
    for (int i = 0; i < 3; i++) {
        HANDLE h = GetStdHandle(ids[i]);
        DWORD mode = 0;
        if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) return 1;
    }
    return 0;
#else
    return GetConsoleWindow() != NULL;
#endif
}
#endif

/* True when this invocation may not show a console at all: --no-console was
 * asked for AND we have no console of our own, which is exactly the
 * tigryxw.exe / tigryxriw.exe Explorer launch. Children spawned from here must
 * therefore be GUI-subsystem twins (see child_path). */
static int silent_launch(int no_console) {
#if defined(_WIN32)
    return no_console && !have_console();
#else
    (void)no_console;
    return 0;
#endif
}

/* True when the child that actually runs the user's program should be
 * spawned detached rather than waited on.
 *
 * Only on an Explorer / file-association launch: --no-console was asked for
 * AND we have no console at all, which is exactly the tigryxw.exe case. There
 * the dispatcher has nothing left to report once the child is running, and
 * Windows keeps showing the "busy" cursor for as long as the process it
 * launched is alive - so waiting would spin the cursor for the entire life
 * of the program. Detaching lets the dispatcher exit the moment the child
 * is up and the spinner stops.
 *
 * From a shell we always wait, so `tigryx run ...` behaves like any other CLI
 * command: the prompt returns when the program exits and $? is its status. */
static int should_detach(int no_console) {
    return silent_launch(no_console);
}

#if defined(_WIN32)
static int run_process(const char *exe, char **argv, int argc, int detach) {
    /* Build a single quoted command line for CreateProcess */
    char cmdline[8192] = {0};
    size_t pos = 0;
    for (int i = 0; i < argc && argv[i]; i++) {
        if (i > 0 && pos < sizeof cmdline - 1) cmdline[pos++] = ' ';
        int needs_quote = (strchr(argv[i], ' ') || strchr(argv[i], '\t'));
        if (needs_quote && pos < sizeof cmdline - 1) cmdline[pos++] = '"';
        size_t len = strlen(argv[i]);
        if (pos + len >= sizeof cmdline - 2) { fprintf(stderr, "tigryx: command line too long\n"); return 1; }
        memcpy(cmdline + pos, argv[i], len); pos += len;
        if (needs_quote && pos < sizeof cmdline - 1) cmdline[pos++] = '"';
    }
    cmdline[pos] = '\0';

    STARTUPINFOA si = {0}; si.cb = sizeof si;
    /* Hand the child our own std handles. Without STARTF_USESTDHANDLES a
     * CONSOLE-subsystem child spawned under CREATE_NO_WINDOW (we get here
     * whenever we have no console window - mintty, IDEs, any terminal that
     * pipes stdio instead of giving us a real console) has its stdin/out/err
     * rebound to the invisible console, so its diagnostics - parse errors
     * included - go nowhere. Passing our handles explicitly makes stderr
     * reach the user's terminal/pipe in that case, and is a no-op under
     * cmd/PowerShell (they'd be inherited anyway). NULL handles (GUI parent)
     * simply leave the child without that stream, as before. */
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi = {0};
    /* The children (tigryxri, tigryxpp, tigryxvm) are CONSOLE-subsystem
     * binaries, which is what we want from a shell: they inherit our console
     * and their output appears inline. But if WE have no console (tigryxw.exe
     * on an Explorer launch) each child would be given its own console
     * window - the very flash we're avoiding. Suppress it:
     *
     *   DETACHED_PROCESS  - child gets no console at all. Already implies
     *                       CREATE_NO_WINDOW, and the two must not be
     *                       combined (CREATE_NO_WINDOW is ignored when
     *                       DETACHED_PROCESS is set).
     *   CREATE_NO_WINDOW  - child gets a console, but no window for it.
     *
     * CREATE_NO_WINDOW is Windows 2000+: Windows 95/98/ME ignore it and hand
     * the console child a visible console window anyway - that is the window
     * that pops up when a .tp/.tgx/.tlx/.tla is double-clicked there.
     * DETACHED_PROCESS has been honoured since Win95, so use it instead on
     * that target. We still wait for the child as usual; only its console is
     * gone (its stdio output has nowhere to go, which is already the case
     * under CREATE_NO_WINDOW on NT). */
    DWORD creation_flags = 0;
    if (detach) {
        creation_flags |= DETACHED_PROCESS;
    } else if (!have_console()) {
#if defined(RELIC_WIN95)
        creation_flags |= DETACHED_PROCESS;
#else
        creation_flags |= CREATE_NO_WINDOW;
#endif
    }
    if (!CreateProcessA(exe, cmdline, NULL, NULL, TRUE, creation_flags, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "tigryx: failed to launch '%s' (error %lu)\n", exe, GetLastError());
        return 1;
    }
    if (detach) {
        /* Don't wait — let the child run independently so the dispatcher
         * can exit immediately and Windows stops showing the spinner. */
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return 0;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}
#elif defined(RELIC_DOS)
static int run_process(const char *exe, char **argv, int argc, int detach) {
    /* DJGPP provides spawnvp for process execution with argv array */
    (void)argc; (void)detach;
    /* Convert forward slashes to backslashes for DJGPP spawnvp */
    char exe_normalized[512];
    snprintf(exe_normalized, sizeof(exe_normalized), "%s", exe);
    for(char *p = exe_normalized; *p; p++) {
        if(*p == '/') *p = '\\';
    }
    /* Mode 0 = wait for child process to finish */
    return spawnvp(0, exe_normalized, argv);
}
#else
static int run_process(const char *exe, char **argv, int argc, int detach) {
    (void)argc;
    pid_t pid = fork();
    if (pid < 0) { fprintf(stderr, "tigryx: fork failed: %s\n", strerror(errno)); return 1; }
    if (pid == 0) {
        execv(exe, argv);
        fprintf(stderr, "tigryx: exec '%s' failed: %s\n", exe, strerror(errno));
        _exit(127);
    }
    if (detach) return 0;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
#endif

/* -------------------------------------------------------------------------
 * Subcommand implementations
 * ---------------------------------------------------------------------- */

/* Forward a `--tier <name>`/`--platform <id>`/`--backend <id>` found in
 * extra[] onto a tigryxpp argv being built for a pre-flatten step. The
 * flags already reach the *target* tool through extra[]; the preprocessor
 * invocation is a separate argv and needs its own copies (it is what turns
 * the tier and target identity into TGX_* defines).
 * TGX_TIER env needs no forwarding: the child inherits it. */
static void pp_forward_tier(char **pp_args, int *pp_argc,
                            char **extra, int nextra) {
    static const char *const fwd[] = { "--tier", "--platform", "--backend" };
    for(int f = 0; f < 3; f++) {
        for(int i = 0; i + 1 < nextra; i++) {
            if(strcmp(extra[i], fwd[f]) == 0) {
                pp_args[(*pp_argc)++] = (char*)fwd[f];
                pp_args[(*pp_argc)++] = extra[i + 1];
                break;
            }
        }
    }
    /* --native-class: a valueless flag - forward when present so the
     * spawned tigryxpp leaves class syntax for the compiler. */
    for(int i = 0; i < nextra; i++) {
        if(strcmp(extra[i], "--native-class") == 0) {
            pp_args[(*pp_argc)++] = (char*)"--native-class";
            break;
        }
    }
}

/* tigryx run <file> [tigryx flags] [-- user args]
 *
 * Spawns: tigryxri --input <file> [tigryx flags...] -- [user args...]
 * The "--" sentinel tells tigryxri where its own flags end and the
 * program's @args passthrough begins. */
static int cmd_run(const char *dir, const char *input, const char *output,
                   char **extra, int nextra, char **user_argv, int user_argc, int no_console) {
    (void)output; /* run has no output file */
    char ri[512], pp[512], tmp_r[1024];
    /* Explorer / file-association launch: run the program through the
     * GUI-subsystem twins so Windows never creates a console for them. */
    int silent = silent_launch(no_console);
    child_path(dir, "tigryxri", silent, ri, sizeof ri);
    child_path(dir, "tigryxpp", silent, pp, sizeof pp);

    const char *ext = file_ext(input);
    const char *actual_input = input;
    int need_cleanup = 0;

    vlog("[tigryx] cmd_run dir=%s input=%s\n", dir, input);
    vlog("[tigryx] ri=%s pp=%s\n", ri, pp);
    if (ext_ieq(ext, ".tgx")) {
        if (!relic_pp_tmp_path(input, tmp_r, sizeof tmp_r)) {
            fprintf(stderr, "tigryx: cannot build temp path for '%s'\n", input);
            return 1;
        }
        vlog("[tigryx] preprocessing %s -> %s\n", input, tmp_r);
        /* tigryxpp [--verbose] <input.tgx> <output.tp> */
        char *pp_args[12]; int pp_argc = 0;
        pp_args[pp_argc++] = pp;
        if (g_verbose) pp_args[pp_argc++] = "--verbose";
        pp_forward_tier(pp_args, &pp_argc, extra, nextra);
        pp_args[pp_argc++] = (char*)input;
        pp_args[pp_argc++] = tmp_r;
        pp_args[pp_argc] = NULL;
        int rc = run_process(pp, pp_args, pp_argc, 0);
        if (rc != 0) {
#if defined(_WIN32)
            DeleteFileA(tmp_r);
#else
            remove(tmp_r);
#endif
            return rc;
        }
        actual_input = tmp_r;
        need_cleanup = 1;
    }

    /* tigryxri --input <file> [extra...] [-- user...]
     * Always emit the "--" separator (even with no user args) so tigryxri
     * has an unambiguous end-of-flags marker. */
    int base = 3;
    int argc_out = base + nextra + 1 /* "--" */ + user_argc;
    int total = argc_out + 1 /* NULL */;
    char **args = calloc((size_t)total, sizeof(char*));
    if (!args) { fprintf(stderr, "tigryx: out of memory\n"); return 1; }
    args[0] = ri;
    args[1] = "--input";
    args[2] = (char*)actual_input;
    for (int i = 0; i < nextra; i++) args[base + i] = extra[i];
    args[base + nextra] = "--";
    for (int i = 0; i < user_argc; i++) args[base + nextra + 1 + i] = user_argv[i];
    args[argc_out] = NULL;

    vlog("[tigryx] running tigryxri --input %s (nextra=%d user_argc=%d)\n", actual_input, nextra, user_argc);
    int detach = should_detach(no_console);
    int rc = run_process(ri, args, argc_out, detach);
    vlog("[tigryx] tigryxri returned %d (detach=%d)\n", rc, detach);
    free(args);

    /* Detached: tigryxri is still reading the preprocessed .tp, so deleting it
     * here would pull the file out from under it. It lives in the OS temp dir
     * and is PID-tagged (relic_pp_tmp_path), so leaving it is safe - the OS
     * reclaims it. When we waited for the child, the file is done with. */
    if (!detach) {
#if defined(_WIN32)
        if (need_cleanup) DeleteFileA(tmp_r);
#else
        if (need_cleanup) remove(tmp_r);
#endif
    }
    return rc;
}

#if defined(RELIC_COMMERCIAL) || defined(RELIC_MAKER)
/* tigryx bind <file> [extra args] -- produces bound executable via tigryxri --output-executable
 * Commercial/maker: relies on tigryxvm as the bound VM runtime.
 * `no_console`, if set, is forwarded to tigryxri as --no-console, which
 * bakes RELIC_BIND_FLAG_NO_CONSOLE into the produced .exe's footer so it
 * hides its own console when double-clicked (see tigryx.h). */
static int cmd_bind(const char *dir, const char *input, const char *output, char **extra, int nextra, int no_console) {
    char ri[512], pp[512], tmp_r[1024];
    sibling_path(dir, "tigryxri", ri, sizeof ri);
    sibling_path(dir, "tigryxpp", pp, sizeof pp);

    const char *ext = file_ext(input);
    const char *actual_input = input;
    int need_cleanup = 0;

    if (ext_ieq(ext, ".tgx")) {
        if (!relic_pp_tmp_path(input, tmp_r, sizeof tmp_r)) {
            fprintf(stderr, "tigryx: cannot build temp path for '%s'\n", input);
            return 1;
        }
        char *pp_args[12]; int pp_argc = 0;
        pp_args[pp_argc++] = pp;
        if (g_verbose) pp_args[pp_argc++] = "--verbose";
        pp_forward_tier(pp_args, &pp_argc, extra, nextra);
        pp_args[pp_argc++] = (char*)input;
        pp_args[pp_argc++] = tmp_r;
        pp_args[pp_argc] = NULL;
        int rc = run_process(pp, pp_args, pp_argc, 0);
        if (rc != 0) {
#if defined(_WIN32)
            DeleteFileA(tmp_r);
#else
            remove(tmp_r);
#endif
            return rc;
        }
        actual_input = tmp_r;
        need_cleanup = 1;
    }

    /* tigryxri --input <file> --output-executable <file.exe> [extra...]
     * Default output is derived from the *user's* input path (not the temp
     * .tp), so the .exe lands next to the .tgx, not in %TEMP%. */
    char out_exe[1024];
    if (output)
        snprintf(out_exe, sizeof out_exe, "%s", output);
    else
        replace_ext(input, EXE_EXT[0] ? EXE_EXT : ".out", out_exe, sizeof out_exe);

    int base = 5;
    int total = base + nextra + 1 + (no_console ? 1 : 0);
    char **args = calloc((size_t)total, sizeof(char*));
    if (!args) { fprintf(stderr, "tigryx: out of memory\n"); return 1; }
    args[0] = ri;
    args[1] = "--input";
    args[2] = (char*)actual_input;
    args[3] = "--output-executable";
    args[4] = out_exe;
    for (int i = 0; i < nextra; i++) args[base + i] = extra[i];
    int n = base + nextra;
    if (no_console) args[n++] = "--no-console";
    args[n] = NULL;

    int rc = run_process(ri, args, n, 0);
    free(args);

#if defined(_WIN32)
    if (need_cleanup) DeleteFileA(tmp_r);
#else
    if (need_cleanup) remove(tmp_r);
#endif
    return rc;
}
#endif /* RELIC_COMMERCIAL || RELIC_MAKER */

#if defined(RELIC_COMMERCIAL)
/* True if a file exists at `path`. */
static int tool_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* tigryx build <file> [extra args] -- native compilation
 *
 * Pipeline:
 *   1. If the input is .tgx, run tigryxpp to preprocess it to a temp .tp.
 *   2. Invoke the native compiler, which emits a standalone exe.
 *
 * Two compilers are probed for: tigryxcc (direct .obj emission) and
 * tigryxrc (C-emitting transpiler). Where tigryxcc is present it wins.
 * The two take the same -o <path> convention; tigryxrc
 * additionally needs --run so it executes its generated build script rather
 * than leaving a .build.ps1 behind. */
static int cmd_build(const char *dir, const char *input, const char *output, char **extra, int nextra) {
    char cc_exe[512], rc_exe[512], pp[512], tmp_r[1024];
    sibling_path(dir, "tigryxcc", cc_exe, sizeof cc_exe);
    sibling_path(dir, "tigryxrc", rc_exe, sizeof rc_exe);
    sibling_path(dir, "tigryxpp",  pp,      sizeof pp);

    const int  use_cc = tool_exists(cc_exe);
    const char *tool  = use_cc ? cc_exe : rc_exe;

    const char *ext = file_ext(input);

    /* Bytecode input needs tigryxcc: it consumes the IRBuffer directly.
     * tigryxrc transpiles C from the AST, so it has nothing to feed on -
     * error out rather than let it lex a binary file as source. */
    if (!use_cc && (ext_ieq(ext, ".tlx") || ext_ieq(ext, ".b"))) {
        fprintf(stderr,
            "tigryx: build from bytecode (.tlx) requires tigryxcc;\n"
            "        tigryxrc compiles from source only\n");
        return 1;
    }

    const char *actual_input = input;
    int need_cleanup = 0;

    /* tigryxcc already does its own .tgx -> .tp preprocessing internally
     * (mirrors this exact block; see tigryxce.c), so pre-flattening here
     * would just be redundant -- and actively wrong for --pack-auto/
     * --assets, which need the *user's* project directory to resolve
     * relative asset paths against: pre-flattening replaces `input`
     * with a %TEMP% path before tigryxcc ever sees the real one, so
     * every relative asset path would resolve against %TEMP% instead
     * of the project. tigryxrc has no such internal handling, so it
     * still needs the pre-flatten step below. */
    if (!use_cc && ext_ieq(ext, ".tgx")) {
        if (!relic_pp_tmp_path(input, tmp_r, sizeof tmp_r)) {
            fprintf(stderr, "tigryx: cannot build temp path for '%s'\n", input);
            return 1;
        }
        char *pp_args[12]; int pp_argc = 0;
        pp_args[pp_argc++] = pp;
        if (g_verbose) pp_args[pp_argc++] = "--verbose";
        pp_forward_tier(pp_args, &pp_argc, extra, nextra);
        pp_args[pp_argc++] = (char*)input;
        pp_args[pp_argc++] = tmp_r;
        pp_args[pp_argc] = NULL;
        int rc = run_process(pp, pp_args, pp_argc, 0);
        if (rc != 0) {
#if defined(_WIN32)
            DeleteFileA(tmp_r);
#else
            remove(tmp_r);
#endif
            return rc;
        }
        actual_input = tmp_r;
        need_cleanup = 1;
    }

    /* <tool> <input.tp> -o <output.exe> [--run] [extra...]
     * --run tells tigryxrc to execute the generated build script after
     * emitting it, so 'tigryx build' produces the final .exe in one step
     * rather than leaving a .build.ps1 for the user to run manually;
     * tigryxcc links directly so it needs no such flag.
     * Default output is derived from the *user's* input path (not the temp
     * .tp), so the .exe lands next to the .tgx, not in %TEMP%. */
    char out_exe[1024];
    if (output)
        snprintf(out_exe, sizeof out_exe, "%s", output);
    else
        replace_ext(input, EXE_EXT[0] ? EXE_EXT : ".out", out_exe, sizeof out_exe);

    int base = use_cc ? 4 : 5;  /* exe, input, -o, output [, --run] */
    int total = base + nextra + 1;
    char **args = calloc((size_t)total, sizeof(char*));
    if (!args) { fprintf(stderr, "tigryx: out of memory\n"); return 1; }
    args[0] = (char*)tool;
    args[1] = (char*)actual_input;
    args[2] = "-o";
    args[3] = out_exe;
    if (!use_cc) args[4] = "--run";
    for (int i = 0; i < nextra; i++) args[base + i] = extra[i];
    args[base + nextra] = NULL;

    int rc2 = run_process(tool, args, base + nextra, 0);
    free(args);

#if defined(_WIN32)
    if (need_cleanup) DeleteFileA(tmp_r);
#else
    if (need_cleanup) remove(tmp_r);
#endif
    return rc2;
}
#endif /* RELIC_COMMERCIAL */

/* -------------------------------------------------------------------------
 * Asset packing: shared .tlx trailer read/write, and the `bc --pack-auto`
 * scanner.
 * ---------------------------------------------------------------------- */

/* Read an existing .tlx (or a plain .tlx with no trailer yet) fully, and
 * either parse its existing asset trailer (to append to) or hand back a
 * fresh empty RelicAssets. *file_buf_out is the whole file and
 * *bytecode_end_out is where the bytecode-only payload ends within it;
 * both are needed by pack_write_back() to rewrite the file. Returns NULL
 * on error (already reported to stderr). Caller frees *file_buf_out with
 * free() and the return value with relic_assets_free(). */
static RelicAssets *pack_read_existing(const char *tlx_path, const char *tool,
                                       u8 **file_buf_out, long *bytecode_end_out) {
    FILE *src = fopen(tlx_path, "rb");
    if(!src) {
        fprintf(stderr, "%s: cannot open '%s'\n", tool, tlx_path);
        return NULL;
    }
    fseek(src, 0, SEEK_END);
    long file_sz = ftell(src);
    rewind(src);

    u8 *file_buf = (u8*)malloc((size_t)file_sz);
    if(!file_buf || (long)fread(file_buf, 1, (size_t)file_sz, src) != file_sz) {
        fprintf(stderr, "%s: read error on '%s'\n", tool, tlx_path);
        fclose(src); free(file_buf);
        return NULL;
    }
    fclose(src);

    long bytecode_end = file_sz;
    RelicAssets *assets = NULL;
    if(file_sz >= 12) {
        /* Trailer fields are little-endian on disk (see tigryx.h). */
        u32 magic_check = relic_rd32le(file_buf + file_sz - 4);
        if(magic_check == RELIC_ASSET_MAGIC) {
            assets = relic_assets_load_mem(file_buf, (size_t)file_sz);
            if(assets) {
                u32 dir_offset = relic_rd32le(file_buf + file_sz - 12);
                bytecode_end = (long)dir_offset - (long)assets->data_size;
            } else {
                fprintf(stderr, "%s: failed to parse existing asset trailer (corrupt?). Cannot safely repack.\n", tool);
                fprintf(stderr, "%s: hint: recreate the .tlx from source, or manually remove the corrupted trailer\n", tool);
                free(file_buf);
                return NULL;
            }
        }
    }
    if(!assets) {
        assets = (RelicAssets*)calloc(1, sizeof(RelicAssets));
        if(!assets) { fprintf(stderr, "%s: out of memory\n", tool); free(file_buf); return NULL; }
    }
    *file_buf_out = file_buf;
    *bytecode_end_out = bytecode_end;
    return assets;
}

/* Rewrite tlx_path as file_buf[0..bytecode_end) followed by the asset
 * trailer for `assets`, atomically. Returns 1 on success. */
static int pack_write_back(const char *tlx_path, const char *tool,
                           const u8 *file_buf, long bytecode_end, RelicAssets *assets) {
    char tmp_path[1024];
    snprintf(tmp_path, sizeof tmp_path, "%s.pack_tmp", tlx_path);
    FILE *out = fopen(tmp_path, "wb");
    if(!out) {
        fprintf(stderr, "%s: cannot write temp file '%s'\n", tool, tmp_path);
        return 0;
    }
    if(fwrite(file_buf, 1, (size_t)bytecode_end, out) != (size_t)bytecode_end) {
        fprintf(stderr, "%s: write error\n", tool);
        fclose(out);
        return 0;
    }
    if(!relic_assets_write(assets, out)) {
        fprintf(stderr, "%s: failed to write asset trailer\n", tool);
        fclose(out);
        return 0;
    }
    fclose(out);
#if defined(_WIN32)
    if(!DeleteFileA(tlx_path)) {
        DWORD err = GetLastError();
        if(err != ERROR_FILE_NOT_FOUND) {
            fprintf(stderr, "%s: failed to delete original file '%s' (error %lu)\n", tool, tlx_path, err);
            fprintf(stderr, "%s: hint: file may be in use by another process\n", tool);
            return 0;
        }
    }
    if(!MoveFileA(tmp_path, tlx_path)) {
        DWORD err = GetLastError();
        fprintf(stderr, "%s: failed to rename temp file to '%s' (error %lu)\n", tool, tlx_path, err);
        return 0;
    }
#else
    if(rename(tmp_path, tlx_path) != 0) {
        fprintf(stderr, "%s: failed to rename temp file to '%s' (%s)\n", tool, tlx_path, strerror(errno));
        return 0;
    }
#endif
    return 1;
}

/* pack-auto scanner (PackAutoFile, pack_auto_run() and its helpers) now
 * lives in tigryx_pack_auto.h, shared with tigryxcc's tigryxce.c. */
#include "../../include/tigryx_pack_auto.h"

/* tigryx bc <file.tp|.tgx> [extra args] -- compile to .tlx bytecode */
static int cmd_bc(const char *dir, const char *input, const char *output, char **extra, int nextra, int pack_auto, int pack_fonts) {
    char ri[512], pp[512], tmp_r[1024];
    sibling_path(dir, "tigryxri", ri, sizeof ri);
    sibling_path(dir, "tigryxpp", pp, sizeof pp);

    const char *ext = file_ext(input);
    const char *actual_input = input;
    int need_cleanup = 0;

    if (ext_ieq(ext, ".tgx")) {
        if (!relic_pp_tmp_path(input, tmp_r, sizeof tmp_r)) {
            fprintf(stderr, "tigryx: cannot build temp path for '%s'\n", input);
            return 1;
        }
        char *pp_args[12]; int pp_argc = 0;
        pp_args[pp_argc++] = pp;
        if (g_verbose) pp_args[pp_argc++] = "--verbose";
        pp_forward_tier(pp_args, &pp_argc, extra, nextra);
        pp_args[pp_argc++] = (char*)input;
        pp_args[pp_argc++] = tmp_r;
        pp_args[pp_argc] = NULL;
        int rc = run_process(pp, pp_args, pp_argc, 0);
        if (rc != 0) {
#if defined(_WIN32)
            DeleteFileA(tmp_r);
#else
            remove(tmp_r);
#endif
            return rc;
        }
        actual_input = tmp_r;
        need_cleanup = 1;
    }

    char out_tlx[1024];
    if (output)
        snprintf(out_tlx, sizeof out_tlx, "%s", output);
    else
        replace_ext(input, ".tlx", out_tlx, sizeof out_tlx);

    int base = 5;
    int total = base + nextra + 1;
    char **args = calloc((size_t)total, sizeof(char*));
    if (!args) { fprintf(stderr, "tigryx: out of memory\n"); return 1; }
    args[0] = ri;
    args[1] = "--input";
    args[2] = (char*)actual_input;
    args[3] = "--output-bytecode";
    args[4] = out_tlx;
    for (int i = 0; i < nextra; i++) args[base + i] = extra[i];
    args[base + nextra] = NULL;

    int rc = run_process(ri, args, base + nextra, 0);
    free(args);

    if (rc == 0 && pack_auto) {
        /* Scan the flattened source (imports already resolved by tigryxpp
         * above) for asset references, then append whatever it found to
         * the .tlx just produced - additive to any earlier `tigryx pack`,
         * same as running that afterwards by hand. */
        char base_dir[1024];
        const char *slash = strrchr(input, '/');
#if defined(_WIN32) || defined(RELIC_DOS)
        const char *bslash = strrchr(input, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
        if (slash) snprintf(base_dir, sizeof base_dir, "%.*s", (int)(slash - input), input);
        else       snprintf(base_dir, sizeof base_dir, ".");

        u8 *file_buf = NULL;
        long bytecode_end = 0;
        RelicAssets *assets = pack_read_existing(out_tlx, "tigryx bc --pack-auto", &file_buf, &bytecode_end);
        if (assets) {
            int n = pack_auto_run(assets, actual_input, base_dir, dir, g_verbose,
                                  /*force_fonts=*/pack_fonts);
            if (n > 0) {
                if (!pack_write_back(out_tlx, "tigryx bc --pack-auto", file_buf, bytecode_end, assets)) rc = 1;
                else fprintf(stdout, "tigryx bc --pack-auto: packed %d file(s) into '%s'\n", n, out_tlx);
            } else {
                fprintf(stdout, "tigryx bc --pack-auto: found nothing to pack\n");
            }
            free(file_buf);
            relic_assets_free(assets);
        } else {
            rc = 1;
        }
    }

#if defined(_WIN32)
    if (need_cleanup) DeleteFileA(tmp_r);
#else
    if (need_cleanup) remove(tmp_r);
#endif
    return rc;
}

/* tigryx asm <file.tp|.tgx> [extra args] -- compile to .tla assembly listing */
static int cmd_asm(const char *dir, const char *input, const char *output, char **extra, int nextra) {
    char ri[512], pp[512], tmp_r[1024];
    sibling_path(dir, "tigryxri", ri, sizeof ri);
    sibling_path(dir, "tigryxpp", pp, sizeof pp);

    const char *ext = file_ext(input);
    const char *actual_input = input;
    int need_cleanup = 0;

    if (ext_ieq(ext, ".tgx")) {
        if (!relic_pp_tmp_path(input, tmp_r, sizeof tmp_r)) {
            fprintf(stderr, "tigryx: cannot build temp path for '%s'\n", input);
            return 1;
        }
        char *pp_args[12]; int pp_argc = 0;
        pp_args[pp_argc++] = pp;
        if (g_verbose) pp_args[pp_argc++] = "--verbose";
        pp_forward_tier(pp_args, &pp_argc, extra, nextra);
        pp_args[pp_argc++] = (char*)input;
        pp_args[pp_argc++] = tmp_r;
        pp_args[pp_argc] = NULL;
        int rc = run_process(pp, pp_args, pp_argc, 0);
        if (rc != 0) {
#if defined(_WIN32)
            DeleteFileA(tmp_r);
#else
            remove(tmp_r);
#endif
            return rc;
        }
        actual_input = tmp_r;
        need_cleanup = 1;
    }

    char out_rla[1024];
    if (output)
        snprintf(out_rla, sizeof out_rla, "%s", output);
    else
        replace_ext(input, ".tla", out_rla, sizeof out_rla);

    int base = 5;
    int total = base + nextra + 1;
    char **args = calloc((size_t)total, sizeof(char*));
    if (!args) { fprintf(stderr, "tigryx: out of memory\n"); return 1; }
    args[0] = ri;
    args[1] = "--input";
    args[2] = (char*)actual_input;
    args[3] = "--output-assembly";
    args[4] = out_rla;
    for (int i = 0; i < nextra; i++) args[base + i] = extra[i];
    args[base + nextra] = NULL;

    int rc = run_process(ri, args, base + nextra, 0);
    free(args);
#if defined(_WIN32)
    if (need_cleanup) DeleteFileA(tmp_r);
#else
    if (need_cleanup) remove(tmp_r);
#endif
    return rc;
}

/* tigryx pack <file.tlx> --assets <glob> [--assets <glob> ...] -- append assets to .tlx */
static int cmd_pack(const char *tlx_path, char **asset_globs, int nglobs) {
    if(nglobs == 0) {
        fprintf(stderr, "tigryx pack: no --assets patterns specified\n");
        return 1;
    }

    /* Read the existing .tlx fully so we can preserve the bytecode payload
     * and append to (rather than replace) any assets already packed into
     * it. Shared with `bc --pack-auto` - see pack_read_existing(). */
    u8 *file_buf = NULL;
    long bytecode_end = 0;
    RelicAssets *assets = pack_read_existing(tlx_path, "tigryx pack", &file_buf, &bytecode_end);
    if(!assets) return 1;

    int existing_count = assets->count;
    int total_added = 0;
    for(int i = 0; i < nglobs; i++) {
        int n = relic_assets_add_glob(assets, asset_globs[i]);
        if(n < 0) {
            fprintf(stderr, "tigryx pack: failed to expand glob '%s'\n", asset_globs[i]);
            free(file_buf); relic_assets_free(assets);
            return 1;
        }
        total_added += n;
    }

    if(total_added == 0) {
        fprintf(stderr, "tigryx pack: no files matched the given pattern(s)\n");
        free(file_buf); relic_assets_free(assets);
        return 1;
    }

    int assets_total = assets->count;
    int ok = pack_write_back(tlx_path, "tigryx pack", file_buf, bytecode_end, assets);
    free(file_buf);
    relic_assets_free(assets);
    if(!ok) return 1;

    if(existing_count > 0)
        fprintf(stdout, "tigryx pack: packed %d file(s) into '%s' (now %d asset(s) total)\n",
                total_added, tlx_path, assets_total);
    else
        fprintf(stdout, "tigryx pack: packed %d file(s) into '%s'\n", total_added, tlx_path);
    return 0;
}

/* tigryx pp <file.tgx> [extra args] -- preprocess only */
static int cmd_pp(const char *dir, const char *input, const char *output, char **extra, int nextra) {
    char pp[512];
    sibling_path(dir, "tigryxpp", pp, sizeof pp);

    char out_r[1024];
    if (output)
        snprintf(out_r, sizeof out_r, "%s", output);
    else
        replace_ext(input, ".tp", out_r, sizeof out_r);

    int base = 3 + (g_verbose ? 1 : 0);
    int total = base + nextra + 1;
    char **args = calloc((size_t)total, sizeof(char*));
    if (!args) { fprintf(stderr, "tigryx: out of memory\n"); return 1; }
    int ai = 0;
    args[ai++] = pp;
    if (g_verbose) args[ai++] = "--verbose";
    args[ai++] = (char*)input;
    args[ai++] = out_r;
    for (int i = 0; i < nextra; i++) args[base + i] = extra[i];
    args[base + nextra] = NULL;

    int rc = run_process(pp, args, base + nextra, 0);
    free(args);
    return rc;
}

/* -------------------------------------------------------------------------
 * Usage
 * ---------------------------------------------------------------------- */
static void print_usage(void) {
    relic_console_ensure();
    fprintf(stderr,
        "\n"
        RELIC_COPYRIGHT "\n"
#if !defined(RELIC_COMMERCIAL) && !defined(RELIC_MAKER)
        "\n"
        "This is free software licensed under the GNU GPL v3. It comes with\n"
        "ABSOLUTELY NO WARRANTY; for details see COPYING or <https://www.gnu.org/licenses/>.\n"
        "You are welcome to redistribute it under certain conditions.\n"
#else
        "\n"
        "Commercially licensed software. See COMMERCIAL.md for licence terms.\n"
#endif
        "\n"
        "BETA SOFTWARE - this is a beta release. There will be bugs, and the\n"
        "API and behaviour may change between releases without notice.\n"
        "\n"
        "Version:  tigryx "
#if defined(RELIC_COMMERCIAL)
        "pro "
#elif defined(RELIC_MAKER)
        "maker "
#endif
        RELIC_VERSION " BETA\n"
        "Platform: " RELIC_PLATFORM_ID "\n"
        "Backend:  " RELIC_BACKEND_ID "\n"
        "\n"
        "Usage:\n"
        "  tigryx run   <file.tp|.tgx|.tla|.tlx>            [args]  interpret or run\n"
        "  tigryx bc    <file.tp|.tgx> [-o <out.tlx>]       [args]  compile to .tlx bytecode\n"
        "  tigryx asm   <file.tp|.tgx> [-o <out.tla>]       [args]  compile to .tla assembly\n"
#if defined(RELIC_COMMERCIAL) || defined(RELIC_MAKER)
        "  tigryx bind  <file.tp|.tgx|.tlx> [-o <out.exe>]  [args]  produce bound executable\n"
#endif
#if defined(RELIC_COMMERCIAL)
        "  tigryx build <file.tp|.tgx|.tlx> [-o <out.exe>]  [args]  produce native executable\n"
#endif
        "  tigryx pp    <file.tgx>    [-o <out.tp>]         [args]  preprocess .tgx to .tp\n"
        "  tigryx pack  <file.tlx>   --assets <glob> ...          embed assets into .tlx\n"
        "  tigryx version                                         print version\n"
        "\n"
        "Options (forwarded to the underlying tool where applicable):\n"
        "  -o / --output-file <path>  override the default output filename\n"
        "  --verbose                  enable verbose/diagnostic output\n"
#if defined(RELIC_COMMERCIAL) || defined(RELIC_MAKER)
        "  --heap <size>              set VM heap size, e.g. 64M, 256M (run/bind/bc/asm)\n"
        "  --no-console               (run) hide the console on an Explorer/shortcut\n"
        "                             launch, e.g. from a .tgx/.tlx file association;\n"
        "                             (bind) bake the same behaviour into the bound .exe\n"
#else
        "  --heap <size>              set VM heap size, e.g. 64M, 256M (run/bc/asm)\n"
        "  --no-console               (run) hide the console on an Explorer/shortcut\n"
        "                             launch, e.g. from a .tgx/.tlx file association\n"
#endif
#if defined(RELIC_COMMERCIAL)
        "  --assets <glob>            (pack/bc/build) add files matching glob to the\n"
        "                             asset table; may be given multiple times\n"
        "  --pack-auto                (bc/build) best-effort: scan the source for asset\n"
#else
        "  --assets <glob>            (pack/bc) add files matching glob to the\n"
        "                             asset table; may be given multiple times\n"
        "  --pack-auto                (bc) best-effort: scan the source for asset\n"
#endif
        "                             paths (@file_open/@file_load_png literals, @text/\n"
        "                             @text_measure font names, .rlm texture tables,\n"
        "                             #pragma arg assets) and pack whatever is found;\n"
        "                             additive to --assets / a later `tigryx pack`\n"
#if defined(RELIC_COMMERCIAL)
        "  --pack-fonts               (bc/build) implies --pack-auto, and packs EVERY\n"
#else
        "  --pack-fonts               (bc) implies --pack-auto, and packs EVERY\n"
#endif
        "                             font found instead of only the faces named in the\n"
        "                             source (same as `#pragma arg pack_fonts`); needed\n"
        "                             when faces are selected at runtime (e.g. ui.font)\n"
        "                             and can't be resolved by the source scan\n"
#if defined(RELIC_COMMERCIAL)
        "  --upx                      (build) compress the produced .exe with UPX\n"
        "                             (off by default)\n"
        "  --cpu sse2|sse4.1|avx2     (build) target instruction-set level for the\n"
        "                             produced .exe (default sse2 = x64 baseline)\n"
#endif
        "  --tier low|medium|high|super\n"
        "                             Adaptive Build tier: forwarded to tigryxpp\n"
        "                             (TGX_TIER_* defines) and tigryxri (the .tlx\n"
        "                             stamp / mismatch warning). Defaults to this\n"
        "                             platform's detected tier; TGX_TIER env also\n"
        "                             works and is inherited by every tool.\n"
#if defined(RELIC_DOS)
        "  --colour-depth <N>         DOS only: pin screen colour depth (8|16|24|32)\n"
        "  --res <WxH>                DOS only: physical mode for WINFLAG_STRETCH windows\n"
        "  --skip-clear               DOS only: skip the per-frame colour clear (use only when scene fills screen)\n"
        "  --proxy <url>             DOS only: HTTP proxy (routes HTTPS via TLS-terminating proxy)\n"
#endif
        "\n"
    );
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */
int main(int argc, char **argv) {
    /* GUI-subsystem binary: attach to parent's console if launched from a
     * shell. On Explorer launches this fails silently (no console at all).
     * Must be the very first thing — before any I/O. */
    relic_console_init();

    /* Recognize --no-console wherever it appears and strip it out of argv
     * before any other parsing, tracking it in `no_console`. It requests
     * that the transient console Windows creates on an Explorer/file-
     * association/shortcut launch (see tigryx.h) be hidden. For `run`, that
     * means our own console, handled directly below. For `bind` it instead
     * gets re-attached to the args forwarded to tigryxri (see cmd_bind),
     * which bakes it into the produced .exe's footer so *that* program
     * hides its own console the same way when double-clicked. */
    int no_console = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-console") == 0) {
            no_console = 1;
            for (int j = i; j < argc - 1; j++) argv[j] = argv[j + 1];
            argc--;
            i--;
        }
    }

    /* Capture argv[0] for DJGPP executable path resolution */
    if (argc > 0 && argv[0]) {
        snprintf(g_argv0, sizeof g_argv0, "%s", argv[0]);
    }

    if (argc < 2) {
        relic_console_attach(no_console);
        print_usage();
        return 1;
    }

    const char *sub = argv[1];

    /* Only `run` hides its own console here; `bind` is a CLI operation that
     * forwards the flag into the produced executable instead (see below). */
    relic_console_attach(no_console && strcmp(sub, "run") == 0);

    if (strcmp(sub, "version") == 0 || strcmp(sub, "--version") == 0) {
        relic_console_ensure();
        printf("\n"
               RELIC_COPYRIGHT "\n"
#if !defined(RELIC_COMMERCIAL) && !defined(RELIC_MAKER)
               "\n"
               "This is free software licensed under the GNU GPL v3. It comes with\n"
               "ABSOLUTELY NO WARRANTY; for details see COPYING or <https://www.gnu.org/licenses/>.\n"
               "You are welcome to redistribute it under certain conditions.\n"
#else
               "\n"
               "Commercially licensed software. See COMMERCIAL.md for licence terms.\n"
#endif
               "\n"
               "BETA SOFTWARE - this is a beta release. There will be bugs, and the\n"
               "API and behaviour may change between releases without notice.\n"
               "\n"
               "Version:  tigryx "
#if defined(RELIC_COMMERCIAL)
               "pro "
#elif defined(RELIC_MAKER)
               "maker "
#endif
               RELIC_VERSION " BETA\n"
               "Platform: " RELIC_PLATFORM_ID "\n"
               "Backend:  " RELIC_BACKEND_ID "\n"
               "\n");
        return 0;
    }

    if (strcmp(sub, "--help") == 0 || strcmp(sub, "-h") == 0) {
        print_usage();
        return 0;
    }

    /* All other subcommands need at least a file argument */
    if (argc < 3) {
        relic_console_ensure();
        fprintf(stderr, "tigryx: '%s' requires a file argument\n", sub);
        print_usage();
        return 1;
    }

    char dir[2048] = {0};
    if (!self_dir(dir, sizeof dir)) {
        relic_console_ensure();
        fprintf(stderr, "tigryx: could not determine own directory\n");
        return 1;
    }
    const char *input = argv[2];

    /* Split the post-<file> tokens into two groups:
     *   - tigryx flags: recognized options that configure the toolchain.
     *     Consumed here (-o) or kept in extra[] and forwarded to the spawned
     *     tool (tigryxri / tigryxvm / tigryxrc).
     *   - user args:   everything the program itself should see via @args.
     *
     * The boundary between the two is the FIRST token that is not a
     * recognized tigryx flag, or an explicit "--" separator (which is itself
     * consumed). Recognized tigryx flags must therefore precede any user
     * args; "--" is the escape hatch when a user arg would otherwise look
     * like a tigryx flag. Only `run` forwards the user args (to tigryxri for
     * immediate execution); bind/bc/build/asm produce artifacts and the
     * passthrough happens when the resulting program is later run. */
    const char *output = NULL;
    int pack_auto = 0;
    int pack_fonts = 0;
    char **extra = argv + 3;
    int nextra = argc - 3;
    char **user_argv = NULL;
    int    user_argc = 0;
    for (int i = 0; i < nextra; i++) {
        if (strcmp(extra[i], "--") == 0) {
            user_argv = extra + i + 1;
            user_argc = nextra - i - 1;
            nextra = i;                 /* drop "--" and the rest from extra[] */
            break;
        } else if (strcmp(extra[i], "--output-file") == 0 || strcmp(extra[i], "-o") == 0) {
            if (i + 1 >= nextra) {
                fprintf(stderr, "tigryx: %s requires a filename\n", extra[i]);
                return 1;
            }
            output = extra[i + 1];
            /* Remove the two entries from extra by shifting. */
            for (int j = i; j < nextra - 2; j++) extra[j] = extra[j + 2];
            nextra -= 2;
            i--;  /* recheck this index */
        } else if ((strcmp(extra[i], "--pack-auto") == 0 ||
                    strcmp(extra[i], "--pack-fonts") == 0) &&
                   strcmp(sub, "bc") == 0) {
            /* `bc`-only; not a tigryxri flag, so it must NOT be forwarded
             * in extra[] like --heap/--assets are. Remove the one entry.
             * --pack-fonts implies --pack-auto and additionally packs every
             * font found (same as `#pragma arg pack_fonts`).
             * (For `build`, both flags ARE tigryxcc flags and fall
             * through to the sub=="build" branch below instead, which
             * keeps them in extra[] for the compiler to parse itself.) */
            if (strcmp(extra[i], "--pack-fonts") == 0) pack_fonts = 1;
            pack_auto = 1;
            for (int j = i; j < nextra - 1; j++) extra[j] = extra[j + 1];
            nextra -= 1;
            i--;  /* recheck this index */
        } else if (strcmp(extra[i], "--heap") == 0) {
            if (i + 1 >= nextra) {
                fprintf(stderr, "tigryx: --heap requires a size argument (e.g. 64M)\n");
                return 1;
            }
            i++;  /* skip the value; both flag and value stay in extra[] */
        } else if (strcmp(extra[i], "--tier") == 0) {
            /* Adaptive Build tier. Validated here for a fast, clear error;
             * both tokens stay in extra[] so the spawned tool sees them
             * (tigryxri: .tlx stamp + mismatch reference; tigryxpp: via
             * pp_forward_tier; tigryxcc: its own --tier parsing). */
            if (i + 1 >= nextra) {
                fprintf(stderr, "tigryx: --tier requires a value (low|medium|high|super)\n");
                return 1;
            }
            if (relic_tier_parse(extra[i + 1]) < 0) {
                fprintf(stderr, "tigryx: --tier: unknown tier '%s' "
                                "(low|medium|high|super)\n", extra[i + 1]);
                return 1;
            }
            i++;
        } else if (strcmp(extra[i], "--platform") == 0 ||
                   strcmp(extra[i], "--backend")  == 0) {
            /* Adaptive Build target identity for cross-compilation
             * (e.g. building BBC bytecode on a Windows host). No value
             * validation here - the platform-id space is open; both
             * tokens stay in extra[] (tigryxpp via pp_forward_tier;
             * tigryxri/tigryxcc parse it themselves). */
            if (i + 1 >= nextra) {
                fprintf(stderr, "tigryx: %s requires a platform/backend id\n", extra[i]);
                return 1;
            }
            i++;
        } else if (strcmp(extra[i], "--assets") == 0) {
            if (i + 1 >= nextra) {
                fprintf(stderr, "tigryx: --assets requires a glob pattern\n");
                return 1;
            }
            i++;  /* skip the value; both flag and value stay in extra[] */
        } else if (strcmp(extra[i], "--verbose") == 0) {
            g_verbose = 1;
            /* No value; kept in extra[] and parsed again by the spawned tool. */
        } else if (strcmp(extra[i], "--jit")        == 0 ||
                   strcmp(extra[i], "--no-jit")     == 0 ||
                   strcmp(extra[i], "--debug")      == 0 ||
                   strcmp(extra[i], "--native-class") == 0 ||
                   strcmp(extra[i], "--debug-info") == 0) {
            /* No value; kept in extra[] and parsed again by the spawned tool. */
        } else if (strcmp(sub, "build") == 0 &&
                   (strcmp(extra[i], "--console")    == 0 ||
                    strcmp(extra[i], "--windowed")   == 0 ||
                    strcmp(extra[i], "--upx")        == 0 ||
                    strcmp(extra[i], "--no-upx")     == 0 ||
                    strcmp(extra[i], "--no-link")    == 0 ||
                    strcmp(extra[i], "--pack-auto")  == 0 ||
                    strcmp(extra[i], "--pack-fonts") == 0 ||
                    strcmp(extra[i], "--app")        == 0 ||
                    strcmp(extra[i], "--bundle")     == 0 ||
                    strcmp(extra[i], "--exe")        == 0 ||
                    strcmp(extra[i], "--no-app")     == 0)) {
            /* tigryxcc flags: no value; forwarded to the compiler. Gated on
             * 'build' so 'run' still treats them as user args. */
        } else if (strcmp(sub, "build") == 0 &&
                   (strcmp(extra[i], "--target-os") == 0 ||
                    strcmp(extra[i], "--icon")      == 0)) {
            /* tigryxcc flags with a value; both stay in extra[]. */
            if (i + 1 >= nextra) {
                fprintf(stderr, "tigryx: %s requires a value\n", extra[i]);
                return 1;
            }
            i++;
        } else if (strcmp(sub, "build") == 0 &&
                   strcmp(extra[i], "--cpu") == 0) {
            /* tigryxcc flag with a value; both stay in extra[]. The
             * compiler validates the level itself. */
            if (i + 1 >= nextra) {
                fprintf(stderr, "tigryx: --cpu requires sse2|sse4.1|avx2\n");
                return 1;
            }
            i++;
#if defined(RELIC_DOS)
        } else if (strcmp(extra[i], "--colour-depth") == 0 ||
                   strcmp(extra[i], "--color-depth")  == 0) {
            /* DOS-only: validate here so we fail fast before spawning
             * tigryxri / tigryxvm. The flag stays in extra[] and the
             * underlying tool parses it again (where it actually sets
             * g_relic_preferred_color_depth before backend init).
             * DJGPP-gated -- no other platform sees this knob. */
            if (i + 1 >= nextra) {
                fprintf(stderr, "tigryx: --colour-depth requires a value (8|16|24|32)\n");
                return 1;
            }
            if (relic_parse_colour_depth(extra[i + 1]) == 0) {
                fprintf(stderr, "tigryx: --colour-depth must be 8, 16, 24, or 32\n");
                return 1;
            }
            i++;
        } else if (strcmp(extra[i], "--skip-clear") == 0) {
            /* DOS+TinyGL only: skip per-frame GL colour-buffer clear.
             * No value; flag stays in extra[] and the spawned tool
             * (tigryxri / tigryxvm) sets g_relic_skip_clear itself. */
        } else if (strcmp(extra[i], "--proxy") == 0) {
            /* DOS-only: HTTP proxy (plain HTTP can't do HTTPS).
             * Takes a URL value; both flag and value stay in extra[] and are
             * parsed by the spawned tool (tigryxri / tigryxvm). */
            if (i + 1 >= nextra) {
                fprintf(stderr, "tigryx: --proxy requires a URL argument (e.g. http://192.168.1.50:8080)\n");
                return 1;
            }
            i++;
#endif
        } else {
            /* First non-tigryx token: everything from here is user args. */
            user_argv = extra + i;
            user_argc = nextra - i;
            nextra = i;
            break;
        }
    }

    if ((pack_auto || pack_fonts) && strcmp(sub, "bc") != 0) {
        fprintf(stderr, "tigryx: --pack-auto/--pack-fonts are only supported by 'tigryx bc'\n");
        return 1;
    }

    if (strcmp(sub, "run") == 0)   return cmd_run(dir, input, output, extra, nextra, user_argv, user_argc, no_console);
    if (strcmp(sub, "bc") == 0)    return cmd_bc(dir, input, output, extra, nextra, pack_auto, pack_fonts);
    if (strcmp(sub, "asm") == 0)   return cmd_asm(dir, input, output, extra, nextra);
#if defined(RELIC_COMMERCIAL) || defined(RELIC_MAKER)
    if (strcmp(sub, "bind") == 0)  return cmd_bind(dir, input, output, extra, nextra, no_console);
#else
    if (strcmp(sub, "bind") == 0) {
        fprintf(stderr,
            "tigryx: 'bind' is only available in the commercial or maker edition of Tigryx.\n"
            "       The open-source edition supports: run, bc, asm, pp, pack.\n"
            "       Use 'tigryx bc <file>' to produce a portable .tlx instead.\n");
        return 1;
    }
#endif
#if defined(RELIC_COMMERCIAL)
    if (strcmp(sub, "build") == 0) return cmd_build(dir, input, output, extra, nextra);
#else
    if (strcmp(sub, "build") == 0) {
        fprintf(stderr,
            "tigryx: 'build' is only available in the commercial edition of Tigryx.\n"
            "       The maker edition supports: run, bc, asm, bind, pp, pack.\n"
            "       Use 'tigryx bind <file>' to produce a bound executable instead.\n");
        return 1;
    }
#endif
    if (strcmp(sub, "pp") == 0)    return cmd_pp(dir, input, output, extra, nextra);

    if (strcmp(sub, "pack") == 0) {
        /* tigryx pack <file.tlx> --assets <glob> [--assets <glob> ...] */
        char *globs[64];
        int nglobs = 0;
        for (int i = 0; i < nextra - 1; i++) {
            if (strcmp(extra[i], "--assets") == 0) {
                if (nglobs < 64) globs[nglobs++] = extra[i + 1];
                i++;
            }
        }
        return cmd_pack(input, globs, nglobs);
    }

    relic_console_ensure();
    fprintf(stderr, "tigryx: unknown subcommand '%s'\n", sub);
    print_usage();
    return 1;
}
