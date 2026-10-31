#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB_IDENT_OFF = 0,
    USB_IDENT_HID,
    USB_IDENT_MIDI,
} usb_ident_mode_t;

esp_err_t usb_ident_start(void);
usb_ident_mode_t usb_ident_mode(void);
const char *usb_ident_name(void);
int usb_ident_mounted(void);
void usb_ident_set_mode(usb_ident_mode_t mode);
void usb_ident_cycle(void);
void usb_ident_tap(uint8_t note0);

#ifdef __cplusplus
}
#endif
