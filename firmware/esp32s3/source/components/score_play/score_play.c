#include "nk_out.h"
#include "score_play.h"

#include "board_pins.h"
#include "mixer.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *TAG = "score";

#define JOB_LIST 1
#define JOB_OPEN 2

typedef struct {
    uint8_t op;
    int16_t off;
    int16_t count;
    char path[480];
} ScoreJob;

static void score_do_list(int offset, int count);
static esp_err_t score_do_open(const char *path);

#define EVT_PLAY  (1u << 0)
#define EVT_STOP  (1u << 1)
#define MAX_NOTES 24000
#define BLE_TAP_MIN_MS 55
#define BLE_TAP_SPAN_MS 76 /* 55..130 */

static uint16_t s_ble_held;
static uint32_t s_ble_until[15];

typedef struct {
    uint32_t t;
    uint8_t k; /* 1..15 */
} ScoreNote;

static ScoreNote *s_notes;
static int s_n;
static char s_title[192];
static char s_path[512];
static EventGroupHandle_t s_ev;
static QueueHandle_t s_jobs;
static volatile int s_listing;
static volatile int s_list_pend_off = -1;
static volatile int s_list_pend_n;
static volatile int s_pause;
static volatile int s_idx;
static volatile int s_playing;
static volatile int s_ended;
static volatile int s_opening;
static volatile int s_play_when_ready;
static void (*s_note_tap)(uint8_t note_id, int down);
static volatile int s_tempo_pct = 100; /* 50..200 */

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

static int ends_with(const char *n, const char *ext)
{
    size_t a = strlen(n), b = strlen(ext);
    if (a < b) {
        return 0;
    }
    return strcasecmp(n + a - b, ext) == 0;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void pct_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '%' && hex_nibble(r[1]) >= 0 && hex_nibble(r[2]) >= 0) {
            *w++ = (char)((hex_nibble(r[1]) << 4) | hex_nibble(r[2]));
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static void print_pct(const char *s)
{
    char buf[512];
    size_t n = 0;
    while (*s && n + 4 < sizeof(buf)) {
        unsigned char c = (unsigned char)*s++;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '/' || c == '.' || c == '-' || c == '_' ) {
            buf[n++] = (char)c;
        } else {
            n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%%%02X", c);
        }
    }
    if (n) {
        nk_write(buf, n);
    }
}

static int is_score_name(const char *n)
{
    return ends_with(n, ".txt") || ends_with(n, ".json") || ends_with(n, ".sky");
}

