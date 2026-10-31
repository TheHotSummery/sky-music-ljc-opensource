#include "nk_out.h"
#include "audio_bank.h"
#include "audio_i2s.h"
#include "board_pins.h"
#include "board_power.h"
#include "drill.h"
#include "ble_link.h"
#include "encoder.h"
#include "enc_fn.h"
#include "key_link.h"
#include "mixer.h"
#include "rgb_led.h"
#include "net_link.h"
#include "sd_music.h"
#include "score_play.h"
#include "ui_tft.h"
#include "touch_cst816.h"
#include "mic_i2s.h"
#include "sky_proto.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app";
static SemaphoreHandle_t s_cmd_mu;
static TaskHandle_t s_status_task;

static void send_play(unsigned one_based, bool sine)
{
    if (one_based < 1 || one_based > AUDIO_BANK_WAV_COUNT) {
        return;
    }
    MixCmd cmd = {
        .type = sine ? MIX_CMD_TEST_SINE : MIX_CMD_NOTE_ON,
        .note_id = (uint8_t)(one_based - 1),
        .velocity = 127,
    };
    mixer_submit(&cmd);
}

static void send_off(unsigned one_based)
{
    MixCmd cmd = {
        .type = MIX_CMD_NOTE_OFF,
        .note_id = (uint8_t)(one_based ? one_based - 1 : 0),
        .velocity = 0,
    };
    mixer_submit(&cmd);
}

