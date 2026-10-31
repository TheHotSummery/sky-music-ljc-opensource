#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ICS-43434 on I2S1 (pins INMP_*): bring-up / verification only.
 * Does not share clocks with PCM5102 (I2S0 TX).
 *
 * Console:
 *   mic          — status + last RMS/peak
 *   mic start    — enable RX + meter task
 *   mic stop
 *   mic tap      — log when peak crosses threshold (clap test)
 *   mic lab      — record (no save) then ENC1 Play/Pause/Stop
 *   mic lab off
 *   mic ear      — realtime monitor (headphones! ENC2=vol; key→1s mute)
 *   mic ear off
 */

typedef enum {
    MIC_LAB_OFF = 0,
    MIC_LAB_IDLE,   /* click ENC1 → record */
    MIC_LAB_REC,
    MIC_LAB_READY,  /* rotate Play/Pause/Stop, click = action */
    MIC_LAB_PLAY,
    MIC_LAB_PAUSE,
    MIC_LAB_EAR,    /* realtime listen */
} mic_lab_phase_t;

esp_err_t mic_i2s_start(void); /* register cmds; hardware starts on "mic start" */
bool mic_i2s_handle_line(const char *line);
int mic_i2s_running(void);
int mic_i2s_rms(void);
int mic_i2s_peak(void);

int mic_lab_active(void);
mic_lab_phase_t mic_lab_phase(void);
int mic_lab_sel(void); /* 0=Play 1=Pause 2=Stop */
int mic_lab_len(void); /* recorded samples */
int mic_lab_play_pos(void);
int mic_ear_howl(void); /* 1 if feedback duck active */
int mic_ear_key_mute(void); /* 1 while post-key silence */
bool mic_lab_on_rotate(int delta);
bool mic_lab_on_click(void);
bool mic_lab_on_long(void);
typedef void (*mic_lab_ui_cb_t)(int enter);
void mic_lab_set_ui_cb(mic_lab_ui_cb_t cb);

#ifdef __cplusplus
}
#endif
