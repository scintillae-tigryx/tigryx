/*
 * tigryx_pp.h
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
 * tigryx_pp.h - in-process entry point for the Tigryx preprocessor.
 *
 * The standalone `tigryxpp` executable (src/preprocessor/tigryxpp.c) is
 * normally invoked as a subprocess by tigryxri/tigryx (see main.c). On
 * platforms with no process model - Emscripten, and the in-browser
 * tigryxwc compiler - there is nothing to spawn, so tigryxri calls this
 * function directly instead.
 *
 * tigryxpp.c compiles to two things depending on RELIC_PP_NO_MAIN:
 *   - undefined (default): the file also defines main(), producing the
 *     tigryxpp CLI tool.
 *   - defined: main() is omitted, producing just this entry point plus its
 *     static helpers, suitable for linking into another executable (see
 *     the `tigryxpp_lib` CMake target).
 */
#ifndef TIGRYX_PP_H
#define TIGRYX_PP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Preprocess `input_path` (a .tgx source file) - resolving #import,
 * #define/#if conditionals and Adaptive Build tier pragmas - and write
 * the result to `output_path` (or stdout if NULL). `class` declarations,
 * method calls and `[e0,e1,...]` array literals are native language
 * syntax handled by the compiler; they pass through untouched.
 *
 * Returns 0 on success, non-zero on failure - matching the tigryxpp
 * process exit code. Safe to call repeatedly within the same process: all
 * preprocessor-internal state is reset at the start of each call. */
int tigryx_pp_run(const char *input_path, const char *output_path);

/* Add a `#define` that is applied at the start of every subsequent
 * tigryx_pp_run() call (after the tier predefines, so a -D wins).
 * Equivalent to the `-D NAME[=VALUE]` command-line flag; `value` may be
 * NULL (defines NAME as "1"). Returns 0 on success, -1 when the
 * user-define table is full. Persists across runs — this is process
 * configuration, not per-file state. */
int tigryx_pp_define(const char *name, const char *value);

/* The Adaptive Build tier is configured through relic_tier_*()
 * (include/tigryx_tier.h), which this library links: call
 * relic_tier_set_override_name() before tigryx_pp_run() to pin a tier
 * (same as --tier on the CLI), or relic_tier_set_override(-1) to clear
 * it back to auto-detect. The resolved tier is always emitted into the
 * .tp as `#pragma arg tier <name>`. */

#ifdef __cplusplus
}
#endif

#endif /* TIGRYX_PP_H */
