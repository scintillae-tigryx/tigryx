/*
 * syswin32.c
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
 * syswin32.c - Win32 system services shared by backends.
 *
 * Extracted from backend_opengl_win32.c: @platform probing, the Win32
 * clipboard (CF_TEXT), and the WinINet worker-thread HTTP layer. None of
 * this needs a window or GL context, so the console-only runtime backend
 * (tigryx_rt_console) uses it directly to give console executables
 * working @platform/@clipboard/@http builtins.
 */
#include "syswin32.h"
#include "tigryx_tier.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__)
#include <cpuid.h>
#if !defined(__get_cpuid)
#undef __cpuid
/* CPUID shim for GCC toolchains that lack __get_cpuid (mirrors the one in
 * backend_opengl_win32.c). */
static void __cpuid(int cpui[4], int eax) {
    __asm__ __volatile__ (
        "cpuid"
        : "=a"(cpui[0]), "=b"(cpui[1]), "=c"(cpui[2]), "=d"(cpui[3])
        : "a"(eax), "c"(0)
        : "memory"
    );
}
#endif
#endif

#ifndef RELIC_PLATFORM_ID
#define RELIC_PLATFORM_ID "unknown"
#endif
#ifndef RELIC_BACKEND_ID
#define RELIC_BACKEND_ID "syswin32"
#endif

/* -------------------------------------------------------------------------
 * @platform helpers
 * ---------------------------------------------------------------------- */
#define PLATFORM_INFO_SIZE 116u

/* Copies a string into the VM's platform-strings scratch area.
 * Returns the VM memory offset of the copy, or 0 on overflow. */
static u32 syswin32_platstr(RelicVM *vm, u32 *cursor, const char *s) {
    if (!vm || !cursor || !s) return 0;
    u32 base = vm->platform_strings_base;
    u32 sz   = vm->platform_strings_size;
    if (sz == 0) return 0;
    u32 n = (u32)strlen(s) + 1u;
    if (*cursor + n > sz) return 0;
    u32 off = base + *cursor;
    memcpy(vm->mem + off, s, n);
    *cursor += n;
    return off;
}

/* Writes a little-endian u32 pointer into a VM struct field. */
static void syswin32_putptr(u8 *mem, int off, u32 ptr) {
    mem[off+0] =  ptr        & 0xFF;
    mem[off+1] = (ptr >>  8) & 0xFF;
    mem[off+2] = (ptr >> 16) & 0xFF;
    mem[off+3] = (ptr >> 24) & 0xFF;
}

/* Maps an architecture and CPU-class code to a display string. */
static const char *syswin32_cpu_class_str(u8 arch, u8 cpu_class) {
    if (arch == 0) {
        switch (cpu_class) {
            case 0: return "8086";    case 1: return "286";
            case 2: return "386";     case 3: return "486";
            case 4: return "Pentium"; case 5: return "MMX";
            case 6: return "SSE";     case 7: return "SSE2";
            default: return "Unknown";
        }
    } else if (arch == 1) {
        switch (cpu_class) {
            case 0: return "Baseline"; case 1: return "SSE3";
            case 2: return "SSE4.2";   case 3: return "AVX1";
            case 4: return "AVX2";     case 5: return "AVX512";
            default: return "Unknown";
        }
    } else if (arch == 2 || arch == 3) {
        switch (cpu_class) {
            case 0: return "v6";          case 1: return "v7/Cortex-A";
            case 2: return "v7/Cortex-M"; case 3: return "v8/AArch64";
            default: return "Unknown";
        }
    }
    return "Unknown";
}

/* @platform(info): fills the PlatformInfo struct in VM memory. The
 * display/GPU/window/backend fields come from `view` so the same probe
 * serves windowed (OpenGL) and console (no-graphics) executables. */
