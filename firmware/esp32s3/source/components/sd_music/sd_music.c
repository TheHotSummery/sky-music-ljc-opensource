#include "sd_music.h"

#include "board_pins.h"
#include "mixer.h"
#include "nk_out.h"

#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MUSIC_DIR SD_MOUNT_POINT "/music"
#define MUSIC_PATH_MAX 384
#define DEC_IN_CHUNK 12288
#define MQ_CAP 24576 /* ~0.5s @44.1k mono; WAV/FLAC bursts must not drop */

static const char *TAG = "music";

static volatile int s_stop;
static volatile int s_pause;
static volatile int s_ended;
static volatile int s_busy;
static char s_path[MUSIC_PATH_MAX];
static char s_pending[MUSIC_PATH_MAX]; /* next play request (survives stop race) */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static volatile uint32_t s_bytes_done;
static volatile uint32_t s_bytes_total;
static volatile uint32_t s_pcm_out; /* frames pushed to mixer @ AUDIO_SAMPLE_RATE */
static volatile uint32_t s_src_rate;
static volatile uint32_t s_dur_ms; /* file duration, probed once at open */
static volatile int s_tempo_pct = 100;
static int s_codec_ready;
static int s_yield_n;
static volatile int s_fin_eof; /* 1 = last play_decoded reached file EOF */
static int (*s_next_provider)(char *out, size_t out_sz);

static int clamp_tempo(int pct)
{
    if (pct < 50) {
        return 50;
    }
    if (pct > 200) {
        return 200;
    }
    return pct;
}

static void *audio_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    }
    return p;
}

static void *audio_realloc(void *old, size_t n)
{
    void *p = heap_caps_realloc(old, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_realloc(old, n, MALLOC_CAP_8BIT);
    }
    return p;
}

static const char *ext_of(const char *n)
{
    const char *d = strrchr(n, '.');
    return d ? d + 1 : "";
}

/* 板上直接播：WAV/MP3/FLAC（+裸 AAC）。M4A/OGG 容器留给 App 转码。 */
static esp_audio_simple_dec_type_t type_from_name(const char *n)
{
    const char *e = ext_of(n);
    if (strcasecmp(e, "wav") == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    }
    if (strcasecmp(e, "mp3") == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (strcasecmp(e, "flac") == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    }
    if (strcasecmp(e, "aac") == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

static int is_music_name(const char *n)
{
    return type_from_name(n) != ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

static void list_music(void)
{
    nk_printf("BEGIN_MUSIC\n");
    DIR *dir = opendir(MUSIC_DIR);
    int n = 0;
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') {
                continue;
            }
            if (!is_music_name(ent->d_name)) {
                continue;
            }
            nk_printf("file=%s/%s bytes=0\n", MUSIC_DIR, ent->d_name);
            n++;
        }
        closedir(dir);
    }
    nk_printf("shown=%d total=%d\n", n, n);
    nk_printf("END_MUSIC\n");
}

static int sample_s16(const uint8_t *p, int bytes)
{
    if (bytes == 1) {
        return ((int)p[0] - 128) << 8;
    }
    if (bytes == 3) {
        int32_t v = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
        if (v & 0x800000) {
            v |= ~0xFFFFFF;
        }
        return (int16_t)(v >> 8);
    }
    if (bytes == 4) {
        int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                              ((uint32_t)p[3] << 24));
        return (int16_t)(v >> 16);
    }
    return (int16_t)(p[0] | (p[1] << 8));
}

/* Decode PCM → mono int16 frames (mix L/R). Returns frame count. */
static int pcm_to_mono16(const uint8_t *in, int nbytes, int ch, int bits, int16_t *out, int max_frames)
{
    int bytes = bits / 8;
    if (bytes < 1) {
        bytes = 2;
    }
    if (ch < 1) {
        ch = 1;
    }
    int frame_bytes = bytes * ch;
    if (frame_bytes <= 0) {
        return 0;
    }
    int frames = nbytes / frame_bytes;
    if (frames > max_frames) {
        frames = max_frames;
    }
    for (int i = 0; i < frames; ++i) {
        const uint8_t *f = in + i * frame_bytes;
        int32_t acc = 0;
        int use = ch > 2 ? 2 : ch;
        for (int c = 0; c < use; ++c) {
            acc += sample_s16(f + c * bytes, bytes);
        }
        if (use > 1) {
            acc /= use;
        }
        out[i] = (int16_t)acc;
    }
    return frames;
}

