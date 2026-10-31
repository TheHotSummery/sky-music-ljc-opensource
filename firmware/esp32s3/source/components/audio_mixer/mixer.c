#include "mixer.h"

#include "audio_i2s.h"
#include "board_pins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "mixer";

#define STEAL_FADE_SAMPLES 128 /* ~5.8 ms @ 22050, fade the stolen PCM itself */
#define MASTER_Q15_MAX     32767
#define SINE_LUT_SIZE      256
#define LIMIT_REL_SHIFT    11 /* ~93 ms release @ 22050 */

typedef struct {
    bool active;
    uint8_t note_id;
    uint32_t pos;       /* PCM: Q16.16 frames */
    uint32_t step_q16;  /* PCM increment; 65536 = 1.0 */
    uint32_t gain_q15;
    uint32_t phase;
    uint32_t phase_inc;
    bool sine;
    const int16_t *pcm;
    uint32_t pcm_frames;
    int32_t hp_x;
    int32_t hp_y;
    /* Stolen voice keeps playing its remaining PCM, amplitude ramps to 0. */
    bool steal_sine;
    const int16_t *steal_pcm;
    uint32_t steal_pos; /* Q16.16 */
    uint32_t steal_frames;
    uint32_t steal_step_q16;
    uint32_t steal_gain_q15;
    uint32_t steal_phase;
    uint32_t steal_phase_inc;
    uint16_t steal_fade;
} Voice;

typedef struct {
    const int16_t *pcm;
    uint32_t frames;
} SampleSlot;

static QueueHandle_t s_cmdq;
static Voice s_voices[AUDIO_MAX_VOICES];
static SampleSlot s_bank[AUDIO_BANK_WAV_COUNT];
static int s_poly_limit = AUDIO_DEFAULT_VOICES;
static uint32_t s_master_q15 = (MASTER_Q15_MAX * MIXER_MASTER_UI_DEFAULT) / MIXER_MASTER_UI_MAX;
static uint8_t s_master_ui = MIXER_MASTER_UI_DEFAULT;
static volatile int s_transpose;
static uint32_t s_step_q16[25]; /* index = n+12, 2^(n/12) in Q16.16 */
static volatile uint64_t s_sample_count;
static volatile int s_active_voices;
static volatile uint16_t s_voice_mask;
static uint8_t s_hud_layers[AUDIO_BANK_WAV_COUNT];
static uint8_t s_hud_remain[AUDIO_BANK_WAV_COUNT];
static uint8_t s_hud_gen[AUDIO_BANK_WAV_COUNT];
static volatile uint32_t s_block_rate;
static uint32_t s_rate_blocks;
static TickType_t s_rate_tick;
static int16_t s_sine_lut[SINE_LUT_SIZE];
static int16_t *s_out;
static volatile int s_hpf_hz = 90;
static volatile int s_limit_th = 28000;
static volatile int s_pad_db = 24;
static uint32_t s_pad_q15 = 2068; /* 32767 * 10^(-24/20) */
static volatile int s_color;
static int32_t s_hp_x[2];
static int32_t s_hp_y[2];
static int32_t s_lim_env[2];
static int32_t s_acc[AUDIO_BLOCK_FRAMES * AUDIO_CHANNELS_OUT];
static MixerScope s_scope;

#define STREAM_BLOCKS 96
static int16_t *s_st;
static int16_t s_st_hold[AUDIO_BLOCK_FRAMES];
static int s_st_starved;
static uint32_t s_st_gaps;
static volatile uint32_t s_st_wr;
static volatile uint32_t s_st_rd;
static volatile int s_st_run;

static void init_sine_lut(void)
{
    for (int i = 0; i < SINE_LUT_SIZE; ++i) {
        const float t = (2.0f * 3.14159265f * (float)i) / (float)SINE_LUT_SIZE;
        s_sine_lut[i] = (int16_t)(sinf(t) * 10000.0f);
    }
}

static int32_t pcm_at(const int16_t *pcm, uint32_t frames, uint32_t pos_q16)
{
    uint32_t i = pos_q16 >> 16;
    if (i >= frames) {
        return 0;
    }
    int32_t a = pcm[i];
    if (i + 1 >= frames) {
        return a;
    }
    int32_t b = pcm[i + 1];
    uint32_t f = pos_q16 & 0xFFFFu;
    return a + (int32_t)(((int32_t)(b - a) * (int32_t)f) >> 16);
}

