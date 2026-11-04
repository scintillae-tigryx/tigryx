/*
 * tier.c
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

/* tier.c — Adaptive Build tier detection. See include/tigryx_tier.h.
 *
 * Compiled into the core library, the standalone preprocessor, the
 * dispatcher and the rt archives. Everything OS-specific lives behind
 * preprocessor guards so the same file builds on Win32, Linux, macOS,
 * DOS (DJGPP and gcc-ia16 real mode) and Emscripten: on platforms with
 * nothing worth probing, the compile-time ceiling decides and every
 * probe returns "unknown" rather than guessing. */

#include "tigryx_tier.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef RELIC_PLATFORM_ID
#  define RELIC_PLATFORM_ID "unknown"
#endif
#ifndef RELIC_BACKEND_ID
#  define RELIC_BACKEND_ID "unknown"
#endif

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
/* probe_cpu_mhz reads HKLM\...\CentralProcessor\0\~MHz - advapi32 is
 * implicit under MinGW but not MSVC; keep the dependency self-contained
 * so every target that links tier.c resolves it. */
#  if defined(_MSC_VER)
#    pragma comment(lib, "advapi32.lib")
#  endif
#endif
#if defined(__APPLE__)
#  include <sys/sysctl.h>
#endif
#if defined(__linux__) || defined(__sun) || defined(__unix__)
#  include <unistd.h>
#endif
#if defined(__linux__)
#  include <ctype.h>
#endif
#if defined(__EMSCRIPTEN__)
#  include <emscripten.h>
#endif
#if defined(RELIC_TOS)
#  include <osbind.h>
/* phystop (0x42e) is supervisor-only; Supexec runs this in that mode. */
static long tier_phystop(void) { return *(volatile long *)0x42eL; }
#endif

/* ------------------------------------------------------------ */
/* names                                                        */
/* ------------------------------------------------------------ */

static const char *const TIER_NAMES[4] = { "low", "medium", "high", "super" };

