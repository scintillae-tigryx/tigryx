/*
 * musdec_impl.h
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
 * musdec_impl.h - implementation of musdec.h. Include from exactly one
 * translation unit per backend (the backend's musdec.c shim).
 */
#ifndef RELIC_MUSDEC_IMPL_H
#define RELIC_MUSDEC_IMPL_H

#include <stdlib.h>
#include <string.h>
#include "musdec.h"

/* Memory-only decoders: no stdio, no pushdata. */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../../../thirdparty/audio/stb_vorbis.c"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "../../../thirdparty/audio/dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "../../../thirdparty/audio/dr_flac.h"

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "../../../thirdparty/audio/dr_wav.h"

#define MUSDEC_CHUNK   1024   /* source frames decoded per refill */
#define MUSDEC_MAXCH   8

struct MusDec {
    int            kind;
    unsigned char *data;       /* owned copy of the compressed stream */
    int            len;
    stb_vorbis    *vb;
    drmp3          mp3;
    drflac        *flac;
    drwav          wav;
    int            channels;
    int            rate;
    double         step;       /* source frames per output sample */
    double         pos;        /* fractional position between prev and cur */
    float          prev, cur;  /* linear-interpolation endpoints */
    float          mono[MUSDEC_CHUNK];
    int            mono_n, mono_i;
    float          inter[MUSDEC_CHUNK * MUSDEC_MAXCH];
    int            ended;
};

int musdec_sniff(const unsigned char *p, int len)
{
    if (!p || len < 4) return MUSDEC_NONE;
    if (p[0] == 'O' && p[1] == 'g' && p[2] == 'g' && p[3] == 'S') return MUSDEC_VORBIS;
    if (p[0] == 'f' && p[1] == 'L' && p[2] == 'a' && p[3] == 'C') return MUSDEC_FLAC;
    if (len >= 12 && p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F' &&
        p[8] == 'W' && p[9] == 'A' && p[10] == 'V' && p[11] == 'E') return MUSDEC_WAV;
    if (len >= 12 && p[0] == 'F' && p[1] == 'O' && p[2] == 'R' && p[3] == 'M' &&
        p[8] == 'A' && p[9] == 'I' && p[10] == 'F') return MUSDEC_WAV;
    if (p[0] == 'I' && p[1] == 'D' && p[2] == '3') return MUSDEC_MP3;
    /* Bare MPEG audio frame sync (11 set bits; layer bits non-zero). */
    if (p[0] == 0xFF && (p[1] & 0xE0) == 0xE0 && (p[1] & 0x06) != 0) return MUSDEC_MP3;
    return MUSDEC_NONE;
}

/* Decode up to MUSDEC_CHUNK interleaved frames; returns frames read. */
static int musdec_read(MusDec *d)
{
    switch (d->kind) {
    case MUSDEC_VORBIS:
        return stb_vorbis_get_samples_float_interleaved(d->vb, d->channels, d->inter,
                                                        MUSDEC_CHUNK * d->channels);
    case MUSDEC_MP3:
        return (int)drmp3_read_pcm_frames_f32(&d->mp3, MUSDEC_CHUNK, d->inter);
    case MUSDEC_FLAC:
        return (int)drflac_read_pcm_frames_f32(d->flac, MUSDEC_CHUNK, d->inter);
    case MUSDEC_WAV:
        return (int)drwav_read_pcm_frames_f32(&d->wav, MUSDEC_CHUNK, d->inter);
    }
    return 0;
}

static void musdec_rewind(MusDec *d)
{
    switch (d->kind) {
    case MUSDEC_VORBIS: stb_vorbis_seek_start(d->vb); break;
    case MUSDEC_MP3:    drmp3_seek_to_pcm_frame(&d->mp3, 0); break;
    case MUSDEC_FLAC:   drflac_seek_to_pcm_frame(d->flac, 0); break;
    case MUSDEC_WAV:    drwav_seek_to_pcm_frame(&d->wav, 0); break;
    }
}

