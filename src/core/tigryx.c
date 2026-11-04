/*
 * tigryx.c
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
 * tigryx.c - Relic VM runner (relic-vm)
 *
 * Usage:
 *   relic-vm [--verbose] [--jit] [--heap <size>] [--no-console] <program.tlx>
 *
 * Bound mode: if this executable has been bound to a .tlx via `relic bind`,
 * the bytecode is appended to the .exe and a 32-byte footer at the very end
 * tells us where to find it. In that case the runtime ignores any path on the
 * command line (but still honors --verbose / --jit flags) and runs the
 * embedded program. See try_load_embedded() below. `relic bind --no-console`
 * also bakes a console-visibility flag into that footer - see tigryx.h.
 *
 * NOTE: End users should use `relic run` rather than invoking relic-vm directly.
 */
#define _GNU_SOURCE
#include "tigryx.h"
#include "tigryx_tier.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define RELIC_COPYRIGHT "Copyright (c) 2026 Scintillae Limited"
#ifndef RELIC_PLATFORM_ID
#  define RELIC_PLATFORM_ID "unknown"
#endif
#ifndef RELIC_BACKEND_ID
#  define RELIC_BACKEND_ID "unknown"
#endif

#if defined(__EMSCRIPTEN__)
   /* No /proc/self/exe in MEMFS; we never bind payloads in the wasm build. */
#elif defined(_WIN32)
#  include <windows.h>
#elif defined(RELIC_DOS)
#  include <dos.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#elif defined(__linux__) || defined(__sun)
#  include <unistd.h>
#endif

/* Footer appended by `relic bind` (via relic-ri --output-executable) to a copy of this executable.
 * Layout is 32 bytes at the very end of the file:
 *      char     magic[8] = "RLCBIND\x02"
 *      uint64_t payload_offset   (LE, byte offset within the .exe)
 *      uint64_t payload_size     (LE, length of the embedded .b)
 *      uint32_t heap_bytes       (LE, 0 = use compiled-in default)
 *      uint32_t flags            (LE, bit 0 = RELIC_BIND_FLAG_NO_CONSOLE)
 * The magic's last byte is a version tag so future format bumps can stay
 * backward-detectable. */
/* RELIC_BIND_MAGIC, _MAGIC_SIZE, _FOOTER_SIZE are defined in tigryx.h */

static int g_verbose = 0;

static void fatal(const char *title, const char *msg) {
    /* A fatal error must be visible even if the console was hidden as an
     * Explorer launch (see tigryx.h). */
    relic_console_ensure();
    fprintf(stderr, "%s: %s\n", title, msg);
    fflush(stderr);
}

#define vlog(...) do { if(g_verbose) { printf(__VA_ARGS__); fflush(stdout); } } while(0)

/* Provided by whichever backend library is linked into this build
 * (see src/backends/<name>/). Every backend exports exactly one symbol
 * with this name so the core does not need to know about specific ports. */
extern RelicBackend relic_backend;

/* Provided by whichever JIT library is linked. The "null" JIT's compile()
 * returns false, so --jit silently falls back to the interpreter when no
 * real JIT is built for this platform. */
extern RelicJit relic_jit;

/* ------------------------------------------------------------------
 * Self-executable path lookup (best-effort, returns false on failure).
 * ------------------------------------------------------------------ */
static int get_self_path(char *buf, size_t buflen) {
#if defined(__EMSCRIPTEN__)
    (void)buf; (void)buflen;
    return 0;   /* Bound-payload mode is not supported in the wasm build. */
#elif defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)buflen);
    if(n == 0 || n >= buflen) return 0;
    buf[n] = '\0';
    return 1;
#elif defined(RELIC_IA16)
    /* DOS 3.0+: the fully-qualified program path sits just past the end of
     * the environment block. envseg = word at PSP:2Ch; the env is a run of
     * ASCIZ strings ended by a NUL byte, then a u16 count, then the ASCIZ
     * program path. */
    {
        extern unsigned _psp;
        u16 envseg = *(const u16 RELIC_FAR *)MK_FP(_psp, 0x2C);
        const char RELIC_FAR *e = (const char RELIC_FAR *)MK_FP(envseg, 0);
        u32 off = 0;
        /* Skip env strings: each is NUL-terminated, the block ends with an
         * extra NUL. Guard against a corrupt env by capping the scan. */
        for(;;) {
            char c = *e;
            e = (const char RELIC_FAR *)relic_cfp_add(e, 1);
            if(++off > 0x8000u) return 0;   /* env can't exceed ~32K */
            if(c) continue;
            if(*e == 0) { e = (const char RELIC_FAR *)relic_cfp_add(e, 1); break; }
        }
        e = (const char RELIC_FAR *)relic_cfp_add(e, 2);   /* skip count word */
        size_t i = 0;
        while(i + 1 < buflen) {
            char c = *e;
            e = (const char RELIC_FAR *)relic_cfp_add(e, 1);
            buf[i++] = c;
            if(!c) break;
        }
        buf[buflen - 1] = '\0';
        return buf[0] != '\0';
    }
#elif defined(RELIC_DOS)
    /* DJGPP/DOS: self-path resolution not supported */
    (void)buf; (void)buflen;
    return 0;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)buflen;
    if(_NSGetExecutablePath(buf, &size) != 0) return 0;
    return 1;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, buflen - 1);
    if(n <= 0) return 0;
    buf[n] = '\0';
    return 1;
