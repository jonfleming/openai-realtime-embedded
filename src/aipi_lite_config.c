#include "aipi_lite_config.h"
#include "driver/gpio.h"
#include <esp_log.h>

static const char* TAG = "AIPI-Lite";

esp_err_t aipi_lite_init_power_management(void) {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << POWER_KEEP_ALIVE_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure power keep-alive pin: %s", esp_err_to_name(err));
        return err;
    }
    
    // CRITICAL: Set HIGH immediately to prevent power cutoff
    gpio_set_level(POWER_KEEP_ALIVE_PIN, 1);
    ESP_LOGI(TAG, "Power keep-alive pin initialized (HIGH)");
    
    return ESP_OK;
}

esp_err_t aipi_lite_init_audio_pins(void) {
    esp_err_t err = ESP_OK;
    
    // Audio pins are configured by I2S driver, but we need to ensure
    // the speaker amp enable pin is properly configured
    gpio_config_t amp_conf = {};
    amp_conf.intr_type = GPIO_INTR_DISABLE;
    amp_conf.mode = GPIO_MODE_OUTPUT;
    amp_conf.pin_bit_mask = (1ULL << SPEAKER_AMP_ENABLE_PIN);
    amp_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    amp_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    
    err = gpio_config(&amp_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure speaker amp enable pin: %s", esp_err_to_name(err));
        return err;
    }
    
    // Disable amplifier initially (will be enabled when audio starts)
    aipi_lite_enable_speaker_amp(false);
    ESP_LOGI(TAG, "Audio pins initialized");
    
    return ESP_OK;
}

esp_err_t aipi_lite_init_display_pins(void) {
    // Display pins are configured by LCD driver via esp_lcd_panel_io
    // This function exists for API completeness and future display initialization
    ESP_LOGI(TAG, "Display pins initialized (configured by LCD driver)");
    return ESP_OK;
}

esp_err_t aipi_lite_init_button_pins(void) {
    // Left button (GPIO1) - hardware power button with internal pull-up
    gpio_config_t left_btn_conf = {};
    left_btn_conf.intr_type = GPIO_INTR_DISABLE;
    left_btn_conf.mode = GPIO_MODE_INPUT;
    left_btn_conf.pin_bit_mask = (1ULL << LEFT_BUTTON_PIN);
    left_btn_conf.pull_up_en = GPIO_PULLUP_ENABLE;  // Power button typically needs pull-up
    left_btn_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    
    esp_err_t err = gpio_config(&left_btn_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure left button pin: %s", esp_err_to_name(err));
        return err;
    }
    
    // Right button (GPIO42) - standard GPIO button with pull-down
    gpio_config_t right_btn_conf = {};
    right_btn_conf.intr_type = GPIO_INTR_DISABLE;
    right_btn_conf.mode = GPIO_MODE_INPUT;
    right_btn_conf.pin_bit_mask = (1ULL << RIGHT_BUTTON_PIN);
    right_btn_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    right_btn_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    
    err = gpio_config(&right_btn_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure right button pin: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "Button pins initialized");
    return ESP_OK;
}

void aipi_lite_enable_speaker_amp(bool enable) {
    gpio_set_level(SPEAKER_AMP_ENABLE_PIN, enable ? 1 : 0);
    if (enable) {
        ESP_LOGV(TAG, "Speaker amplifier enabled");
    } else {
        ESP_LOGV(TAG, "Speaker amplifier disabled");
    }
}
