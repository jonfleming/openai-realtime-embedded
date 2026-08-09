/*
 * Minimal ES8311 codec driver for AIPI-Lite.
 *
 * Register init sequence mirrors the reference used by the stock firmware
 * for this exact board (ESPHome es8311 component / the working Arduino
 * sketch) and is configured for:
 *   - I2S slave mode (ESP32 is the I2S master)
 *   - 16 kHz, 16-bit data in 32-bit slots (BCLK = 64 * fs = 1.024 MHz,
 *     MCLK/BCLK = 4)
 *   - MCLK supplied on the I2S MCLK pin at 256 * fs (4.096 MHz @ 16 kHz)
 *   - ADC (mic) + DAC (speaker) both enabled
 *
 * Critical details verified against the working reference:
 *   - Register 0x00 (RESET) must be left at 0x80 (power-on command) as the
 *     LAST write. Leaving it at 0x00 powers the codec down: I2C still ACKs
 *     and registers read back, but both the ADC and DAC stay silent.
 *   - The 16 kHz clock coefficients are CLK2=0x08 (pre_mult=1), CLK3=0x10
 *     (adc_osr=16), CLK4=0x20 (dac_osr=32), CLK6=0x03 (bclk_div=4).
 *   - ADC volume is 0xC8 (REG17), DAC volume 0xBF (REG32), mic gain 0x00
 *     (REG16). Do not add extra register writes on top of the reference
 *     sequence - the defaults it leaves alone are correct.
 *
 * Uses the ESP-IDF v5.x I2C master driver (driver/i2c_master.h).
 * The ES8311 I2C address is 0x18 (7-bit) with CE low, 0x19 with CE high;
 * both are probed at startup and the one that ACKs is used.
 */

#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD

#include "es8311.h"

#include <string.h>
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "aipi_lite_config.h"

static const char *TAG = "ES8311";

#define ES8311_I2C_FREQ_HZ 100000
#define ES8311_I2C_TIMEOUT_MS 100

// ES8311 7-bit I2C addresses (CE pin low -> 0x18, CE high -> 0x19).
#define ES8311_ADDR_CE_LOW 0x18
#define ES8311_ADDR_CE_HIGH 0x19

// ---- Register map (addresses from the ES8311 datasheet) ----
#define ES8311_RESET_REG        0x00  // reset + power-on command (0x80)
#define ES8311_CLK_MANAGER1     0x01  // MCLK source, clock enables
#define ES8311_CLK_MANAGER2     0x02  // pre_div, pre_mult
#define ES8311_CLK_MANAGER3     0x03  // fs_mode, adc_osr
#define ES8311_CLK_MANAGER4     0x04  // dac_osr
#define ES8311_CLK_MANAGER5     0x05  // adc_div, dac_div
#define ES8311_CLK_MANAGER6     0x06  // bclk_div, sclk invert
#define ES8311_CLK_MANAGER7     0x07  // lrck_h
#define ES8311_CLK_MANAGER8     0x08  // lrck_l
#define ES8311_SDPIN_REG        0x09  // DAC serial port (format, width)
#define ES8311_SDPOUT_REG       0x0A  // ADC serial port (format, width)
#define ES8311_SYSTEM_REG0D     0x0D  // power up analog
#define ES8311_SYSTEM_REG0E     0x0E  // enable PGA + ADC modulator
#define ES8311_SYSTEM_REG12     0x12  // power up DAC
#define ES8311_SYSTEM_REG13     0x13  // output to HP drive
#define ES8311_SYSTEM_REG14     0x14  // analog / DMIC mic select
#define ES8311_ADC_REG16        0x16  // mic gain
#define ES8311_ADC_REG17        0x17  // ADC volume
#define ES8311_ADC_REG1C        0x1C  // ADC EQ bypass, DC offset cancel
#define ES8311_DAC_REG31        0x31  // DAC mute bits
#define ES8311_DAC_REG32        0x32  // DAC volume
#define ES8311_DAC_REG37        0x37  // DAC EQ bypass

// Fail fast on I2C write problems so a missing/wrong codec is obvious in the log.
#define ES8311_WRITE(reg, val)                                                    \
    do {                                                                          \
        esp_err_t _err = es8311_write_reg((reg), (val));                          \
        if (_err != ESP_OK) {                                                     \
            ESP_LOGE(TAG, "init aborted: write reg 0x%02x=0x%02x failed: %s",     \
                     (unsigned)(reg), (unsigned)(val), esp_err_to_name(_err));    \
            return _err;                                                          \
        }                                                                         \
    } while (0)

