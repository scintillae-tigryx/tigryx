/*
 * backend_relic3d_snd.h
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
 * backend_relic3d_snd.h
 *
 * Sound and music subsystem for the Relic3D Win32 backend.
 * Included directly into backend_relic3d_win32.c.
 *
 * This file provides:
 *   - PCM voice mixer over WinMM waveOut
 *   - OPL2 FM synthesiser (software emulation)
 *   - MUS sequencer (MUS music format)
 *   - MIDI parser + native Windows MIDI output
 *   - Preset tone effects and raw PCM playback
 *   - MUS-to-MIDI format conversion
 */

/* =========================================================================
 * Sound subsystem — PCM voice mixer
 * ========================================================================= */

#define SND_SAMPLE_RATE  22050
#define SND_AMPLITUDE    110
#define SND_SILENCE      128
#define SND_CHUNK_SAMPLES 512
#define SND_STREAM_BUFFERS 3
#define SND_MAX_VOICES   16

typedef struct {
    u8 *data;
    int len;
    int pos;
    int volume;
    int active;
} R3DSndVoice;

typedef struct {
    HWAVEOUT         hwo;
    WAVEFORMATEX     wfx;
    CRITICAL_SECTION lock;
    HANDLE           thread;
    int              running;
    u8               bufs[SND_STREAM_BUFFERS][SND_CHUNK_SAMPLES];
    WAVEHDR          hdrs[SND_STREAM_BUFFERS];
    R3DSndVoice      voices[SND_MAX_VOICES];
} R3DSndState;

static R3DSndState g_snd = {0};

static float g_master_vol = 1.0f;
static float g_sfx_vol = 1.0f;
static float g_muted_master = -1.0f;

static int r3d_snd_fill_sweep(u8 *buf, int buf_sz,
                               int freq_start, int freq_end, int ms, int volume)
{
    int samples = (SND_SAMPLE_RATE * ms) / 1000;
    if (samples > buf_sz) samples = buf_sz;
    if (volume < 0)   volume = 0;
    if (volume > 255) volume = 255;
    int amplitude = (SND_AMPLITUDE * volume) / 255;

    static float filter_state = 0.0f;
    const float alpha = 0.15f;

    for (int i = 0; i < samples; i++) {
        float t    = (samples > 1) ? (float)i / (float)(samples - 1) : 0.0f;
        float freq = (float)freq_start + t * (float)(freq_end - freq_start);
        if (freq < 20.0f) freq = 20.0f;
        float half = (float)SND_SAMPLE_RATE / (2.0f * freq);
        int phase = (int)((float)i / half) & 1;
        float env = 1.0f;
        float ms_f = (float)ms;
        float t_ms = (float)i * 1000.0f / (float)SND_SAMPLE_RATE;
        if (t_ms < 5.0f)         env = t_ms / 5.0f;
        else if (t_ms > ms_f-5.0f) env = (ms_f - t_ms) / 5.0f;
        if (env < 0.0f) env = 0.0f;
        int val = SND_SILENCE + (phase ? 1 : -1) * (int)(amplitude * env);

        float input = (float)val;
        float output = alpha * input + (1.0f - alpha) * filter_state;
        filter_state = output;
        buf[i] = (u8)((int)(output + 0.5f));
    }
    return samples;
}

static void mus_render(u8 *dst, int n);

