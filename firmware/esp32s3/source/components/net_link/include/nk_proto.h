#pragma once
#include <stdint.h>

/* TCP 帧：A5 | len_le16(type+payload) | type | payload | xor(type..payload) */
#define NK_MAGIC 0xA5
#define NK_VER   1

#define NK_CMD    0x01 /* UTF-8 命令，无换行，与串口同一套 */
#define NK_STATUS 0x10 /* NkStatus 推送，约 10Hz */
#define NK_SCOPE  0x11 /* MixerScope，约 20Hz，示波器/峰值 */
#define NK_TEXT   0x12 /* UTF-8 文本，与串口 printf 相同（列表/完整 status） */

typedef struct __attribute__((packed)) {
    uint8_t ver;
    uint8_t flags; /* 0 sd 1 factory 2 usb 3 score_play 4 score_pause */
    uint8_t voices;
    uint8_t poly;
    uint16_t master;
    uint16_t hpf;
    uint16_t limit;
    uint16_t score_n;
    uint16_t score_idx;
    uint16_t voice_mask;
    uint16_t slot_mask;
    uint32_t heap_int;
    uint32_t psram_free;
    uint32_t samples;
    char bank[12];
    char ip[16];
    uint8_t tcp_clients;
} NkStatus;