typedef struct {
    int16_t *ring;
    int cap;
    int r;
    int w;
    int n;
    uint32_t pos_q16; /* source position fraction for linear resample */
    uint32_t step_q16;
    uint32_t src_rate;
    int16_t hold;
    int16_t hold_next;
    int have_pair;
} mono_q_t;

static void mq_set_rate(mono_q_t *q, uint32_t src_rate, uint32_t out_rate)
{
    q->src_rate = src_rate ? src_rate : out_rate;
    uint32_t eff = (uint32_t)((uint64_t)q->src_rate * (uint32_t)clamp_tempo(s_tempo_pct) / 100u);
    if (eff < 4000) {
        eff = 4000;
    }
    if (out_rate < 1) {
        out_rate = 1;
    }
    q->step_q16 = (uint32_t)(((uint64_t)eff << 16) / out_rate);
    if (q->step_q16 < 1) {
        q->step_q16 = 1;
    }
}

static int mq_push(mono_q_t *q, const int16_t *src, int n)
{
    for (int i = 0; i < n; ++i) {
        if (q->n >= q->cap) {
            return i;
        }
        q->ring[q->w] = src[i];
        q->w = (q->w + 1) % q->cap;
        q->n++;
    }
    return n;
}

static int mq_take(mono_q_t *q, int16_t *out)
{
    if (q->n <= 0) {
        return 0;
    }
    *out = q->ring[q->r];
    q->r = (q->r + 1) % q->cap;
    q->n--;
    return 1;
}

static int mq_src_needed(const mono_q_t *q, int out_frames)
{
    /* ceil(out * step / 65536) + 2 headroom for linear pair */
    uint64_t need = ((uint64_t)out_frames * (uint64_t)q->step_q16 + 0xFFFFu) >> 16;
    return (int)need + 2;
}

static int mq_avail(const mono_q_t *q)
{
    return q->n + (q->have_pair ? 2 : 0);
}

/* Linear resample. Returns 0=full block ready. -1=need more (blk untouched). */
static int mq_pop_resampled(mono_q_t *q, int16_t *blk, int frames, int flush)
{
    if (q->src_rate) {
        mq_set_rate(q, q->src_rate, AUDIO_SAMPLE_RATE); /* pick up live tempo */
    }
    if (!flush && mq_avail(q) < mq_src_needed(q, frames)) {
        return -1;
    }
    if (!q->have_pair) {
        if (!mq_take(q, &q->hold)) {
            return -1;
        }
        if (!mq_take(q, &q->hold_next)) {
            q->hold_next = q->hold;
        }
        q->have_pair = 1;
    }
    for (int i = 0; i < frames; ++i) {
        uint32_t frac = q->pos_q16 & 0xFFFFu;
        int32_t a = q->hold;
        int32_t b = q->hold_next;
        blk[i] = (int16_t)(a + (((b - a) * (int32_t)frac) >> 16));
        q->pos_q16 += q->step_q16;
        while (q->pos_q16 >= (1u << 16)) {
            q->pos_q16 -= (1u << 16);
            q->hold = q->hold_next;
            if (!mq_take(q, &q->hold_next)) {
                q->hold_next = q->hold;
                if (!flush) {
                    /* Shouldn't happen if mq_src_needed was honored; pad rest. */
                    for (int j = i + 1; j < frames; ++j) {
                        blk[j] = q->hold;
                    }
                    return 0;
                }
            }
        }
    }
    return 0;
}

static void music_yield(void)
{
    /* Let mixer / other core1 tasks breathe during FLAC bursts. RGB/UI live on core0. */
    if ((++s_yield_n & 7) == 0) {
        if (mixer_stream_used() >= 16) {
            vTaskDelay(1); /* buffer healthy — real sleep so we don't spin the core */
        } else {
            taskYIELD();
        }
    }
}

/* esp_audio_simple_dec has no "finished" out-flag — only input raw.eos.
 * We finish when: file EOF + decoder idle, OR played past probed duration. */
