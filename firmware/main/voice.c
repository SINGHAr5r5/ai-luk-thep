#include "voice.h"
#include <stdio.h>
#include <string.h>
#include "app_state.h"
#include "audio.h"
#include "audio_in.h"
#include "audio_out.h"
#include "buttons.h"
#include "settings.h"
#include "ui.h"
#include "ws_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "voice";

#define FRAME_SAMPLES   640           // 40 ms, same size the bridge uses for its own frames
#define MAX_TALK_MS     8000          // BOOT is also the Wi-Fi reset key, see buttons.c
#define ERROR_SHOW_MS   2500

static volatile bool s_down, s_capturing;
static bool s_idle_pending;
static esp_timer_handle_t s_err_timer;

bool voice_configured(void)
{
    return settings_has(SETTING_BRIDGE_URL) && settings_has(SETTING_BRIDGE_TOKEN);
}

// ------------------------------------------------------------- helpers
static void err_timeout(void *arg)
{
    if (app_get_state() == ST_ERROR && ws_is_connected()) app_enter_idle();
}

static void idle_after_drain_task(void *arg)
{
    audio_out_drain(4000);                       // let the last sentence finish before the Orb calms down
    app_state_t s = app_get_state();
    if (s == ST_SPEAKING || s == ST_THINKING) app_enter_idle();
    s_idle_pending = false;
    vTaskDelete(NULL);
}

static void go_idle_when_quiet(void)
{
    if (s_idle_pending) return;
    s_idle_pending = true;
    xTaskCreate(idle_after_drain_task, "idle_drain", 3072, NULL, 4, NULL);
}

// ------------------------------------------------------------- bridge -> board
// Standard base64 -> bytes; returns the number of bytes written, or -1 on bad input / overflow.
static int b64_decode(const char *in, uint8_t *out, size_t cap)
{
    static int8_t map[256];
    if (!map['B']) {                                   // 'A' is 0, so test a letter that is never 0
        memset(map, -1, sizeof(map));
        for (int i = 0; i < 26; i++) { map['A' + i] = i; map['a' + i] = 26 + i; }
        for (int i = 0; i < 10; i++) map['0' + i] = 52 + i;
        map['+'] = 62; map['/'] = 63;
    }
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *in && *in != '='; in++) {
        int v = map[(uint8_t)*in];
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (acc >> bits) & 0xFF;
        }
    }
    return (int)o;
}

static void on_text_image(const cJSON *msg)
{
    const cJSON *role = cJSON_GetObjectItem(msg, "role");
    const cJSON *jw = cJSON_GetObjectItem(msg, "w"), *jh = cJSON_GetObjectItem(msg, "h");
    const cJSON *a8 = cJSON_GetObjectItem(msg, "a8");
    if (!cJSON_IsString(role) || !cJSON_IsNumber(jw) || !cJSON_IsNumber(jh) || !cJSON_IsString(a8)) return;
    int w = jw->valueint, h = jh->valueint;
    if (w < 1 || w > 240 || h < 1 || h > 240) return;
    size_t n = (size_t)w * h;
    uint8_t *buf = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    if (b64_decode(a8->valuestring, buf, n) != (int)n) {
        ESP_LOGW(TAG, "text_image: bad bitmap (%dx%d)", w, h);
        free(buf);
        return;
    }
    ui_show_text(role->valuestring, w, h, buf);        // the screen owns the buffer from here
}

static void on_conn(bool ready)
{
    if (ready) {
        ESP_LOGI(TAG, "voice link up");
        app_enter_idle();
    } else {
        ESP_LOGW(TAG, "voice link lost");
        s_down = false;
        audio_out_flush();
        app_set_state(ST_BRIDGE_CONNECTING);
    }
}

