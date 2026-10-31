#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PY32F002A -> ESP32-S3 UART key protocol (3.3V, 115200 8N1)
 *
 * Frame:
 *   0xA5 | len | type | payload[len] | xor
 * xor = type XOR payload[0] XOR ... payload[len-1]
 *
 * type 0x01 KEYMASK  payload: uint16 LE, bit0..bit14 = key 0..14 pressed
 * type 0x02 KEYEVENT payload: key_id (0-14), state (1=down, 0=up)
 *
 * PY32 should scan a 3x5 matrix with diodes if chords are required,
 * debounce locally, then send KEYEVENT on edges (and optional KEYMASK
 * as heartbeat every 20-50 ms).
 */

#define KEY_PROTO_SOF              0xA5
#define KEY_PROTO_TYPE_MASK        0x01
#define KEY_PROTO_TYPE_EVENT       0x02
#define KEY_PROTO_MAX_PAYLOAD      8
#define KEY_COUNT                  15

#ifdef __cplusplus
}
#endif