static int past_duration(void)
{
    if (s_dur_ms == 0 || s_pcm_out == 0) {
        return 0;
    }
    uint32_t now_ms = (uint32_t)(((uint64_t)s_pcm_out * 1000u) / (uint32_t)AUDIO_SAMPLE_RATE);
    return now_ms >= s_dur_ms + 80u;
}

static int drain_mq(mono_q_t *mq, int16_t *blk, int *primed, int flush)
{
    int did = 0;
    int nblk = 0;
    while (!s_stop && !s_pause) {
        if (past_duration()) {
            s_fin_eof = 1;
            mq->n = 0; /* drop rest — do not keep feeding past end */
            mq->r = mq->w = 0;
            mq->have_pair = 0;
            return did ? did : 1;
        }
        int space = mixer_stream_space();
        if (*primed && space < 2 && !flush) {
            break;
        }
        int enough = mq_avail(mq) >= mq_src_needed(mq, AUDIO_BLOCK_FRAMES);
        if (!enough && !(flush && mq_avail(mq) > 0)) {
            break;
        }
        if (mq_pop_resampled(mq, blk, AUDIO_BLOCK_FRAMES, flush && !enough) != 0) {
            break;
        }
        while (!s_stop && !s_pause && !mixer_stream_push(blk, AUDIO_BLOCK_FRAMES)) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (s_stop) {
            break;
        }
        s_pcm_out += AUDIO_BLOCK_FRAMES;
        did = 1;
        if (!*primed && mixer_stream_used() >= 24) {
            mixer_stream_go();
            *primed = 1;
        }
        if (flush && mq_avail(mq) == 0) {
            break;
        }
        /* Don't monopolize CPU while priming a large FLAC buffer. */
        if (++nblk >= 8) {
            music_yield();
            nblk = 0;
            if (!flush && *primed && mixer_stream_space() < 4) {
                break;
            }
        }
    }
    return did;
}

