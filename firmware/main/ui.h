#pragma once
#include "esp_err.h"
#include "app_state.h"

esp_err_t ui_init(void);              // LCD + LVGL + backlight
void      ui_show_state(app_state_t s);
void      ui_set_brightness(uint8_t pct);

typedef enum {
    UI_CAP_NONE, UI_CAP_SCAN_AP, UI_CAP_SCAN_SETUP, UI_CAP_CONNECTING, UI_CAP_CONNECTED,
    UI_CAP_FAILED, UI_CAP_TESTING, UI_CAP_RESET, UI_CAP_WIFI_LOST, UI_CAP_UPDATING, UI_CAP_UPDATE_DONE,
} ui_caption_t;

void ui_show_qr(const char *payload, ui_caption_t caption);  // replaces the Orb with a QR code
void ui_set_caption(ui_caption_t caption);                   // Thai caption line at the bottom
void ui_set_status_text(const char *text);                   // small ASCII status line (overrides state text)
void ui_set_ota_progress(int pct);                           // 0..100, shown while ST_OTA_UPDATING

// Text for the screen, shaped on the Pi and sent as an A8 bitmap. role: "stt" (what was heard) or "reply".
// Takes ownership of `a8` (heap_caps_malloc'd, w*h bytes). The text is hidden again 12 s after the board
// goes idle, and cleared when a new turn starts listening.
void ui_show_text(const char *role, int w, int h, uint8_t *a8);
void ui_clear_text(void);
void ui_set_battery(int level, bool charging, bool present);   // battery % next to the clock
