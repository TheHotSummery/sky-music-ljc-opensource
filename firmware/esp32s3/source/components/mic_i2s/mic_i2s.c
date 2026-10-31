#include "mic_i2s.h"

#include "board_pins.h"
#include "key_link.h"
#include "mixer.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "mic";

#define MIC_SR           22050
#define MIC_FRAMES       256
#define MIC_TAP_THRESH   4000
#define MIC_LAB_SEC      4
#define MIC_LAB_MAX      (MIC_SR * MIC_LAB_SEC)
#define MIC_EAR_GAIN_Q8  (3 * 256) /* ~3× digital boost (lab playback was quiet) */
#define MIC_HOWL_ON      12000
#define MIC_HOWL_OFF     4500
#define MIC_HOWL_ON_MS   350
#define MIC_HOWL_OFF_MS  800
#define MIC_KEY_MUTE_MS  1000

static i2s_chan_handle_t s_rx;
static int32_t *s_dma;
static TaskHandle_t s_task;
static TaskHandle_t s_play_task;
static volatile int s_run;
static volatile int s_tap_mode;
static volatile int s_rms;
static volatile int s_peak;
static volatile uint32_t s_taps;
/* 0=left (ICS LR→GND typical), 1=right */
static int s_slot_right;
static int s_ws_inv;
static int s_shift = 16; /* ICS-43434 24bit-in-32: >>16 works on this board */

static volatile mic_lab_phase_t s_lab_phase = MIC_LAB_OFF;
static volatile int s_lab_sel; /* 0 play 1 pause 2 stop */
static int16_t *s_lab_pcm;
static volatile int s_lab_wr;
static volatile int s_lab_len;
static volatile int s_lab_play_pos;
static volatile int s_lab_play_run;
static mic_lab_ui_cb_t s_lab_ui_cb;

/* Ear monitor */
static volatile int s_ear_howl;
static volatile int s_ear_key_mute;
static volatile int s_ear_gain_q8 = MIC_EAR_GAIN_Q8;
static TickType_t s_ear_mute_until;
static TickType_t s_howl_hi_since;
static TickType_t s_howl_lo_since;
static uint16_t s_ear_last_mask;
static int s_ear_primed;

static int decode_peak_rms(const int32_t *slot32, size_t n, int *peak_out)
{
    int64_t acc = 0;
    int peak = 0;
    for (size_t i = 0; i < n; ++i) {
        int32_t s = slot32[i] >> s_shift;
        if (s > 32767) {
            s = 32767;
        }
        if (s < -32768) {
            s = -32768;
        }
        int a = s < 0 ? -s : s;
        if (a > peak) {
            peak = a;
        }
        acc += (int64_t)s * (int64_t)s;
    }
    *peak_out = peak;
    if (n == 0) {
        return 0;
    }
    return (int)sqrt((double)acc / (double)n);
}

static void append_lab_samples(const int32_t *slot32, size_t n)
{
    if (!s_lab_pcm || s_lab_phase != MIC_LAB_REC) {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        if (s_lab_wr >= MIC_LAB_MAX) {
            s_lab_len = s_lab_wr;
            s_lab_phase = MIC_LAB_READY;
            s_lab_sel = 0;
            ESP_LOGI(TAG, "lab rec full %d samples (%.1fs)", s_lab_len,
                     (double)s_lab_len / (double)MIC_SR);
            return;
        }
        int32_t s = slot32[i] >> s_shift;
        if (s > 32767) {
            s = 32767;
        }
        if (s < -32768) {
            s = -32768;
        }
        s_lab_pcm[s_lab_wr++] = (int16_t)s;
    }
}

