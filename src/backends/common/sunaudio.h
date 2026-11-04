/*
 * sunaudio.h
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
 * sunaudio.h - Solaris /dev/audio sound engine, shared by the Solaris
 *              backends (opengl-linux under __sun, relic3d-x11).
 *
 * This is a HEADER, not a .c file, and is meant to be #included into
 * exactly one place in exactly one backend .c file - the same pattern as
 * wr_render.h / lines3d.h, so everything stays static and no symbols leak.
 *
 * Contents (all u8 unsigned-PCM at SA_SND_RATE Hz mono, matching the
 * PulseAudio/WinMM engines):
 *
 *   - PCM voice mixer   (@play, @tone)      - 16 voices, sweep synth
 *   - OPL2 FM synth     (@music)            - MUS music sequencer at 140Hz
 *   - MIDI->FM player   (@music on MThd)    - pthread sequencer feeding
 *                                             the same OPL2 voices
 *   - GENMIDI loader    (@music cmd 3)      - "#OPL_II#" lump parser
 *   - Mixer groups      (@mixer)            - master/music/sfx + mute
 *
 * The only Solaris-specific part is the sink: a pthread writes mixed
 * chunks to /dev/audio (or /dev/sound/0). Blocking write() paces the
 * mixer - no timers needed. AUDIO_SETINFO is asked for 8-bit linear
 * mono at 22050Hz (audiocs/SUNW,CS4231 and newer); older dbri-class
 * hardware that rejects it falls back to 8kHz mu-law, and sa_write()
 * then resamples+encodes (naive decimation + a tableless G.711 encode).
 *
 * API (call from the backend's snd_* ops):
 *   sa_snd_init()        open device, start mixer thread. 0 = ok
 *   sa_snd_shutdown()    stop music + voices, drain, close
 *   sa_snd_available()   1 when a device opened successfully
 *   sa_snd_tone_freq(freq, ms, vol)     @tone equivalent
 *   sa_snd_tone_id(id, vol)             canned effect table
 *   sa_snd_play(buf, len, vol)          PCM voice (buf copied)
 *   sa_snd_music(buf, len, cmd)         0 stop / 1 FM synth / 2 same / 3 GENMIDI
 *   sa_snd_mixer(cmd, value)            0 master, 1 music, 2 sfx, 3 mute-toggle
 *
 * Everything is internal-static; the sa_* entry points are the only
 * intended call sites. Requires -lpthread -lm (and <sys/audioio.h>,
 * present on every Solaris with audio support).
 */
#ifndef RELIC_SUNAUDIO_H
#define RELIC_SUNAUDIO_H

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/audioio.h>
#include <stropts.h>   /* I_FLUSH / FLUSHRW for the /dev/audio STREAMS device */

#define SA_SND_RATE        22050
#define SA_SND_AMPLITUDE   110
#define SA_SND_SILENCE     128
#define SA_SND_CHUNK       512
#define SA_SND_VOICES      16

/* Streamed music decoders (Ogg Vorbis / MP3 / FLAC / WAV), shared API in
 * common/musdec.h. The sa_-prefixed names keep this header clear of a
 * backend TU that already includes common/musdec_backend.h for its own
 * mixer (opengl-linux includes both; sunaudio serves the /dev/audio path
 * there, on Solaris it IS the relic3d-x11 engine). */
#include "musdec.h"
static MusDec *sa_musdec = NULL;
#define SA_MUSDEC_AMP 96.0f

typedef struct { int f0, f1, ms; } SaSndSeg;

typedef struct {
    u8 *data;
    int  len;
    int  pos;
    int  volume;
    int  active;
} SaSndVoice;

typedef struct {
    pthread_mutex_t lock;
    pthread_t       thread;
    int             running;
    int             started;
    int             fd;            /* /dev/audio output, -1 = none      */
    int             ulaw;          /* device is 8kHz mu-law only        */
    double          rs_acc;        /* resample phase (mixer thread only)*/
    SaSndVoice      voices[SA_SND_VOICES];
} SaSndState;

static SaSndState sa_snd = {0};
static int        sa_avail = 0;

static float sa_master_vol = 1.0f;
static float sa_sfx_vol    = 1.0f;
static float sa_muted_master = -1.0f;

/* ------------------------------------------------------------------
 * /dev/audio sink
 * ---------------------------------------------------------------- */

/* G.711 mu-law encode of a signed 16-bit sample (tableless reference
 * implementation). */
static u8 sa_lin2ulaw(int s) {
    const int BIAS = 0x84;
    int sign = 0;
    if (s < 0) { sign = 0x80; s = -s; }
    if (s > 32635) s = 32635;
    s += BIAS;
    int exp = 7;
    for (int m = 0x4000; m > 0 && !(s & m); m >>= 1) exp--;
    int mant = (s >> (exp + 3)) & 0x0F;
    return (u8)(~(sign | (exp << 4) | mant) & 0xFF);
}

static int sa_audio_open(void) {
    int fd = open("/dev/audio", O_WRONLY | O_NONBLOCK);
    if (fd < 0) fd = open("/dev/sound/0", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    /* Open nonblocking so a wedged device can't hang us, then clear the
     * flag: blocking write() below is what paces the mixer thread. */
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);

    audio_info_t ai;
    AUDIO_INITINFO(&ai);
    ai.play.sample_rate = SA_SND_RATE;
    ai.play.channels    = 1;
    ai.play.precision   = 8;
    ai.play.encoding    = AUDIO_ENCODING_LINEAR;
    if (ioctl(fd, AUDIO_SETINFO, &ai) == 0) { sa_snd.ulaw = 0; return fd; }

    /* Oldest Sun audio (dbri on SS20-class, early amd on Ultra 1/2) is
     * 8kHz mu-law only - take it and convert in sa_audio_write. */
    AUDIO_INITINFO(&ai);
    ai.play.sample_rate = 8000;
    ai.play.channels    = 1;
    ai.play.precision   = 8;
    ai.play.encoding    = AUDIO_ENCODING_ULAW;
    if (ioctl(fd, AUDIO_SETINFO, &ai) == 0) { sa_snd.ulaw = 1; return fd; }

    close(fd);
    return -1;
}

static int sa_audio_write(const u8 *buf, int n) {
    if (sa_snd.fd < 0) return -1;
    if (!sa_snd.ulaw) {
        int off = 0;
        while (off < n) {
            ssize_t w = write(sa_snd.fd, buf + off, (size_t)(n - off));
            if (w < 0) { if (errno == EINTR) continue; return -1; }
            off += (int)w;
        }
        return 0;
    }
    /* mu-law fallback: 22050 -> 8000 decimation + G.711 encode. */
    u8 obuf[2048];
    int   o = 0;
    const double step = (double)SA_SND_RATE / 8000.0;
    double pos = sa_snd.rs_acc;
    while (pos + 0.5 < (double)n && o < (int)sizeof(obuf)) {
        int s = ((int)buf[(int)pos] - 128) << 8;
        obuf[o++] = sa_lin2ulaw(s);
        pos += step;
    }
    sa_snd.rs_acc = pos - (double)n;
    if (sa_snd.rs_acc < 0.0) sa_snd.rs_acc = 0.0;
    int off = 0;
    while (off < o) {
        ssize_t w = write(sa_snd.fd, obuf + off, (size_t)(o - off));
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        off += (int)w;
    }
    return 0;
}

