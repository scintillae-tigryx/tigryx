/*
 * mvm.c
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
 * mvm.c - embedded multi-VM host for standalone runtimes
 * (tigryxri / tigryxvm / tigryxcc-compiled exes).
 *
 * Lets a program that already owns the one real window run additional
 * .tlx programs inside it: @os_spawn loads a child RelicVM onto its own
 * fiber, @os_dock pins the child's surface into a rect of the parent's
 * own surface (host-logical coords), @os_kill/@os_status manage it.
 * Every parent @flip first runs each live child one slice so its frame
 * lands on the shared backbuffer before the present.
 *
 * The rendering path needs a backend that implements gfx_guest_begin/
 * end (opengl-win32 does); without one children still run, just with
 * nothing visible. Children get input only while focused (@os_focus),
 * no clipboard, run with their own folder as cwd, and share the host's
 * backend_data like tigryxos guests.
 *
 * Win32 only for now - fibers, VEH and the watchdog are all Win32.
 * On other platforms attach() is a no-op and @os_* returns -1 exactly
 * as if the host never provided the vtable.
 */
#include "tigryx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The host path is experimental: only TIGRYX_EXPERIMENTAL_MVM builds
 * compile it. Everywhere else (and on Win95-class targets, which lack
 * fibers/VEH and the address space for guarded reservations) attach()
 * is a no-op stub and @os_* returns -1. */
#if defined(_WIN32) && !defined(RELIC_WIN95) && defined(TIGRYX_EXPERIMENTAL_MVM)
#include <windows.h>
#define MVM_HOST 1
#endif

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

#ifdef MVM_HOST

#define MVM_MAX_KIDS   16
#define MVM_KID_MEM    (64u  << 20)
#define MVM_KID_MAXMEM (512u << 20)

/* Same guarded-reservation trick tigryxos uses: JIT'd child code does
 * no bounds checks, so the child heap lives +4GiB inside an 8GiB
 * reservation - a stray access faults into the VEH instead of
 * scribbling over the host. */
#define MVM_RESERVE (8ull << 30)
#define MVM_MEMOFF  (4ull << 30)

typedef struct MvmChild {
    RelicVM       vm;          /* FIRST member: (MvmChild*)&kid->vm == kid */
    IRBuffer      ir;
    RelicAssets  *assets;
    RelicJitCode *jcode;
    LPVOID        fiber;
    void         *vm_resv;     /* guarded reservation, or NULL */
    char          name[64];
    char          dir[MAX_PATH];  /* cwd while the child runs ("" = host's) */
    int           gw, gh;      /* logical surface size from @window */
    int           docked;
    int           dock_r[4];   /* x,y,w,h in host surface coords */
    int           dead, crashed, closed, close_pending;
    int           frame_done;  /* at @flip; depth clear consumes it */
    int           flipped;     /* completed a frame during this slice */
    void         *snap;        /* backend copy of the last complete frame */
    int           win_open;    /* called @window (STATUS WINDOW bit) */
    int           has_view, view_r[4];
    int           has_vp,   vp_r[4];
    int           gen;         /* handle generation */
} MvmChild;

static MvmChild     g_kids[MVM_MAX_KIDS];
static int          g_nkids;
static RelicVM     *g_host_vm;
static RelicBackend *g_be;          /* host's real backend table */
static const RelicJit *g_jit;
static LPVOID       g_host_fiber;
static LPVOID       g_sched_fiber;   /* == g_host_fiber */
static RelicBackend g_host_wrap;     /* host backend + wrapped gfx_flip */
static RelicBackend g_kid_backend;   /* real backend + child shims */
static void      (*g_real_flip)(RelicVM *vm);
static void      (*g_real_inp_key)(RelicVM *vm);
static void      (*g_real_inp_keyheld)(RelicVM *vm);
static MvmChild  *g_focus_kid;         /* child that currently owns input */
static int          g_attached;
static const RelicOSOps g_mvm_os_ops;

static void mvm_free_child(MvmChild *c);

static MvmChild *kid_of_vm(RelicVM *vm) { return (MvmChild *)vm; }

static MvmChild *kid_of_handle(int h) {
    int slot;
    MvmChild *c;
    if (h < 0) return NULL;
    slot = h & 0xFFFF;
    if (slot >= g_nkids) return NULL;
    c = &g_kids[slot];
    if (c->closed || (h >> 16) != c->gen) return NULL;
    return c;
}

