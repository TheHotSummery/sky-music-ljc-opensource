#include "audio_bank.h"

#include "board_pins.h"
#include "mixer.h"

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "wear_levelling.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "audio_bank";
static int s_loaded;
static bool s_sd_ok;
static char s_sd_err[48] = "not_inited";
static sdmmc_card_t *s_card;
static wl_handle_t s_factory_wl = WL_INVALID_HANDLE;
static bool s_factory_ok;
static AudioBankSlot s_slots[AUDIO_BANK_WAV_COUNT];
static char s_paths[AUDIO_BANK_WAV_COUNT][80];
static int16_t *s_pcm[AUDIO_BANK_WAV_COUNT];
static char s_bank_name[16] = "piano";
static uint32_t s_piano_fp;

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void sd_set_err(const char *msg)
{
    strncpy(s_sd_err, msg, sizeof(s_sd_err) - 1);
    s_sd_err[sizeof(s_sd_err) - 1] = 0;
}

static void sd_gpio_prepare(void)
{
    gpio_set_pull_mode(PIN_SD_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_SD_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_SD_SCK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_SD_CS, GPIO_PULLUP_ONLY);
}

static esp_err_t mount_sd(void)
{
    ESP_LOGI(TAG, "SD SPI CS=%d SCK=%d MOSI=%d MISO=%d",
             (int)PIN_SD_CS, (int)PIN_SD_SCK, (int)PIN_SD_MOSI, (int)PIN_SD_MISO);
    sd_gpio_prepare();
    vTaskDelay(pdMS_TO_TICKS(50));

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16384,
    };
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        sd_set_err(esp_err_to_name(err));
        return err;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    /* PCB traces: try 20 MHz, then slower if the card NACKs. */
    static const int freqs[] = {20000, 10000, 4000};
    err = ESP_FAIL;
    for (int i = 0; i < (int)(sizeof(freqs) / sizeof(freqs[0])); ++i) {
        host.max_freq_khz = freqs[i];
        err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mount, &s_card);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "sd mounted at %d kHz", freqs[i]);
            break;
        }
        ESP_LOGW(TAG, "sd %d kHz: %s", freqs[i], esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sd mount failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "check: module VCC=5V or 3.3V as labeled, GND common,");
        ESP_LOGE(TAG, "CS->GPIO%d SCK->GPIO%d MOSI->GPIO%d MISO->GPIO%d, FAT32 (not exFAT)",
                 (int)PIN_SD_CS, (int)PIN_SD_SCK, (int)PIN_SD_MOSI, (int)PIN_SD_MISO);
        sd_set_err(esp_err_to_name(err));
        return err;
    }
    sdmmc_card_print_info(stdout, s_card);
    /* Warm SDSPI DMA block_buf while internal DMA is still plentiful (pre-WiFi). */
    {
        void *probe = heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (probe && s_card) {
            (void)sdmmc_read_sectors(s_card, probe, 0, 1);
            free(probe);
        } else if (probe) {
            free(probe);
        }
    }
    sd_set_err("ok");
    return ESP_OK;
}

static esp_err_t mount_factory(void)
{
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(FACTORY_MOUNT_POINT, FACTORY_PART_LABEL, &cfg, &s_factory_wl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "factory assets mount: %s (no image until assets/factory/piano/1.wav exists)",
                 esp_err_to_name(err));
        s_factory_ok = false;
        return err;
    }
    s_factory_ok = true;
    ESP_LOGI(TAG, "factory assets at %s", FACTORY_MOUNT_POINT);
    return ESP_OK;
}