static void sa_audio_close(int fd) {
    if (fd < 0) return;
    ioctl(fd, AUDIO_DRAIN, 0);   /* graceful drain, may block briefly */
    ioctl(fd, I_FLUSH, FLUSHRW); /* then flush so a blocked write wakes */
    close(fd);
}

/* ------------------------------------------------------------------
 * Tone sweep synth (square wave with 5ms edges + LP filter)
 * ---------------------------------------------------------------- */
static int sa_snd_fill_sweep(u8 *buf, int buf_sz,
                             int freq_start, int freq_end, int ms, int volume)
{
    int samples = (SA_SND_RATE * ms) / 1000;
    if (samples > buf_sz) samples = buf_sz;
    if (volume < 0)   volume = 0;
    if (volume > 255) volume = 255;
    float volume_scale = (float)volume / 255.0f;

    static float filter_state = 0.0f;
    const float alpha = 0.15f;

    for (int i = 0; i < samples; i++) {
        float t    = (samples > 1) ? (float)i / (float)(samples - 1) : 0.0f;
        float freq = (float)freq_start + t * (float)(freq_end - freq_start);
        if (freq < 20.0f) freq = 20.0f;
        float half = (float)SA_SND_RATE / (2.0f * freq);
        int phase = (int)((float)i / half) & 1;
        float env = 1.0f;
        float ms_f = (float)ms;
        float t_ms = (float)i * 1000.0f / (float)SA_SND_RATE;
        if      (t_ms < 5.0f)        env = t_ms / 5.0f;
        else if (t_ms > ms_f - 5.0f) env = (ms_f - t_ms) / 5.0f;
        if (env < 0.0f) env = 0.0f;
        int val = SA_SND_SILENCE + (phase ? 1 : -1) *
                  (int)(SA_SND_AMPLITUDE * env * volume_scale);

        float input  = (float)val;
        float output = alpha * input + (1.0f - alpha) * filter_state;
        filter_state = output;
        buf[i] = (u8)((int)(output + 0.5f));
    }
    return samples;
}

/* Defined later; renders MUS/MIDI music into dst (caller holds lock). */
static void sa_mus_render(u8 *dst, int n);

/* Mix the streamed music track into dst. Caller holds sa_snd.lock. */
static void sa_musdec_mix(u8 *dst, int n)
{
    if (!sa_musdec) return;
    while (n > 0) {
        float tmp[1024];
        int k = n > 1024 ? 1024 : n;
        float gain = SA_MUSDEC_AMP * sa_mus.master;
        int i;
        musdec_render(sa_musdec, tmp, k);
        for (i = 0; i < k; i++) {
            int mixed = (int)dst[i] - 128 + (int)(tmp[i] * gain);
            if (mixed > 127)  mixed = 127;
            if (mixed < -128) mixed = -128;
            dst[i] = (u8)(mixed + 128);
        }
        dst += k;
        n -= k;
    }
}

static void sa_musdec_stop(void)
{
    MusDec *old;
    pthread_mutex_lock(&sa_snd.lock);
    old = sa_musdec;
    sa_musdec = NULL;
    pthread_mutex_unlock(&sa_snd.lock);
    musdec_close(old);
}

static int sa_musdec_start(const u8 *buf, int len)
{
    MusDec *d, *old;
    if (musdec_sniff(buf, len) == MUSDEC_NONE) return 0;
    d = musdec_open(buf, len, SA_SND_RATE);
    if (!d) return 0;
    pthread_mutex_lock(&sa_snd.lock);
    old = sa_musdec;
    sa_musdec = d;
    pthread_mutex_unlock(&sa_snd.lock);
    musdec_close(old);
    return 1;
}

/* Mix all active voices into dst, return mixed sample count. */
static int sa_snd_mixer_fill(u8 *dst, int dst_samples)
{
    memset(dst, SA_SND_SILENCE, dst_samples);

    pthread_mutex_lock(&sa_snd.lock);
    sa_mus_render(dst, dst_samples);
    sa_musdec_mix(dst, dst_samples);
    for (int v = 0; v < SA_SND_VOICES; v++) {
        if (!sa_snd.voices[v].active) continue;
        SaSndVoice *voice = &sa_snd.voices[v];
        int vol = voice->volume;
        for (int i = 0; i < dst_samples && voice->pos < voice->len; i++) {
            int sample = (int)voice->data[voice->pos] - 128;
            int mixed  = (int)dst[i] - 128 + (sample * vol) / 255;
            if (mixed > 127)  mixed = 127;
            if (mixed < -128) mixed = -128;
            dst[i] = (u8)(mixed + 128);
            voice->pos++;
        }
        if (voice->pos >= voice->len) {
            voice->active = 0;
            free(voice->data);
            voice->data = NULL;
        }
    }
    pthread_mutex_unlock(&sa_snd.lock);

    if (sa_master_vol < 1.0f) {
        for (int i = 0; i < dst_samples; i++) {
            int s = (int)dst[i] - 128;
            s = (int)(s * sa_master_vol);
            dst[i] = (u8)(s + 128);
        }
    }
    return dst_samples;
}

/* Copy buf into a free voice slot and start playback. */
static int sa_snd_start_voice(const u8 *buf, int len, int volume)
{
    if (sa_snd.fd < 0 || !sa_avail) return 0;
    if (!sa_snd.started || !buf || len <= 0) return 0;
    u8 *copy = (u8*)malloc((size_t)len);
    if (!copy) return 0;
    memcpy(copy, buf, (size_t)len);

    pthread_mutex_lock(&sa_snd.lock);
    int started = 0;
    for (int i = 0; i < SA_SND_VOICES; i++) {
        if (!sa_snd.voices[i].active) {
            sa_snd.voices[i].data   = copy;
            sa_snd.voices[i].len    = len;
            sa_snd.voices[i].pos    = 0;
            sa_snd.voices[i].volume = volume;
            sa_snd.voices[i].active = 1;
            started = 1;
            break;
        }
    }
    if (!started) free(copy);
    pthread_mutex_unlock(&sa_snd.lock);
    return started;
}