/* ------------------------------------------------------------------ */
/* Watchdog / crash containment                                        */
/* ------------------------------------------------------------------ */

static volatile MvmChild *g_active_kid;
static volatile DWORD     g_slice_start;
/* Runaway guard, not a time-share quantum: a docked child normally ends
 * its slice at @flip. Cutting it at a display-frame budget (16ms, as
 * tigryxos does) lands mid-frame for any child whose draw + frame-pacing
 * wait exceeds it, and the rest of that frame then draws onto the
 * host's next backbuffer - visible as flicker/tearing in the pane. */
#define MVM_SLICE_MS 100
/* Burst scheduling: how much of a host frame a frame-producing child may
 * keep before control returns to the host (keeps the UI responsive), and
 * a hard cap on re-entries so a pathological guest can't spin forever. */
#define MVM_KID_BURST_MS  12
#define MVM_KID_BURST_MAX 512
static DWORD WINAPI mvm_watchdog(LPVOID p) {
    (void)p;
    for (;;) {
        MvmChild *c = (MvmChild *)g_active_kid;
        if (c && (DWORD)(GetTickCount() - g_slice_start) > MVM_SLICE_MS)
            c->vm.yield_req = 1;
        Sleep(1);
    }
    return 0;
}

static LONG WINAPI mvm_veh(EXCEPTION_POINTERS *ep) {
    int i;
    LPVOID f;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    f = GetCurrentFiber();
    for (i = 0; i < g_nkids; i++) {
        MvmChild *c = &g_kids[i];
        if (c->closed || c->fiber != f) continue;
        c->crashed = 1;
        SwitchToFiber(g_sched_fiber);
        return EXCEPTION_CONTINUE_SEARCH;   /* unreached */
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ------------------------------------------------------------------ */
/* Child backend shims                                                 */
/* ------------------------------------------------------------------ */

static void mvm_kid_init(RelicVM *vm) {
    /* Borrow the host's backend_data - the child draws into the same
     * window under the slice transform, like tigryxos guests. */
    vm->backend_data = g_host_vm->backend_data;
}

static void mvm_kid_shutdown(RelicVM *vm) { (void)vm; }

static void mvm_kid_gfx_window(RelicVM *vm) {
    MvmChild *c = kid_of_vm(vm);
    int rw = vm->r[3], rh = vm->r[4];
    if (rw < 64) rw = 64;
    if (rh < 64) rh = 64;
    c->gw = rw;
    c->gh = rh;
    c->win_open = 1;    /* virtual: the real window belongs to the host */
}

/* While a burst is running the per-flip capture is deferred to the
 * burst end - one copy per host frame, not one per guest frame. */
static int g_cap_defer;

/* Frame boundary = yield point, same as tigryxos's guest_gfx_flip. */
static void mvm_kid_gfx_flip(RelicVM *vm) {
    MvmChild *c = kid_of_vm(vm);
    if (!g_cap_defer && c->docked && g_be->gfx_guest_snap_capture)
        c->snap = g_be->gfx_guest_snap_capture(vm, c->snap);
    c->flipped = 1;
    c->frame_done = 1;
    SwitchToFiber(g_sched_fiber);
}

static void mvm_kid_yield(RelicVM *vm) {
    (void)vm;
    SwitchToFiber(g_sched_fiber);
}

/* Input is opt-in: @os_focus(handle) makes a child the input target.
 * @key/@keyheld then pass through to the real backend (which drains
 * last_key / reads the held table); @mouse reports dock-relative
 * coords, or (-1,-1,0,0) while the cursor is outside the pane.
 * While a child holds focus the HOST's wrapped inp_key/inp_keyheld
 * return 0 without consuming, so the key the user presses reaches the
 * child's slice instead of being eaten by the parent frame. */
static void mvm_kid_inp_key(RelicVM *vm) {
    if (kid_of_vm(vm) == g_focus_kid && g_be->inp_key) {
        g_be->inp_key(vm);
        return;
    }
    vm->r[1] = 0;
}
/* A close-request rescued from a focused child's @key_held(27) poll -
 * the backend hands the flag to whoever asks first, but it belongs to
 * the host. */
static int g_close_pending;

static void mvm_kid_inp_keyheld(RelicVM *vm) {
    /* Key 0 is the host window's close-button channel - never a child's. */
    if (vm->r[1] == 0) { vm->r[1] = 0; return; }
    if (kid_of_vm(vm) == g_focus_kid && g_be->inp_keyheld) {
        /* Drain the close-request flag before the child's poll can
         * consume it; the child then sees only the physical Escape. */
        if (vm->r[1] == 27 && g_be->inp_close_requested &&
            g_be->inp_close_requested(vm))
            g_close_pending = 1;
        g_be->inp_keyheld(vm);
        return;
    }
    vm->r[1] = 0;
}
static void mvm_kid_inp_mouse(RelicVM *vm) {
    MvmChild *c = kid_of_vm(vm);
    u32 ptr = (u32)vm->r[1];
    i32 x = -1, y = -1, b = 0, w = 0;
    if (ptr + 16u > vm->mem_sz) return;
    if (c == g_focus_kid && c->docked && c->gw > 0 && c->gh > 0 &&
        g_be->inp_mouse) {
        int dw, dh;
        g_be->inp_mouse(vm);   /* host-logical x,y at ptr */
        memcpy(&x, vm->mem + ptr + 0,  4);
        memcpy(&y, vm->mem + ptr + 4,  4);
        memcpy(&b, vm->mem + ptr + 8,  4);
        memcpy(&w, vm->mem + ptr + 12, 4);
        dw = c->dock_r[2] > 0 ? c->dock_r[2] : 1;
        dh = c->dock_r[3] > 0 ? c->dock_r[3] : 1;
        x = (x - c->dock_r[0]) * c->gw / dw;
        y = (y - c->dock_r[1]) * c->gh / dh;
        if (x < 0 || y < 0 || x >= c->gw || y >= c->gh) {
            x = -1; y = -1; b = 0;
        }
    }
    memcpy(vm->mem + ptr + 0,  &x, 4);
    memcpy(vm->mem + ptr + 4,  &y, 4);
    memcpy(vm->mem + ptr + 8,  &b, 4);
    memcpy(vm->mem + ptr + 12, &w, 4);
}

/* Host-side input gating: while a child is focused the parent's @key
 * reads nothing and, crucially, leaves last_key in place so the child's
 * slice can still pop it. @keyheld goes dead for the parent too (its
 * shortcuts shouldn't fire while the game owns input). Mouse is NOT
 * gated: the parent needs clicks to manage focus/Stop/etc. */
static void mvm_host_inp_key(RelicVM *vm) {
    if (g_focus_kid) { vm->r[1] = 0; return; }
    if (g_real_inp_key) g_real_inp_key(vm); else vm->r[1] = 0;
}
static void mvm_host_inp_keyheld(RelicVM *vm) {
    /* The window close request is a host-level event: deliver it even
     * while a child owns input (and even if a child's own poll raced us
     * to the flag - g_close_pending). A physical Escape stays gated;
     * the child owns real keys. Key 0 (dedicated close channel) is
     * never gated. */
    if (vm->r[1] == 0) {
        if (g_real_inp_keyheld) g_real_inp_keyheld(vm); else vm->r[1] = 0;
        /* A focused child's 27-poll can still rescue a pending close
         * into g_close_pending; the dedicated channel claims it too. */
        if (g_close_pending) { g_close_pending = 0; vm->r[1] = 1; }
        return;
    }
    if (vm->r[1] == 27) {
        int cr = g_close_pending;
        g_close_pending = 0;
        if (g_be->inp_close_requested && g_be->inp_close_requested(vm))
            cr = 1;
        if (cr) { vm->r[1] = 1; return; }
    }
    if (g_focus_kid) { vm->r[1] = 0; return; }
    if (g_real_inp_keyheld) g_real_inp_keyheld(vm); else vm->r[1] = 0;
}

/* @platform: run the real op, then patch the window rect fields
 * (52/56 = pos, 60/64 = size) to describe the child's surface rect
 * inside the host rather than the whole window. */
static void mvm_kid_sys_platform(RelicVM *vm) {
    MvmChild *c = kid_of_vm(vm);
    u32 ptr = (u32)vm->r[1];
    i32 v;
    g_be->sys_platform(vm);
    if (ptr + 68 > vm->mem_sz) return;
    v = c->docked ? c->dock_r[0] : 0; memcpy(vm->mem + ptr + 52, &v, 4);
    v = c->docked ? c->dock_r[1] : 0; memcpy(vm->mem + ptr + 56, &v, 4);
    v = c->gw; memcpy(vm->mem + ptr + 60, &v, 4);
    v = c->gh; memcpy(vm->mem + ptr + 64, &v, 4);
}

static void mvm_kid_sys_console(RelicVM *vm) {
    MvmChild *c = kid_of_vm(vm);
    u32 ptr = (u32)vm->r[1];
    char buf[512];
    int n = 0;
    while (ptr < vm->mem_sz && n < (int)sizeof(buf) - 1) {
        u8 ch = vm->mem[ptr++];
        if (!ch) break;
        buf[n++] = (char)ch;
    }
    buf[n] = 0;
    fprintf(stderr, "%s| %s", c->name, buf);
    if (n && buf[n - 1] != '\n') fputc('\n', stderr);
    fflush(stderr);
}

/* @view/@viewport persist for the child but the backend drops them at
 * each slice end - record the args so they can be re-asserted. */
static void mvm_kid_gfx_view(RelicVM *vm) {
    MvmChild *c = kid_of_vm(vm);
    c->has_view = (vm->r[3] > 0 && vm->r[4] > 0);
    if (c->has_view) {
        c->view_r[0] = vm->r[1]; c->view_r[1] = vm->r[2];
        c->view_r[2] = vm->r[3]; c->view_r[3] = vm->r[4];
    }
    g_be->gfx_view(vm);
}

/* Music is one process-wide player in the backend, so a child killed
 * mid-song would leave it playing. Remember which child started the
 * current track; mvm_free_child stops it on the way out. */
static MvmChild *g_music_kid;

static void mvm_kid_snd_music(RelicVM *vm) {
    MvmChild *c = kid_of_vm(vm);
    int cmd = vm->r[3];
    g_be->snd_music(vm);
    if (cmd == 0) { if (g_music_kid == c) g_music_kid = NULL; }
    else if (cmd != 3) g_music_kid = c;   /* 3 = load GENMIDI bank only */
}

static void mvm_kid_gfx_viewport(RelicVM *vm) {
    MvmChild *c = kid_of_vm(vm);
    c->has_vp = (vm->r[3] > 0 && vm->r[4] > 0);
    if (c->has_vp) {
        c->vp_r[0] = vm->r[1]; c->vp_r[1] = vm->r[2];
        c->vp_r[2] = vm->r[3]; c->vp_r[3] = vm->r[4];
    }
    g_be->gfx_viewport(vm);
}

/* Clipboard is the host's: children in run-mode get nothing. */
static void mvm_kid_clip_set(RelicVM *vm) { vm->r[1] = -1; }
static void mvm_kid_clip_get(RelicVM *vm) { vm->r[1] = -1; }

/* ------------------------------------------------------------------ */
/* Child lifecycle                                                     */
/* ------------------------------------------------------------------ */

/* Optional <path-stem>.inf sidecar "heap_mb=N" - same convention as
 * tigryxos's guest_heap_for. */
static size_t mvm_heap_for(const char *path) {
    char inf[512], buf[512], *dot, *p;
    FILE *f;
    size_t n, want;
    snprintf(inf, sizeof inf, "%s", path);
    dot = strrchr(inf, '.');
    if (dot) *dot = 0;
    strncat(inf, ".inf", sizeof inf - strlen(inf) - 1);
    f = fopen(inf, "rb");
    if (!f) return MVM_KID_MEM;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    p = strstr(buf, "heap_mb=");
    if (!p) return MVM_KID_MEM;
    want = (size_t)atoi(p + 8) << 20;
    if (want > MVM_KID_MAXMEM) want = MVM_KID_MAXMEM;
    return want > MVM_KID_MEM ? want : MVM_KID_MEM;
}

static void WINAPI mvm_kid_main(LPVOID p) {
    MvmChild *c = (MvmChild *)p;
    if (c->jcode) {
        g_jit->run(c->jcode, &c->vm);
    } else {
        relic_vm_run(&c->vm);
    }
    c->dead = 1;
    fprintf(stderr, "[mvm] %s exited (code %d)\n", c->name, c->vm.exit_code);
    /* A fiber must not return; hand back to the host forever. */
    SwitchToFiber(g_sched_fiber);
}

static int mvm_spawn(const char *path) {
    int slot = -1, i, main_fn = -1;
    size_t mem_sz;
    MvmChild *c;
    char jerr[256] = {0};

    if (!g_host_fiber) {
        g_host_fiber = ConvertThreadToFiber(NULL);
        g_sched_fiber = g_host_fiber;
        if (!g_host_fiber) return -1;
    }
    for (i = 0; i < g_nkids; i++)
        if (g_kids[i].closed) { slot = i; break; }
    if (slot < 0) {
        if (g_nkids >= MVM_MAX_KIDS) return -1;
        slot = g_nkids++;
    }
    c = &g_kids[slot];
    { int gen = c->gen; memset(c, 0, sizeof *c);
      c->gen = (gen + 1) & 0x7FFF; }

    if (!relic_bytecode_load(&c->ir, path, &main_fn)) {
        fprintf(stderr, "[mvm] cannot load %s\n", path);
        c->closed = 1;
        return -1;
    }
    if (main_fn < 0 || main_fn >= c->ir.nfuncs) {
        fprintf(stderr, "[mvm] %s: no main()\n", path);
        c->closed = 1;
        return -1;
    }
    mem_sz = mvm_heap_for(path);
    if (!relic_vm_init(&c->vm, &c->ir, &g_kid_backend, mem_sz)) {
        fprintf(stderr, "[mvm] %s: vm init failed\n", path);
        c->closed = 1;
        return -1;
    }
    {
        u8 *resv = (u8*)VirtualAlloc(NULL, (SIZE_T)MVM_RESERVE,
                                     MEM_RESERVE, PAGE_NOACCESS);
        u8 *gmem = resv ? (u8*)VirtualAlloc(resv + MVM_MEMOFF, mem_sz,
                                            MEM_COMMIT, PAGE_READWRITE)
                        : NULL;
        if (gmem) {
            memcpy(gmem, c->vm.mem, c->vm.heap_base);
            free(c->vm.mem);
            c->vm.mem  = gmem;
            c->vm_resv = resv;
        } else {
            if (resv) VirtualFree(resv, 0, MEM_RELEASE);
            fprintf(stderr, "[mvm] %s: guard reservation failed, unguarded\n",
                    path);
        }
    }
    c->assets = relic_assets_load(path);
    c->vm.assets   = c->assets;
    c->vm.exe_path = g_host_vm ? g_host_vm->exe_path : NULL;
    c->vm.yield_hook = mvm_kid_yield;
    c->vm.os_ops   = &g_mvm_os_ops;

    { i32 v = (i32)c->ir.len; c->vm.sp -= 4;
      relic_far_write(c->vm.mem, c->vm.sp, &v, 4); }
    { i32 v = (i32)c->vm.fp;  c->vm.sp -= 4;
      relic_far_write(c->vm.mem, c->vm.sp, &v, 4); }
    c->vm.fp = c->vm.sp;
    c->vm.pc = (u32)IRFNOFF(&c->ir, main_fn);

    c->jcode = NULL;
    if (g_jit && g_jit->compile &&
        g_jit->compile(&c->ir, &c->jcode, jerr, sizeof jerr)) {
        c->vm.jit_name    = g_jit->name;
        c->vm.jit_enabled = 1;
        fprintf(stderr, "[mvm] %s: jit '%s' compiled\n", path, g_jit->name);
    } else if (g_jit && g_jit->compile) {
        fprintf(stderr, "[mvm] %s: jit failed (%s); interpreter\n",
                path, jerr[0] ? jerr : "?");
    }

    c->fiber = CreateFiber(1u << 20, mvm_kid_main, c);
    if (!c->fiber) {
        fprintf(stderr, "[mvm] %s: CreateFiber failed\n", path);
        mvm_free_child(c);
        return -1;
    }
    {
        const char *slash = strrchr(path, '/');
        char *dot;
        if (!slash) slash = strrchr(path, '\\');
        snprintf(c->name, sizeof c->name, "%s", slash ? slash + 1 : path);
        dot = strrchr(c->name, '.'); if (dot) *dot = 0;
    }
    /* The child's relative paths ("game1.wad", "assets/x.png") resolve
     * against its own .tlx folder, as `tigryx run` from that folder
     * would. The process has one cwd, so mvm_run_kids swaps it in for
     * each slice and puts the host's back afterwards. */
    {
        char *base = NULL;
        DWORD n = GetFullPathNameA(path, sizeof c->dir, c->dir, &base);
        if (n == 0 || n >= sizeof c->dir || !base) c->dir[0] = 0;
        else *base = 0;
    }
    fprintf(stderr, "[mvm] spawned %s\n", c->name);
    return slot;
}

static void mvm_free_child(MvmChild *c) {
    if (c->closed) return;
    if (c == g_music_kid) {
        /* cmd 0 = stop; the backend reads nothing else for it. */
        c->vm.r[3] = 0;
        if (g_be->snd_music) g_be->snd_music(&c->vm);
        g_music_kid = NULL;
    }
    if (c->fiber) { DeleteFiber(c->fiber); c->fiber = NULL; }
    if (c->jcode) { g_jit->free(c->jcode); c->jcode = NULL; }
    if (c->vm_resv) {
        VirtualFree(c->vm_resv, 0, MEM_RELEASE);
        c->vm_resv = NULL; c->vm.mem = NULL;
    }
    relic_vm_free(&c->vm);
    relic_ir_free(&c->ir);
    if (c->assets) { relic_assets_free(c->assets); c->assets = NULL; }
    if (c->snap && g_be->gfx_guest_snap_free) g_be->gfx_guest_snap_free(c->snap);
    c->snap = NULL;
    if (c == g_focus_kid) g_focus_kid = NULL;
    c->closed = 1;
}

/* ------------------------------------------------------------------ */
/* RelicOSOps vtable                                                   */
/* ------------------------------------------------------------------ */

static int mvm_os_spawn(RelicVM *caller, const char *path) {
    int slot = mvm_spawn(path);
    (void)caller;
    if (slot < 0) return -1;
    return slot | (g_kids[slot].gen << 16);
}

static int mvm_os_kill(RelicVM *caller, int h) {
    MvmChild *c = kid_of_handle(h);
    (void)caller;
    if (!c) return 0;
    if (&c->vm == caller) { c->close_pending = 1; return 1; }
    mvm_free_child(c);
    return 1;
}

static int mvm_os_status(RelicVM *caller, int h) {
    MvmChild *c = kid_of_handle(h);
    int st = 0;
    (void)caller;
    if (!c) return -1;
    if (!c->dead && !c->crashed) st |= RELIC_OS_ST_RUNNING;
    if (c->dead || c->crashed)   st |= RELIC_OS_ST_DEAD;
    if (c->win_open || c->docked) st |= RELIC_OS_ST_WINDOW;
    if (c == g_focus_kid)        st |= RELIC_OS_ST_FOCUS;
    return st;
}

static int mvm_os_focus(RelicVM *caller, int h) {
    MvmChild *c;
    (void)caller;
    if (h < 0) { g_focus_kid = NULL; return 1; }
    c = kid_of_handle(h);
    if (!c) return 0;
    g_focus_kid = c;   /* input routes here until focus(-1) or kill */
    return 1;
}

static int mvm_os_count(RelicVM *caller) {
    int n = 0, i;
    (void)caller;
    for (i = 0; i < g_nkids; i++)
        if (!g_kids[i].closed) n++;
    return n;
}

static int mvm_os_handle_at(RelicVM *caller, int ord) {
    int i, n = 0;
    (void)caller;
    for (i = 0; i < g_nkids; i++) {
        if (g_kids[i].closed) continue;
        if (n == ord) return i | (g_kids[i].gen << 16);
        n++;
    }
    return -1;
}

static int mvm_os_set_rect(RelicVM *caller, int h,
                           int x, int y, int w, int ht) {
    MvmChild *c = kid_of_handle(h);
    (void)caller;
    if (!c) return 0;
    /* A standalone child's only rect is its dock rect. */
    c->dock_r[0] = x; c->dock_r[1] = y;
    c->dock_r[2] = w; c->dock_r[3] = ht;
    return 1;
}

static int mvm_os_get_rect(RelicVM *caller, int h, i32 out[4]) {
    MvmChild *c = kid_of_handle(h);
    (void)caller;
    if (!c) return 0;
    out[0] = c->dock_r[0]; out[1] = c->dock_r[1];
    out[2] = c->dock_r[2]; out[3] = c->dock_r[3];
    return 1;
}

static int mvm_os_name(RelicVM *caller, int h, char *buf, int cap) {
    MvmChild *c = kid_of_handle(h);
    int n;
    (void)caller;
    if (!c) return -1;
    n = (int)strlen(c->name);
    if (n > cap - 1) n = cap - 1;
    memcpy(buf, c->name, n);
    buf[n] = 0;
    return n;
}

static int mvm_os_iconify(RelicVM *caller, int h, int iconify) {
    (void)caller; (void)h; (void)iconify;
    return 0;
}

static int mvm_os_dock(RelicVM *caller, int h,
                       int x, int y, int w, int ht) {
    MvmChild *c = kid_of_handle(h);
    (void)caller;
    if (!c) return 0;
    if (w <= 0 || ht <= 0) { c->docked = 0; return 1; }
    c->docked = 1;
    c->dock_r[0] = x; c->dock_r[1] = y;
    c->dock_r[2] = w; c->dock_r[3] = ht;
    c->win_open = 1;
    return 1;
}

static const RelicOSOps g_mvm_os_ops = {
    mvm_os_spawn, mvm_os_kill, mvm_os_status, mvm_os_focus,
    mvm_os_count, mvm_os_handle_at, mvm_os_set_rect, mvm_os_get_rect,
    mvm_os_name, mvm_os_iconify, mvm_os_dock
};

/* ------------------------------------------------------------------ */
/* Scheduling: children run one slice per host @flip                    */
/* ------------------------------------------------------------------ */

static void mvm_run_kids(void) {
    int i;
    char host_dir[MAX_PATH];
    DWORD hn = GetCurrentDirectoryA(sizeof host_dir, host_dir);
    if (hn == 0 || hn >= sizeof host_dir) host_dir[0] = 0;
    for (i = 0; i < g_nkids; i++) {
        MvmChild *c = &g_kids[i];
        int swapped;
        if (c->closed || c->dead || c->crashed) continue;
        if (c->close_pending) { mvm_free_child(c); continue; }
        swapped = c->dir[0] && host_dir[0] && SetCurrentDirectoryA(c->dir);
        g_active_kid  = c;
        if (g_be->gfx_guest_begin) {
            /* Re-enter the child after each completed @flip while a burst
             * budget remains, instead of waiting for the next host frame.
             * Without this a child's frame rate is capped by the host's
             * (one flip per host present) - BENCH's graphics tests, which
             * count draw-loop iterations against a wall clock, then read
             * ~2% of standalone speed under vsync. guest_begin/end run
             * ONCE per burst (the full GL attrib push/pop is several ms,
             * so doing it per guest frame capped the child at ~150fps);
             * mid-burst flips keep the guest transform live - the
             * backbuffer is not presented until run_kids returns, so a
             * half-drawn frame can never reach the screen. */
            DWORD burst0 = GetTickCount();
            int   spins  = 0;
            int x = 0, y = 0, w = 0, h = 0, lw = 1, lh = 1;
            /* Undocked or pre-@window: begin() with a zero-area clip
             * so stray early draws can't scribble over the frame. */
            if (c->docked) {
                x = c->dock_r[0]; y = c->dock_r[1];
                w = c->dock_r[2]; h = c->dock_r[3];
            }
            if (c->gw > 0 && c->gh > 0) { lw = c->gw; lh = c->gh; }
            g_be->gfx_guest_begin(&c->vm, x, y, w, h, lw, lh);
            g_cap_defer = 1;
            do {
                g_slice_start = GetTickCount();
                c->vm.yield_req = 0;
                /* Shared depth buffer: wipe the slice once per child
                 * frame (same fix as tigryxos's guest_depth_reset). */
                if (c->frame_done) {
                    c->frame_done = 0;
                    if (g_be->gfx_guest_depth_clear)
                        g_be->gfx_guest_depth_clear(&c->vm);
                }
                if (c->has_view) {
                    c->vm.r[1] = c->view_r[0]; c->vm.r[2] = c->view_r[1];
                    c->vm.r[3] = c->view_r[2]; c->vm.r[4] = c->view_r[3];
                    g_be->gfx_view(&c->vm);
                }
                if (c->has_vp) {
                    c->vm.r[1] = c->vp_r[0]; c->vm.r[2] = c->vp_r[1];
                    c->vm.r[3] = c->vp_r[2]; c->vm.r[4] = c->vp_r[3];
                    g_be->gfx_viewport(&c->vm);
                }
                c->flipped = 0;
                SwitchToFiber(c->fiber);
                spins++;
            } while (c->flipped && !c->dead && !c->crashed &&
                     !c->close_pending && spins < MVM_KID_BURST_MAX &&
                     (DWORD)(GetTickCount() - burst0) < MVM_KID_BURST_MS);
            g_cap_defer = 0;
            if (c->flipped) {
                /* Burst ended on a frame boundary: the backbuffer holds
                 * exactly that frame, so one capture covers the whole
                 * burst (it is also the newest frame for retention). */
                if (c->docked && g_be->gfx_guest_snap_capture)
                    c->snap = g_be->gfx_guest_snap_capture(&c->vm,
                                                           c->snap);
            } else if (c->docked && c->snap &&
                       g_be->gfx_guest_snap_present) {
                /* Slice ended mid-frame (pacing wait or watchdog cut):
                 * the pane region shows partial work - put the last
                 * complete frame back over it. */
                g_be->gfx_guest_snap_present(&c->vm, c->snap);
            }
            g_be->gfx_guest_end(&c->vm);
        } else {
            g_slice_start = GetTickCount();
            c->vm.yield_req = 0;
            /* Backend has no slice hooks: the child still needs CPU to
             * reach @window. Draws land wherever the backend leaves
             * them - unavoidable without slice isolation. */
            SwitchToFiber(c->fiber);
        }
        g_active_kid = NULL;
        if (swapped) SetCurrentDirectoryA(host_dir);
    }
}

static void mvm_host_flip(RelicVM *vm) {
    mvm_run_kids();
    if (g_real_flip) g_real_flip(vm);
}

/* ------------------------------------------------------------------ */
/* Attach / shutdown                                                   */
/* ------------------------------------------------------------------ */

void relic_mvm_attach(RelicVM *vm, const RelicJit *jit) {
    static int veh_done, watchdog_done;
    if (g_attached || !vm) return;
    g_attached = 1;
    g_host_vm  = vm;
    g_jit      = jit;

    /* Wrap the host's backend table: every @flip first gives each
     * child a slice, then performs the real present + event pump.
     * @key/@keyheld go quiet while a child holds input focus so the
     * keypress reaches the child's slice instead. Mouse stays live -
     * the parent needs clicks to manage focus. */
    g_be = vm->backend;   /* the real backend this host was started on */
    g_host_wrap = *g_be;
    g_real_flip = g_be->gfx_flip;
    g_real_inp_key     = g_be->inp_key;
    g_real_inp_keyheld = g_be->inp_keyheld;
    g_host_wrap.gfx_flip    = mvm_host_flip;
    g_host_wrap.inp_key     = mvm_host_inp_key;
    g_host_wrap.inp_keyheld = mvm_host_inp_keyheld;
    vm->backend = &g_host_wrap;

    /* Child backend: real table + shims. File ops nulled so they fall
     * through to vm.c's per-VM files table; input + clipboard denied. */
    g_kid_backend = *g_be;
    g_kid_backend.init          = mvm_kid_init;
    g_kid_backend.shutdown      = mvm_kid_shutdown;
    g_kid_backend.gfx_window    = mvm_kid_gfx_window;
    g_kid_backend.gfx_flip      = mvm_kid_gfx_flip;
    g_kid_backend.sys_platform  = mvm_kid_sys_platform;
    g_kid_backend.sys_console   = mvm_kid_sys_console;
    g_kid_backend.inp_key       = mvm_kid_inp_key;
    g_kid_backend.inp_keyheld   = mvm_kid_inp_keyheld;
    g_kid_backend.inp_mouse     = mvm_kid_inp_mouse;
    g_kid_backend.inp_gamepad_count = NULL;
    g_kid_backend.inp_gamepad       = NULL;
    g_kid_backend.sys_fopen     = NULL;
    g_kid_backend.sys_fread     = NULL;
    g_kid_backend.sys_fwrite    = NULL;
    g_kid_backend.sys_fclose    = NULL;
    g_kid_backend.sys_fseek     = NULL;
    g_kid_backend.sys_flen      = NULL;
    g_kid_backend.gfx_view      = mvm_kid_gfx_view;
    g_kid_backend.gfx_viewport  = mvm_kid_gfx_viewport;
    g_kid_backend.sys_clip_set  = mvm_kid_clip_set;
    g_kid_backend.sys_clip_get  = mvm_kid_clip_get;
    if (g_be->snd_music) g_kid_backend.snd_music = mvm_kid_snd_music;

    vm->os_ops = &g_mvm_os_ops;

    if (!veh_done) {
        AddVectoredExceptionHandler(1, mvm_veh);
        veh_done = 1;
    }
    if (!watchdog_done) {
        HANDLE t = CreateThread(NULL, 0, mvm_watchdog, NULL, 0, NULL);
        if (t) CloseHandle(t);
        watchdog_done = 1;
    }
}

void relic_mvm_shutdown(void) {
    int i;
    for (i = 0; i < g_nkids; i++)
        mvm_free_child(&g_kids[i]);
    g_nkids = 0;
}

#else  /* !MVM_HOST - no fiber/VEH support on this platform */

void relic_mvm_attach(RelicVM *vm, const RelicJit *jit) {
    (void)vm; (void)jit;
}

void relic_mvm_shutdown(void) {}

#endif /* MVM_HOST */