static int r3d_snd_mixer_fill(u8 *dst, int dst_samples)
{
    memset(dst, SND_SILENCE, dst_samples);

    EnterCriticalSection(&g_snd.lock);
    mus_render(dst, dst_samples);
    for (int v = 0; v < SND_MAX_VOICES; v++) {
        if (!g_snd.voices[v].active) continue;
        R3DSndVoice *voice = &g_snd.voices[v];
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
    LeaveCriticalSection(&g_snd.lock);

    if (g_master_vol < 1.0f) {
        for (int i = 0; i < dst_samples; i++) {
            int s = (int)dst[i] - 128;
            s = (int)(s * g_master_vol);
            dst[i] = (u8)(s + 128);
        }
    }
    return dst_samples;
}

static int r3d_snd_start_voice(const u8 *buf, int len, int volume)
{
    if (!g_snd.running || len <= 0) return 0;
    u8 *copy = (u8*)malloc(len);
    if (!copy) return 0;
    memcpy(copy, buf, len);

    EnterCriticalSection(&g_snd.lock);
    int started = 0;
    for (int i = 0; i < SND_MAX_VOICES; i++) {
        if (!g_snd.voices[i].active) {
            g_snd.voices[i].data   = copy;
            g_snd.voices[i].len    = len;
            g_snd.voices[i].pos    = 0;
            g_snd.voices[i].volume = volume;
            g_snd.voices[i].active = 1;
            started = 1;
            break;
        }
    }
    if (!started) free(copy);
    LeaveCriticalSection(&g_snd.lock);
    return started;
}

static DWORD WINAPI r3d_snd_thread_proc(LPVOID param)
{
    (void)param;

    for (int i = 0; i < SND_STREAM_BUFFERS; i++) {
        r3d_snd_mixer_fill(g_snd.bufs[i], SND_CHUNK_SAMPLES);
        waveOutWrite(g_snd.hwo, &g_snd.hdrs[i], sizeof(WAVEHDR));
    }

    while (g_snd.running) {
        int refilled = 0;
        for (int i = 0; i < SND_STREAM_BUFFERS; i++) {
            if (g_snd.hdrs[i].dwFlags & WHDR_DONE) {
                waveOutUnprepareHeader(g_snd.hwo, &g_snd.hdrs[i], sizeof(WAVEHDR));
                r3d_snd_mixer_fill(g_snd.bufs[i], SND_CHUNK_SAMPLES);
                waveOutPrepareHeader(g_snd.hwo, &g_snd.hdrs[i], sizeof(WAVEHDR));
                waveOutWrite(g_snd.hwo, &g_snd.hdrs[i], sizeof(WAVEHDR));
                refilled++;
            }
        }
        if (!refilled) Sleep(5);
    }

    waveOutReset(g_snd.hwo);
    return 0;
}

static void r3d_snd_init(void)
{
    InitializeCriticalSection(&g_snd.lock);
    g_snd.running = 1;

    g_snd.wfx.wFormatTag      = WAVE_FORMAT_PCM;
    g_snd.wfx.nChannels       = 1;
    g_snd.wfx.nSamplesPerSec  = SND_SAMPLE_RATE;
    g_snd.wfx.wBitsPerSample  = 8;
    g_snd.wfx.nBlockAlign     = 1;
    g_snd.wfx.nAvgBytesPerSec = SND_SAMPLE_RATE;

    if (waveOutOpen(&g_snd.hwo, WAVE_MAPPER, &g_snd.wfx,
                    0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_snd.hwo = NULL;
        return;
    }

    for (int i = 0; i < SND_STREAM_BUFFERS; i++) {
        memset(&g_snd.hdrs[i], 0, sizeof(WAVEHDR));
        g_snd.hdrs[i].lpData         = (LPSTR)g_snd.bufs[i];
        g_snd.hdrs[i].dwBufferLength = SND_CHUNK_SAMPLES;
        waveOutPrepareHeader(g_snd.hwo, &g_snd.hdrs[i], sizeof(WAVEHDR));
    }

    g_snd.thread = CreateThread(NULL, 0, r3d_snd_thread_proc, NULL, 0, NULL);
}

static void midi_play_stop(void);

static void r3d_snd_shutdown(void)
{
    midi_play_stop();
    g_snd.running = 0;
    if (g_snd.thread) {
        WaitForSingleObject(g_snd.thread, 2000);
        CloseHandle(g_snd.thread);
        g_snd.thread = NULL;
    }
    if (g_snd.hwo) {
        waveOutReset(g_snd.hwo);
        for (int i = 0; i < SND_STREAM_BUFFERS; i++) {
            waveOutUnprepareHeader(g_snd.hwo, &g_snd.hdrs[i], sizeof(WAVEHDR));
        }
        waveOutClose(g_snd.hwo);
        g_snd.hwo = NULL;
    }
    EnterCriticalSection(&g_snd.lock);
    for (int i = 0; i < SND_MAX_VOICES; i++) {
        if (g_snd.voices[i].data) {
            free(g_snd.voices[i].data);
            g_snd.voices[i].data = NULL;
        }
        g_snd.voices[i].active = 0;
    }
    LeaveCriticalSection(&g_snd.lock);
    DeleteCriticalSection(&g_snd.lock);
}

/* =========================================================================
 * OPL2 FM synthesiser + MUS sequencer + MIDI player
 * ========================================================================= */

#define MUS_MAX_CHANNELS  16
#define MUS_MAX_VOICES    24
#define MUS_TICK_RATE     140
#define MUS_PERC_CHANNEL  15

static float opl_sin_table[1024];
static int   opl_tables_ready = 0;

/** @brief Build the OPL waveform lookup table. */
static void opl_build_tables(void) {
    if (opl_tables_ready) return;
    for (int i = 0; i < 1024; i++)
        opl_sin_table[i] = sinf((float)i * 6.283185307f / 1024.0f);
    opl_tables_ready = 1;
}

static float opl_wave(int waveform, u32 phase) {
    int idx = (int)((phase >> 22) & 0x3FF);
    switch (waveform & 7) {
        case 0: return opl_sin_table[idx];
        case 1: return (idx < 512) ? opl_sin_table[idx] : 0.0f;
        case 2: return (opl_sin_table[idx] < 0) ? -opl_sin_table[idx] : opl_sin_table[idx];
        case 3:
            if (idx < 256)  return opl_sin_table[idx];
            if (idx < 512)  return 0.0f;
            if (idx < 768)  return opl_sin_table[idx - 512];
            return 0.0f;
        case 4: return (idx < 512) ? opl_sin_table[(idx * 2) & 0x3FF] : 0.0f;
        case 5: {
            if (idx >= 512) return 0.0f;
            float s = opl_sin_table[(idx * 2) & 0x3FF];
            return (s < 0) ? -s : s;
        }
        case 6: return (idx < 512) ? 1.0f : -1.0f;
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
} OplEnv;

static float opl_decay_mul(int rate) {
    if (rate <= 0) return 1.0f;
    if (rate > 15) rate = 15;
    double f = -7.4493 * (1.0 / 39.28064) / (double)SND_SAMPLE_RATE;
    return (float)pow(2.0, f * pow(2.0, (double)rate));
}

static float opl_attack_inc(int rate) {
    if (rate <= 0) return 0.0f;
    if (rate >= 15) return 1.0f;
    float inc = (float)pow(2.0, (double)rate) / 32768.0f;
    inc *= 22050.0f / (float)SND_SAMPLE_RATE;
    if (inc > 1.0f) inc = 1.0f;
    return inc;
}

static void opl_env_init(OplEnv *e, int atk, int dec, int sus, int rel, int eg) {
    e->level     = 0.0f;
    e->stage     = 0;
    e->atk_inc   = opl_attack_inc(atk);
    e->dec_mul   = opl_decay_mul(dec);
    e->sus_level = (float)pow(2.0, -0.5 * (double)sus);
    e->rel_mul   = opl_decay_mul(rel > 0 ? rel : 1);
    e->eg_flag   = eg;
}

#define OPL_ENV_SILENCE 0.0005f

static float opl_env_tick(OplEnv *e) {
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
            if (e->level <= OPL_ENV_SILENCE) { e->level = 0.0f; e->stage = 4; }
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
} OplOpDef;

typedef struct {
    OplOpDef mod;
    OplOpDef car;
    int      feedback;
    int      additive;
    int      note_offset;
} OplVoiceDef;

typedef struct {
    int         valid;
    int         dual;
    int         fixed;
    int         fixed_note;
    OplVoiceDef v[2];
} OplInstrument;

static OplInstrument g_genmidi_melodic[128];
static OplInstrument g_genmidi_perc[47];
static int           g_genmidi_loaded = 0;

static void opl_parse_op(const u8 *d, OplOpDef *op) {
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

static void opl_parse_voice_def(const u8 *v, OplOpDef *mod, OplOpDef *car,
                                 int *feedback, int *additive, int *note_offset) {
    opl_parse_op(v + 0, mod);
    *feedback = (v[6] >> 1) & 7;
    *additive = v[6] & 1;
    opl_parse_op(v + 7, car);
    *note_offset = (i16)(v[14] | (v[15] << 8));
}

static void genmidi_load(const u8 *lump, int lump_len) {
    if (!lump || lump_len < 8 + 175 * 36) return;
    if (memcmp(lump, "#OPL_II#", 8) != 0) return;

    const u8 *p = lump + 8;
    for (int i = 0; i < 175; i++, p += 36) {
        OplInstrument *inst = (i < 128) ? &g_genmidi_melodic[i]
                                        : &g_genmidi_perc[i - 128];
        u16 flags = p[0] | (p[1] << 8);
        inst->dual  = (flags & 0x04) ? 1 : 0;
        inst->fixed = (flags & 0x01) ? 1 : 0;
        inst->fixed_note = p[3];
        inst->valid = 1;

        opl_parse_voice_def(p + 4, &inst->v[0].mod, &inst->v[0].car,
                            &inst->v[0].feedback, &inst->v[0].additive,
                            &inst->v[0].note_offset);
        opl_parse_voice_def(p + 20, &inst->v[1].mod, &inst->v[1].car,
                            &inst->v[1].feedback, &inst->v[1].additive,
                            &inst->v[1].note_offset);
    }
    g_genmidi_loaded = 1;
}

static const float opl_mult_table[16] = {
    0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f,
    8.0f, 9.0f, 10.0f, 10.0f, 12.0f, 12.0f, 15.0f, 15.0f
};

typedef struct {
    u32   phase;
    u32   step;
    OplEnv env;
    int   wave;
    float out_level;
} OplOp;

typedef struct {
    int   active;
    int   channel;
    int   note;
    float vol;
    int   released;
    OplOp mod;
    OplOp car;
    int   feedback;
    int   additive;
    float fb_out[2];
} FmVoice;

typedef struct {
    int program;
    int volume;
    int last_velocity;
} MusChannel;

typedef struct {
    int        playing;
    int        external;
    u8        *score;
    int        score_len;
    int        score_start;
    int        pos;
    int        delay_ticks;
    float      samples_to_tick;
    float      master;
    MusChannel ch[MUS_MAX_CHANNELS];
    FmVoice    voices[MUS_MAX_VOICES];
} MusPlayer;

static MusPlayer g_mus = {0};

static int g_mus_no_cymbals = 0;
static int g_mus_single_voice = 0;

static int mus_note_is_cymbal(int note) {
    switch (note) {
        case 42: case 44: case 46:
        case 49: case 51: case 52: case 53:
        case 55: case 57: case 59:
            return 1;
        default:
            return 0;
    }
}

static float dmx_note_freq(int note) {
    return (float)(440.0 * pow(2.0, ((double)note - 57.0) / 12.0));
}

static void fm_voice_setup_op(OplOp *op, const OplOpDef *def, float freq, float mult_factor) {
    float op_freq = freq * mult_factor;
    op->step = (u32)((double)op_freq * 4294967296.0 / (double)SND_SAMPLE_RATE);
    op->phase = 0;
    op->wave = def->wave;
    op->out_level = (float)pow(2.0, -0.125 * (double)def->volume);
    opl_env_init(&op->env, def->attack, def->decay, def->sustain, def->release, def->eg);
}

static FmVoice *mus_alloc_voice(void) {
    FmVoice *slot = NULL;
    for (int i = 0; i < MUS_MAX_VOICES; i++) {
        if (!g_mus.voices[i].active) { slot = &g_mus.voices[i]; break; }
    }
    if (!slot) {
        float lowest = 1e9f;
        for (int i = 0; i < MUS_MAX_VOICES; i++) {
            if (g_mus.voices[i].released) {
                float e = g_mus.voices[i].car.env.level;
                if (e < lowest) { lowest = e; slot = &g_mus.voices[i]; }
            }
        }
    }
    if (!slot) {
        float lowest = 1e9f;
        for (int i = 0; i < MUS_MAX_VOICES; i++) {
            float e = g_mus.voices[i].car.env.level;
            if (e < lowest) { lowest = e; slot = &g_mus.voices[i]; }
        }
    }
    return slot;
}

static void mus_setup_voice(FmVoice *slot, const OplInstrument *inst, int vidx,
                            int channel, int note, float gain, int perc) {
    const OplVoiceDef *vd = &inst->v[vidx];
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

    float freq = dmx_note_freq(play_note);

    memset(slot, 0, sizeof(*slot));
    slot->active   = 1;
    slot->channel  = channel;
    slot->note     = note;
    slot->vol      = gain;
    slot->feedback = vd->feedback;
    slot->additive = vd->additive;
    fm_voice_setup_op(&slot->mod, &vd->mod, freq, opl_mult_table[vd->mod.mult & 0xF]);
    fm_voice_setup_op(&slot->car, &vd->car, freq, opl_mult_table[vd->car.mult & 0xF]);
}

static void mus_note_on(int channel, int note, int velocity) {
    int perc = (channel == MUS_PERC_CHANNEL) ? 1 : 0;

    if (perc && g_mus_no_cymbals && mus_note_is_cymbal(note)) return;

    OplInstrument *inst = NULL;
    if (perc) {
        int pi = note - 35;
        if (pi >= 0 && pi < 47 && g_genmidi_perc[pi].valid)
            inst = &g_genmidi_perc[pi];
    } else {
        int prog = g_mus.ch[channel].program;
        if (prog >= 0 && prog < 128 && g_genmidi_melodic[prog].valid)
            inst = &g_genmidi_melodic[prog];
    }
    if (!inst && g_genmidi_loaded) {
        inst = &g_genmidi_melodic[0];
    }

    int cvol = g_mus.ch[channel].volume;
    if (cvol <= 0) cvol = 127;
    float gain = ((float)velocity / 127.0f) * ((float)cvol / 127.0f);

    if (inst) {
        int play_dual = inst->dual && !g_mus_single_voice;
        float vgain = play_dual ? gain * 0.5f : gain;

        FmVoice *slot = mus_alloc_voice();
        if (!slot) return;
        mus_setup_voice(slot, inst, 0, channel, note, vgain, perc);

        if (play_dual) {
            FmVoice *slot2 = mus_alloc_voice();
            if (slot2) mus_setup_voice(slot2, inst, 1, channel, note, vgain, perc);
        }
        return;
    }

    FmVoice *slot = mus_alloc_voice();
    if (!slot) return;
    int play_note = perc ? 60 : note;
    while (play_note < 0)   play_note += 12;
    while (play_note > 127) play_note -= 12;
    float freq = dmx_note_freq(play_note);
    u32 step = (u32)((double)freq * 4294967296.0 / (double)SND_SAMPLE_RATE);
    memset(slot, 0, sizeof(*slot));
    slot->active  = 1;
    slot->channel = channel;
    slot->note    = note;
    slot->vol     = gain;
    slot->mod.step = step;
    slot->mod.phase = 0;
    slot->mod.wave = 0;
    slot->mod.out_level = 0.5f;
    opl_env_init(&slot->mod.env, 12, 6, 8, 6, 1);
    slot->car.step = step;
    slot->car.phase = 0;
    slot->car.wave = 0;
    slot->car.out_level = 1.0f;
    opl_env_init(&slot->car.env, 12, 4, 10, 5, 1);
    slot->feedback = 3;
    slot->additive = 0;
}

static void mus_note_off(int channel, int note) {
    for (int i = 0; i < MUS_MAX_VOICES; i++) {
        FmVoice *v = &g_mus.voices[i];
        if (v->active && v->channel == channel && v->note == note && !v->released) {
            v->released = 1;
            v->mod.env.stage = 3;
            v->car.env.stage = 3;
        }
    }
}

static void mus_channel_silence(int channel) {
    for (int i = 0; i < MUS_MAX_VOICES; i++) {
        FmVoice *v = &g_mus.voices[i];
        if (v->active && v->channel == channel && !v->released) {
            v->released = 1;
            v->mod.env.stage = 3;
            v->car.env.stage = 3;
        }
    }
}

static int mus_read_varlen(void) {
    int value = 0;
    while (g_mus.pos < g_mus.score_len) {
        u8 b = g_mus.score[g_mus.pos++];
        value = (value << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return value;
}

static void mus_do_tick(void) {
    if (!g_mus.playing || !g_mus.score) return;
    if (g_mus.delay_ticks > 0) { g_mus.delay_ticks--; return; }

    while (g_mus.playing) {
        if (g_mus.pos >= g_mus.score_len) {
            g_mus.pos = g_mus.score_start;
            g_mus.delay_ticks = 0;
            return;
        }
        u8 event = g_mus.score[g_mus.pos++];
        int type    = (event >> 4) & 7;
        int channel = event & 0x0F;
        int last    = event & 0x80;

        if (type == 0) {
            if (g_mus.pos < g_mus.score_len) {
                int note = g_mus.score[g_mus.pos++] & 0x7F;
                mus_note_off(channel, note);
            }
        } else if (type == 1) {
            int note = 0;
            int vel = g_mus.ch[channel].last_velocity;
            if (g_mus.pos < g_mus.score_len) {
                u8 nb = g_mus.score[g_mus.pos++];
                note = nb & 0x7F;
                if (nb & 0x80 && g_mus.pos < g_mus.score_len) {
                    vel = g_mus.score[g_mus.pos++] & 0x7F;
                    g_mus.ch[channel].last_velocity = vel;
                }
            }
            mus_note_on(channel, note, vel);
        } else if (type == 2) {
            if (g_mus.pos < g_mus.score_len) g_mus.pos++;
        } else if (type == 3) {
            if (g_mus.pos < g_mus.score_len) {
                int ctrl = g_mus.score[g_mus.pos++] & 0x7F;
                if (ctrl == 10 || ctrl == 11) mus_channel_silence(channel);
            }
        } else if (type == 4) {
            int ctrl = 0, val = 0;
            if (g_mus.pos < g_mus.score_len) ctrl = g_mus.score[g_mus.pos++] & 0x7F;
            if (g_mus.pos < g_mus.score_len) val  = g_mus.score[g_mus.pos++] & 0x7F;
            if (ctrl == 0)      g_mus.ch[channel].program = val;
            else if (ctrl == 3) g_mus.ch[channel].volume  = val;
        } else if (type == 6) {
            g_mus.pos = g_mus.score_start;
            g_mus.delay_ticks = 0;
            return;
        }

        if (last) {
            g_mus.delay_ticks = mus_read_varlen();
            if (g_mus.delay_ticks > 0) {
                g_mus.delay_ticks--;
                return;
            }
        }
    }
}

static const double opl_fb_scale[8] = {
    0.0,
    134217728.0,
    268435456.0,
    536870912.0,
    1073741824.0,
    2147483648.0,
    4294967296.0,
    8589934592.0,
};

static double g_opl_mod_depth = (4.0 * 4294967296.0);

static float fm_voice_sample(FmVoice *v) {
    float mod_env = opl_env_tick(&v->mod.env);
    float car_env = opl_env_tick(&v->car.env);

    float fb = 0.0f;
    if (v->feedback > 0) {
        fb = (v->fb_out[0] + v->fb_out[1]) * 0.5f;
    }
    u32 mod_phase = v->mod.phase + (u32)(i64)(fb * opl_fb_scale[v->feedback]);
    float mod_out = opl_wave(v->mod.wave, mod_phase) * mod_env * v->mod.out_level;

    v->fb_out[0] = v->fb_out[1];
    v->fb_out[1] = mod_out;

    v->mod.phase += v->mod.step;

    float result;
    if (v->additive) {
        float car_out = opl_wave(v->car.wave, v->car.phase) * car_env * v->car.out_level;
        result = (mod_out + car_out) * 0.5f;
    } else {
        u32 car_phase = v->car.phase + (u32)(i64)(mod_out * g_opl_mod_depth);
        float car_out = opl_wave(v->car.wave, car_phase) * car_env * v->car.out_level;
        result = car_out;
    }
    v->car.phase += v->car.step;

    if (v->car.env.stage == 4) v->active = 0;

    return result;
}

static void mus_render(u8 *dst, int n) {
    if (!g_mus.playing) return;
    const float amp = 32.0f;

    if (g_mus.external) {
        for (int i = 0; i < n; i++) {
            float acc = 0.0f;
            for (int vi = 0; vi < MUS_MAX_VOICES; vi++) {
                FmVoice *v = &g_mus.voices[vi];
                if (!v->active) continue;
                acc += fm_voice_sample(v) * v->vol;
            }
            int mixed = (int)dst[i] - 128 + (int)(acc * amp * g_mus.master);
            if (mixed > 127)  mixed = 127;
            if (mixed < -128) mixed = -128;
            dst[i] = (u8)(mixed + 128);
        }
        return;
    }

    if (!g_mus.score) return;
    const float samples_per_tick = (float)SND_SAMPLE_RATE / (float)MUS_TICK_RATE;

    int rendered = 0;
    while (rendered < n) {
        if (g_mus.samples_to_tick <= 0.0f) {
            mus_do_tick();
            g_mus.samples_to_tick += samples_per_tick;
        }
        int block = n - rendered;
        if ((float)block > g_mus.samples_to_tick)
            block = (int)g_mus.samples_to_tick;
        if (block <= 0) block = 1;

        for (int i = 0; i < block; i++) {
            float acc = 0.0f;
            for (int vi = 0; vi < MUS_MAX_VOICES; vi++) {
                FmVoice *v = &g_mus.voices[vi];
                if (!v->active) continue;
                acc += fm_voice_sample(v) * v->vol;
            }
            int idx = rendered + i;
            int mixed = (int)dst[idx] - 128 + (int)(acc * amp * g_mus.master);
            if (mixed > 127)  mixed = 127;
            if (mixed < -128) mixed = -128;
            dst[idx] = (u8)(mixed + 128);
        }
        g_mus.samples_to_tick -= (float)block;
        rendered += block;
    }
}

static int mus_start(const u8 *buf, int len) {
    if (!buf || len < 16) return 0;
    if (buf[0] != 'M' || buf[1] != 'U' || buf[2] != 'S' || buf[3] != 0x1A)
        return 0;
    int score_len   = buf[4] | (buf[5] << 8);
    int score_start = buf[6] | (buf[7] << 8);
    if (score_start < 16 || score_start >= len) return 0;
    int end = score_start + score_len;
    if (score_len <= 0 || end > len) end = len;

    opl_build_tables();

    u8 *copy = (u8*)malloc(len);
    if (!copy) return 0;
    memcpy(copy, buf, len);

    EnterCriticalSection(&g_snd.lock);
    if (g_mus.score) free(g_mus.score);
    memset(&g_mus, 0, sizeof(g_mus));
    g_mus.score        = copy;
    g_mus.score_len    = end;
    g_mus.score_start  = score_start;
    g_mus.pos          = score_start;
    g_mus.delay_ticks  = 0;
    g_mus.samples_to_tick = 0.0f;
    g_mus.master       = 1.0f;
    for (int i = 0; i < MUS_MAX_CHANNELS; i++) {
        g_mus.ch[i].volume = 127;
        g_mus.ch[i].last_velocity = 100;
    }
    g_mus.playing      = 1;
    LeaveCriticalSection(&g_snd.lock);
    return 1;
}

static void mus_stop(void) {
    EnterCriticalSection(&g_snd.lock);
    g_mus.playing = 0;
    g_mus.external = 0;
    for (int i = 0; i < MUS_MAX_VOICES; i++) g_mus.voices[i].active = 0;
    if (g_mus.score) { free(g_mus.score); g_mus.score = NULL; }
    LeaveCriticalSection(&g_snd.lock);
}

/* =========================================================================
 * MIDI parser + playback (native + FM)
 * ========================================================================= */

#define MIDI_MAX_EVENTS  32768

typedef struct {
    u32 tick;
    u8  status;
    u8  data1;
    u8  data2;
} MidiEvent;

typedef struct {
    int        playing;
    HMIDIOUT   hmo;
    HANDLE     thread;
    MidiEvent *events;
    int        n_events;
    int        divisions;
    int        tempo;
} MidiPlayer;

static MidiPlayer g_midi = {0};

static u32 midi_read_vlq(const u8 *buf, int len, int *pos) {
    u32 val = 0;
    while (*pos < len) {
        u8 b = buf[(*pos)++];
        val = (val << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return val;
}

static void midi_parse_track(const u8 *data, int tlen) {
    int pos = 0;
    u32 abs_tick = 0;
    u8 running = 0;

    while (pos < tlen && g_midi.n_events < MIDI_MAX_EVENTS) {
        u32 delta = midi_read_vlq(data, tlen, &pos);
        abs_tick += delta;
        if (pos >= tlen) break;

        u8 b = data[pos];
        if (b == 0xFF) {
            pos++;
            if (pos >= tlen) break;
            u8 mtype = data[pos++];
            u32 mlen = midi_read_vlq(data, tlen, &pos);
            if (mtype == 0x51 && mlen == 3 && pos + 3 <= tlen) {
                g_midi.tempo = ((int)data[pos] << 16) |
                               ((int)data[pos+1] << 8) |
                               (int)data[pos+2];
            }
            pos += (int)mlen;
        } else if (b == 0xF0 || b == 0xF7) {
            pos++;
            u32 slen = midi_read_vlq(data, tlen, &pos);
            pos += (int)slen;
        } else {
            u8 status, d1, d2 = 0;
            if (b & 0x80) {
                status = b;
                running = b;
                pos++;
            } else {
                status = running;
            }
            if (pos >= tlen) break;
            d1 = data[pos++];
            u8 hi = status & 0xF0;
            if (hi != 0xC0 && hi != 0xD0) {
                if (pos >= tlen) break;
                d2 = data[pos++];
            }
            MidiEvent *ev = &g_midi.events[g_midi.n_events++];
            ev->tick   = abs_tick;
            ev->status = status;
            ev->data1  = d1;
            ev->data2  = d2;
        }
    }
}

static int midi_event_cmp(const void *a, const void *b) {
    const MidiEvent *ea = (const MidiEvent *)a;
    const MidiEvent *eb = (const MidiEvent *)b;
    if (ea->tick < eb->tick) return -1;
    if (ea->tick > eb->tick) return 1;
    return 0;
}

static DWORD WINAPI midi_thread_proc(LPVOID param) {
    (void)param;
    while (g_midi.playing) {
        u32 last_tick = 0;
        for (int i = 0; i < g_midi.n_events && g_midi.playing; i++) {
            MidiEvent *ev = &g_midi.events[i];
            if (ev->tick > last_tick) {
                u32 dt = ev->tick - last_tick;
                double ms = (double)dt * ((double)g_midi.tempo / (double)g_midi.divisions) / 1000.0;
                int sleep_ms = (int)(ms + 0.5);
                if (sleep_ms > 0) {
                    while (sleep_ms > 0 && g_midi.playing) {
                        int chunk = sleep_ms > 20 ? 20 : sleep_ms;
                        Sleep(chunk);
                        sleep_ms -= chunk;
                    }
                }
                last_tick = ev->tick;
            }
            if (!g_midi.playing) break;
            u8 hi = ev->status & 0xF0;
            DWORD msg;
            if (hi == 0xC0 || hi == 0xD0) {
                msg = (DWORD)ev->status | ((DWORD)ev->data1 << 8);
            } else {
                msg = (DWORD)ev->status | ((DWORD)ev->data1 << 8) | ((DWORD)ev->data2 << 16);
            }
            midiOutShortMsg(g_midi.hmo, msg);
        }
    }
    return 0;
}

static u8 *mus_to_midi(const u8 *mus, int mus_len, int *out_len) {
    if (!mus || mus_len < 16) return NULL;
    if (mus[0]!='M'||mus[1]!='U'||mus[2]!='S'||mus[3]!=0x1A) return NULL;

    int score_len   = mus[4] | (mus[5] << 8);
    int score_start = mus[6] | (mus[7] << 8);
    if (score_start < 12 || score_start >= mus_len) return NULL;
    int score_end = score_start + score_len;
    if (score_len <= 0 || score_end > mus_len) score_end = mus_len;

    #define MUS2MIDI_CH(c) ((c)==15 ? 9 : ((c)<9 ? (c) : (c)+1))

    #define WRITE_VLQ(val) do { \
        u32 _v = (u32)(val); \
        u8 _vlq[5]; int _n = 0; \
        _vlq[_n++] = _v & 0x7F; \
        while ((_v >>= 7) > 0) { _vlq[_n++] = 0x80 | (_v & 0x7F); } \
        while (_n > 0) { if (wp < cap) mid[wp++] = _vlq[--_n]; } \
    } while(0)

    int cap = mus_len * 3 + 256;
    u8 *mid = (u8*)malloc(cap);
    if (!mid) return NULL;
    int wp = 0;

    memcpy(mid + wp, "MThd", 4); wp += 4;
    mid[wp++]=0; mid[wp++]=0; mid[wp++]=0; mid[wp++]=6;
    mid[wp++]=0; mid[wp++]=0;
    mid[wp++]=0; mid[wp++]=1;
    mid[wp++]=0; mid[wp++]=140;

    memcpy(mid + wp, "MTrk", 4); wp += 4;
    int trk_len_pos = wp;
    mid[wp++]=0; mid[wp++]=0; mid[wp++]=0; mid[wp++]=0;
    int trk_start = wp;

    mid[wp++]=0;
    mid[wp++]=0xFF; mid[wp++]=0x51; mid[wp++]=0x03;
    mid[wp++]=0x0F; mid[wp++]=0x42; mid[wp++]=0x40;

    u32 pending_delay = 0;
    int rp = score_start;
    while (rp < score_end && wp + 16 < cap) {
        u8 event = mus[rp++];
        int type    = (event >> 4) & 7;
        int channel = event & 0x0F;
        int last    = event & 0x80;
        int midi_ch = MUS2MIDI_CH(channel);

        if (type == 6) break;

        if (type == 0) {
            if (rp >= score_end) break;
            int note = mus[rp++] & 0x7F;
            WRITE_VLQ(pending_delay); pending_delay = 0;
            mid[wp++] = 0x80 | midi_ch;
            mid[wp++] = (u8)note;
            mid[wp++] = 64;
        } else if (type == 1) {
            if (rp >= score_end) break;
            u8 nb = mus[rp++];
            int note = nb & 0x7F;
            int vel = 127;
            if (nb & 0x80) {
                if (rp >= score_end) break;
                vel = mus[rp++] & 0x7F;
            }
            WRITE_VLQ(pending_delay); pending_delay = 0;
            mid[wp++] = 0x90 | midi_ch;
            mid[wp++] = (u8)note;
            mid[wp++] = (u8)vel;
        } else if (type == 2) {
            if (rp >= score_end) break;
            int bend_val = mus[rp++];
            int midi_bend = bend_val * 64;
            WRITE_VLQ(pending_delay); pending_delay = 0;
            mid[wp++] = 0xE0 | midi_ch;
            mid[wp++] = midi_bend & 0x7F;
            mid[wp++] = (midi_bend >> 7) & 0x7F;
        } else if (type == 3) {
            if (rp >= score_end) break;
            int ctrl = mus[rp++] & 0x7F;
            WRITE_VLQ(pending_delay); pending_delay = 0;
            mid[wp++] = 0xB0 | midi_ch;
            if (ctrl == 10 || ctrl == 11) { mid[wp++]=123; mid[wp++]=0; }
            else if (ctrl == 12)          { mid[wp++]=120; mid[wp++]=0; }
            else if (ctrl == 14)          { mid[wp++]=121; mid[wp++]=0; }
            else                          { mid[wp++]=0;   mid[wp++]=0; }
        } else if (type == 4) {
            if (rp + 1 >= score_end) break;
            int ctrl = mus[rp++] & 0x7F;
            int val  = mus[rp++] & 0x7F;
            WRITE_VLQ(pending_delay); pending_delay = 0;
            if (ctrl == 0) {
                mid[wp++] = 0xC0 | midi_ch;
                mid[wp++] = (u8)val;
            } else {
                mid[wp++] = 0xB0 | midi_ch;
                switch (ctrl) {
                    case 1: mid[wp++]=0;  mid[wp++]=(u8)val; break;
                    case 2: mid[wp++]=1;  mid[wp++]=(u8)val; break;
                    case 3: mid[wp++]=7;  mid[wp++]=(u8)val; break;
                    case 4: mid[wp++]=10; mid[wp++]=(u8)val; break;
                    case 5: mid[wp++]=11; mid[wp++]=(u8)val; break;
                    case 6: mid[wp++]=91; mid[wp++]=(u8)val; break;
                    case 7: mid[wp++]=93; mid[wp++]=(u8)val; break;
                    case 8: mid[wp++]=64; mid[wp++]=(u8)val; break;
                    case 9: mid[wp++]=67; mid[wp++]=(u8)val; break;
                    default: mid[wp++]=0; mid[wp++]=0; break;
                }
            }
        }

        if (last) {
            u32 delay = 0;
            while (rp < score_end) {
                u8 db = mus[rp++];
                delay = (delay << 7) | (db & 0x7F);
                if (!(db & 0x80)) break;
            }
            pending_delay += delay;
        }
    }

    WRITE_VLQ(pending_delay);
    mid[wp++] = 0xFF; mid[wp++] = 0x2F; mid[wp++] = 0x00;

    int trk_len = wp - trk_start;
    mid[trk_len_pos+0] = (trk_len >> 24) & 0xFF;
    mid[trk_len_pos+1] = (trk_len >> 16) & 0xFF;
    mid[trk_len_pos+2] = (trk_len >> 8)  & 0xFF;
    mid[trk_len_pos+3] = trk_len & 0xFF;

    #undef WRITE_VLQ
    #undef MUS2MIDI_CH

    *out_len = wp;
    return mid;
}

static void midi_play_stop(void) {
    if (!g_midi.playing && !g_midi.hmo) return;
    g_midi.playing = 0;
    if (g_midi.thread) {
        WaitForSingleObject(g_midi.thread, 3000);
        CloseHandle(g_midi.thread);
        g_midi.thread = NULL;
    }
    if (g_midi.hmo) {
        midiOutReset(g_midi.hmo);
        midiOutClose(g_midi.hmo);
        g_midi.hmo = NULL;
    }
    if (g_midi.events) {
        free(g_midi.events);
        g_midi.events = NULL;
    }
    g_midi.n_events = 0;
}

static int midi_play_start(const u8 *buf, int len) {
    if (!buf || len < 14) return 0;
    if (buf[0] != 'M' || buf[1] != 'T' || buf[2] != 'h' || buf[3] != 'd') return 0;

    int hdr_len = (buf[4]<<24)|(buf[5]<<16)|(buf[6]<<8)|buf[7];
    int format  = (buf[8]<<8)|buf[9];
    int ntracks = (buf[10]<<8)|buf[11];
    int division = (buf[12]<<8)|buf[13];
    (void)hdr_len;

    if (format > 1 || ntracks < 1 || division <= 0) return 0;

    g_midi.events = (MidiEvent*)malloc(MIDI_MAX_EVENTS * sizeof(MidiEvent));
    if (!g_midi.events) return 0;
    g_midi.n_events = 0;
    g_midi.divisions = division;
    g_midi.tempo = 500000;

    int pos = 8 + hdr_len;
    for (int t = 0; t < ntracks && pos + 8 <= len; t++) {
        if (buf[pos]!='M'||buf[pos+1]!='T'||buf[pos+2]!='r'||buf[pos+3]!='k') {
            pos += 8; continue;
        }
        int tlen = (buf[pos+4]<<24)|(buf[pos+5]<<16)|(buf[pos+6]<<8)|buf[pos+7];
        pos += 8;
        if (pos + tlen > len) tlen = len - pos;
        midi_parse_track(buf + pos, tlen);
        pos += tlen;
    }

    if (g_midi.n_events == 0) { free(g_midi.events); g_midi.events = NULL; return 0; }

    qsort(g_midi.events, g_midi.n_events, sizeof(MidiEvent), midi_event_cmp);

    if (midiOutOpen(&g_midi.hmo, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        free(g_midi.events); g_midi.events = NULL;
        return 0;
    }

    g_midi.playing = 1;
    g_midi.thread = CreateThread(NULL, 0, midi_thread_proc, NULL, 0, NULL);
    if (!g_midi.thread) {
        g_midi.playing = 0;
        midiOutClose(g_midi.hmo); g_midi.hmo = NULL;
        free(g_midi.events); g_midi.events = NULL;
        return 0;
    }
    return 1;
}

/* =========================================================================
 * Tone effects and raw PCM playback
 * ========================================================================= */

typedef struct { int f0, f1, ms; } R3DSndSeg;

static void r3d_snd_play_segs_async(const R3DSndSeg *segs, int n, int volume)
{
    int total = 0;
    for (int i = 0; i < n; i++)
        total += (SND_SAMPLE_RATE * segs[i].ms) / 1000 + 1;
    u8 *buf = (u8*)malloc(total);
    if (!buf) return;
    int pos = 0;
    for (int i = 0; i < n; i++)
        pos += r3d_snd_fill_sweep(buf + pos, total - pos,
                                   segs[i].f0, segs[i].f1, segs[i].ms, volume);
    r3d_snd_start_voice(buf, pos, 255);
    free(buf);
}

static void r3d_snd_tone_freq(RelicVM *vm) {
    int freq   = (int)(u16)vm->r[1];
    int ms     = (int)(u16)vm->r[2];
    int volume = (int)(u8) vm->r[3];
    if (freq < 20) freq = 20;
    if (ms   < 1)  ms   = 1;
    if (volume < 0)   volume = 0;
    if (volume > 255) volume = 255;
    int samples = (SND_SAMPLE_RATE * ms) / 1000 + 1;
    u8 *buf = (u8*)malloc(samples);
    if (!buf) return;
    int len = r3d_snd_fill_sweep(buf, samples, freq, freq, ms, volume);
    int scaled = (int)(255 * g_sfx_vol);
    if (scaled > 255) scaled = 255;
    r3d_snd_start_voice(buf, len, scaled);
    free(buf);
}

static void r3d_snd_tone_id(RelicVM *vm) {
    u8 id = (u8)vm->r[1];
    int volume = (int)(u8)vm->r[2];
    if (volume < 0)   volume = 0;
    if (volume > 255) volume = 255;

    switch (id) {
        case 0: { R3DSndSeg s[] = { {300,120,60},{120,60,80},{60,40,100},{40,30,120} };
                  r3d_snd_play_segs_async(s, 4, (int)(volume * g_sfx_vol)); break; }
        case 1: { R3DSndSeg s[] = { {1600,200,120} };
                  r3d_snd_play_segs_async(s, 1, (int)(volume * g_sfx_vol)); break; }
        case 2: { R3DSndSeg s[] = { {2000,1600,25},{1600,1200,20} };
                  r3d_snd_play_segs_async(s, 2, (int)(volume * g_sfx_vol)); break; }
        case 3: { R3DSndSeg s[] = { {1000,600,40},{600,400,30} };
                  r3d_snd_play_segs_async(s, 2, (int)(volume * g_sfx_vol)); break; }
        case 4: { R3DSndSeg s[] = { {500,200,50},{200,100,70} };
                  r3d_snd_play_segs_async(s, 2, (int)(volume * g_sfx_vol)); break; }
        case 5: { R3DSndSeg s[] = { {330,330,60},{440,440,60},{550,550,60},{660,660,60},{880,880,90} };
                  r3d_snd_play_segs_async(s, 5, (int)(volume * g_sfx_vol)); break; }
        case 6: { R3DSndSeg s[] = { {250,900,110} };
                  r3d_snd_play_segs_async(s, 1, (int)(volume * g_sfx_vol)); break; }
        case 7: { R3DSndSeg s[] = { {880,660,80},{660,494,80},{494,330,90},{330,220,100},{220,110,130} };
                  r3d_snd_play_segs_async(s, 5, (int)(volume * g_sfx_vol)); break; }
        case 8: { R3DSndSeg s[] = { {988,988,55},{1319,1319,110} };
                  r3d_snd_play_segs_async(s, 2, (int)(volume * g_sfx_vol)); break; }
        case 9: { R3DSndSeg s[] = { {523,523,80},{659,659,80},{784,784,80},{1047,1047,180} };
                  r3d_snd_play_segs_async(s, 4, (int)(volume * g_sfx_vol)); break; }
        case 10: { R3DSndSeg s[] = { {300,150,40},{150,80,60},{80,50,60} };
                   r3d_snd_play_segs_async(s, 3, (int)(volume * g_sfx_vol)); break; }
        case 11: { R3DSndSeg s[] = { {400,1600,100},{1600,400,100} };
                   r3d_snd_play_segs_async(s, 2, (int)(volume * g_sfx_vol)); break; }
        default: break;
    }
}

static void r3d_snd_play(RelicVM *vm) {
    u32 ptr = (u32)vm->r[1];
    u32 len = (u32)vm->r[2];
    int volume = (int)(u8)vm->r[3];
    if (ptr + len > vm->mem_sz) return;
    if (volume < 0)   volume = 0;
    if (volume > 255) volume = 255;

    int eff_vol = (int)(volume * g_sfx_vol);
    if (eff_vol > 255) eff_vol = 255;
    if (eff_vol < 255) {
        u8 *scaled = (u8*)malloc(len);
        if (!scaled) {
            r3d_snd_start_voice(vm->mem + ptr, (int)len, eff_vol);
            return;
        }
        for (u32 i = 0; i < len; i++) {
            int sample = (int)vm->mem[ptr + i];
            int offset = sample - 128;
            offset = (offset * eff_vol) / 255;
            scaled[i] = (u8)(128 + offset);
        }
        r3d_snd_start_voice(scaled, (int)len, 255);
        free(scaled);
    } else {
        r3d_snd_start_voice(vm->mem + ptr, (int)len, 255);
    }
}

static void r3d_snd_music(RelicVM *vm) {
    u32 ptr = (u32)vm->r[1];
    u32 len = (u32)vm->r[2];
    int cmd = vm->r[3];
    if (cmd == 0) {
        mus_stop();
        midi_play_stop();
        return;
    }
    if (ptr + len > vm->mem_sz) return;

    const u8 *buf = vm->mem + ptr;

    if (cmd == 3) {
        genmidi_load(buf, (int)len);
        return;
    }

    mus_stop();
    midi_play_stop();

    int is_midi = (len >= 4 && buf[0]=='M' && buf[1]=='T' && buf[2]=='h' && buf[3]=='d');
    int is_mus  = (len >= 4 && buf[0]=='M' && buf[1]=='U' && buf[2]=='S' && buf[3]==0x1A);

    g_mus_no_cymbals = 0;
    g_mus_single_voice = 0;
    g_opl_mod_depth = (4.0 * 4294967296.0);

    if (cmd == 2) {
        if (is_midi) {
            midi_play_start(buf, (int)len);
        } else if (is_mus) {
            int midi_len = 0;
            u8 *midi_buf = mus_to_midi(buf, (int)len, &midi_len);
            if (midi_buf) {
                if (!midi_play_start(midi_buf, midi_len))
                    mus_start(buf, (int)len);
                free(midi_buf);
            } else {
                mus_start(buf, (int)len);
            }
        }
        return;
    }

    if (is_mus) {
        mus_start(buf, (int)len);
    } else if (is_midi) {
        midi_play_start(buf, (int)len);
    }
}

static void r3d_snd_mixer(RelicVM *vm) {
    u8 cmd   = (u8)vm->r[1];
    u8 value = (u8)vm->r[2];
    float f  = (float)value / 255.0f;

    switch (cmd) {
        case 0: g_master_vol = f; break;
        case 1:
            EnterCriticalSection(&g_snd.lock);
            g_mus.master = f;
            LeaveCriticalSection(&g_snd.lock);
            break;
        case 2: g_sfx_vol = f; break;
        case 3:
            if (value == 0) {
                if (g_muted_master < 0.0f) {
                    g_muted_master = g_master_vol;
                    g_master_vol = 0.0f;
                }
            } else {
                if (g_muted_master >= 0.0f) {
                    g_master_vol = g_muted_master;
                    g_muted_master = -1.0f;
                }
            }
            break;
        default: break;
    }
}
