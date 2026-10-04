#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

// Persistent settings in NVS (namespace "hbox"). Nothing secret is hard-coded in the repo.
#define SETTING_WIFI_SSID  "wifi_ssid"
#define SETTING_WIFI_PASS  "wifi_pass"
#define SETTING_BRIDGE_URL "bridge_url"
#define SETTING_TOKEN      "token"        // OTA upload token
#define SETTING_BRIDGE_TOKEN "bridge_token" // voice bridge device token (separate from the OTA token)

esp_err_t settings_get(const char *key, char *out, size_t out_len);  // ESP_ERR_NVS_NOT_FOUND if unset
esp_err_t settings_set(const char *key, const char *value);
esp_err_t settings_erase(const char *key);
bool      settings_has(const char *key);