static uint32_t voice_remain(const Voice *v)
{
    if (!v->active) {
        return 0;
    }
    if (v->sine) {
        return UINT32_MAX;
    }
    if (v->pcm_frames <= (v->pos >> 16)) {
        return 0;
    }
    return v->pcm_frames - (v->pos >> 16);
}

static void capture_steal(Voice *v)
{
    v->steal_sine = v->sine;
    v->steal_pcm = v->pcm;
    v->steal_pos = v->pos;
    v->steal_frames = v->pcm_frames;
    v->steal_gain_q15 = v->gain_q15;
    v->steal_phase = v->phase;
    v->steal_phase_inc = v->phase_inc;
    v->steal_step_q16 = v->step_q16;
    v->steal_fade = STEAL_FADE_SAMPLES;
}

static int32_t steal_contrib(Voice *v)
{
    if (!v->steal_fade) {
        return 0;
    }
    int32_t o = 0;
    if (v->steal_sine) {
        o = s_sine_lut[v->steal_phase >> 24];
        v->steal_phase += v->steal_phase_inc;
    } else if (v->steal_pcm && (v->steal_pos >> 16) < v->steal_frames) {
        o = pcm_at(v->steal_pcm, v->steal_frames, v->steal_pos);
        v->steal_pos += v->steal_step_q16 ? v->steal_step_q16 : 65536u;
    }
    o = (o * (int32_t)v->steal_gain_q15) >> 15;
    o = (o * (int32_t)v->steal_fade) / (int32_t)STEAL_FADE_SAMPLES;
    v->steal_fade--;
    return o;
}

static Voice *find_free_voice(void)
{
    const int n = s_poly_limit;
    for (int i = 0; i < n; ++i) {
        if (!s_voices[i].active) {
            return &s_voices[i];
        }
    }
    int steal = 0;
    uint32_t best_remain = UINT32_MAX;
    for (int i = 0; i < n; ++i) {
        uint32_t remain = voice_remain(&s_voices[i]);
        if (remain < best_remain) {
            best_remain = remain;
            steal = i;
        }
    }
    return &s_voices[steal];
}

static uint32_t vel_to_gain(uint8_t velocity)
{
    if (velocity > 127) {
        velocity = 127;
    }
    return ((uint32_t)velocity * MASTER_Q15_MAX) / 127;
}

static void apply_cmd(const MixCmd *cmd)
{
    switch (cmd->type) {
    case MIX_CMD_STOP_ALL:
        memset(s_voices, 0, sizeof(s_voices));
        s_lim_env[0] = s_lim_env[1] = 0;
        mixer_stream_stop();
        break;
    case MIX_CMD_SET_MASTER: {
        uint8_t v = cmd->velocity;
        if (v > MIXER_MASTER_UI_MAX) {
            v = MIXER_MASTER_UI_MAX;
        }
        s_master_ui = v;
        s_master_q15 = ((uint32_t)v * MASTER_Q15_MAX) / MIXER_MASTER_UI_MAX;
        break;
    }
    case MIX_CMD_SET_SAMPLE:
        if (cmd->note_id < AUDIO_BANK_WAV_COUNT) {
            s_bank[cmd->note_id].pcm = cmd->pcm;
            s_bank[cmd->note_id].frames = cmd->frames;
        }
        break;
    case MIX_CMD_NOTE_OFF:
        /* One-shot piano: release does not cut the sample. */
        break;
    case MIX_CMD_NOTE_ON:
    case MIX_CMD_TEST_SINE: {
        Voice *v = find_free_voice();
        if (v->active) {
            capture_steal(v);
        } else {
            v->steal_fade = 0;
            v->steal_pcm = NULL;
            v->steal_sine = false;
        }
        v->active = true;
        v->note_id = cmd->note_id;
        v->pos = 0;
        v->step_q16 = s_step_q16[s_transpose + 12];
        v->gain_q15 = vel_to_gain(cmd->velocity ? cmd->velocity : 127);
        v->phase = 0;
        v->hp_x = 0;
        v->hp_y = 0;
        const float freq = 220.0f * powf(2.0f, (float)(cmd->note_id + s_transpose) / 12.0f);
        v->phase_inc = (uint32_t)((freq * 4294967296.0) / (double)AUDIO_SAMPLE_RATE);
        const bool want_sine = (cmd->type == MIX_CMD_TEST_SINE);
        if (!want_sine && cmd->note_id < AUDIO_BANK_WAV_COUNT && s_bank[cmd->note_id].pcm) {
            v->sine = false;
            v->pcm = s_bank[cmd->note_id].pcm;
            v->pcm_frames = s_bank[cmd->note_id].frames;
        } else {
            v->sine = true;
            v->pcm = NULL;
            v->pcm_frames = 0;
        }
        if (cmd->note_id < AUDIO_BANK_WAV_COUNT) {
            s_hud_gen[cmd->note_id]++;
        }
        break;
    }
    default:
        break;
    }
}