void syswin32_sys_platform(RelicVM *vm, const SysWin32PlatformView *view) {
    u32 ptr = (u32)vm->r[1];
    if (ptr + PLATFORM_INFO_SIZE > vm->mem_sz) return;
    u8 *mem = vm->mem + ptr;

    u32 cursor = 0;

    syswin32_putptr(mem, 0, syswin32_platstr(vm, &cursor, "Windows"));

    {
        const char *ver_str = "Unknown";
        char ver_buf[16];
        OSVERSIONINFOEXW ovi;
        memset(&ovi, 0, sizeof(ovi));
        ovi.dwOSVersionInfoSize = sizeof(ovi);
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll) {
            typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOEXW*);
            RtlGetVersionFn fn = (RtlGetVersionFn)GetProcAddress(ntdll, "RtlGetVersion");
            if (fn && fn(&ovi) == 0) {
                int v = (ovi.dwMajorVersion == 10 && ovi.dwBuildNumber >= 22000)
                      ? 11 : (int)ovi.dwMajorVersion;
                snprintf(ver_buf, sizeof(ver_buf), "%d", v);
                ver_str = ver_buf;
            }
        }
        /* RtlGetVersion is NT-only (NT4+). On Win9x fall back to
         * GetVersionExA, which has been in kernel32 since Win95 and
         * reports major=4 for 95/98/ME. */
        if (ver_str[0] == 'U') {
            OSVERSIONINFOA ova;
            memset(&ova, 0, sizeof(ova));
            ova.dwOSVersionInfoSize = sizeof(ova);
            if (GetVersionExA(&ova)) {
                snprintf(ver_buf, sizeof(ver_buf), "%d", (int)ova.dwMajorVersion);
                ver_str = ver_buf;
            }
        }
        syswin32_putptr(mem, 4, syswin32_platstr(vm, &cursor, ver_str));
    }

    {
        u8 arch = 1, cpu_class = 0;
        const char *arch_str = "x64";
#if defined(_M_ARM64) || defined(__aarch64__)
        arch = 3; cpu_class = 3; arch_str = "ARM64";
#elif defined(_M_ARM) || defined(__arm__)
        arch = 2; cpu_class = 1; arch_str = "ARM";
#else
        int cpui[4] = {0};
        __cpuid(cpui, 1);
        int ecx1 = cpui[2];
        __cpuid(cpui, 7);
        int ebx7 = cpui[1];
        if      (ebx7 & (1 << 16)) { arch = 1; cpu_class = 5; }
        else if (ebx7 & (1 << 5))  { arch = 1; cpu_class = 4; }
        else if (ecx1 & (1 << 28)) { arch = 1; cpu_class = 3; }
        else if (ecx1 & (1 << 19)) { arch = 1; cpu_class = 2; }
        else if (ecx1 & (1 << 0))  { arch = 1; cpu_class = 1; }
        else                        { arch = 1; cpu_class = 0; }
#endif
        syswin32_putptr(mem, 8,  syswin32_platstr(vm, &cursor, arch_str));
        syswin32_putptr(mem, 12, syswin32_platstr(vm, &cursor,
                                                syswin32_cpu_class_str(arch, cpu_class)));
    }

    syswin32_putptr(mem, 16, syswin32_platstr(vm, &cursor,
                                            view && view->display_mode ? view->display_mode : "Text"));

    {
        u16 sw = (u16)GetSystemMetrics(SM_CXSCREEN);
        u16 sh = (u16)GetSystemMetrics(SM_CYSCREEN);
        mem[20] = sw & 0xFF;        mem[21] = (sw >> 8) & 0xFF;
        mem[22] = sh & 0xFF;        mem[23] = (sh >> 8) & 0xFF;

        u32 colours = 16777216;
        HDC hdc = GetDC(NULL);
        if (hdc) {
            int bpp = GetDeviceCaps(hdc, BITSPIXEL) * GetDeviceCaps(hdc, PLANES);
            ReleaseDC(NULL, hdc);
            if (bpp <= 8)       colours = 256;
            else if (bpp <= 16) colours = 65536;
            else                colours = 16777216;
        }
        mem[24] =  colours        & 0xFF;
        mem[25] = (colours >>  8) & 0xFF;
        mem[26] = (colours >> 16) & 0xFF;
        mem[27] = (colours >> 24) & 0xFF;
    }

    {
        u32 sound_flags = view ? view->sound_flags : 0;
        mem[28] =  sound_flags        & 0xFF;
        mem[29] = (sound_flags >>  8) & 0xFF;
        mem[30] = (sound_flags >> 16) & 0xFF;
        mem[31] = (sound_flags >> 24) & 0xFF;
    }
    syswin32_putptr(mem, 32, syswin32_platstr(vm, &cursor, "Read-write"));
    syswin32_putptr(mem, 36, syswin32_platstr(vm, &cursor, "Hardware"));
    syswin32_putptr(mem, 40, syswin32_platstr(vm, &cursor,
                                            view && view->gfx_arch ? view->gfx_arch : "Null"));

    syswin32_putptr(mem, 44, syswin32_platstr(vm, &cursor,
                                            view && view->gpu_str ? view->gpu_str : "None"));
    syswin32_putptr(mem, 48, syswin32_platstr(vm, &cursor,
                                            view && view->gl_version ? view->gl_version : "0.0"));

    {
        u32 uwx = (u32)(view ? view->win_x : 0);
        u32 uwy = (u32)(view ? view->win_y : 0);
        u32 uww = (u32)(view ? view->win_w : 0);
        u32 uwh = (u32)(view ? view->win_h : 0);
        mem[52] =  uwx        & 0xFF; mem[53] = (uwx >>  8) & 0xFF;
        mem[54] = (uwx >> 16) & 0xFF; mem[55] = (uwx >> 24) & 0xFF;
        mem[56] =  uwy        & 0xFF; mem[57] = (uwy >>  8) & 0xFF;
        mem[58] = (uwy >> 16) & 0xFF; mem[59] = (uwy >> 24) & 0xFF;
        mem[60] =  uww        & 0xFF; mem[61] = (uww >>  8) & 0xFF;
        mem[62] = (uww >> 16) & 0xFF; mem[63] = (uww >> 24) & 0xFF;
        mem[64] =  uwh        & 0xFF; mem[65] = (uwh >>  8) & 0xFF;
        mem[66] = (uwh >> 16) & 0xFF; mem[67] = (uwh >> 24) & 0xFF;
    }

    {
        const char *js;
        if (!vm->jit_name || !vm->jit_name[0] || strcmp(vm->jit_name, "null") == 0)
            js = "Not available";
        else if (!vm->jit_enabled)
            js = "Disabled (--no-jit)";
        else if (strstr(vm->jit_name, "-v2"))
            js = "Enabled (JIT v2)";
        else
            js = "Enabled (JIT v1)";
        syswin32_putptr(mem, 68, syswin32_platstr(vm, &cursor, js));
    }

    /* --- CPU speed (MHz), core count, and processor name --- */
    {
        u32 cpu_speed = 0, cpu_cores = 0;
        char proc_name[128] = "Unknown";
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        cpu_cores = (u32)si.dwNumberOfProcessors;
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0501
        /* Try to get physical core count via GetLogicalProcessorInformation.
         * Use GetProcAddress and only compile this path when targeting XP+ so
         * Win9x builds (WINVER/_WIN32_WINNT < 0x0501) do not require the
         * XP-era typedefs (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, etc.). */
        HMODULE hk32 = GetModuleHandleA("kernel32.dll");
        if (hk32) {
            typedef BOOL (WINAPI *GLPI_t)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);
            GLPI_t pGLPI = (GLPI_t)GetProcAddress(hk32, "GetLogicalProcessorInformation");
            if (pGLPI) {
                DWORD bufLen = 0;
                pGLPI(NULL, &bufLen);
                if (bufLen > 0) {
                    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buf =
                        (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)HeapAlloc(GetProcessHeap(), 0, bufLen);
                    if (buf && pGLPI(buf, &bufLen)) {
                        DWORD count = bufLen / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
                        u32 phys = 0; DWORD i;
                        for (i = 0; i < count; i++) {
                            if (buf[i].Relationship == RelationProcessorCore)
                                phys++;
                        }
                        if (phys > 0) cpu_cores = phys;
                    }
                    if (buf) HeapFree(GetProcessHeap(), 0, buf);
                }
            }
        }
