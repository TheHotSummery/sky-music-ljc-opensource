#include "sky_session.h"

#include "esp_random.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

static uint8_t s_token[SKY_SESSION_TOKEN_LEN];
static int64_t s_deadline_us; /* 0 = none */
static sky_session_changed_fn s_changed;

void sky_session_set_changed_cb(sky_session_changed_fn cb)
{
    s_changed = cb;
}

static void notify_changed(void)
{
    if (s_changed) {
        s_changed();
    }
}

static bool alive(void)
{
    if (s_deadline_us == 0) {
        return false;
    }
    if (esp_timer_get_time() >= s_deadline_us) {
        s_deadline_us = 0;
        memset(s_token, 0, sizeof(s_token));
        return false;
    }
    return true;
}

esp_err_t sky_session_issue(void)
{
    esp_fill_random(s_token, sizeof(s_token));
    s_deadline_us = esp_timer_get_time() + (int64_t)SKY_SESSION_TTL_S * 1000000LL;
    notify_changed();
    return ESP_OK;
}

void sky_session_revoke(void)
{
    memset(s_token, 0, sizeof(s_token));
    s_deadline_us = 0;
    notify_changed();
}

bool sky_session_valid(void)
{
    return alive();
}

uint16_t sky_session_ttl_remaining(void)
{
    if (!alive()) {
        return 0;
    }
    int64_t left = s_deadline_us - esp_timer_get_time();
    if (left <= 0) {
        return 0;
    }
    int64_t sec = left / 1000000LL;
    if (sec > 65535) {
        sec = 65535;
    }
    return (uint16_t)sec;
}

size_t sky_session_fill_gatt(uint8_t out[SKY_SESSION_GATT_SIZE])
{
    if (!out) {
        return 0;
    }
    memset(out, 0, SKY_SESSION_GATT_SIZE);
    if (!alive()) {
        return SKY_SESSION_GATT_SIZE;
    }
    memcpy(out, s_token, SKY_SESSION_TOKEN_LEN);
    uint16_t ttl = sky_session_ttl_remaining();
    out[16] = (uint8_t)(ttl & 0xFF);
    out[17] = (uint8_t)((ttl >> 8) & 0xFF);
    out[18] = 0;
    out[19] = 0;
    return SKY_SESSION_GATT_SIZE;
}

bool sky_session_hex(char *buf, size_t cap)
{
    if (!buf || cap < 33 || !alive()) {
        if (buf && cap) {
            buf[0] = 0;
        }
        return false;
    }
    for (size_t i = 0; i < SKY_SESSION_TOKEN_LEN; ++i) {
        sprintf(buf + i * 2, "%02x", s_token[i]);
    }
    buf[32] = 0;
    return true;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool sky_session_check_hex(const char *hex32)
{
    if (!hex32 || !alive()) {
        return false;
    }
    uint8_t got[SKY_SESSION_TOKEN_LEN];
    for (size_t i = 0; i < SKY_SESSION_TOKEN_LEN; ++i) {
        int hi = hex_nibble(hex32[i * 2]);
        int lo = hex_nibble(hex32[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        got[i] = (uint8_t)((hi << 4) | lo);
    }
    return memcmp(got, s_token, SKY_SESSION_TOKEN_LEN) == 0;
}

bool sky_session_check_bearer(const char *auth_header)
{
    if (!auth_header || !alive()) {
        return false;
    }
    const char *p = auth_header;
    if (strncmp(p, "Bearer ", 7) == 0 || strncmp(p, "bearer ", 7) == 0) {
        p += 7;
    }
    while (*p == ' ') {
        p++;
    }
    size_t n = strlen(p);
    if (n != 32) {
        return false;
    }
    return sky_session_check_hex(p);
}