static void dump_status(void)
{
    /* USB-JTAG UART echo of a full status dump underruns I2S (buzz). TCP tap still gets it. */
    nk_out_set_uart_echo(0);
    nk_printf("BEGIN_STATUS\n");
    nk_printf("rate=%d\n", AUDIO_SAMPLE_RATE);
    nk_printf("block=%d\n", AUDIO_BLOCK_FRAMES);
    nk_printf("heap_int_free=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    nk_printf("heap_int_min=%u\n", (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    nk_printf("heap_int_largest=%u\n", (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    nk_printf("psram_free=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    nk_printf("psram_min=%u\n", (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    nk_printf("psram_largest=%u\n", (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    nk_printf("voices=%d\n", mixer_active_voices());
    nk_printf("poly=%d/%d\n", mixer_polyphony(), mixer_voice_cap());
    nk_printf("voice_mask=%u\n", (unsigned)mixer_voice_mask());
    nk_printf("master=%u\n", (unsigned)mixer_master_ui());
    nk_printf("hpf=%d\n", mixer_hpf_hz());
    nk_printf("limit=%d\n", mixer_limit());
    nk_printf("pad_db=%d\n", mixer_pad_db());
    nk_printf("ishift=%d\n", audio_i2s_shift());
    nk_printf("samples=%llu\n", (unsigned long long)mixer_sample_count());
    nk_printf("sd_ok=%d\n", audio_bank_sd_ok() ? 1 : 0);
    nk_printf("factory_ok=%d\n", audio_bank_factory_ok() ? 1 : 0);
    nk_printf("sd_err=%s\n", audio_bank_sd_error());
    nk_printf("sd_pins=CS%d,SCK%d,MOSI%d,MISO%d\n",
           (int)PIN_SD_CS, (int)PIN_SD_SCK, (int)PIN_SD_MOSI, (int)PIN_SD_MISO);
    nk_printf("tft_pins=SCL%d,SDA%d,CS%d,DC%d,RES%d,BLK%d\n",
           (int)PIN_TFT_SCK, (int)PIN_TFT_MOSI, (int)PIN_TFT_CS,
           (int)PIN_TFT_DC, (int)PIN_TFT_RST, (int)PIN_TFT_BLK);
    nk_printf("vsys_always_on=%d (no boost)\n", board_boost_get());
    nk_printf("int_pin=GPIO%d tp_int=GPIO%d\n", (int)PIN_INT, (int)PIN_TP_INT);
    nk_printf("touch_ok=%d chip=0x%02X\n", touch_cst816_ok(), touch_cst816_chip_id());
    nk_printf("mic_run=%d rms=%d peak=%d\n", mic_i2s_running(), mic_i2s_rms(), mic_i2s_peak());
    nk_printf("ch32_ok=%d\n", key_link_ok());
    {
        uint8_t who = 0, ver = 0;
        if (key_link_whoami(&who, &ver) == ESP_OK) {
            nk_printf("ch32_whoami=0x%02X ver=0x%02X keymask=0x%04X\n", who, ver,
                      key_link_keymask());
        } else {
            nk_printf("ch32_whoami=fail keymask=0x%04X\n", key_link_keymask());
        }
    }
    key_link_dump_power();
    nk_printf("ws2812=%d@GPIO%d\n", WS2812_COUNT, (int)PIN_WS2812);
    nk_printf("enc2_pins=A%d,B%d,SW%d (LEFT vol/mute, hold+turn=key)\n", (int)PIN_EC11_A, (int)PIN_EC11_B,
              (int)PIN_EC11_SW);
    nk_printf("bank_name=%s\n", audio_bank_name());
    enc_fn_dump();
    score_dump_status();
    sd_music_dump_status();
    net_dump_status();
    ble_dump_status();
    nk_printf("bank=%d/%d\n", audio_bank_loaded_count(), AUDIO_BANK_WAV_COUNT);
    nk_printf("END_STATUS\n");
    nk_out_set_uart_echo(1);
}

static void handle_line_inner(char *line)
{
    unsigned a = 0, b = 0;
    MixCmd cmd = {0};

    if (strncmp(line, "play ", 5) == 0) {
        if (sscanf(line + 5, "%u", &a) == 1) {
            send_play(a, false);
        }
        return;
    }
    if (strncmp(line, "chord ", 6) == 0) {
        char *p = line + 6;
        while (*p) {
            a = (unsigned)strtoul(p, &p, 10);
            if (a >= 1 && a <= AUDIO_BANK_WAV_COUNT) {
                send_play(a, false);
            }
            while (*p == ' ') {
                p++;
            }
        }
        return;
    }
    if (sscanf(line, "sine %u", &a) == 1 && a < AUDIO_BANK_WAV_COUNT) {
        MixCmd scmd = {.type = MIX_CMD_TEST_SINE, .note_id = (uint8_t)a, .velocity = 127};
        mixer_submit(&scmd);
        return;
    }
    if (sscanf(line, "off %u", &a) == 1) {
        send_off(a);
        return;
    }
    if (strcmp(line, "stop") == 0) {
        cmd.type = MIX_CMD_STOP_ALL;
        mixer_submit(&cmd);
        return;
    }
    if (sscanf(line, "master %u", &b) == 1) {
        cmd.type = MIX_CMD_SET_MASTER;
        cmd.velocity = (uint8_t)(b > MIXER_MASTER_UI_MAX ? MIXER_MASTER_UI_MAX : b);
        mixer_submit(&cmd);
        printf("master=%u\n", (unsigned)cmd.velocity);
        return;
    }
    if (strcmp(line, "unmute") == 0 || strcmp(line, "xsmt on") == 0) {
        esp_err_t e = key_link_xsmt_set(1);
        printf("xsmt unmute err=%s muted=%d\n", esp_err_to_name(e), key_link_xsmt_muted());
        return;
    }
    if (strcmp(line, "xsmt off") == 0 || strcmp(line, "mute dac") == 0) {
        esp_err_t e = key_link_xsmt_set(0);
        printf("xsmt mute err=%s muted=%d\n", esp_err_to_name(e), key_link_xsmt_muted());
        return;
    }
    if (strcmp(line, "audio") == 0) {
        uint64_t s0 = mixer_sample_count();
        printf("audio master=%u voices=%d samples=%llu bank=%d/%d factory=%d sd=%d\n",
               (unsigned)mixer_master_ui(), mixer_active_voices(),
               (unsigned long long)s0, audio_bank_loaded_count(), AUDIO_BANK_WAV_COUNT,
               audio_bank_factory_ok() ? 1 : 0, audio_bank_sd_ok() ? 1 : 0);
        printf("audio bat_mv=%lu level=%d alrt=%d xsmt_muted=%d ch32=%d\n",
               (unsigned long)key_link_bat_mv(), key_link_bat_level(), key_link_bat_alrt(),
               key_link_xsmt_muted(), key_link_ok());
        printf("hint: unmute | master 80 | sine 0 | stop   (sine=no bank needed)\n");
        return;
    }
    if (sscanf(line, "poly %u", &b) == 1) {
        mixer_set_polyphony((int)b);
        ESP_LOGI(TAG, "polyphony %d / %d", mixer_polyphony(), mixer_voice_cap());
        return;
    }
    if (strcmp(line, "whoami") == 0) {
        uint8_t who = 0, ver = 0;
        if (key_link_whoami(&who, &ver) != ESP_OK) {
            ESP_LOGW(TAG, "CH32 I2C fail");
            return;
        }
        ESP_LOGI(TAG, "CH32 WHOAMI=0x%02X VER=0x%02X ok=%d mask=0x%04X boost=%d", who, ver,
                 key_link_ok(), key_link_keymask(), board_boost_get());
        return;
    }
    if (strcmp(line, "boost on") == 0) {
        board_boost_set(1);
        ESP_LOGI(TAG, "boost=1");
        return;
    }
    if (strcmp(line, "boost off") == 0) {
        board_boost_set(0);
        ESP_LOGI(TAG, "boost=0");
        return;
    }
    if (strncmp(line, "bank ", 5) == 0) {
        MixCmd stop = { .type = MIX_CMD_STOP_ALL };
        mixer_submit(&stop);
        vTaskDelay(pdMS_TO_TICKS(30));
        if (audio_bank_load(line + 5) != ESP_OK) {
            ESP_LOGW(TAG, "bank load failed (need /sdcard/violin/1.wav .. 15.wav)");
        }
        return;
    }
    if (strcmp(line, "bank") == 0) {
        ESP_LOGI(TAG, "bank %s %d/%d", audio_bank_name(), audio_bank_loaded_count(), AUDIO_BANK_WAV_COUNT);
        return;
    }
    if (sscanf(line, "hpf %u", &b) == 1) {
        mixer_set_hpf_hz((int)b);
        ESP_LOGI(TAG, "hpf %d Hz (0=off)", mixer_hpf_hz());
        return;
    }
    if (sscanf(line, "limit %u", &b) == 1) {
        mixer_set_limit((int)b);
        ESP_LOGI(TAG, "limit %d", mixer_limit());
        return;
    }
    if (sscanf(line, "pad %u", &b) == 1) {
        mixer_set_pad_db((int)b);
        ESP_LOGI(TAG, "pad %d dB", mixer_pad_db());
        return;
    }
    if (sscanf(line, "ishift %u", &b) == 1) {
        audio_i2s_set_shift((int)b);
        ESP_LOGI(TAG, "i2s shift %d", audio_i2s_shift());
        return;
    }
    if (strncmp(line, "wifi ", 5) == 0) {
        net_cmd(line + 5);
        return;
    }
    if (strcmp(line, "wifi") == 0) {
        net_cmd("ap");
        return;
    }
    if (strncmp(line, "music ", 6) == 0) {
        sd_music_cmd(line + 6);
        return;
    }
    if (strcmp(line, "music") == 0) {
        sd_music_cmd("list");
        return;
    }
    if (strncmp(line, "score list", 10) == 0) {
        int off = 0, n = 400;
        sscanf(line + 10, "%d %d", &off, &n);
        score_cmd_list(off, n);
        return;
    }
    if (strncmp(line, "score open ", 11) == 0) {
        score_cmd_open(line + 11);
        return;
    }
    if (strcmp(line, "score play") == 0) {
        score_cmd_play();
        return;
    }
    if (strcmp(line, "score pause") == 0) {
        score_cmd_pause();
        return;
    }
    if (strcmp(line, "score stop") == 0) {
        score_cmd_stop();
        return;
    }
    if (strncmp(line, "enc1 ", 5) == 0) {
        enc_fn_cmd(line + 5);
        return;
    }
    if (strncmp(line, "ble ", 4) == 0) {
        ble_cmd(line + 4);
        return;
    }
    if (strcmp(line, "ble") == 0) {
        ble_dump_status();
        return;
    }
    if (strcmp(line, "status") == 0) {
        if (s_status_task) {
            xTaskNotifyGive(s_status_task);
        } else {
            dump_status();
        }
        return;
    }
    if (ui_tft_handle_line(line)) {
        return;
    }
    if (touch_cst816_handle_line(line)) {
        return;
    }
    if (mic_i2s_handle_line(line)) {
        return;
    }
    if (sky_proto_handle_line(line)) {
        return;
    }
    if (line[0] != 0) {
        ESP_LOGW(TAG, "unknown: %.80s", line);
    }
}

static void status_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
        }
        if (s_cmd_mu) {
            xSemaphoreTake(s_cmd_mu, portMAX_DELAY);
        }
        dump_status();
        if (s_cmd_mu) {
            xSemaphoreGive(s_cmd_mu);
        }
    }
}

static void handle_line(char *line)
{
    if (!s_cmd_mu) {
        handle_line_inner(line);
        return;
    }
    xSemaphoreTake(s_cmd_mu, portMAX_DELAY);
    handle_line_inner(line);
    xSemaphoreGive(s_cmd_mu);
}

static void console_task(void *arg)
{
    (void)arg;
    char acc[1024];
    size_t n = 0;
    int drop = 0;
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    net_hook_stdio();
    ESP_LOGI(TAG, "cmds: status | touch | mic start|tap|stop | play N | tft …");
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (!drop && n) {
                acc[n] = 0;
                handle_line(acc);
            }
            n = 0;
            drop = 0;
            continue;
        }
        if (drop) {
            continue;
        }
        if (n + 1 < sizeof(acc)) {
            acc[n++] = (char)c;
        } else {
            drop = 1;
            n = 0;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "sky-music-v2  %d Hz  (keys+mix+WS2812+TFT+touch+mic)",
             AUDIO_SAMPLE_RATE);
    s_cmd_mu = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(board_power_init());
    /* TFT first: backlight + brand splash while audio/SD/BLE come up. */
    if (ui_tft_start() != ESP_OK) {
        ESP_LOGW(TAG, "TFT not started");
    }
    if (net_link_start(handle_line) != ESP_OK) {
        ESP_LOGW(TAG, "net link not started");
    }
    ESP_ERROR_CHECK(audio_i2s_init());
    ESP_ERROR_CHECK(mixer_start());
    mixer_set_pad_db(0);
    MixCmd vol = {
        .type = MIX_CMD_SET_MASTER,
        .velocity = MIXER_MASTER_UI_DEFAULT,
    };
    mixer_submit(&vol);

    if (audio_bank_init() != ESP_OK) {
        ESP_LOGW(TAG, "SD bank not ready");
    }
    /* Factory FAT is up — pull theme background into the single PSRAM slot. */
    ui_tft_bg_reload();
    (void)enc_fn_start();
    /* Unlock UI nav only after enc_fn mode scan (music→score→play) finishes. */
    ui_tft_nav_enable();

    if (key_link_start() != ESP_OK) {
        ESP_LOGE(TAG, "key_link I2C start failed");
    }
    if (touch_cst816_start() != ESP_OK) {
        ESP_LOGW(TAG, "CST816 touch not started");
    }
    (void)mic_i2s_start();
    if (rgb_led_start() != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 not started");
    }
    if (ble_link_start() != ESP_OK) {
        ESP_LOGW(TAG, "BLE not started");
    }
    if (encoder_start() != ESP_OK) {
        ESP_LOGW(TAG, "ENC2 not started");
    }
    if (score_play_start() != ESP_OK) {
        ESP_LOGW(TAG, "score player not started");
    } else {
        score_set_note_tap(ble_link_note);
    }
    drill_start();
    if (sd_music_start() != ESP_OK) {
        ESP_LOGW(TAG, "sd music not started");
    }
    xTaskCreatePinnedToCore(status_task, "status", 4096, NULL, 2, &s_status_task, 0);
    xTaskCreatePinnedToCore(console_task, "console", 12288, NULL, 3, NULL, 0);
    dump_status();
}