static char *load_text_psram(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(fp);
        return NULL;
    }
    uint8_t *raw = heap_caps_malloc((size_t)sz + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw) {
        fclose(fp);
        return NULL;
    }
    if (fread(raw, 1, (size_t)sz, fp) != (size_t)sz) {
        heap_caps_free(raw);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    raw[sz] = 0;
    raw[sz + 1] = 0;

    if (sz >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        size_t u16n = ((size_t)sz - 2) / 2;
        char *utf8 = heap_caps_malloc(u16n * 3 + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!utf8) {
            heap_caps_free(raw);
            return NULL;
        }
        const uint16_t *w = (const uint16_t *)(raw + 2);
        size_t o = 0;
        for (size_t i = 0; i < u16n; ++i) {
            uint16_t c = w[i];
            if (c < 0x80) {
                utf8[o++] = (char)c;
            } else if (c < 0x800) {
                utf8[o++] = (char)(0xC0 | (c >> 6));
                utf8[o++] = (char)(0x80 | (c & 0x3F));
            } else {
                utf8[o++] = (char)(0xE0 | (c >> 12));
                utf8[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
                utf8[o++] = (char)(0x80 | (c & 0x3F));
            }
        }
        utf8[o] = 0;
        heap_caps_free(raw);
        if (out_len) {
            *out_len = o;
        }
        return utf8;
    }
    if (out_len) {
        *out_len = (size_t)sz;
    }
    return (char *)raw;
}

static int parse_sky(const char *s)
{
    s_n = 0;
    s_title[0] = 0;
    const char *nm = strstr(s, "\"name\"");
    if (nm) {
        nm = strchr(nm + 6, '"');
        if (nm) {
            nm++;
            size_t o = 0;
            const char *q = nm;
            while (*q && *q != '"' && o + 4 < sizeof(s_title)) {
                if (q[0] == '\\' && q[1] == 'u' && hex_nibble(q[2]) >= 0) {
                    unsigned code = 0;
                    int ok = 1;
                    for (int k = 0; k < 4; ++k) {
                        int n = hex_nibble(q[2 + k]);
                        if (n < 0) {
                            ok = 0;
                            break;
                        }
                        code = (code << 4) | (unsigned)n;
                    }
                    if (ok) {
                        if (code < 0x80) {
                            s_title[o++] = (char)code;
                        } else if (code < 0x800) {
                            s_title[o++] = (char)(0xC0 | (code >> 6));
                            s_title[o++] = (char)(0x80 | (code & 0x3F));
                        } else {
                            s_title[o++] = (char)(0xE0 | (code >> 12));
                            s_title[o++] = (char)(0x80 | ((code >> 6) & 0x3F));
                            s_title[o++] = (char)(0x80 | (code & 0x3F));
                        }
                        q += 6;
                        continue;
                    }
                }
                s_title[o++] = *q++;
            }
            s_title[o] = 0;
        }
    }

    const char *p = s;
    while ((p = strstr(p, "\"time\"")) != NULL) {
        p += 6;
        while (*p && *p != ':') {
            p++;
        }
        if (!*p) {
            break;
        }
        char *end = NULL;
        unsigned long t = strtoul(p + 1, &end, 10);
        if (!end) {
            p++;
            continue;
        }
        const char *k = strstr(end, "\"key\"");
        if (!k || k > end + 96) {
            p = end;
            continue;
        }
        k = strstr(k, "Key");
        if (!k) {
            p = end;
            continue;
        }
        int idx = atoi(k + 3);
        if (idx < 0 || idx > 14) {
            p = k + 3;
            continue;
        }
        if (s_n > 0 && s_notes[s_n - 1].t == (uint32_t)t && s_notes[s_n - 1].k == (uint8_t)(idx + 1)) {
            p = k + 3;
            continue;
        }
        if (s_n >= MAX_NOTES) {
            break;
        }
        s_notes[s_n].t = (uint32_t)t;
        s_notes[s_n].k = (uint8_t)(idx + 1);
        s_n++;
        p = k + 3;
    }
    return s_n;
}

static void emit_tap(uint8_t id0, int down)
{
    if (s_note_tap) {
        s_note_tap(id0, down);
    }
}

static void ble_tap_release_all(void)
{
    for (int i = 0; i < 15; i++) {
        if (s_ble_held & (1u << i)) {
            emit_tap((uint8_t)i, 0);
        }
    }
    s_ble_held = 0;
    memset(s_ble_until, 0, sizeof(s_ble_until));
}

static void ble_tap_poll(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    for (int i = 0; i < 15; i++) {
        if ((s_ble_held & (1u << i)) == 0) {
            continue;
        }
        if ((int32_t)(now - s_ble_until[i]) >= 0) {
            emit_tap((uint8_t)i, 0);
            s_ble_held &= (uint16_t) ~(1u << i);
            s_ble_until[i] = 0;
        }
    }
}

static uint32_t tap_hold_ms(uint8_t one_based)
{
    uint32_t hold = BLE_TAP_MIN_MS + (esp_random() % BLE_TAP_SPAN_MS);
    uint32_t t0 = s_notes[s_idx].t;
    for (int i = s_idx + 1; i < s_n; i++) {
        if (s_notes[i].k != one_based) {
            continue;
        }
        uint32_t gap = s_notes[i].t - t0;
        int tempo = s_tempo_pct > 0 ? s_tempo_pct : 100;
        gap = (uint32_t)((uint64_t)gap * 100u / (uint32_t)tempo);
        if (gap > 12 && hold + 8 > gap) {
            hold = gap - 8;
        }
        break;
    }
    if (hold < 25) {
        hold = 25;
    }
    return hold;
}

static void ble_tap_down(uint8_t id0)
{
    if (id0 > 14) {
        return;
    }
    if (s_ble_held & (1u << id0)) {
        emit_tap(id0, 0);
    }
    emit_tap(id0, 1);
    s_ble_held |= (uint16_t)(1u << id0);
    s_ble_until[id0] = (uint32_t)(esp_timer_get_time() / 1000) + tap_hold_ms((uint8_t)(id0 + 1));
}

static void fire_note(uint8_t one_based)
{
    uint8_t id0 = (uint8_t)(one_based - 1);
    MixCmd cmd = {
        .type = MIX_CMD_NOTE_ON,
        .note_id = id0,
        .velocity = 127,
    };
    mixer_submit(&cmd);
    ble_tap_down(id0);
}

static void play_task(void *arg)
{
    (void)arg;
    for (;;) {
        xEventGroupWaitBits(s_ev, EVT_PLAY, pdTRUE, pdFALSE, portMAX_DELAY);
        xEventGroupClearBits(s_ev, EVT_STOP);
        s_playing = 1;
        s_pause = 0;
        s_idx = 0;
        int tempo_applied = clamp_tempo(s_tempo_pct);
        int64_t t0 = esp_timer_get_time();
        while (s_idx < s_n) {
            EventBits_t bits = xEventGroupGetBits(s_ev);
            if (bits & EVT_STOP) {
                xEventGroupClearBits(s_ev, EVT_STOP);
                break;
            }
            int tempo = clamp_tempo(s_tempo_pct);
            if (tempo != tempo_applied) {
                int64_t now_us = esp_timer_get_time();
                int64_t score_us = (now_us - t0) * tempo_applied / 100;
                t0 = now_us - score_us * 100 / tempo;
                tempo_applied = tempo;
            }
            if (s_pause) {
                ble_tap_release_all();
                vTaskDelay(pdMS_TO_TICKS(20));
                t0 = esp_timer_get_time() - (int64_t)s_notes[s_idx].t * 1000 * 100 / tempo_applied;
                continue;
            }
            ble_tap_poll();
            int64_t target = t0 + (int64_t)s_notes[s_idx].t * 1000 * 100 / tempo_applied;
            int64_t now = esp_timer_get_time();
            if (target - now > 2000) {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            uint32_t same = s_notes[s_idx].t;
            while (s_idx < s_n && s_notes[s_idx].t == same) {
                fire_note(s_notes[s_idx].k);
                s_idx++;
            }
        }
        ble_tap_release_all();
        s_playing = 0;
        if (s_idx >= s_n && s_n > 0) {
            s_ended = 1;
            ESP_LOGI(TAG, "score finished idx=%d/%d", s_idx, s_n);
        }
    }
}

static void score_io_task(void *arg)
{
    (void)arg;
    ScoreJob job;
    for (;;) {
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job.op == JOB_LIST) {
            score_do_list(job.off, job.count);
        } else if (job.op == JOB_OPEN) {
            score_do_open(job.path);
        }
    }
}

void score_cmd_list(int offset, int count)
{
    if (!s_jobs) {
        return;
    }
    if (s_listing) {
        s_list_pend_off = offset;
        s_list_pend_n = count;
        return;
    }
    ScoreJob job = {
        .op = JOB_LIST,
        .off = (int16_t)offset,
        .count = (int16_t)count,
        .path = {0},
    };
    s_listing = 1;
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        s_listing = 0;
        ESP_LOGW(TAG, "list busy");
    }
}

