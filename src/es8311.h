#ifndef ES8311_H
#define ES8311_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the I2C bus and configure the ES8311 codec.
 *
 * Configures the codec for I2S slave mode, 16 kHz, 16-bit data in 32-bit
 * slots (BCLK = 64 * fs = 1.024 MHz), clocked from the I2S MCLK pin at
 * 256 * fs (4.096 MHz @ 16 kHz). Enables both ADC (mic) and DAC (speaker)
 * and unmutes.
 *
 * @return ESP_OK on success
 */
esp_err_t es8311_init(void);

/**
 * @brief Enable ADC + DAC paths and unmute (safe to call again after init).
 * @return ESP_OK on success
 */
esp_err_t es8311_start(void);

/**
 * @brief Set speaker (DAC) volume.
 * @param volume 0..100
 */
void es8311_set_volume(int volume);

/**
 * @brief Set microphone (ADC) analog gain.
 * @param gain_db 0, 3, 6, ..., 24 (clamped to supported steps)
 */
void es8311_set_mic_gain(int gain_db);

#ifdef __cplusplus
}
#endif

#endif  // ES8311_H
