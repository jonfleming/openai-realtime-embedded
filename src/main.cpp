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

// Interrupt state - protected by FreeRTOS task notifications or atomic ops
static bool s_interrupted = false;

// Button interrupt handler - called when interrupt button is pressed
#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
static void IRAM_ATTR aipi_lite_interrupt_handler(void* arg) {
    bool was_interrupted = s_interrupted;
    s_interrupted = !was_interrupted; // Toggle state
    
    ESP_LOGI(TAG, "Interrupt button pressed: %s", 
             s_interrupted ? "INTERRUPTION START" : "NORMAL MODE");
    
    if (s_interrupted) {
        oai_stop_audio_playback();
    }
}
#elif defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
// Waveshare 1.8 uses BSP-managed buttons; we'll check button state from a task
static void IRAM_ATTR waveshare_1_8_interrupt_handler(void* arg) {
    bool was_interrupted = s_interrupted;
    s_interrupted = !was_interrupted; // Toggle state
    
    ESP_LOGI(TAG, "Interrupt button pressed: %s", 
             s_interrupted ? "INTERRUPTION START" : "NORMAL MODE");
    
    if (s_interrupted) {
        oai_stop_audio_playback();
    }
}
#elif defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
// Waveshare 2.06 uses BSP-managed buttons; we'll check button state from a task
static void IRAM_ATTR waveshare_2_06_interrupt_handler(void* arg) {
    bool was_interrupted = s_interrupted;
    s_interrupted = !was_interrupted; // Toggle state
    
    ESP_LOGI(TAG, "Interrupt button pressed: %s", 
             s_interrupted ? "INTERRUPTION START" : "NORMAL MODE");
    
    if (s_interrupted) {
        oai_stop_audio_playback();
    }
}
#else
// Freenove Media Kit
static void IRAM_ATTR freenove_interrupt_handler(void* arg) {
    bool was_interrupted = s_interrupted;
    s_interrupted = !was_interrupted; // Toggle state
    
    ESP_LOGI(TAG, "Interrupt button pressed: %s", 
             s_interrupted ? "INTERRUPTION START" : "NORMAL MODE");
    
    if (s_interrupted) {
        oai_stop_audio_playback();
    }
}
#endif

void oai_init_interrupt_button(void) {
    ESP_LOGI(TAG, "Initializing interrupt button...");
#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
    // Left button (GPIO1) is the interrupt button on AIPI-Lite
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << LEFT_BUTTON_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    
    if (gpio_config(&io_conf) == ESP_OK) {
        gpio_install_isr_service(0);
        gpio_isr_handler_add((gpio_num_t)LEFT_BUTTON_PIN, aipi_lite_interrupt_handler, NULL);
        ESP_LOGI(TAG, "Interrupt button initialized (GPIO %d)", LEFT_BUTTON_PIN);
    } else {
        ESP_LOGE(TAG, "Failed to configure interrupt button");
    }
#elif defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
    // Waveshare 1.8: BSP manages buttons; check BSP documentation for button pin
    // For now, we'll use GPIO0 (boot button) as interrupt button
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << 0); // Boot button on GPIO0
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    
    if (gpio_config(&io_conf) == ESP_OK) {
        gpio_install_isr_service(0);
        gpio_isr_handler_add((gpio_num_t)0, waveshare_1_8_interrupt_handler, NULL);
        ESP_LOGI(TAG, "Interrupt button initialized (Waveshare 1.8 GPIO %d)", 42);
    } else {
        ESP_LOGE(TAG, "Failed to configure interrupt button");
    }
#elif defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
    // Waveshare 2.06: BSP manages buttons; check BSP documentation for button pin
    // Using GPIO42 (right button) as interrupt button
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << 42);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    
    if (gpio_config(&io_conf) == ESP_OK) {
        gpio_install_isr_service(0);
        gpio_isr_handler_add((gpio_num_t)42, waveshare_2_06_interrupt_handler, NULL);
        ESP_LOGI(TAG, "Interrupt button initialized (Waveshare 2.06 GPIO %d)", 42);
    } else {
        ESP_LOGE(TAG, "Failed to configure interrupt button");
    }
#else
    // Freenove Media Kit: Left button on GPIO19
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << 19);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    
    if (gpio_config(&io_conf) == ESP_OK) {
        gpio_install_isr_service(0);
        gpio_isr_handler_add((gpio_num_t)19, freenove_interrupt_handler, NULL);
        ESP_LOGI(TAG, "Interrupt button initialized (Freenove GPIO %d)", 19);
    } else {
        ESP_LOGE(TAG, "Failed to configure interrupt button");
    }
#endif
}

bool oai_is_interrupted(void) {
    return s_interrupted;
}

void oai_set_interrupted(bool interrupted) {
    s_interrupted = interrupted;
    ESP_LOGI(TAG, "Interrupt state set: %s", interrupted ? "INTERRUPTED" : "NORMAL");
}

// Stop audio playback (speaker)
void oai_stop_audio_playback(void) {
#ifndef LINUX_BUILD
#if defined(WAVESHARE_BSP_BOARD) && WAVESHARE_BSP_BOARD
    // For Waveshare boards using BSP codec devices - close the codec device
    extern void* s_spk_codec_dev;
    if (s_spk_codec_dev != NULL) {
        extern esp_err_t esp_codec_dev_close(void*);
        esp_codec_dev_close(s_spk_codec_dev);
        ESP_LOGI(TAG, "Audio playback stopped (BSP codec)");
    }
#else
    // For other boards - disable I2S TX channel briefly to stop audio
    extern void* s_i2s_tx_chan;
    if (s_i2s_tx_chan != NULL) {
        extern esp_err_t i2s_channel_disable(void*);
        extern esp_err_t i2s_channel_enable(void*);
        i2s_channel_disable(s_i2s_tx_chan);
        vTaskDelay(pdMS_TO_TICKS(10)); // Brief delay to stop audio
        i2s_channel_enable(s_i2s_tx_chan);
        ESP_LOGI(TAG, "Audio playback stopped (I2S TX)");
    }
#endif
#else
    ESP_LOGI(TAG, "Audio playback stop (Linux mode - no-op)");
#endif
}

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

  // Initialize interrupt button AFTER audio but BEFORE WebRTC
  oai_init_interrupt_button();
  
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
