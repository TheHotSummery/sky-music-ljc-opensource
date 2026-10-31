#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t sd_music_start(void);
void sd_music_cmd(const char *args);
void sd_music_dump_status(void);
const char *sd_music_path(void);
void sd_music_get_progress(uint32_t *ms, uint32_t *dur_ms, unsigned *pct, int *playing);
int sd_music_paused(void); /* 1 if path loaded and paused */
int sd_music_take_ended(void); /* 1 if last play finished to EOF (auto-next) */
/** Return 0 and write next full path when auto-next should chain. Called on music task. */
void sd_music_set_next_provider(int (*fn)(char *out, size_t out_sz));
void sd_music_set_tempo_pct(int pct); /* 50..200, 100 = file rate */
int sd_music_tempo_pct(void);
void sd_music_pause_toggle(void);
#ifdef __cplusplus
}
#endif
