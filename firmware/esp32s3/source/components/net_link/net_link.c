#include "net_link.h"
#include "nk_proto.h"
#include "board_pins.h"

#include "audio_bank.h"
#include "mixer.h"
#include "score_play.h"
#include "sd_music.h"
#include "sky_session.h"
#include "ui_tft.h"
#include "usb_ident.h"

#include "esp_system.h"
#include "mbedtls/sha256.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "nk_out.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <strings.h>
#include <unistd.h>

static const char *TAG = "net";
#define TCP_PORT 2333
/** Connect attempts after join (first + retries). ~5s apart → 5 tries (~25s) then stop. */
#define STA_CONNECT_MAX 5
#define STA_RETRY_TICKS 20 /* wifi_mgr period 200ms → 4s */

enum {
    WIFI_PREF_OFF = 0,
    WIFI_PREF_STA = 1,
    WIFI_PREF_AP = 2,
};

static net_line_cb_t s_cb;
static volatile int s_wifi_on;
static volatile int s_tcp_clients;
static volatile int s_sta_ok;
static volatile int s_wifi_pref; /* OFF / STA / AP — never auto-flip AP on STA fail */
static volatile uint16_t s_last_disc;
static volatile int s_sta_attempts; /* connect() calls since last join */
static volatile int s_sta_give_up;  /* hit STA_CONNECT_MAX */
static net_wifi_status_cb_t s_wifi_status_cb;
static int s_tcp_started;
static int s_driver;
static int s_portal_on;
static int s_api_on;
static int s_wifi_events;
static char s_ip[16] = "0.0.0.0";
static char s_mode[12] = "off";
static char s_sta_ssid[33];
static int s_cfd = -1;
static SemaphoreHandle_t s_tx_mu;
static SemaphoreHandle_t s_scan_mu;
static httpd_handle_t s_httpd;

extern const uint8_t portal_html_start[] asm("_binary_portal_html_start");
extern const uint8_t portal_html_end[] asm("_binary_portal_html_end");
static TaskHandle_t s_dns_task;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

static uint8_t xor_bytes(const uint8_t *p, size_t n)
{
    uint8_t x = 0;
    for (size_t i = 0; i < n; ++i) {
        x ^= p[i];
    }
    return x;
}

static void nk_send(uint8_t type, const void *payload, uint16_t n)
{
    if (s_cfd < 0) {
        return;
    }
    uint8_t hdr[3] = { NK_MAGIC, (uint8_t)((n + 1) & 0xFF), (uint8_t)(((n + 1) >> 8) & 0xFF) };
    uint8_t t = type;
    uint8_t x = type ^ xor_bytes((const uint8_t *)payload, n);
    if (s_tx_mu) {
        xSemaphoreTake(s_tx_mu, portMAX_DELAY);
    }
    int fd = s_cfd;
    if (fd >= 0) {
        if (send(fd, hdr, 3, 0) < 0 || send(fd, &t, 1, 0) < 0 ||
            (n && send(fd, payload, n, 0) < 0) || send(fd, &x, 1, 0) < 0) {
            /* drop; rx loop will close */
        }
    }
    if (s_tx_mu) {
        xSemaphoreGive(s_tx_mu);
    }
}

static void net_emit_text(const char *data, size_t n)
{
    while (n && s_cfd >= 0) {
        uint16_t chunk = n > 400 ? 400 : (uint16_t)n;
        nk_send(NK_TEXT, data, chunk);
        data += chunk;
        n -= chunk;
    }
}

static int log_vprintf(const char *fmt, va_list ap)
{
    char buf[384];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n <= 0) {
        return n;
    }
    size_t w = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    fwrite(buf, 1, w, stdout);
    net_emit_text(buf, w);
    return n;
}

void net_hook_stdio(void)
{
    static int done;
    if (done) {
        return;
    }
    nk_out_set_tap(net_emit_text);
    esp_log_set_vprintf(log_vprintf);
    done = 1;
    ESP_LOGI(TAG, "stdio tap -> UART + TCP NK_TEXT");
}

static void fill_status(NkStatus *st)
{
    memset(st, 0, sizeof(*st));
    st->ver = NK_VER;
    int sn = 0, si = 0, sp = 0, pa = 0;
    score_get_runtime(&sn, &si, &sp, &pa);
    if (audio_bank_sd_ok()) {
        st->flags |= 1u;
    }
    if (audio_bank_factory_ok()) {
        st->flags |= 2u;
    }
    if (usb_ident_mounted()) {
        st->flags |= 4u;
    }
    if (sp) {
        st->flags |= 8u;
    }
    if (pa) {
        st->flags |= 16u;
    }
    st->voices = (uint8_t)mixer_active_voices();
    st->poly = (uint8_t)mixer_polyphony();
    st->master = mixer_master_ui();
    st->hpf = (uint16_t)mixer_hpf_hz();
    st->limit = (uint16_t)mixer_limit();
    st->score_n = (uint16_t)sn;
    st->score_idx = (uint16_t)si;
    st->voice_mask = mixer_voice_mask();
    st->slot_mask = audio_bank_loaded_mask();
    st->heap_int = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    st->psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    st->samples = (uint32_t)mixer_sample_count();
    strncpy(st->bank, audio_bank_name(), sizeof(st->bank) - 1);
    strncpy(st->ip, s_ip, sizeof(st->ip) - 1);
    st->tcp_clients = (uint8_t)s_tcp_clients;
}

static void status_push_task(void *arg)
{
    (void)arg;
    NkStatus st;
    MixerScope sc;
    unsigned tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (s_cfd < 0) {
            continue;
        }
        mixer_scope_get(&sc);
        nk_send(NK_SCOPE, &sc, sizeof(sc));
        if ((tick++ & 1u) == 0) {
            fill_status(&st);
            nk_send(NK_STATUS, &st, sizeof(st));
        }
    }
}

static void handle_tcp_bytes(uint8_t *buf, size_t *len)
{
    size_t n = *len;
    size_t i = 0;
    while (i < n) {
        if (buf[i] != NK_MAGIC) {
            /* 兼容 nc 纯文本：读到换行当命令 */
            size_t j = i;
            while (j < n && buf[j] != '\n' && buf[j] != NK_MAGIC) {
                j++;
            }
            if (j < n && buf[j] == '\n') {
                buf[j] = 0;
                if (j > i && s_cb) {
                    char *line = (char *)buf + i;
                    if (line[0] == '\r' && j == i) {
                    } else {
                        if (j > i && buf[j - 1] == '\r') {
                            buf[j - 1] = 0;
                        }
                        s_cb(line);
                    }
                }
                i = j + 1;
                continue;
            }
            if (j == n && buf[i] != NK_MAGIC) {
                break;
            }
            i++;
            continue;
        }
        if (i + 3 > n) {
            break;
        }
        uint16_t body = (uint16_t)buf[i + 1] | ((uint16_t)buf[i + 2] << 8);
        if (body < 1 || body > 1024) {
            i++;
            continue;
        }
        if (i + 3 + body + 1 > n) {
            break;
        }
        uint8_t *p = buf + i + 3;
        uint8_t x = buf[i + 3 + body];
        if (xor_bytes(p, body) != x) {
            i++;
            continue;
        }
        uint8_t type = p[0];
        if (type == NK_CMD && body > 1 && s_cb) {
            char cmd[256];
            size_t cl = body - 1;
            if (cl >= sizeof(cmd)) {
                cl = sizeof(cmd) - 1;
            }
            memcpy(cmd, p + 1, cl);
            cmd[cl] = 0;
            s_cb(cmd);
        }
        i += 3 + body + 1;
    }
    if (i > 0 && i < n) {
        memmove(buf, buf + i, n - i);
        *len = n - i;
    } else if (i >= n) {
        *len = 0;
    }
}

