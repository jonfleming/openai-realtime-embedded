// Battery monitoring for the supported boards.
//
// Board backends:
//   - Freenove Media Kit: GPIO20 is a *shared* pin — the LCD reset line AND
//     the battery sense tap of a voltage divider (battery_mV = adc_mV * 2.5 -
//     3300 per the official FNK0102 docs/schematic). init_lvgl() drives the
//     pin for the reset pulse; once the panel is initialized this module
//     reconfigures the pin as an ADC input (ADC2_CH9) so the divider can be
//     sampled, mirroring the board's official firmware.
//   - AIPI-Lite: battery divider on GPIO2 (ADC1_CH1). The raw ADC -> percent
//     lookup table is lifted from the xiaozhi-esp32 AIPI-Lite board support
//     (the stock firmware for this board).
//   - Waveshare 1.8 / 2.06: AXP2101 PMU on the BSP-owned I2C bus (addr 0x34).
//     Percent comes from the PMU's own E-Gauge fuel gauge (reg 0xA4), with a
//     voltage-based fallback from the battery ADC (reg 0x34/0x35).

#include "battery.h"

#include <esp_log.h>

#if defined(WAVESHARE_BSP_BOARD) && WAVESHARE_BSP_BOARD
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"
#else
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#endif

#define TAG "Battery"

// ---------------------------------------------------------------------------
// Waveshare boards: AXP2101 PMU on the BSP I2C bus
// ---------------------------------------------------------------------------
#if defined(WAVESHARE_BSP_BOARD) && WAVESHARE_BSP_BOARD

#define AXP2101_I2C_ADDR     0x34
#define AXP2101_STATUS1      0x00 // bit5: VBUS good, bit3: battery present
#define AXP2101_FUEL_CTRL    0x18 // bit3: E-Gauge (fuel gauge) enable
#define AXP2101_ADC_CTRL     0x30 // bit0: battery voltage ADC channel enable
#define AXP2101_BAT_ADC_H    0x34 // battery voltage, 13-bit, 1 mV/LSB
#define AXP2101_BAT_ADC_L    0x35
#define AXP2101_BAT_DET_CTRL 0x68 // bit0: battery detection enable
#define AXP2101_BAT_PERCENT  0xA4 // battery percentage 0..100 (fuel gauge)

static i2c_master_bus_handle_t s_i2c = NULL;
static i2c_master_dev_handle_t s_axp = NULL;
static bool s_axp_ok = false;

static esp_err_t axp_read_reg(uint8_t reg, uint8_t *val)
{
    if (s_axp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_axp, &reg, 1, val, 1, 100);
}

static esp_err_t axp_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_axp, buf, sizeof(buf), 100);
}

// Set or clear a single bit without touching the rest of the register.
static esp_err_t axp_rmw_bit(uint8_t reg, uint8_t bit, bool set)
{
    uint8_t val = 0;
    esp_err_t err = axp_read_reg(reg, &val);
    if (err != ESP_OK) {
        return err;
    }
    if (set) {
        val |= (uint8_t)(1U << bit);
    } else {
        val &= (uint8_t)~(1U << bit);
    }
    return axp_write_reg(reg, val);
}

// LiPo voltage -> percent (piecewise linear), used only as a fallback when
// the AXP2101's own fuel gauge has not produced a valid percentage yet.
static int axp_voltage_to_percent(int mv)
{
    static const uint16_t seg_v[11] = {4200, 4100, 4000, 3900, 3800, 3700,
                                       3600, 3500, 3400, 3300, 3200};
    if (mv >= seg_v[0]) {
        return 100;
    }
    if (mv <= seg_v[10]) {
        return 0;
    }
    for (int i = 0; i < 10; i++) {
        if (mv > seg_v[i + 1]) { // between seg_v[i] and seg_v[i+1]
            return ((mv - seg_v[i + 1]) * 10) / (seg_v[i] - seg_v[i + 1]) +
                   (10 - i - 1) * 10;
        }
    }
    return 0;
}

// Read the battery voltage ADC (reg 0x34/0x35, 13-bit, 1 mV/LSB). Returns the
// voltage in mV or -1 on an I2C error.
static int axp_read_bat_mv(void)
{
    uint8_t hi = 0, lo = 0;
    if (axp_read_reg(AXP2101_BAT_ADC_H, &hi) != ESP_OK ||
        axp_read_reg(AXP2101_BAT_ADC_L, &lo) != ESP_OK) {
        return -1;
    }
    return ((hi & 0x1F) << 8) | lo;
}