static bool load_wav_file(const char *path, int16_t **pcm_out, uint32_t *frames_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }

    uint8_t riff[12];
    if (fread(riff, 1, 12, fp) != 12 || memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        ESP_LOGW(TAG, "%s: not RIFF/WAVE", path);
        fclose(fp);
        return false;
    }

    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t rate = 0;
    uint32_t data_bytes = 0;
    long data_pos = -1;

    for (;;) {
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, fp) != 8) {
            break;
        }
        uint32_t sz = read_u32(hdr + 4);
        if (memcmp(hdr, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            uint32_t n = sz > sizeof(fmt) ? sizeof(fmt) : sz;
            if (fread(fmt, 1, n, fp) != n) {
                fclose(fp);
                return false;
            }
            if (sz > n) {
                fseek(fp, (long)(sz - n), SEEK_CUR);
            }
            if (read_u16(fmt) != 1) {
                ESP_LOGW(TAG, "%s: not PCM", path);
                fclose(fp);
                return false;
            }
            channels = read_u16(fmt + 2);
            rate = read_u32(fmt + 4);
            bits = read_u16(fmt + 14);
        } else if (memcmp(hdr, "data", 4) == 0) {
            data_bytes = sz;
            data_pos = ftell(fp);
            break;
        } else {
            fseek(fp, (long)sz + (sz & 1), SEEK_CUR);
        }
    }

    if (data_pos < 0 || channels == 0 || bits != 16) {
        ESP_LOGW(TAG, "%s: unsupported wav ch=%u bits=%u", path, channels, bits);
        fclose(fp);
        return false;
    }
    if (rate != AUDIO_SAMPLE_RATE) {
        ESP_LOGW(TAG, "%s: rate %u != %u, skip", path, (unsigned)rate, AUDIO_SAMPLE_RATE);
        fclose(fp);
        return false;
    }

    const uint32_t src_frames = data_bytes / (channels * (bits / 8));
    size_t bytes = src_frames * sizeof(int16_t);
    size_t sync = (bytes + 31u) & ~31u;
    int16_t *pcm = heap_caps_aligned_alloc(64, sync ? sync : 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) {
        ESP_LOGE(TAG, "%s: PSRAM alloc %u frames failed", path, (unsigned)src_frames);
        fclose(fp);
        return false;
    }

    fseek(fp, data_pos, SEEK_SET);
    if (channels == 1) {
        if (fread(pcm, sizeof(int16_t), src_frames, fp) != src_frames) {
            heap_caps_free(pcm);
            fclose(fp);
            return false;
        }
    } else {
        for (uint32_t i = 0; i < src_frames; ++i) {
            int16_t lr[2];
            if (fread(lr, sizeof(int16_t), 2, fp) != 2) {
                heap_caps_free(pcm);
                fclose(fp);
                return false;
            }
            pcm[i] = lr[0];
        }
    }
    fclose(fp);
    if (sync > bytes) {
        memset((uint8_t *)pcm + bytes, 0, sync - bytes);
    }
    (void)esp_cache_msync(pcm, sync, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    *pcm_out = pcm;
    *frames_out = src_frames;
    return true;
}

static int16_t pcm_peak(const int16_t *pcm, uint32_t frames)
{
    int16_t peak = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        int16_t a = pcm[i];
        if (a < 0) {
            a = (int16_t)(-a);
        }
        if (a > peak) {
            peak = a;
        }
    }
    return peak;
}

static uint32_t pcm_fingerprint(const int16_t *p, uint32_t n)
{
    uint32_t h = n * 2654435761u;
    if (!p || n == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < n; i += 64) {
        h ^= (uint16_t)p[i];
        h *= 16777619u;
    }
    h ^= (uint16_t)p[n - 1];
    return h;
}

