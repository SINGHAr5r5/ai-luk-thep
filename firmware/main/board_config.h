#pragma once
// SpotPear ESP32-AI-1.54 V2.0 (ESP32-S3 N16R8). Pin map taken from xiaozhi-esp32
// main/boards/spotpear/sp-esp32-s3-1.54-muma and cross-checked against the factory
// firmware backup (board id "sp-esp32-s3-1.54-muma-a": ST7789 + ES8311 + CST816D).
#include "driver/gpio.h"

// ---- power ----
// GPIO3 is the power latch: HIGH keeps the board powered when running from battery.
// The factory firmware drives it LOW + deep-sleeps to switch the board off.
#define POWER_HOLD_PIN          GPIO_NUM_3
#define POWER_CHARGE_DETECT_PIN GPIO_NUM_41

// ---- LCD: ST7789 240x240, SPI ----
#define DISPLAY_WIDTH           240
#define DISPLAY_HEIGHT          240
#define DISPLAY_SPI_HOST        SPI3_HOST
#define DISPLAY_SPI_SCLK_PIN    GPIO_NUM_4
#define DISPLAY_SPI_MOSI_PIN    GPIO_NUM_2
#define DISPLAY_SPI_CS_PIN      GPIO_NUM_5
#define DISPLAY_SPI_DC_PIN      GPIO_NUM_47
#define DISPLAY_SPI_RESET_PIN   GPIO_NUM_38
#define DISPLAY_SPI_CLK_HZ      (40 * 1000 * 1000)
#define DISPLAY_INVERT_COLOR    true
#define DISPLAY_BACKLIGHT_PIN   GPIO_NUM_42
#define DISPLAY_BACKLIGHT_INVERT true   // LOW = on

// ---- audio: ES8311 codec (I2C 0x18) + I2S ----
#define AUDIO_SAMPLE_RATE       16000   // the voice pipeline is 16 kHz / 16-bit / mono end to end (factory used 24 kHz)
#define AUDIO_I2C_PORT          I2C_NUM_0
#define AUDIO_CODEC_ADDR_7BIT   0x18
#define AUDIO_I2S_GPIO_MCLK     GPIO_NUM_16
#define AUDIO_I2S_GPIO_WS       GPIO_NUM_45
#define AUDIO_I2S_GPIO_BCLK     GPIO_NUM_9
#define AUDIO_I2S_GPIO_DIN      GPIO_NUM_10   // codec ADC -> ESP (microphone)
#define AUDIO_I2S_GPIO_DOUT     GPIO_NUM_8    // ESP -> codec DAC (speaker)
#define AUDIO_CODEC_PA_PIN      GPIO_NUM_46   // speaker amplifier enable
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_15
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_14

// ---- touch: CST816D (I2C 0x15) ----
#define TOUCH_I2C_SDA_PIN       GPIO_NUM_11
#define TOUCH_I2C_SCL_PIN       GPIO_NUM_7
#define TOUCH_RST_PIN           GPIO_NUM_6
#define TOUCH_INT_PIN           GPIO_NUM_12

// ---- misc ----
#define BOOT_BUTTON_PIN         GPIO_NUM_0
#define STATUS_LED_PIN          GPIO_NUM_48   // WS2812