int relic_tier_parse(const char *name) {
    if(!name) return -1;
    for(int i = 0; i < 4; i++) {
        const char *a = name, *b = TIER_NAMES[i];
        while(*a && *b) {
            char ca = *a++;
            if(ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if(ca != *b++) break;
        }
        if(!*b && !*a) return i;
    }
    return -1;
}

const char *relic_tier_name(int tier) {
    if(tier < 0 || tier > 3) return "?";
    return TIER_NAMES[tier];
}

/* ------------------------------------------------------------ */
/* compile-time ceiling                                         */
/* ------------------------------------------------------------ */

static int str_has(const char *s, const char *sub) {
    return strstr(s, sub) != NULL;
}

/* Cross-compile identity override (see relic_tier_set_platform in the
 * header). NULL = use the compiled-in RELIC_PLATFORM_ID/BACKEND_ID. */
static const char *g_platform_id = NULL;
static const char *g_backend_id  = NULL;

void relic_tier_set_platform(const char *platform_id, const char *backend_id) {
    g_platform_id = platform_id;
    g_backend_id  = backend_id;
}
const char *relic_tier_platform(void) {
    return g_platform_id ? g_platform_id : RELIC_PLATFORM_ID;
}
const char *relic_tier_backend(void) {
    return g_backend_id ? g_backend_id : RELIC_BACKEND_ID;
}

/* Map RELIC_PLATFORM_ID to the best tier it can ship. Order matters:
 * specific ids are tested before generic substrings ("relic3d" would
 * otherwise also match "win95-relic3d" — which happens to be the same
 * tier anyway, but "x86" must come last). */
int relic_tier_ceiling(void) {
    const char *p = relic_tier_platform();
    if(str_has(p, "gccia16") || str_has(p, "bbc"))            return RELIC_TIER_LOW;
    /* Solaris targets UltraSPARC-or-better hardware only (V8 SPARC is
     * the 32-bit variant's baseline): even the software-rendered
     * solaris-*-relic3d builds run on Pentium-II-class machines, so they
     * must not inherit MEDIUM from the generic "relic3d" substring. */
    if(str_has(p, "solaris"))                                 return RELIC_TIER_HIGH;
    if(str_has(p, "win95")   || str_has(p, "frelic3d")  ||
       str_has(p, "videl")   ||
       str_has(p, "tos")    || str_has(p, "st-shifter") ||
       str_has(p, "relic3d"))                               return RELIC_TIER_MEDIUM;
    if(str_has(p, "riscos")) {
        /* One OS, five decades of silicon. ARM2/ARM3 (Archimedes,
         * ARM610/710 Risc PC) sit with the 68k/Win95 machines;
         * StrongARM, the Iyonix's XScale and the Pi's Cortex-A7 are
         * Pentium-II-to-netbook class. Nothing RISC OS runs on earns
         * SUPER: the OS itself is single-core and <=1GB. */
        if(str_has(p, "-arm2") || str_has(p, "-arm3"))
            return RELIC_TIER_MEDIUM;
        return RELIC_TIER_HIGH;
    }
    if(str_has(p, "tinygl"))                                  return RELIC_TIER_HIGH;
    if(str_has(p, "x86") || str_has(p, "sparc"))              return RELIC_TIER_HIGH;
    return RELIC_TIER_SUPER;
}

/* ------------------------------------------------------------ */
/* runtime probe (demote-only)                                  */
/* ------------------------------------------------------------ */

unsigned long relic_probe_memory_kb(void) {
#if defined(_WIN32)
    /* GlobalMemoryStatusEx first (correct >4GB), GlobalMemoryStatus as a
     * 9x-era fallback (saturates near 2GB/4GB — good enough for the
     * SUPER threshold, which sits at 2GB). The extended call is
     * GetProcAddress'd because Win9x kernel32 lacks it, and the struct
     * is declared locally because the SDK gates MEMORYSTATUSEX behind
     * _WIN32_WINNT >= 0x0500 while the Win95 builds compile for 0x0400
     * with headers that predate it anyway. */
    typedef struct {
        DWORD     dwLength;
        DWORD     dwMemoryLoad;
        DWORDLONG ullTotalPhys;
        DWORDLONG ullAvailPhys;
        DWORDLONG ullTotalPageFile;
        DWORDLONG ullAvailPageFile;
        DWORDLONG ullTotalVirtual;
        DWORDLONG ullAvailVirtual;
        DWORDLONG ullAvailExtendedVirtual;
    } TIER_MEMSTATUSEX;
    HMODULE hk = GetModuleHandleA("kernel32.dll");
    if(hk) {
        typedef BOOL (WINAPI *GMSE_t)(TIER_MEMSTATUSEX *);
        GMSE_t pGMSE = (GMSE_t)GetProcAddress(hk, "GlobalMemoryStatusEx");
        if(pGMSE) {
            TIER_MEMSTATUSEX ms;
            ms.dwLength = sizeof(ms);
            if(pGMSE(&ms))
                return (unsigned long)(ms.ullTotalPhys / 1024u);
        }
    }
    {
        MEMORYSTATUS ms;
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatus(&ms);
        return (unsigned long)(ms.dwTotalPhys / 1024u);
    }
#elif defined(__APPLE__)
    unsigned long long mem = 0;
    size_t sz = sizeof(mem);
    if(sysctlbyname("hw.memsize", &mem, &sz, NULL, 0) == 0 && mem > 0)
        return (unsigned long)(mem / 1024u);
    return 0;
#elif defined(__linux__) || defined(__sun)
    {
        long pages = sysconf(_SC_PHYS_PAGES);
        long psz   = sysconf(_SC_PAGESIZE);
        if(pages > 0 && psz > 0)
            return (unsigned long)((unsigned long long)pages *
                                   (unsigned long long)psz / 1024u);
    }
    return 0;
#elif defined(__EMSCRIPTEN__)
    /* navigator.deviceMemory is Chrome-only and coarse (GB, powers of 2);
     * 0 means "not reported". emscripten_run_script_int rather than
     * EM_ASM_INT: EM_ASM_* needs -std=gnu*, and this file is built under
     * strict -std=c* for the wasm targets. */
    return (unsigned long)emscripten_run_script_int(
        "(typeof navigator.deviceMemory === 'number')"
        " ? navigator.deviceMemory * 1048576 : 0");
#elif defined(RELIC_TOS)
    /* phystop = top of ST-RAM. TT-RAM (if any) is not counted - this is
     * only a hint, and the ceiling is MEDIUM either way. */
    {
        long p = Supexec(tier_phystop);
        return p > 0 ? (unsigned long)(p / 1024u) : 0;
    }
#else
    return 0;
#endif
}

/* CPU core/thread count, 0 = unknown. */
static int probe_cores(void) {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#elif defined(__APPLE__)
    int n = 0; size_t sz = sizeof(n);
    if(sysctlbyname("hw.ncpu", &n, &sz, NULL, 0) == 0) return n;
    return 0;
    /* DJGPP and gcc-ia16 both define __unix__ but ship neither
     * _SC_NPROCESSORS_ONLN nor a meaningful core count - DOS is
     * single-tasking, so "unknown" is the honest answer. */
#elif defined(__linux__) || defined(__sun) || \
      (defined(__unix__) && !defined(RELIC_TOS) && \
       !defined(__DJGPP__) && !defined(__ia16__))
    {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        return n > 0 ? (int)n : 0;
    }
#elif defined(__EMSCRIPTEN__)
    return emscripten_run_script_int(
        "navigator.hardwareConcurrency | 0");
#else
    return 0;
#endif
}

/* CPU clock in MHz, 0 = unknown. A weak-but-known signal, so it only
 * ever demotes; unknown reads as "not slower". */
static int probe_cpu_mhz(void) {
#if defined(_WIN32)
    DWORD mhz = 0, sz = sizeof(mhz);
    HKEY hk;
    if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0, KEY_READ, &hk) == ERROR_SUCCESS) {
        RegQueryValueExA(hk, "~MHz", NULL, NULL, (LPBYTE)&mhz, &sz);
        RegCloseKey(hk);
    }
    return (int)mhz;
