#include "enc_fn.h"

#include "audio_bank.h"
#include "board_pins.h"
#include "drill.h"
#include "mixer.h"
#include "nk_out.h"
#include "score_play.h"
#include "sd_music.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "enc1";

/* Soft cap only (not FAT). Path may include long UTF-8 LFN; 2048 × 384 ≈ 768KB PSRAM. */
#define MAX_INDEX   2048
#define ENC_NAME_MAX 384
#define LIB_WIN     24  /* TCP status dump window */

static const char *s_mode_name[ENC_FN_MODE_COUNT] = {
    "play", "music", "score", "follow", "record",
};

static int s_mode = ENC_FN_PLAY;
static char (*s_item)[ENC_NAME_MAX];
static int s_cap;
static int s_n;
static int s_idx;
static SemaphoreHandle_t s_mu;
static int s_armed;
static int s_loop = 1; /* default: play next when the current track ends */
static char s_last_music_bn[96]; /* basename of last music play (for auto-next) */
static bool (*s_nav_rotate)(int delta);
static bool (*s_nav_click)(void);
static bool (*s_nav_long)(void);
static void (*s_list_changed_cb)(void);
static char s_tip[64];
static TickType_t s_tip_until;
static char s_lib_m[LIB_WIN][96];
static int s_lib_mn; /* full music count */
static int s_lib_moff;
static char s_lib_s[LIB_WIN][96];
static int s_lib_sn;
static int s_lib_soff;

static int ends_with(const char *n, const char *ext)
{
    size_t a = strlen(n), b = strlen(ext);
    if (a < b) {
        return 0;
    }
    return strcasecmp(n + a - b, ext) == 0;
}

static void add_item(const char *s)
{
    if (!s_item || s_n >= s_cap || !s || !s[0]) {
        return;
    }
    strncpy(s_item[s_n], s, ENC_NAME_MAX - 1);
    s_item[s_n][ENC_NAME_MAX - 1] = 0;
    s_n++;
}

static void scan_dir_files(const char *dir, int wav, int score)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        int ok = 0;
        if (wav && (ends_with(ent->d_name, ".wav") || ends_with(ent->d_name, ".mp3") ||
                    ends_with(ent->d_name, ".flac") || ends_with(ent->d_name, ".aac"))) {
            ok = 1;
        }
        if (score && (ends_with(ent->d_name, ".txt") || ends_with(ent->d_name, ".json") ||
                      ends_with(ent->d_name, ".sky"))) {
            ok = 1;
        }
        if (!ok) {
            continue;
        }
        char path[ENC_NAME_MAX];
        size_t dl = strlen(dir);
        size_t nl = strlen(ent->d_name);
        if (dl + 1 + nl + 1 > sizeof(path)) {
            ESP_LOGW(TAG, "skip too-long name (%uB): %.40s…", (unsigned)(dl + 1 + nl), ent->d_name);
            continue;
        }
        memcpy(path, dir, dl);
        path[dl] = '/';
        memcpy(path + dl + 1, ent->d_name, nl + 1);
        add_item(path);
    }
    closedir(d);
}