#endif /* _WIN32_WINNT >= 0x0501 */
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD mhz = 0, sz = sizeof(mhz);
            if (RegQueryValueExA(hKey, "~MHz", NULL, NULL, (LPBYTE)&mhz, &sz) == ERROR_SUCCESS)
                cpu_speed = (u32)mhz;
            char buf[128]; sz = sizeof(buf);
            if (RegQueryValueExA(hKey, "ProcessorNameString", NULL, NULL, (LPBYTE)buf, &sz) == ERROR_SUCCESS) {
                buf[sizeof(buf)-1] = 0;
                /* Trim leading spaces */
                char *p = buf; while (*p == ' ') p++;
                if (*p) { snprintf(proc_name, sizeof(proc_name), "%.*s", (int)sizeof(proc_name) - 1, p); }
            }
            RegCloseKey(hKey);
        }
        mem[72] =  cpu_speed       & 0xFF;
        mem[73] = (cpu_speed >>  8) & 0xFF;
        mem[74] = (cpu_speed >> 16) & 0xFF;
        mem[75] = (cpu_speed >> 24) & 0xFF;
        mem[76] =  cpu_cores       & 0xFF;
        mem[77] = (cpu_cores >>  8) & 0xFF;
        mem[78] = (cpu_cores >> 16) & 0xFF;
        mem[79] = (cpu_cores >> 24) & 0xFF;
        syswin32_putptr(mem, 80, syswin32_platstr(vm, &cursor, proc_name));
    }

    /* touch: 0 = no touch (desktop Windows). Web backend sets this later. */
    {
        u32 touch = 0;
        mem[84] =  touch        & 0xFF;
        mem[85] = (touch >>  8) & 0xFF;
        mem[86] = (touch >> 16) & 0xFF;
        mem[87] = (touch >> 24) & 0xFF;
    }

    /* network: 1 = available (wininet is linked on Windows). */
    {
        u32 network = 1;
        mem[88] =  network        & 0xFF;
        mem[89] = (network >>  8) & 0xFF;
        mem[90] = (network >> 16) & 0xFF;
        mem[91] = (network >> 24) & 0xFF;
    }

    syswin32_putptr(mem, 92,  syswin32_platstr(vm, &cursor, RELIC_PLATFORM_ID));
    syswin32_putptr(mem, 96,  syswin32_platstr(vm, &cursor,
                                              view && view->backend_name ? view->backend_name
                                                                         : RELIC_BACKEND_ID));
    syswin32_putptr(mem, 100, syswin32_platstr(vm, &cursor, RELIC_VERSION));
    syswin32_putptr(mem, 104, syswin32_platstr(vm, &cursor, __DATE__));

    /* Adaptive Build tail: detected tier + physical RAM (u32 each). */
    syswin32_putptr(mem, 108, (u32)relic_tier_detect());
    syswin32_putptr(mem, 112, (u32)relic_probe_memory_kb());
}