#elif defined(__linux__)
    {
        FILE *f = fopen("/proc/cpuinfo", "r");
        char line[256];
        int mhz = 0;
        if(!f) return 0;
        while(fgets(line, sizeof line, f)) {
            if(strncmp(line, "cpu MHz", 7) == 0) {
                const char *c = strchr(line, ':');
                if(c) mhz = atoi(c + 1);
                break;
            }
        }
        fclose(f);
        return mhz;
    }
#elif defined(__APPLE__)
    unsigned long long hz = 0; size_t sz = sizeof(hz);
    if(sysctlbyname("hw.cpufrequency", &hz, &sz, NULL, 0) == 0 && hz > 0)
        return (int)(hz / 1000000u);
    return 0;
#else
    return 0;
#endif
}

int relic_tier_probe(void) {
    int cores = probe_cores();
    unsigned long mem_kb = relic_probe_memory_kb();
    int mhz = probe_cpu_mhz();
    /* SUPER = "2016-era desktop or better". Conservative demote rules —
     * anything not measured reads as adequate, so VMs and containers
     * that report partial info don't get unfairly pushed down. */
    if(cores > 0 && cores < 2)                    return RELIC_TIER_HIGH;
    if(mem_kb > 0 && mem_kb < 2UL * 1024 * 1024)  return RELIC_TIER_HIGH;
    if(mhz > 0 && mhz < 1200)                     return RELIC_TIER_HIGH;
    if(cores == 0 && mem_kb == 0 && mhz == 0)     return -1;
    return RELIC_TIER_SUPER;
}

/* ------------------------------------------------------------ */
/* effective tier                                               */
/* ------------------------------------------------------------ */

static int g_tier_override = -1;

void relic_tier_set_override(int tier) { g_tier_override = tier; }
int  relic_tier_override(void)         { return g_tier_override; }

int relic_tier_set_override_name(const char *name, char *err, int errcap) {
    int t = relic_tier_parse(name);
    if(t < 0) {
        if(err) snprintf(err, (size_t)errcap,
                         "unknown tier '%s' (low|medium|high|super)", name);
        return -1;
    }
    if(t > relic_tier_ceiling()) {
        if(err) snprintf(err, (size_t)errcap,
                         "tier '%s' is above this build's ceiling '%s' "
                         "(platform %s)", name,
                         relic_tier_name(relic_tier_ceiling()),
                         relic_tier_platform());
        return -2;
    }
    g_tier_override = t;
    return 0;
}

int relic_tier_detect(void) {
    if(g_tier_override >= 0) return g_tier_override;
    {
        const char *e = getenv("TGX_TIER");
        if(e && *e) {
            int t = relic_tier_parse(e);
            if(t >= 0) {
                int ceil = relic_tier_ceiling();
                return t > ceil ? ceil : t;
            }
        }
    }
    {
        int ceil = relic_tier_ceiling();
        if(ceil != RELIC_TIER_SUPER) return ceil;
        {
            int p = relic_tier_probe();
            return (p >= 0 && p < ceil) ? p : ceil;
        }
    }
}

/* ------------------------------------------------------------ */
/* backend capability flags                                     */
/* ------------------------------------------------------------ */

