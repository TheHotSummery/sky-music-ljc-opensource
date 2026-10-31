#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool loaded;
    uint32_t frames;
    int16_t peak;
    uint32_t path_hash;
} AudioBankSlot;

esp_err_t audio_bank_init(void);
int audio_bank_loaded_count(void);
bool audio_bank_sd_ok(void);
bool audio_bank_factory_ok(void);
uint16_t audio_bank_loaded_mask(void);
const char *audio_bank_sd_error(void);
bool audio_bank_get_slot(int index0, AudioBankSlot *out);
const char *audio_bank_slot_path(int index0);
const char *audio_bank_name(void);
esp_err_t audio_bank_load(const char *folder_name);

#ifdef __cplusplus
}
#endif