/* Push every decoded mono frame — never drop (old bug: half-full discard → 失真/变速). */
static int mq_push_all(mono_q_t *mq, const int16_t *src, int n, int16_t *blk, int *primed)
{
    int off = 0;
    while (off < n && !s_stop && !s_fin_eof) {
        while (s_pause && !s_stop) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (past_duration()) {
            s_fin_eof = 1;
            break;
        }
        int pushed = mq_push(mq, src + off, n - off);
        if (pushed > 0) {
            off += pushed;
        }
        if (off >= n) {
            break;
        }
        /* Queue full: drain to mixer, then retry. */
        if (!drain_mq(mq, blk, primed, 0)) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    return off;
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Header probe once per file. get_progress must not re-estimate (VBR ratio drifts). */
static uint32_t dur_wav(FILE *fp, uint32_t fsz)
{
    uint8_t b[16];
    uint32_t rate = 0, data = 0;
    int ch = 0, bits = 0;
    if (fseek(fp, 12, SEEK_SET) != 0) {
        return 0;
    }
    for (int n = 0; n < 32; ++n) {
        if (fread(b, 1, 8, fp) != 8) {
            break;
        }
        uint32_t sz = rd_le32(b + 4);
        if (memcmp(b, "fmt ", 4) == 0 && sz >= 16 && fread(b, 1, 16, fp) == 16) {
            ch = b[2] | (b[3] << 8);
            rate = rd_le32(b + 4);
            bits = b[14] | (b[15] << 8);
            if (sz > 16) {
                fseek(fp, (long)((sz - 16) + (sz & 1)), SEEK_CUR);
            }
        } else if (memcmp(b, "data", 4) == 0) {
            data = sz;
            if (data == 0 || data > fsz) {
                long at = ftell(fp);
                if (at > 0 && (uint32_t)at < fsz) {
                    data = fsz - (uint32_t)at;
                }
            }
            break;
        } else if (fseek(fp, (long)(sz + (sz & 1)), SEEK_CUR) != 0) {
            break;
        }
    }
    if (!rate || ch < 1 || bits < 8 || !data) {
        return 0;
    }
    uint32_t bpf = (uint32_t)ch * (uint32_t)(bits >> 3);
    if (!bpf) {
        return 0;
    }
    return (uint32_t)(((uint64_t)data * 1000ull) / ((uint64_t)rate * bpf));
}

static uint32_t dur_mp3(FILE *fp, uint32_t fsz)
{
    uint8_t h[10];
    uint32_t off = 0;
    if (fread(h, 1, 10, fp) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3') {
        off = 10u + (((uint32_t)h[6] & 0x7f) << 21) + (((uint32_t)h[7] & 0x7f) << 14) +
              (((uint32_t)h[8] & 0x7f) << 7) + ((uint32_t)h[9] & 0x7f);
    }
    if (fseek(fp, (long)off, SEEK_SET) != 0) {
        return 0;
    }
    uint32_t dur = 0;
    for (int i = 0; i < 4096; ++i) {
        int c = fgetc(fp);
        if (c != 0xFF) {
            if (c < 0) {
                break;
            }
            continue;
        }
        uint8_t fr[3];
        if (fread(fr, 1, 3, fp) != 3 || (fr[0] & 0xE0) != 0xE0) {
            if (fseek(fp, -3, SEEK_CUR) != 0) {
                break;
            }
            continue;
        }
        int ver = (fr[0] >> 3) & 3;
        int layer = (fr[0] >> 1) & 3;
        int bri = (fr[1] >> 4) & 15;
        int sri = (fr[1] >> 2) & 3;
        int kbps = 0, sr = 0, spf = 1152;
        if (ver == 3 && layer == 1) {
            static const int br[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
            static const int srs[4] = {44100, 48000, 32000, 0};
            kbps = br[bri];
            sr = srs[sri];
        } else if ((ver == 2 || ver == 0) && layer == 1) {
            static const int br[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
            static const int srs[4] = {22050, 24000, 16000, 0};
            static const int srs25[4] = {11025, 12000, 8000, 0};
            kbps = br[bri];
            sr = ver == 2 ? srs[sri] : srs25[sri];
            spf = 576;
        } else {
            continue;
        }
        if (!kbps || !sr) {
            continue;
        }
        int mono = ((fr[2] >> 6) & 3) == 3;
        int xoff = (ver == 3) ? (mono ? 21 : 36) : (mono ? 13 : 21);
        long frame = ftell(fp) - 4;
        uint8_t tag[12];
        if (frame > 0 && fseek(fp, frame + xoff, SEEK_SET) == 0 && fread(tag, 1, 12, fp) == 12 &&
            ((tag[0] == 'X' && tag[1] == 'i' && tag[2] == 'n' && tag[3] == 'g') ||
             (tag[0] == 'I' && tag[1] == 'n' && tag[2] == 'f' && tag[3] == 'o')) &&
            (rd_be32(tag + 4) & 1u)) {
            uint32_t frames = rd_be32(tag + 8);
            if (frames > 0 && frames < 10000000u) {
                dur = (uint32_t)(((uint64_t)frames * (uint32_t)spf * 1000u) / (uint32_t)sr);
            }
        }
        if (!dur) {
            uint32_t audio = fsz > off ? fsz - off : fsz;
            dur = (uint32_t)(((uint64_t)audio * 8u) / (uint32_t)kbps);
        }
        break;
    }
    return dur;
}

static uint32_t dur_flac(FILE *fp)
{
    uint8_t b[26];
    if (fread(b, 1, sizeof(b), fp) != sizeof(b) || memcmp(b, "fLaC", 4) != 0 || (b[4] & 0x7f) != 0) {
        return 0;
    }
    /* STREAMINFO @8, rate/samples packed at +10 (8 bytes). */
    uint64_t v = 0;
    for (int i = 18; i < 26; ++i) {
        v = (v << 8) | b[i];
    }
    uint32_t rate = (uint32_t)(v >> 44);
    uint64_t samples = v & ((1ull << 36) - 1);
    if (!rate || !samples) {
        return 0;
    }
    return (uint32_t)((samples * 1000ull) / rate);
}

static uint32_t probe_duration(FILE *fp, esp_audio_simple_dec_type_t type, uint32_t fsz)
{
    uint32_t d = 0;
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_WAV) {
        d = dur_wav(fp, fsz);
    } else if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_MP3) {
        d = dur_mp3(fp, fsz);
    } else if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC) {
        d = dur_flac(fp);
    }
    fseek(fp, 0, SEEK_SET);
    return d;
}

static int play_decoded(const char *path)
{
    esp_audio_simple_dec_type_t type = type_from_name(path);
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        nk_printf("music unsupported fmt %s\n", path);
        return -1;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        nk_printf("music open fail %s\n", path);
        return -1;
    }
    struct stat st;
    s_bytes_total = 0;
    if (fstat(fileno(fp), &st) == 0 && st.st_size > 0) {
        s_bytes_total = (uint32_t)st.st_size;
    }
    s_bytes_done = 0;
    s_pcm_out = 0;
    s_src_rate = 0;
    s_fin_eof = 0;
    s_dur_ms = probe_duration(fp, type, s_bytes_total);
    nk_printf("music dur=%u ms\n", (unsigned)s_dur_ms);

    uint8_t extra[64];
    memset(extra, 0, sizeof(extra));
    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = type,
        .dec_cfg = extra,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC) {
        extra[0] = 1; /* AAC with ASC / ADTS */
        cfg.cfg_size = 8;
    }

    esp_audio_simple_dec_handle_t dec = NULL;
    esp_audio_err_t er = esp_audio_simple_dec_open(&cfg, &dec);
    if (er != ESP_AUDIO_ERR_OK) {
        nk_printf("music dec_open %d\n", (int)er);
        fclose(fp);
        return -1;
    }

    uint8_t *in_buf = audio_alloc(DEC_IN_CHUNK);
    int out_cap = 16384;
    uint8_t *out_buf = audio_alloc((size_t)out_cap);
    int mono_cap = 8192;
    int16_t *mono_tmp = audio_alloc((size_t)mono_cap * sizeof(int16_t));
    mono_q_t mq = {0};
    mq.cap = MQ_CAP;
    mq.ring = audio_alloc((size_t)mq.cap * sizeof(int16_t));
    if (!in_buf || !out_buf || !mono_tmp || !mq.ring) {
        nk_printf("music no buf\n");
        free(in_buf);
        free(out_buf);
        free(mono_tmp);
        free(mq.ring);
        esp_audio_simple_dec_close(dec);
        fclose(fp);
        return -1;
    }

    mixer_stream_start();
    int16_t blk[AUDIO_BLOCK_FRAMES];
    int primed = 0;
    int opened_info = 0;
    int eof = 0;
    uint32_t in_len = 0;
    int ch = 2;
    int bits = 16;
    int eof_stall = 0;

    while (!s_stop) {
        while (s_pause && !s_stop) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (s_stop) {
            break;
        }

        /* Prefer keeping mixer fed when we already have PCM. */
        drain_mq(&mq, blk, &primed, 0);

        if (s_fin_eof || past_duration()) {
            s_fin_eof = 1;
            nk_printf("music dur-gate pcm=%lu dur=%u\n", (unsigned long)s_pcm_out, (unsigned)s_dur_ms);
            break;
        }

        if (!eof && in_len < DEC_IN_CHUNK && mq.n < mq.cap - 2048) {
            size_t nread = fread(in_buf + in_len, 1, DEC_IN_CHUNK - in_len, fp);
            in_len += (uint32_t)nread;
            s_bytes_done += (uint32_t)nread;
            if (nread == 0) {
                eof = 1;
            }
        }

        /* Same end condition as pcm5102-sd-player: file done and input buffer empty. */
        if (eof && in_len == 0 && mq_avail(&mq) == 0) {
            s_fin_eof = 1;
            break;
        }

        if (in_len > 0 || (eof && mq.n < mq.cap - 1024)) {
            esp_audio_simple_dec_raw_t raw = {
                .buffer = in_buf,
                .len = in_len,
                .consumed = 0,
                .eos = eof,
            };
            int progressed = 0;
            do {
                if (s_stop) {
                    break;
                }
                if (mq.n > mq.cap - 4096) {
                    drain_mq(&mq, blk, &primed, 0);
                    if (mq.n > mq.cap - 4096) {
                        break;
                    }
                }
                esp_audio_simple_dec_out_t out = {
                    .buffer = out_buf,
                    .len = (uint32_t)out_cap,
                };
                er = esp_audio_simple_dec_process(dec, &raw, &out);
                music_yield();
                if (er == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    int need = (int)out.needed_size;
                    if (need < out_cap + 4096) {
                        need = out_cap + 4096;
                    }
                    uint8_t *nb = audio_realloc(out_buf, (size_t)need);
                    if (!nb) {
                        nk_printf("music outmem\n");
                        goto done;
                    }
                    out_buf = nb;
                    out_cap = need;
                    progressed = 1;
                    continue;
                }
                if (er != ESP_AUDIO_ERR_OK) {
                    nk_printf("music decode %d\n", (int)er);
                    goto done;
                }
                if (out.decoded_size && !s_fin_eof) {
                    progressed = 1;
                    if (!opened_info) {
                        esp_audio_simple_dec_info_t info = {0};
                        if (esp_audio_simple_dec_get_info(dec, &info) != ESP_AUDIO_ERR_OK ||
                            info.sample_rate == 0) {
                            nk_printf("music no pcm info\n");
                            goto done;
                        }
                        s_src_rate = info.sample_rate;
                        mq_set_rate(&mq, info.sample_rate, AUDIO_SAMPLE_RATE);
                        ch = info.channel > 0 ? (int)info.channel : 1;
                        bits = info.bits_per_sample > 0 ? (int)info.bits_per_sample : 16;
                        nk_printf("music play %s rate=%u ch=%d bits=%d ->%d\n", path,
                                  (unsigned)info.sample_rate, ch, bits, AUDIO_SAMPLE_RATE);
                        opened_info = 1;
                    }
                    /* Convert in slices if decoder returns a huge PCM chunk. */
                    const uint8_t *p = out.buffer;
                    int left = (int)out.decoded_size;
                    int bps = bits / 8;
                    if (bps < 1) {
                        bps = 2;
                    }
                    int frame_bytes = bps * (ch < 1 ? 1 : ch);
                    while (left >= frame_bytes && !s_stop) {
                        int max_frames = mono_cap;
                        int nbytes = left;
                        if (nbytes / frame_bytes > max_frames) {
                            nbytes = max_frames * frame_bytes;
                        }
                        int got = pcm_to_mono16(p, nbytes, ch, bits, mono_tmp, max_frames);
                        if (got <= 0) {
                            break;
                        }
                        if (mq_push_all(&mq, mono_tmp, got, blk, &primed) < got) {
                            goto done; /* stopped */
                        }
                        if (s_fin_eof) {
                            left = 0;
                            break;
                        }
                        p += got * frame_bytes;
                        left -= got * frame_bytes;
                    }
                } else if (eof && out.decoded_size == 0 && raw.consumed == 0 && in_len == 0) {
                    /* Official end pattern: eos set, no more PCM, nothing consumed. */
                    progressed = 0;
                    eof_stall = 99;
                }
                if (raw.consumed > 0) {
                    if (raw.consumed > raw.len) {
                        raw.consumed = raw.len;
                    }
                    uint32_t left_in = raw.len - raw.consumed;
                    if (left_in && raw.consumed) {
                        memmove(in_buf, raw.buffer + raw.consumed, left_in);
                    }
                    in_len = left_in;
                    raw.buffer = in_buf;
                    raw.len = in_len;
                    raw.consumed = 0;
                    progressed = 1;
                } else {
                    break;
                }
            } while (raw.len && !s_stop);

            if (!progressed && eof) {
                eof_stall++;
            } else if (progressed) {
                eof_stall = 0;
            }
            /* Idle after EOF, or decoder spinning without consuming input. */
            if (eof && (( !progressed && in_len == 0 && mq_avail(&mq) == 0) || eof_stall >= 8)) {
                while (!s_stop && mq_avail(&mq) > 0) {
                    if (!drain_mq(&mq, blk, &primed, 1)) {
                        vTaskDelay(pdMS_TO_TICKS(1));
                    }
                }
                s_fin_eof = 1;
                break;
            }
            if (!progressed && !eof && in_len >= DEC_IN_CHUNK && mq_avail(&mq) == 0) {
                nk_printf("music dec stuck\n");
                goto done;
            }
        } else if (eof) {
            /* No more input to feed decoder — flush and finish. */
            while (!s_stop && mq_avail(&mq) > 0) {
                if (!drain_mq(&mq, blk, &primed, 1)) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
            }
            s_fin_eof = 1;
            break;
        }

        if (mq_avail(&mq) == 0 && !eof) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    if (!primed && mixer_stream_used() > 0) {
        mixer_stream_go();
        primed = 1;
    }
    /* Drain mixer (cap wait — a stuck used>0 used to block forever → no auto-next). */
    for (int wait_ms = 0; primed && mixer_stream_used() > 0 && wait_ms < 3000; wait_ms += 10) {
        if (s_stop && !s_fin_eof) {
            break; /* manual stop before EOF */
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
done:
    /* Fallback: played ≥95% of probed duration ⇒ treat as finished (header dur vs pcm). */
    if (!s_fin_eof && opened_info && s_dur_ms > 0 && s_pcm_out > 0) {
        uint32_t now_ms = (uint32_t)(((uint64_t)s_pcm_out * 1000u) / (uint32_t)AUDIO_SAMPLE_RATE);
        if (now_ms + 50u >= (s_dur_ms * 95u) / 100u) {
            s_fin_eof = 1;
        }
    }
    free(in_buf);
    free(out_buf);
    free(mono_tmp);
    free(mq.ring);
    esp_audio_simple_dec_close(dec);
    fclose(fp);
    mixer_stream_stop();
    nk_printf("music done eof=%d stop=%d pcm=%lu dur=%u\n", s_fin_eof, s_stop,
              (unsigned long)s_pcm_out, (unsigned)s_dur_ms);
    return opened_info ? 0 : -1;
}

static void music_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            char path[MUSIC_PATH_MAX];
            taskENTER_CRITICAL(&s_mux);
            memcpy(path, s_pending, sizeof(path));
            s_pending[0] = 0;
            taskEXIT_CRITICAL(&s_mux);
            if (!path[0]) {
                break;
            }

            memcpy(s_path, path, sizeof(s_path));
            s_stop = 0;
            s_pause = 0;
            s_ended = 0;
            s_fin_eof = 0;
            s_pcm_out = 0;
            s_bytes_done = 0;
            s_bytes_total = 0;
            s_dur_ms = 0;
            s_src_rate = 0;
            s_busy = 1;
            play_decoded(s_path);
            int nat = s_fin_eof; /* EOF reached = natural end (not based on s_stop) */
            s_busy = 0;
            s_pause = 0;
            s_path[0] = 0;
            taskENTER_CRITICAL(&s_mux);
            int more = s_pending[0] != 0;
            taskEXIT_CRITICAL(&s_mux);
            /* Chain next track here — don't rely on 250ms poll + take_ended races. */
            if (nat && !more && s_next_provider) {
                char nxt[MUSIC_PATH_MAX];
                if (s_next_provider(nxt, sizeof(nxt)) == 0 && nxt[0]) {
                    taskENTER_CRITICAL(&s_mux);
                    strncpy(s_pending, nxt, sizeof(s_pending) - 1);
                    s_pending[sizeof(s_pending) - 1] = 0;
                    more = 1;
                    taskEXIT_CRITICAL(&s_mux);
                    nk_printf("music auto-next %s\n", nxt);
                }
            }
            if (nat && !more) {
                s_ended = 1;
                nk_printf("music ended (no auto-next)\n");
            }
            if (!more) {
                break;
            }
        }
    }
}