static int16_t sat16(int32_t x)
{
    if (x > 32767) {
        return 32767;
    }
    if (x < -32768) {
        return -32768;
    }
    return (int16_t)x;
}

/* Peak envelope limiter: instant attack, slow release. Compresses chords
 * toward `lim` without hard-clipping; single notes below lim pass through. */
static int32_t apply_limit(int32_t x, int ch, int lim)
{
    if (lim <= 0) {
        s_lim_env[ch] = 0;
        return x;
    }
    int32_t a = x < 0 ? -x : x;
    if (a > s_lim_env[ch]) {
        s_lim_env[ch] = a;
    } else {
        s_lim_env[ch] -= (s_lim_env[ch] - a) >> LIMIT_REL_SHIFT;
    }
    int32_t env = s_lim_env[ch];
    if (env > lim) {
        x = (int32_t)((int64_t)x * (int64_t)lim / env);
    }
    return x;
}

static void mix_block(int16_t *out)
{
    memset(s_acc, 0, sizeof(s_acc));

    const int nvoice = s_poly_limit;
    for (int vi = 0; vi < nvoice; ++vi) {
        Voice *v = &s_voices[vi];
        if (!v->active) {
            continue;
        }
        for (int n = 0; n < AUDIO_BLOCK_FRAMES; ++n) {
            int32_t s = 0;
            bool ended = false;
            if (v->sine) {
                const uint32_t idx = v->phase >> 24;
                s = s_sine_lut[idx];
                v->phase += v->phase_inc;
            } else if (v->pcm && (v->pos >> 16) < v->pcm_frames) {
                s = pcm_at(v->pcm, v->pcm_frames, v->pos);
                v->pos += v->step_q16 ? v->step_q16 : 65536u;
            } else {
                ended = true;
            }

            if (ended && v->steal_fade == 0) {
                v->active = false;
                break;
            }

            if (!ended && s_color && !v->sine) {
                int32_t y = (s - v->hp_x) + ((v->hp_y * 27) >> 5);
                v->hp_x = s;
                v->hp_y = y;
                s = y + (s >> 2);
            } else if (ended) {
                s = 0;
            }

            s = (s * (int32_t)v->gain_q15) >> 15;
            s += steal_contrib(v);

            const int idx = n * 2;
            s_acc[idx] += s;
            s_acc[idx + 1] += s;
        }
    }

    if (s_st_run) {
        uint32_t wr = s_st_wr;
        uint32_t rd = s_st_rd;
        const int16_t *blk;
        if (wr != rd && s_st) {
            blk = s_st + ((rd % STREAM_BLOCKS) * AUDIO_BLOCK_FRAMES);
            memcpy(s_st_hold, blk, sizeof(s_st_hold));
            s_st_rd = rd + 1;
            s_st_starved = 0;
        } else {
            /* Repeating the last block sounds like a buzz. Drop to silence. */
            if (!s_st_starved) {
                s_st_starved = 1;
                s_st_gaps++;
                ESP_LOGW(TAG, "stream underrun n=%lu", (unsigned long)s_st_gaps);
            }
            memset(s_st_hold, 0, sizeof(s_st_hold));
            blk = s_st_hold;
        }
        for (int n = 0; n < AUDIO_BLOCK_FRAMES; ++n) {
            int32_t s = blk[n];
            const int idx = n * 2;
            s_acc[idx] += s;
            s_acc[idx + 1] += s;
        }
    }

    const int hz = s_hpf_hz;
    const int lim = s_limit_th;
    int32_t alpha = 32768;
    if (hz > 0) {
        int32_t sub = (9327L * (int32_t)hz) / 1000;
        if (sub < 8) {
            sub = 8;
        }
        if (sub > 30000) {
            sub = 30000;
        }
        alpha = 32768 - sub;
    } else {
        s_hp_x[0] = s_hp_x[1] = s_hp_y[0] = s_hp_y[1] = 0;
    }

    MixerScope tap;
    memset(&tap, 0, sizeof(tap));
    uint32_t abs_acc = 0;
    for (int n = 0; n < AUDIO_BLOCK_FRAMES; ++n) {
        int32_t a = s_acc[n * 2];
        if (a < 0) {
            a = -a;
        }
        if (a > tap.peak_bus) {
            tap.peak_bus = a;
        }
        if (a > 32767) {
            tap.clip_bus++;
        }
        abs_acc += (uint32_t)a;
    }
    tap.mean_abs = abs_acc / (uint32_t)AUDIO_BLOCK_FRAMES;

    for (int n = 0; n < AUDIO_BLOCK_FRAMES; ++n) {
        for (int ch = 0; ch < 2; ++ch) {
            const int i = n * 2 + ch;
            int32_t x = s_acc[i];
            if (hz > 0) {
                int32_t y = (alpha * (s_hp_y[ch] + x - s_hp_x[ch])) >> 15;
                s_hp_x[ch] = x;
                s_hp_y[ch] = y;
                x = y;
            }
            if (ch == 0) {
                int32_t a = x < 0 ? -x : x;
                if (a > tap.peak_hpf) {
                    tap.peak_hpf = a;
                }
            }
            int32_t before = s_lim_env[ch];
            x = apply_limit(x, ch, lim);
            if (ch == 0) {
                int32_t a = x < 0 ? -x : x;
                if (a > tap.peak_lim) {
                    tap.peak_lim = a;
                }
                if (lim > 0 && before > lim) {
                    tap.limited = 1;
                }
            }
            x = (x * (int32_t)s_master_q15) >> 15;
            x = (x * (int32_t)s_pad_q15) >> 15;
            if (ch == 0) {
                int32_t a = x < 0 ? -x : x;
                if (a > tap.peak_out) {
                    tap.peak_out = a;
                }
                if (a > 32767) {
                    tap.sat_out++;
                }
            }
            out[i] = sat16(x);
        }
    }

    const int grp = AUDIO_BLOCK_FRAMES / MIX_SCOPE_PTS;
    for (int p = 0; p < MIX_SCOPE_PTS; ++p) {
        int32_t best_o = 0, best_oa = 0;
        int32_t best_b = 0, best_ba = 0;
        for (int k = 0; k < grp; ++k) {
            int32_t s = out[(p * grp + k) * 2];
            int32_t a = s < 0 ? -s : s;
            if (a >= best_oa) {
                best_oa = a;
                best_o = s;
            }
            int32_t b = s_acc[(p * grp + k) * 2];
            int32_t ba = b < 0 ? -b : b;
            if (ba >= best_ba) {
                best_ba = ba;
                best_b = b;
            }
        }
        tap.wave[p] = best_o;
        tap.wave_bus[p] = sat16(best_b);
    }
    {
        uint8_t live = 0;
        for (int i = 0; i < s_poly_limit; ++i) {
            if (s_voices[i].active) {
                live++;
            }
        }
        tap.voices = live;
    }
    tap.limited = (uint8_t)((lim > 0 && tap.peak_hpf > lim) ? 1 : 0);
    tap.master = s_master_ui;
    s_scope = tap;
}