static void ear_push_block(const int32_t *slot32, size_t n)
{
    int16_t blk[AUDIO_BLOCK_FRAMES];
    TickType_t now = xTaskGetTickCount();
    uint16_t mask = key_link_keymask();
    /* Any key edge → silence 1s (mechanical click / note into mic). */
    if (mask != s_ear_last_mask) {
        if ((mask & ~s_ear_last_mask) != 0) {
            s_ear_mute_until = now + pdMS_TO_TICKS(MIC_KEY_MUTE_MS);
        }
        s_ear_last_mask = mask;
    }
    s_ear_key_mute = ((int32_t)(now - s_ear_mute_until) < 0) ? 1 : 0;

    /* Simple howl: sustained loud → duck gain; quiet → release. */
    if (s_rms >= MIC_HOWL_ON) {
        if (s_howl_hi_since == 0) {
            s_howl_hi_since = now;
        } else if (!s_ear_howl && (now - s_howl_hi_since) >= pdMS_TO_TICKS(MIC_HOWL_ON_MS)) {
            s_ear_howl = 1;
            s_ear_gain_q8 = MIC_EAR_GAIN_Q8 / 4;
            ESP_LOGW(TAG, "ear howl duck rms=%d", s_rms);
        }
        s_howl_lo_since = 0;
    } else if (s_rms <= MIC_HOWL_OFF) {
        s_howl_hi_since = 0;
        if (s_ear_howl) {
            if (s_howl_lo_since == 0) {
                s_howl_lo_since = now;
            } else if ((now - s_howl_lo_since) >= pdMS_TO_TICKS(MIC_HOWL_OFF_MS)) {
                s_ear_howl = 0;
                s_ear_gain_q8 = MIC_EAR_GAIN_Q8;
                ESP_LOGI(TAG, "ear howl release");
            }
        }
    } else {
        s_howl_hi_since = 0;
        s_howl_lo_since = 0;
    }

    size_t frames = n;
    if (frames > (size_t)AUDIO_BLOCK_FRAMES) {
        frames = (size_t)AUDIO_BLOCK_FRAMES;
    }
    int g = s_ear_gain_q8;
    for (size_t i = 0; i < (size_t)AUDIO_BLOCK_FRAMES; ++i) {
        if (s_ear_key_mute || i >= frames) {
            blk[i] = 0;
            continue;
        }
        int32_t s = slot32[i] >> s_shift;
        int32_t o = (s * g) >> 8;
        if (o > 32767) {
            o = 32767;
        }
        if (o < -32768) {
            o = -32768;
        }
        blk[i] = (int16_t)o;
    }
    if (!mixer_stream_push(blk, AUDIO_BLOCK_FRAMES)) {
        /* drop if mixer busy */
    } else if (!s_ear_primed && mixer_stream_used() >= 4) {
        mixer_stream_go();
        s_ear_primed = 1;
    }
}