static void unload_samples(void)
{
    for (int i = 0; i < AUDIO_BANK_WAV_COUNT; ++i) {
        mixer_set_sample((uint8_t)i, NULL, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    for (int i = 0; i < AUDIO_BANK_WAV_COUNT; ++i) {
        if (s_pcm[i]) {
            heap_caps_free(s_pcm[i]);
            s_pcm[i] = NULL;
        }
        s_slots[i].loaded = false;
        s_slots[i].frames = 0;
        s_paths[i][0] = 0;
    }
    s_loaded = 0;
}

static int try_load_wav(const char *path, int slot)
{
    int16_t *pcm = NULL;
    uint32_t frames = 0;
    if (!load_wav_file(path, &pcm, &frames)) {
        return 0;
    }
    s_pcm[slot] = pcm;
    mixer_set_sample((uint8_t)slot, pcm, frames);
    s_slots[slot].loaded = true;
    s_slots[slot].frames = frames;
    s_slots[slot].peak = pcm_peak(pcm, frames);
    strncpy(s_paths[slot], path, sizeof(s_paths[slot]) - 1);
    s_paths[slot][sizeof(s_paths[slot]) - 1] = 0;
    ESP_LOGI(TAG, "loaded %s frames=%u", path, (unsigned)frames);
    return 1;
}

static int load_folder(const char *name)
{
    int n = 0;
    for (int i = 0; i < AUDIO_BANK_WAV_COUNT; ++i) {
        char path[80];
        int ok = 0;
        if (s_sd_ok) {
            snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s/%d.wav", name, i + 1);
            ok = try_load_wav(path, i);
            if (!ok) {
                snprintf(path, sizeof(path), SD_MOUNT_POINT "/audio/%s/%d.wav", name, i + 1);
                ok = try_load_wav(path, i);
            }
        }
        if (!ok && s_factory_ok) {
            snprintf(path, sizeof(path), FACTORY_MOUNT_POINT "/%s/%d.wav", name, i + 1);
            ok = try_load_wav(path, i);
        }
        if (ok) {
            n++;
        } else {
            ESP_LOGW(TAG, "missing %s %d", name, i + 1);
        }
    }
    return n;
}

esp_err_t audio_bank_init(void)
{
    s_loaded = 0;
    s_sd_ok = false;
    s_factory_ok = false;
    memset(s_slots, 0, sizeof(s_slots));
    memset(s_pcm, 0, sizeof(s_pcm));
    (void)mount_factory();
    if (mount_sd() == ESP_OK) {
        s_sd_ok = true;
    }
    strncpy(s_bank_name, "piano", sizeof(s_bank_name) - 1);
    s_loaded = load_folder(s_bank_name);
    if (s_slots[0].loaded) {
        s_piano_fp = pcm_fingerprint(s_pcm[0], s_slots[0].frames);
    }
    mixer_set_color(0);
    ESP_LOGI(TAG, "bank %s %d/%d fp0=%08lx factory=%d sd=%d, free PSRAM %u",
             s_bank_name, s_loaded, AUDIO_BANK_WAV_COUNT, (unsigned long)s_piano_fp,
             (int)s_factory_ok, (int)s_sd_ok,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return s_loaded > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

const char *audio_bank_name(void)
{
    return s_bank_name;
}

esp_err_t audio_bank_load(const char *folder_name)
{
    if (!folder_name || (!s_sd_ok && !s_factory_ok)) {
        return ESP_ERR_INVALID_STATE;
    }
    while (*folder_name == ' ' || *folder_name == '\t') {
        folder_name++;
    }
    char name[16];
    strncpy(name, folder_name, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    size_t nlen = strlen(name);
    while (nlen && (name[nlen - 1] == ' ' || name[nlen - 1] == '\t')) {
        name[--nlen] = 0;
    }
    if (!name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < 15 && mixer_active_voices() > 0; ++i) {
        vTaskDelay(pdMS_TO_TICKS(4));
    }
    unload_samples();
    strncpy(s_bank_name, name, sizeof(s_bank_name) - 1);
    s_bank_name[sizeof(s_bank_name) - 1] = 0;
    s_loaded = load_folder(s_bank_name);
    uint32_t fp = s_slots[0].loaded ? pcm_fingerprint(s_pcm[0], s_slots[0].frames) : 0;
    int placeholder = 0;
    if (strcmp(s_bank_name, "piano") == 0) {
        s_piano_fp = fp;
        mixer_set_color(0);
    } else if (s_piano_fp != 0 && fp == s_piano_fp) {
        placeholder = 1;
        mixer_set_color(1);
        ESP_LOGW(TAG, "bank %s WAV same as piano (fp=%08lx) — placeholder EQ on",
                 s_bank_name, (unsigned long)fp);
    } else {
        mixer_set_color(0);
    }
    const char *p0 = audio_bank_slot_path(0);
    ESP_LOGI(TAG, "switched bank %s %d/%d path0=%s fp=%08lx ph=%d", s_bank_name, s_loaded,
             AUDIO_BANK_WAV_COUNT, p0[0] ? p0 : "-", (unsigned long)fp, placeholder);
    printf("EVENT bank=%s loaded=%d/%d path0=%s fp=%08lx placeholder=%d\n", s_bank_name, s_loaded,
           AUDIO_BANK_WAV_COUNT, p0[0] ? p0 : "-", (unsigned long)fp, placeholder);
    return s_loaded > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

int audio_bank_loaded_count(void)
{
    return s_loaded;
}

bool audio_bank_factory_ok(void)
{
    return s_factory_ok;
}

uint16_t audio_bank_loaded_mask(void)
{
    uint16_t m = 0;
    for (int i = 0; i < AUDIO_BANK_WAV_COUNT; ++i) {
        if (s_slots[i].loaded) {
            m |= (uint16_t)(1u << i);
        }
    }
    return m;
}

bool audio_bank_sd_ok(void)
{
    return s_sd_ok;
}

const char *audio_bank_sd_error(void)
{
    return s_sd_err;
}

bool audio_bank_get_slot(int index0, AudioBankSlot *out)
{
    if (!out || index0 < 0 || index0 >= AUDIO_BANK_WAV_COUNT) {
        return false;
    }
    *out = s_slots[index0];
    return true;
}

const char *audio_bank_slot_path(int index0)
{
    if (index0 < 0 || index0 >= AUDIO_BANK_WAV_COUNT || !s_slots[index0].loaded) {
        return "";
    }
    return s_paths[index0];
}