static void *sa_snd_thread_proc(void *param) {
    (void)param;
    u8 chunk[SA_SND_CHUNK];
    while (sa_snd.running) {
        sa_snd_mixer_fill(chunk, SA_SND_CHUNK);
        if (sa_snd.fd >= 0) sa_audio_write(chunk, SA_SND_CHUNK);
        else break;
    }
    return NULL;
}

static int sa_snd_init(void) {
    memset(&sa_snd, 0, sizeof(sa_snd));
    sa_snd.fd = sa_audio_open();
    if (sa_snd.fd < 0) { sa_avail = 0; return -1; }
    sa_avail = 1;

    pthread_mutex_init(&sa_snd.lock, NULL);
    sa_snd.running = 1;
    if (pthread_create(&sa_snd.thread, NULL, sa_snd_thread_proc, NULL) == 0) {
        sa_snd.started = 1;
    } else {
        sa_snd.running = 0;
        sa_snd.started = 0;
        sa_audio_close(sa_snd.fd);
        sa_snd.fd = -1;
        sa_avail = 0;
        return -1;
    }
    return 0;
}

static void sa_midi_fm_stop(void);

static void sa_snd_shutdown(void) {
    sa_midi_fm_stop();
    sa_snd.running = 0;
    if (sa_snd.fd >= 0) {
        /* Flush the stream so a write() blocked on buffer space returns
         * and the mixer thread can exit; then join and close. */
        ioctl(sa_snd.fd, I_FLUSH, FLUSHRW);
        if (sa_snd.started) pthread_join(sa_snd.thread, NULL);
        sa_audio_close(sa_snd.fd);
        sa_snd.fd = -1;
    }
    if (sa_snd.started) {
        pthread_mutex_lock(&sa_snd.lock);
        for (int i = 0; i < SA_SND_VOICES; i++) {
            if (sa_snd.voices[i].data) {
                free(sa_snd.voices[i].data);
                sa_snd.voices[i].data = NULL;
            }
            sa_snd.voices[i].active = 0;
        }
        pthread_mutex_unlock(&sa_snd.lock);
        pthread_mutex_destroy(&sa_snd.lock);
        sa_snd.started = 0;
    }
    sa_avail = 0;
}

static int sa_snd_available(void) { return sa_avail; }

static void sa_snd_play_segs_async(const SaSndSeg *segs, int n, int volume) {
    int total = 0;
    for (int i = 0; i < n; i++)
        total += (SA_SND_RATE * segs[i].ms) / 1000 + 1;
    u8 *buf = (u8*)malloc((size_t)total);
    if (!buf) return;
    int pos = 0;
    for (int i = 0; i < n; i++)
        pos += sa_snd_fill_sweep(buf + pos, total - pos,
                                 segs[i].f0, segs[i].f1, segs[i].ms, volume);
    sa_snd_start_voice(buf, pos, 255);
    free(buf);
}

/* ------------------------------------------------------------------
 * Backend-facing ops (arguments already decoded from VM registers)
 * ---------------------------------------------------------------- */
static void sa_snd_tone_freq(int freq, int ms, int volume) {
    if (!sa_avail) return;
    if (freq < 20) freq = 20;
    if (ms   < 1)  ms   = 1;
    if (volume < 0)   volume = 0;
    if (volume > 255) volume = 255;
    int samples = (SA_SND_RATE * ms) / 1000 + 1;
    u8 *buf = (u8*)malloc((size_t)samples);
    if (!buf) return;
    int len = sa_snd_fill_sweep(buf, samples, freq, freq, ms, volume);
    int scaled = (int)(255 * sa_sfx_vol);
    if (scaled > 255) scaled = 255;
    sa_snd_start_voice(buf, len, scaled);
    free(buf);
}

static void sa_snd_tone_id(int id, int volume) {
    if (!sa_avail) return;
    if (volume < 0)   volume = 0;
    if (volume > 255) volume = 255;

    switch (id) {
        case 0: { SaSndSeg s2[] = { {300,120,60},{120,60,80},{60,40,100},{40,30,120} };
                  sa_snd_play_segs_async(s2, 4, (int)(volume * sa_sfx_vol)); break; }
        case 1: { SaSndSeg s2[] = { {1600,200,120} };
                  sa_snd_play_segs_async(s2, 1, (int)(volume * sa_sfx_vol)); break; }
        case 2: { SaSndSeg s2[] = { {2000,1600,25},{1600,1200,20} };
                  sa_snd_play_segs_async(s2, 2, (int)(volume * sa_sfx_vol)); break; }
        case 3: { SaSndSeg s2[] = { {1000,600,40},{600,400,30} };
                  sa_snd_play_segs_async(s2, 2, (int)(volume * sa_sfx_vol)); break; }
        case 4: { SaSndSeg s2[] = { {500,200,50},{200,100,70} };
                  sa_snd_play_segs_async(s2, 2, (int)(volume * sa_sfx_vol)); break; }
        case 5: { SaSndSeg s2[] = { {330,330,60},{440,440,60},{550,550,60},{660,660,60},{880,880,90} };
                  sa_snd_play_segs_async(s2, 5, (int)(volume * sa_sfx_vol)); break; }
        case 6: { SaSndSeg s2[] = { {250,900,110} };
                  sa_snd_play_segs_async(s2, 1, (int)(volume * sa_sfx_vol)); break; }
        case 7: { SaSndSeg s2[] = { {880,660,80},{660,494,80},{494,330,90},{330,220,100},{220,110,130} };
                  sa_snd_play_segs_async(s2, 5, (int)(volume * sa_sfx_vol)); break; }
        case 8: { SaSndSeg s2[] = { {988,988,55},{1319,1319,110} };
                  sa_snd_play_segs_async(s2, 2, (int)(volume * sa_sfx_vol)); break; }
        case 9: { SaSndSeg s2[] = { {523,523,80},{659,659,80},{784,784,80},{1047,1047,180} };
                  sa_snd_play_segs_async(s2, 4, (int)(volume * sa_sfx_vol)); break; }
        case 10: { SaSndSeg s2[] = { {300,150,40},{150,80,60},{80,50,60} };
                   sa_snd_play_segs_async(s2, 3, (int)(volume * sa_sfx_vol)); break; }
        case 11: { SaSndSeg s2[] = { {400,1600,100},{1600,400,100} };
                   sa_snd_play_segs_async(s2, 2, (int)(volume * sa_sfx_vol)); break; }
        default: break;
    }
}

static void sa_snd_play(const u8 *buf, int len, int volume) {
    if (!sa_avail || !buf || len <= 0) return;
    if (volume < 0)   volume = 0;
    if (volume > 255) volume = 255;

    int eff_vol = (int)(volume * sa_sfx_vol);
    if (eff_vol > 255) eff_vol = 255;
    if (eff_vol < 255) {
        u8 *scaled = (u8*)malloc((size_t)len);
        if (!scaled) { sa_snd_start_voice(buf, len, eff_vol); return; }
        float volume_scale = (float)eff_vol / 255.0f;
        for (int i = 0; i < len; i++) {
            int sample = (int)buf[i];
            int offset = sample - 128;
            offset = (int)(offset * volume_scale);
            scaled[i] = (u8)(128 + offset);
        }
        sa_snd_start_voice(scaled, len, 255);
        free(scaled);
    } else {
        sa_snd_start_voice(buf, len, 255);
    }
}

