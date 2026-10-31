#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SKY_SESSION_TOKEN_LEN 16u
#define SKY_SESSION_TTL_S     600u
#define SKY_SESSION_GATT_SIZE 20u /* token[16] + ttl_s_le16 + pad_le16 */

typedef void (*sky_session_changed_fn)(void);

void sky_session_set_changed_cb(sky_session_changed_fn cb);

esp_err_t sky_session_issue(void);
void sky_session_revoke(void);
bool sky_session_valid(void);
bool sky_session_check_bearer(const char *auth_header); /* "Bearer <hex>" or raw hex */
bool sky_session_check_hex(const char *hex32);          /* 32 hex chars */

/* Fill GATT/serial payload; returns bytes written (0 or 20). */
size_t sky_session_fill_gatt(uint8_t out[SKY_SESSION_GATT_SIZE]);
/* Print 32 hex chars into buf (need ≥33). Returns false if none. */
bool sky_session_hex(char *buf, size_t cap);
uint16_t sky_session_ttl_remaining(void);

#ifdef __cplusplus
}
#endif
