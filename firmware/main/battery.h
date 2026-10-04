#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Battery level from the ADC divider on GPIO1 and charging state from GPIO41
// (same readings and level table as the factory firmware).
esp_err_t battery_init(void);
int       battery_level(void);      // 0..100, smoothed over 3 samples
bool      battery_charging(void);   // false when full
bool      battery_present(void);    // false when the ADC reads like an empty socket (USB only)
int       battery_raw(void);        // last averaged ADC reading, for calibration