/* -------------------------------------------------------------------------
 * System clipboard (@clipboard_set / @clipboard_get) - text only, via the
 * Win32 clipboard (CF_TEXT). The implementation lives in
 * common/win32_clip.h (header-style, like local_clip.h) so other Win32
 * backends can wire the same hooks without linking syswin32.c.
 * ---------------------------------------------------------------------- */
#include "../common/win32_clip.h"

void syswin32_sys_clip_set(RelicVM *vm) { w32_clip_set(vm); }
void syswin32_sys_clip_get(RelicVM *vm) { w32_clip_get(vm); }

/* -------------------------------------------------------------------------
 * Web / HTTP - WinINet + CreateThread worker (see docs/web.md)
 *
 * wininet.dll ships on Win95 (with IE3+) through Win11 and performs HTTP and
 * HTTPS (TLS) for us - proxies, redirects and cert handling all in the OS, so
 * no crypto code lands in the VM. WinINet's calls block, so each request runs
 * on a worker thread that fills the slot and flips the `done` flag; the
 * single-threaded VM only polls.
 * ---------------------------------------------------------------------- */

static CRITICAL_SECTION g_http_lock;
static int g_http_lock_ready = 0;

/* Lazily create the HTTP critical section. Only ever called from the VM
 * thread (the single-threaded VM), so no init race is possible. */
static void http_lock_ensure(void) {
    if (!g_http_lock_ready) {
        InitializeCriticalSection(&g_http_lock);
        g_http_lock_ready = 1;
    }
}

static const char *http_method_str(int method) {
    switch (method) {
        case 0:  return "GET";
        case 1:  return "POST";
        case 2:  return "PUT";
        case 3:  return "DELETE";
        default: return "GET";
    }
}

typedef struct {
    SysWin32HttpReq *tbl;
    int              slot;
    int              generation;
    char             method[8];
    char             url[2048];
    char            *headers;      /* malloc'd, or NULL */
    int              headers_len;
    unsigned char   *body;         /* malloc'd, or NULL */
    int              body_len;
} HttpWorkerCtx;

static void http_worker_ctx_free(HttpWorkerCtx *c) {
    if (!c) return;
    free(c->headers);
    free(c->body);
    free(c);
}