static void tcp_task(void *arg)
{
    (void)arg;
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    listen(listen_fd, 2);
    ESP_LOGI(TAG, "TCP %d framed A5 + text fallback", TCP_PORT);

    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int cfd = accept(listen_fd, (struct sockaddr *)&from, &flen);
        if (cfd < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int nd = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
        s_cfd = cfd;
        s_tcp_clients++;
        nk_out_set_uart_echo(0);
        ESP_LOGI(TAG, "TCP client");
        nk_printf("TCP_READY\n");
        uint8_t acc[2048];
        size_t n = 0;
        for (;;) {
            uint8_t tmp[512];
            int r = recv(cfd, tmp, sizeof(tmp), 0);
            if (r <= 0) {
                break;
            }
            if (n + (size_t)r > sizeof(acc)) {
                n = 0;
            }
            memcpy(acc + n, tmp, (size_t)r);
            n += (size_t)r;
            handle_tcp_bytes(acc, &n);
        }
        s_cfd = -1;
        nk_out_set_uart_echo(1);
        close(cfd);
        s_tcp_clients--;
        ESP_LOGI(TAG, "TCP closed");
    }
}

static void ensure_tcp(void)
{
    if (s_tcp_started) {
        return;
    }
    s_tcp_started = 1;
    if (!s_tx_mu) {
        s_tx_mu = xSemaphoreCreateMutex();
    }
    xTaskCreatePinnedToCore(tcp_task, "tcp_cmd", 8192, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(status_push_task, "tcp_st", 4096, NULL, 3, NULL, 0);
}

static void set_banner(const char *mode, const char *ip)
{
    strncpy(s_mode, mode, sizeof(s_mode) - 1);
    strncpy(s_ip, ip, sizeof(s_ip) - 1);
    char line[20];
    snprintf(line, sizeof(line), "%s %s", mode, ip);
    ui_tft_set_net(line);
}

static esp_err_t wifi_stack_once(void)
{
    static int inited;
    if (inited) {
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t ev = esp_event_loop_create_default();
    if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE) {
        return ev;
    }
    inited = 1;
    return ESP_OK;
}

static bool nvs_load_wifi(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = ssid_sz;
    esp_err_t e = nvs_get_str(h, "ssid", ssid, &n);
    n = pass_sz;
    if (e == ESP_OK) {
        e = nvs_get_str(h, "pass", pass, &n);
    }
    nvs_close(h);
    return e == ESP_OK && ssid[0];
}

static void nvs_save_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, "ssid", ssid ? ssid : "");
    nvs_set_str(h, "pass", pass ? pass : "");
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_clear_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    nvs_commit(h);
    nvs_close(h);
}

static void dns_task(void *arg)
{
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) {
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(s);
        vTaskDelete(NULL);
        return;
    }
    uint8_t q[512], r[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, q, sizeof(q), 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || n > 500) {
            continue;
        }
        memcpy(r, q, (size_t)n);
        r[2] = 0x81;
        r[3] = 0x80;
        r[6] = 0;
        r[7] = 1;
        int len = n;
        r[len++] = 0xC0;
        r[len++] = 0x0C;
        r[len++] = 0;
        r[len++] = 1;
        r[len++] = 0;
        r[len++] = 1;
        r[len++] = 0;
        r[len++] = 0;
        r[len++] = 0;
        r[len++] = 30;
        r[len++] = 0;
        r[len++] = 4;
        r[len++] = 192;
        r[len++] = 168;
        r[len++] = 4;
        r[len++] = 1;
        sendto(s, r, len, 0, (struct sockaddr *)&from, fl);
    }
}

static int rssi_cmp(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

static void json_esc(char *o, size_t cap, const char *s)
{
    size_t n = 0;
    if (cap == 0) {
        return;
    }
    while (*s && n + 2 < cap) {
        char c = *s++;
        if (c == '"' || c == '\\') {
            if (n + 3 >= cap) {
                break;
            }
            o[n++] = '\\';
            o[n++] = c;
        } else if ((unsigned char)c < 0x20) {
            continue;
        } else {
            o[n++] = c;
        }
    }
    o[n] = 0;
}

static esp_err_t portal_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, (const char *)portal_html_start, portal_html_end - portal_html_start);
    return ESP_OK;
}

static esp_err_t portal_scan(httpd_req_t *req)
{
    if (!s_scan_mu) {
        s_scan_mu = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_scan_mu, portMAX_DELAY);
    wifi_scan_config_t sc = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    wifi_ap_record_t rec[20];
    uint16_t n = 20;
    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_records(&n, rec);
        qsort(rec, n, sizeof(rec[0]), rssi_cmp);
    } else {
        n = 0;
    }
    xSemaphoreGive(s_scan_mu);

    char json[1600];
    size_t p = 0;
    json[p++] = '[';
    json[sizeof(json) - 1] = 0;
    int out_n = 0;
    char seen[20][33];
    for (uint16_t i = 0; i < n; ++i) {
        char ssid[33];
        memcpy(ssid, rec[i].ssid, 32);
        ssid[32] = 0;
        if (!ssid[0] || strncmp(ssid, "S3Keys", 6) == 0) {
            continue;
        }
        int dup = 0;
        for (int k = 0; k < out_n; ++k) {
            if (strcmp(seen[k], ssid) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup || out_n >= 20) {
            continue;
        }
        strncpy(seen[out_n], ssid, 32);
        seen[out_n][32] = 0;
        char esc[96];
        json_esc(esc, sizeof(esc), ssid);
        int lock = rec[i].authmode != WIFI_AUTH_OPEN;
        int w = snprintf(json + p, sizeof(json) - p, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"lock\":%d}",
                         out_n ? "," : "", esc, (int)rec[i].rssi, lock);
        if (w <= 0 || (size_t)w >= sizeof(json) - p) {
            break;
        }
        p += (size_t)w;
        out_n++;
    }
    if (p + 2 < sizeof(json)) {
        json[p++] = ']';
        json[p] = 0;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t portal_status(httpd_req_t *req)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":%d,\"ip\":\"%s\"}", s_sta_ok ? 1 : 0, s_ip);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static int url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && r[1] && r[2]) {
            unsigned v = 0;
            sscanf(r + 1, "%2x", &v);
            *w++ = (char)v;
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
    return 0;
}

static void parse_form(const char *body, char *ssid, size_t ss, char *pass, size_t ps)
{
    ssid[0] = 0;
    pass[0] = 0;
    char tmp[384];
    strncpy(tmp, body, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    char *tok = strtok(tmp, "&");
    while (tok) {
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = 0;
            url_decode(eq + 1);
            if (strcmp(tok, "ssid") == 0) {
                strncpy(ssid, eq + 1, ss - 1);
            } else if (strcmp(tok, "pass") == 0) {
                strncpy(pass, eq + 1, ps - 1);
            }
        }
        tok = strtok(NULL, "&");
    }
}

