#include "drill.h"

#include "board_pins.h"
#include "rgb_led.h"
#include "score_play.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "drill";

#define MAX_NOTES 24000

typedef struct {
    uint32_t t;
    uint8_t k; /* 1..15 */
} Note;

enum {
    ST_OFF = 0,
    ST_OPEN,
    ST_FOLLOW,
    ST_DONE,
    ST_REC,
    ST_SAVE,
};

static Note *s_notes;
static int s_n;
static int s_at;
static uint16_t s_need;
static uint16_t s_got;
static int s_hit;
static int s_miss;
static int s_state;
static int s_gen;
static int s_open_gen;
static int64_t s_t0;
static uint16_t s_flash;
static int64_t s_flash_until;
static char s_target[160];
static char s_save_as[160];
static char s_saved[160];
static volatile int s_saved_ready;
static TaskHandle_t s_task;

static int is_new_name(const char *p)
{
    return p && strcmp(p, DRILL_NEW_NAME) == 0;
}

static void lights(uint16_t hold, uint16_t flash)
{
    rgb_led_hint(hold, flash);
}

static void load_chord(void)
{
    s_need = 0;
    s_got = 0;
    if (s_at >= s_n) {
        return;
    }
    uint32_t t = s_notes[s_at].t;
    for (int i = s_at; i < s_n && s_notes[i].t == t; ++i) {
        if (s_notes[i].k >= 1 && s_notes[i].k <= 15) {
            s_need |= (uint16_t)(1u << (s_notes[i].k - 1));
        }
    }
    lights(s_need, 0);
}

static void finish_follow(void)
{
    s_state = ST_DONE;
    s_need = 0;
    s_got = 0;
    lights(0, 0);
    ESP_LOGI(TAG, "follow done hit=%d miss=%d", s_hit, s_miss);
}

static void advance_chord(void)
{
    if (s_at >= s_n) {
        finish_follow();
        return;
    }
    uint32_t t = s_notes[s_at].t;
    while (s_at < s_n && s_notes[s_at].t == t) {
        s_at++;
    }
    s_hit++;
    if (s_at >= s_n) {
        finish_follow();
    } else {
        load_chord();
        ESP_LOGI(TAG, "follow hit %d/%d", s_at, s_n);
    }
}

static int copy_open_score(void)
{
    int n = score_note_count();
    if (n <= 0) {
        return 0;
    }
    if (n > MAX_NOTES) {
        n = MAX_NOTES;
    }
    for (int i = 0; i < n; ++i) {
        uint32_t t = 0;
        uint8_t k = 0;
        if (!score_note_get(i, &t, &k)) {
            n = i;
            break;
        }
        s_notes[i].t = t;
        s_notes[i].k = k;
    }
    s_n = n;
    s_at = 0;
    s_hit = 0;
    s_miss = 0;
    return n > 0;
}

static int write_score(const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return 0;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    fprintf(fp, "{\"name\":\"%s\",\"songNotes\":[", base);
    for (int i = 0; i < s_n; ++i) {
        unsigned key0 = s_notes[i].k ? (unsigned)(s_notes[i].k - 1) : 0;
        fprintf(fp, "%s{\"time\":%u,\"key\":\"Key%u\"}", i ? "," : "", (unsigned)s_notes[i].t, key0);
    }
    fprintf(fp, "]}");
    fclose(fp);
    return 1;
}

static void service(void)
{
    int64_t now = esp_timer_get_time();
    if (s_flash && now >= s_flash_until) {
        s_flash = 0;
        if (s_state == ST_FOLLOW) {
            lights(s_need & (uint16_t)~s_got, 0);
        }
    }

    if (s_state == ST_OPEN && !score_is_opening()) {
        int gen = s_open_gen;
        if (gen != s_gen) {
            return;
        }
        if (copy_open_score()) {
            s_state = ST_FOLLOW;
            load_chord();
            ESP_LOGI(TAG, "follow start notes=%d", s_n);
        } else {
            s_state = ST_OFF;
            lights(0, 0);
            ESP_LOGW(TAG, "follow open empty");
        }
    }

    if (s_state == ST_SAVE) {
        if (s_n <= 0) {
            s_state = ST_OFF;
            ESP_LOGW(TAG, "record empty, not saved");
            return;
        }
        mkdir(SD_MOUNT_POINT "/puzi", 0775);
        if (write_score(s_save_as)) {
            strncpy(s_saved, s_save_as, sizeof(s_saved) - 1);
            s_saved[sizeof(s_saved) - 1] = 0;
            s_saved_ready = 1;
            ESP_LOGI(TAG, "record saved %s notes=%d", s_saved, s_n);
        } else {
            ESP_LOGW(TAG, "record save failed %s", s_save_as);
        }
        s_state = ST_OFF;
        s_n = 0;
    }
}

static void drill_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(40));
        service();
    }
}

void drill_start(void)
{
    if (s_notes) {
        return;
    }
    s_notes = heap_caps_calloc(MAX_NOTES, sizeof(Note), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_notes) {
        ESP_LOGE(TAG, "no PSRAM");
        return;
    }
    xTaskCreatePinnedToCore(drill_task, "drill", 4096, NULL, 3, &s_task, 0);
    ESP_LOGI(TAG, "follow/record ready");
}

void drill_abort(void)
{
    s_gen++;
    if (s_state == ST_SAVE) {
        return;
    }
    s_state = ST_OFF;
    s_need = 0;
    s_got = 0;
    s_flash = 0;
    s_n = 0;
    lights(0, 0);
}