/* Store a freshly-created WinINet handle into the slot iff it is still valid
 * (not closed/reused). Returns 1 on success; 0 means the caller must close
 * the handle itself and abandon the request. */
static int http_store_handle(SysWin32HttpReq *tbl, int slot, int gen,
                             HINTERNET *field, HINTERNET h) {
    EnterCriticalSection(&g_http_lock);
    int ok = (tbl[slot].in_use && tbl[slot].generation == gen);
    if (ok) *field = h;
    LeaveCriticalSection(&g_http_lock);
    return ok;
}

static DWORD WINAPI http_worker(LPVOID arg) {
    HttpWorkerCtx *c = (HttpWorkerCtx *)arg;
    SysWin32HttpReq *tbl = c->tbl;
    int slot = c->slot, gen = c->generation;
    int err = 0, status = 0;
    unsigned char *body = NULL;
    long body_len = 0;

    /* Split the URL into host / port / path (+ query). */
    URL_COMPONENTSA uc;
    char host[256] = {0}, path[2048] = {0}, extra[2048] = {0};
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize     = sizeof(uc);
    uc.lpszHostName     = host;  uc.dwHostNameLength   = sizeof(host);
    uc.lpszUrlPath      = path;  uc.dwUrlPathLength    = sizeof(path);
    uc.lpszExtraInfo    = extra; uc.dwExtraInfoLength  = sizeof(extra);
    if (!InternetCrackUrlA(c->url, 0, 0, &uc)) { err = 1; goto finish; }

    int is_https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    INTERNET_PORT port = uc.nPort;
    char fullpath[4096];
    snprintf(fullpath, sizeof(fullpath), "%s%s",
             path[0] ? path : "/", extra);
    fullpath[sizeof(fullpath) - 1] = '\0';

    HINTERNET hs = InternetOpenA("Relic/1.0", INTERNET_OPEN_TYPE_PRECONFIG,
                                 NULL, NULL, 0);
    if (!hs) { err = 1; goto finish; }
    if (!http_store_handle(tbl, slot, gen, &tbl[slot].hSession, hs)) {
        InternetCloseHandle(hs); goto aborted;
    }

    HINTERNET hc = InternetConnectA(hs, host, port, NULL, NULL,
                                    INTERNET_SERVICE_HTTP, 0, 0);
    if (!hc) { err = 1; goto finish; }
    if (!http_store_handle(tbl, slot, gen, &tbl[slot].hConnect, hc)) {
        InternetCloseHandle(hc); goto aborted;
    }

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE
                | INTERNET_FLAG_KEEP_CONNECTION;
    if (is_https) flags |= INTERNET_FLAG_SECURE;
    HINTERNET hr = HttpOpenRequestA(hc, c->method, fullpath, NULL, NULL,
                                    NULL, flags, 0);
    if (!hr) { err = 1; goto finish; }
    if (!http_store_handle(tbl, slot, gen, &tbl[slot].hRequest, hr)) {
        InternetCloseHandle(hr); goto aborted;
    }

    BOOL ok = HttpSendRequestA(
        hr,
        c->headers_len > 0 ? c->headers : NULL,
        c->headers_len > 0 ? (DWORD)c->headers_len : 0,
        c->body_len > 0 ? c->body : NULL,
        c->body_len > 0 ? (DWORD)c->body_len : 0);
    if (!ok) { err = 1; goto finish; }

    /* HTTP status code. */
    {
        DWORD sc = 0, sz = sizeof(sc), idx = 0;
        if (HttpQueryInfoA(hr, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                           &sc, &sz, &idx)) {
            status = (int)sc;
        }
    }

    /* Drain the response body into a growing heap buffer. */
    {
        long cap = 0;
        for (;;) {
            if (body_len + 8192 > cap) {
                cap = body_len + 8192;
                unsigned char *nb = (unsigned char *)realloc(body, (size_t)cap);
                if (!nb) { err = 1; break; }
                body = nb;
            }
            DWORD got = 0;
            if (!InternetReadFile(hr, body + body_len, 8192, &got)) { err = 1; break; }
            if (got == 0) break;
            body_len += (long)got;
        }
    }

finish:
    EnterCriticalSection(&g_http_lock);
    if (tbl[slot].in_use && tbl[slot].generation == gen) {
        tbl[slot].data     = body;
        tbl[slot].data_len = body_len;
        tbl[slot].status   = status;
        tbl[slot].done     = err ? -1 : 1;
        /* Take ownership of the handles so we can close them after unlocking;
         * http_close will see NULL and skip them. */
        HINTERNET h1 = tbl[slot].hRequest;
        HINTERNET h2 = tbl[slot].hConnect;
        HINTERNET h3 = tbl[slot].hSession;
        tbl[slot].hRequest = NULL;
        tbl[slot].hConnect = NULL;
        tbl[slot].hSession = NULL;
        LeaveCriticalSection(&g_http_lock);
        if (h1) InternetCloseHandle(h1);
        if (h2) InternetCloseHandle(h2);
        if (h3) InternetCloseHandle(h3);
    } else {
        /* Slot was closed/reused while we ran: http_close already released the
         * stored handles. Just drop our body buffer. */
        LeaveCriticalSection(&g_http_lock);
        free(body);
    }
    http_worker_ctx_free(c);
    return 0;

aborted:
    /* Slot was closed during setup; handles we stored are owned by http_close,
     * and the un-stored handle was closed at the goto site. */
    free(body);
    http_worker_ctx_free(c);
    return 0;
}

