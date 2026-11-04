/*
 * tigryx_tier.h
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

/* tigryx_tier.h — the Adaptive Build: platform "capability tier" detection.
 *
 * Tiers bucket the machine a build of the toolchain runs on, so a program
 * can reshape itself via the preprocessor (#if TGX_TIER >= TGX_TIER_HIGH,
 * #ifdef TGX_LOW, capability macros like TGX_HAS_MESH). The per-target
 * tier ceilings are defined in src/core/tier.c.
 *
 *   LOW    1980s    BBC co-processor, 8086/286 DOS        (vga-dos, min-vm)
 *   MEDIUM 1990s    Atari Falcon, Pentium DOS, Win95/98   (sw 3D, 8bpp)
 *   HIGH   2003+    early-GL desktops, i386, TinyGL DOS   (hw 3D, low-res)
 *   SUPER  2016+    modern x64/ARM64 desktop, WebGL2      (textures, fog)
 *
 * Detection is layered (relic_tier_detect):
 *   1. explicit override  -- --tier on the tool, relic_tier_set_override(),
 *                            or `#pragma arg tier` in the source (applied by
 *                            the preprocessor/tool, not visible here).
 *   2. TGX_TIER environment variable.
 *   3. auto: the compile-time ceiling from RELIC_PLATFORM_ID, refined
 *      downward by an OS probe (cores/RAM/MHz) where the ceiling is SUPER.
 *
 * This file is deliberately dependency-free (no tigryx.h, no RelicVM) so it
 * can be linked into tigryxpp, the dispatcher, tigryxri, tigryxcc, and
 * the rt archives alike.
 */
#ifndef TIGRYX_TIER_H
#define TIGRYX_TIER_H

#ifdef __cplusplus
extern "C" {
#endif

#define RELIC_TIER_LOW    0
#define RELIC_TIER_MEDIUM 1
#define RELIC_TIER_HIGH   2
#define RELIC_TIER_SUPER  3

/* Capability bitmask for relic_backend_caps(). Mirrored as TGX_HAS_* /
 * TGX_NO_* / TGX_MEM_64K preprocessor defines in tigryxpp — keep the
 * names in tier_apply_defines() (tigryxpp.c) in step. */
#define RELIC_CAP_MESH      0x0001u  /* @mesh implemented                */
#define RELIC_CAP_TEXTURE   0x0002u  /* @texture implemented             */
#define RELIC_CAP_SPRITE    0x0004u  /* @sprite implemented              */
#define RELIC_CAP_GPU       0x0008u  /* hardware 3D (GL/Metal), not sw   */
#define RELIC_CAP_FILES     0x0010u  /* @file_* read/write               */
#define RELIC_CAP_PCM       0x0020u  /* @play / sample audio             */
#define RELIC_CAP_TONE      0x0040u  /* tone/beeper audio                */
#define RELIC_CAP_PALETTE8  0x0080u  /* 8bpp indexed, @palette_upload    */
#define RELIC_CAP_NO_FPU    0x0100u  /* FP is emulated/soft — prefer fix */
#define RELIC_CAP_NO_F32    0x0200u  /* no f32 opcodes at all (BBC)      */
#define RELIC_CAP_MEM_64K   0x0400u  /* flat 64K address space (BBC)     */
#define RELIC_CAP_TOUCH     0x0800u  /* touch input plausible            */

/* Parse a tier name ("low".."super", case-insensitive). Returns 0..3 or
 * -1 if the name is not a tier. */
int  relic_tier_parse(const char *name);

/* Canonical lowercase name for a tier ordinal; "?" if out of range. */
const char *relic_tier_name(int tier);

/* Highest tier this build of the toolchain can plausibly target, from
 * RELIC_PLATFORM_ID alone (no OS calls). A `dos-relic2d-gccia16-x86` build can
 * never be asked for SUPER; a desktop x64 build tops out at SUPER and may
 * be refined down by relic_tier_probe(). */
int  relic_tier_ceiling(void);

/* OS/hardware probe: returns RELIC_TIER_HIGH on a desktop that is too
 * weak for SUPER, -1 when there is nothing to probe (embedded/retro
 * targets, or nothing measured). Never promotes — only demotes. */
int  relic_tier_probe(void);

/* Effective tier for this process: override > TGX_TIER env > auto
 * (min(ceiling, probe)). This is the single source of truth used by the
 * preprocessor's defines, the .tlx tier stamp, and the mismatch check. */
int  relic_tier_detect(void);

/* Explicit override (-1 clears). Callers that take a user-supplied name
 * should use relic_tier_set_override_name instead, which validates. */
void relic_tier_set_override(int tier);
int  relic_tier_override(void);

/* Validate a user-supplied tier name and set it as the override.
 * Returns 0 on success; -1 if the name is not a tier; -2 if it names a
 * tier above relic_tier_ceiling() (nothing is set in either failure, and
 * `err` receives a short message when non-NULL). */
int  relic_tier_set_override_name(const char *name, char *err, int errcap);

/* Capability bitmask for the *build* (RELIC_BACKEND_ID + RELIC_PLATFORM_ID),
 * independent of the requested tier: `--tier low` on an OpenGL desktop
 * still reports MESH|GPU, because the binary genuinely has them. The tier
 * decides which code the game compiles; the caps describe the machine. */
unsigned int relic_backend_caps(void);

/* Cross-compile identity override: a hosted compiler (e.g. the Windows-
 * hosted TOS tigryxcc) spawns a tigryxpp whose own RELIC_PLATFORM_ID/
 * RELIC_BACKEND_ID describe the *host*, not the target. --platform /
 * --backend on tigryxpp call this so the ceiling, capability bits and
 * the TGX_PLATFORM/TGX_BACKEND macros reflect the build target.
 * NULL restores the compiled-in ids. */
void relic_tier_set_platform(const char *platform_id, const char *backend_id);
const char *relic_tier_platform(void);   /* effective platform id */
const char *relic_tier_backend(void);    /* effective backend id  */

/* Physical RAM in KB, 0 = unknown. Probes the host OS on desktop targets;
 * returns 0 on retro targets where "RAM" isn't a useful runtime signal
 * (their ceiling already pins the tier). */
unsigned long relic_probe_memory_kb(void);

#ifdef __cplusplus
}
#endif

#endif /* TIGRYX_TIER_H */
