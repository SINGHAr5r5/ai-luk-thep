#include "battery.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "battery";
static adc_oneshot_unit_handle_t s_adc;
static volatile int s_raw, s_level;
static volatile bool s_charging;
static int s_hist[3], s_hist_n;

// raw ADC (12 bit, 12 dB attenuation) -> percent, measured by the factory firmware
static const struct { int adc; int level; } LUT[] = {{1980, 0}, {2081, 20}, {2163, 40}, {2250, 60}, {2340, 80}, {2480, 100}};

static int level_from_raw(int raw)
{
    if (raw < LUT[0].adc) return 0;
    if (raw >= LUT[5].adc) return 100;
    for (int i = 0; i < 5; i++) {
        if (raw >= LUT[i].adc && raw < LUT[i + 1].adc) {
            float t = (float)(raw - LUT[i].adc) / (LUT[i + 1].adc - LUT[i].adc);
            return LUT[i].level + (int)(t * (LUT[i + 1].level - LUT[i].level));
        }
    }
    return 0;
}

static void sample(void *arg)
{
    s_charging = gpio_get_level(POWER_CHARGE_DETECT_PIN) == 1;
    int v = 0;
    if (adc_oneshot_read(s_adc, ADC_CHANNEL_0, &v) != ESP_OK) return;
    if (s_hist_n < 3) s_hist[s_hist_n++] = v;
    else { s_hist[0] = s_hist[1]; s_hist[1] = s_hist[2]; s_hist[2] = v; }
    int sum = 0;
    for (int i = 0; i < s_hist_n; i++) sum += s_hist[i];
    s_raw = sum / s_hist_n;
    s_level = level_from_raw(s_raw);
}

esp_err_t battery_init(void)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << POWER_CHARGE_DETECT_PIN, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io);
    adc_oneshot_unit_init_cfg_t u = {.unit_id = ADC_UNIT_1, .ulp_mode = ADC_ULP_MODE_DISABLE};
    esp_err_t err = adc_oneshot_new_unit(&u, &s_adc);
    if (err != ESP_OK) return err;
    adc_oneshot_chan_cfg_t c = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    err = adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &c);
    if (err != ESP_OK) return err;
    for (int i = 0; i < 3; i++) sample(NULL);       // have a value straight away
    const esp_timer_create_args_t a = {.callback = sample, .name = "battery"};
    esp_timer_handle_t t;
    esp_timer_create(&a, &t);
    esp_timer_start_periodic(t, 5 * 1000000LL);
    ESP_LOGI(TAG, "raw %d -> %d%%, charging %d", s_raw, s_level, s_charging);
    return ESP_OK;
}

int battery_level(void) { return s_level; }
bool battery_charging(void) { return s_charging && s_level < 100; }
bool battery_present(void) { return s_raw >= 1500; }
int battery_raw(void) { return s_raw; }