/* ------------------------------------------------------------------
 * OPL2 FM synthesiser + MUS sequencer + MIDI->FM player.
 * Ported from the win32 gold backend / opengl-linux; identical math,
 * pthread mutexes instead of Win32 critical sections.
 * ---------------------------------------------------------------- */
#define SA_MUS_CHANNELS  16
#define SA_MUS_VOICES    24
#define SA_MUS_TICK_RATE 140
#define SA_MUS_PERC_CH   15

static float sa_opl_sin_table[1024];
static int   sa_opl_tables_ready = 0;

static void sa_opl_build_tables(void) {
    if (sa_opl_tables_ready) return;
    for (int i = 0; i < 1024; i++)
        sa_opl_sin_table[i] = sinf((float)i * 6.283185307f / 1024.0f);
    sa_opl_tables_ready = 1;
}

static float sa_opl_wave(int waveform, u32 phase) {
    int idx = (int)((phase >> 22) & 0x3FF);
    switch (waveform & 7) {
        case 0:
            return sa_opl_sin_table[idx];
        case 1:
            return (idx < 512) ? sa_opl_sin_table[idx] : 0.0f;
        case 2:
            return (sa_opl_sin_table[idx] < 0) ? -sa_opl_sin_table[idx]
                                             :  sa_opl_sin_table[idx];
        case 3:
            if (idx < 256)  return sa_opl_sin_table[idx];
            if (idx < 512)  return 0.0f;
            if (idx < 768)  return sa_opl_sin_table[idx - 512];
            return 0.0f;
        case 4:
            return (idx < 512) ? sa_opl_sin_table[(idx * 2) & 0x3FF] : 0.0f;
        case 5: {
            if (idx >= 512) return 0.0f;
            float s = sa_opl_sin_table[(idx * 2) & 0x3FF];
            return (s < 0) ? -s : s;
        }
        case 6:
            return (idx < 512) ? 1.0f : -1.0f;
        case 7: {
            float t = (float)idx * (1.0f / 1024.0f);
            return 2.0f * t - 1.0f;
        }
    }
    return 0.0f;
}

typedef struct {
    float level;
    int   stage;
    float atk_inc;
    float dec_mul;
    float sus_level;
    float rel_mul;
    int   eg_flag;
} SaOplEnv;

static float sa_opl_decay_mul(int rate) {
    if (rate <= 0) return 1.0f;
    if (rate > 15) rate = 15;
    double f = -7.4493 * (1.0 / 39.28064) / (double)SA_SND_RATE;
    return (float)pow(2.0, f * pow(2.0, (double)rate));
}

static float sa_opl_attack_inc(int rate) {
    if (rate <= 0) return 0.0f;
    if (rate >= 15) return 1.0f;
    float inc = (float)pow(2.0, (double)rate) / 32768.0f;
    inc *= 22050.0f / (float)SA_SND_RATE;
    if (inc > 1.0f) inc = 1.0f;
    return inc;
}

static void sa_opl_env_init(SaOplEnv *e, int atk, int dec, int sus, int rel, int eg) {
    e->level     = 0.0f;
    e->stage     = 0;
    e->atk_inc   = sa_opl_attack_inc(atk);
    e->dec_mul   = sa_opl_decay_mul(dec);
    e->sus_level = (float)pow(2.0, -0.5 * (double)sus);
    e->rel_mul   = sa_opl_decay_mul(rel > 0 ? rel : 1);
    e->eg_flag   = eg;
}

#define SA_OPL_ENV_SILENCE 0.0005f

static float sa_opl_env_tick(SaOplEnv *e) {
    switch (e->stage) {
        case 0:
            e->level += e->atk_inc;
            if (e->level >= 1.0f) { e->level = 1.0f; e->stage = 1; }
            break;
        case 1:
            e->level *= e->dec_mul;
            if (e->level <= e->sus_level) {
                e->level = e->sus_level;
                e->stage = e->eg_flag ? 2 : 3;
            }
            break;
        case 2:
            break;
        case 3:
            e->level *= e->rel_mul;
            if (e->level <= SA_OPL_ENV_SILENCE) { e->level = 0.0f; e->stage = 4; }
            break;
        case 4:
            e->level = 0.0f;
            break;
    }
    return e->level;
}

typedef struct {
    u8 mult;
    int ksr;
    int eg;
    int vib;
    int am;
    int volume;
    int ksl;
    int attack;
    int decay;
    int sustain;
    int release;
    int wave;
} SaOplOpDef;

typedef struct {
    SaOplOpDef mod;
    SaOplOpDef car;
    int        feedback;
    int        additive;
    int        note_offset;
} SaOplVoiceDef;

typedef struct {
    int           valid;
    int           dual;
    int           fixed;
    int           fixed_note;
    SaOplVoiceDef v[2];
} SaOplInstrument;

static SaOplInstrument sa_genmidi_melodic[128];
static SaOplInstrument sa_genmidi_perc[47];
static int             sa_genmidi_loaded = 0;

static void sa_opl_parse_op(const u8 *d, SaOplOpDef *op) {
    op->am      = (d[0] >> 7) & 1;
    op->vib     = (d[0] >> 6) & 1;
    op->eg      = (d[0] >> 5) & 1;
    op->ksr     = (d[0] >> 4) & 1;
    op->mult    = d[0] & 0x0F;
    op->attack  = (d[1] >> 4) & 0x0F;
    op->decay   = d[1] & 0x0F;
    op->sustain = (d[2] >> 4) & 0x0F;
    op->release = d[2] & 0x0F;
    op->wave    = d[3] & 7;
    op->ksl     = (d[4] >> 6) & 3;
    op->volume  = d[5] & 0x3F;
}

static void sa_opl_parse_voice_def(const u8 *v, SaOplOpDef *mod, SaOplOpDef *car,
                                   int *feedback, int *additive, int *note_offset) {
    sa_opl_parse_op(v + 0, mod);
    *feedback = (v[6] >> 1) & 7;
    *additive = v[6] & 1;
    sa_opl_parse_op(v + 7, car);
    *note_offset = (i16)(v[14] | (v[15] << 8));
}

