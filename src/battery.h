#pragma once

// battery.h
#ifndef BATTERY_H
#define BATTERY_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the board's battery monitor (ADC or AXP2101 PMU over I2C).
// Must be called after init_lvgl() on Freenove: the battery sense pin is
// shared with the LCD reset line, and init_lvgl pulses it during panel init.
// Returns ESP_OK when the monitor is usable, otherwise the battery UI shows
// nothing.
esp_err_t oai_battery_init(void);

// Returns the remaining battery charge in percent (0..100), or -1 when no
// battery is present / the monitor is unavailable. Safe to call from any
// task; takes a few milliseconds (ADC read or I2C register read).
int oai_battery_get_percent(void);

#ifdef __cplusplus
}
#endif

#endif // BATTERY_H
