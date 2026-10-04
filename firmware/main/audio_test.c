#include "audio_test.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "audio.h"
#include "audio_in.h"
#include "audio_out.h"
#include "app_state.h"
#include "buttons.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_test";
#define FRAME      320                        // 20 ms
#define REC_SECS   4
#define REC_CAP    (AUDIO_RATE * REC_SECS)

static int16_t *s_rec;
static volatile bool s_down, s_busy;

static void fill_tone(int16_t *dst, size_t n, float freq, float amp, float *phase)
{
    float step = 2.0f * (float)M_PI * freq / AUDIO_RATE;
    for (size_t i = 0; i < n; i++) {
        dst[i] = (int16_t)(sinf(*phase) * amp * 32767.0f);
        *phase += step;
        if (*phase > 2.0f * (float)M_PI) *phase -= 2.0f * (float)M_PI;
    }
}

static void stats(const int16_t *x, size_t n, float *rms, int *peak, int *clipped)
{
    double sum = 0;
    int pk = 0, cl = 0;
    for (size_t i = 0; i < n; i++) {
        int a = abs(x[i]);
        if (a > pk) pk = a;
        if (a >= 32700) cl++;
        sum += (double)x[i] * x[i];
    }
    *rms = n ? sqrtf((float)(sum / n)) / 32768.0f : 0;
    *peak = pk;
    *clipped = cl;
}

static float goertzel_fraction(const int16_t *x, size_t n, float freq)
{
    float coeff = 2.0f * cosf(2.0f * (float)M_PI * freq / AUDIO_RATE);
    float s1 = 0, s2 = 0;
    double energy = 0;
    for (size_t i = 0; i < n; i++) {
        float s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
        energy += (double)x[i] * x[i];
    }
    float p = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    float amp = 2.0f * sqrtf(p > 0 ? p : 0) / n;
    float total = (float)(energy / n);
    return total > 0 ? (amp * amp / 2.0f) / total : 0;
}

// ---------------------------------------------------------------- push-to-talk loopback
static void loop_task(void *arg)
{
    app_set_state(ST_LISTENING);
    audio_in_start();
    size_t n = 0;
    while (s_down && n + FRAME <= REC_CAP) {
        if (audio_in_read(s_rec + n, FRAME) < 0) break;
        n += FRAME;
    }
    float rms; int peak, clipped;
    stats(s_rec, n, &rms, &peak, &clipped);
    ESP_LOGI(TAG, "recorded %.2f s  rms %.3f  peak %d  clipped %d", (float)n / AUDIO_RATE, rms, peak, clipped);

    app_set_state(ST_SPEAKING);
    for (size_t i = 0; i < n; i += FRAME * 2) {
        size_t c = (n - i < FRAME * 2) ? n - i : FRAME * 2;
        audio_out_write((const uint8_t *)(s_rec + i), c * sizeof(int16_t));
    }
    audio_out_drain(REC_SECS * 1000 + 2000);
    app_enter_idle();
    s_busy = false;
    vTaskDelete(NULL);
}

static void on_press(void)
{
    if (s_busy || app_get_state() != ST_IDLE) return;
    s_busy = true;
    s_down = true;
    xTaskCreate(loop_task, "ptt_loop", 4096, NULL, 5, NULL);
}

static void on_release(void) { s_down = false; }

void audio_test_init(void)
{
    s_rec = heap_caps_malloc(REC_CAP * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_rec) { ESP_LOGE(TAG, "no PSRAM for the record buffer"); return; }
    buttons_set_ptt(on_press, on_release);
}

// ---------------------------------------------------------------- console helpers
void audio_test_beep(int freq_hz, int ms)
{
    int16_t buf[FRAME];
    float phase = 0;
    for (int done = 0; done < ms * AUDIO_RATE / 1000; done += FRAME) {
        fill_tone(buf, FRAME, (float)freq_hz, 0.5f, &phase);
        audio_out_write((const uint8_t *)buf, sizeof(buf));
    }
    audio_out_drain(ms + 2000);
}

void audio_test_selftest(void)
{
    if (!s_rec || s_busy) { printf("busy or no buffer\n"); return; }
    s_busy = true;
    const int frames = 100;   // 2 s
    float phase = 0;
    int16_t tone[FRAME];
    audio_out_flush();
    audio_in_start();
    for (int f = 0; f < frames; f++) {
        if (f >= 15 && f < 65) {   // 1 s of 1 kHz starting 0.3 s in
            fill_tone(tone, FRAME, 1000.0f, 0.5f, &phase);
            audio_out_write((const uint8_t *)tone, sizeof(tone));
        }
        audio_in_read(s_rec + f * FRAME, FRAME);
    }
    audio_out_drain(2000);

    // quiet reference: the first 0.2 s, before any tone is queued
    float base, rms; int pk, cl;
    stats(s_rec, 10 * FRAME, &base, &pk, &cl);
    // loudest 0.5 s window
    const int win = 25;
    int best = 0;
    float best_rms = 0;
    for (int f = 0; f + win <= frames; f++) {
        stats(s_rec + f * FRAME, win * FRAME, &rms, &pk, &cl);
        if (rms > best_rms) { best_rms = rms; best = f; }
    }
    float frac = goertzel_fraction(s_rec + best * FRAME, win * FRAME, 1000.0f);
    int total_clip = 0;
    stats(s_rec, frames * FRAME, &rms, &pk, &total_clip);
    bool heard = best_rms > 3.0f * (base > 0.0005f ? base : 0.0005f) && frac > 0.4f;

    printf("selftest: quiet rms %.4f | loudest 0.5 s at %.2f s: rms %.4f, 1 kHz share %.0f%% | peak %d, clipped %d\n",
           base, best * FRAME / (float)AUDIO_RATE, best_rms, frac * 100.0f, pk, total_clip);
    printf("selftest: %s\n", heard ? "PASS - the microphone heard the speaker's 1 kHz tone" :
           "FAIL - tone not heard (speaker silent, mic dead, or too quiet: try `vol 90` / `gain 36`)");
    s_busy = false;
}

void audio_test_levels(void)
{
    if (!s_rec || s_busy) { printf("busy or no buffer\n"); return; }
    s_busy = true;
    audio_in_start();
    for (int i = 0; i < 10; i++) {   // 10 x 200 ms
        audio_in_read(s_rec, FRAME * 10);
        float rms; int pk, cl;
        stats(s_rec, FRAME * 10, &rms, &pk, &cl);
        int bars = (int)(rms * 200);
        if (bars > 40) bars = 40;
        printf("%4d ms  rms %.4f  peak %5d  %.*s\n", (i + 1) * 200, rms, pk, bars, "########################################");
    }
    s_busy = false;
}
