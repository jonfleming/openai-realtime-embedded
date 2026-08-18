#pragma once

// lcd.h
#ifndef LCD_H
#define LCD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_err.h"

extern lv_disp_t * disp_handle;

esp_err_t init_lvgl(void);
void lvgl_ui(void);
void lvgl_ui_label_set_text(const char *text);
void lvgl_ui_status_set_text(const char *text);
void lvgl_ui_clear_messages(void);
// Show the battery indicator at the bottom of the screen. pct is 0..100;
// pass -1 to hide it (no battery / monitor unavailable).
void lvgl_ui_battery_set_percent(int pct);

#ifdef __cplusplus
}
#endif

#endif // LCD_H