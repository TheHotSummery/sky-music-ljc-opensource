#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_i2s_init(void);
esp_err_t audio_i2s_write(const int16_t *stereo_frames, size_t frame_count);
void audio_i2s_set_shift(int sh); /* 0–12 右移；量产耳机 PCM5102 默认 0 */
int audio_i2s_shift(void);

#ifdef __cplusplus
}
#endif
