#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

esp_err_t wifi_mgr_init(void);
bool      wifi_mgr_has_credentials(void);
esp_err_t wifi_mgr_connect_saved(uint32_t timeout_ms);   // 3 attempts inside the timeout
esp_err_t wifi_mgr_start_provisioning(void);              // SoftAP + captive portal + QR on screen
void      wifi_mgr_stop_provisioning(void);
esp_err_t wifi_mgr_scan(wifi_ap_record_t *out, uint16_t *count);   // count: in = capacity, out = found
esp_err_t wifi_mgr_save_credentials(const char *ssid, const char *pass);
void      wifi_mgr_clear_credentials(void);               // used by the long BOOT press
bool      wifi_mgr_is_connected(void);
uint32_t  wifi_mgr_down_seconds(void);                    // 0 while connected
const char *wifi_mgr_ip_str(void);                        // "" while not connected
void      wifi_mgr_start_mdns(const char *hostname);      // <hostname>.local