static void on_json(cJSON *msg)
{
    const cJSON *t = cJSON_GetObjectItem(msg, "type");
    if (!cJSON_IsString(t)) return;
    const char *type = t->valuestring;

    if (!strcmp(type, "state")) {
        const cJSON *v = cJSON_GetObjectItem(msg, "value");
        if (!cJSON_IsString(v) || s_capturing) return;       // our own LISTENING wins while the button is held
        if (!strcmp(v->valuestring, "thinking")) app_set_state(ST_THINKING);
        else if (!strcmp(v->valuestring, "speaking")) app_set_state(ST_SPEAKING);
        else if (!strcmp(v->valuestring, "idle")) go_idle_when_quiet();
    } else if (!strcmp(type, "stt")) {
        ESP_LOGI(TAG, "heard: %s", cJSON_GetStringValue(cJSON_GetObjectItem(msg, "text")));
    } else if (!strcmp(type, "reply_text")) {
        if (cJSON_IsTrue(cJSON_GetObjectItem(msg, "final")))
            ESP_LOGI(TAG, "reply: %s", cJSON_GetStringValue(cJSON_GetObjectItem(msg, "text")));
        // subtitles on screen arrive with the Thai font in Phase 7
    } else if (!strcmp(type, "text_image")) {
        on_text_image(msg);
    } else if (!strcmp(type, "tts_start")) {
        audio_out_flush();
        app_set_state(ST_SPEAKING);
    } else if (!strcmp(type, "tts_end")) {
        go_idle_when_quiet();
    } else if (!strcmp(type, "error")) {
        ESP_LOGW(TAG, "bridge error (%s): %s", cJSON_GetStringValue(cJSON_GetObjectItem(msg, "stage")),
                 cJSON_GetStringValue(cJSON_GetObjectItem(msg, "msg")));
        app_set_state(ST_ERROR);
        if (s_err_timer) { esp_timer_stop(s_err_timer); esp_timer_start_once(s_err_timer, ERROR_SHOW_MS * 1000LL); }
    }
}

static void on_audio(const uint8_t *pcm, size_t len)
{
    audio_out_write(pcm, len);
}

// ------------------------------------------------------------- board -> bridge
static void capture_task(void *arg)
{
    static int16_t frame[FRAME_SAMPLES];
    ws_send_json("{\"type\":\"listen_start\"}");
    app_set_state(ST_LISTENING);
    audio_in_start();
    int64_t t0 = esp_timer_get_time();
    while (s_down && ws_is_connected() && (esp_timer_get_time() - t0) < MAX_TALK_MS * 1000LL) {
        if (audio_in_read(frame, FRAME_SAMPLES) < 0) break;
        ws_send_audio((const uint8_t *)frame, sizeof(frame));
    }
    ws_send_json("{\"type\":\"listen_stop\"}");
    ESP_LOGI(TAG, "sent %.1f s of speech", (esp_timer_get_time() - t0) / 1e6);
    app_set_state(ST_THINKING);
    s_capturing = false;
    vTaskDelete(NULL);
}

static void on_press(void)
{
    app_state_t st = app_get_state();
    if (!ws_is_connected() || s_capturing) return;
    if (st != ST_IDLE && st != ST_THINKING && st != ST_SPEAKING && st != ST_ERROR) return;
    audio_out_flush();                           // pressing during a reply interrupts it (barge-in by button)
    s_capturing = true;
    s_down = true;
    xTaskCreate(capture_task, "capture", 4096, NULL, 5, NULL);
}

static void on_release(void) { s_down = false; }

// ------------------------------------------------------------- start-up
static void start_task(void *arg)
{
    while (!audio_codec()) vTaskDelay(pdMS_TO_TICKS(200));    // the codec opens in its own task
    char url[160], token[96];
    if (settings_get(SETTING_BRIDGE_URL, url, sizeof(url)) != ESP_OK ||
        settings_get(SETTING_BRIDGE_TOKEN, token, sizeof(token)) != ESP_OK) {
        vTaskDelete(NULL);
    }
    const esp_timer_create_args_t a = {.callback = err_timeout, .name = "voice_err"};
    esp_timer_create(&a, &s_err_timer);
    ws_client_set_callbacks(on_conn, on_json, on_audio);
    buttons_set_ptt(on_press, on_release);                    // takes over from the Phase 4 local loopback
    app_set_state(ST_BRIDGE_CONNECTING);
    ws_client_start(url, token);
    vTaskDelete(NULL);
}

void voice_start(void)
{
    if (!voice_configured()) {
        ESP_LOGW(TAG, "no bridge configured; BOOT keeps the local loopback test (console: bridge <url>, bridge_token <t>)");
        return;
    }
    xTaskCreate(start_task, "voice_start", 4096, NULL, 4, NULL);
}

void voice_test_ptt(int hold_ms)
{
    if (!ws_is_connected()) { printf("not connected to the bridge\n"); return; }
    on_press();
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    on_release();
}