void drill_on_key(int note_id, int down)
{
    if (!down || note_id < 0 || note_id > 14 || !s_notes) {
        return;
    }
    uint16_t bit = (uint16_t)(1u << note_id);
    if (s_state == ST_REC) {
        if (s_n >= MAX_NOTES) {
            return;
        }
        uint32_t ms = (uint32_t)((esp_timer_get_time() - s_t0) / 1000);
        s_notes[s_n].t = ms;
        s_notes[s_n].k = (uint8_t)(note_id + 1);
        s_n++;
        return;
    }
    if (s_state != ST_FOLLOW) {
        return;
    }
    if (s_need & bit) {
        if (s_got & bit) {
            return;
        }
        s_got |= bit;
        if ((s_got & s_need) == s_need) {
            advance_chord();
        } else {
            lights(s_need & (uint16_t)~s_got, 0);
        }
        return;
    }
    s_miss++;
    s_flash = bit;
    s_flash_until = esp_timer_get_time() + 200000;
    lights(s_need & (uint16_t)~s_got, bit);
    ESP_LOGI(TAG, "follow miss key=%d", note_id + 1);
}

esp_err_t drill_follow_start(const char *score_path)
{
    if (!s_notes) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state == ST_OPEN || s_state == ST_FOLLOW) {
        return ESP_OK; /* already running */
    }
    if (s_state == ST_DONE) {
        drill_abort();
    }
    if (s_state != ST_OFF || !score_path || !score_path[0] || is_new_name(score_path)) {
        return ESP_ERR_INVALID_ARG;
    }
    score_cmd_stop();
    if (score_cmd_open(score_path) != ESP_OK) {
        ESP_LOGW(TAG, "follow open busy");
        return ESP_ERR_INVALID_STATE;
    }
    s_open_gen = s_gen;
    s_state = ST_OPEN;
    s_hit = s_miss = s_at = s_n = 0;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
    return ESP_OK;
}

void drill_follow_stop(void)
{
    if (s_state == ST_OFF) {
        return;
    }
    drill_abort();
    ESP_LOGI(TAG, "follow stop");
}

void drill_follow_toggle(const char *score_path)
{
    if (!s_notes) {
        return;
    }
    if (s_state == ST_OPEN || s_state == ST_FOLLOW || s_state == ST_DONE) {
        drill_follow_stop();
        return;
    }
    (void)drill_follow_start(score_path);
}

void drill_record_toggle(const char *sel_path)
{
    if (!s_notes) {
        return;
    }
    if (s_state == ST_REC) {
        if (s_target[0]) {
            strncpy(s_save_as, s_target, sizeof(s_save_as) - 1);
            s_save_as[sizeof(s_save_as) - 1] = 0;
        } else {
            snprintf(s_save_as, sizeof(s_save_as), "%s/puzi/rec%lu.txt", SD_MOUNT_POINT,
                     (unsigned long)(esp_timer_get_time() / 1000));
        }
        s_state = ST_SAVE;
        if (s_task) {
            xTaskNotifyGive(s_task);
        }
        ESP_LOGI(TAG, "record stop notes=%d", s_n);
        return;
    }
    if (s_state != ST_OFF) {
        return;
    }
    s_n = 0;
    s_target[0] = 0;
    if (sel_path && sel_path[0] && !is_new_name(sel_path)) {
        strncpy(s_target, sel_path, sizeof(s_target) - 1);
        s_target[sizeof(s_target) - 1] = 0;
    }
    s_t0 = esp_timer_get_time();
    s_state = ST_REC;
    lights(0, 0);
    ESP_LOGI(TAG, "record start");
}

void drill_view(drill_view_t *v)
{
    if (!v) {
        return;
    }
    memset(v, 0, sizeof(*v));
    if (s_state == ST_OPEN || s_state == ST_FOLLOW) {
        v->follow = 1;
    } else if (s_state == ST_DONE) {
        v->follow = 2;
    }
    if (s_state == ST_REC) {
        v->rec = 1;
        v->rec_ms = (uint32_t)((esp_timer_get_time() - s_t0) / 1000);
        v->rec_n = s_n;
    } else if (s_state == ST_SAVE) {
        v->rec = 2;
        v->rec_n = s_n;
    }
    v->hit = s_hit;
    v->miss = s_miss;
    v->at = s_at;
    v->total = s_n;
    v->expect = (uint16_t)(s_need & (uint16_t)~s_got);
    v->upcoming = 0;
    if (s_state == ST_FOLLOW && s_notes && s_at < s_n) {
        uint32_t t0 = s_notes[s_at].t;
        int i = s_at;
        while (i < s_n && s_notes[i].t == t0) {
            i++;
        }
        if (i < s_n) {
            uint32_t t1 = s_notes[i].t;
            for (; i < s_n && s_notes[i].t == t1; ++i) {
                if (s_notes[i].k >= 1 && s_notes[i].k <= 15) {
                    v->upcoming |= (uint16_t)(1u << (s_notes[i].k - 1));
                }
            }
        }
    }
}

int drill_timeline(drill_fall_t *out, int max_n)
{
    if (!out || max_n <= 0 || !s_notes || s_state != ST_FOLLOW || s_at >= s_n) {
        return 0;
    }
    int n = 0;
    uint32_t t_cur = s_notes[s_at].t;
    for (int i = s_at; i < s_n && n < max_n; ++i) {
        if (s_notes[i].k < 1 || s_notes[i].k > 15) {
            continue;
        }
        out[n].key = s_notes[i].k;
        out[n].t_ms = s_notes[i].t;
        out[n].on_judge = (s_notes[i].t == t_cur) ? 1 : 0;
        n++;
    }
    return n;
}

int drill_take_saved(char *path, size_t n)
{
    if (!s_saved_ready || !path || n == 0) {
        return 0;
    }
    s_saved_ready = 0;
    strncpy(path, s_saved, n - 1);
    path[n - 1] = 0;
    return path[0] != 0;
}
