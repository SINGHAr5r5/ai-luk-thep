#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t audio_in_init(void);
void      audio_in_start(void);                                  // resets the level + silence tracking
void      audio_in_stop(void);
int       audio_in_read(int16_t *pcm, size_t samples);           // blocking; returns samples read (16 kHz mono)
float     audio_in_level(void);                                  // smoothed RMS 0..1, for the animation
bool      audio_in_vad_is_silent(uint32_t ms);                   // true when quiet for at least `ms`
void      audio_in_set_gain_db(float db);                        // ES8311 mic gain 0..42 dB