static esp_err_t start_sta_from_creds(const char *ssid, const char *pass);
static void ensure_httpd(int with_portal);

static esp_err_t portal_save(httpd_req_t *req)
{
    char body[384] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n < 0) {
        return ESP_FAIL;
    }
    char ssid[33] = {0};
    char pass[65] = {0};
    parse_form(body, ssid, sizeof(ssid), pass, sizeof(pass));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "{\"ok\":1}", HTTPD_RESP_USE_STRLEN);
    if (ssid[0]) {
        nvs_save_wifi(ssid, pass);
        start_sta_from_creds(ssid, pass);
    }
    return ESP_OK;
}

static bool api_require_auth(httpd_req_t *req)
{
    char auth[96];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"AUTH_REQUIRED\"}", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    if (!sky_session_check_bearer(auth)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"AUTH_REQUIRED\"}", HTTPD_RESP_USE_STRLEN);
        return false;
    }
    return true;
}

static esp_err_t api_status(httpd_req_t *req)
{
    char buf[220];
    snprintf(buf, sizeof(buf),
             "{\"wifi\":\"%s\",\"ip\":\"%s\",\"tcp\":%d,\"api\":\"1.1\",\"sta_ok\":%d,\"disc\":%u,\"ssid\":\"%s\",\"try\":%d,\"give_up\":%d}",
             s_mode, s_ip, TCP_PORT, s_sta_ok ? 1 : 0, (unsigned)s_last_disc, s_sta_ssid, s_sta_attempts,
             s_sta_give_up ? 1 : 0);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static int api_type_dir(const char *type, char *dir, size_t dir_sz)
{
    if (!type) {
        return -1;
    }
    if (strcmp(type, "music") == 0) {
        snprintf(dir, dir_sz, "%s/music", SD_MOUNT_POINT);
        return 0;
    }
    if (strcmp(type, "score") == 0) {
        snprintf(dir, dir_sz, "%s/puzi", SD_MOUNT_POINT);
        return 0;
    }
    return -1;
}

static int ends_with_ci(const char *n, const char *ext)
{
    size_t a = strlen(n), b = strlen(ext);
    if (a < b) {
        return 0;
    }
    return strcasecmp(n + a - b, ext) == 0;
}

static int music_ext_ok(const char *name)
{
    return ends_with_ci(name, ".wav") || ends_with_ci(name, ".mp3") || ends_with_ci(name, ".flac") ||
           ends_with_ci(name, ".aac");
}

static int name_ok_for_type(const char *type, const char *name)
{
    if (!name || name[0] == '.' || strchr(name, '/') || strstr(name, "..")) {
        return 0;
    }
    if (strcmp(type, "music") == 0) {
        return music_ext_ok(name);
    }
    return ends_with_ci(name, ".txt") || ends_with_ci(name, ".json") || ends_with_ci(name, ".sky");
}

static int cmp_strptr(const void *a, const void *b)
{
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static esp_err_t api_list(httpd_req_t *req)
{
    if (!api_require_auth(req)) {
        return ESP_OK;
    }
    char q[128] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    char type[16] = {0};
    char cursor_s[12] = "0";
    char limit_s[12] = "50";
    httpd_query_key_value(q, "type", type, sizeof(type));
    httpd_query_key_value(q, "cursor", cursor_s, sizeof(cursor_s));
    httpd_query_key_value(q, "limit", limit_s, sizeof(limit_s));
    int cursor = atoi(cursor_s);
    int limit = atoi(limit_s);
    if (cursor < 0) {
        cursor = 0;
    }
    if (limit < 1) {
        limit = 50;
    }
    if (limit > 100) {
        limit = 100;
    }
    char dir[64];
    if (api_type_dir(type, dir, sizeof(dir)) != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"INVALID_ARGUMENT\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    DIR *d = opendir(dir);
    if (!d) {
        size_t dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        size_t ifr = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGW(TAG, "opendir %s fail errno=%d dma_largest=%u int_free=%u", dir, errno,
                 (unsigned)dma, (unsigned)ifr);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        /* Often FatFs window alloc fail after WiFi — not an unmounted card. */
        httpd_resp_send(req, "{\"error\":\"SD_BUSY\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    const int cap = 2048;
    char **names = heap_caps_calloc((size_t)cap, sizeof(char *), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!names) {
        names = calloc((size_t)cap, sizeof(char *));
    }
    int n = 0;
    if (names) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && n < cap) {
            if (!name_ok_for_type(type, ent->d_name)) {
                continue;
            }
            size_t len = strlen(ent->d_name) + 1;
            char *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!copy) {
                copy = malloc(len);
            }
            if (!copy) {
                break;
            }
            memcpy(copy, ent->d_name, len);
            names[n++] = copy;
        }
    }
    closedir(d);
    if (names && n > 1) {
        qsort(names, (size_t)n, sizeof(names[0]), cmp_strptr);
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "{\"type\":\"");
    httpd_resp_sendstr_chunk(req, type);
    httpd_resp_sendstr_chunk(req, "\",\"total\":");
    char num[32];
    snprintf(num, sizeof(num), "%d", n);
    httpd_resp_sendstr_chunk(req, num);
    httpd_resp_sendstr_chunk(req, ",\"cursor\":");
    snprintf(num, sizeof(num), "%d", cursor);
    httpd_resp_sendstr_chunk(req, num);
    httpd_resp_sendstr_chunk(req, ",\"items\":[");
    /* Skip per-file stat by default: each stat can burn a 512B DMA scratch under WiFi+BLE. */
    int want_size = 0;
    int out = 0;
    for (int i = cursor; i < n && out < limit; ++i, ++out) {
        if (out) {
            httpd_resp_sendstr_chunk(req, ",");
        }
        /* Stream name escape — long UTF-8 LFN must not truncate mid-JSON. */
        httpd_resp_sendstr_chunk(req, "{\"name\":\"");
        {
            const char *s = names[i] ? names[i] : "";
            char piece[128];
            size_t pn = 0;
            while (*s) {
                unsigned char c = (unsigned char)*s++;
                if (c == '"' || c == '\\') {
                    if (pn + 2 >= sizeof(piece)) {
                        piece[pn] = 0;
                        httpd_resp_sendstr_chunk(req, piece);
                        pn = 0;
                    }
                    piece[pn++] = '\\';
                    piece[pn++] = (char)c;
                } else if (c < 0x20) {
                    continue;
                } else {
                    if (pn + 1 >= sizeof(piece)) {
                        piece[pn] = 0;
                        httpd_resp_sendstr_chunk(req, piece);
                        pn = 0;
                    }
                    piece[pn++] = (char)c;
                }
            }
            if (pn) {
                piece[pn] = 0;
                httpd_resp_sendstr_chunk(req, piece);
            }
        }
        long size = 0;
        if (want_size) {
            char path[400];
            if (snprintf(path, sizeof(path), "%s/%s", dir, names[i]) < (int)sizeof(path)) {
                struct stat st;
                if (stat(path, &st) == 0) {
                    size = (long)st.st_size;
                }
            }
        }
        char tail[48];
        snprintf(tail, sizeof(tail), "\",\"size\":%ld}", size);
        httpd_resp_sendstr_chunk(req, tail);
    }
    int next = (cursor + out < n) ? (cursor + out) : -1;
    snprintf(num, sizeof(num), "],\"next\":%d}", next);
    httpd_resp_sendstr_chunk(req, num);
    httpd_resp_sendstr_chunk(req, NULL);

    if (names) {
        for (int i = 0; i < n; ++i) {
            free(names[i]);
        }
        free(names);
    }
    return ESP_OK;
}

static esp_err_t api_file(httpd_req_t *req)
{
    if (!api_require_auth(req)) {
        return ESP_OK;
    }
    char q[256] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    char type[16] = {0};
    char name[160] = {0};
    httpd_query_key_value(q, "type", type, sizeof(type));
    httpd_query_key_value(q, "name", name, sizeof(name));
    url_decode(name);
    char dir[64];
    if (api_type_dir(type, dir, sizeof(dir)) != 0 || !name_ok_for_type(type, name)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"INVALID_ARGUMENT\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char path[220];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"INVALID_ARGUMENT\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGW(TAG, "fopen %s fail errno=%d dma_largest=%u", path, errno,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"NOT_FOUND\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) {
        sz = 0;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char disp[200];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    char clen[16];
    snprintf(clen, sizeof(clen), "%ld", sz);
    httpd_resp_set_hdr(req, "Content-Length", clen);

    /* Prefer DMA-capable read window so SDSPI avoids a second temp DMA alloc. */
    const size_t chunk = 2048;
    char *buf = heap_caps_malloc(chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        buf = heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        fclose(fp);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"NO_MEM\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    size_t rd;
    while ((rd = fread(buf, 1, chunk, fp)) > 0) {
        if (httpd_resp_send_chunk(req, buf, rd) != ESP_OK) {
            break;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    fclose(fp);
    return ESP_OK;
}

/* ---- G3-2 upload / rename / delete ---- */
#define UP_CHUNK_MAX 4096
#define UP_MUSIC_MAX (24u * 1024u * 1024u)
#define UP_SCORE_MAX (1u * 1024u * 1024u)
#define UP_IDLE_MS 120000

typedef struct {
    int active;
    uint32_t task;
    uint32_t size_total;
    uint32_t size_got;
    int replace;
    int have_sha;
    uint8_t sha_expect[32];
    mbedtls_sha256_context sha;
    TickType_t last_ms;
    char type[8];
    char name[160];
    char tmp_path[220];
    char final_path[220];
} up_task_t;

static up_task_t s_up;

static void api_json_err(httpd_req_t *req, const char *status, const char *code)
{
    char body[64];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", code);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static int basename_eq_path(const char *path, const char *name)
{
    if (!path || !name || !name[0]) {
        return 0;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strcasecmp(base, name) == 0;
}

static int file_in_use(const char *name)
{
    int playing = 0;
    sd_music_get_progress(NULL, NULL, NULL, &playing);
    if (playing && basename_eq_path(sd_music_path(), name)) {
        return 1;
    }
    int sp = 0, paused = 0;
    score_get_progress(NULL, NULL, NULL, &sp, &paused);
    if (sp && basename_eq_path(score_current_path(), name)) {
        return 1;
    }
    return 0;
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

static int parse_sha256_hex(const char *hex, uint8_t out[32])
{
    if (!hex || strlen(hex) != 64) {
        return -1;
    }
    for (int i = 0; i < 32; ++i) {
        int a = hex_nibble(hex[i * 2]);
        int b = hex_nibble(hex[i * 2 + 1]);
        if (a < 0 || b < 0) {
            return -1;
        }
        out[i] = (uint8_t)((a << 4) | b);
    }
    return 0;
}

static const char *json_find_key(const char *json, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        p += strlen(pat);
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == ':') {
            p++;
            while (*p && isspace((unsigned char)*p)) {
                p++;
            }
            return p;
        }
    }
    return NULL;
}

static int json_get_str(const char *json, const char *key, char *out, size_t out_sz)
{
    const char *p = json_find_key(json, key);
    if (!p || *p != '"' || out_sz < 2) {
        return -1;
    }
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_sz) {
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[n++] = *p++;
    }
    out[n] = 0;
    return (*p == '"') ? 0 : -1;
}

static int json_get_u32(const char *json, const char *key, uint32_t *out)
{
    const char *p = json_find_key(json, key);
    if (!p || !isdigit((unsigned char)*p)) {
        return -1;
    }
    *out = (uint32_t)strtoul(p, NULL, 10);
    return 0;
}

static int recv_body(httpd_req_t *req, char *buf, size_t cap)
{
    int need = req->content_len;
    if (need < 0 || (size_t)need >= cap) {
        return -1;
    }
    int got = 0;
    while (got < need) {
        int n = httpd_req_recv(req, buf + got, (size_t)(need - got));
        if (n <= 0) {
            return -1;
        }
        got += n;
    }
    buf[got] = 0;
    return got;
}

static void up_clear(int unlink_tmp)
{
    if (unlink_tmp && s_up.tmp_path[0]) {
        unlink(s_up.tmp_path);
    }
    if (s_up.active) {
        mbedtls_sha256_free(&s_up.sha);
    }
    memset(&s_up, 0, sizeof(s_up));
}

static esp_err_t api_upload_begin(httpd_req_t *req)
{
    if (!api_require_auth(req)) {
        return ESP_OK;
    }
    char body[512];
    if (recv_body(req, body, sizeof(body)) < 0) {
        api_json_err(req, "400 Bad Request", "INVALID_ARGUMENT");
        return ESP_OK;
    }
    char type[16] = {0};
    char name[160] = {0};
    char sha_hex[68] = {0};
    char conflict[16] = "fail";
    uint32_t size = 0;
    if (json_get_str(body, "type", type, sizeof(type)) != 0 ||
        json_get_str(body, "name", name, sizeof(name)) != 0 || json_get_u32(body, "size", &size) != 0) {
        api_json_err(req, "400 Bad Request", "INVALID_ARGUMENT");
        return ESP_OK;
    }
    (void)json_get_str(body, "conflict", conflict, sizeof(conflict));
    int have_sha = (json_get_str(body, "sha256", sha_hex, sizeof(sha_hex)) == 0);
    char dir[64];
    if (api_type_dir(type, dir, sizeof(dir)) != 0 || !name_ok_for_type(type, name)) {
        api_json_err(req, "400 Bad Request", "INVALID_ARGUMENT");
        return ESP_OK;
    }
    uint32_t lim = (strcmp(type, "music") == 0) ? UP_MUSIC_MAX : UP_SCORE_MAX;
    if (size == 0 || size > lim) {
        api_json_err(req, "400 Bad Request", "INVALID_ARGUMENT");
        return ESP_OK;
    }
    if (file_in_use(name)) {
        api_json_err(req, "409 Conflict", "BUSY");
        return ESP_OK;
    }
    char final_path[220];
    snprintf(final_path, sizeof(final_path), "%s/%s", dir, name);
    struct stat st;
    int exists = (stat(final_path, &st) == 0);
    int replace = (strcasecmp(conflict, "replace") == 0);
    if (exists && !replace) {
        api_json_err(req, "409 Conflict", "EXISTS");
        return ESP_OK;
    }
    if (s_up.active) {
        TickType_t now = xTaskGetTickCount();
        if ((now - s_up.last_ms) < pdMS_TO_TICKS(UP_IDLE_MS)) {
            api_json_err(req, "409 Conflict", "BUSY");
            return ESP_OK;
        }
        up_clear(1);
    }
    uint32_t task = esp_random();
    if (task == 0) {
        task = 1;
    }
    char tmp_path[220];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.up_%08lx.part", dir, (unsigned long)task);
    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) {
        ESP_LOGW(TAG, "upload begin fopen %s errno=%d", tmp_path, errno);
        api_json_err(req, "503 Service Unavailable", "NO_SD");
        return ESP_OK;
    }
    fclose(fp);

    memset(&s_up, 0, sizeof(s_up));
    s_up.active = 1;
    s_up.task = task;
    s_up.size_total = size;
    s_up.replace = replace;
    strncpy(s_up.type, type, sizeof(s_up.type) - 1);
    strncpy(s_up.name, name, sizeof(s_up.name) - 1);
    strncpy(s_up.tmp_path, tmp_path, sizeof(s_up.tmp_path) - 1);
    strncpy(s_up.final_path, final_path, sizeof(s_up.final_path) - 1);
    mbedtls_sha256_init(&s_up.sha);
    mbedtls_sha256_starts(&s_up.sha, 0);
    if (have_sha) {
        if (parse_sha256_hex(sha_hex, s_up.sha_expect) != 0) {
            up_clear(1);
            api_json_err(req, "400 Bad Request", "INVALID_ARGUMENT");
            return ESP_OK;
        }
        s_up.have_sha = 1;
    }
    s_up.last_ms = xTaskGetTickCount();

    char resp[96];
    snprintf(resp, sizeof(resp), "{\"task\":\"%08lx\",\"chunk\":%d,\"offset\":0}", (unsigned long)task,
             UP_CHUNK_MAX);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_upload_chunk(httpd_req_t *req)
{
    if (!api_require_auth(req)) {
        return ESP_OK;
    }
    if (!s_up.active) {
        api_json_err(req, "404 Not Found", "NOT_FOUND");
        return ESP_OK;
    }
    char q[96] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    char task_s[16] = {0};
    char off_s[16] = {0};
    httpd_query_key_value(q, "task", task_s, sizeof(task_s));
    httpd_query_key_value(q, "offset", off_s, sizeof(off_s));
    uint32_t task = (uint32_t)strtoul(task_s, NULL, 16);
    uint32_t offset = (uint32_t)strtoul(off_s, NULL, 10);
    if (task != s_up.task) {
        api_json_err(req, "404 Not Found", "NOT_FOUND");
        return ESP_OK;
    }
    int clen = req->content_len;
    if (clen < 0 || clen > UP_CHUNK_MAX) {
        api_json_err(req, "400 Bad Request", "INVALID_ARGUMENT");
        return ESP_OK;
    }
    if (offset != s_up.size_got) {
        /* Idempotent retry of last chunk not supported without buffering; reject. */
        api_json_err(req, "409 Conflict", "STALE_ITEM");
        return ESP_OK;
    }
    if (s_up.size_got + (uint32_t)clen > s_up.size_total) {
        api_json_err(req, "400 Bad Request", "INVALID_ARGUMENT");
        return ESP_OK;
    }
    FILE *fp = fopen(s_up.tmp_path, "ab");
    if (!fp) {
        api_json_err(req, "503 Service Unavailable", "NO_SD");
        return ESP_OK;
    }
    char *buf = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(1024);
    }
    if (!buf) {
        fclose(fp);
        api_json_err(req, "503 Service Unavailable", "NO_MEM");
        return ESP_OK;
    }
    int got = 0;
    while (got < clen) {
        int n = httpd_req_recv(req, buf, (size_t)((clen - got) > 1024 ? 1024 : (clen - got)));
        if (n <= 0) {
            free(buf);
            fclose(fp);
            api_json_err(req, "400 Bad Request", "INVALID_ARGUMENT");
            return ESP_OK;
        }
        if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n) {
            free(buf);
            fclose(fp);
            api_json_err(req, "503 Service Unavailable", "NO_SD");
            return ESP_OK;
        }
        mbedtls_sha256_update(&s_up.sha, (const unsigned char *)buf, (size_t)n);
        got += n;
    }
    free(buf);
    fclose(fp);
    s_up.size_got += (uint32_t)clen;
    s_up.last_ms = xTaskGetTickCount();

    char resp[48];
    snprintf(resp, sizeof(resp), "{\"offset\":%lu}", (unsigned long)s_up.size_got);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_upload_commit(httpd_req_t *req)
{
    if (!api_require_auth(req)) {
        return ESP_OK;
    }
    if (!s_up.active) {
        api_json_err(req, "404 Not Found", "NOT_FOUND");
        return ESP_OK;
    }
    char q[64] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    char task_s[16] = {0};
    httpd_query_key_value(q, "task", task_s, sizeof(task_s));
    uint32_t task = (uint32_t)strtoul(task_s, NULL, 16);
    if (task != s_up.task) {
        api_json_err(req, "404 Not Found", "NOT_FOUND");
        return ESP_OK;
    }
    if (s_up.size_got != s_up.size_total) {
        api_json_err(req, "400 Bad Request", "INVALID_ARGUMENT");
        return ESP_OK;
    }
    uint8_t dig[32];
    mbedtls_sha256_finish(&s_up.sha, dig);
    if (s_up.have_sha && memcmp(dig, s_up.sha_expect, 32) != 0) {
        up_clear(1);
        api_json_err(req, "400 Bad Request", "CHECKSUM_MISMATCH");
        return ESP_OK;
    }
    if (file_in_use(s_up.name)) {
        up_clear(1);
        api_json_err(req, "409 Conflict", "BUSY");
        return ESP_OK;
    }
    if (s_up.replace) {
        unlink(s_up.final_path);
    }
    if (rename(s_up.tmp_path, s_up.final_path) != 0) {
        ESP_LOGW(TAG, "commit rename errno=%d", errno);
        up_clear(1);
        api_json_err(req, "503 Service Unavailable", "NO_SD");
        return ESP_OK;
    }
    char esc[160];
    json_esc(esc, sizeof(esc), s_up.name);
    char resp[256];
    snprintf(resp, sizeof(resp), "{\"ok\":1,\"name\":\"%s\",\"size\":%lu}", esc,
             (unsigned long)s_up.size_total);
    s_up.tmp_path[0] = 0;
    up_clear(0);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_upload_cancel(httpd_req_t *req)
{
    if (!api_require_auth(req)) {
        return ESP_OK;
    }
    char q[64] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    char task_s[16] = {0};
    httpd_query_key_value(q, "task", task_s, sizeof(task_s));
    uint32_t task = (uint32_t)strtoul(task_s, NULL, 16);
    if (!s_up.active || task != s_up.task) {
        api_json_err(req, "404 Not Found", "NOT_FOUND");
        return ESP_OK;
    }
    up_clear(1);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "{\"ok\":1}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_rename(httpd_req_t *req)
{
    if (!api_require_auth(req)) {
        return ESP_OK;
    }
    char body[384];
    if (recv_body(req, body, sizeof(body)) < 0) {
        api_json_err(req, "400 Bad Request", "INVALID_ARGUMENT");
        return ESP_OK;
    }
    char type[16] = {0};
    char from[160] = {0};
    char to[160] = {0};
    if (json_get_str(body, "type", type, sizeof(type)) != 0 ||
        json_get_str(body, "from", from, sizeof(from)) != 0 ||
        json_get_str(body, "to", to, sizeof(to)) != 0) {
        api_json_err(req, "400 Bad Request", "INVALID_ARGUMENT");
        return ESP_OK;
    }
    char dir[64];
    if (api_type_dir(type, dir, sizeof(dir)) != 0 || !name_ok_for_type(type, from) ||
        !name_ok_for_type(type, to)) {
        api_json_err(req, "400 Bad Request", "INVALID_ARGUMENT");
        return ESP_OK;
    }
    if (file_in_use(from) || file_in_use(to)) {
        api_json_err(req, "409 Conflict", "BUSY");
        return ESP_OK;
    }
    char src[220], dst[220];
    snprintf(src, sizeof(src), "%s/%s", dir, from);
    snprintf(dst, sizeof(dst), "%s/%s", dir, to);
    struct stat st;
    if (stat(src, &st) != 0) {
        api_json_err(req, "404 Not Found", "NOT_FOUND");
        return ESP_OK;
    }
    if (stat(dst, &st) == 0) {
        api_json_err(req, "409 Conflict", "EXISTS");
        return ESP_OK;
    }
    if (rename(src, dst) != 0) {
        api_json_err(req, "503 Service Unavailable", "NO_SD");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "{\"ok\":1}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_delete(httpd_req_t *req)
{
    if (!api_require_auth(req)) {
        return ESP_OK;
    }
    char body[256];
    if (recv_body(req, body, sizeof(body)) < 0) {
        api_json_err(req, "400 Bad Request", "INVALID_ARGUMENT");
        return ESP_OK;
    }
    char type[16] = {0};
    char name[160] = {0};
    if (json_get_str(body, "type", type, sizeof(type)) != 0 ||
        json_get_str(body, "name", name, sizeof(name)) != 0) {
        api_json_err(req, "400 Bad Request", "INVALID_ARGUMENT");
        return ESP_OK;
    }
    char dir[64];
    if (api_type_dir(type, dir, sizeof(dir)) != 0 || !name_ok_for_type(type, name)) {
        api_json_err(req, "400 Bad Request", "INVALID_ARGUMENT");
        return ESP_OK;
    }
    if (file_in_use(name)) {
        api_json_err(req, "409 Conflict", "BUSY");
        return ESP_OK;
    }
    char path[220];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (unlink(path) != 0) {
        api_json_err(req, "404 Not Found", "NOT_FOUND");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "{\"ok\":1}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void ensure_httpd(int with_portal)
{
    if (!s_httpd) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
        /* Keep lean: WiFi+BLE already ate most internal DMA/stack RAM. */
        cfg.max_uri_handlers = with_portal ? 18 : 12;
        cfg.max_open_sockets = 3;
        cfg.stack_size = 7168;
        cfg.core_id = 1;
        esp_err_t err = httpd_start(&s_httpd, &cfg);
        if (err != ESP_OK) {
            size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            ESP_LOGW(TAG, "httpd start %s int_free=%u largest=%u; retry tiny", esp_err_to_name(err),
                     (unsigned)free_int, (unsigned)largest);
            cfg.stack_size = 5120;
            cfg.max_open_sockets = 2;
            cfg.max_uri_handlers = with_portal ? 14 : 10;
            err = httpd_start(&s_httpd, &cfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "httpd fail %s (API unavailable)", esp_err_to_name(err));
        return;
    }
        }
        ESP_LOGI(TAG, "httpd :%d ok", cfg.server_port);
    }
    if (with_portal && !s_portal_on) {
        httpd_uri_t g = {.uri = "/", .method = HTTP_GET, .handler = portal_get};
        httpd_uri_t g2 = {.uri = "/generate_204", .method = HTTP_GET, .handler = portal_get};
        httpd_uri_t g3 = {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = portal_get};
        httpd_uri_t sc = {.uri = "/scan", .method = HTTP_GET, .handler = portal_scan};
        httpd_uri_t st = {.uri = "/status", .method = HTTP_GET, .handler = portal_status};
        httpd_uri_t p = {.uri = "/save", .method = HTTP_POST, .handler = portal_save};
    httpd_register_uri_handler(s_httpd, &g);
    httpd_register_uri_handler(s_httpd, &g2);
    httpd_register_uri_handler(s_httpd, &g3);
    httpd_register_uri_handler(s_httpd, &sc);
    httpd_register_uri_handler(s_httpd, &st);
    httpd_register_uri_handler(s_httpd, &p);
        if (!s_dns_task) {
            xTaskCreatePinnedToCore(dns_task, "dns", 2560, NULL, 3, &s_dns_task, 1);
        }
    s_portal_on = 1;
    }
    if (!s_api_on && s_httpd) {
        httpd_uri_t a1 = {.uri = "/api/v1/status", .method = HTTP_GET, .handler = api_status};
        httpd_uri_t a2 = {.uri = "/api/v1/list", .method = HTTP_GET, .handler = api_list};
        httpd_uri_t a3 = {.uri = "/api/v1/file", .method = HTTP_GET, .handler = api_file};
        httpd_uri_t a4 = {.uri = "/api/v1/upload/begin", .method = HTTP_POST, .handler = api_upload_begin};
        httpd_uri_t a5 = {.uri = "/api/v1/upload/chunk", .method = HTTP_PUT, .handler = api_upload_chunk};
        httpd_uri_t a6 = {.uri = "/api/v1/upload/commit", .method = HTTP_POST, .handler = api_upload_commit};
        httpd_uri_t a7 = {.uri = "/api/v1/upload/cancel", .method = HTTP_POST, .handler = api_upload_cancel};
        httpd_uri_t a8 = {.uri = "/api/v1/rename", .method = HTTP_POST, .handler = api_rename};
        httpd_uri_t a9 = {.uri = "/api/v1/delete", .method = HTTP_POST, .handler = api_delete};
        httpd_register_uri_handler(s_httpd, &a1);
        httpd_register_uri_handler(s_httpd, &a2);
        httpd_register_uri_handler(s_httpd, &a3);
        httpd_register_uri_handler(s_httpd, &a4);
        httpd_register_uri_handler(s_httpd, &a5);
        httpd_register_uri_handler(s_httpd, &a6);
        httpd_register_uri_handler(s_httpd, &a7);
        httpd_register_uri_handler(s_httpd, &a8);
        httpd_register_uri_handler(s_httpd, &a9);
        s_api_on = 1;
    }
}

static void notify_wifi_status(void)
{
    if (s_wifi_status_cb) {
        s_wifi_status_cb();
    }
}

static void apply_sta_up(void)
{
    set_banner("STA", s_ip);
    s_last_disc = 0;
    s_sta_give_up = 0;
    s_sta_attempts = 0;
    /* HTTP before TCP status task — both need internal stack. */
    ensure_httpd(0);
    ensure_tcp();
    static int mdns_ok;
    if (!mdns_ok) {
        if (mdns_init() == ESP_OK) {
            mdns_hostname_set("s3keys");
            mdns_instance_name_set("S3Keys");
            mdns_service_add(NULL, "_s3keys", "_tcp", TCP_PORT, NULL, 0);
            mdns_ok = 1;
        }
    }
    ESP_LOGI(TAG, "STA ip=%s TCP %d HTTP /api/v1 mdns s3keys.local", s_ip, TCP_PORT);
    notify_wifi_status();
}

static void poll_sta_ip(void)
{
    if (!s_sta_netif || s_wifi_pref != WIFI_PREF_STA) {
        return;
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK || info.ip.addr == 0) {
        if (s_sta_ok) {
        s_sta_ok = 0;
        }
        return;
    }
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    int first = !s_sta_ok;
    s_sta_ok = 1;
    if (first || strcmp(s_ip, ip) != 0) {
        strncpy(s_ip, ip, sizeof(s_ip) - 1);
        apply_sta_up();
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        if (s_wifi_pref == WIFI_PREF_STA && !s_sta_give_up && s_sta_attempts < STA_CONNECT_MAX) {
            s_sta_attempts++;
            esp_err_t er = esp_wifi_connect();
            if (er != ESP_OK) {
                ESP_LOGW(TAG, "STA_START connect %s", esp_err_to_name(er));
            }
        }
        return;
    }
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        s_last_disc = e ? e->reason : 0;
        s_sta_ok = 0;
        ESP_LOGW(TAG, "STA down reason=%u ssid=%s pref=%s", (unsigned)s_last_disc, s_sta_ssid,
                 s_wifi_pref == WIFI_PREF_STA ? "sta" : (s_wifi_pref == WIFI_PREF_AP ? "ap" : "off"));
        notify_wifi_status();
        /* Stay STA and let wifi_mgr retry — do NOT open SoftAP. */
        return;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
    strncpy(s_ip, ip, sizeof(s_ip) - 1);
    s_sta_ok = 1;
    apply_sta_up();
}

static esp_err_t wifi_driver(void)
{
    if (s_driver) {
        return ESP_OK;
    }
    if (!s_sta_netif) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /* Runtime trim: BLE coexist leaves little contiguous internal DMA RAM. */
    /* largest internal DMA block is often ~4–6KB with BLE up; 2×1600 fits. */
    cfg.static_rx_buf_num = 2;
    cfg.dynamic_rx_buf_num = 8;
    cfg.tx_buf_type = 1; /* dynamic TX — avoid large static internal blocks */
    cfg.dynamic_tx_buf_num = 8;
    cfg.cache_tx_buf_num = 0;
    cfg.rx_ba_win = 2;
    cfg.mgmt_sbuf_num = 6; /* API minimum */
    cfg.rx_mgmt_buf_num = 3;
    cfg.ampdu_rx_enable = 0;
    cfg.ampdu_tx_enable = 0;
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s (internal RAM)", esp_err_to_name(err));
        size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        ESP_LOGE(TAG, "internal free=%u largest_dma=%u", (unsigned)free_int, (unsigned)largest);
        return err;
    }
    if (!s_wifi_events) {
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL));
        s_wifi_events = 1;
    }
    s_driver = 1;
    return ESP_OK;
}

static esp_err_t wifi_ensure_started(void)
{
    if (s_wifi_on) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start %s", esp_err_to_name(err));
        return err;
    }
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    esp_wifi_set_max_tx_power(52);
    ui_tft_radio_busy(true);
    s_wifi_on = 1;
    return ESP_OK;
}

static esp_err_t start_ap_portal(void)
{
    esp_err_t err = wifi_driver();
    if (err != ESP_OK) {
        return err;
    }
    s_wifi_pref = WIFI_PREF_AP;
    s_sta_ok = 0;
    s_sta_ssid[0] = 0;
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, "S3Keys", 6);
    ap.ap.ssid_len = 6;
    memcpy(ap.ap.password, "12345678", 8);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.beacon_interval = 100;
    /* APSTA so portal /scan can still see home APs; explicit only — never from STA fail. */
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        return err;
    }
    err = wifi_ensure_started();
    if (err != ESP_OK) {
        return err;
    }
    strncpy(s_ip, "192.168.4.1", sizeof(s_ip) - 1);
    set_banner("AP", "192.168.4.1");
    ensure_httpd(1);
    ensure_tcp();
    ESP_LOGI(TAG, "AP S3Keys/12345678 portal http://192.168.4.1 TCP 2333 /api/v1");
    notify_wifi_status();
    return ESP_OK;
}

static esp_err_t start_sta_from_creds(const char *ssid, const char *pass)
{
    esp_err_t err = wifi_driver();
    if (err != ESP_OK) {
        return err;
    }
    /* Intentional home WiFi: tear SoftAP first (was auto-reopened after join fail). */
    s_wifi_pref = WIFI_PREF_STA;
    s_sta_ok = 0;
    s_last_disc = 0;
    s_sta_give_up = 0;
    s_sta_attempts = 0;
    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
    s_sta_ssid[sizeof(s_sta_ssid) - 1] = 0;
    strncpy(s_ip, "0.0.0.0", sizeof(s_ip) - 1);

    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, pass ? pass : "", sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;

    (void)esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(80));

    /* STA-only: SoftAP off while joining — BLE coexist is already tight. */
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode STA %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config STA %s", esp_err_to_name(err));
        return err;
    }
    err = wifi_ensure_started();
    if (err != ESP_OK) {
        return err;
    }
    s_sta_attempts = 1;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_connect %s", esp_err_to_name(err));
    }
    set_banner("STA", "...");
    ESP_LOGI(TAG, "STA join %s (AP off, max %d tries, no AP fallback)", ssid, STA_CONNECT_MAX);
    notify_wifi_status();
    return ESP_OK;
}

static void wifi_mgr(void *arg)
{
    (void)arg;
    if (wifi_stack_once() != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
    set_banner("off", "-");
    ESP_LOGI(TAG, "wifi idle (serial: wifi ap | wifi join SSID PASS); no auto-AP; STA max %d tries",
             STA_CONNECT_MAX);
    int retry_ticks = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!s_wifi_on) {
            retry_ticks = 0;
            continue;
        }
        poll_sta_ip();
        if (s_wifi_pref == WIFI_PREF_STA && !s_sta_ok && !s_sta_give_up) {
            retry_ticks++;
            if (retry_ticks >= STA_RETRY_TICKS) {
                retry_ticks = 0;
                if (s_sta_attempts >= STA_CONNECT_MAX) {
                    s_sta_give_up = 1;
                    set_banner("STA", "fail");
                    ESP_LOGW(TAG, "STA give up after %d tries ssid=%s last_disc=%u (wifi join again to retry)",
                             STA_CONNECT_MAX, s_sta_ssid, (unsigned)s_last_disc);
                    notify_wifi_status();
                } else {
                    s_sta_attempts++;
                    ESP_LOGI(TAG, "STA retry %d/%d ssid=%s last_disc=%u", s_sta_attempts, STA_CONNECT_MAX,
                             s_sta_ssid, (unsigned)s_last_disc);
                    notify_wifi_status();
                    (void)esp_wifi_connect();
                }
            }
        } else {
            retry_ticks = 0;
        }
    }
}

