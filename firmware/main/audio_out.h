#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t audio_out_init(void);
esp_err_t audio_out_write(const uint8_t *pcm, size_t len);   // 16 kHz mono int16; queues into a PSRAM ring buffer
void      audio_out_flush(void);                             // drop everything still queued (barge-in / abort)
bool      audio_out_drain(uint32_t timeout_ms);              // wait until the queue has been played
void      audio_out_set_volume(uint8_t v);                   // 0..100
float     audio_out_level(void);                             // smoothed RMS 0..1, for the animation