/* Refill the mono frame buffer, looping once at end of stream. */
static int musdec_refill(MusDec *d)
{
    int n = musdec_read(d);
    if (n <= 0) {
        musdec_rewind(d);
        n = musdec_read(d);
        if (n <= 0) { d->ended = 1; return 0; }
    }
    int ch = d->channels;
    float inv = 1.0f / (float)ch;
    for (int i = 0; i < n; i++) {
        float acc = 0.0f;
        for (int c = 0; c < ch; c++) acc += d->inter[i * ch + c];
        d->mono[i] = acc * inv;
    }
    d->mono_n = n;
    d->mono_i = 0;
    return n;
}

static float musdec_next_frame(MusDec *d)
{
    if (d->mono_i >= d->mono_n && musdec_refill(d) <= 0) return 0.0f;
    return d->mono[d->mono_i++];
}

MusDec *musdec_open(const unsigned char *p, int len, int out_rate)
{
    int kind = musdec_sniff(p, len);
    if (kind == MUSDEC_NONE || out_rate <= 0) return NULL;
    MusDec *d = (MusDec *)calloc(1, sizeof(MusDec));
    if (!d) return NULL;
    d->data = (unsigned char *)malloc((size_t)len);
    if (!d->data) { free(d); return NULL; }
    memcpy(d->data, p, (size_t)len);
    d->len  = len;
    d->kind = kind;

    int ok = 0;
    if (kind == MUSDEC_VORBIS) {
        int err = 0;
        d->vb = stb_vorbis_open_memory(d->data, len, &err, NULL);
        if (d->vb) {
            stb_vorbis_info vi = stb_vorbis_get_info(d->vb);
            d->channels = vi.channels;
            d->rate = (int)vi.sample_rate;
            ok = 1;
        }
    } else if (kind == MUSDEC_MP3) {
        if (drmp3_init_memory(&d->mp3, d->data, (size_t)len, NULL)) {
            d->channels = (int)d->mp3.channels;
            d->rate = (int)d->mp3.sampleRate;
            ok = 1;
        }
    } else if (kind == MUSDEC_FLAC) {
        d->flac = drflac_open_memory(d->data, (size_t)len, NULL);
        if (d->flac) {
            d->channels = (int)d->flac->channels;
            d->rate = (int)d->flac->sampleRate;
            ok = 1;
        }
    } else if (kind == MUSDEC_WAV) {
        if (drwav_init_memory(&d->wav, d->data, (size_t)len, NULL)) {
            d->channels = (int)d->wav.channels;
            d->rate = (int)d->wav.sampleRate;
            ok = 1;
        }
    }
    if (!ok) { free(d->data); free(d); return NULL; }
    if (d->channels < 1 || d->channels > MUSDEC_MAXCH || d->rate <= 0) {
        musdec_close(d);   /* decoder opened but the stream is unusable */
        return NULL;
    }
    d->step = (double)d->rate / (double)out_rate;
    d->prev = musdec_next_frame(d);
    d->cur  = musdec_next_frame(d);
    d->pos  = 0.0;
    return d;
}

void musdec_close(MusDec *d)
{
    if (!d) return;
    switch (d->kind) {
    case MUSDEC_VORBIS: if (d->vb) stb_vorbis_close(d->vb); break;
    case MUSDEC_MP3:    drmp3_uninit(&d->mp3); break;
    case MUSDEC_FLAC:   if (d->flac) drflac_close(d->flac); break;
    case MUSDEC_WAV:    drwav_uninit(&d->wav); break;
    }
    free(d->data);
    free(d);
}

int musdec_render(MusDec *d, float *out, int n)
{
    if (!d || d->ended) {
        for (int i = 0; i < n; i++) out[i] = 0.0f;
        return 0;
    }
    for (int i = 0; i < n; i++) {
        while (d->pos >= 1.0) {
            d->prev = d->cur;
            d->cur = musdec_next_frame(d);
            d->pos -= 1.0;
        }
        out[i] = d->prev + (d->cur - d->prev) * (float)d->pos;
        d->pos += d->step;
    }
    return d->ended ? 0 : n;
}

#endif
