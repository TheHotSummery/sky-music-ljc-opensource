#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Product nav: ENC1 long-press cycles modes. ENC1 hold+rotate=tempo.
 * ENC2 rotate=volume, hold+rotate=transpose.
 * Modes: play | music | score | follow | record
 * follow waits for the lit keys; record writes a score. See docs/UI_NAV.md */

typedef enum {
    ENC_FN_PLAY = 0,
    ENC_FN_MUSIC,
    ENC_FN_SCORE,
    ENC_FN_FOLLOW,
    ENC_FN_RECORD,
    ENC_FN_MODE_COUNT,
} enc_fn_mode_t;

esp_err_t enc_fn_start(void);
/* Optional UI nav hooks (Home/Library). Return true = consumed. */
void enc_fn_set_nav_hooks(bool (*on_rotate)(int delta), bool (*on_click)(void),
                          bool (*on_long)(void));
void enc_fn_rotate(int delta);
void enc_fn_held_rotate(int delta); /* playback tempo, score and music */
/*
 * Map raw encoder detents → list steps by inter-pulse Δt (common AV/car-HUD style):
 *   Δt > 50ms → ×1,  20–50ms → ×5,  ≤20ms → ×20.  No coast after release.
 */
int enc_fn_accel_delta(int raw_delta);
void enc_fn_click(void);
void enc_fn_long(void);
void enc_fn_dump(void);
void enc_fn_cmd(const char *args);

enc_fn_mode_t enc_fn_mode(void);
const char *enc_fn_mode_name(void);
void enc_fn_set_mode(enc_fn_mode_t mode); /* jump mode + rescan (Library) */
int enc_fn_list_n(void);
int enc_fn_list_idx(void); /* 0-based, -1 if empty */
const char *enc_fn_list_name(int idx); /* basename; "" if OOB */
const char *enc_fn_list_path(int idx); /* full path; "" if OOB */
bool enc_fn_select_idx(int idx); /* 0-based; false if empty/OOB */
bool enc_fn_select_name(const char *basename); /* rescan + select by basename */
void enc_fn_rescan_current(void);
void enc_fn_set_list_changed_cb(void (*cb)(void));
/* Activate current selection (same as ENC1 short press, ignoring UI nav hooks). */
void enc_fn_activate(void);
void enc_fn_on_key(int note_id, int down);
/* One-shot UI tip after failed open/play ("" if none). */
const char *enc_fn_take_tip(void);

#ifdef __cplusplus
}
#endif