static void mixer_task(void *arg)
{
    (void)arg;
    MixCmd cmd;
    for (;;) {
        while (xQueueReceive(s_cmdq, &cmd, 0) == pdTRUE) {
            apply_cmd(&cmd);
        }
        mix_block(s_out);
        int live = 0;
        uint16_t mask = 0;
        for (int i = 0; i < AUDIO_MAX_VOICES; ++i) {
            if (s_voices[i].active) {
                live++;
                if (s_voices[i].note_id < 16) {
                    mask |= (uint16_t)(1u << s_voices[i].note_id);
                }
            }
        }
        s_active_voices = live;
        s_voice_mask = mask;

        uint8_t layers[AUDIO_BANK_WAV_COUNT] = {0};
        uint8_t remain[AUDIO_BANK_WAV_COUNT] = {0};
        for (int i = 0; i < AUDIO_MAX_VOICES; ++i) {
            if (!s_voices[i].active || s_voices[i].note_id >= AUDIO_BANK_WAV_COUNT) {
                continue;
            }
            const uint8_t nid = s_voices[i].note_id;
            layers[nid]++;
            uint8_t pct = 0;
            if (s_voices[i].pcm_frames > 0 && (s_voices[i].pos >> 16) < s_voices[i].pcm_frames) {
                uint32_t left = s_voices[i].pcm_frames - (s_voices[i].pos >> 16);
                pct = (uint8_t)((left * 100u) / s_voices[i].pcm_frames);
            }
            if (pct > remain[nid]) {
                remain[nid] = pct;
            }
        }
        memcpy(s_hud_layers, layers, sizeof(layers));
        memcpy(s_hud_remain, remain, sizeof(remain));

        s_rate_blocks++;
        TickType_t now = xTaskGetTickCount();
        TickType_t dt = now - s_rate_tick;
        if (dt >= pdMS_TO_TICKS(500)) {
            s_block_rate = (s_rate_blocks * 1000u) / ((uint32_t)dt * portTICK_PERIOD_MS);
            s_rate_blocks = 0;
            s_rate_tick = now;
        }

        if (audio_i2s_write(s_out, AUDIO_BLOCK_FRAMES) != ESP_OK) {
            ESP_LOGE(TAG, "i2s write failed");
        }
        s_sample_count += AUDIO_BLOCK_FRAMES;
    }
}