static void mic_task(void *arg)
{
    (void)arg;
    size_t bytes = 0;
    TickType_t last_log = 0;
    for (;;) {
        if (!s_run || !s_rx) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        esp_err_t err = i2s_channel_read(s_rx, s_dma, MIC_FRAMES * sizeof(int32_t), &bytes,
                                         pdMS_TO_TICKS(200));
        if (err != ESP_OK || bytes < sizeof(int32_t)) {
            continue;
        }
        size_t n = bytes / sizeof(int32_t);
        int peak = 0;
        int rms = decode_peak_rms(s_dma, n, &peak);
        s_rms = rms;
        s_peak = peak;
        if (s_lab_phase == MIC_LAB_REC) {
            append_lab_samples(s_dma, n);
        } else if (s_lab_phase == MIC_LAB_EAR) {
            ear_push_block(s_dma, n);
        }
        if (s_tap_mode && peak >= MIC_TAP_THRESH) {
            s_taps++;
            ESP_LOGI(TAG, "TAP #%lu peak=%d rms=%d", (unsigned long)s_taps, peak, rms);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        TickType_t now = xTaskGetTickCount();
        if (!s_tap_mode && s_lab_phase == MIC_LAB_OFF && (now - last_log) >= pdMS_TO_TICKS(500)) {
            last_log = now;
            ESP_LOGI(TAG, "rms=%d peak=%d raw0=0x%08lX shift=%d slot=%s", rms, peak,
                     (unsigned long)(uint32_t)s_dma[0], s_shift, s_slot_right ? "R" : "L");
        }
    }
}

static void lab_play_task(void *arg)
{
    (void)arg;
    int16_t blk[AUDIO_BLOCK_FRAMES];
    int primed = 0;
    for (;;) {
        if (s_lab_phase == MIC_LAB_PAUSE) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!s_lab_play_run || s_lab_phase != MIC_LAB_PLAY || !s_lab_pcm || s_lab_len <= 0) {
            primed = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int pos = s_lab_play_pos;
        if (pos >= s_lab_len) {
            /* drain remaining streamed audio then finish */
            while (s_lab_play_run && mixer_stream_used() > 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            mixer_stream_stop();
            s_lab_play_pos = 0;
            s_lab_phase = MIC_LAB_READY;
            s_lab_play_run = 0;
            primed = 0;
            ESP_LOGI(TAG, "lab play done");
            continue;
        }
        int n = s_lab_len - pos;
        if (n > AUDIO_BLOCK_FRAMES) {
            n = AUDIO_BLOCK_FRAMES;
        }
        memcpy(blk, &s_lab_pcm[pos], (size_t)n * sizeof(int16_t));
        if (n < AUDIO_BLOCK_FRAMES) {
            memset(blk + n, 0, (size_t)(AUDIO_BLOCK_FRAMES - n) * sizeof(int16_t));
        }
        while (s_lab_play_run && s_lab_phase == MIC_LAB_PLAY &&
               !mixer_stream_push(blk, AUDIO_BLOCK_FRAMES)) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        if (!(s_lab_play_run && s_lab_phase == MIC_LAB_PLAY)) {
            continue;
        }
        s_lab_play_pos = pos + n;
        if (!primed && mixer_stream_used() >= 8) {
            mixer_stream_go();
            primed = 1;
        }
    }
}

static void lab_play_ensure_task(void)
{
    if (!s_play_task) {
        xTaskCreatePinnedToCore(lab_play_task, "mic_play", 4096, NULL, 6, &s_play_task, 0);
    }
}

static void lab_play_start(void)
{
    if (!s_lab_pcm || s_lab_len <= 0) {
        printf("lab: no recording\n");
        return;
    }
    lab_play_ensure_task();
    if (s_lab_phase != MIC_LAB_PAUSE) {
        mixer_stream_stop();
        mixer_stream_start();
        s_lab_play_pos = 0;
    }
    s_lab_phase = MIC_LAB_PLAY;
    s_lab_play_run = 1;
    ESP_LOGI(TAG, "lab play from %d / %d", s_lab_play_pos, s_lab_len);
}

static void lab_play_pause(void)
{
    if (s_lab_phase == MIC_LAB_PLAY) {
        s_lab_phase = MIC_LAB_PAUSE;
        s_lab_play_run = 0;
        ESP_LOGI(TAG, "lab pause @%d", s_lab_play_pos);
    }
}

static void lab_play_stop(void)
{
    s_lab_play_run = 0;
    s_lab_play_pos = 0;
    mixer_stream_stop();
    if (s_lab_len > 0) {
        s_lab_phase = MIC_LAB_READY;
    } else {
        s_lab_phase = MIC_LAB_IDLE;
    }
    ESP_LOGI(TAG, "lab stop");
}

static void mic_hw_stop(void)
{
    s_run = 0;
    vTaskDelay(pdMS_TO_TICKS(30));
    if (s_rx) {
        (void)i2s_channel_disable(s_rx);
        (void)i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    if (s_dma) {
        free(s_dma);
        s_dma = NULL;
    }
}

static esp_err_t mic_hw_init(void)
{
    mic_hw_stop();

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_MIC_PORT, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = MIC_FRAMES;
    chan.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new rx: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_slot_config_t slot =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    slot.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    slot.slot_mask = s_slot_right ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;

    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SR);
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    i2s_std_config_t std = {
        .clk_cfg = clk,
        .slot_cfg = slot,
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = PIN_INMP_SCK,
                .ws = PIN_INMP_WS,
                .dout = I2S_GPIO_UNUSED,
                .din = PIN_INMP_SD,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = s_ws_inv ? true : false,
                    },
            },
    };
    err = i2s_channel_init_std_mode(s_rx, &std);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "std init: %s", esp_err_to_name(err));
        mic_hw_stop();
        return err;
    }

    /* Don't pull DIN — floating pull-up looks like full-scale. */
    gpio_set_pull_mode(PIN_INMP_SD, GPIO_FLOATING);

    s_dma = heap_caps_calloc(MIC_FRAMES, sizeof(int32_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_dma) {
        mic_hw_stop();
        return ESP_ERR_NO_MEM;
    }
    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        mic_hw_stop();
        return err;
    }
    ESP_LOGI(TAG, "ICS-43434 I2S1 RX %d Hz BCLK=%d WS=%d DIN=%d slot=%s ws_inv=%d shift=%d", MIC_SR,
             (int)PIN_INMP_SCK, (int)PIN_INMP_WS, (int)PIN_INMP_SD, s_slot_right ? "R" : "L",
             s_ws_inv, s_shift);
    return ESP_OK;
}

static esp_err_t mic_rx_ensure(void)
{
    if (s_run && s_rx) {
        return ESP_OK;
    }
    if (mic_hw_init() != ESP_OK) {
        return ESP_FAIL;
    }
    s_tap_mode = 0;
    s_run = 1;
    if (!s_task) {
        xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 5, &s_task, 0);
    }
    return ESP_OK;
}