#elif defined(__sun)
    /* Solaris: /proc/self/path/a.out needs Solaris 8+; getexecname()
     * + realpath() covers Solaris 7 (see relic_sun_exepath). */
    return relic_sun_exepath(buf, buflen) ? 1 : 0;
#else
    (void)buf; (void)buflen;
    return 0;
#endif
}

/* Try to read an embedded .b payload from the tail of our own executable.
 * On success returns a buffer of size *out_size; caller frees with
 * relic_far_free (ia16: the payload can exceed the near heap, so it is
 * allocated in far memory; flat hosts: plain malloc). Returns NULL if no
 * bound payload is present (this is the normal case for a freestanding
 * `relic.exe`). */
static void RELIC_FAR *try_load_embedded(size_t *out_size, u32 *out_heap_bytes, u32 *out_flags) {
    char self_path[2048];
    if(!get_self_path(self_path, sizeof self_path)) return NULL;

    FILE *f = fopen(self_path, "rb");
    if(!f) return NULL;

    if(fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long file_size = ftell(f);
    if(file_size < (long)RELIC_BIND_FOOTER_SIZE) { fclose(f); return NULL; }

    if(fseek(f, file_size - RELIC_BIND_FOOTER_SIZE, SEEK_SET) != 0) {
        fclose(f); return NULL;
    }
    uint8_t footer[RELIC_BIND_FOOTER_SIZE];
    if(fread(footer, 1, RELIC_BIND_FOOTER_SIZE, f) != RELIC_BIND_FOOTER_SIZE) {
        fclose(f); return NULL;
    }
    if(memcmp(footer, RELIC_BIND_MAGIC, RELIC_BIND_MAGIC_SIZE) != 0) {
        fclose(f); return NULL;
    }

    /* Footer fields are little-endian on disk (see tigryx.h). */
    uint64_t payload_off = relic_rd64le(footer + 8);
    uint64_t payload_size = relic_rd64le(footer + 16);
    uint32_t heap_bytes = relic_rd32le(footer + 24);
    uint32_t flags = relic_rd32le(footer + 28);

    if(payload_off >= (uint64_t)file_size ||
       payload_size == 0 ||
       payload_off + payload_size > (uint64_t)file_size - RELIC_BIND_FOOTER_SIZE) {
        fclose(f); return NULL;
    }

    if(fseek(f, (long)payload_off, SEEK_SET) != 0) { fclose(f); return NULL; }
    void RELIC_FAR *data = relic_far_alloc(payload_size);
    if(!data) { fclose(f); return NULL; }
    if(relic_fread_far(f, (u8 RELIC_FAR *)data, (u32)payload_size) != (size_t)payload_size) {
        relic_far_free(data); fclose(f); return NULL;
    }
    fclose(f);

    *out_size = (size_t)payload_size;
    *out_heap_bytes = heap_bytes;
    *out_flags = flags;
    return data;
}

int main(int argc, char **argv) {
    /* GUI-subsystem binary: attach to parent's console if launched from a
     * shell. On Explorer launches this fails silently (no console at all).
     * Must be the very first thing — before any I/O. */
    relic_console_init();

#if defined(RELIC_RISCOS)
    /* Disarm FPA trap enables before any guest float op runs: SCL's
     * default FP context would otherwise kill the task on 1.0/0.0. */
    relic_fpe_untrap();
#endif

    const char *bytecode_path = NULL;
    u32 mem_sz = RELIC_MEM_SIZE;
    int heap_given = 0;   /* an explicit --heap was passed */
    int jit_mode = (strcmp(relic_jit.name, "null") != 0);  /* Auto-enable if available */
    const char *proxy_url = NULL;
    const char *tier_cli = NULL;

    /* --help / -h: print usage and exit before any console attach logic. */
    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            relic_console_ensure();
            fprintf(stderr,
                "\n"
                RELIC_COPYRIGHT "\n"
                "\n"
                "BETA SOFTWARE - this is a beta release. There will be bugs, and the\n"
                "API and behaviour may change between releases without notice.\n"
                "\n"
                "Version:  tigryxvm " RELIC_VERSION " BETA\n"
                "Platform: " RELIC_PLATFORM_ID "\n"
                "Backend:  " RELIC_BACKEND_ID "\n"
                "\n"
                "Usage:\n"
                "  tigryxvm [options] <program.tlx>          run a bytecode file\n"
                "  tigryxvm [options] -- <args...>           pass args after -- to program\n"
                "  tigryxvm --version                        print version\n"
                "\n"
                "Options:\n"
                "  --verbose                  enable verbose/diagnostic output\n"
                "  --jit                      force enable JIT (auto-enabled if available)\n"
                "  --no-jit                   disable JIT, use interpreter\n"
                "  --heap <size>              set VM heap size, e.g. 64M, 256M\n"
                "  --tier low|medium|high|super\n"
                "                             Adaptive Build: reference tier for the\n"
                "                             bytecode-mismatch warning\n"
                "  --platform <id>            Adaptive Build: target platform identity\n"
                "  --backend <id>             Adaptive Build: target backend identity\n"
                "  --no-console               hide the console on an Explorer/shortcut launch\n"
#if defined(RELIC_DOS)
                "  --colour-depth <N>         DOS+TinyGL only: pin screen colour depth (8|16|24|32)\n"
                "  --res <WxH>                DOS only: physical mode for WINFLAG_STRETCH windows\n"
                "  --skip-clear               DOS+TinyGL only: skip per-frame GL colour clear\n"
#endif
#if defined(RELIC_RISCOS)
                "  --fullscreen               RISC OS only: force @window fullscreen mode\n"
#endif
#if !defined(_WIN32)
                "  --proxy <url>              HTTP proxy for plain-HTTP backends (DOS; non-curl POSIX)\n"
#endif
                "  --version                  print version and exit\n"
                "\n"
                "  Bound executables (produced by 'relic bind') carry their bytecode\n"
                "  inside and take no <program.tlx> argument; any non-flag token is\n"
                "  passed through to the program via @args.\n"
            );
            return 0;
        }
    }

    /* Detect an embedded payload up front (see try_load_embedded below): a
     * bound exe (`relic bind`) carries its bytecode inside itself and takes
     * no bytecode-path argument, so on such a binary any non-flag token is a
     * user passthrough arg rather than a path. */
    size_t embedded_size = 0;
    u32    embedded_heap = 0;
    u32    embedded_flags = 0;
    void  RELIC_FAR *embedded_data = try_load_embedded(&embedded_size, &embedded_heap, &embedded_flags);

    /* Use the heap size baked into the bound exe as the default, unless the
     * user overrides it with --heap on the command line. */
    if(embedded_data && embedded_heap > 0) {
        mem_sz = embedded_heap;
        vlog("[tigryxvm] baked heap size: %u MB\n", embedded_heap / (1024*1024));
    }

    /* See tigryx.h: a bound exe hides its own console on an Explorer launch
     * only if `tigryx bind --no-console` baked that in (or --no-console is
     * passed directly, for dev use running tigryxvm unbound); fatal()/
     * @console un-hide it. Decided here, once, rather than at the very top
     * of main(), so the embedded flag (which needs try_load_embedded above)
     * and an explicit --no-console on the command line both take effect. */
    int no_console = (embedded_flags & RELIC_BIND_FLAG_NO_CONSOLE) != 0;
    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--") == 0) break;
        if(strcmp(argv[i], "--no-console") == 0) { no_console = 1; break; }
    }
    relic_console_attach(no_console);

    /* User passthrough args exposed to the program via @args (0-indexed, no
     * program name, no relic flags). Begins at an explicit "--" separator, or
     * at the first token that is neither a recognized relic-vm flag nor the
     * (single) bytecode-path positional. */
    char **user_argv = NULL;
    int    user_argc = 0;

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--") == 0) {
            user_argv = &argv[i + 1];
            user_argc = argc - (i + 1);
            break;
        }
        if(strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if(strcmp(argv[i], "--jit") == 0) {
            jit_mode = 1;
        } else if(strcmp(argv[i], "--no-jit") == 0) {
            jit_mode = 0;
        } else if(strcmp(argv[i], "--tier") == 0) {
            /* Adaptive Build: reference tier for the bytecode-mismatch
             * warning. Deferred to after the loop so a later --platform
             * already set the ceiling it validates against. */
            if(i + 1 < argc) { tier_cli = argv[++i]; }
        } else if(strcmp(argv[i], "--platform") == 0) {
            if(i + 1 < argc) { relic_tier_set_platform(argv[++i], relic_tier_backend()); }
        } else if(strcmp(argv[i], "--backend") == 0) {
            if(i + 1 < argc) { relic_tier_set_platform(relic_tier_platform(), argv[++i]); }
        } else if(strcmp(argv[i], "--heap") == 0) {
            if(i + 1 < argc) { mem_sz = relic_parse_mem_size(argv[++i]); heap_given = 1; }
            else { fprintf(stderr, "error: --heap requires a size argument\n"); return 1; }
        } else if(strcmp(argv[i], "--no-console") == 0) {
            /* Already applied above (relic_console_attach was already
             * called); just consume it so it isn't mistaken for the
             * bytecode-path positional or the start of user args. */
#if defined(RELIC_DOS)
        } else if(strcmp(argv[i], "--colour-depth") == 0 ||
                  strcmp(argv[i], "--color-depth")  == 0) {
            /* DOS+TinyGL only: pin screen colour depth to 8/16/24/32.
             * Other DOS backends ignore the global. Flag is DJGPP-gated
             * so non-DOS mains never see it (per the "no platform-
             * specific knobs in shared core code" rule). */
            if(i + 1 < argc) {
                int d = relic_parse_colour_depth(argv[++i]);
                if(d == 0) {
                    fprintf(stderr, "error: --colour-depth must be 8, 16, 24, or 32\n");
                    return 1;
                }
                g_relic_preferred_color_depth = d;
            } else {
                fprintf(stderr, "error: --colour-depth requires a value (8|16|24|32)\n");
                return 1;
            }
        } else if(strcmp(argv[i], "--res") == 0) {
            /* DOS only: physical mode for WINFLAG_STRETCH windows; ignored
             * otherwise. DJGPP-gated. */
            if(i + 1 < argc) {
                int rw = 0, rh = 0;
                if(!relic_parse_mode_res(argv[++i], &rw, &rh)) {
                    fprintf(stderr, "error: --res must be <W>x<H> (e.g. 640x480)\n");
                    return 1;
                }
                g_relic_stretch_mode_w = rw;
                g_relic_stretch_mode_h = rh;
            } else {
                fprintf(stderr, "error: --res requires a value (<W>x<H>)\n");
                return 1;
            }
        } else if(strcmp(argv[i], "--skip-clear") == 0) {
            /* DOS+TinyGL only: opt out of the per-frame GL colour-buffer
             * clear for programs whose 3D scene fully covers the screen.
             * See src/core/vm.c for details. DJGPP-gated. */
            g_relic_skip_clear = 1;
#endif
#if defined(RELIC_RISCOS)
        } else if(strcmp(argv[i], "--fullscreen") == 0) {
            /* RISC OS only: force the fullscreen games-mode path for
             * every @window call (backend ORs WINFLAG_FULLSCREEN in), so
             * windowed programs can be exercised fullscreen unmodified. */
            g_relic_fullscreen = 1;
#endif
#if !defined(_WIN32)
        } else if(strcmp(argv[i], "--proxy") == 0) {
            /* HTTP proxy URL for plain-HTTP backends (DOS lwIP, POSIX
             * socket fallback without libcurl); terminates TLS at the proxy. */
            if(i+1 < argc) { proxy_url = argv[++i]; }
            else { fprintf(stderr, "error: --proxy requires a URL argument\n"); return 1; }
#endif
        } else if(strcmp(argv[i], "--version") == 0) {
            relic_console_ensure();
            printf("relic-vm %s BETA (%s, backend-%s, jit=%s) built %s %s\n",
                   RELIC_VERSION, RELIC_PLATFORM_ID, RELIC_BACKEND_ID, relic_jit.name, __DATE__, __TIME__);
            printf("%s\n", RELIC_COPYRIGHT);
            return 0;
        } else if(argv[i][0] != '-' && !embedded_data && !bytecode_path) {
            /* The single bytecode-path positional (dev use: relic-vm prog.tlx).
             * Bound exes carry their bytecode inside, so this never fires for
             * them - their first positional is a user arg instead. */
            bytecode_path = argv[i];
        } else {
            /* Anything else - an unrecognized option, or any token once the
             * bytecode path is taken / this is a bound exe - is trapped as the
             * start of the user passthrough args (exposed via @args). */
            user_argv = &argv[i];
            user_argc = argc - i;
            break;
        }
    }

    /* Deferred --tier application: validated against the ceiling of the
     * --platform already seen (if any), so argument order doesn't matter. */
    if(tier_cli) {
        char err[160];
        if(relic_tier_set_override_name(tier_cli, err, sizeof err) != 0) {
            fprintf(stderr, "error: --tier: %s\n", err);
            return 1;
        }
    }

    if(!embedded_data && !bytecode_path) {
#ifdef __APPLE__
        /* Try to find bytecode in app bundle Resources (macOS) */
        char exe_path[1024];
        uint32_t size = sizeof(exe_path);
        if(_NSGetExecutablePath(exe_path, &size) == 0) {
            char *last_slash = strrchr(exe_path, '/');
            if(last_slash) {
                *last_slash = '\0';
                static char resources_path[2048];
                snprintf(resources_path, sizeof(resources_path), "%s/../Resources/program.b", exe_path);
                FILE *test = fopen(resources_path, "rb");
                if(test) {
                    fclose(test);
                    bytecode_path = resources_path;
                }
            }
        }
#endif
        if(!bytecode_path) {
            relic_console_ensure();
            fprintf(stderr,
                "\n"
                RELIC_COPYRIGHT "\n"
                "\n"
                "BETA SOFTWARE - this is a beta release. There will be bugs, and the\n"
                "API and behaviour may change between releases without notice.\n"
                "\n"
                "Version:  tigryxvm " RELIC_VERSION " BETA\n"
                "Platform: " RELIC_PLATFORM_ID "\n"
                "Backend:  " RELIC_BACKEND_ID "\n"
                "\n"
                "Usage:\n"
                "  tigryxvm [options] <program.tlx>          run a bytecode file\n"
                "  tigryxvm [options] -- <args...>           pass args after -- to program\n"
                "  tigryxvm --version                        print version\n"
                "\n"
                "Options:\n"
                "  --verbose                  enable verbose/diagnostic output\n"
                "  --jit                      force enable JIT (auto-enabled if available)\n"
                "  --no-jit                   disable JIT, use interpreter\n"
                "  --heap <size>              set VM heap size, e.g. 64M, 256M\n"
                "  --tier low|medium|high|super\n"
                "                             Adaptive Build: reference tier for the\n"
                "                             bytecode-mismatch warning\n"
                "  --platform <id>            Adaptive Build: target platform identity\n"
                "  --backend <id>             Adaptive Build: target backend identity\n"
                "  --no-console               hide the console on an Explorer/shortcut launch\n"
#if defined(RELIC_DOS)
                "  --colour-depth <N>         DOS+TinyGL only: pin screen colour depth (8|16|24|32)\n"
                "  --res <WxH>                DOS only: physical mode for WINFLAG_STRETCH windows\n"
                "  --skip-clear               DOS+TinyGL only: skip per-frame GL colour clear\n"
#endif
#if defined(RELIC_RISCOS)
                "  --fullscreen               RISC OS only: force @window fullscreen mode\n"
#endif
#if !defined(_WIN32)
                "  --proxy <url>              HTTP proxy for plain-HTTP backends (DOS; non-curl POSIX)\n"
#endif
                "  --version                  print version and exit\n"
                "\n"
                "  Bound executables (produced by 'relic bind') carry their bytecode\n"
                "  inside and take no <program.tlx> argument; any non-flag token is\n"
                "  passed through to the program via @args.\n"
            );
            return 1;
        }
    }

    IRBuffer *ir = calloc(1, sizeof(IRBuffer));
    if(!ir) { fatal("Error", "Out of memory"); relic_far_free(embedded_data); return 1; }

    int main_fn_id = -1;
    RelicAssets *assets = NULL;
    if(embedded_data) {
        vlog("[tigryxvm] loading embedded payload (%zu bytes)\n", embedded_size);
        if(!relic_bytecode_load_mem(ir, embedded_data, embedded_size, &main_fn_id)) {
            fatal("Error", "Failed to load embedded bytecode");
            relic_far_free(embedded_data); relic_ir_free(ir); free(ir);
            return 1;
        }
        assets = relic_assets_load_mem(embedded_data, embedded_size);
        relic_far_free(embedded_data);
        embedded_data = NULL;
    } else {
        vlog("[tigryxvm] loading %s\n", bytecode_path);
        if(!relic_bytecode_load(ir, bytecode_path, &main_fn_id)) {
            fatal("Error", "Failed to load bytecode file");
            relic_ir_free(ir); free(ir);
            return 1;
        }
        assets = relic_assets_load(bytecode_path);
    }
    /* Heap precedence: --heap flag > bind-time footer > .tlx v5 embedded
     * value > RELIC_MEM_SIZE default. */
    if(!heap_given && !embedded_heap && ir->heap_size) {
        mem_sz = ir->heap_size;
        vlog("[tigryxvm] .tlx embedded heap %lu bytes\n",
             (unsigned long)ir->heap_size);
    }

    if(assets) vlog("[tigryxvm] %d embedded asset(s)\n", assets->count);

    /* Adaptive Build: warn when the bundled/loaded bytecode was compiled
     * for a different tier than this machine (see the same check in
     * tigryxri's main.c). */
    {
        int host = relic_tier_detect();
        if(ir->tier >= 0 && ir->tier != host)
            fprintf(stderr,
                "warning: this program was compiled for tier '%s' but this "
                "platform is tier '%s'; it may run slowly or use features "
                "the program did not expect\n",
                relic_tier_name(ir->tier), relic_tier_name(host));
    }

    vlog("[tigryxvm] loaded %d words, %d fns, %d bytes strings\n",
         ir->len, ir->nfuncs, ir->strings_len);

    if(main_fn_id < 0 || main_fn_id >= ir->nfuncs) {
        fatal("Error", "Invalid main function ID in bytecode");
        relic_ir_free(ir); free(ir);
        return 1;
    }

    vlog("[tigryxvm] main fn_id=%d, starting vm at pc=%d\n",
         main_fn_id, IRFNOFF(ir, main_fn_id));

    RelicVM vm = {0};
    if(!relic_vm_init(&vm, ir, &relic_backend, mem_sz)) {
        fatal("Error", "VM init failed");
        relic_ir_free(ir); free(ir); relic_assets_free(assets);
        return 1;
    }
    vm.verbose = g_verbose;
    vm.assets = assets;
    vm.jit_name = relic_jit.name;
    vm.jit_enabled = jit_mode;
    vm.http_proxy = proxy_url;
    /* Expose only the user passthrough args to the program via @args - never
     * relic-vm's own argv (program path / flags). */
    vm.cli_argc = user_argc;
    vm.cli_argv = user_argv;
    vm.exe_path = argv[0];

    relic_mvm_attach(&vm, &relic_jit);

    relic_splash_banner(&vm);

    /* Push a sentinel call frame so RET from main() stops the VM cleanly. */
    { i32 v = (i32)ir->len;  vm.sp -= 4; relic_far_write(vm.mem, vm.sp, &v, 4); }
    { i32 v = (i32)vm.fp;    vm.sp -= 4; relic_far_write(vm.mem, vm.sp, &v, 4); }
    vm.fp = vm.sp;

    vm.pc = IRFNOFF(ir, main_fn_id);

    if(jit_mode) {
        RelicJitCode *jcode = NULL;
        char jerr[256] = {0};
        if(relic_jit.compile && relic_jit.compile(ir, &jcode, jerr, sizeof jerr)) {
            vlog("[tigryxvm] jit '%s' compiled, running\n", relic_jit.name);
            vm.exit_code = relic_jit.run(jcode, &vm);
            relic_jit.free(jcode);
        } else {
            vlog("[tigryxvm] jit '%s' unavailable (%s); using interpreter\n",
                 relic_jit.name, jerr[0] ? jerr : "no reason given");
            relic_vm_run(&vm);
        }
    } else {
        relic_vm_run(&vm);
    }

    vlog("[tigryxvm] vm exited code=%d\n", vm.exit_code);
    relic_mvm_shutdown();
    relic_vm_free(&vm);
    relic_ir_free(ir); free(ir);
    relic_assets_free(assets);

    return vm.exit_code;
}
