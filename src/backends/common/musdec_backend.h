/*
 * musdec_backend.h
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
 * musdec_backend.h - per-backend glue for the streamed @music decoders.
 *
 * The stream itself lives in musdec.h / musdec_impl.h (compiled once per
 * backend through that backend's musdec.c shim). This header carries the
 * small amount of backend-specific wiring: the g_musdec pointer and the
 * start / stop / mix helpers, written once here so the win32, linux, macOS,
 * emscripten and metal backends all behave identically.
 *
 * Include it ONCE in the backend's main source file, after the sound
 * state struct and the MUSDEC_* macros below are defined:
 *
 *   MUSDEC_LOCK()     take the mixer lock (empty if the mixer is
 *                     single-threaded, e.g. emscripten)
 *   MUSDEC_UNLOCK()   release the mixer lock
 *   MUSDEC_VOLUME()   expression for the @mixer music volume, 0..1
 *                     (typically an accessor that reads g_mus.master;
 *                     evaluate it under the lock the caller already holds)
 *   MUSDEC_OUT_RATE   mixer output rate in Hz (SND_SAMPLE_RATE)
 *
 * musdec_mix is called from the mixer fill while the mixer lock is held
 * (the same as mus_render); musdec_start / musdec_stop take it themselves
 * and close the superseded stream outside the lock so a decoder's
 * destructor never runs inside the audio callback's critical section.
 */
#ifndef RELIC_MUSDEC_BACKEND_H
#define RELIC_MUSDEC_BACKEND_H

#include "musdec.h"

static MusDec *g_musdec = NULL;
#define MUSDEC_AMP 96.0f   /* full-scale stream -> 8-bit mix level */

/** @brief Mix the streamed music track into dst. Caller holds the mixer lock. */
static void musdec_mix(u8 *dst, int n)
{
    if (!g_musdec) return;
    float tmp[1024];
    while (n > 0) {
        int k = n > 1024 ? 1024 : n;
        float gain;
        musdec_render(g_musdec, tmp, k);
        gain = MUSDEC_AMP * (MUSDEC_VOLUME());
        for (int i = 0; i < k; i++) {
            int mixed = (int)dst[i] - 128 + (int)(tmp[i] * gain);
            if (mixed > 127)  mixed = 127;
            if (mixed < -128) mixed = -128;
            dst[i] = (u8)(mixed + 128);
        }
        dst += k;
        n -= k;
    }
}

/** @brief Stop and release the streamed music track. */
static void musdec_stop(void)
{
    MusDec *old;
    MUSDEC_LOCK();
    old = g_musdec;
    g_musdec = NULL;
    MUSDEC_UNLOCK();
    musdec_close(old);
}

/** @brief Start a streamed music track; returns 1 if the format was handled. */
static int musdec_start(const u8 *buf, int len)
{
    MusDec *d, *old;
    if (musdec_sniff(buf, len) == MUSDEC_NONE) return 0;
    d = musdec_open(buf, len, MUSDEC_OUT_RATE);
    if (!d) return 0;
    MUSDEC_LOCK();
    old = g_musdec;
    g_musdec = d;
    MUSDEC_UNLOCK();
    musdec_close(old);
    return 1;
}

#endif /* RELIC_MUSDEC_BACKEND_H */