bool mixer_submit(const MixCmd *cmd)
{
    if (!s_cmdq || !cmd) {
        return false;
    }
    /* Master must be readable immediately by UI / sky_proto snapshot (queue alone races). */
    if (cmd->type == MIX_CMD_SET_MASTER) {
        uint8_t v = cmd->velocity;
        if (v > MIXER_MASTER_UI_MAX) {
            v = MIXER_MASTER_UI_MAX;
        }
        s_master_ui = v;
        s_master_q15 = ((uint32_t)v * MASTER_Q15_MAX) / MIXER_MASTER_UI_MAX;
    }
    return xQueueSend(s_cmdq, cmd, 0) == pdTRUE;
}

uint64_t mixer_sample_count(void)
{
    return s_sample_count;
}

int mixer_active_voices(void)
{
    return s_active_voices;
}

int mixer_polyphony(void)
{
    return s_poly_limit;
}

int mixer_voice_cap(void)
{
    return AUDIO_MAX_VOICES;
}

void mixer_set_polyphony(int n)
{
    if (n < 1) {
        n = 1;
    }
    if (n > AUDIO_MAX_VOICES) {
        n = AUDIO_MAX_VOICES;
    }
    s_poly_limit = n;
    for (int i = n; i < AUDIO_MAX_VOICES; ++i) {
        s_voices[i].active = false;
        s_voices[i].steal_fade = 0;
    }
}

uint16_t mixer_voice_mask(void)
{
    return s_voice_mask;
}

uint8_t mixer_master_ui(void)
{
    return s_master_ui;
}

void mixer_set_transpose(int semitones)
{
    if (semitones < -12) {
        semitones = -12;
    }
    if (semitones > 12) {
        semitones = 12;
    }
    s_transpose = semitones;
}

int mixer_transpose(void)
{
    return s_transpose;
}

