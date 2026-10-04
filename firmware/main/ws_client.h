#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

// WebSocket link to the voice bridge on the Pi (see bridge/README.md).
// Handles hello/token, reassembly of fragmented frames and reconnects with backoff 1,2,4..30 s.
typedef void (*ws_conn_cb_t)(bool ready);                      // ready = hello accepted by the bridge
typedef void (*ws_json_cb_t)(cJSON *msg);                      // every JSON message after hello (not freed by the callee)
typedef void (*ws_audio_cb_t)(const uint8_t *pcm, size_t len); // 16 kHz mono s16le, even length

void      ws_client_set_callbacks(ws_conn_cb_t conn, ws_json_cb_t json, ws_audio_cb_t audio);
esp_err_t ws_client_start(const char *url, const char *token); // copies both, starts the manager task
void      ws_send_json(const char *json);
void      ws_send_audio(const uint8_t *pcm, size_t len);
bool      ws_is_connected(void);                               // connected AND hello accepted
