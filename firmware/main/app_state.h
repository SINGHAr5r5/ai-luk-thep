#pragma once

typedef enum {
    ST_BOOT, ST_WIFI_CONNECTING, ST_WIFI_PROVISIONING, ST_BRIDGE_CONNECTING,
    ST_IDLE, ST_LISTENING, ST_THINKING, ST_SPEAKING, ST_OTA_UPDATING, ST_ERROR,
} app_state_t;

void        app_set_state(app_state_t s);
app_state_t app_get_state(void);
void        app_enter_idle(void);   // IDLE with the IP / version status line
