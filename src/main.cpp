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
#include "battery.h"

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

// Interrupt state - toggled by the button polling task below, read by the
// audio uplink/downlink tasks (possibly on the other core). volatile so the
// compiler never caches the flag across loop iterations.
static volatile bool s_interrupted = false;

// Waveshare 1.8/2.06 only expose PWR (AXP2101 PMU) and BOOT (GPIO0) buttons;
// there is no GPIO42 "right button" on these boards. BOOT is the only button
// wired to a readable GPIO, so it's the interrupt button.
#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
#define INTERRUPT_BUTTON_PIN LEFT_BUTTON_PIN  // 1 (left button, also power)
#elif defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
#define INTERRUPT_BUTTON_PIN 0  // BOOT button on GPIO0
#elif defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
#define INTERRUPT_BUTTON_PIN 0  // BOOT button on GPIO0
#else
#define INTERRUPT_BUTTON_PIN 19  // Freenove Media Kit: left button on GPIO19
#endif

// Poll the interrupt button from a task on every board. Polling is required
// instead of a GPIO ISR: pressing the button mutes the ES8311 codec over I2C
// and sends a response.cancel over SCTP, none of which is ISR-safe. The old
// IRAM_ATTR handlers crashed on the 2.06 because ESP_LOG takes a recursive
// lock that aborts in interrupt context (lock_init_generic panic).
static void interrupt_button_poll_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(500)); // let the system stabilize after boot

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;  // no interrupt, just polling
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << INTERRUPT_BUTTON_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;

    if (gpio_config(&io_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure interrupt button GPIO %d", INTERRUPT_BUTTON_PIN);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Interrupt button polling started (%s GPIO %d)", BOARD_TAG, INTERRUPT_BUTTON_PIN);

    bool last_state = gpio_get_level((gpio_num_t)INTERRUPT_BUTTON_PIN);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50)); // poll every 50 ms

        bool current_state = gpio_get_level((gpio_num_t)INTERRUPT_BUTTON_PIN);

        // Negative edge: the button press drives the pin LOW.
        if (last_state == 1 && current_state == 0) {
            s_interrupted = !s_interrupted; // toggle state

            ESP_LOGI(TAG, "Interrupt button pressed: %s",
                     s_interrupted ? "INTERRUPTION START" : "NORMAL MODE");

            if (s_interrupted) {
                oai_stop_audio_playback();
                oai_send_interrupt();
            } else {
                oai_resume_audio_playback();
            }
        }

        last_state = current_state;
    }
}

void oai_init_interrupt_button(void) {
    xTaskCreate(interrupt_button_poll_task, "interrupt_poll", 4096, NULL, 5, NULL);
    // Freenove/AIPI: keep the I2S TX DMA fed with silence while interrupted so
    // the speaker never re-transmits the last frame on underrun (the button
    // buzz that wouldn't stop until pressed again). Compiled out on Waveshare
    // boards (they mute the BSP codec DAC instead).
#if !defined(WAVESHARE_BSP_BOARD) || !WAVESHARE_BSP_BOARD
    oai_start_silence_pump();
#endif
}

bool oai_is_interrupted(void) {
    return s_interrupted;
}

void oai_set_interrupted(bool interrupted) {
    s_interrupted = interrupted;
    ESP_LOGI(TAG, "Interrupt state set: %s", interrupted ? "INTERRUPTED" : "NORMAL");
}

// Battery saver: while Paused (mic muted, server audio ignored) the device is
// doing nothing useful, so kill the backlight after this long to extend battery
// life. Restored as soon as the interrupt button returns the state to
// "Listening" (see status_display_task below).
#define DISPLAY_AUTO_OFF_PAUSE_MS (10 * 1000)

// Reflect the conversation mode on the display ("Listening" / "Paused").
// Runs as its own task so the LVGL update always happens in task context --
// never from the button ISRs on the 2.06/AIPI/Freenove boards -- and works
// uniformly on every board. Initial state is "Listening".
//
// Also handles the display auto-off: once the device has been Paused for
// DISPLAY_AUTO_OFF_PAUSE_MS the backlight is turned off (the screen is
// unreadable in Paused mode anyway and saves battery), and it is switched
// back on immediately when the button restores "Listening".
static void status_display_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(300)); // let lvgl_ui() finish building the screen
    bool last = false;
    bool backlight_off = false;
    TickType_t paused_since = 0;
    lvgl_ui_status_set_text("Listening");
    while (1) {
        bool interrupted = oai_is_interrupted();
        if (interrupted != last) {
            lvgl_ui_status_set_text(interrupted ? "Paused" : "Listening");
            last = interrupted;
            if (interrupted) {
                paused_since = xTaskGetTickCount();
            } else if (backlight_off) {
                // Back to "Listening": wake the display immediately.
                lvgl_ui_set_backlight(true);
                backlight_off = false;
            }
        }
        // Paused for a while and still off: kill the backlight once, then
        // wait for the next state change (the 100 ms poll adds <100 ms jitter
        // to the 10 s threshold, which is irrelevant here).
        if (last && !backlight_off &&
            (xTaskGetTickCount() - paused_since) >= pdMS_TO_TICKS(DISPLAY_AUTO_OFF_PAUSE_MS)) {
            lvgl_ui_set_backlight(false);
            backlight_off = true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Poll the battery every few seconds and push changes to the bottom-of-screen
// indicator. Runs as its own task so the LVGL update always happens in task
// context and the ADC/I2C read never blocks the audio or network tasks.
static void battery_display_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(500)); // let lvgl_ui() finish building the screen
    int last_pct = -2;
    while (1) {
        int pct = oai_battery_get_percent();
        if (pct != last_pct) {
            lvgl_ui_battery_set_percent(pct);
            last_pct = pct;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
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
    xTaskCreate(status_display_task, "status_display", 4096, NULL, 3, NULL);
    // Battery monitor (after init_lvgl so the Freenove shared LCD_RST pin has
    // finished its reset pulse and can be released to the ADC).
    if (oai_battery_init() == ESP_OK) {
      xTaskCreate(battery_display_task, "battery_display", 4096, NULL, 2, NULL);
    }
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