static void score_do_list(int offset, int count)
{
    if (offset < 0) {
        offset = 0;
    }
    if (count <= 0 || count > 500) {
        count = 80;
    }
    nk_printf("BEGIN_SCORES\n");
    const char *dirs[] = { SD_MOUNT_POINT "/puzi", SD_MOUNT_POINT };
    int total = 0;
    int shown = 0;
    for (int d = 0; d < 2; ++d) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) {
            continue;
        }
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.' && (ent->d_name[1] == 0 || ent->d_name[1] == '.')) {
                continue;
            }
            if (!is_score_name(ent->d_name)) {
                continue;
            }
            if (total >= offset && shown < count) {
                nk_printf("file=%s/%s bytes=0\n", dirs[d], ent->d_name);
                shown++;
            }
            total++;
        }
        closedir(dir);
        if (total > 0 && d == 0) {
            break;
        }
    }
    nk_printf("offset=%d shown=%d total=%d\n", offset, shown, total);
    nk_printf("END_SCORES\n");
    s_listing = 0;
    if (s_list_pend_off >= 0) {
        int off = s_list_pend_off;
        int n = s_list_pend_n;
        s_list_pend_off = -1;
        score_cmd_list(off, n);
    }
}

esp_err_t score_cmd_open(const char *path)
{
    if (!path || !path[0] || !s_jobs) {
        return ESP_ERR_INVALID_ARG;
    }
    ScoreJob job = {
        .op = JOB_OPEN,
        .off = 0,
        .count = 0,
        .path = {0},
    };
    strncpy(job.path, path, sizeof(job.path) - 1);
    s_opening = 1;
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        s_opening = 0;
        ESP_LOGW(TAG, "open busy");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t score_do_open(const char *path)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    score_cmd_stop();
    char full[512];
    if (path[0] == '/') {
        strncpy(full, path, sizeof(full) - 1);
        full[sizeof(full) - 1] = 0;
    } else {
        snprintf(full, sizeof(full), "%s/puzi/%s", SD_MOUNT_POINT, path);
    }
    pct_decode(full);

    size_t len = 0;
    char *text = load_text_psram(full, &len);
    if (!text) {
        ESP_LOGW(TAG, "open fail");
        s_opening = 0;
        s_play_when_ready = 0;
        nk_printf("BEGIN_SCORE_INFO\npath=-\nnotes=0\nEND_SCORE_INFO\n");
        return ESP_ERR_NOT_FOUND;
    }
    int n = parse_sky(text);
    heap_caps_free(text);
    strncpy(s_path, full, sizeof(s_path) - 1);
    s_path[sizeof(s_path) - 1] = 0;
    ESP_LOGI(TAG, "opened notes=%d bytes=%u", n, (unsigned)len);
    nk_printf("BEGIN_SCORE_INFO\n");
    nk_printf("path=");
    print_pct(s_path);
    nk_printf("\n");
    nk_printf("title=");
    print_pct(s_title[0] ? s_title : "-");
    nk_printf("\n");
    nk_printf("notes=%d\n", n);
    nk_printf("ms=%u\n", n ? (unsigned)s_notes[n - 1].t : 0);
    nk_printf("END_SCORE_INFO\n");
    s_opening = 0;
    if (s_play_when_ready) {
        s_play_when_ready = 0;
        if (n > 0) {
            score_cmd_play();
        }
    }
    return n > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

void score_cmd_play(void)
{
    /* open/list 已改成后台任务，GUI 常会立刻再发 play，此时谱还没进内存。 */
    if (s_opening) {
        s_play_when_ready = 1;
        ESP_LOGI(TAG, "play waiting for open");
        return;
    }
    if (s_n <= 0) {
        ESP_LOGW(TAG, "no score loaded");
        return;
    }
    s_play_when_ready = 0;
    s_pause = 0;
    xEventGroupClearBits(s_ev, EVT_STOP);
    if (!s_playing) {
        xEventGroupSetBits(s_ev, EVT_PLAY);
    }
}

void score_cmd_pause(void)
{
    s_pause = !s_pause;
}

void score_cmd_stop(void)
{
    s_pause = 0;
    xEventGroupSetBits(s_ev, EVT_STOP);
    MixCmd cmd = { .type = MIX_CMD_STOP_ALL };
    mixer_submit(&cmd);
}

void score_dump_status(void)
{
    nk_printf("score_path=");
    print_pct(s_path[0] ? s_path : "-");
    nk_printf("\n");
    nk_printf("score_title=");
    print_pct(s_title[0] ? s_title : "-");
    nk_printf("\n");
    nk_printf("score_notes=%d\n", s_n);
    nk_printf("score_idx=%d\n", s_idx);
    nk_printf("score_play=%d\n", s_playing);
    nk_printf("score_pause=%d\n", s_pause);
    {
        unsigned dur = (s_n > 0) ? (unsigned)s_notes[s_n - 1].t : 0;
        unsigned now = 0;
        if (s_n > 0) {
            if (s_idx >= s_n) {
                now = dur;
            } else if (s_idx > 0) {
                now = (unsigned)s_notes[s_idx].t;
            }
        }
        unsigned pct = dur ? (now * 100u / dur) : (s_n ? (unsigned)s_idx * 100u / (unsigned)s_n : 0);
        if (pct > 100) {
            pct = 100;
        }
        nk_printf("score_ms=%u\n", now);
        nk_printf("score_dur=%u\n", dur);
        nk_printf("score_pct=%u\n", pct);
    }
}

void score_get_runtime(int *n, int *idx, int *playing, int *paused)
{
    if (n) {
        *n = s_n;
    }
    if (idx) {
        *idx = s_idx;
    }
    if (playing) {
        *playing = s_playing;
    }
    if (paused) {
        *paused = s_pause;
    }
}

const char *score_current_path(void)
{
    return s_path;
}

void score_get_progress(uint32_t *ms, uint32_t *dur_ms, unsigned *pct, int *playing, int *paused)
{
    unsigned dur = (s_n > 0 && s_notes) ? (unsigned)s_notes[s_n - 1].t : 0;
    unsigned now = 0;
    if (s_n > 0 && s_notes) {
        if (s_idx >= s_n) {
            now = dur;
        } else if (s_idx > 0) {
            now = (unsigned)s_notes[s_idx].t;
        }
    }
    unsigned p = dur ? (now * 100u / dur) : (s_n ? (unsigned)s_idx * 100u / (unsigned)s_n : 0);
    if (p > 100) {
        p = 100;
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
        *playing = s_playing;
    }
    if (paused) {
        *paused = s_pause;
    }
}

int score_take_ended(void)
{
    int v = s_ended;
    s_ended = 0;
    return v;
}

int score_is_opening(void)
{
    return s_opening;
}

int score_note_count(void)
{
    return s_n;
}

int score_note_get(int i, uint32_t *t_ms, uint8_t *key_1based)
{
    if (!s_notes || i < 0 || i >= s_n) {
        return 0;
    }
    if (t_ms) {
        *t_ms = s_notes[i].t;
    }
    if (key_1based) {
        *key_1based = s_notes[i].k;
    }
    return 1;
}

void score_set_note_tap(void (*fn)(uint8_t note_id, int down))
{
    s_note_tap = fn;
}

void score_set_tempo_pct(int pct)
{
    s_tempo_pct = clamp_tempo(pct);
}

int score_tempo_pct(void)
{
    return s_tempo_pct;
}

esp_err_t score_play_start(void)
{
    s_notes = heap_caps_calloc(MAX_NOTES, sizeof(ScoreNote), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_notes) {
        return ESP_ERR_NO_MEM;
    }
    s_ev = xEventGroupCreate();
    s_jobs = xQueueCreate(3, sizeof(ScoreJob));
    if (!s_jobs) {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreatePinnedToCore(play_task, "score", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(score_io_task, "score_io", 8192, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "score player ready, table in PSRAM %d notes max", MAX_NOTES);
    return ESP_OK;
}
