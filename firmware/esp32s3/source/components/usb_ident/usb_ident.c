#include "usb_ident.h"

#include "board_pins.h"
#include "class/hid/hid_device.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb";

#define USB_VID 0x303A
#define USB_PID 0x40A1 /* not 0x1001 (USB-JTAG serial) */

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)

static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(1)),
};

static const char *hid_string_descriptor[] = {
    (char[]){0x09, 0x04},
    "S3Keys",
    "S3 Music Keys",
    "0001",
    "HID Keyboard",
};

static const tusb_desc_device_t s_dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = 64,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(hid_report_descriptor), 0x81, 16, 10),
};

static const uint8_t kmap[15] = {
    HID_KEY_1, HID_KEY_2, HID_KEY_3, HID_KEY_4, HID_KEY_5,
    HID_KEY_6, HID_KEY_7, HID_KEY_8, HID_KEY_9, HID_KEY_0,
    HID_KEY_Q, HID_KEY_W, HID_KEY_E, HID_KEY_R, HID_KEY_T,
};

static const char *kname[15] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "Q", "W", "E", "R", "T",
};

static usb_ident_mode_t s_mode = USB_IDENT_HID;
static volatile int s_boot_prev = 1;
static TickType_t s_hid_up_tick;
static bool s_hid_need_up;
static volatile int s_attached;
static bool s_tusb_ok;

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (!event) {
        return;
    }
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        s_attached = 1;
        ESP_LOGI(TAG, "host attached VID %04X PID %04X (%s)", USB_VID, USB_PID, usb_ident_name());
        printf("EVENT usb=mounted mode=%s vid=%04X pid=%04X\n", usb_ident_name(), USB_VID, USB_PID);
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        s_attached = 0;
        ESP_LOGI(TAG, "host detached");
        printf("EVENT usb=unmounted\n");
    }
}

usb_ident_mode_t usb_ident_mode(void)
{
    return s_mode;
}

const char *usb_ident_name(void)
{
    switch (s_mode) {
    case USB_IDENT_HID: return "hid";
    case USB_IDENT_MIDI: return "midi";
    default: return "off";
    }
}

int usb_ident_mounted(void)
{
    if (!s_tusb_ok) {
        return 0;
    }
    return (s_attached || tud_mounted()) ? 1 : 0;
}

void usb_ident_set_mode(usb_ident_mode_t mode)
{
    s_mode = mode;
    ESP_LOGI(TAG, "identity %s (native USB; flash still uses CH343 UART0)", usb_ident_name());
    printf("EVENT usb=mode %s mounted=%d\n", usb_ident_name(), usb_ident_mounted());
}

void usb_ident_cycle(void)
{
    usb_ident_set_mode((usb_ident_mode_t)((s_mode + 1) % 3));
}

static void hid_send(const uint8_t keycode[6])
{
    if (!s_tusb_ok || !tud_mounted() || !tud_hid_ready()) {
        return;
    }
    tud_hid_keyboard_report(1, 0, keycode);
}

void usb_ident_tap(uint8_t note0)
{
    if (note0 >= 15 || s_mode == USB_IDENT_OFF) {
        return;
    }
    if (s_mode == USB_IDENT_HID) {
        uint8_t keycode[6] = {0};
        keycode[0] = kmap[note0];
        hid_send(keycode);
        s_hid_need_up = true;
        s_hid_up_tick = xTaskGetTickCount() + pdMS_TO_TICKS(40);
        printf("EVENT hid key=%u code=%s mounted=%d\n",
               (unsigned)(note0 + 1), kname[note0], usb_ident_mounted());
        return;
    }
    unsigned midi = (unsigned)(note0 + 60);
    ESP_LOGI(TAG, "midi note %u (USB MIDI iface TBD)", midi);
    printf("EVENT midi note=%u vel=127 mounted=%d\n", midi, usb_ident_mounted());
}

static void boot_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BOOT_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    s_boot_prev = gpio_get_level(PIN_BOOT_BTN);
    for (;;) {
        int lv = gpio_get_level(PIN_BOOT_BTN);
        if (s_boot_prev == 1 && lv == 0) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (gpio_get_level(PIN_BOOT_BTN) == 0) {
                usb_ident_cycle();
                while (gpio_get_level(PIN_BOOT_BTN) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
        }
        s_boot_prev = lv;
        if (s_hid_need_up && (int32_t)(xTaskGetTickCount() - s_hid_up_tick) >= 0) {
            uint8_t none[6] = {0};
            hid_send(none);
            s_hid_need_up = false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t usb_ident_start(void)
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(usb_event_cb);
    tusb_cfg.descriptor.device = &s_dev_desc;
    tusb_cfg.descriptor.full_speed_config = hid_configuration_descriptor;
    tusb_cfg.descriptor.string = hid_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]);
#if TUD_OPT_HIGH_SPEED
    tusb_cfg.descriptor.high_speed_config = hid_configuration_descriptor;
#endif
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tinyusb: %s (CH343 debug still ok)", esp_err_to_name(err));
        s_tusb_ok = false;
    } else {
        s_tusb_ok = true;
        ESP_LOGI(TAG, "native USB HID 303A:40A1 — plug the S3 USB port, not CH343");
    }
    xTaskCreatePinnedToCore(boot_task, "bootbtn", 2048, NULL, 3, NULL, 0);
    return ESP_OK;
}