esp_err_t net_wifi_off(void)
{
    s_wifi_pref = WIFI_PREF_OFF;
    s_sta_ssid[0] = 0;
    s_last_disc = 0;
    s_sta_attempts = 0;
    s_sta_give_up = 0;
    if (s_wifi_on) {
        (void)esp_wifi_disconnect();
        esp_wifi_stop();
        ui_tft_radio_busy(false);
        s_wifi_on = 0;
        s_sta_ok = 0;
        ui_tft_set_net(NULL);
        set_banner("off", "0.0.0.0");
    }
    return ESP_OK;
}

esp_err_t net_wifi_ap(void)
{
    if (wifi_stack_once() != ESP_OK) {
        return ESP_FAIL;
    }
    return start_ap_portal();
}

esp_err_t net_wifi_sta(void)
{
    char ssid[33] = {0};
    char pass[65] = {0};
    if (!nvs_load_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) || !ssid[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    if (wifi_stack_once() != ESP_OK) {
        return ESP_FAIL;
    }
    return start_sta_from_creds(ssid, pass);
}

esp_err_t net_wifi_clear(void)
{
    nvs_clear_wifi();
    ESP_LOGI(TAG, "wifi creds cleared");
    return ESP_OK;
}

esp_err_t net_wifi_join(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pass && strlen(pass) > 63) {
        return ESP_ERR_INVALID_ARG;
    }
    if (wifi_stack_once() != ESP_OK) {
        return ESP_FAIL;
    }
    nvs_save_wifi(ssid, pass ? pass : "");
    return start_sta_from_creds(ssid, pass ? pass : "");
}