static void ear_stop_stream(void)
{
    s_ear_primed = 0;
    s_ear_howl = 0;
    s_ear_key_mute = 0;
    s_ear_gain_q8 = MIC_EAR_GAIN_Q8;
    s_howl_hi_since = 0;
    s_howl_lo_since = 0;
    mixer_stream_stop();
}

static void lab_exit(void)
{
    s_lab_play_run = 0;
    ear_stop_stream();
    s_lab_phase = MIC_LAB_OFF;
    if (s_lab_ui_cb) {
        s_lab_ui_cb(0);
    }
    printf("mic lab/ear off\n");
}

static void lab_enter(void)
{
    if (s_lab_phase == MIC_LAB_EAR) {
        ear_stop_stream();
    } else if (s_lab_phase != MIC_LAB_OFF) {
        printf("mic lab already on\n");
        return;
    }
    if (!s_lab_pcm) {
        s_lab_pcm = heap_caps_malloc((size_t)MIC_LAB_MAX * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_lab_pcm) {
            printf("mic lab: no PSRAM\n");
            return;
        }
    }
    if (mic_rx_ensure() != ESP_OK) {
        printf("mic lab: rx fail\n");
        return;
    }
    s_lab_wr = 0;
    s_lab_len = 0;
    s_lab_play_pos = 0;
    s_lab_sel = 0;
    s_lab_phase = MIC_LAB_IDLE;
    lab_play_ensure_task();
    if (s_lab_ui_cb) {
        s_lab_ui_cb(1);
    }
    printf("mic lab: ENC1 click=录/执行  rotate=播放/暂停/停止  long=退出 (max %ds)\n", MIC_LAB_SEC);
}

static void ear_enter(void)
{
    if (s_lab_phase != MIC_LAB_OFF && s_lab_phase != MIC_LAB_EAR) {
        s_lab_play_run = 0;
        ear_stop_stream();
    }
    if (mic_rx_ensure() != ESP_OK) {
        printf("mic ear: rx fail\n");
        return;
    }
    mixer_stream_stop();
    mixer_stream_start();
    s_ear_primed = 0;
    s_ear_howl = 0;
    s_ear_key_mute = 0;
    s_ear_gain_q8 = MIC_EAR_GAIN_Q8;
    s_ear_mute_until = 0;
    s_howl_hi_since = 0;
    s_howl_lo_since = 0;
    s_ear_last_mask = key_link_keymask();
    s_lab_phase = MIC_LAB_EAR;
    if (s_lab_ui_cb) {
        s_lab_ui_cb(1);
    }
    printf("*** 请插入耳机再听！喇叭耳返易啸叫 ***\n");
    printf("mic ear: ENC2调音量  按键后静音1s  啸叫自动压增益  ENC1长按退出\n");
}

void mic_lab_set_ui_cb(mic_lab_ui_cb_t cb)
{
    s_lab_ui_cb = cb;
}

int mic_lab_active(void)
{
    return s_lab_phase != MIC_LAB_OFF;
}

mic_lab_phase_t mic_lab_phase(void)
{
    return s_lab_phase;
}

int mic_lab_sel(void)
{
    return s_lab_sel;
}

int mic_lab_len(void)
{
    return s_lab_len;
}

int mic_lab_play_pos(void)
{
    return s_lab_play_pos;
}

int mic_ear_howl(void)
{
    return s_ear_howl;
}

int mic_ear_key_mute(void)
{
    return s_ear_key_mute;
}

bool mic_lab_on_rotate(int delta)
{
    if (s_lab_phase == MIC_LAB_OFF) {
        return false;
    }
    if (s_lab_phase == MIC_LAB_EAR || s_lab_phase < MIC_LAB_READY) {
        return true;
    }
    if (!delta) {
        return true;
    }
    int d = delta > 0 ? 1 : -1;
    s_lab_sel = (s_lab_sel + d + 3 * 8) % 3;
    return true;
}

