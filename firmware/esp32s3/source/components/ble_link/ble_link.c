#include "ble_link.h"
#include "enc_fn.h"
#include "net_link.h"
#include "nk_out.h"
#include "sky_proto.h"
#include "sky_session.h"
#include "ui_tft.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "host/ble_gap.h"
#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

void ble_store_config_init(void);

static const char *TAG = "ble";
/* Locked HOGP (Android+iOS typed OK 2026-10-05): see docs/BLE_HID.md.
 * Do not put BLE-MIDI in GATT/adv, do not notify 9-byte reports, do not
 * gatt_changed on connect, do not encrypt the Report Map. */
#define DEV_NAME "SKY-music"

/* App service stays out of advertising so hosts still classify this as HID. */
static const ble_uuid128_t s_sky_service_uuid = BLE_UUID128_INIT(
    0x31, 0x30, 0x76, 0x2d, 0x43, 0x49, 0x53, 0x55, 0x4d, 0x59, 0x4b, 0x53, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_sky_keys_uuid = BLE_UUID128_INIT(
    0x31, 0x30, 0x76, 0x2d, 0x43, 0x49, 0x53, 0x55, 0x4d, 0x59, 0x4b, 0x53, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_sky_map_uuid = BLE_UUID128_INIT(
    0x31, 0x30, 0x76, 0x2d, 0x43, 0x49, 0x53, 0x55, 0x4d, 0x59, 0x4b, 0x53, 0x03, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_sky_caps_uuid = BLE_UUID128_INIT(
    0x31, 0x30, 0x76, 0x2d, 0x43, 0x49, 0x53, 0x55, 0x4d, 0x59, 0x4b, 0x53, 0x04, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_sky_cmd_uuid = BLE_UUID128_INIT(
    0x31, 0x30, 0x76, 0x2d, 0x43, 0x49, 0x53, 0x55, 0x4d, 0x59, 0x4b, 0x53, 0x05, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_sky_result_uuid = BLE_UUID128_INIT(
    0x31, 0x30, 0x76, 0x2d, 0x43, 0x49, 0x53, 0x55, 0x4d, 0x59, 0x4b, 0x53, 0x06, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_sky_state_uuid = BLE_UUID128_INIT(
    0x31, 0x30, 0x76, 0x2d, 0x43, 0x49, 0x53, 0x55, 0x4d, 0x59, 0x4b, 0x53, 0x07, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_sky_session_uuid = BLE_UUID128_INIT(
    0x31, 0x30, 0x76, 0x2d, 0x43, 0x49, 0x53, 0x55, 0x4d, 0x59, 0x4b, 0x53, 0x08, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_sky_wifi_uuid = BLE_UUID128_INIT(
    0x31, 0x30, 0x76, 0x2d, 0x43, 0x49, 0x53, 0x55, 0x4d, 0x59, 0x4b, 0x53, 0x09, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_sky_score_list_uuid = BLE_UUID128_INIT(
    0x31, 0x30, 0x76, 0x2d, 0x43, 0x49, 0x53, 0x55, 0x4d, 0x59, 0x4b, 0x53, 0x0a, 0x00, 0x40, 0x6e);

#define HID_USAGE_Y 0x1C
#define HID_USAGE_U 0x18
#define HID_USAGE_I 0x0C
#define HID_USAGE_O 0x12
#define HID_USAGE_P 0x13
#define HID_USAGE_H 0x0B
#define HID_USAGE_J 0x0D
#define HID_USAGE_K 0x0E
#define HID_USAGE_L 0x0F
#define HID_USAGE_SEMI 0x33
#define HID_USAGE_N 0x11
#define HID_USAGE_M 0x10
#define HID_USAGE_COMMA 0x36
#define HID_USAGE_DOT 0x37
#define HID_USAGE_SLASH 0x38
#define HID_USAGE_1 0x1E
#define HID_USAGE_2 0x1F
#define HID_USAGE_3 0x20
#define HID_USAGE_4 0x21
#define HID_USAGE_5 0x22
#define HID_USAGE_6 0x23
#define HID_USAGE_7 0x24
#define HID_USAGE_8 0x25
#define HID_USAGE_9 0x26
#define HID_USAGE_0 0x27
#define HID_USAGE_Q 0x14
#define HID_USAGE_W 0x1A
#define HID_USAGE_E 0x08
#define HID_USAGE_R 0x15
#define HID_USAGE_T 0x17

/* Sky PC 3x5: Y U I O P / H J K L ; / N M , . / */
static const uint8_t kmap_sky[15] = {
    HID_USAGE_Y, HID_USAGE_U, HID_USAGE_I, HID_USAGE_O, HID_USAGE_P,
    HID_USAGE_H, HID_USAGE_J, HID_USAGE_K, HID_USAGE_L, HID_USAGE_SEMI,
    HID_USAGE_N, HID_USAGE_M, HID_USAGE_COMMA, HID_USAGE_DOT, HID_USAGE_SLASH,
};
static const uint8_t kmap_num[15] = {
    HID_USAGE_1, HID_USAGE_2, HID_USAGE_3, HID_USAGE_4, HID_USAGE_5,
    HID_USAGE_6, HID_USAGE_7, HID_USAGE_8, HID_USAGE_9, HID_USAGE_0,
    HID_USAGE_Q, HID_USAGE_W, HID_USAGE_E, HID_USAGE_R, HID_USAGE_T,
};

#define HID_KEY_SLOTS 15
#define HID_RPT_LEN   (2 + HID_KEY_SLOTS)
#define HID_BOOT_LEN  8

/* Same map as SkBleKeyboard / NimBLEHIDDevice: Report ID 1, 8-byte payload
 * over GATT (ID is in 0x2908, not in the notify). iPad HID needs this. */
static const uint8_t hid_report_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05,
    0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
    /* Battery System Remaining Capacity, Report ID 2 — so HID host can show % */
    0x05, 0x84, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x02, 0x05, 0x85, 0x09, 0x65, 0x15, 0x00, 0x25, 0x64,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0xC0,
};
static const uint8_t s_bat_cpf[7] = {0x04, 0x00, 0xAD, 0x27, 0x01, 0x00, 0x00}; /* uint8 percent */
static const uint8_t s_ext_bat[2] = {0x19, 0x2A};                               /* 0x2A19 */

static int s_inited;
static volatile int s_adv;
static uint8_t s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static int s_map_sky = 1;
static uint8_t s_hid_rpt[HID_RPT_LEN];
static uint8_t s_hid_boot[HID_BOOT_LEN];
static uint16_t s_h_hid_inp;
static uint16_t s_h_hid_boot;
static uint16_t s_h_hid_bat;
static uint16_t s_h_bat;
static uint16_t s_h_sky_keys;
static uint16_t s_h_sky_result;
static uint16_t s_h_sky_state;
static uint16_t s_h_sky_session;
static uint16_t s_h_sky_wifi;
static uint16_t s_h_sky_score_list;
static uint16_t s_sky_subscribed;
static uint16_t s_sky_result_sub;
static uint16_t s_sky_state_sub;
static uint16_t s_sky_session_sub;
static uint16_t s_sky_wifi_sub;
static uint16_t s_sky_score_list_sub;
static uint16_t s_sky_down_mask;
static sky_result_v1_t s_sky_last_result;
static uint8_t s_hid_info[4] = {0x11, 0x01, 0x00, 0x01}; /* 1.11, RemoteWake — SkBleKeyboard hidInfo(0,1) */
static uint8_t s_proto = 1; /* Report protocol default (NimBLEHIDDevice / HOGP) */
static uint8_t s_ctrl_pt;
static uint8_t s_led_rpt;
static uint8_t s_pnp[7] = {0x02, 0x02, 0xE5, 0x11, 0xA1, 0x10, 0x02}; /* USB VID 0xE502, PID 0xA111 */
static const char *s_mfg = "SKY-music";
static const char *s_model = "SKY-kb";
static const char *s_fwrev = "1.1";
static uint8_t s_bat = 100;

static int gatt_acc(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {.uuid = BLE_UUID16_DECLARE(0x2A29),
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ},
                {.uuid = BLE_UUID16_DECLARE(0x2A24),
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ},
                {.uuid = BLE_UUID16_DECLARE(0x2A50),
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ},
                {.uuid = BLE_UUID16_DECLARE(0x2A26),
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ},
                {0},
            },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {.uuid = BLE_UUID16_DECLARE(0x2A19),
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                 .val_handle = &s_h_bat,
                 .descriptors =
                     (struct ble_gatt_dsc_def[]){
                         {.uuid = BLE_UUID16_DECLARE(0x2904),
                          .att_flags = BLE_ATT_F_READ,
                          .access_cb = gatt_acc},
                         {0},
                     }},
                {0},
            },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {.uuid = BLE_UUID16_DECLARE(0x2A4A),
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ},
                {.uuid = BLE_UUID16_DECLARE(0x2A4B),
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ,
                 .descriptors =
                     (struct ble_gatt_dsc_def[]){
                         {.uuid = BLE_UUID16_DECLARE(0x2907),
                          .att_flags = BLE_ATT_F_READ,
                          .access_cb = gatt_acc},
                         {0},
                     }},
                {.uuid = BLE_UUID16_DECLARE(0x2A4C),
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_WRITE_NO_RSP},
                {.uuid = BLE_UUID16_DECLARE(0x2A4E),
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
                {.uuid = BLE_UUID16_DECLARE(0x2A22),
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_NOTIFY,
                 .val_handle = &s_h_hid_boot},
                {.uuid = BLE_UUID16_DECLARE(0x2A32),
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
                {.uuid = BLE_UUID16_DECLARE(0x2A4D),
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
                 .val_handle = &s_h_hid_inp,
                 .descriptors =
                     (struct ble_gatt_dsc_def[]){
                         {.uuid = BLE_UUID16_DECLARE(0x2908),
                          .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                          .access_cb = gatt_acc,
                          .arg = (void *)(uintptr_t)0x0101},
                         {0},
                     }},
                {.uuid = BLE_UUID16_DECLARE(0x2A4D),
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
                 .val_handle = &s_h_hid_bat,
                 .descriptors =
                     (struct ble_gatt_dsc_def[]){
                         {.uuid = BLE_UUID16_DECLARE(0x2908),
                          .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                          .access_cb = gatt_acc,
                          .arg = (void *)(uintptr_t)0x0201},
                         {0},
                     }},
                {.uuid = BLE_UUID16_DECLARE(0x2A4D),
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                          BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC,
                 .descriptors =
                     (struct ble_gatt_dsc_def[]){
                         {.uuid = BLE_UUID16_DECLARE(0x2908),
                          .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                          .access_cb = gatt_acc,
                          .arg = (void *)(uintptr_t)0x0102},
                         {0},
                     }},
                {0},
            },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_sky_service_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {.uuid = &s_sky_keys_uuid.u,
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_NOTIFY,
                 .val_handle = &s_h_sky_keys},
                {.uuid = &s_sky_map_uuid.u,
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC},
                {.uuid = &s_sky_caps_uuid.u,
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ},
                {.uuid = &s_sky_cmd_uuid.u,
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC},
                {.uuid = &s_sky_result_uuid.u,
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                 .val_handle = &s_h_sky_result},
                {.uuid = &s_sky_state_uuid.u,
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                 .val_handle = &s_h_sky_state},
                {.uuid = &s_sky_session_uuid.u,
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
                 .val_handle = &s_h_sky_session},
                {.uuid = &s_sky_wifi_uuid.u,
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                 .val_handle = &s_h_sky_wifi},
                {.uuid = &s_sky_score_list_uuid.u,
                 .access_cb = gatt_acc,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                 .val_handle = &s_h_sky_score_list},
                {0},
            },
    },
    {0},
};

static void notify_bytes(uint16_t handle, const void *data, uint16_t len);
static void hid_rebuild(void);
static void hid_notify(void);

static void sky_key_notify(uint8_t key, int down)
{
    if (!s_sky_subscribed) {
        return;
    }
    uint8_t event[4] = {key, down ? 1 : 0, s_map_sky ? 1 : 0, 0};
    notify_bytes(s_h_sky_keys, event, sizeof(event));
}

static void sky_set_map(int sky)
{
    if (s_map_sky == sky) {
        return;
    }
    for (uint8_t key = 0; key < 15; ++key) {
        if (s_sky_down_mask & (uint16_t)(1u << key)) {
            sky_key_notify(key, 0);
        }
    }
    s_sky_down_mask = 0;
    hid_rebuild();
    hid_notify();
    s_map_sky = sky;
    sky_proto_bump();
}

static int sky_map_get_op(void)
{
    return s_map_sky ? 1 : 0;
}

static void sky_map_set_op(int sky)
{
    sky_set_map(sky ? 1 : 0);
}

static void sky_notify_state(void)
{
    if (!s_sky_state_sub) {
        return;
    }
    sky_state_v1_t st;
    sky_proto_fill_state(&st);
    notify_bytes(s_h_sky_state, &st, sizeof(st));
}

static void sky_notify_result(const sky_result_v1_t *r)
{
    if (!r) {
        return;
    }
    s_sky_last_result = *r;
    if (s_sky_result_sub) {
        notify_bytes(s_h_sky_result, r, sizeof(*r));
    }
}

static void sky_notify_session(void)
{
    uint8_t buf[SKY_SESSION_GATT_SIZE];
    sky_session_fill_gatt(buf);
    if (s_sky_session_sub) {
        notify_bytes(s_h_sky_session, buf, sizeof(buf));
    }
}

static void sky_notify_wifi(void)
{
    if (!s_sky_wifi_sub) {
        return;
    }
    uint8_t buf[12];
    net_wifi_fill_status(buf);
    notify_bytes(s_h_sky_wifi, buf, sizeof(buf));
}

static void sky_notify_score_list(void)
{
    if (!s_sky_score_list_sub) {
        return;
    }
    uint8_t buf[240];
    size_t n = sky_proto_fill_score_list(buf, sizeof(buf));
    if (n >= 2) {
        notify_bytes(s_h_sky_score_list, buf, (uint16_t)n);
    }
}

static void on_local_score_list_changed(void)
{
    sky_proto_refresh_score_list();
    sky_notify_score_list();
    sky_notify_state();
}

static int sky_gatt_acc(uint16_t attr, struct ble_gatt_access_ctxt *ctxt)
{
    const ble_uuid_t *uuid = ctxt->chr ? ctxt->chr->uuid : NULL;
    if (!uuid) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (ble_uuid_cmp(uuid, &s_sky_map_uuid.u) == 0) {
            const char *map = s_map_sky ? "sky" : "num";
            return os_mbuf_append(ctxt->om, map, 3) == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (ble_uuid_cmp(uuid, &s_sky_caps_uuid.u) == 0) {
            /* major=1 minor=6 flags=key|map|cmd|wifi|wifi_status|ble_score  keys=15 ch32=7 */
            static const uint8_t caps[6] = {1, 6, 0x3F, 0, 15, 7};
            return os_mbuf_append(ctxt->om, caps, sizeof(caps)) == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (ble_uuid_cmp(uuid, &s_sky_result_uuid.u) == 0) {
            return os_mbuf_append(ctxt->om, &s_sky_last_result, sizeof(s_sky_last_result)) == 0
                       ? 0
                       : BLE_ATT_ERR_UNLIKELY;
        }
        if (ble_uuid_cmp(uuid, &s_sky_state_uuid.u) == 0) {
            sky_state_v1_t st;
            sky_proto_fill_state(&st);
            return os_mbuf_append(ctxt->om, &st, sizeof(st)) == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (ble_uuid_cmp(uuid, &s_sky_session_uuid.u) == 0) {
            uint8_t buf[SKY_SESSION_GATT_SIZE];
            sky_session_fill_gatt(buf);
            return os_mbuf_append(ctxt->om, buf, sizeof(buf)) == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (ble_uuid_cmp(uuid, &s_sky_wifi_uuid.u) == 0) {
            uint8_t buf[12];
            net_wifi_fill_status(buf);
            return os_mbuf_append(ctxt->om, buf, sizeof(buf)) == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
        if (ble_uuid_cmp(uuid, &s_sky_score_list_uuid.u) == 0) {
            uint8_t buf[240];
            size_t n = sky_proto_fill_score_list(buf, sizeof(buf));
            return os_mbuf_append(ctxt->om, buf, n) == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
        }
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
        ble_uuid_cmp(uuid, &s_sky_map_uuid.u) == 0) {
        uint8_t map[3];
        if (OS_MBUF_PKTLEN(ctxt->om) != sizeof(map) ||
            ble_hs_mbuf_to_flat(ctxt->om, map, sizeof(map), NULL) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (memcmp(map, "sky", 3) == 0) {
            sky_set_map(1);
        } else if (memcmp(map, "num", 3) == 0) {
            sky_set_map(0);
        } else {
            return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        }
        sky_notify_state();
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
        ble_uuid_cmp(uuid, &s_sky_cmd_uuid.u) == 0) {
        uint8_t buf[SKY_CMD_MAX_SIZE];
        uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
        if (n < SKY_CMD_HDR_SIZE || n > sizeof(buf) ||
            ble_hs_mbuf_to_flat(ctxt->om, buf, n, NULL) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        sky_result_v1_t r;
        if (sky_proto_exec(buf, n, &r) != ESP_OK) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        sky_notify_result(&r);
        if (r.result_code == SKY_RC_OK || r.opcode == SKY_OP_GET_SNAPSHOT) {
            sky_notify_state();
        }
        if (r.result_code == SKY_RC_OK && r.opcode == SKY_OP_SESSION_TOKEN) {
            sky_notify_session();
        }
        if (r.result_code == SKY_RC_OK && r.opcode == SKY_OP_SCORE_FILE) {
            sky_notify_score_list();
        }
        return 0;
    }
    (void)attr;
    return BLE_ATT_ERR_UNLIKELY;
}

static int gatt_acc(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)arg;
    if (ctxt->chr && ctxt->chr->uuid && ctxt->chr->uuid->type == BLE_UUID_TYPE_128) {
        return sky_gatt_acc(attr, ctxt);
    }
    uint16_t uuid = 0;
    if (ctxt->chr && ctxt->chr->uuid && ctxt->chr->uuid->type == BLE_UUID_TYPE_16) {
        uuid = ble_uuid_u16(ctxt->chr->uuid);
    } else if (ctxt->dsc && ctxt->dsc->uuid && ctxt->dsc->uuid->type == BLE_UUID_TYPE_16) {
        uuid = ble_uuid_u16(ctxt->dsc->uuid);
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (uuid == 0x2A29) {
            os_mbuf_append(ctxt->om, s_mfg, strlen(s_mfg));
            return 0;
        }
        if (uuid == 0x2A24) {
            os_mbuf_append(ctxt->om, s_model, strlen(s_model));
            return 0;
        }
        if (uuid == 0x2A26) {
            os_mbuf_append(ctxt->om, s_fwrev, strlen(s_fwrev));
            return 0;
        }
        if (uuid == 0x2A50) {
            os_mbuf_append(ctxt->om, s_pnp, sizeof(s_pnp));
            return 0;
        }
        if (uuid == 0x2A19) {
            os_mbuf_append(ctxt->om, &s_bat, 1);
            return 0;
        }
        if (uuid == 0x2A4A) {
            os_mbuf_append(ctxt->om, s_hid_info, 4);
            return 0;
        }
        if (uuid == 0x2A4B) {
            os_mbuf_append(ctxt->om, hid_report_map, sizeof(hid_report_map));
            return 0;
        }
        if (uuid == 0x2A4E) {
            os_mbuf_append(ctxt->om, &s_proto, 1);
            return 0;
        }
        if (uuid == 0x2A4D && attr == s_h_hid_inp) {
            os_mbuf_append(ctxt->om, s_hid_boot, sizeof(s_hid_boot));
            return 0;
        }
        if (uuid == 0x2A4D && attr == s_h_hid_bat) {
            os_mbuf_append(ctxt->om, &s_bat, 1);
            return 0;
        }
        if (uuid == 0x2A22) {
            os_mbuf_append(ctxt->om, s_hid_boot, sizeof(s_hid_boot));
            return 0;
        }
        if (uuid == 0x2A32) {
            os_mbuf_append(ctxt->om, &s_led_rpt, 1);
            return 0;
        }
        if (uuid == 0x2A4D) {
            os_mbuf_append(ctxt->om, &s_led_rpt, 1);
            return 0;
        }
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC && uuid == 0x2908) {
        uintptr_t v = (uintptr_t)arg;
        uint8_t ref[2] = {(uint8_t)(v >> 8), (uint8_t)(v & 0xFF)};
        os_mbuf_append(ctxt->om, ref, 2);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC && uuid == 0x2904) {
        os_mbuf_append(ctxt->om, s_bat_cpf, sizeof(s_bat_cpf));
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC && uuid == 0x2907) {
        os_mbuf_append(ctxt->om, s_ext_bat, sizeof(s_ext_bat));
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[16];
        if (n > sizeof(buf)) {
            n = sizeof(buf);
        }
        ble_hs_mbuf_to_flat(ctxt->om, buf, n, NULL);
        if (uuid == 0x2A4C && n) {
            s_ctrl_pt = buf[0];
            return 0;
        }
        if (uuid == 0x2A4E && n) {
            s_proto = buf[0] ? 1 : 0;
            return 0;
        }
        if ((uuid == 0x2A32 ||
             (uuid == 0x2A4D && attr != s_h_hid_inp && attr != s_h_hid_bat)) &&
            n) {
            s_led_rpt = buf[0];
            return 0;
        }
        return 0;
    }
    return 0;
}

static void notify_bytes(uint16_t handle, const void *data, uint16_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !handle) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        return;
    }
    int rc = ble_gatts_notify_custom(s_conn, handle, om);
    if (rc != 0) {
        ESP_LOGD(TAG, "notify h=%u rc=%d", handle, rc);
    }
}

static void hid_rebuild(void)
{
    memset(s_hid_rpt, 0, sizeof(s_hid_rpt));
    memset(s_hid_boot, 0, sizeof(s_hid_boot));
}

static void hid_notify(void)
{
    s_hid_boot[0] = s_hid_rpt[0];
    s_hid_boot[1] = 0;
    memcpy(&s_hid_boot[2], &s_hid_rpt[2], 6);
    notify_bytes(s_h_hid_inp, s_hid_boot, sizeof(s_hid_boot));
    notify_bytes(s_h_hid_boot, s_hid_boot, sizeof(s_hid_boot));
}

void ble_link_set_battery(uint8_t soc)
{
    if (soc > 100) {
        soc = 100;
    }
    if (soc == s_bat) {
        return;
    }
    s_bat = soc;
    notify_bytes(s_h_bat, &s_bat, 1);
    notify_bytes(s_h_hid_bat, &s_bat, 1);
}

static const uint8_t *cur_map(void)
{
    return s_map_sky ? kmap_sky : kmap_num;
}

static void hid_set_key(uint8_t usage, int down)
{
    const int last = HID_RPT_LEN;
    if (!usage) {
        return;
    }
    if (down) {
        for (int i = 2; i < last; ++i) {
            if (s_hid_rpt[i] == usage) {
                return;
            }
        }
        for (int i = 2; i < last; ++i) {
            if (s_hid_rpt[i] == 0) {
                s_hid_rpt[i] = usage;
                break;
            }
        }
    } else {
        for (int i = 2; i < last; ++i) {
            if (s_hid_rpt[i] == usage) {
                s_hid_rpt[i] = 0;
            }
        }
        uint8_t tmp[HID_KEY_SLOTS] = {0};
        int n = 0;
        for (int i = 2; i < last; ++i) {
            if (s_hid_rpt[i] && n < HID_KEY_SLOTS) {
                tmp[n++] = s_hid_rpt[i];
            }
        }
        memcpy(&s_hid_rpt[2], tmp, HID_KEY_SLOTS);
    }
    hid_notify();
}

void ble_link_note(uint8_t note_id, int down)
{
    if (note_id > 14) {
        return;
    }
    hid_set_key(cur_map()[note_id], down);
    if (down) {
        s_sky_down_mask |= (uint16_t)(1u << note_id);
    } else {
        s_sky_down_mask &= (uint16_t)~(1u << note_id);
    }
    sky_key_notify(note_id, down);
}

static void ble_advertise(void);

static int on_passkey(struct ble_gap_event *event)
{
    struct ble_sm_io pkey = {0};
    pkey.action = event->passkey.params.action;
    if (pkey.action == BLE_SM_IOACT_NUMCMP) {
        pkey.numcmp_accept = 1;
        ESP_LOGI(TAG, "pair confirm %" PRIu32, event->passkey.params.numcmp);
    } else if (pkey.action == BLE_SM_IOACT_DISP) {
        pkey.passkey = 0;
        ESP_LOGI(TAG, "pair display unused");
    } else if (pkey.action == BLE_SM_IOACT_INPUT) {
        pkey.passkey = 0;
        ESP_LOGW(TAG, "pair input unused");
    } else if (pkey.action == BLE_SM_IOACT_OOB) {
        memset(pkey.oob, 0, sizeof(pkey.oob));
    } else if (pkey.action == BLE_SM_IOACT_NONE) {
        return 0;
    }
    int rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
    ESP_LOGI(TAG, "pair inject rc=%d action=%d", rc, pkey.action);
    return 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            uint16_t neu = event->connect.conn_handle;
            if (s_conn != BLE_HS_CONN_HANDLE_NONE && s_conn != neu) {
                uint16_t old = s_conn;
                s_conn = neu;
                ESP_LOGI(TAG, "switch host %u -> %u", old, neu);
                ble_gap_terminate(old, BLE_ERR_REM_USER_CONN_TERM);
            } else {
                s_conn = neu;
                ESP_LOGI(TAG, "connected handle=%u", s_conn);
            }
            s_adv = 0;
            (void)ble_gap_adv_stop();
            hid_rebuild();
            s_sky_down_mask = 0;
            s_sky_subscribed = 0;
            s_sky_result_sub = 0;
            s_sky_state_sub = 0;
            s_sky_session_sub = 0;
            s_sky_wifi_sub = 0;
            s_sky_score_list_sub = 0;
            ui_tft_radio_busy(true);
            (void)ble_att_set_preferred_mtu(247);
            /* SkBleKeyboard: startSecurity on connect so iOS bonds as HID. */
            int rc = ble_gap_security_initiate(s_conn);
            ESP_LOGI(TAG, "security_initiate rc=%d", rc);
        } else {
            if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
                ble_advertise();
            }
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected handle=%u reason=%d", event->disconnect.conn.conn_handle,
                 event->disconnect.reason);
        if (event->disconnect.conn.conn_handle == s_conn) {
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            hid_rebuild();
            s_sky_down_mask = 0;
            s_sky_subscribed = 0;
            s_sky_result_sub = 0;
            s_sky_state_sub = 0;
            s_sky_session_sub = 0;
            s_sky_wifi_sub = 0;
            s_sky_score_list_sub = 0;
            ui_tft_radio_busy(false);
            ble_advertise();
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
            ble_advertise();
        }
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe attr=%u notify=%d", event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        if (event->subscribe.attr_handle == s_h_sky_keys) {
            s_sky_subscribed = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_h_sky_result) {
            s_sky_result_sub = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_h_sky_state) {
            s_sky_state_sub = event->subscribe.cur_notify;
            if (s_sky_state_sub) {
                sky_notify_state();
            }
        } else if (event->subscribe.attr_handle == s_h_sky_session) {
            s_sky_session_sub = event->subscribe.cur_notify;
            if (s_sky_session_sub) {
                sky_notify_session();
            }
        } else if (event->subscribe.attr_handle == s_h_sky_wifi) {
            s_sky_wifi_sub = event->subscribe.cur_notify;
            if (s_sky_wifi_sub) {
                sky_notify_wifi();
            }
        } else if (event->subscribe.attr_handle == s_h_sky_score_list) {
            s_sky_score_list_sub = event->subscribe.cur_notify;
            if (s_sky_score_list_sub) {
                sky_notify_score_list();
            }
        }
        if (event->subscribe.cur_notify &&
            (event->subscribe.attr_handle == s_h_bat || event->subscribe.attr_handle == s_h_hid_bat)) {
            notify_bytes(event->subscribe.attr_handle, &s_bat, 1);
        }
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "enc status=%d", event->enc_change.status);
        if (event->enc_change.status == 0) {
            notify_bytes(s_h_bat, &s_bat, 1);
            notify_bytes(s_h_hid_bat, &s_bat, 1);
        } else {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            ESP_LOGW(TAG, "enc failed, drop this phone's bond");
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        ESP_LOGW(TAG, "repeat pairing, drop old bond");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        return on_passkey(event);
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu %u", event->mtu.value);
        return 0;
    default:
        return 0;
    }
}

static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)DEV_NAME;
    fields.name_len = (uint8_t)strlen(DEV_NAME);
    fields.name_is_complete = 1;
    fields.appearance = 0x03C1; /* keyboard */
    fields.appearance_is_present = 1;
    static ble_uuid16_t uu16[2];
    uu16[0] = (ble_uuid16_t)BLE_UUID16_INIT(0x1812);
    uu16[1] = (ble_uuid16_t)BLE_UUID16_INIT(0x180F);
    fields.uuids16 = uu16;
    fields.num_uuids16 = 2;
    fields.uuids16_is_complete = 1;
    static uint8_t bat_sd[3];
    bat_sd[0] = 0x0F;
    bat_sd[1] = 0x18;
    bat_sd[2] = s_bat;
    fields.svc_data_uuid16 = bat_sd;
    fields.svc_data_uuid16_len = 3;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv fields %d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (uint8_t *)DEV_NAME;
    rsp.name_len = (uint8_t)strlen(DEV_NAME);
    rsp.name_is_complete = 1;
    (void)ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params advp;
    memset(&advp, 0, sizeof(advp));
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;
    advp.itvl_min = BLE_GAP_ADV_ITVL_MS(80);
    advp.itvl_max = BLE_GAP_ADV_ITVL_MS(120);
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &advp, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "adv start %d", rc);
        return;
    }
    s_adv = 1;
    ui_tft_radio_busy(true);
    ESP_LOGI(TAG, "adv %s HID keyboard map=%s", DEV_NAME, s_map_sky ? "sky" : "num");
}