const char *net_wifi_mode_str(void)
{
    return s_mode;
}

const char *net_wifi_ip(void)
{
    return s_ip;
}

bool net_wifi_ready(void)
{
    return s_wifi_on != 0;
}

void net_cmd(const char *args)
{
    if (!args) {
        return;
    }
    while (*args == ' ') {
        args++;
    }
    if (strcmp(args, "sta") == 0) {
        if (net_wifi_sta() != ESP_OK) {
            ESP_LOGW(TAG, "no saved wifi; wifi join SSID PASS");
        }
        return;
    }
    if (strcmp(args, "ap") == 0 || strcmp(args, "on") == 0) {
        (void)net_wifi_ap();
        return;
    }
    if (strcmp(args, "off") == 0) {
        (void)net_wifi_off();
        return;
    }
    if (strcmp(args, "clear") == 0) {
        (void)net_wifi_clear();
        return;
    }
    if (strncmp(args, "join ", 5) == 0) {
        char ssid[33] = {0};
        char pass[65] = {0};
        const char *p = args + 5;
        while (*p == ' ') {
            p++;
        }
        /* "SSID with spaces" PASS  — or —  SSID words... PASS (last token = password) */
        if (*p == '"') {
            p++;
            size_t i = 0;
            while (*p && *p != '"' && i + 1 < sizeof(ssid)) {
                ssid[i++] = *p++;
            }
            ssid[i] = 0;
            if (*p == '"') {
                p++;
            }
            while (*p == ' ') {
                p++;
            }
            strncpy(pass, p, sizeof(pass) - 1);
        } else {
            const char *last_sp = strrchr(p, ' ');
            if (last_sp && last_sp > p) {
                size_t sl = (size_t)(last_sp - p);
                if (sl >= sizeof(ssid)) {
                    sl = sizeof(ssid) - 1;
                }
                memcpy(ssid, p, sl);
                ssid[sl] = 0;
                while (sl > 0 && ssid[sl - 1] == ' ') {
                    ssid[--sl] = 0;
                }
                strncpy(pass, last_sp + 1, sizeof(pass) - 1);
            } else {
                strncpy(ssid, p, sizeof(ssid) - 1);
            }
        }
        if (ssid[0]) {
            (void)net_wifi_join(ssid, pass);
        } else {
            ESP_LOGW(TAG, "wifi join need SSID");
        }
        return;
    }
    ESP_LOGW(TAG, "wifi ap | off | clear | join SSID PASS | sta");
    ESP_LOGW(TAG, "  SSID may contain spaces; last token is password");
    ESP_LOGW(TAG, "  or: wifi join \"My SSID\" password");
}

