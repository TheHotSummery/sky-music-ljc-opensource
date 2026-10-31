#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOUCH_GEST_NONE = 0,
    TOUCH_GEST_SWIPE_UP = 0x01,
    TOUCH_GEST_SWIPE_DOWN = 0x02,
    TOUCH_GEST_SWIPE_LEFT = 0x03,
    TOUCH_GEST_SWIPE_RIGHT = 0x04,
    TOUCH_GEST_SINGLE = 0x05,
    TOUCH_GEST_DOUBLE = 0x0B,
    TOUCH_GEST_LONG = 0x0C,
} touch_gest_t;

typedef struct {
    uint16_t raw_x;
    uint16_t raw_y;
    int16_t ui_x; /* landscape UI coords, -1 if invalid */
    int16_t ui_y;
    uint8_t fingers;
    uint8_t gesture;
    uint8_t pressed;
} touch_sample_t;

esp_err_t touch_cst816_start(void);
int touch_cst816_ok(void);
uint8_t touch_cst816_chip_id(void);
bool touch_cst816_read(touch_sample_t *out);
void touch_cst816_dump(void);
/* Console: touch | touch paint | touch ui on|off | touch dbg */
bool touch_cst816_handle_line(const char *line);

#ifdef __cplusplus
}
#endif
