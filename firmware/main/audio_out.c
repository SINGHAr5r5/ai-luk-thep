#include "audio_out.h"
#include <math.h>
#include <stdbool.h>
#include "audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

static const char *TAG = "audio_out";
#define RING_BYTES (96 * 1024)   // ~3 s of 16 kHz mono
#define CHUNK      640           // 20 ms

static RingbufHandle_t s_rb;
static volatile size_t s_pending;       // bytes queued but not yet played
static volatile float s_level;

static void play_task(void *arg)
{
    for (;;) {
        size_t n = 0;
        uint8_t *p = xRingbufferReceiveUpTo(s_rb, &n, pdMS_TO_TICKS(40), CHUNK);
        if (!p) { s_level *= 0.7f; continue; }
        n &= ~(size_t)1;
        double sum = 0;
        const int16_t *s = (const int16_t *)p;
        for (size_t i = 0; i < n / 2; i++) sum += (double)s[i] * s[i];
        if (n) {
            float rms = sqrtf((float)(sum / (n / 2))) / 32768.0f;
            s_level = s_level * 0.7f + rms * 0.3f;
            esp_codec_dev_write(audio_codec(), p, n);
        }
        vRingbufferReturnItem(s_rb, p);
        s_pending = (s_pending > n) ? s_pending - n : 0;
    }
}

esp_err_t audio_out_init(void)
{
    if (s_rb) return ESP_OK;
    if (!audio_codec()) return ESP_ERR_INVALID_STATE;
    s_rb = xRingbufferCreateWithCaps(RING_BYTES, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (!s_rb) { ESP_LOGE(TAG, "no memory for the ring buffer"); return ESP_ERR_NO_MEM; }
    xTaskCreatePinnedToCore(play_task, "audio_out", 4096, NULL, 6, NULL, 1);
    return ESP_OK;
}

esp_err_t audio_out_write(const uint8_t *pcm, size_t len)
{
    if (!s_rb) return ESP_ERR_INVALID_STATE;
    s_pending += len;
    if (xRingbufferSend(s_rb, pcm, len, pdMS_TO_TICKS(2000)) != pdTRUE) {
        s_pending = (s_pending > len) ? s_pending - len : 0;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void audio_out_flush(void)
{
    if (!s_rb) return;
    size_t n;
    void *p;
    while ((p = xRingbufferReceiveUpTo(s_rb, &n, 0, RING_BYTES)) != NULL) {
        vRingbufferReturnItem(s_rb, p);
        s_pending = (s_pending > n) ? s_pending - n : 0;
    }
}

bool audio_out_drain(uint32_t timeout_ms)
{
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (s_pending > 0) {
        if (xTaskGetTickCount() > end) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(80));   // DMA tail
    return true;
}

void audio_out_set_volume(uint8_t v)
{
    if (v > 100) v = 100;
    if (audio_codec()) esp_codec_dev_set_out_vol(audio_codec(), v);
}

float audio_out_level(void) { return s_level; }
