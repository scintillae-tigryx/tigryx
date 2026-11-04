/*
 * jit_null.c
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
 * jit_null.c - Stub JIT used by platforms that do not (yet) have a real
 * code generator. compile() always returns false, so the --jit flag falls
 * back cleanly to the interpreter. Linked in by default (see RELIC_JIT in
 * the top-level CMakeLists.txt).
 */
#include "tigryx.h"
#include <string.h>

static bool null_compile(IRBuffer *ir, RelicJitCode **out_code,
                         char *err, int err_sz) {
    (void)ir; (void)out_code;
    if(err && err_sz > 0) {
        const char *msg =
            "no JIT available for this platform (built with RELIC_JIT=null)";
        int n = (int)strlen(msg);
        if(n >= err_sz) n = err_sz - 1;
        memcpy(err, msg, n);
        err[n] = 0;
    }
    return false;
}

static int null_run(RelicJitCode *code, RelicVM *vm) {
    (void)code; (void)vm;
    /* Should never be reached: compile() returns false, so main never calls run. */
    return 1;
}

static void null_free(RelicJitCode *code) {
    (void)code;
}

RelicJit relic_jit = {
    .name    = "null",
    .compile = null_compile,
    .run     = null_run,
    .free    = null_free,
};
