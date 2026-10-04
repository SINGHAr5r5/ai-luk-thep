#include "buttons.h"
#include "board_config.h"
#include "wifi_mgr.h"
#include "ui.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LONG_PRESS_MS 5000
static const char *TAG = "buttons";
static button_cb_t s_on_press, s_on_release;

void buttons_set_ptt(button_cb_t on_press, button_cb_t on_release)
{
    s_on_press = on_press;
    s_on_release = on_release;
}

static void button_task(void *arg)
{
    TickType_t down_at = 0;
    bool was_pressed = false, reset_fired = false;
    for (;;) {
        bool pressed = gpio_get_level(BOOT_BUTTON_PIN) == 0;   // 30 ms poll doubles as debounce
        if (pressed && !was_pressed) {
            down_at = xTaskGetTickCount();
            reset_fired = false;
            if (s_on_press) s_on_press();
        } else if (!pressed && was_pressed) {
            if (s_on_release) s_on_release();
        }
        if (pressed && !reset_fired && (xTaskGetTickCount() - down_at) * portTICK_PERIOD_MS >= LONG_PRESS_MS) {
            reset_fired = true;
            ESP_LOGW(TAG, "BOOT held %d ms: clearing Wi-Fi credentials", LONG_PRESS_MS);
            if (s_on_release) s_on_release();
            wifi_mgr_clear_credentials();
            ui_set_caption(UI_CAP_RESET);
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void buttons_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    xTaskCreate(button_task, "buttons", 3072, NULL, 5, NULL);
}