/* @http_request(method, url, headers, body, body_len) -> i32 handle */
void syswin32_sys_http_request(RelicVM *vm, SysWin32HttpReq *tbl) {
    if (!tbl) { vm->r[1] = -1; return; }
    http_lock_ensure();

    int method   = vm->r[1];
    u32 url_ptr  = (u32)vm->r[2];
    u32 hdr_ptr  = (u32)vm->r[3];
    u32 body_ptr = (u32)vm->r[4];
    i32 body_len = vm->r[5];

    if (url_ptr == 0 || url_ptr >= vm->mem_sz) { vm->r[1] = -1; return; }

    HttpWorkerCtx *c = (HttpWorkerCtx *)calloc(1, sizeof(HttpWorkerCtx));
    if (!c) { vm->r[1] = -1; return; }

    /* Copy URL out of VM memory. */
    int ul = 0;
    while (ul < (int)sizeof(c->url) - 1 && url_ptr + (u32)ul < vm->mem_sz
           && vm->mem[url_ptr + ul] != 0) {
        c->url[ul] = (char)vm->mem[url_ptr + ul]; ul++;
    }
    c->url[ul] = '\0';
    if (ul == 0) { free(c); vm->r[1] = -1; return; }

    /* Copy headers (NUL-terminated block) out of VM memory. */
    if (hdr_ptr != 0 && hdr_ptr < vm->mem_sz) {
        int hl = 0;
        while (hdr_ptr + (u32)hl < vm->mem_sz && vm->mem[hdr_ptr + hl] != 0) hl++;
        if (hl > 0) {
            c->headers = (char *)malloc((size_t)hl);
            if (c->headers) {
                memcpy(c->headers, vm->mem + hdr_ptr, (size_t)hl);
                c->headers_len = hl;
            }
        }
    }

    /* Copy request body out of VM memory. */
    if (body_ptr != 0 && body_len > 0 && body_ptr + (u32)body_len <= vm->mem_sz) {
        c->body = (unsigned char *)malloc((size_t)body_len);
        if (c->body) {
            memcpy(c->body, vm->mem + body_ptr, (size_t)body_len);
            c->body_len = body_len;
        }
    }

    strncpy(c->method, http_method_str(method), sizeof(c->method) - 1);
    c->method[sizeof(c->method) - 1] = '\0';
    c->tbl = tbl;

    /* Allocate a slot. */
    int slot = -1;
    EnterCriticalSection(&g_http_lock);
    for (int i = 0; i < SYSWIN32_MAX_HTTP; i++) {
        if (!tbl[i].in_use) {
            slot = i;
            tbl[i].in_use     = true;
            tbl[i].generation++;
            tbl[i].done       = 0;
            tbl[i].status     = 0;
            tbl[i].data       = NULL;
            tbl[i].data_len   = 0;
            tbl[i].read_pos   = 0;
            tbl[i].hSession   = NULL;
            tbl[i].hConnect   = NULL;
            tbl[i].hRequest   = NULL;
            break;
        }
    }
    if (slot >= 0) {
        c->slot = slot;
        c->generation = tbl[slot].generation;
    }
    LeaveCriticalSection(&g_http_lock);
    if (slot < 0) { http_worker_ctx_free(c); vm->r[1] = -1; return; }

    HANDLE th = CreateThread(NULL, 0, http_worker, c, 0, NULL);
    if (!th) {
        EnterCriticalSection(&g_http_lock);
        tbl[slot].in_use = false;
        LeaveCriticalSection(&g_http_lock);
        http_worker_ctx_free(c);
        vm->r[1] = -1;
        return;
    }
    CloseHandle(th);  /* detach; worker frees its own ctx */
    vm->r[1] = slot;
}

