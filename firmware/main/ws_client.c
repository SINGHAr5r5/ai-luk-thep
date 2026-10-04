#include "ws_client.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "ota_mgr.h"

static const char *TAG = "ws_client";

#define TXT_MAX      65536     // text_image messages carry a base64 bitmap (~45 KB)
#define BIT_DOWN     BIT0
#define BACKOFF_MAX  30

static char s_url[160], s_token[96], s_device_id[32];
static esp_websocket_client_handle_t s_client;
static EventGroupHandle_t s_ev;
static volatile bool s_ready;
static ws_conn_cb_t s_conn_cb;
static ws_json_cb_t s_json_cb;
static ws_audio_cb_t s_audio_cb;
static char *s_txt;          // reassembly buffer for fragmented text frames
static uint8_t s_carry;      // odd byte left over between binary fragments
static bool s_have_carry;

void ws_client_set_callbacks(ws_conn_cb_t conn, ws_json_cb_t json, ws_audio_cb_t audio)
{
    s_conn_cb = conn; s_json_cb = json; s_audio_cb = audio;
}

bool ws_is_connected(void) { return s_ready; }

void ws_send_json(const char *json)
{
    if (s_client && esp_websocket_client_is_connected(s_client))
        esp_websocket_client_send_text(s_client, json, strlen(json), pdMS_TO_TICKS(1000));
}

void ws_send_audio(const uint8_t *pcm, size_t len)
{
    if (s_ready && s_client)
        esp_websocket_client_send_bin(s_client, (const char *)pcm, len, pdMS_TO_TICKS(1000));
}

static void send_hello(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "hello");
    cJSON_AddStringToObject(o, "device_id", s_device_id);
    cJSON_AddStringToObject(o, "fw_version", ota_mgr_running_version());
    cJSON_AddStringToObject(o, "token", s_token);
    char *txt = cJSON_PrintUnformatted(o);
    if (txt) { ws_send_json(txt); cJSON_free(txt); }
    cJSON_Delete(o);
}

static void on_text(const char *txt, size_t len)
{
    cJSON *msg = cJSON_ParseWithLength(txt, len);
    if (!msg) return;
    const cJSON *type = cJSON_GetObjectItem(msg, "type");
    if (cJSON_IsString(type) && !strcmp(type->valuestring, "hello")) {
        if (cJSON_IsTrue(cJSON_GetObjectItem(msg, "ok"))) {
            ESP_LOGI(TAG, "bridge accepted hello");
            s_ready = true;
            if (s_conn_cb) s_conn_cb(true);
        }
    } else if (cJSON_IsString(type) && !strcmp(type->valuestring, "error") && !s_ready) {
        ESP_LOGE(TAG, "bridge refused the connection: check the bridge token");
    } else if (s_json_cb) {
        s_json_cb(msg);
    }
    cJSON_Delete(msg);
}

static void on_binary(const uint8_t *p, size_t n)
{
    if (!s_audio_cb || !n) return;
    if (s_have_carry) {                       // keep sample alignment across fragment boundaries
        uint8_t pair[2] = {s_carry, p[0]};
        s_audio_cb(pair, 2);
        p++; n--; s_have_carry = false;
    }
    if (n & 1) { s_carry = p[n - 1]; s_have_carry = true; n--; }
    if (n) s_audio_cb(p, n);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "socket open, sending hello");
        send_hello();
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x01) {             // text, possibly fragmented
            if (d->payload_len > TXT_MAX - 1) break;
            if (d->payload_offset + d->data_len > TXT_MAX - 1) break;
            memcpy(s_txt + d->payload_offset, d->data_ptr, d->data_len);
            if (d->payload_offset + d->data_len == d->payload_len) {
                s_txt[d->payload_len] = 0;
                on_text(s_txt, d->payload_len);
            }
        } else if (d->op_code == 0x02) {
            if (d->payload_offset == 0) s_have_carry = false;
            on_binary((const uint8_t *)d->data_ptr, d->data_len);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
    case WEBSOCKET_EVENT_CLOSED:
        xEventGroupSetBits(s_ev, BIT_DOWN);
        break;
    default:
        break;
    }
}

static void manager_task(void *arg)
{
    int backoff = 1;
    for (;;) {
        xEventGroupClearBits(s_ev, BIT_DOWN);
        esp_websocket_client_config_t cfg = {
            .uri = s_url,
            .buffer_size = 4096,
            .task_stack = 6144,
            .disable_auto_reconnect = true,   // we do our own exponential backoff
            .network_timeout_ms = 8000,
            .pingpong_timeout_sec = 40,
        };
        s_client = esp_websocket_client_init(&cfg);
        esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event, NULL);
        ESP_LOGI(TAG, "connecting to %s", s_url);
        if (esp_websocket_client_start(s_client) == ESP_OK) {
            xEventGroupWaitBits(s_ev, BIT_DOWN, pdTRUE, pdFALSE, portMAX_DELAY);
        }
        bool was_ready = s_ready;
        s_ready = false;
        if (was_ready && s_conn_cb) s_conn_cb(false);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        backoff = was_ready ? 1 : (backoff * 2 > BACKOFF_MAX ? BACKOFF_MAX : backoff * 2);
        ESP_LOGW(TAG, "link down, retrying in %d s", backoff);
        vTaskDelay(pdMS_TO_TICKS(backoff * 1000));
    }
}

esp_err_t ws_client_start(const char *url, const char *token)
{
    if (s_ev) return ESP_ERR_INVALID_STATE;
    strlcpy(s_url, url, sizeof(s_url));
    strlcpy(s_token, token, sizeof(s_token));
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "ai-luk-thep-%02x%02x%02x", mac[3], mac[4], mac[5]);
    s_txt = heap_caps_malloc(TXT_MAX, MALLOC_CAP_SPIRAM);
    s_ev = xEventGroupCreate();
    if (!s_txt || !s_ev) return ESP_ERR_NO_MEM;
    xTaskCreate(manager_task, "ws_mgr", 4096, NULL, 5, NULL);
    return ESP_OK;
}
