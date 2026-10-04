#include "audio.h"
#include "board_config.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "audio";
static esp_codec_dev_handle_t s_dev;

esp_codec_dev_handle_t audio_codec(void) { return s_dev; }

esp_err_t audio_init(void)
{
    if (s_dev) return ESP_OK;

    // ---- control bus ----
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bc = {
        .i2c_port = AUDIO_I2C_PORT,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bc, &bus), TAG, "i2c bus");

    // ---- I2S (full duplex, 16 kHz, 16-bit, mono) ----
    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 6;
    cc.dma_frame_num = 320;   // 20 ms
    cc.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, &tx, &rx), TAG, "i2s channel");
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_GPIO_MCLK, .bclk = AUDIO_I2S_GPIO_BCLK, .ws = AUDIO_I2S_GPIO_WS,
            .dout = AUDIO_I2S_GPIO_DOUT, .din = AUDIO_I2S_GPIO_DIN,
        },
    };
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &sc), TAG, "i2s tx init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &sc), TAG, "i2s rx init");

    // ---- ES8311 via esp_codec_dev ----
    audio_codec_i2c_cfg_t i2c_cfg = {.port = AUDIO_I2C_PORT, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    es8311_codec_cfg_t es = {
        .ctrl_if = ctrl,
        .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = AUDIO_CODEC_PA_PIN,
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es);
    if (!codec) { ESP_LOGE(TAG, "ES8311 not found on I2C"); return ESP_ERR_NOT_FOUND; }

    audio_codec_i2s_cfg_t id = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&id);
    esp_codec_dev_cfg_t dc = {.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT, .codec_if = codec, .data_if = data};
    s_dev = esp_codec_dev_new(&dc);
    if (!s_dev) return ESP_FAIL;

    esp_codec_dev_sample_info_t fs = {.sample_rate = AUDIO_RATE, .channel = 1, .bits_per_sample = 16};
    if (esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "codec open failed"); return ESP_FAIL; }
    // Tuned with the console `selftest` sweep: no clipping, clean 1 kHz at vol 40 / gain 18 with the speaker
    // next to the mic (worst case). Real speech sits further away, hence a little more input gain.
    esp_codec_dev_set_in_gain(s_dev, 24.0f);
    esp_codec_dev_set_out_vol(s_dev, 50);
    ESP_LOGI(TAG, "ES8311 ready: %d Hz, 16-bit, mono", AUDIO_RATE);
    return ESP_OK;
}