bool mic_lab_on_click(void)
{
    if (s_lab_phase == MIC_LAB_OFF) {
        return false;
    }
    if (s_lab_phase == MIC_LAB_EAR) {
        /* Reminder nudge — volume stays on ENC2 */
        printf("ear: 请用耳机 + 左旋钮调音量 (howl=%d key_mute=%d)\n", s_ear_howl, s_ear_key_mute);
        return true;
    }
    if (s_lab_phase == MIC_LAB_IDLE) {
        s_lab_wr = 0;
        s_lab_len = 0;
        s_lab_play_pos = 0;
        s_lab_phase = MIC_LAB_REC;
        ESP_LOGI(TAG, "lab rec start");
        return true;
    }
    if (s_lab_phase == MIC_LAB_REC) {
        s_lab_len = s_lab_wr;
        s_lab_phase = MIC_LAB_READY;
        s_lab_sel = 0;
        ESP_LOGI(TAG, "lab rec stop %d samples", s_lab_len);
        return true;
    }
    /* READY / PLAY / PAUSE: run selected action */
    if (s_lab_sel == 0) {
        lab_play_start();
    } else if (s_lab_sel == 1) {
        lab_play_pause();
    } else {
        lab_play_stop();
    }
    return true;
}

bool mic_lab_on_long(void)
{
    if (s_lab_phase == MIC_LAB_OFF) {
        return false;
    }
    lab_exit();
    return true;
}

esp_err_t mic_i2s_start(void)
{
    ESP_LOGI(TAG, "mic cmds: start|stop|tap|lab|ear|left|right|wsinv|shift N|dump");
    return ESP_OK;
}

int mic_i2s_running(void)
{
    return s_run;
}

int mic_i2s_rms(void)
{
    return s_rms;
}

int mic_i2s_peak(void)
{
    return s_peak;
}

bool mic_i2s_handle_line(const char *line)
{
    unsigned a = 0;
    if (!line) {
        return false;
    }
    if (strcmp(line, "mic") == 0) {
        printf("mic_run=%d rms=%d peak=%d taps=%lu slot=%s ws_inv=%d shift=%d lab=%d howl=%d\n",
               s_run, s_rms, s_peak, (unsigned long)s_taps, s_slot_right ? "R" : "L", s_ws_inv,
               s_shift, (int)s_lab_phase, s_ear_howl);
        printf("hint: mic ear | mic lab | left/right | shift 16\n");
        return true;
    }
    if (strcmp(line, "mic lab") == 0) {
        lab_enter();
        return true;
    }
    if (strcmp(line, "mic lab off") == 0 || strcmp(line, "mic ear off") == 0) {
        if (s_lab_phase != MIC_LAB_OFF) {
            lab_exit();
        } else {
            printf("mic lab/ear already off\n");
        }
        return true;
    }
    if (strcmp(line, "mic ear") == 0) {
        ear_enter();
        return true;
    }
    if (strcmp(line, "mic left") == 0) {
        s_slot_right = 0;
        printf("slot=L (re-run mic start)\n");
        return true;
    }
    if (strcmp(line, "mic right") == 0) {
        s_slot_right = 1;
        printf("slot=R (re-run mic start)\n");
        return true;
    }
    if (strcmp(line, "mic wsinv") == 0) {
        s_ws_inv = !s_ws_inv;
        printf("ws_inv=%d (re-run mic start)\n", s_ws_inv);
        return true;
    }
    if (sscanf(line, "mic shift %u", &a) == 1) {
        if (a > 24) {
            a = 24;
        }
        s_shift = (int)a;
        printf("shift=%d\n", s_shift);
        return true;
    }
    if (strcmp(line, "mic dump") == 0) {
        if (!s_run || !s_dma) {
            printf("mic not running\n");
            return true;
        }
        printf("raw:");
        for (int i = 0; i < 8; i++) {
            printf(" %08lX", (unsigned long)(uint32_t)s_dma[i]);
        }
        printf("\n");
        return true;
    }
    if (strcmp(line, "mic start") == 0) {
        if (mic_hw_init() != ESP_OK) {
            printf("mic_init=fail\n");
            return true;
        }
        s_tap_mode = 0;
        s_run = 1;
        if (!s_task) {
            xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 5, &s_task, 0);
        }
        printf("mic_run=1 slot=%s\n", s_slot_right ? "R" : "L");
        return true;
    }
    if (strcmp(line, "mic tap") == 0) {
        if (mic_hw_init() != ESP_OK) {
            printf("mic_init=fail\n");
            return true;
        }
        s_tap_mode = 1;
        s_run = 1;
        if (!s_task) {
            xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 5, &s_task, 0);
        }
        printf("mic_tap=1\n");
        return true;
    }
    if (strcmp(line, "mic stop") == 0) {
        if (s_lab_phase != MIC_LAB_OFF) {
            lab_exit();
        }
        mic_hw_stop();
        printf("mic_run=0\n");
        return true;
    }
    return false;
}
