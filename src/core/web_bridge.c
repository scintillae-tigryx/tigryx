/*
 * web_bridge.c
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
 * web_bridge.c - WASM<->JS bridge for the tigryxrcjs generated runtime.
 *
 * Compiled into tigryxvm only when targeting Emscripten. On any other
 * platform the entire file is a no-op translation unit.
 *
 * Purpose:
 *   tigryxrcjs emits JavaScript that drives the VM directly (per-function
 *   compiled JS replacing relic_vm_run). For that to work, the generated
 *   JS needs:
 *
 *     1. a way to perform all the setup that `main()` in tigryx.c normally
 *        does (load bytecode + asset trailer, init the VM, install the
 *        backend, push the sentinel frame, set pc to main's entry); and
 *
 *     2. a way to read/write the VM's registers, pc, sp, fp, mem, etc.
 *        from JS without a function-call per access. We expose them as
 *        offsets so the generated JS can index straight into the heap
 *        view (HEAP32[(vmPtr + offR + i*4) >> 2]).
 *
 *   The bridge does NOT execute the program. The generated JS calls into
 *   the per-opcode host trampolines (relic_op_gfx_*, relic_op_sys_*, ...)
 *   exported from src/core/vm.c by EMSCRIPTEN_KEEPALIVE there.
 *
 * Lifecycle (driven from JS):
 *
 *     relic_js_setup("/program.tlx")   -> 0 on success, <0 on error.
 *     vm = relic_js_vm();
 *     pc = relic_js_fn_offset(relic_js_main_fn());   (already loaded by setup)
 *     ... run the generated JS dispatch loop, occasionally calling the
 *         relic_op_* trampolines and reading/writing vm via the offsets ...
 *     relic_js_shutdown();
 *
 * There is exactly one VM per page (singleton state below). This matches
 * the current shell.html / tigryxvm.{js,wasm,data} packaging, and is the
 * same restriction documented in runtime.js for Stage 1.
 */
#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tigryx.h"

/* Provided by the linked backend (opengl-emscripten in the web build). */
extern RelicBackend relic_backend;

/* Verbose flag helper defined in the Emscripten backend via EM_JS.
 * Checks ?verbose=1 URL param or Module._relic_verbose. */
extern int relic_verbose_enabled(void);

/* Backend-side flag: tell the OpenGL backend to skip its emscripten_sleep()
 * calls (gfx_flip, inp_key, inp_keyheld). The JS-codegen runtime yields to
 * the browser via requestAnimationFrame between calls instead, sidestepping
 * ASYNCIFY's single-entry limitation that traps with start_rewind(null) when
 * each host op is invoked as its own wasm entry. Defined in
 * src/backends/opengl-emscripten/backend_opengl_emscripten.c. */
extern void relic_backend_set_skip_sleep(int skip);

/* -------------------------------------------------------------------------
 * Singleton VM state
 * ---------------------------------------------------------------------- */
static RelicVM      g_vm;
static IRBuffer    *g_ir      = NULL;
static RelicAssets *g_assets  = NULL;
static int          g_main_fn = -1;
static int          g_inited  = 0;

/* -------------------------------------------------------------------------
 * Setup / teardown
 * ---------------------------------------------------------------------- */
EMSCRIPTEN_KEEPALIVE
int relic_js_setup(const char *tlx_path) {
    if (relic_verbose_enabled())
        fprintf(stderr, "[relic-js] setup: path=%s\n", tlx_path ? tlx_path : "(null)");
    if (g_inited) return -10;   /* already initialised */
    if (!tlx_path || !*tlx_path) return -1;

    IRBuffer *ir = (IRBuffer *)calloc(1, sizeof(IRBuffer));
    if (!ir) return -2;

    int main_fn_id = -1;
    if (!relic_bytecode_load(ir, tlx_path, &main_fn_id)) {
        fprintf(stderr, "[relic-js] bytecode_load FAILED\n");
        relic_ir_free(ir); free(ir);
        return -3;
    }
    if (relic_verbose_enabled())
        fprintf(stderr, "[relic-js] bytecode loaded: nfuncs=%d main_fn=%d ir_len=%d\n",
                ir->nfuncs, main_fn_id, (int)ir->len);
    if (main_fn_id < 0 || main_fn_id >= ir->nfuncs) {
        fprintf(stderr, "[relic-js] main_fn out of range\n");
        relic_ir_free(ir); free(ir);
        return -4;
    }

    /* Asset trailer is optional (e.g. tdoom.tlx ships its game WAD this way). */
    RelicAssets *assets = relic_assets_load(tlx_path);
    if (relic_verbose_enabled())
        fprintf(stderr, "[relic-js] assets: %s\n", assets ? "loaded" : "none");

    /* Guest heap: honour the .tlx's embedded request (the program's
     * `#pragma arg heap`, baked at bc time — same precedence tigryxri
     * gives it over its default). Files without one keep the historical
     * 64 MiB. RELIC_HEAP_AUTO resolves to the minimum viable heap. */
    u32 heap = ir->heap_size;
    if (heap == 0) heap = 64u * 1024u * 1024u;
    if (heap == RELIC_HEAP_AUTO) {
        u32 data_end = (u32)ir->globals_size + (u32)ir->strings_len;
        data_end = (data_end + 3u) & ~3u;
        data_end += RELIC_PLATFORM_STR_SIZE;
        u32 need = data_end + (u32)RELIC_STACK_SIZE + RELIC_HEAP_AUTO_MARGIN;
        heap = (need + 4095u) & ~4095u;
    }

    memset(&g_vm, 0, sizeof g_vm);
    if (!relic_vm_init(&g_vm, ir, &relic_backend, heap)) {
        fprintf(stderr, "[relic-js] vm_init FAILED\n");
        if (assets) relic_assets_free(assets);
        relic_ir_free(ir); free(ir);
        return -5;
    }
    if (relic_verbose_enabled())
        fprintf(stderr, "[relic-js] vm_init OK, mem=%u\n", (unsigned)g_vm.mem_sz);
    g_vm.assets = assets;

    /* Sentinel call frame so RET from main() halts the VM cleanly,
     * matching the setup that tigryx.c's main() does. */
    {
        i32 v;
        v = (i32)ir->len;     g_vm.sp -= 4; memcpy(g_vm.mem + g_vm.sp, &v, 4);
        v = (i32)g_vm.fp;     g_vm.sp -= 4; memcpy(g_vm.mem + g_vm.sp, &v, 4);
    }
    g_vm.fp = g_vm.sp;
    g_vm.pc = IRFNOFF(ir, main_fn_id);

    g_ir      = ir;
    g_assets  = assets;
    g_main_fn = main_fn_id;
    g_inited  = 1;

    /* Tell the backend to skip emscripten_sleep(); JS-codegen yields. */
    relic_backend_set_skip_sleep(1);
    if (relic_verbose_enabled())
        fprintf(stderr, "[relic-js] setup complete, pc=%u\n", (unsigned)g_vm.pc);
    return 0;
}