unsigned int relic_backend_caps(void) {
    const char *b = relic_tier_backend();
    const char *p = relic_tier_platform();
    unsigned int c = 0;

    if(str_has(b, "metal-ios")) {
        /* metal-ios: 2D sprite/texture + file I/O are real; @mesh and
         * all audio are stubs, so MESH/PCM/TONE
         * stay unadvertised. Touch is the native input modality. */
        c |= RELIC_CAP_TEXTURE | RELIC_CAP_SPRITE | RELIC_CAP_GPU |
             RELIC_CAP_FILES | RELIC_CAP_TOUCH;
    } else if(str_has(b, "opengl") || str_has(b, "metal")) {
        c |= RELIC_CAP_MESH | RELIC_CAP_TEXTURE | RELIC_CAP_SPRITE |
             RELIC_CAP_GPU | RELIC_CAP_FILES | RELIC_CAP_PCM | RELIC_CAP_TONE;
        if(str_has(b, "emscripten")) c |= RELIC_CAP_TOUCH;
    } else if(str_has(b, "allegro")) {
        c |= RELIC_CAP_MESH | RELIC_CAP_TEXTURE | RELIC_CAP_SPRITE |
             RELIC_CAP_PALETTE8 | RELIC_CAP_FILES |
             RELIC_CAP_PCM | RELIC_CAP_TONE;
    } else if(str_has(b, "st-tos")) {
        /* ST/STe shifter: 4/2/1-plane palette video (16/4/2 colours),
         * guest file access. No sound hooks yet (STe DMA + PSG tones
         * are planned), so sound_flags reports 0 and PCM/TONE stay
         * unadvertised. */
        c |= RELIC_CAP_PALETTE8 | RELIC_CAP_FILES;
    } else if(str_has(b, "videl")) {
        /* Falcon030: DMA PCM + soundchip tones (sys_platform reports
         * sound_flags=3), sw-rendered meshes, palette-indexed video. */
        c |= RELIC_CAP_MESH | RELIC_CAP_TEXTURE | RELIC_CAP_SPRITE |
             RELIC_CAP_PALETTE8 | RELIC_CAP_FILES |
             RELIC_CAP_PCM | RELIC_CAP_TONE;
    } else if(str_has(b, "relic3d")) {
        /* r3d_snd_play/r3d_snd_music play real guest PCM + MIDI. */
        c |= RELIC_CAP_MESH | RELIC_CAP_TEXTURE | RELIC_CAP_SPRITE |
             RELIC_CAP_PALETTE8 | RELIC_CAP_FILES |
             RELIC_CAP_PCM | RELIC_CAP_TONE;
    } else if(str_has(b, "vidc")) {
        /* RISC OS VIDC/GraphicsV 8bpp: sw3d meshes, sprite/texture,
         * palette video. SoundDMA gives PCM+tones on every IOMD machine
         * (arm3 and up); arm2/VIDC10 has no SoundDMA so PCM stays
         * unadvertised there - the hooks still no-op safely if called. */
        c |= RELIC_CAP_MESH | RELIC_CAP_TEXTURE | RELIC_CAP_SPRITE |
             RELIC_CAP_PALETTE8 | RELIC_CAP_FILES;
        if(!str_has(p, "-arm2"))
            c |= RELIC_CAP_PCM | RELIC_CAP_TONE;
    } else if(str_has(b, "vga")) {
        /* mode 13h/12h: no @mesh — sprite/texture/palette are real;
         * vga_snd_play does real SoundBlaster DMA PCM. */
        c |= RELIC_CAP_TEXTURE | RELIC_CAP_SPRITE |
             RELIC_CAP_PALETTE8 | RELIC_CAP_FILES |
             RELIC_CAP_PCM | RELIC_CAP_TONE;
    } else if(str_has(b, "tgp")) {
        /* TGP terminal: textures, sprites and wr_* world rendering are
         * real ops on the wire; 3D mesh is deliberately not advertised
         * until the terminal's vertex-colour path is fixed and tested. */
        c |= RELIC_CAP_TEXTURE | RELIC_CAP_SPRITE |
             RELIC_CAP_FILES;
    } else if(str_has(b, "bbc")) {
        /* 65C102 parasite: SN76489 tones, lines/lines3d wireframe,
         * palette video; no sprites, textures or guest file access. */
        c |= RELIC_CAP_TONE;
    } else if(str_has(b, "null")) {
        c |= RELIC_CAP_FILES;
    }

    /* Platform-side caps (orthogonal to the backend). */
    if(str_has(p, "frelic3d") || str_has(p, "gccia16") || str_has(p, "bbc"))
        c |= RELIC_CAP_NO_FPU;
    /* Every pre-Pi Acorn build is soft-float (the FPA ABI is emulated);
     * only -arm7 has real VFP hardware. */
    if(str_has(p, "riscos") && !str_has(p, "-arm7"))
        c |= RELIC_CAP_NO_FPU;
    if(str_has(p, "bbc"))
        c |= RELIC_CAP_NO_F32 | RELIC_CAP_MEM_64K;

    return c;
}
