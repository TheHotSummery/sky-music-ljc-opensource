#include "audio_i2s.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "audio_i2s";
static i2s_chan_handle_t s_tx;
static int32_t *s_dma;
static int s_shift; /* 0 for PCM5102 headphones; was 6 for line-into-amp */

static void dac_mute_pins(bool unmute)
{
    (void)unmute;
    /* Production: XSMT hard-wired 3V3; GPIO7/38/39 are unused / ENC2. */
}

esp_err_t audio_i2s_init(void)
{
    dac_mute_pins(false);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 16;
    chan_cfg.dma_frame_num = AUDIO_BLOCK_FRAMES;
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }

    /* Match known-good production PCM5102: Philips data32+slot32, sample=(s16<<16). */
    i2s_std_slot_config_t slot =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    slot.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE);
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    i2s_std_config_t std_cfg = {
        .clk_cfg = clk,
        .slot_cfg = slot,
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode: %s", esp_err_to_name(err));
        return err;
    }

    s_dma = heap_caps_calloc(AUDIO_BLOCK_FRAMES * AUDIO_CHANNELS_OUT, sizeof(int32_t),
                             MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_dma) {
        return ESP_ERR_NO_MEM;
    }

    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    dac_mute_pins(true);

    ESP_LOGI(TAG, "I2S %d Hz Philips 32-bit (s16<<16) BCLK=%d WS=%d DOUT=%d",
             AUDIO_SAMPLE_RATE, (int)PIN_I2S_BCLK, (int)PIN_I2S_WS, (int)PIN_I2S_DOUT);
    return ESP_OK;
}

esp_err_t audio_i2s_write(const int16_t *stereo_frames, size_t frame_count)
{
    const size_t n = frame_count * AUDIO_CHANNELS_OUT;
    const int sh = s_shift;
    for (size_t i = 0; i < n; ++i) {
        int16_t s = stereo_frames[i];
        if (sh > 0) {
            s = (int16_t)(s >> sh);
        }
        s_dma[i] = ((int32_t)s) << 16;
    }
    size_t bytes_written = 0;
    return i2s_channel_write(s_tx, s_dma, n * sizeof(int32_t), &bytes_written, portMAX_DELAY);
}

void audio_i2s_set_shift(int sh)
{
    if (sh < 0) {
        sh = 0;
    }
    if (sh > 12) {
        sh = 12;
    }
    s_shift = sh;
}

int audio_i2s_shift(void)
{
    return s_shift;
}
