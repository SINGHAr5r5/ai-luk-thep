#pragma once
#include "esp_err.h"
#include "esp_codec_dev.h"

#define AUDIO_RATE 16000   // Hz, 16-bit mono

esp_err_t audio_init(void);                 // I2C + I2S + ES8311 + speaker amplifier, codec opened at 16 kHz mono
esp_codec_dev_handle_t audio_codec(void);