/* @http_poll(handle) -> i32 (0=pending, 1=ready, <0=error) */
void syswin32_sys_http_poll(RelicVM *vm, SysWin32HttpReq *tbl) {
    if (!tbl) { vm->r[1] = -1; return; }
    int handle = vm->r[1];
    if (handle < 0 || handle >= SYSWIN32_MAX_HTTP) { vm->r[1] = -1; return; }
    http_lock_ensure();
    EnterCriticalSection(&g_http_lock);
    int done = tbl[handle].in_use ? tbl[handle].done : -1;
    LeaveCriticalSection(&g_http_lock);
    vm->r[1] = done;
}

/* @http_status(handle) -> i32 HTTP status code */
void syswin32_sys_http_status(RelicVM *vm, SysWin32HttpReq *tbl) {
    if (!tbl) { vm->r[1] = -1; return; }
    int handle = vm->r[1];
    if (handle < 0 || handle >= SYSWIN32_MAX_HTTP) { vm->r[1] = -1; return; }
    http_lock_ensure();
    EnterCriticalSection(&g_http_lock);
    int status = tbl[handle].in_use ? tbl[handle].status : -1;
    LeaveCriticalSection(&g_http_lock);
    vm->r[1] = status;
}

/* @http_read(handle, buf, cap) -> i32 bytes copied (0=EOF) */
void syswin32_sys_http_read(RelicVM *vm, SysWin32HttpReq *tbl) {
    if (!tbl) { vm->r[1] = -1; return; }
    int handle  = vm->r[1];
    u32 buf_ptr = (u32)vm->r[2];
    i32 cap     = vm->r[3];
    if (handle < 0 || handle >= SYSWIN32_MAX_HTTP || cap <= 0
        || buf_ptr == 0 || buf_ptr >= vm->mem_sz) { vm->r[1] = -1; return; }
    http_lock_ensure();

    EnterCriticalSection(&g_http_lock);
    if (!tbl[handle].in_use) { LeaveCriticalSection(&g_http_lock); vm->r[1] = -1; return; }
    long avail = tbl[handle].data_len - tbl[handle].read_pos;
    if (avail <= 0) { LeaveCriticalSection(&g_http_lock); vm->r[1] = 0; return; }
    long to_copy = avail < cap ? avail : cap;
    if (buf_ptr + (u32)to_copy > vm->mem_sz) to_copy = vm->mem_sz - buf_ptr;
    memcpy(vm->mem + buf_ptr, tbl[handle].data + tbl[handle].read_pos, (size_t)to_copy);
    tbl[handle].read_pos += to_copy;
    LeaveCriticalSection(&g_http_lock);
    vm->r[1] = (i32)to_copy;
}

/* @http_close(handle) */
void syswin32_sys_http_close(RelicVM *vm, SysWin32HttpReq *tbl) {
    if (!tbl) return;
    int handle = vm->r[1];
    if (handle < 0 || handle >= SYSWIN32_MAX_HTTP) return;
    http_lock_ensure();

    EnterCriticalSection(&g_http_lock);
    if (!tbl[handle].in_use) { LeaveCriticalSection(&g_http_lock); return; }
    tbl[handle].in_use = false;
    tbl[handle].generation++;   /* invalidate any in-flight worker */
    HINTERNET h1 = tbl[handle].hRequest;
    HINTERNET h2 = tbl[handle].hConnect;
    HINTERNET h3 = tbl[handle].hSession;
    tbl[handle].hRequest = NULL;
    tbl[handle].hConnect = NULL;
    tbl[handle].hSession = NULL;
    unsigned char *data = tbl[handle].data;
    tbl[handle].data     = NULL;
    tbl[handle].data_len = 0;
    tbl[handle].read_pos = 0;
    LeaveCriticalSection(&g_http_lock);

    /* Close child-first so closing the session root never double-closes a
     * child. Closing an in-flight handle aborts the worker's blocking call. */
    if (h1) InternetCloseHandle(h1);
    if (h2) InternetCloseHandle(h2);
    if (h3) InternetCloseHandle(h3);
    free(data);
}