esp_err_t oai_battery_init(void)
{
    s_i2c = bsp_i2c_get_handle();
    if (s_i2c == NULL) {
        ESP_LOGW(TAG, "No BSP I2C bus handle; battery monitoring disabled");
        return ESP_ERR_INVALID_STATE;
    }

    // Register the AXP2101 as a device on the BSP-owned bus. The 1.8 board's
    // bus runs at 100 kHz (CONFIG_BSP_I2C_FAST_MODE=n); the 2.06 board runs
    // 400 kHz. Either way a conservative 100 kHz device speed is valid.
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(s_i2c, &dev_cfg, &s_axp);
    if (err != ESP_OK || s_axp == NULL) {
        ESP_LOGW(TAG, "AXP2101 I2C device add failed: %s; battery monitoring disabled",
                 esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_FAIL;
    }

    err = axp_rmw_bit(AXP2101_FUEL_CTRL, 3, true);    // E-Gauge on
    err |= axp_rmw_bit(AXP2101_ADC_CTRL, 0, true);              // BAT voltage ADC on
    err |= axp_rmw_bit(AXP2101_BAT_DET_CTRL, 0, true);          // battery detection on
    s_axp_ok = (err == ESP_OK);

    if (s_axp_ok) {
        // One-shot diagnostic: surface the raw PMU state at boot so a "battery
        // not showing" report can be triaged from the serial log without a
        // rebuild (status1, fuel-gauge %, battery mV).
        uint8_t status1 = 0, pct = 0;
        int mv = axp_read_bat_mv();
        axp_read_reg(AXP2101_STATUS1, &status1);
        axp_read_reg(AXP2101_BAT_PERCENT, &pct);
        ESP_LOGI(TAG, "AXP2101 battery monitor ready (I2C 0x%02X); status1=0x%02X "
                 "batt_present=%d fuel=%d%% batt_mv=%d",
                 AXP2101_I2C_ADDR, status1, (status1 >> 3) & 1, pct, mv);
    } else {
        ESP_LOGW(TAG, "AXP2101 config failed (0x%X); battery monitoring disabled", err);
    }
    return s_axp_ok ? ESP_OK : ESP_FAIL;
}

int oai_battery_get_percent(void)
{
    if (!s_axp_ok) {
        return -1;
    }

    // Preferred: the AXP2101 E-Gauge percentage (reg 0xA4). On the 2.06 this
    // is what the stock/xiaozhi firmware reads directly; it can briefly read
    // 0 on a fresh battery before the gauge has learned the curve.
    uint8_t pct = 0;
    if (axp_read_reg(AXP2101_BAT_PERCENT, &pct) == ESP_OK && pct > 0 && pct <= 100) {
        return pct;
    }

    // Fallback: battery voltage ADC -> percent. This is also the "is a
    // battery actually attached" test: a real LiPo reads above ~2 V while a
    // USB-only board reads near 0 V (the AXP2101 powers off well before the
    // cell drops to 2.6 V, so <2 V reliably means no battery). We prefer this
    // over status1 bit 3, which the 2.06 can report inconsistently.
    int mv = axp_read_bat_mv();
    if (mv < 2000 || mv > 4500) {
        return -1;
    }
    return axp_voltage_to_percent(mv);
}

// ---------------------------------------------------------------------------
// Freenove Media Kit + AIPI-Lite: raw ADC on the battery divider
// ---------------------------------------------------------------------------
#else

#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
// GPIO2 (ADC1_CH1), 12 dB attenuation. Raw counts -> percent table below is
// from the xiaozhi-esp32 AIPI-Lite support (no voltage math needed).
#define BATTERY_ADC_PIN GPIO_NUM_2
#else
// Freenove Media Kit: GPIO20 (ADC2_CH9) is the shared LCD_RST/battery tap.
// The divider node is 0.4 * VBAT + 1.32 V, so battery_mV = adc_mV * 2.5 - 3300.
#define BATTERY_ADC_PIN GPIO_NUM_20
#endif

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static adc_unit_t s_unit;
static adc_channel_t s_chan;
static int s_avg_raw = 0;

static int adc_raw_to_mv_fallback(int raw)
{
    // ESP32-S3, 12 dB attenuation: full scale ~3100 mV at 4095. Only used if
    // efuse calibration is unavailable.
    return raw * 3100 / 4095;
}

static int batt_read_mv(int *raw_out)
{
    if (s_adc == NULL) {
        return -1;
    }
    int raw = 0;
    if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK) {
        return -1;
    }
    if (raw_out != NULL) {
        *raw_out = raw;
    }

    // Exponential moving average: the divider node jitters under load, which
    // otherwise shows up as a "rising" charge curve.
    if (s_avg_raw == 0) {
        s_avg_raw = raw;
    } else {
        s_avg_raw = (s_avg_raw * 3 + raw) / 4;
    }
    raw = s_avg_raw;

    int mv = 0;
    if (s_cali != NULL && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
        return mv;
    }
    return adc_raw_to_mv_fallback(raw);
}