static int cmp_item(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static void sort_items(void)
{
    if (s_n > 1) {
        qsort(s_item, (size_t)s_n, sizeof(s_item[0]), cmp_item);
    }
}

static void snapshot_lib(void);

static void scan_scores(void)
{
    scan_dir_files(SD_MOUNT_POINT "/puzi", 0, 1);
    if (s_n == 0) {
        scan_dir_files(SD_MOUNT_POINT, 0, 1);
    }
    /* No TF / empty card: still offer factory demo scores. */
    if (s_n == 0) {
        scan_dir_files(FACTORY_MOUNT_POINT "/puzi", 0, 1);
    }
}

static void rescan(void)
{
    s_n = 0;
    s_idx = 0;
    switch (s_mode) {
    case ENC_FN_PLAY:
        add_item("piano");
        add_item("violin");
        {
            const char *cur = audio_bank_name();
            for (int i = 0; i < s_n; ++i) {
                if (cur && strcmp(s_item[i], cur) == 0) {
                    s_idx = i;
                }
            }
        }
        break;
    case ENC_FN_MUSIC:
        scan_dir_files(SD_MOUNT_POINT "/music", 1, 0);
        sort_items();
        break;
    case ENC_FN_SCORE:
    case ENC_FN_FOLLOW:
        scan_scores();
        sort_items();
        break;
    case ENC_FN_RECORD:
        scan_scores();
        sort_items();
        if (s_n < s_cap) {
            memmove(s_item[1], s_item[0], (size_t)s_n * ENC_NAME_MAX);
            strncpy(s_item[0], DRILL_NEW_NAME, ENC_NAME_MAX - 1);
            s_item[0][ENC_NAME_MAX - 1] = 0;
            s_n++;
            s_idx = 0;
        }
        break;
    default:
        break;
    }
    snapshot_lib();
}

static const char *sel(void)
{
    if (s_n <= 0 || s_idx < 0 || s_idx >= s_n) {
        return "-";
    }
    return s_item[s_idx];
}

static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void fill_lib_win(char dst[][96], int *off_out, int total, int focus)
{
    int off = 0;
    if (total > LIB_WIN) {
        off = focus - LIB_WIN / 2;
        if (off < 0) {
            off = 0;
        }
        if (off + LIB_WIN > total) {
            off = total - LIB_WIN;
        }
    }
    *off_out = off;
    int n = total - off;
    if (n > LIB_WIN) {
        n = LIB_WIN;
    }
    for (int i = 0; i < n; ++i) {
        strncpy(dst[i], basename_of(s_item[off + i]), 95);
        dst[i][95] = 0;
    }
    for (int i = n; i < LIB_WIN; ++i) {
        dst[i][0] = 0;
    }
}

static void snapshot_lib(void)
{
    if (s_mode == ENC_FN_MUSIC) {
        s_lib_mn = s_n;
        fill_lib_win(s_lib_m, &s_lib_moff, s_n, s_idx);
    } else if (s_mode == ENC_FN_SCORE || s_mode == ENC_FN_FOLLOW) {
        s_lib_sn = s_n;
        fill_lib_win(s_lib_s, &s_lib_soff, s_n, s_idx);
    }
}

static void stop_engines(void)
{
    s_armed = 0;
    (void)sd_music_take_ended();
    (void)score_take_ended();
    sd_music_cmd("stop");
    score_cmd_stop();
    drill_abort();
}

static void apply_bank(void)
{
    MixCmd stop = { .type = MIX_CMD_STOP_ALL };
    mixer_submit(&stop);
    vTaskDelay(pdMS_TO_TICKS(20));
    if (audio_bank_load(sel()) != ESP_OK) {
        ESP_LOGW(TAG, "bank %s fail", sel());
    } else {
        ESP_LOGI(TAG, "ENC1 bank %s", sel());
    }
}

static void play_music(void)
{
    int playing = 0, paused = 0, n = 0, idx = 0;
    score_get_runtime(&n, &idx, &playing, &paused);
    if (playing) {
        score_cmd_stop();
    }
    const char *p = sel();
    if (p[0] == '-') {
        ESP_LOGW(TAG, "no music files in /sdcard/music");
        return;
    }
    /* Prefer full path — basenames can exceed the old 180B cmd buffer. */
    char cmd[8 + ENC_NAME_MAX];
    if (snprintf(cmd, sizeof(cmd), "play %s", p) >= (int)sizeof(cmd)) {
        ESP_LOGW(TAG, "music path too long");
        return;
    }
    {
        const char *bn = basename_of(p);
        strncpy(s_last_music_bn, bn, sizeof(s_last_music_bn) - 1);
        s_last_music_bn[sizeof(s_last_music_bn) - 1] = 0;
    }
    sd_music_cmd(cmd);
    s_armed = 1;
    /* Do not take_ended() here — it races with natural-end auto-next. */
    ESP_LOGI(TAG, "ENC1 music %s", basename_of(p));
}

static void play_score_now(void)
{
    const char *p = sel();
    if (p[0] == '-') {
        ESP_LOGW(TAG, "no scores in /sdcard/puzi");
        return;
    }
    sd_music_cmd("stop");
    (void)score_take_ended();
    if (score_cmd_open(p) != ESP_OK) {
        return;
    }
    for (int i = 0; i < 40 && score_is_opening(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (score_note_count() <= 0) {
        return;
    }
    score_cmd_play();
    s_armed = 1;
    ESP_LOGI(TAG, "ENC1 score %s", basename_of(p));
}

static int same_basename(const char *path, const char *sel_path)
{
    const char *a = basename_of(path);
    const char *b = basename_of(sel_path);
    return a[0] && b[0] && strcasecmp(a, b) == 0;
}

static void play_score(void)
{
    int n = 0, idx = 0, playing = 0, paused = 0;
    score_get_runtime(&n, &idx, &playing, &paused);
    if ((playing || paused) && same_basename(score_current_path(), sel())) {
        if (playing && !paused) {
            score_cmd_pause();
            ESP_LOGI(TAG, "ENC1 score pause");
            return;
        }
        if (paused) {
            score_cmd_play();
            ESP_LOGI(TAG, "ENC1 score resume");
            return;
        }
    }
    const char *p = sel();
    if (p[0] == '-') {
        ESP_LOGW(TAG, "no scores in /sdcard/puzi");
        return;
    }
    sd_music_cmd("stop");
    if (score_cmd_open(p) != ESP_OK) {
        ESP_LOGW(TAG, "score open busy/fail %s", basename_of(p));
        snprintf(s_tip, sizeof(s_tip), "打开失败(忙)");
        s_tip_until = xTaskGetTickCount() + pdMS_TO_TICKS(2500);
        return;
    }
    for (int i = 0; i < 40 && score_is_opening(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (score_note_count() <= 0) {
        ESP_LOGW(TAG, "score empty/bad format %s", basename_of(p));
        snprintf(s_tip, sizeof(s_tip), "谱格式错误或为空");
        s_tip_until = xTaskGetTickCount() + pdMS_TO_TICKS(2500);
        return;
    }
    score_cmd_play();
    s_armed = 1;
    (void)score_take_ended();
    ESP_LOGI(TAG, "ENC1 score %s notes=%d", basename_of(p), score_note_count());
}

static void play_sel(void)
{
    switch (s_mode) {
    case ENC_FN_PLAY:
        apply_bank();
        break;
    case ENC_FN_MUSIC:
        play_music();
        break;
    case ENC_FN_SCORE:
        play_score();
        break;
    case ENC_FN_FOLLOW:
        sd_music_cmd("stop");
        score_cmd_stop();
        drill_follow_toggle(sel());
        break;
    case ENC_FN_RECORD:
        sd_music_cmd("stop");
        score_cmd_stop();
        drill_record_toggle(sel());
        break;
    default:
        break;
    }
}

enc_fn_mode_t enc_fn_mode(void)
{
    return (enc_fn_mode_t)s_mode;
}

const char *enc_fn_mode_name(void)
{
    if (s_mode < 0 || s_mode >= ENC_FN_MODE_COUNT) {
        return "?";
    }
    return s_mode_name[s_mode];
}

int enc_fn_list_n(void)
{
    return s_n;
}

int enc_fn_list_idx(void)
{
    return s_n ? s_idx : -1;
}

const char *enc_fn_list_name(int idx)
{
    if (idx < 0 || idx >= s_n) {
        return "";
    }
    return basename_of(s_item[idx]);
}

const char *enc_fn_list_path(int idx)
{
    if (idx < 0 || idx >= s_n) {
        return "";
    }
    return s_item[idx];
}

bool enc_fn_select_idx(int idx)
{
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
    if (s_n <= 0) {
        rescan();
    }
    bool ok = false;
    if (s_n > 0 && idx >= 0 && idx < s_n) {
        s_idx = idx;
        snapshot_lib();
        ok = true;
    }
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
    return ok;
}

void enc_fn_rescan_current(void)
{
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
    rescan();
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
}

void enc_fn_set_list_changed_cb(void (*cb)(void))
{
    s_list_changed_cb = cb;
}

const char *enc_fn_take_tip(void)
{
    if (!s_tip[0]) {
        return "";
    }
    if ((int32_t)(xTaskGetTickCount() - s_tip_until) >= 0) {
        s_tip[0] = 0;
        return "";
    }
    return s_tip;
}

bool enc_fn_select_name(const char *basename)
{
    if (!basename || !basename[0]) {
        return false;
    }
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
    rescan();
    bool ok = false;
    for (int i = 0; i < s_n; ++i) {
        if (strcasecmp(basename_of(s_item[i]), basename) == 0) {
            s_idx = i;
            snapshot_lib();
            ok = true;
            break;
        }
    }
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
    return ok;
}

void enc_fn_activate(void)
{
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
    if (s_n <= 0) {
        rescan();
    }
    play_sel();
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
}

void enc_fn_set_nav_hooks(bool (*on_rotate)(int delta), bool (*on_click)(void), bool (*on_long)(void))
{
    s_nav_rotate = on_rotate;
    s_nav_click = on_click;
    s_nav_long = on_long;
}

static TickType_t s_accel_last_tick;
static int s_accel_have_last;

static int wrap_idx(int idx, int n)
{
    if (n <= 0) {
        return 0;
    }
    idx %= n;
    if (idx < 0) {
        idx += n;
    }
    return idx;
}

int enc_fn_accel_delta(int raw_delta)
{
    if (!raw_delta) {
        return 0;
    }
    TickType_t now = xTaskGetTickCount();
    int pulses = raw_delta > 0 ? raw_delta : -raw_delta;
    int sign = raw_delta > 0 ? 1 : -1;
    uint32_t dt_ms = 100; /* default: slow */

    if (s_accel_have_last) {
        uint32_t elapsed = (uint32_t)(now - s_accel_last_tick) * portTICK_PERIOD_MS;
        /* Idle gap → treat next turn as slow (one step), not leftover speed. */
        if (elapsed > 200u) {
            dt_ms = 100;
        } else {
            dt_ms = elapsed / (uint32_t)(pulses > 0 ? pulses : 1);
            if (dt_ms == 0) {
                dt_ms = 1;
            }
        }
    }
    s_accel_last_tick = now;
    s_accel_have_last = 1;

    int mult = 1;
    if (dt_ms <= 20u) {
        mult = 20; /* 暴走 */
    } else if (dt_ms <= 50u) {
        mult = 5;
    } else {
        mult = 1;
    }
    return sign * pulses * mult;
}

void enc_fn_rotate(int delta)
{
    if (!delta) {
        return;
    }
    if (s_nav_rotate && s_nav_rotate(delta)) {
        return;
    }
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
    if (s_n <= 0) {
        rescan();
    }
    if (s_n <= 0) {
        ESP_LOGW(TAG, "ENC1 %s list empty", enc_fn_mode_name());
        if (s_mu) {
            xSemaphoreGive(s_mu);
        }
        return;
    }
    int d = enc_fn_accel_delta(delta);
    if (d > s_n) {
        d = s_n;
    } else if (d < -s_n) {
        d = -s_n;
    }
    s_idx = wrap_idx(s_idx + d, s_n);
    ESP_LOGD(TAG, "ENC1 browse %s [%d/%d] raw=%d step=%d %s", enc_fn_mode_name(), s_idx + 1, s_n,
             delta, d, basename_of(sel()));
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
}

void enc_fn_held_rotate(int delta)
{
    if (!delta) {
        return;
    }
    int step = delta > 0 ? 10 : -10;
    int n = delta > 0 ? delta : -delta;
    int pct = score_tempo_pct();
    for (int i = 0; i < n; i++) {
        pct += step;
    }
    score_set_tempo_pct(pct);
    sd_music_set_tempo_pct(score_tempo_pct());
    ESP_LOGI(TAG, "tempo x%d.%d", score_tempo_pct() / 100, (score_tempo_pct() % 100) / 10);
}

void enc_fn_click(void)
{
    if (s_nav_click && s_nav_click()) {
        return;
    }
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
    if (s_n <= 0) {
        rescan();
    }
    play_sel();
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
}

void enc_fn_long(void)
{
    if (s_nav_long && s_nav_long()) {
        return;
    }
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
    stop_engines();
    s_mode = (s_mode + 1) % ENC_FN_MODE_COUNT;
    rescan();
    ESP_LOGI(TAG, "ENC1 mode %s n=%d sel=%s", enc_fn_mode_name(), s_n, basename_of(sel()));
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
}

void enc_fn_set_mode(enc_fn_mode_t mode)
{
    if (mode < 0 || mode >= ENC_FN_MODE_COUNT) {
        return;
    }
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
    stop_engines();
    s_mode = (int)mode;
    rescan();
    ESP_LOGI(TAG, "ENC1 set mode %s n=%d", enc_fn_mode_name(), s_n);
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
}

void enc_fn_dump(void)
{
    int prev = (s_n > 0) ? (s_idx + s_n - 1) % s_n : -1;
    int next = (s_n > 0) ? (s_idx + 1) % s_n : -1;
    nk_printf("enc1_mode=%s\n", enc_fn_mode_name());
    nk_printf("enc1_idx=%d\n", s_n ? s_idx + 1 : 0);
    nk_printf("enc1_n=%d\n", s_n);
    nk_printf("enc1_sel=%s\n", basename_of(sel()));
    nk_printf("enc1_prev=%s\n", prev >= 0 ? basename_of(s_item[prev]) : "-");
    nk_printf("enc1_next=%s\n", next >= 0 ? basename_of(s_item[next]) : "-");
    nk_printf("enc1_stub=0\n");
    nk_printf("enc1_loop=%d\n", s_loop);
    {
        int need = 0;
        if (s_mode == ENC_FN_MUSIC) {
            need = !same_basename(sd_music_path(), sel());
        } else if (s_mode == ENC_FN_SCORE) {
            need = !same_basename(score_current_path(), sel());
        }
        nk_printf("enc1_need_ok=%d\n", need);
    }
    nk_printf("lib_mn=%d\n", s_lib_mn);
    nk_printf("lib_moff=%d\n", s_lib_moff);
    nk_printf("lib_sn=%d\n", s_lib_sn);
    nk_printf("lib_soff=%d\n", s_lib_soff);
    static int s_lib_div = 7;
    if (++s_lib_div >= 8) {
        s_lib_div = 0;
        snapshot_lib();
        int nm = s_lib_mn - s_lib_moff;
        if (nm > LIB_WIN) {
            nm = LIB_WIN;
        }
        for (int i = 0; i < nm; ++i) {
            nk_printf("lib_m%d=%s\n", i, s_lib_m[i]);
        }
        int ns = s_lib_sn - s_lib_soff;
        if (ns > LIB_WIN) {
            ns = LIB_WIN;
        }
        for (int i = 0; i < ns; ++i) {
            nk_printf("lib_s%d=%s\n", i, s_lib_s[i]);
        }
    }
}

void enc_fn_cmd(const char *args)
{
    if (!args) {
        return;
    }
    while (*args == ' ') {
        args++;
    }
    if (strcmp(args, "+") == 0) {
        enc_fn_rotate(1);
        return;
    }
    if (strcmp(args, "-") == 0) {
        enc_fn_rotate(-1);
        return;
    }
    if (strcmp(args, "click") == 0) {
        enc_fn_click();
        return;
    }
    if (strcmp(args, "long") == 0) {
        enc_fn_long();
        return;
    }
    ESP_LOGI(TAG, "enc1 +|-|click|long");
}

/* Soft-switch to music library (no stop_engines) and advance past last played. */
static void ensure_music_list_unlocked(void)
{
    if (s_mode != ENC_FN_MUSIC) {
        s_mode = ENC_FN_MUSIC;
        rescan();
        if (s_last_music_bn[0]) {
            for (int i = 0; i < s_n; ++i) {
                if (strcasecmp(basename_of(s_item[i]), s_last_music_bn) == 0) {
                    s_idx = i;
                    break;
                }
            }
        }
    }
}

static void queue_music_next(void)
{
    if (!s_loop) {
        return;
    }
    ensure_music_list_unlocked();
    if (s_n <= 0) {
        ESP_LOGW(TAG, "auto-next: music list empty");
        return;
    }
    s_idx = (s_idx + 1) % s_n;
    ESP_LOGI(TAG, "ENC1 music next [%d/%d] %s", s_idx + 1, s_n, basename_of(sel()));
    play_music();
}

/* Called on music task at EOF — return next full path, or -1. */
static int provide_music_next(char *out, size_t out_sz)
{
    if (!out || out_sz < 2 || !s_loop) {
        return -1;
    }
    if (s_mu && xSemaphoreTake(s_mu, pdMS_TO_TICKS(50)) != pdTRUE) {
        return -1;
    }
    ensure_music_list_unlocked();
    int ok = -1;
    if (s_n > 0) {
        s_idx = (s_idx + 1) % s_n;
        const char *p = sel();
        if (p[0] && p[0] != '-') {
            strncpy(out, p, out_sz - 1);
            out[out_sz - 1] = 0;
            strncpy(s_last_music_bn, basename_of(p), sizeof(s_last_music_bn) - 1);
            s_last_music_bn[sizeof(s_last_music_bn) - 1] = 0;
            s_armed = 1;
            ok = 0;
            ESP_LOGI(TAG, "ENC1 music next [%d/%d] %s", s_idx + 1, s_n, basename_of(p));
        }
    }
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
    return ok;
}

static void auto_next(void)
{
    /* Fallback if music-task provider could not take the mutex. */
    if (sd_music_take_ended()) {
        if (s_loop) {
            queue_music_next();
        }
        return;
    }
    if (!s_loop || s_n <= 0) {
        (void)score_take_ended();
        return;
    }
    if (s_mode == ENC_FN_SCORE && score_take_ended()) {
        s_idx = (s_idx + 1) % s_n;
        ESP_LOGI(TAG, "ENC1 score next [%d/%d] %s", s_idx + 1, s_n, basename_of(sel()));
        play_score_now();
    }
}

static void enc_auto_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (s_mu) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
        }
        auto_next();
        if (s_mode == ENC_FN_RECORD) {
            char saved[160];
            if (drill_take_saved(saved, sizeof(saved))) {
                rescan();
                for (int i = 0; i < s_n; ++i) {
                    if (strcasecmp(s_item[i], saved) == 0) {
                        s_idx = i;
                        break;
                    }
                }
                ESP_LOGI(TAG, "record indexed %s", basename_of(saved));
                if (s_list_changed_cb) {
                    s_list_changed_cb();
                }
            }
        }
        if (s_mu) {
            xSemaphoreGive(s_mu);
        }
    }
}

void enc_fn_on_key(int note_id, int down)
{
    drill_on_key(note_id, down);
}

esp_err_t enc_fn_start(void)
{
    s_cap = MAX_INDEX;
    s_item = heap_caps_malloc((size_t)s_cap * ENC_NAME_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_item) {
        ESP_LOGE(TAG, "no PSRAM for lib index");
        return ESP_ERR_NO_MEM;
    }
    s_mu = xSemaphoreCreateMutex();
    s_mode = ENC_FN_MUSIC;
    rescan();
    ESP_LOGI(TAG, "music index n=%d", s_n);
    s_mode = ENC_FN_SCORE;
    rescan();
    ESP_LOGI(TAG, "score index n=%d (cap=%d)", s_n, s_cap);
    s_mode = ENC_FN_PLAY;
    rescan();
    sd_music_set_next_provider(provide_music_next);
    xTaskCreatePinnedToCore(enc_auto_task, "enc_auto", 8192, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "ENC1 browse/click/long; index cap=%d loop=%d", s_cap, s_loop);
    return ESP_OK;
}