static i2c_master_bus_handle_t s_bus_handle = NULL;
static i2c_master_dev_handle_t s_dev_handle = NULL;

static esp_err_t es8311_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev_handle, buf, sizeof(buf),
                               ES8311_I2C_TIMEOUT_MS);
}

static int es8311_read_reg(uint8_t reg)
{
    uint8_t val = 0;
    esp_err_t err = i2c_master_transmit_receive(s_dev_handle, &reg, 1, &val, 1,
                                                ES8311_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return -1;
    }
    return (int)val;
}

// Create the I2C bus, then probe 0x18 / 0x19 and attach whichever ACKs.
static esp_err_t es8311_init_bus(void)
{
    if (s_bus_handle != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t addr = 0;
    uint8_t addr_candidates[2] = {ES8311_ADDR_CE_LOW, ES8311_ADDR_CE_HIGH};
    for (int i = 0; i < 2; i++) {
        if (i2c_master_probe(s_bus_handle, addr_candidates[i],
                             ES8311_I2C_TIMEOUT_MS) == ESP_OK) {
            addr = addr_candidates[i];
            ESP_LOGI(TAG, "ES8311 found at I2C address 0x%02x", addr);
            break;
        }
    }
    if (addr == 0) {
        ESP_LOGE(TAG, "No ES8311 ACK on I2C bus (tried 0x%02x, 0x%02x on SDA=%d SCL=%d)",
                 ES8311_ADDR_CE_LOW, ES8311_ADDR_CE_HIGH, I2C_SDA_PIN, I2C_SCL_PIN);
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = ES8311_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

esp_err_t es8311_init(void)
{
    esp_err_t err = es8311_init_bus();
    if (err != ESP_OK) {
        return err;
    }

    // ---- Reset: hold digital blocks in reset, then release (still powered down) ----
    ES8311_WRITE(ES8311_RESET_REG, 0x1F);
    vTaskDelay(pdMS_TO_TICKS(20));
    ES8311_WRITE(ES8311_RESET_REG, 0x00);

    // ---- Clock: 16 kHz, MCLK pin at 256 * fs = 4.096 MHz ----
    // Register values are the exact reference sequence (ESPHome es8311 /
    // the working Arduino sketch on this board). bclk_div=4 means the
    // codec expects BCLK = MCLK/4 = 1.024 MHz = 64 * fs (32-bit slots).
    ES8311_WRITE(ES8311_CLK_MANAGER1, 0x3F);  // MCLK pin source, all clocks enabled
    ES8311_WRITE(ES8311_CLK_MANAGER2, 0x08);  // pre_div=1, pre_mult=1
    ES8311_WRITE(ES8311_CLK_MANAGER3, 0x10);  // fs_mode=0, adc_osr=16
    ES8311_WRITE(ES8311_CLK_MANAGER4, 0x20);  // dac_osr=32
    ES8311_WRITE(ES8311_CLK_MANAGER5, 0x00);  // adc_div=1, dac_div=1
    ES8311_WRITE(ES8311_CLK_MANAGER6, 0x03);  // bclk_div=4 (MCLK/BCLK), sclk not inverted
    ES8311_WRITE(ES8311_CLK_MANAGER7, 0x00);  // lrck_h
    ES8311_WRITE(ES8311_CLK_MANAGER8, 0xFF);  // lrck_l

    // ---- Serial port: I2S (Philips) normal format, 16-bit data ----
    ES8311_WRITE(ES8311_SDPIN_REG, 0x0C);
    ES8311_WRITE(ES8311_SDPOUT_REG, 0x0C);

    // ---- Analog mic input: 0 dB PGA gain, ADC volume 0xC8 ----
    ES8311_WRITE(ES8311_SYSTEM_REG14, 0x1A);  // analog MIC input selected
    ES8311_WRITE(ES8311_ADC_REG16, 0x00);     // ADC gain scale (software gain used instead)
    ES8311_WRITE(ES8311_ADC_REG17, 0xC8);     // ADC volume

    // ---- DAC: volume 0xBF (0 dB), unmuted, powered up ----
    ES8311_WRITE(ES8311_DAC_REG32, 0xBF);
    ES8311_WRITE(ES8311_SYSTEM_REG0D, 0x01);  // power up analog circuitry
    ES8311_WRITE(ES8311_SYSTEM_REG0E, 0x02);  // enable analog PGA + ADC modulator
    ES8311_WRITE(ES8311_SYSTEM_REG12, 0x00);  // power up DAC
    ES8311_WRITE(ES8311_SYSTEM_REG13, 0x10);  // enable output to HP drive
    ES8311_WRITE(ES8311_ADC_REG1C, 0x6A);     // ADC EQ bypass, DC offset cancel
    ES8311_WRITE(ES8311_DAC_REG37, 0x08);     // DAC EQ bypass
    ES8311_WRITE(ES8311_DAC_REG31, 0x00);     // unmute DAC

    // ---- Repeat ADC enable writes exactly as the reference does ----
    ES8311_WRITE(ES8311_SYSTEM_REG14, 0x1A);
    ES8311_WRITE(ES8311_ADC_REG16, 0x00);
    ES8311_WRITE(ES8311_ADC_REG17, 0xC8);
    ES8311_WRITE(ES8311_SYSTEM_REG0D, 0x01);
    ES8311_WRITE(ES8311_SYSTEM_REG0E, 0x02);

    // ---- Power-on command - must be the LAST write to REG00 ----
    ES8311_WRITE(ES8311_RESET_REG, 0x80);
    vTaskDelay(pdMS_TO_TICKS(5));

    // Log the resulting register state so a misconfig is visible in the log.
    ESP_LOGI(TAG, "ES8311 initialized (16 kHz, I2S slave, 16-bit in 32-bit slots)");
    ESP_LOGI(TAG, "regs: RST=0x%02x CLK1=0x%02x CLK2=0x%02x CLK3=0x%02x CLK4=0x%02x CLK6=0x%02x SDPIN=0x%02x SDPOUT=0x%02x DACVOL=0x%02x ADCVOL=0x%02x",
             es8311_read_reg(ES8311_RESET_REG),
             es8311_read_reg(ES8311_CLK_MANAGER1),
             es8311_read_reg(ES8311_CLK_MANAGER2),
             es8311_read_reg(ES8311_CLK_MANAGER3),
             es8311_read_reg(ES8311_CLK_MANAGER4),
             es8311_read_reg(ES8311_CLK_MANAGER6),
             es8311_read_reg(ES8311_SDPIN_REG),
             es8311_read_reg(ES8311_SDPOUT_REG),
             es8311_read_reg(ES8311_DAC_REG32),
             es8311_read_reg(ES8311_ADC_REG17));
    return ESP_OK;
}

esp_err_t es8311_start(void)
{
    if (s_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Re-assert the exact enable state from the reference sequence (safe to
    // call again after init; does not touch the clock configuration).
    ES8311_WRITE(ES8311_RESET_REG, 0x80);     // ensure powered on
    ES8311_WRITE(ES8311_SDPIN_REG, 0x0C);     // 16-bit, serial port running
    ES8311_WRITE(ES8311_SDPOUT_REG, 0x0C);
    ES8311_WRITE(ES8311_DAC_REG32, 0xBF);     // DAC volume
    ES8311_WRITE(ES8311_ADC_REG17, 0xC8);     // ADC volume
    ES8311_WRITE(ES8311_SYSTEM_REG0E, 0x02);  // PGA + ADC modulator on
    ES8311_WRITE(ES8311_SYSTEM_REG12, 0x00);  // DAC on
    ES8311_WRITE(ES8311_SYSTEM_REG14, 0x1A);  // analog mic
    ES8311_WRITE(ES8311_SYSTEM_REG0D, 0x01);  // analog power on
    ES8311_WRITE(ES8311_DAC_REG37, 0x08);     // DAC EQ bypass
    ES8311_WRITE(ES8311_DAC_REG31, 0x00);     // unmute
    return ESP_OK;
}

void es8311_set_volume(int volume)
{
    if (s_dev_handle == NULL) {
        return;
    }
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    // ESP-ADF maps 0..100 -> 0..255 (DAC_REG32 is a linear volume register).
    es8311_write_reg(ES8311_DAC_REG32, (uint8_t)(volume * 255 / 100));
}

void es8311_set_mic_gain(int gain_db)
{
    if (s_dev_handle == NULL) {
        return;
    }
    // Gain steps are 3 dB each in bits[7:5] of ADC_REG16 (0..24 dB).
    if (gain_db < 0) {
        gain_db = 0;
    } else if (gain_db > 24) {
        gain_db = 24;
    }
    uint8_t step = (uint8_t)(gain_db / 3);
    es8311_write_reg(ES8311_ADC_REG16, (uint8_t)(step << 5));
}

#endif  // AIPI_LITE_BOARD