static void music_stop(void)
{
    s_pause = 0;
    s_stop = 1;
    mixer_stream_stop();
}

void sd_music_pause_toggle(void)
{
    if (!s_path[0] || s_stop) {
        return;
    }
    s_pause = !s_pause;
    nk_printf("music pause=%d\n", s_pause);
}

int sd_music_paused(void)
{
    return (s_path[0] && !s_stop && s_pause) ? 1 : 0;
}

static void music_play(const char *name)
{
    char full[MUSIC_PATH_MAX];
    if (!name || !name[0]) {
        nk_printf("music play <file.(wav|mp3|flac|…)>\n");
        return;
    }
    while (*name == ' ') {
        name++;
    }
    if (name[0] == '/') {
        strncpy(full, name, sizeof(full) - 1);
        full[sizeof(full) - 1] = 0;
    } else {
        snprintf(full, sizeof(full), "%s/%s", MUSIC_DIR, name);
    }
    if (type_from_name(full) == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        nk_printf("music unsupported %s\n", full);
        return;
    }
    /* Queue path first, then stop current — music_task must not wipe the new path. */
    taskENTER_CRITICAL(&s_mux);
    strncpy(s_pending, full, sizeof(s_pending) - 1);
    s_pending[sizeof(s_pending) - 1] = 0;
    taskEXIT_CRITICAL(&s_mux);
    music_stop();
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void sd_music_get_progress(uint32_t *ms, uint32_t *dur_ms, unsigned *pct, int *playing)
{
    /* s_dur_ms is frozen at open. This path only scales position — no byte-ratio estimate. */
    uint32_t now = 0;
    uint32_t dur = 0;
    unsigned p = 0;
    int active = s_busy && s_path[0] && !s_stop;
    if (active) {
        now = (uint32_t)(((uint64_t)s_pcm_out * 1000u) / (uint32_t)AUDIO_SAMPLE_RATE);
        dur = s_dur_ms;
        int tempo = s_tempo_pct;
        if (tempo != 100 && dur) {
            dur = (uint32_t)(((uint64_t)dur * 100u) / (uint32_t)tempo);
        }
        if (dur && now > dur) {
            now = dur;
            p = 100;
        } else if (dur) {
            p = (unsigned)(((uint64_t)now * 100u) / dur);
        }
    }
    if (ms) {
        *ms = now;
    }
    if (dur_ms) {
        *dur_ms = dur;
    }
    if (pct) {
        *pct = p;
    }
    if (playing) {
        *playing = active && !s_pause;
    }
}

void sd_music_dump_status(void)
{
    nk_printf("music_path=%s\n", s_path[0] ? s_path : "-");
    uint32_t ms = 0, dur = 0;
    unsigned pct = 0;
    int playing = 0;
    sd_music_get_progress(&ms, &dur, &pct, &playing);
    nk_printf("music_play=%d\n", playing);
    nk_printf("music_ms=%lu\n", (unsigned long)ms);
    nk_printf("music_dur=%lu\n", (unsigned long)dur);
    nk_printf("music_pct=%u\n", pct);
    nk_printf("music_rate=%u\n", (unsigned)s_src_rate);
}

const char *sd_music_path(void)
{
    return s_path;
}

void sd_music_set_tempo_pct(int pct)
{
    s_tempo_pct = clamp_tempo(pct);
}

int sd_music_tempo_pct(void)
{
    return s_tempo_pct;
}

int sd_music_take_ended(void)
{
    int v = s_ended;
    s_ended = 0;
    return v;
}

void sd_music_set_next_provider(int (*fn)(char *out, size_t out_sz))
{
    s_next_provider = fn;
}

void sd_music_cmd(const char *args)
{
    if (!args) {
        return;
    }
    while (*args == ' ') {
        args++;
    }
    if (strcmp(args, "list") == 0) {
        list_music();
        return;
    }
    if (strcmp(args, "stop") == 0) {
        music_stop();
        nk_printf("music stop\n");
        return;
    }
    if (strcmp(args, "pause") == 0) {
        sd_music_pause_toggle();
        return;
    }
    if (strncmp(args, "play ", 5) == 0) {
        music_play(args + 5);
        return;
    }
    nk_printf("music list | play file | stop | pause  (wav/mp3/flac/aac)\n");
}

esp_err_t sd_music_start(void)
{
    if (!s_codec_ready) {
        esp_audio_err_t er = esp_audio_dec_register_default();
        if (er != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "dec register %d", (int)er);
        }
        er = esp_audio_simple_dec_register_default();
        if (er != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "simple_dec register %d", (int)er);
        } else {
            s_codec_ready = 1;
            ESP_LOGI(TAG, "audio codecs ready (wav/mp3/flac/aac)");
        }
    }
    /*
     * Core 1 + moderate priority: keep mixer(core1/pri10) fed, but do not starve
     * key_link/UI/RGB on core 0 (was pri8@core0 → FLAC made LED/UI ~0.5fps).
     */
    xTaskCreatePinnedToCore(music_task, "sd_music", 16384, NULL, 5, &s_task, 1);
    return ESP_OK;
}