static void sa_genmidi_load(const u8 *lump, int lump_len) {
    if (!lump || lump_len < 8 + 175 * 36) return;
    if (memcmp(lump, "#OPL_II#", 8) != 0) return;

    const u8 *p = lump + 8;
    for (int i = 0; i < 175; i++, p += 36) {
        SaOplInstrument *inst = (i < 128) ? &sa_genmidi_melodic[i]
                                          : &sa_genmidi_perc[i - 128];
        u16 flags = p[0] | (p[1] << 8);
        inst->dual  = (flags & 0x04) ? 1 : 0;
        inst->fixed = (flags & 0x01) ? 1 : 0;
        inst->fixed_note = p[3];
        inst->valid = 1;

        sa_opl_parse_voice_def(p + 4, &inst->v[0].mod, &inst->v[0].car,
                               &inst->v[0].feedback, &inst->v[0].additive,
                               &inst->v[0].note_offset);
        sa_opl_parse_voice_def(p + 20, &inst->v[1].mod, &inst->v[1].car,
                               &inst->v[1].feedback, &inst->v[1].additive,
                               &inst->v[1].note_offset);
    }
    sa_genmidi_loaded = 1;
}

static const float sa_opl_mult_table[16] = {
    0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f,
    8.0f, 9.0f, 10.0f, 10.0f, 12.0f, 12.0f, 15.0f, 15.0f
};

typedef struct {
    u32      phase;
    u32      step;
    SaOplEnv env;
    int      wave;
    float    out_level;
} SaOplOp;

typedef struct {
    int     active;
    int     channel;
    int     note;
    float   vol;
    int     released;
    SaOplOp mod;
    SaOplOp car;
    int     feedback;
    int     additive;
    float   fb_out[2];
} SaFmVoice;

typedef struct {
    int program;
    int volume;
    int last_velocity;
} SaMusChannel;

typedef struct {
    int          playing;
    int          external;
    u8          *score;
    int          score_len;
    int          score_start;
    int          pos;
    int          delay_ticks;
    float        samples_to_tick;
    float        master;
    SaMusChannel ch[SA_MUS_CHANNELS];
    SaFmVoice    voices[SA_MUS_VOICES];
} SaMusPlayer;

static SaMusPlayer sa_mus = {0};

static int sa_mus_no_cymbals = 0;
static int sa_mus_single_voice = 0;

static int sa_mus_note_is_cymbal(int note) {
    switch (note) {
        case 42: case 44: case 46:
        case 49: case 51: case 52: case 53:
        case 55: case 57: case 59:
            return 1;
        default:
            return 0;
    }
}

static float sa_dmx_note_freq(int note) {
    return (float)(440.0 * pow(2.0, ((double)note - 57.0) / 12.0));
}

static void sa_fm_voice_setup_op(SaOplOp *op, const SaOplOpDef *def,
                                 float freq, float mult_factor) {
    float op_freq = freq * mult_factor;
    op->step = (u32)((double)op_freq * 4294967296.0 / (double)SA_SND_RATE);
    op->phase = 0;
    op->wave = def->wave;
    op->out_level = (float)pow(2.0, -0.125 * (double)def->volume);
    sa_opl_env_init(&op->env, def->attack, def->decay, def->sustain,
                    def->release, def->eg);
}

static SaFmVoice *sa_mus_alloc_voice(void) {
    SaFmVoice *slot = NULL;
    for (int i = 0; i < SA_MUS_VOICES; i++) {
        if (!sa_mus.voices[i].active) { slot = &sa_mus.voices[i]; break; }
    }
    if (!slot) {
        float lowest = 1e9f;
        for (int i = 0; i < SA_MUS_VOICES; i++) {
            if (sa_mus.voices[i].released) {
                float e = sa_mus.voices[i].car.env.level;
                if (e < lowest) { lowest = e; slot = &sa_mus.voices[i]; }
            }
        }
    }
    if (!slot) {
        float lowest = 1e9f;
        for (int i = 0; i < SA_MUS_VOICES; i++) {
            float e = sa_mus.voices[i].car.env.level;
            if (e < lowest) { lowest = e; slot = &sa_mus.voices[i]; }
        }
    }
    return slot;
}

static void sa_mus_setup_voice(SaFmVoice *slot, const SaOplInstrument *inst,
                               int vidx, int channel, int note,
                               float gain, int perc) {
    const SaOplVoiceDef *vd = &inst->v[vidx];
    int play_note;
    if (inst->fixed) {
        play_note = inst->fixed_note;
    } else if (perc) {
        play_note = 60;
    } else {
        play_note = note + vd->note_offset;
    }
    while (play_note < 0)   play_note += 12;
    while (play_note > 127) play_note -= 12;

    float freq = sa_dmx_note_freq(play_note);

    memset(slot, 0, sizeof(*slot));
    slot->active   = 1;
    slot->channel  = channel;
    slot->note     = note;
    slot->vol      = gain;
    slot->feedback = vd->feedback;
    slot->additive = vd->additive;
    sa_fm_voice_setup_op(&slot->mod, &vd->mod, freq,
                         sa_opl_mult_table[vd->mod.mult & 0xF]);
    sa_fm_voice_setup_op(&slot->car, &vd->car, freq,
                         sa_opl_mult_table[vd->car.mult & 0xF]);
}

static void sa_mus_note_on(int channel, int note, int velocity) {
    int perc = (channel == SA_MUS_PERC_CH) ? 1 : 0;

    if (perc && sa_mus_no_cymbals && sa_mus_note_is_cymbal(note)) return;

    SaOplInstrument *inst = NULL;
    if (perc) {
        int pi = note - 35;
        if (pi >= 0 && pi < 47 && sa_genmidi_perc[pi].valid)
            inst = &sa_genmidi_perc[pi];
    } else {
        int prog = sa_mus.ch[channel].program;
        if (prog >= 0 && prog < 128 && sa_genmidi_melodic[prog].valid)
            inst = &sa_genmidi_melodic[prog];
    }
    if (!inst && sa_genmidi_loaded) {
        inst = &sa_genmidi_melodic[0];
    }

    int cvol = sa_mus.ch[channel].volume;
    if (cvol <= 0) cvol = 127;
    float gain = ((float)velocity / 127.0f) * ((float)cvol / 127.0f);

    if (inst) {
        int play_dual = inst->dual && !sa_mus_single_voice;
        float vgain = play_dual ? gain * 0.5f : gain;

        SaFmVoice *slot = sa_mus_alloc_voice();
        if (!slot) return;
        sa_mus_setup_voice(slot, inst, 0, channel, note, vgain, perc);

        if (play_dual) {
            SaFmVoice *slot2 = sa_mus_alloc_voice();
            if (slot2) sa_mus_setup_voice(slot2, inst, 1, channel, note, vgain, perc);
        }
        return;
    }

    SaFmVoice *slot = sa_mus_alloc_voice();
    if (!slot) return;
    int play_note = perc ? 60 : note;
    while (play_note < 0)   play_note += 12;
    while (play_note > 127) play_note -= 12;
    float freq = sa_dmx_note_freq(play_note);
    u32 step = (u32)((double)freq * 4294967296.0 / (double)SA_SND_RATE);
    memset(slot, 0, sizeof(*slot));
    slot->active  = 1;
    slot->channel = channel;
    slot->note    = note;
    slot->vol     = gain;
    slot->mod.step = step;
    slot->mod.phase = 0;
    slot->mod.wave = 0;
    slot->mod.out_level = 0.5f;
    sa_opl_env_init(&slot->mod.env, 12, 6, 8, 6, 1);
    slot->car.step = step;
    slot->car.phase = 0;
    slot->car.wave = 0;
    slot->car.out_level = 1.0f;
    sa_opl_env_init(&slot->car.env, 12, 4, 10, 5, 1);
    slot->feedback = 3;
    slot->additive = 0;
}

