#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DRILL_NEW_NAME "新录音"

typedef struct {
    int follow; /* 0 off, 1 waiting, 2 done */
    int rec;    /* 0 off, 1 recording, 2 saving */
    int hit;
    int miss;
    int at; /* notes finished, 0-based */
    int total;
    int rec_n;
    uint32_t rec_ms;
    uint16_t expect;   /* bits still needed, bit0 = key1 */
    uint16_t upcoming; /* next chord after current, 0 if none */
} drill_view_t;

void drill_start(void);
void drill_abort(void); /* mode change: drop follow and an unsaved take */
void drill_on_key(int note_id, int down);
void drill_follow_toggle(const char *score_path);
/* Start-only / stop-only (App protocol). Idempotent. */
esp_err_t drill_follow_start(const char *score_path);
void drill_follow_stop(void);
void drill_record_toggle(const char *sel_path);
void drill_view(drill_view_t *v);
/* Peek upcoming notes from current chord (for waterfall UI). Returns count. */
typedef struct {
    uint8_t key; /* 1..15 */
    uint32_t t_ms;
    int on_judge; /* 1 = current waiting chord */
} drill_fall_t;
int drill_timeline(drill_fall_t *out, int max_n);
/* 1 if a file was just written. Caller refreshes the index. */
int drill_take_saved(char *path, size_t n);

#ifdef __cplusplus
}
#endif