static void on_sync(void)
{
    /* New identity vs stuck SKY-music / SKYk3 bonds (bumped for v1.6 GATT table). Byte 5 two MSBs set. */
    static const uint8_t rnd[6] = {0xE3, 0x6D, 0x59, 0x53, 0x6B, 0xC7};
    int rc = ble_hs_id_set_rnd(rnd);
    if (rc == 0) {
        s_own_addr_type = BLE_OWN_ADDR_RANDOM;
    } else {
        ESP_LOGW(TAG, "rnd addr %d, public", rc);
        (void)ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    ble_advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble reset %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int gatt_svr_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        return rc;
    }
    return ble_gatts_add_svcs(s_svcs);
}

static void ble_on(void)
{
    if (s_adv || s_conn != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    if (!s_inited) {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            err = nvs_flash_init();
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nvs %s", esp_err_to_name(err));
            return;
        }
        nimble_port_init();
        ble_hs_cfg.sync_cb = on_sync;
        ble_hs_cfg.reset_cb = on_reset;
        ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
        /* Just Works + LE SC + bond — SkBleKeyboard iOS/iPadOS path. */
        ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_mitm = 0;
        ble_hs_cfg.sm_sc = 1;
        ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_svc_gap_device_name_set(DEV_NAME);
        int rc = gatt_svr_init();
        if (rc != 0) {
            ESP_LOGE(TAG, "gatt init %d", rc);
            return;
        }
        ble_svc_gap_device_appearance_set(0x03C1); /* keyboard, after GAP service exists */
        ble_store_config_init();
        nimble_port_freertos_init(host_task);
        s_inited = 1;
        return;
    }
    ble_advertise();
}