static void sa_mus_note_off(int channel, int note) {
    for (int i = 0; i < SA_MUS_VOICES; i++) {
        SaFmVoice *v = &sa_mus.voices[i];
        if (v->active && v->channel == channel && v->note == note && !v->released) {
            v->released = 1;
            v->mod.env.stage = 3;
            v->car.env.stage = 3;
        }
    }
}

static void sa_mus_channel_silence(int channel) {
    for (int i = 0; i < SA_MUS_VOICES; i++) {
        SaFmVoice *v = &sa_mus.voices[i];
        if (v->active && v->channel == channel && !v->released) {
            v->released = 1;
            v->mod.env.stage = 3;
            v->car.env.stage = 3;
        }
    }
}

static int sa_mus_read_varlen(void) {
    int value = 0;
    while (sa_mus.pos < sa_mus.score_len) {
        u8 b = sa_mus.score[sa_mus.pos++];
        value = (value << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return value;
}

static void sa_mus_do_tick(void) {
    if (!sa_mus.playing || !sa_mus.score) return;
    if (sa_mus.delay_ticks > 0) { sa_mus.delay_ticks--; return; }

    while (sa_mus.playing) {
        if (sa_mus.pos >= sa_mus.score_len) {
            sa_mus.pos = sa_mus.score_start;
            sa_mus.delay_ticks = 0;
            return;
        }
        u8 event = sa_mus.score[sa_mus.pos++];
        int type    = (event >> 4) & 7;
        int channel = event & 0x0F;
        int last    = event & 0x80;

        if (type == 0) {
            if (sa_mus.pos < sa_mus.score_len) {
                int note = sa_mus.score[sa_mus.pos++] & 0x7F;
                sa_mus_note_off(channel, note);
            }
        } else if (type == 1) {
            int note = 0;
            int vel = sa_mus.ch[channel].last_velocity;
            if (sa_mus.pos < sa_mus.score_len) {
                u8 nb = sa_mus.score[sa_mus.pos++];
                note = nb & 0x7F;
                if (nb & 0x80 && sa_mus.pos < sa_mus.score_len) {
                    vel = sa_mus.score[sa_mus.pos++] & 0x7F;
                    sa_mus.ch[channel].last_velocity = vel;
                }
            }
            sa_mus_note_on(channel, note, vel);
        } else if (type == 2) {
            if (sa_mus.pos < sa_mus.score_len) sa_mus.pos++;
        } else if (type == 3) {
            if (sa_mus.pos < sa_mus.score_len) {
                int ctrl = sa_mus.score[sa_mus.pos++] & 0x7F;
                if (ctrl == 10 || ctrl == 11) sa_mus_channel_silence(channel);
            }
        } else if (type == 4) {
            int ctrl = 0, val = 0;
            if (sa_mus.pos < sa_mus.score_len) ctrl = sa_mus.score[sa_mus.pos++] & 0x7F;
            if (sa_mus.pos < sa_mus.score_len) val  = sa_mus.score[sa_mus.pos++] & 0x7F;
            if (ctrl == 0)      sa_mus.ch[channel].program = val;
            else if (ctrl == 3) sa_mus.ch[channel].volume  = val;
        } else if (type == 6) {
            sa_mus.pos = sa_mus.score_start;
            sa_mus.delay_ticks = 0;
            return;
        }

        if (last) {
            sa_mus.delay_ticks = sa_mus_read_varlen();
            if (sa_mus.delay_ticks > 0) {
                sa_mus.delay_ticks--;
                return;
            }
        }
    }
}

static const double sa_opl_fb_scale[8] = {
    0.0,
    134217728.0,
    268435456.0,
    536870912.0,
    1073741824.0,
    2147483648.0,
    4294967296.0,
    8589934592.0,
};

static double sa_opl_mod_depth = (4.0 * 4294967296.0);

static float sa_fm_voice_sample(SaFmVoice *v) {
    float mod_env = sa_opl_env_tick(&v->mod.env);
    float car_env = sa_opl_env_tick(&v->car.env);

    float fb = 0.0f;
    if (v->feedback > 0) {
        fb = (v->fb_out[0] + v->fb_out[1]) * 0.5f;
    }
    u32 mod_phase = v->mod.phase + (u32)(i64)(fb * sa_opl_fb_scale[v->feedback]);
    float mod_out = sa_opl_wave(v->mod.wave, mod_phase) * mod_env * v->mod.out_level;

    v->fb_out[0] = v->fb_out[1];
    v->fb_out[1] = mod_out;

    v->mod.phase += v->mod.step;

    float result;
    if (v->additive) {
        float car_out = sa_opl_wave(v->car.wave, v->car.phase) * car_env * v->car.out_level;
        result = (mod_out + car_out) * 0.5f;
    } else {
        u32 car_phase = v->car.phase + (u32)(i64)(mod_out * sa_opl_mod_depth);
        float car_out = sa_opl_wave(v->car.wave, car_phase) * car_env * v->car.out_level;
        result = car_out;
    }
    v->car.phase += v->car.step;

    if (v->car.env.stage == 4) v->active = 0;

    return result;
}

static void sa_mus_render(u8 *dst, int n) {
    if (!sa_mus.playing) return;
    const float amp = 32.0f;

    if (sa_mus.external) {
        for (int i = 0; i < n; i++) {
            float acc = 0.0f;
            for (int vi = 0; vi < SA_MUS_VOICES; vi++) {
                SaFmVoice *v = &sa_mus.voices[vi];
                if (!v->active) continue;
                acc += sa_fm_voice_sample(v) * v->vol;
            }
            int mixed = (int)dst[i] - 128 + (int)(acc * amp * sa_mus.master);
            if (mixed > 127)  mixed = 127;
            if (mixed < -128) mixed = -128;
            dst[i] = (u8)(mixed + 128);
        }
        return;
    }

    if (!sa_mus.score) return;
    const float samples_per_tick = (float)SA_SND_RATE / (float)SA_MUS_TICK_RATE;

    int rendered = 0;
    while (rendered < n) {
        if (sa_mus.samples_to_tick <= 0.0f) {
            sa_mus_do_tick();
            sa_mus.samples_to_tick += samples_per_tick;
        }
        int block = n - rendered;
        if ((float)block > sa_mus.samples_to_tick)
            block = (int)sa_mus.samples_to_tick;
        if (block <= 0) block = 1;

        for (int i = 0; i < block; i++) {
            float acc = 0.0f;
            for (int vi = 0; vi < SA_MUS_VOICES; vi++) {
                SaFmVoice *v = &sa_mus.voices[vi];
                if (!v->active) continue;
                acc += sa_fm_voice_sample(v) * v->vol;
            }
            int idx = rendered + i;
            int mixed = (int)dst[idx] - 128 + (int)(acc * amp * sa_mus.master);
            if (mixed > 127)  mixed = 127;
            if (mixed < -128) mixed = -128;
            dst[idx] = (u8)(mixed + 128);
        }
        sa_mus.samples_to_tick -= (float)block;
        rendered += block;
    }
}

static int sa_mus_start(const u8 *buf, int len) {
    if (!buf || len < 16) return 0;
    if (buf[0] != 'M' || buf[1] != 'U' || buf[2] != 'S' || buf[3] != 0x1A)
        return 0;
    int score_len   = buf[4] | (buf[5] << 8);
    int score_start = buf[6] | (buf[7] << 8);
    if (score_start < 16 || score_start >= len) return 0;
    int end = score_start + score_len;
    if (score_len <= 0 || end > len) end = len;

    sa_opl_build_tables();

    u8 *copy = (u8*)malloc((size_t)len);
    if (!copy) return 0;
    memcpy(copy, buf, (size_t)len);

    pthread_mutex_lock(&sa_snd.lock);
    if (sa_mus.score) free(sa_mus.score);
    memset(&sa_mus, 0, sizeof(sa_mus));
    sa_mus.score        = copy;
    sa_mus.score_len    = end;
    sa_mus.score_start  = score_start;
    sa_mus.pos          = score_start;
    sa_mus.delay_ticks  = 0;
    sa_mus.samples_to_tick = 0.0f;
    sa_mus.master       = 1.0f;
    for (int i = 0; i < SA_MUS_CHANNELS; i++) {
        sa_mus.ch[i].volume = 127;
        sa_mus.ch[i].last_velocity = 100;
    }
    sa_mus.playing      = 1;
    pthread_mutex_unlock(&sa_snd.lock);
    return 1;
}

static void sa_mus_stop(void) {
    pthread_mutex_lock(&sa_snd.lock);
    sa_mus.playing = 0;
    sa_mus.external = 0;
    for (int i = 0; i < SA_MUS_VOICES; i++) sa_mus.voices[i].active = 0;
    if (sa_mus.score) { free(sa_mus.score); sa_mus.score = NULL; }
    pthread_mutex_unlock(&sa_snd.lock);
}

/* ------------------------------------------------------------------
 * MIDI -> FM synthesizer playback.
 *
 * Parses a Standard MIDI File, then drives the OPL2 FM synth on a
 * background pthread - the same engine used for MUS playback but fed
 * from MIDI events. Uses nanosleep() for inter-event timing.
 * ---------------------------------------------------------------- */
#define SA_MF_MAX_EVENTS 32768

typedef struct {
    u32 tick;
    u8  status;
    u8  data1;
    u8  data2;
} SaMidiFmEvent;

static struct {
    volatile int   playing;
    pthread_t      thread;
    SaMidiFmEvent *events;
    int            n_events;
    int            divisions;
    int            tempo;
} sa_midi_fm = {0};

static u32 sa_mf_read_vlq(const u8 *buf, int len, int *pos) {
    u32 val = 0;
    while (*pos < len) {
        u8 b = buf[(*pos)++];
        val = (val << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return val;
}

static void sa_mf_parse_track(const u8 *data, int tlen) {
    int pos = 0;
    u32 abs_tick = 0;
    u8 running = 0;

    while (pos < tlen && sa_midi_fm.n_events < SA_MF_MAX_EVENTS) {
        u32 delta = sa_mf_read_vlq(data, tlen, &pos);
        abs_tick += delta;
        if (pos >= tlen) break;

        u8 b = data[pos];
        if (b == 0xFF) {
            pos++;
            if (pos >= tlen) break;
            u8 mtype = data[pos++];
            u32 mlen = sa_mf_read_vlq(data, tlen, &pos);
            if (mtype == 0x51 && mlen == 3 && pos + 3 <= tlen) {
                sa_midi_fm.tempo = ((int)data[pos] << 16) |
                                   ((int)data[pos+1] << 8) |
                                   (int)data[pos+2];
            }
            pos += (int)mlen;
        } else if (b == 0xF0 || b == 0xF7) {
            pos++;
            u32 slen = sa_mf_read_vlq(data, tlen, &pos);
            pos += (int)slen;
        } else {
            u8 status, d1, d2 = 0;
            if (b & 0x80) { status = b; running = b; pos++; }
            else           { status = running; }
            if (pos >= tlen) break;
            d1 = data[pos++];
            u8 hi = status & 0xF0;
            if (hi != 0xC0 && hi != 0xD0) {
                if (pos >= tlen) break;
                d2 = data[pos++];
            }
            SaMidiFmEvent *ev = &sa_midi_fm.events[sa_midi_fm.n_events++];
            ev->tick   = abs_tick;
            ev->status = status;
            ev->data1  = d1;
            ev->data2  = d2;
        }
    }
}

static int sa_mf_event_cmp(const void *a, const void *b) {
    const SaMidiFmEvent *ea = (const SaMidiFmEvent *)a;
    const SaMidiFmEvent *eb = (const SaMidiFmEvent *)b;
    if (ea->tick < eb->tick) return -1;
    if (ea->tick > eb->tick) return  1;
    return 0;
}

static void *sa_mf_thread_proc(void *param) {
    (void)param;

    while (sa_midi_fm.playing) {
        u32 last_tick = 0;
        for (int i = 0; i < sa_midi_fm.n_events && sa_midi_fm.playing; i++) {
            SaMidiFmEvent *ev = &sa_midi_fm.events[i];
            if (ev->tick > last_tick) {
                u32 dt = ev->tick - last_tick;
                double us = (double)dt * (double)sa_midi_fm.tempo
                            / (double)sa_midi_fm.divisions;
                struct timespec ts;
                ts.tv_sec  = (time_t)(us / 1000000.0);
                ts.tv_nsec = (long)((us - (double)ts.tv_sec * 1000000.0) * 1000.0);
                while (nanosleep(&ts, &ts) == -1 && sa_midi_fm.playing)
                    ;
                last_tick = ev->tick;
            }
            if (!sa_midi_fm.playing) break;

            u8 hi    = ev->status & 0xF0;
            u8 ch    = ev->status & 0x0F;
            int fm_ch = (ch == 9) ? 15 : (ch < 15 ? ch : 14);

            pthread_mutex_lock(&sa_snd.lock);
            switch (hi) {
                case 0x80:
                    sa_mus_note_off(fm_ch, ev->data1);
                    break;
                case 0x90:
                    if (ev->data2 == 0)
                        sa_mus_note_off(fm_ch, ev->data1);
                    else
                        sa_mus_note_on(fm_ch, ev->data1, ev->data2);
                    break;
                case 0xB0:
                    if (ev->data1 == 7)
                        sa_mus.ch[fm_ch].volume = ev->data2;
                    break;
                case 0xC0:
                    sa_mus.ch[fm_ch].program = ev->data1 & 0x7F;
                    break;
                default:
                    break;
            }
            pthread_mutex_unlock(&sa_snd.lock);
        }
    }
    return NULL;
}

static void sa_midi_fm_stop(void) {
    if (!sa_midi_fm.playing) return;
    sa_midi_fm.playing = 0;
    pthread_join(sa_midi_fm.thread, NULL);
    if (sa_midi_fm.events) { free(sa_midi_fm.events); sa_midi_fm.events = NULL; }
    sa_midi_fm.n_events = 0;
}

static int sa_midi_fm_play_start(const u8 *buf, int len) {
    if (!buf || len < 14) return 0;
    if (buf[0]!='M'||buf[1]!='T'||buf[2]!='h'||buf[3]!='d') return 0;

    int hdr_len  = (buf[4]<<24)|(buf[5]<<16)|(buf[6]<<8)|buf[7];
    int format   = (buf[8]<<8)|buf[9];
    int ntracks  = (buf[10]<<8)|buf[11];
    int division = (buf[12]<<8)|buf[13];
    if (format > 1 || ntracks < 1 || division <= 0) return 0;

    sa_midi_fm_stop();

    sa_midi_fm.events = (SaMidiFmEvent*)malloc(SA_MF_MAX_EVENTS * sizeof(SaMidiFmEvent));
    if (!sa_midi_fm.events) return 0;
    sa_midi_fm.n_events  = 0;
    sa_midi_fm.divisions = division;
    sa_midi_fm.tempo     = 500000;

    int pos = 8 + hdr_len;
    for (int t = 0; t < ntracks && pos + 8 <= len; t++) {
        if (buf[pos]!='M'||buf[pos+1]!='T'||buf[pos+2]!='r'||buf[pos+3]!='k') {
            pos += 8; continue;
        }
        int tlen = (buf[pos+4]<<24)|(buf[pos+5]<<16)|(buf[pos+6]<<8)|buf[pos+7];
        pos += 8;
        if (pos + tlen > len) tlen = len - pos;
        sa_mf_parse_track(buf + pos, tlen);
        pos += tlen;
    }

    if (sa_midi_fm.n_events == 0) {
        free(sa_midi_fm.events); sa_midi_fm.events = NULL;
        return 0;
    }
    qsort(sa_midi_fm.events, sa_midi_fm.n_events, sizeof(SaMidiFmEvent),
          sa_mf_event_cmp);

    sa_opl_build_tables();
    pthread_mutex_lock(&sa_snd.lock);
    if (sa_mus.score) { free(sa_mus.score); sa_mus.score = NULL; }
    for (int i = 0; i < SA_MUS_VOICES; i++) sa_mus.voices[i].active = 0;
    for (int i = 0; i < SA_MUS_CHANNELS; i++) {
        sa_mus.ch[i].volume = 127;
        sa_mus.ch[i].last_velocity = 100;
        sa_mus.ch[i].program = 0;
    }
    sa_mus.master   = 1.0f;
    sa_mus.external = 1;
    sa_mus.playing  = 1;
    pthread_mutex_unlock(&sa_snd.lock);

    sa_midi_fm.playing = 1;
    if (pthread_create(&sa_midi_fm.thread, NULL, sa_mf_thread_proc, NULL) != 0) {
        sa_midi_fm.playing = 0;
        free(sa_midi_fm.events); sa_midi_fm.events = NULL;
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------
 * @music(ptr, len, cmd)
 *   cmd=0: stop all music
 *   cmd=1: OPL2 FM soft-synth (MUS direct or MIDI via FM thread)
 *   cmd=2: same as 1 (no native MIDI out exists on Solaris)
 *   cmd=3: load a GENMIDI bank from buf into the FM instrument tables
 * ---------------------------------------------------------------- */
static void sa_snd_music(const u8 *buf, int len, int cmd) {
    if (cmd == 0) {
        sa_midi_fm_stop();
        sa_mus_stop();
        sa_musdec_stop();
        return;
    }

    if (!buf || len <= 0) return;

    if (cmd == 3) {
        sa_genmidi_load(buf, len);
        return;
    }

    sa_midi_fm_stop();
    sa_mus_stop();
    /* Ogg Vorbis / MP3 / FLAC / WAV: decoded and streamed into the mixer
     * (looping); replaces any previous stream on success. */
    if (sa_musdec_start(buf, len)) return;
    sa_musdec_stop();

    int is_midi = (len >= 4 && buf[0]=='M' && buf[1]=='T' && buf[2]=='h' && buf[3]=='d');
    int is_mus  = (len >= 4 && buf[0]=='M' && buf[1]=='U' && buf[2]=='S' && buf[3]==0x1A);

    sa_mus_no_cymbals   = 0;
    sa_mus_single_voice = 0;
    sa_opl_mod_depth    = (4.0 * 4294967296.0);

    if (is_mus) {
        sa_mus_start(buf, len);
    } else if (is_midi) {
        sa_midi_fm_play_start(buf, len);
    }
}

/* @mixer(cmd, value): 0 master, 1 music, 2 sfx, 3 mute toggle. */
static void sa_snd_mixer(int cmd, int value) {
    float f = (float)(value & 0xFF) / 255.0f;

    switch (cmd & 0xFF) {
        case 0: sa_master_vol = f; break;
        case 1:
            pthread_mutex_lock(&sa_snd.lock);
            sa_mus.master = f;
            pthread_mutex_unlock(&sa_snd.lock);
            break;
        case 2: sa_sfx_vol = f; break;
        case 3:
            if (value == 0) {
                if (sa_muted_master < 0.0f) {
                    sa_muted_master = sa_master_vol;
                    sa_master_vol = 0.0f;
                }
            } else {
                if (sa_muted_master >= 0.0f) {
                    sa_master_vol = sa_muted_master;
                    sa_muted_master = -1.0f;
                }
            }
            break;
        default: break;
    }
}

#endif /* RELIC_SUNAUDIO_H */
