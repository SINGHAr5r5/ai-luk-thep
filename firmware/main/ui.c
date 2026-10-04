#include "ui.h"
#include "ui_anim.h"
#include "board_config.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ui";
static lv_display_t *s_disp;

#define BL_TIMER   LEDC_TIMER_0
#define BL_CHANNEL LEDC_CHANNEL_0
#define BL_MAX     ((1 << 10) - 1)

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BL_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .gpio_num = DISPLAY_BACKLIGHT_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = DISPLAY_BACKLIGHT_INVERT,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void ui_set_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, BL_MAX * pct / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
}

esp_err_t ui_init(void)
{
    backlight_init();  // stays dark until the first frame is drawn

    spi_bus_config_t bus = {
        .mosi_io_num = DISPLAY_SPI_MOSI_PIN,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = DISPLAY_SPI_SCLK_PIN,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = DISPLAY_SPI_CS_PIN,
        .dc_gpio_num = DISPLAY_SPI_DC_PIN,
        .spi_mode = 0,
        .pclk_hz = DISPLAY_SPI_CLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_SPI_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = DISPLAY_SPI_RESET_PIN,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
    uint8_t vcom = 0x38;  // value used by the factory firmware (register 0xBB)
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, 0xBB, &vcom, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = DISPLAY_WIDTH * 40,
        .double_buffer = true,
        .hres = DISPLAY_WIDTH,
        .vres = DISPLAY_HEIGHT,
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {.buff_dma = true, .swap_bytes = true},
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    lvgl_port_lock(0);
    ui_anim_create();
    lvgl_port_unlock();

    vTaskDelay(pdMS_TO_TICKS(100));  // let the first frame reach the panel
    ui_set_brightness(70);
    return ESP_OK;
}

void ui_show_state(app_state_t s)
{
    if (!s_disp) return;
    lvgl_port_lock(0);
    ui_anim_set_state(s);
    lvgl_port_unlock();
}

void ui_show_qr(const char *payload, ui_caption_t caption)
{
    if (!s_disp) return;
    lvgl_port_lock(0);
    ui_anim_show_qr(payload, caption);
    lvgl_port_unlock();
}

void ui_set_caption(ui_caption_t caption)
{
    if (!s_disp) return;
    lvgl_port_lock(0);
    ui_anim_set_caption(caption);
    lvgl_port_unlock();
}

void ui_set_status_text(const char *text)
{
    if (!s_disp) return;
    lvgl_port_lock(0);
    ui_anim_set_status(text);
    lvgl_port_unlock();
}

void ui_set_ota_progress(int pct)
{
    if (!s_disp) return;
    lvgl_port_lock(0);
    ui_anim_set_ota_progress(pct);
    lvgl_port_unlock();
}
