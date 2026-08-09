#ifndef _AIPI_LITE_CONFIG_H_
#define _AIPI_LITE_CONFIG_H_

// AIPI-Lite Hardware Configuration
// This header defines hardware-specific pins and settings for the AIPI-Lite board

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==================== POWER MANAGEMENT ====================
// GPIO10 must be set HIGH on boot to keep the device powered
// (prevents deep sleep/battery cutoff)
#define POWER_KEEP_ALIVE_PIN    10

// ==================== AUDIO CODEC (ES8311) - I2S ====================
// AIPI-Lite uses a compact I²S pinout for the ES8311 codec
#define ADC_BCLK_PIN            14   // Bit Clock
#define ADC_LRCLK_PIN           12   // Word Select (LRCLK)
#define ADC_DATA_PIN            13   // Data In (DIN) - microphone input

// Speaker output pins (same codec, different direction)
#define DAC_BCLK_PIN            14   // BCLK shared for both directions
#define DAC_LRCLK_PIN           12   // LRCLK shared for both directions  
#define DAC_DATA_PIN            11   // Data Out (DOUT) - speaker output

// Speaker amplifier enable pin
#define SPEAKER_AMP_ENABLE_PIN  9

// ==================== DISPLAY (ST7735 SPI) ====================
#define LCD_SPI_HOST            SPI3_HOST
#define DISPLAY_MOSI_PIN        17
#define DISPLAY_CLK_PIN         16
#define DISPLAY_DC_PIN          7
#define DISPLAY_RST_PIN         18
#define DISPLAY_CS_PIN          15
#define DISPLAY_BACKLIGHT_PIN   3    // ⚠️ Strapping pin - works but shows warning

// ==================== USER INPUTS ====================
// Left button (GPIO1) - also serves as hardware power button (dual function)
#define LEFT_BUTTON_PIN         1
// Right button (GPIO42) - standard GPIO button
#define RIGHT_BUTTON_PIN        42

// ==================== I2C (for ES8311 codec configuration) ====================
#define I2C_SDA_PIN             5
#define I2C_SCL_PIN             4
#define ES8311_I2C_ADDR         0x18

// ==================== BATTERY MONITORING ====================
#define BATTERY_MONITOR_PIN     2    // ADC input with 12dB attenuation, multiply by 2.0

// ==================== AUDIO SETTINGS ====================
// AIPI-Lite audio runs at 16 kHz (mirrors the working Arduino sketch /
// stock firmware for this board; ES8311 clocked with MCLK 256 * fs).
#define MIC_SAMPLE_RATE         16000
#define MIC_I2S_CHANNELS        2    // stereo capture
#define MIC_CHANNELS            1    // Opus mono encode
#define OPUS_BITRATE            32000
#define OPUS_COMPLEXITY         2

// ==================== INIT FUNCTIONS ====================
esp_err_t aipi_lite_init_power_management(void);
esp_err_t aipi_lite_init_audio_pins(void);
esp_err_t aipi_lite_init_display_pins(void);
esp_err_t aipi_lite_init_button_pins(void);
void aipi_lite_enable_speaker_amp(bool enable);

#ifdef __cplusplus
}
#endif

#endif // _AIPI_LITE_CONFIG_H_