static void ble_off(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
    }
    if (s_adv) {
        ble_gap_adv_stop();
        s_adv = 0;
    }
    ui_tft_radio_busy(false);
    ESP_LOGI(TAG, "ble off");
}

void ble_cmd(const char *args)
{
    if (!args) {
        return;
    }
    while (*args == ' ') {
        args++;
    }
    if (strcmp(args, "on") == 0 || strcmp(args, "adv") == 0) {
        ble_on();
        return;
    }
    if (strcmp(args, "off") == 0) {
        ble_off();
        return;
    }
    if (strcmp(args, "map sky") == 0) {
        sky_set_map(1);
        ESP_LOGI(TAG, "map sky YUIOP/HJKL;/NM,./");
        return;
    }
    if (strcmp(args, "map num") == 0) {
        sky_set_map(0);
        ESP_LOGI(TAG, "map num 12345/67890/QWERT");
        return;
    }
    ESP_LOGW(TAG, "ble on | off | map sky | map num");
}

void ble_dump_status(void)
{
    nk_printf("ble=%s\n", s_conn != BLE_HS_CONN_HANDLE_NONE ? "conn" : (s_adv ? "adv" : "off"));
    nk_printf("ble_name=%s\n", DEV_NAME);
    nk_printf("ble_map=%s\n", s_map_sky ? "sky" : "num");
    nk_printf("ble_bat=%u\n", (unsigned)s_bat);
}