EMSCRIPTEN_KEEPALIVE
void relic_js_shutdown(void) {
    if (!g_inited) return;
    relic_vm_free(&g_vm);
    if (g_assets) { relic_assets_free(g_assets); g_assets = NULL; }
    if (g_ir)     { relic_ir_free(g_ir); free(g_ir); g_ir = NULL; }
    g_main_fn = -1;
    g_inited  = 0;
}

/* -------------------------------------------------------------------------
 * Accessors (pointer + ids)
 * ---------------------------------------------------------------------- */
EMSCRIPTEN_KEEPALIVE RelicVM *relic_js_vm(void)         { return &g_vm; }
EMSCRIPTEN_KEEPALIVE int      relic_js_main_fn(void)    { return g_main_fn; }
EMSCRIPTEN_KEEPALIVE int      relic_js_fn_count(void)   { return g_ir ? g_ir->nfuncs : 0; }
EMSCRIPTEN_KEEPALIVE int      relic_js_fn_offset(int i) {
    if (!g_ir || i < 0 || i >= g_ir->nfuncs) return -1;
    return (int)IRFNOFF(g_ir, i);
}
EMSCRIPTEN_KEEPALIVE int      relic_js_ir_len(void)     { return g_ir ? g_ir->len : 0; }

/* -------------------------------------------------------------------------
 * Struct-layout offsets (so JS can index the heap directly)
 *
 * These are *byte* offsets into the RelicVM struct starting at the pointer
 * returned by relic_js_vm(). The matching JS reads look like:
 *
 *     const vm   = Module._relic_js_vm();
 *     const offR = Module._relic_js_off_r();
 *     const r3   = HEAP32[(vm + offR + 3*4) >> 2];
 *
 * Cheaper than a JS->WASM call per register access (the difference is the
 * point of this whole exercise).
 * ---------------------------------------------------------------------- */
EMSCRIPTEN_KEEPALIVE unsigned relic_js_off_r(void)         { return (unsigned)offsetof(RelicVM, r); }
EMSCRIPTEN_KEEPALIVE unsigned relic_js_off_d(void)         { return (unsigned)offsetof(RelicVM, d); }
EMSCRIPTEN_KEEPALIVE unsigned relic_js_off_pc(void)        { return (unsigned)offsetof(RelicVM, pc); }
EMSCRIPTEN_KEEPALIVE unsigned relic_js_off_sp(void)        { return (unsigned)offsetof(RelicVM, sp); }
EMSCRIPTEN_KEEPALIVE unsigned relic_js_off_fp(void)        { return (unsigned)offsetof(RelicVM, fp); }
EMSCRIPTEN_KEEPALIVE unsigned relic_js_off_mem(void)       { return (unsigned)offsetof(RelicVM, mem); }
EMSCRIPTEN_KEEPALIVE unsigned relic_js_off_mem_sz(void)    { return (unsigned)offsetof(RelicVM, mem_sz); }
EMSCRIPTEN_KEEPALIVE unsigned relic_js_off_running(void)   { return (unsigned)offsetof(RelicVM, running); }
EMSCRIPTEN_KEEPALIVE unsigned relic_js_off_exit_code(void) { return (unsigned)offsetof(RelicVM, exit_code); }

/* Constants the JS dispatcher uses for register-file sizing. */
EMSCRIPTEN_KEEPALIVE unsigned relic_js_sizeof_reg(void) {
    return (unsigned)sizeof(((RelicVM *)0)->r[0]);     /* 4 */
}
EMSCRIPTEN_KEEPALIVE unsigned relic_js_nregs(void) {
    return (unsigned)(sizeof(((RelicVM *)0)->r) / sizeof(((RelicVM *)0)->r[0])); /* 16 */
}

#endif /* __EMSCRIPTEN__ */
