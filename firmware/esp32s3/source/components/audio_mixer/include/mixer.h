#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "board_pins.h"

#define MIXER_MASTER_UI_MAX     100 /* knob 0–100 maps onto full scale (former 0–255) */
#define MIXER_MASTER_UI_DEFAULT 20

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MIX_CMD_NOTE_ON = 1,
    MIX_CMD_NOTE_OFF,
    MIX_CMD_STOP_ALL,
    MIX_CMD_SET_MASTER,
    MIX_CMD_TEST_SINE,
    MIX_CMD_SET_SAMPLE,
} MixCmdType;

typedef struct {
    MixCmdType type;
    uint8_t note_id;    /* 0..14 sample / key */
    uint8_t velocity;   /* 0..127 note vel; SET_MASTER 0..MIXER_MASTER_UI_MAX */
    const int16_t *pcm; /* MIX_CMD_SET_SAMPLE */
    uint32_t frames;
} MixCmd;

esp_err_t mixer_start(void);
bool mixer_submit(const MixCmd *cmd);
uint64_t mixer_sample_count(void);
int mixer_active_voices(void);
int mixer_polyphony(void);
int mixer_voice_cap(void);
void mixer_set_polyphony(int n);
uint16_t mixer_voice_mask(void);
uint8_t mixer_master_ui(void);
void mixer_set_transpose(int semitones); /* -12..+12, 0=C as sampled */
int mixer_transpose(void);
const char *mixer_transpose_name(void);
void mixer_hud_keys(uint8_t layers[AUDIO_BANK_WAV_COUNT], uint8_t remain_pct[AUDIO_BANK_WAV_COUNT]);
void mixer_hud_gens(uint8_t gen[AUDIO_BANK_WAV_COUNT]); /* NOTE_ON 计数，灯效连击重触发 */
uint32_t mixer_block_rate(void);
void mixer_set_sample(uint8_t note_id, const int16_t *pcm, uint32_t frames);
bool mixer_has_sample(uint8_t note_id);
void mixer_set_color(int mode); /* 0=raw, 1=violin placeholder EQ */
int mixer_color(void);
void mixer_set_hpf_hz(int hz);   /* 0 = off, typical 80–120 */
int mixer_hpf_hz(void);
void mixer_set_limit(int thresh); /* 0 = off, 20000–32767 soft clip */
int mixer_limit(void);
void mixer_set_pad_db(int db); /* 0–36，I2S 额外衰减，PCM5102 线路电平建议 12–24 */
int mixer_pad_db(void);

#define MIX_SCOPE_PTS 64
typedef struct __attribute__((packed)) {
    int32_t peak_bus;   /* mix 累加后、HPF 前，L 声道 |x| 最大 */
    int32_t peak_hpf;
    int32_t peak_lim;   /* 限幅后、乘 master 前 */
    int32_t peak_out;   /* 乘 master 后、sat16 前 */
    uint32_t mean_abs;  /* 总线 L 平均绝对值 */
    uint16_t clip_bus;  /* |bus| > 32767 的帧数 */
    uint16_t sat_out;   /* |out| > 32767 将被 sat16 的帧数 */
    uint8_t voices;
    uint8_t limited;    /* 本块触发了限幅 */
    uint16_t master;
    int16_t wave[MIX_SCOPE_PTS];     /* 输出 L，每 4 帧取峰值 */
    int16_t wave_bus[MIX_SCOPE_PTS]; /* 总线 L（超出 int16 则饱和，供看形状） */
} MixerScope;

void mixer_scope_get(MixerScope *out);

void mixer_stream_start(void);
void mixer_stream_go(void);
void mixer_stream_stop(void);
bool mixer_stream_push(const int16_t *mono, int frames);
int mixer_stream_space(void);
int mixer_stream_used(void);

#ifdef __cplusplus
}
#endif