static void state_watch_task(void *arg)
{
    (void)arg;
    sky_state_v1_t last;
    memset(&last, 0, sizeof(last));
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(300));
        if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_sky_state_sub) {
            continue;
        }
        sky_state_v1_t cur;
        sky_proto_fill_state(&cur);
        int title_chg = strncmp(cur.title, last.title, sizeof(cur.title)) != 0;
        int progress_chg = (cur.transport == 1 || cur.transport == 2) &&
                           (cur.play_ms / 500u != last.play_ms / 500u || cur.dur_ms != last.dur_ms);
        if (cur.mode != last.mode || cur.map != last.map || cur.volume != last.volume ||
            cur.transpose != last.transpose || cur.tempo_pct != last.tempo_pct ||
            cur.transport != last.transport || cur.follow_state != last.follow_state ||
            cur.follow_at != last.follow_at || cur.follow_total != last.follow_total ||
            cur.expect_mask != last.expect_mask || cur.bat_pct != last.bat_pct ||
            cur.chg_flags != last.chg_flags || title_chg || progress_chg) {
            sky_proto_bump();
            sky_proto_fill_state(&cur);
            last = cur;
            notify_bytes(s_h_sky_state, &cur, sizeof(cur));
        }
    }
}

esp_err_t ble_link_start(void)
{
    memset(&s_sky_last_result, 0, sizeof(s_sky_last_result));
    s_sky_last_result.ver = SKY_PROTO_VER;
    sky_proto_set_map_ops(sky_map_get_op, sky_map_set_op);
    sky_session_set_changed_cb(sky_notify_session);
    net_wifi_set_status_cb(sky_notify_wifi);
    enc_fn_set_list_changed_cb(on_local_score_list_changed);
    hid_rebuild();
    ble_on();
    xTaskCreatePinnedToCore(state_watch_task, "ble_st", 3072, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "BLE HID+GATT advertising as %s (proto 1.7 now-playing)", DEV_NAME);
    return ESP_OK;
}
