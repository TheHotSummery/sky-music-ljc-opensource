#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*net_line_cb_t)(char *line);

esp_err_t net_link_start(net_line_cb_t on_line);
void net_hook_stdio(void);
void net_cmd(const char *args);
void net_dump_status(void);

/* G3-1 / N1: programmatic WiFi control (BLE / sky_proto). */
enum {
    NET_WIFI_PHASE_OFF = 0,
    NET_WIFI_PHASE_AP = 1,
    NET_WIFI_PHASE_CONNECTING = 2,
    NET_WIFI_PHASE_OK = 3,
    NET_WIFI_PHASE_FAIL = 4,
};

typedef void (*net_wifi_status_cb_t)(void);

esp_err_t net_wifi_off(void);
esp_err_t net_wifi_ap(void);
esp_err_t net_wifi_sta(void); /* NVS creds */
esp_err_t net_wifi_clear(void);
esp_err_t net_wifi_join(const char *ssid, const char *pass);
const char *net_wifi_mode_str(void);
const char *net_wifi_ip(void);
bool net_wifi_ready(void);
bool net_wifi_sta_ok(void);
/** Last WIFI_EVENT_STA_DISCONNECTED reason (0 if none / got IP). */
unsigned net_wifi_last_disc_reason(void);
int net_wifi_phase(void);
int net_wifi_attempts(void);
bool net_wifi_gave_up(void);
const char *net_wifi_ssid(void);
/** 12 bytes: phase, disc, try, give_up, ipv4[4], pad[4] */
void net_wifi_fill_status(uint8_t out[12]);
void net_wifi_set_status_cb(net_wifi_status_cb_t cb);

#ifdef __cplusplus
}
#endif
