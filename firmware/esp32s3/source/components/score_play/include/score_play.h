#pragma once
#include "esp_err.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t score_play_start(void);
void score_cmd_list(int offset, int count);
esp_err_t score_cmd_open(const char *path);
void score_cmd_play(void);
void score_cmd_pause(void);
void score_cmd_stop(void);
void score_dump_status(void);
void score_get_runtime(int *n, int *idx, int *playing, int *paused);
void score_get_progress(uint32_t *ms, uint32_t *dur_ms, unsigned *pct, int *playing, int *paused);
const char *score_current_path(void);
int score_take_ended(void); /* 1 if last run reached the last note */
int score_is_opening(void);
int score_note_count(void);
int score_note_get(int i, uint32_t *t_ms, uint8_t *key_1based);
void score_set_note_tap(void (*fn)(uint8_t note_id, int down)); /* 0..14, HID/MIDI during auto-play */
void score_set_tempo_pct(int pct); /* 50..200, 100 = written time */
int score_tempo_pct(void);
#ifdef __cplusplus
}
#endif