bool net_wifi_sta_ok(void)
{
    return s_sta_ok != 0;
}

unsigned net_wifi_last_disc_reason(void)
{
    return (unsigned)s_last_disc;
}

int net_wifi_phase(void)
{
    if (s_wifi_pref == WIFI_PREF_AP) {
        return NET_WIFI_PHASE_AP;
    }
    if (s_wifi_pref == WIFI_PREF_OFF || !s_wifi_on) {
        return NET_WIFI_PHASE_OFF;
    }
    if (s_sta_ok) {
        return NET_WIFI_PHASE_OK;
    }
    if (s_sta_give_up) {
        return NET_WIFI_PHASE_FAIL;
    }
    return NET_WIFI_PHASE_CONNECTING;
}

int net_wifi_attempts(void)
{
    return s_sta_attempts;
}

bool net_wifi_gave_up(void)
{
    return s_sta_give_up != 0;
}

const char *net_wifi_ssid(void)
{
    return s_sta_ssid;
}

void net_wifi_fill_status(uint8_t out[12])
{
    memset(out, 0, 12);
    out[0] = (uint8_t)net_wifi_phase();
    out[1] = (uint8_t)s_last_disc;
    out[2] = (uint8_t)s_sta_attempts;
    out[3] = s_sta_give_up ? 1 : 0;
    if (s_sta_ok && s_sta_netif) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_sta_netif, &info) == ESP_OK) {
            uint32_t a = info.ip.addr; /* LE on ESP */
            out[4] = (uint8_t)(a & 0xFF);
            out[5] = (uint8_t)((a >> 8) & 0xFF);
            out[6] = (uint8_t)((a >> 16) & 0xFF);
            out[7] = (uint8_t)((a >> 24) & 0xFF);
        }
    } else if (s_wifi_pref == WIFI_PREF_AP) {
        out[4] = 192;
        out[5] = 168;
        out[6] = 4;
        out[7] = 1;
    }
}