const char *mixer_transpose_name(void)
{
    static const char *nm[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static char buf[8];
    int n = s_transpose;
    int pc = ((n % 12) + 12) % 12;
    if (n == 0) {
        return "C";
    }
    if (n > 0 && n < 12) {
        return nm[pc];
    }
    if (n == 12) {
        return "C^";
    }
    if (n == -12) {
        return "Cv";
    }
    if (n < 0) {
        snprintf(buf, sizeof(buf), "-%s", nm[pc]);
        return buf;
    }
    snprintf(buf, sizeof(buf), "+%s", nm[pc]);
    return buf;
}

void mixer_hud_keys(uint8_t layers[AUDIO_BANK_WAV_COUNT], uint8_t remain_pct[AUDIO_BANK_WAV_COUNT])
{
    if (layers) {
        memcpy(layers, s_hud_layers, AUDIO_BANK_WAV_COUNT);
    }
    if (remain_pct) {
        memcpy(remain_pct, s_hud_remain, AUDIO_BANK_WAV_COUNT);
    }
}

void mixer_hud_gens(uint8_t gen[AUDIO_BANK_WAV_COUNT])
{
    if (gen) {
        memcpy(gen, s_hud_gen, AUDIO_BANK_WAV_COUNT);
    }
}

uint32_t mixer_block_rate(void)
{
    return s_block_rate;
}

void mixer_set_sample(uint8_t note_id, const int16_t *pcm, uint32_t frames)
{
    MixCmd cmd = {
        .type = MIX_CMD_SET_SAMPLE,
        .note_id = note_id,
        .pcm = pcm,
        .frames = frames,
    };
    mixer_submit(&cmd);
}

bool mixer_has_sample(uint8_t note_id)
{
    return note_id < AUDIO_BANK_WAV_COUNT && s_bank[note_id].pcm != NULL;
}

void mixer_set_hpf_hz(int hz)
{
    if (hz < 0) {
        hz = 0;
    }
    if (hz > 400) {
        hz = 400;
    }
    s_hpf_hz = hz;
}

int mixer_hpf_hz(void)
{
    return s_hpf_hz;
}

void mixer_set_limit(int thresh)
{
    if (thresh < 0) {
        thresh = 0;
    }
    if (thresh > 32767) {
        thresh = 32767;
    }
    s_limit_th = thresh;
}

int mixer_limit(void)
{
    return s_limit_th;
}

void mixer_set_pad_db(int db)
{
    if (db < 0) {
        db = 0;
    }
    if (db > 36) {
        db = 36;
    }
    s_pad_db = db;
    uint32_t q = (uint32_t)(32767.0f * powf(10.0f, -(float)db / 20.0f) + 0.5f);
    if (q < 1) {
        q = 1;
    }
    s_pad_q15 = q;
}

int mixer_pad_db(void)
{
    return s_pad_db;
}

void mixer_scope_get(MixerScope *out)
{
    if (out) {
        *out = s_scope;
    }
}

void mixer_set_color(int mode)
{
    s_color = mode ? 1 : 0;
}

int mixer_color(void)
{
    return s_color;
}

void mixer_stream_stop(void)
{
    s_st_run = 0;
    s_st_wr = 0;
    s_st_rd = 0;
    memset(s_st_hold, 0, sizeof(s_st_hold));
}

void mixer_stream_start(void)
{
    mixer_stream_stop();
}

void mixer_stream_go(void)
{
    s_st_run = 1;
}

bool mixer_stream_push(const int16_t *mono, int frames)
{
    if (!s_st || !mono || frames != AUDIO_BLOCK_FRAMES) {
        return false;
    }
    uint32_t wr = s_st_wr;
    uint32_t rd = s_st_rd;
    if ((wr - rd) >= STREAM_BLOCKS) {
        return false;
    }
    memcpy(s_st + ((wr % STREAM_BLOCKS) * AUDIO_BLOCK_FRAMES), mono,
           (size_t)frames * sizeof(int16_t));
    s_st_wr = wr + 1;
    return true;
}

int mixer_stream_space(void)
{
    uint32_t used = s_st_wr - s_st_rd;
    if (used >= STREAM_BLOCKS) {
        return 0;
    }
    return (int)(STREAM_BLOCKS - used);
}

int mixer_stream_used(void)
{
    uint32_t used = s_st_wr - s_st_rd;
    return used > STREAM_BLOCKS ? (int)STREAM_BLOCKS : (int)used;
}

esp_err_t mixer_start(void)
{
    init_sine_lut();
    memset(s_voices, 0, sizeof(s_voices));
    for (int i = 0; i <= 24; i++) {
        float r = powf(2.0f, (float)(i - 12) / 12.0f);
        s_step_q16[i] = (uint32_t)(r * 65536.0f + 0.5f);
    }
    s_step_q16[12] = 65536;

    s_out = heap_caps_calloc(AUDIO_BLOCK_FRAMES * AUDIO_CHANNELS_OUT, sizeof(int16_t),
                             MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_out) {
        return ESP_ERR_NO_MEM;
    }
    s_st = heap_caps_calloc(STREAM_BLOCKS * AUDIO_BLOCK_FRAMES, sizeof(int16_t),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_st) {
        return ESP_ERR_NO_MEM;
    }

    s_cmdq = xQueueCreate(128, sizeof(MixCmd));
    if (!s_cmdq) {
        return ESP_ERR_NO_MEM;
    }
    s_rate_tick = xTaskGetTickCount();

    /* Core 1, high priority: never blocks on mutex, only I2S DMA. */
    BaseType_t ok = xTaskCreatePinnedToCore(mixer_task, "mixer", 8192, NULL, 10, NULL, 1);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "mixer %d voices @ %d Hz, hpf=%d limit=%d (bus then master)", AUDIO_DEFAULT_VOICES,
             AUDIO_SAMPLE_RATE, s_hpf_hz, s_limit_th);
    return ESP_OK;
}