esp_err_t oai_battery_init(void)
{
#if !(defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD)
    // Freenove: release the shared LCD_RST pin (driven HIGH at the end of the
    // reset pulse in init_lvgl()) so the battery divider can bias it. The
    // panel is already initialized; with a battery the divider holds the pin
    // at 2.6-3.0 V, which the board's official firmware also relies on.
    gpio_reset_pin(BATTERY_ADC_PIN);
#endif

    // Derive the ADC unit/channel from the GPIO number so the mapping can
    // never drift from the datasheet (GPIO1-10 -> ADC1 CH0-9, GPIO11-20 ->
    // ADC2 CH0-9 on the ESP32-S3).
    if (adc_oneshot_io_to_channel(BATTERY_ADC_PIN, &s_unit, &s_chan) != ESP_OK) {
        ESP_LOGW(TAG, "GPIO %d is not an ADC pin; battery monitoring disabled", BATTERY_ADC_PIN);
        return ESP_ERR_NOT_SUPPORTED;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = s_unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_adc, s_chan, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    // Efuse-based calibration for millivolt-accurate readings (Freenove).
    // AIPI-Lite only uses raw counts, so a missing calibration is harmless.
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = s_unit,
        .chan = s_chan,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        s_cali = NULL;
    }

    ESP_LOGI(TAG, "Battery ADC ready (GPIO %d -> unit %d, channel %d)",
             BATTERY_ADC_PIN, s_unit, s_chan);
    return ESP_OK;
}

#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
// Raw ADC -> percent, straight from the xiaozhi-esp32 AIPI-Lite support.
static int aipi_raw_to_percent(int raw)
{
    static const struct {
        uint16_t adc;
        uint8_t level;
    } levels[] = {
        {1480, 0}, {1581, 20}, {1663, 40}, {1750, 60}, {1840, 80}, {1980, 100},
    };

    if (raw < 1000) {
        return -1; // no battery connected (net floats near 0)
    }
    if (raw <= levels[0].adc) {
        return 0;
    }
    if (raw >= levels[5].adc) {
        return 100;
    }
    for (int i = 0; i < 5; i++) {
        if (raw >= levels[i].adc && raw < levels[i + 1].adc) {
            return levels[i].level +
                   (raw - levels[i].adc) * (levels[i + 1].level - levels[i].level) /
                       (levels[i + 1].adc - levels[i].adc);
        }
    }
    return 100;
}
#else
// Freenove divider: battery_mV = adc_mV * 2.5 - 3300 (official sketch).
// Out-of-range readings mean the battery is absent (USB-only power).
static int freenove_mv_to_percent(int batt_mv)
{
    const int BATTERY_MIN = 3200;
    const int BATTERY_MAX = 4200;
    if (batt_mv < BATTERY_MIN || batt_mv > BATTERY_MAX) {
        return -1;
    }
    return (batt_mv - BATTERY_MIN) * 100 / (BATTERY_MAX - BATTERY_MIN);
}
#endif

int oai_battery_get_percent(void)
{
    int raw = 0;
    int mv = batt_read_mv(&raw);
    if (mv < 0) {
        return -1;
    }
#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
    return aipi_raw_to_percent(raw);
#else
    int batt_mv = mv * 5 / 2 - 3300; // * 2.5 - 3300 (integer math)
    return freenove_mv_to_percent(batt_mv);
#endif
}

#endif // WAVESHARE_BSP_BOARD
