#include "main.h"

#include <esp_event.h>
#include <esp_log.h>
#include <peer.h>

#ifndef LINUX_BUILD
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_io.h"
#include "lcd.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "esp_http_server.h"
#include "wifi_config.h"

// AIPI-Lite hardware initialization
#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
#include "aipi_lite_config.h"
#include "es8311.h"
#endif

static const char *TAG = "Main";

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_event_loop_create_default());
  
#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
  // Initialize AIPI-Lite specific hardware (power, audio, buttons)
  if (aipi_lite_init_power_management() != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize power management");
    esp_restart();
  }
  if (aipi_lite_init_audio_pins() != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize audio pins");
    esp_restart();
  }
  if (aipi_lite_init_button_pins() != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize button pins");
    esp_restart();
  }
#endif

  peer_init();
  oai_init_audio_capture();

#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
  // Configure the ES8311 codec over I2C for the shared 16 kHz I2S bus.
  if (es8311_init() != ESP_OK) {
    ESP_LOGE(TAG, "ES8311 codec init failed - audio will not work");
  }
#endif

  oai_init_audio_decoder();

#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
  // AIPI-Lite requires enabling the external speaker amp for audio playback.
  aipi_lite_enable_speaker_amp(true);
#endif

  esp_err_t lvgl_ret = init_lvgl();
  if (lvgl_ret == ESP_OK) {
    lvgl_ui();
  } else {
    ESP_LOGE(TAG, "Display/LVGL init failed (%s). Continuing in headless mode.", esp_err_to_name(lvgl_ret));
  }

  wifi_config_init();
  oai_webrtc();
}
#else
int main(void) {
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  peer_init();
  oai_webrtc();
}
#endif
