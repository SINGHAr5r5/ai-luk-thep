#pragma once
#include <stdbool.h>
#include "esp_err.h"

// OTA: push from the Mac (POST /ota) with rollback protection.
//  - a freshly OTA-flashed image is "pending verify"; it must call ota_mgr_mark_valid_if_healthy()
//    within 60 s (Wi-Fi up, push server running) or the board reboots into the previous image
//  - the upload must carry X-OTA-Token and X-OTA-SHA256 headers
esp_err_t   ota_mgr_init(void);                    // rollback state check + watchdog; creates the token on first boot
esp_err_t   ota_mgr_start_push_server(void);       // HTTP on :80  POST /ota  GET /info
esp_err_t   ota_mgr_check_and_pull(const char *manifest_url);   // Phase 8
void        ota_mgr_mark_valid_if_healthy(void);
const char *ota_mgr_running_version(void);
bool        ota_mgr_was_rolled_back(void);
void        ota_mgr_on_progress(void (*cb)(int pct));
