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

// Log the board configuration at startup
#if defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
static const char *BOARD_TAG = "Waveshare 2.06";
#elif defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
static const char *BOARD_TAG = "Waveshare 1.8";
#elif defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
static const char *BOARD_TAG = "AIPI-Lite";
#else
static const char *BOARD_TAG = "Freenove Media Kit";
#endif

// Interrupt state - protected by FreeRTOS task notifications or atomic ops
static bool s_interrupted = false;

// Waveshare 1.8/2.06 only expose PWR (AXP2101 PMU) and BOOT (GPIO0) buttons;
// there is no GPIO42 "right button" on these boards. BOOT is the only button
// wired to a readable GPIO, so it's the interrupt button.
#if defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
#define INTERRUPT_BUTTON_PIN 0  // BOOT button on GPIO0
#else
// Freenove Media Kit: Left button on GPIO19
#define INTERRUPT_BUTTON_PIN 19
#endif

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
#elif defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
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
// Freenove Media Kit and Waveshare 1.8 use polling task instead of interrupt
static void IRAM_ATTR freenove_waveshare_2_06_interrupt_handler(void* arg) {
    bool was_interrupted = s_interrupted;
    s_interrupted = !was_interrupted; // Toggle state
    
    ESP_LOGI(TAG, "Interrupt button pressed: %s", 
             s_interrupted ? "INTERRUPTION START" : "NORMAL MODE");
    
    if (s_interrupted) {
        oai_stop_audio_playback();
    }
}
#endif

// Waveshare 1.8 polling task - checks button state periodically
#if defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
static void waveshare_1_8_interrupt_poll_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(500)); // Wait for system to stabilize
    
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;  // No interrupts, just polling
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << INTERRUPT_BUTTON_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    
    if (gpio_config(&io_conf) == ESP_OK) {
        ESP_LOGI(TAG, "Interrupt button polling started (GPIO %d)", INTERRUPT_BUTTON_PIN);
        
        bool last_state = gpio_get_level((gpio_num_t)INTERRUPT_BUTTON_PIN);
        
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Poll every 50ms
            
            bool current_state = gpio_get_level((gpio_num_t)INTERRUPT_BUTTON_PIN);
            
            // Negative edge trigger (button press = LOW)
            if (last_state == 1 && current_state == 0) {
                bool was_interrupted = s_interrupted;
                s_interrupted = !was_interrupted; // Toggle state
                
                ESP_LOGI(TAG, "Interrupt button pressed: %s", 
                         s_interrupted ? "INTERRUPTION START" : "NORMAL MODE");
                
                if (s_interrupted) {
                    oai_stop_audio_playback();
                }
            }
            
            last_state = current_state;
        }
    } else {
        ESP_LOGE(TAG, "Failed to configure interrupt button polling");
    }
    
    vTaskDelete(NULL);
}
#endif

void oai_init_interrupt_button(void) {
#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
    // Left button (GPIO1) is the interrupt button on AIPI-Lite
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << LEFT_BUTTON_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    
    if (gpio_config(&io_conf) == ESP_OK) {
        gpio_install_isr_service(0);  // AIPI-Lite needs its own ISR service
        gpio_isr_handler_add((gpio_num_t)LEFT_BUTTON_PIN, aipi_lite_interrupt_handler, NULL);
        ESP_LOGI(TAG, "Interrupt button initialized (GPIO %d)", LEFT_BUTTON_PIN);
    } else {
        ESP_LOGE(TAG, "Failed to configure interrupt button");
    }
#elif defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
    // Waveshare 1.8: poll the BOOT button (GPIO0) from a task
    xTaskCreate(waveshare_1_8_interrupt_poll_task, "waveshare_int_poll", 
                4096, NULL, 5, NULL);
#elif defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
    // Waveshare 2.06: BOOT button (GPIO0) as interrupt button (the only
    // readable button; PWR goes to the AXP2101 PMU).
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << 0);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    
    if (gpio_config(&io_conf) == ESP_OK) {
        gpio_install_isr_service(0);  // the 2.06 BSP does not install one
        gpio_isr_handler_add((gpio_num_t)0, waveshare_2_06_interrupt_handler, NULL);
        ESP_LOGI(TAG, "Interrupt button initialized (%s GPIO %d)", BOARD_TAG, 0);
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
        gpio_install_isr_service(0);  // Freenove needs its own ISR service
        gpio_isr_handler_add((gpio_num_t)19, freenove_waveshare_2_06_interrupt_handler, NULL);
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
extern void oai_stop_audio_playback(void);

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_event_loop_create_default());
  
  ESP_LOGI(TAG, "Starting %s build", BOARD_TAG);
  
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
