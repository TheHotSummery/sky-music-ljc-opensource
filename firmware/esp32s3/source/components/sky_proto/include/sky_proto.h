#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SKY App protocol v1.3 (G2 + G3-1): fixed little-endian binary.
 * See docs/CROSS_DEVICE_PROTOCOL_V2.md §G2+G3-1 wire.
 */

#define SKY_PROTO_VER           1u
#define SKY_PROTO_MINOR         7u
#define SKY_STATE_V1_SIZE       72u /* 24 core + play_ms + dur_ms + title[40] */
#define SKY_RESULT_V1_SIZE      8u
#define SKY_CMD_HDR_SIZE        4u
#define SKY_CMD_MAX_SIZE        220u /* allow long UTF-8 music names in OPEN_NAME */
#define SKY_NAME_MAX_BYTES      200u

#define SKY_OP_GET_SNAPSHOT     0x01u
#define SKY_OP_SET_MAP          0x02u
#define SKY_OP_SET_AUDIO        0x03u
#define SKY_OP_SET_MODE         0x04u
#define SKY_OP_SET_TRANSPORT    0x05u
#define SKY_OP_OPEN_ITEM        0x06u
#define SKY_OP_FOLLOW           0x07u
#define SKY_OP_WIFI_CONFIG      0x08u
#define SKY_OP_SESSION_TOKEN    0x09u
#define SKY_OP_SCORE_FILE       0x0Au
#define SKY_OP_OPEN_NAME        0x0Bu

#define SKY_SCORE_LIST          0u
#define SKY_SCORE_PUT_BEGIN     1u
#define SKY_SCORE_PUT_CHUNK     2u
#define SKY_SCORE_PUT_COMMIT    3u
#define SKY_SCORE_DELETE        4u
#define SKY_SCORE_RENAME        5u

#define SKY_NAME_PLAY_SCORE     0u
#define SKY_NAME_FOLLOW_SCORE   1u
#define SKY_NAME_PLAY_MUSIC     2u

#define SKY_TRANSPORT_STOP      0u
#define SKY_TRANSPORT_PLAY      1u
#define SKY_TRANSPORT_PAUSE     2u
#define SKY_TRANSPORT_NEXT      3u
#define SKY_TRANSPORT_PREV      4u

#define SKY_ITEM_BANK           0u
#define SKY_ITEM_MUSIC          1u
#define SKY_ITEM_SCORE          2u

#define SKY_FOLLOW_STOP         0u
#define SKY_FOLLOW_START        1u

#define SKY_WIFI_OFF            0u
#define SKY_WIFI_AP             1u
#define SKY_WIFI_STA            2u
#define SKY_WIFI_CLEAR          3u
#define SKY_WIFI_JOIN           4u

#define SKY_TOKEN_REVOKE        0u
#define SKY_TOKEN_ISSUE         1u

#define SKY_RC_OK               0u
#define SKY_RC_UNSUPPORTED      1u
#define SKY_RC_BAD_ARG          2u
#define SKY_RC_BUSY             3u
#define SKY_RC_FAILED           4u

typedef struct __attribute__((packed)) {
    uint8_t ver;
    uint8_t mode;         /* 0 play .. 4 record */
    uint8_t map;          /* 0 num, 1 sky */
    uint8_t volume;       /* 0..100 */
    int8_t transpose;     /* -12..12 */
    uint8_t tempo_pct;    /* 50..200 */
    uint8_t transport;    /* 0 stop, 1 play, 2 pause */
    uint8_t follow_state; /* drill: 0 off, 1 waiting, 2 done */
    uint16_t state_seq;
    uint16_t key_mask;    /* bit0=K1 .. bit14=K15 */
    uint16_t expect_mask;
    uint16_t follow_at;
    uint16_t follow_total;
    uint16_t bat_mv;
    uint8_t bat_pct;
    uint8_t bat_level;    /* 0 ok, 1 low, 2 critical */
    uint8_t chg_flags;
    uint8_t flags;        /* bit0=bat_ok */
    /* v1.7 now-playing (music/score). Older App ignores bytes past 24. */
    uint32_t play_ms;
    uint32_t dur_ms;
    char title[40];       /* UTF-8 basename, truncated */
} sky_state_v1_t;

_Static_assert(sizeof(sky_state_v1_t) == SKY_STATE_V1_SIZE, "sky_state_v1 size");

typedef struct __attribute__((packed)) {
    uint8_t ver;
    uint8_t opcode;
    uint16_t request_id;
    uint16_t result_code;
    uint16_t state_seq;
} sky_result_v1_t;

_Static_assert(sizeof(sky_result_v1_t) == SKY_RESULT_V1_SIZE, "sky_result_v1 size");

typedef int (*sky_map_get_fn)(void);   /* 1=sky, 0=num */
typedef void (*sky_map_set_fn)(int sky);

void sky_proto_set_map_ops(sky_map_get_fn get_fn, sky_map_set_fn set_fn);
void sky_proto_bump(void); /* bump state_seq after local UI changes (optional) */

void sky_proto_fill_state(sky_state_v1_t *out);
uint16_t sky_proto_state_seq(void);

/* Execute one command; fills result. Returns ESP_OK if parsed (even if RC!=OK). */
esp_err_t sky_proto_exec(const uint8_t *cmd, size_t len, sky_result_v1_t *result);

/* Read BLE score list page buffer (`...000A`). Returns payload length (2..240). */
size_t sky_proto_fill_score_list(uint8_t *out, size_t max_len);
void sky_proto_refresh_score_list(void);

/* Console helpers */
void sky_proto_dump_state(void);
bool sky_proto_handle_line(const char *line);

#ifdef __cplusplus
}
#endif
