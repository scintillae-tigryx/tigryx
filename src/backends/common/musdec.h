/*
 * musdec.h
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
 * musdec.h - streamed music decoding for the backends' @music.
 *
 * Recognises Ogg Vorbis, MP3, FLAC and WAV/AIFF lumps by their magic bytes
 * and decodes them incrementally to mono float samples at the mixer's output
 * rate (linear resampling, channel downmix), looping at end of stream. The
 * compressed data is copied on open, so the caller's buffer (e.g. a reused
 * WAD staging area) may be released immediately.
 *
 * Threading: musdec_open/musdec_close may run on any thread; musdec_render
 * is called from the audio thread. The caller serialises access (open the
 * new stream, swap the pointer under the mixer lock, close the old one
 * outside it).
 *
 * The implementation lives in musdec_impl.h and is compiled once per
 * backend through a one-line musdec.c shim in the backend directory, so the
 * decoders' macros stay out of the backend's own translation unit.
 */
#ifndef RELIC_MUSDEC_H
#define RELIC_MUSDEC_H

#define MUSDEC_NONE   0
#define MUSDEC_VORBIS 1
#define MUSDEC_MP3    2
#define MUSDEC_FLAC   3
#define MUSDEC_WAV    4

typedef struct MusDec MusDec;

/* Format of a buffer by magic bytes (MUSDEC_*); MUSDEC_NONE if unknown. */
int musdec_sniff(const unsigned char *p, int len);

/* Open a looping stream rendering at out_rate Hz. NULL on failure. */
MusDec *musdec_open(const unsigned char *p, int len, int out_rate);

/* Release a stream (NULL is ignored). */
void musdec_close(MusDec *d);

/* Render n mono samples in [-1,1] into out. Returns n, or 0 once the
 * stream can no longer produce audio (out is then zero-filled). */
int musdec_render(MusDec *d, float *out, int n);

#endif
