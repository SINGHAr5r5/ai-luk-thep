#include <stdio.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board_config.h"
#include "app_state.h"
#include "ui.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "buttons.h"
#include "ota_mgr.h"
#include "console.h"
#include "audio.h"
#include "audio_in.h"
#include "audio_out.h"
#include "audio_test.h"

static const char *TAG = "main";
static volatile app_state_t s_state = ST_BOOT;

#define WIFI_CONNECT_TIMEOUT_MS 20000
#define WIFI_LOST_WARN_S        60

void app_set_state(app_state_t s)
{
    s_state = s;
    ui_show_state(s);
}

app_state_t app_get_state(void)
{
    return s_state;
}

void app_enter_idle(void)
{
    char line[40];
    snprintf(line, sizeof(line), "%s  v%s%s", wifi_mgr_ip_str(), ota_mgr_running_version(),
             ota_mgr_was_rolled_back() ? "  (rolled back)" : "");
    app_set_state(ST_IDLE);
    ui_set_status_text(line);
}

static void audio_boot_task(void *arg)
{
    if (audio_init() == ESP_OK && audio_in_init() == ESP_OK && audio_out_init() == ESP_OK) {
        audio_test_init();
    } else {
        ESP_LOGE(TAG, "audio unavailable - continuing without sound");
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Keep the board powered when running from battery (see board_config.h).
    gpio_set_direction(POWER_HOLD_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_HOLD_PIN, 1);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "AI Luk Thep booting, running partition: %s", esp_ota_get_running_partition()->label);

    ota_mgr_init();   // starts the 60 s rollback watchdog when this image was just OTA-flashed
    ESP_LOGI(TAG, "firmware %s", ota_mgr_running_version());

    ESP_ERROR_CHECK(ui_init());
    app_set_state(ST_BOOT);

#ifdef OTA_TEST_BREAK
    // Test build for the rollback acceptance check: crash before anything confirms the image.
    ESP_LOGE(TAG, "OTA_TEST_BREAK: crashing on purpose");
    vTaskDelay(pdMS_TO_TICKS(500));
    abort();
#endif

    ESP_ERROR_CHECK(wifi_mgr_init());
    buttons_init();
    console_start();

    // opening the ES8311 takes ~5 s, so do it beside the Wi-Fi connect instead of in front of it
    xTaskCreate(audio_boot_task, "audio_boot", 4096, NULL, 4, NULL);

    bool online = false;
    if (wifi_mgr_has_credentials()) {
        app_set_state(ST_WIFI_CONNECTING);
        online = (wifi_mgr_connect_saved(WIFI_CONNECT_TIMEOUT_MS) == ESP_OK);
        if (!online) {
            ui_set_caption(UI_CAP_FAILED);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    if (online) {
        wifi_mgr_start_mdns("ai-luk-thep");
        ota_mgr_start_push_server();
        app_enter_idle();   // Phase 5/6: BRIDGE_CONNECTING goes here
        // Phase 6 will add "bridge connected" to this condition; for now Wi-Fi + push server is healthy.
        ota_mgr_mark_valid_if_healthy();
    } else {
        app_set_state(ST_WIFI_PROVISIONING);
        wifi_mgr_start_provisioning();
    }

    // Watchdog for a lost link while running (the plan: warn only after 60 s, don't drop back to QR).
    bool warned = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!online) continue;
        if (wifi_mgr_down_seconds() > WIFI_LOST_WARN_S && !warned) {
            warned = true;
            app_set_state(ST_ERROR);
            ui_set_caption(UI_CAP_WIFI_LOST);
        } else if (warned && wifi_mgr_is_connected()) {
            warned = false;
            app_enter_idle();
        }
    }
}
