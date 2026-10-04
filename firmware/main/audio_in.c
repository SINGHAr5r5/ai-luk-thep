#include "audio_in.h"
#include <math.h>
#include "audio.h"
#include "esp_timer.h"

#define VAD_RMS_THRESHOLD 0.015f   // ~ -36 dBFS; tune with `levels` on the console

static float s_level;
static volatile int64_t s_last_voice_us;

esp_err_t audio_in_init(void) { return audio_codec() ? ESP_OK : ESP_ERR_INVALID_STATE; }

void audio_in_start(void)
{
    s_level = 0;
    s_last_voice_us = esp_timer_get_time();
}

void audio_in_stop(void) {}

int audio_in_read(int16_t *pcm, size_t samples)
{
    if (!audio_codec()) return -1;
    if (esp_codec_dev_read(audio_codec(), pcm, samples * sizeof(int16_t)) != ESP_CODEC_DEV_OK) return -1;
    double sum = 0;
    for (size_t i = 0; i < samples; i++) sum += (double)pcm[i] * pcm[i];
    float rms = sqrtf((float)(sum / samples)) / 32768.0f;
    s_level = s_level * 0.8f + rms * 0.2f;
    if (rms > VAD_RMS_THRESHOLD) s_last_voice_us = esp_timer_get_time();
    return (int)samples;
}

float audio_in_level(void) { return s_level; }

bool audio_in_vad_is_silent(uint32_t ms)
{
    return (esp_timer_get_time() - s_last_voice_us) >= (int64_t)ms * 1000;
}

void audio_in_set_gain_db(float db)
{
    if (audio_codec()) esp_codec_dev_set_in_gain(audio_codec(), db);
}