/* Called from backend shutdown to clean up all in-flight requests. */
void syswin32_http_shutdown(SysWin32HttpReq *tbl) {
    if (!tbl || !g_http_lock_ready) return;
    for (int i = 0; i < SYSWIN32_MAX_HTTP; i++) {
        EnterCriticalSection(&g_http_lock);
        HINTERNET h1 = NULL, h2 = NULL, h3 = NULL;
        unsigned char *data = NULL;
        if (tbl[i].in_use) {
            tbl[i].in_use = false;
            tbl[i].generation++;
            h1 = tbl[i].hRequest; h2 = tbl[i].hConnect; h3 = tbl[i].hSession;
            tbl[i].hRequest = NULL; tbl[i].hConnect = NULL; tbl[i].hSession = NULL;
            data = tbl[i].data;
            tbl[i].data = NULL;
        }
        LeaveCriticalSection(&g_http_lock);
        if (h1) InternetCloseHandle(h1);
        if (h2) InternetCloseHandle(h2);
        if (h3) InternetCloseHandle(h3);
        free(data);
    }
}

/* -------------------------------------------------------------------------
 * Console-only backend object (tigryx_rt_console)
 *
 * No window, GL context or audio device: every gfx/inp/snd hook is left
 * NULL and vm.c degrades them safely. The sys_* hooks that need no
 * graphics — platform info, clipboard, HTTP — are real.
 * ---------------------------------------------------------------------- */

typedef struct {
    SysWin32HttpReq http[SYSWIN32_MAX_HTTP];
} SysWin32State;

static SysWin32State *syswin32_state(RelicVM *vm) {
    return (SysWin32State *)vm->backend_data;
}

static void syswin32_init(RelicVM *vm) {
    vm->backend_data = calloc(1, sizeof(SysWin32State));
}

static void syswin32_shutdown(RelicVM *vm) {
    SysWin32State *s = syswin32_state(vm);
    if (s) {
        syswin32_http_shutdown(s->http);
        free(s);
        vm->backend_data = NULL;
    }
}

static void syswin32_platform(RelicVM *vm) {
    SysWin32PlatformView v;
    memset(&v, 0, sizeof(v));
    v.display_mode = "Text";    /* console exes have a console, not a window */
    v.gfx_arch     = "Null";
    v.gpu_str      = NULL;      /* reported as "None" */
    v.gl_version   = NULL;      /* reported as "0.0" */
    v.sound_flags  = 0;         /* no audio hooks */
    v.backend_name = RELIC_BACKEND_ID;
    syswin32_sys_platform(vm, &v);
}

static void syswin32_http_req(RelicVM *vm) {
    SysWin32State *s = syswin32_state(vm);
    syswin32_sys_http_request(vm, s ? s->http : NULL);
}
static void syswin32_http_pol(RelicVM *vm) {
    SysWin32State *s = syswin32_state(vm);
    syswin32_sys_http_poll(vm, s ? s->http : NULL);
}
static void syswin32_http_st(RelicVM *vm) {
    SysWin32State *s = syswin32_state(vm);
    syswin32_sys_http_status(vm, s ? s->http : NULL);
}
static void syswin32_http_rd(RelicVM *vm) {
    SysWin32State *s = syswin32_state(vm);
    syswin32_sys_http_read(vm, s ? s->http : NULL);
}
static void syswin32_http_cl(RelicVM *vm) {
    SysWin32State *s = syswin32_state(vm);
    syswin32_sys_http_close(vm, s ? s->http : NULL);
}

RelicBackend relic_syswin_backend = {
    .init            = syswin32_init,
    .shutdown        = syswin32_shutdown,
    .sys_platform    = syswin32_platform,
    .sys_http_request = syswin32_http_req,
    .sys_http_poll    = syswin32_http_pol,
    .sys_http_status  = syswin32_http_st,
    .sys_http_read    = syswin32_http_rd,
    .sys_http_close   = syswin32_http_cl,
    .sys_clip_set     = syswin32_sys_clip_set,
    .sys_clip_get     = syswin32_sys_clip_get,
};
