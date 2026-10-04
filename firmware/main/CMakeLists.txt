idf_component_register(
    SRCS "main.c" "ui.c" "ui_anim.c" "img_text.c" "settings.c" "wifi_mgr.c" "buttons.c" "ota_mgr.c" "console.c" "audio.c" "audio_in.c" "audio_out.c" "audio_test.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_driver_gpio esp_driver_spi esp_driver_ledc esp_lcd app_update esp_wifi esp_netif esp_event esp_http_server esp_timer lwip console esp_app_format mbedtls esp_driver_i2c esp_driver_i2s espressif__esp_codec_dev
)