void net_wifi_set_status_cb(net_wifi_status_cb_t cb)
{
    s_wifi_status_cb = cb;
}

void net_dump_status(void)
{
    nk_printf("wifi=%s\n", s_mode);
    nk_printf("wifi_ip=%s\n", s_ip);
    nk_printf("wifi_tcp=%d\n", TCP_PORT);
    nk_printf("wifi_clients=%d\n", s_tcp_clients);
    nk_printf("wifi_sta_ok=%d\n", s_sta_ok ? 1 : 0);
    nk_printf("wifi_disc=%u\n", (unsigned)s_last_disc);
    nk_printf("wifi_ssid=%s\n", s_sta_ssid);
    nk_printf("wifi_try=%d/%d\n", s_sta_attempts, STA_CONNECT_MAX);
    nk_printf("wifi_give_up=%d\n", s_sta_give_up ? 1 : 0);
    nk_printf("wifi_pref=%s\n",
              s_wifi_pref == WIFI_PREF_STA ? "sta" : (s_wifi_pref == WIFI_PREF_AP ? "ap" : "off"));
}

esp_err_t net_link_start(net_line_cb_t on_line)
{
    s_cb = on_line;
    net_hook_stdio();
    xTaskCreatePinnedToCore(wifi_mgr, "wifi_mgr", 6144, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "wifi off at boot. TCP %d after wifi ap|join", TCP_PORT);
    return ESP_OK;
}
