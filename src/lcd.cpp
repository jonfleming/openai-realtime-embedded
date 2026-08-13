// lcd.c
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_common.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
#include "esp_lcd_st7735.h"
#endif
#if defined(WAVESHARE_BSP_BOARD) && WAVESHARE_BSP_BOARD
// Exactly one Waveshare BSP is in the build per board profile (see the
// FREEBUFF_BOARD rules in src/idf_component.yml), so these shared bsp/
// headers resolve to the active board's BSP (1.8 or 2.06).
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#endif
#if defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
// 1.8 (V1.0) only: the FT3168 touch + AMOLED panel are power-gated by the
// TCA9554 IO expander; the 2.06 board wires touch reset to a GPIO directly.
#include "esp_io_expander.h"
#include "esp_io_expander_tca9554.h"
#endif
#include <esp_log.h>

#include "lcd.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

#define TAG "MediaKit"

/**********************
 *     Define Pins and Parameters
 **********************/
//#define FREENOVE_DEDIA_KIT_1_14_INCH

// Board-specific pin definitions (AIPI-Lite vs Freenove)
#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
   #define LCD_SPI_HOST SPI3_HOST
   #define DISPLAY_MOSI_PIN GPIO_NUM_17
   #define DISPLAY_CLK_PIN GPIO_NUM_16
   #define DISPLAY_DC_PIN GPIO_NUM_7
   #define DISPLAY_RST_PIN GPIO_NUM_18
   #define DISPLAY_CS_PIN GPIO_NUM_15
   #define LCD_RST_PIN GPIO_NUM_18
   #define DISPLAY_BACKLIGHT_PIN GPIO_NUM_3
   #define DISPLAY_PCLK_HZ (27 * 1000 * 1000)
   #define DISPLAY_WIDTH 128
   #define DISPLAY_HEIGHT 128
   #define DISPLAY_MIRROR_X false
   #define DISPLAY_MIRROR_Y false
   #define DISPLAY_SWAP_XY false
   #define DISPLAY_INVERT_COLOR true
   #define DISPLAY_RGB_ORDER LCD_RGB_ELEMENT_ORDER_BGR
   #define DISPLAY_OFFSET_X 0
   #define DISPLAY_OFFSET_Y 0
#elif defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
   // Waveshare ESP32-S3 Touch AMOLED 2.06 (410x502 QSPI AMOLED, CO5300
   // controller, watch form factor). The managed BSP
   // (waveshare/esp32_s3_touch_amoled_2_06) owns the panel, touch and audio
   // (see init_lvgl); only the resolution is needed by the shared UI code.
   #define DISPLAY_WIDTH  410
   #define DISPLAY_HEIGHT 502
#elif defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
   // Waveshare ESP32-S3 Touch AMOLED 1.8 (368x448 QSPI AMOLED).
   // The managed BSP (waveshare/esp32_s3_touch_amoled_1_8) owns the panel,
   // touch and the LVGL task (see init_lvgl); only the resolution is needed
   // by the shared UI code below.
   #define DISPLAY_WIDTH  368
   #define DISPLAY_HEIGHT 448
#else
   // Freenove Media Kit (3.5 inch)
   #define FREENOVE_DEDIA_KIT_3_5_INCH
   #define LCD_SPI_HOST SPI3_HOST
   #define DISPLAY_MOSI_PIN GPIO_NUM_21
   #define DISPLAY_CLK_PIN GPIO_NUM_47
   #define DISPLAY_DC_PIN GPIO_NUM_45
   #define DISPLAY_RST_PIN GPIO_NUM_20
   #define DISPLAY_CS_PIN GPIO_NUM_NC
   #define LCD_RST_PIN GPIO_NUM_20
   #define DISPLAY_BACKLIGHT_PIN GPIO_NUM_2
   #define DISPLAY_PCLK_HZ (80 * 1000 * 1000)
   #define DISPLAY_WIDTH 480
   #define DISPLAY_HEIGHT 320
   #define DISPLAY_MIRROR_X false
   #define DISPLAY_MIRROR_Y false
   #define DISPLAY_SWAP_XY true
   #define DISPLAY_INVERT_COLOR true
   #define DISPLAY_RGB_ORDER LCD_RGB_ELEMENT_ORDER_RGB
   #define DISPLAY_OFFSET_X 0
   #define DISPLAY_OFFSET_Y 0
#endif

#define BACKLIGHT_PWM_TIMER LEDC_TIMER_0
#define BACKLIGHT_PWM_CHANNEL LEDC_CHANNEL_0
#define BACKLIGHT_PWM_FREQ 1000                // PWM frequency: 5kHz
#define BACKLIGHT_RESOLUTION LEDC_TIMER_13_BIT // 13-bit resolution (0 ~ 8191)

esp_lcd_panel_handle_t panel = NULL;
lv_disp_t * disp_handle;

// Define a structure
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *container;
} lvgl_screen_t;

lvgl_screen_t lvgl_screen;

// LVGL mutex access: the Waveshare BSP owns the lock on those boards
// (bsp_display_lock() wraps lvgl_port_lock() internally); the SPI-panel
// boards use esp_lvgl_port directly.
static void lcd_disp_lock(void)
{
#if defined(WAVESHARE_BSP_BOARD) && WAVESHARE_BSP_BOARD
    bsp_display_lock(0);
#else
    lvgl_port_lock(0);
#endif
}

static void lcd_disp_unlock(void)
{
#if defined(WAVESHARE_BSP_BOARD) && WAVESHARE_BSP_BOARD
    bsp_display_unlock();
#else
    lvgl_port_unlock();
#endif
}

#if defined(FREENOVE_DEDIA_KIT_3_5_INCH) && !(defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD)
typedef struct {
    int cmd;             
    const void *data;     
    size_t data_bytes;   
    unsigned int delay_ms;  
} st7796_lcd_init_cmd_t;

typedef struct {
    const st7796_lcd_init_cmd_t *init_cmds;     
    uint16_t init_cmds_size;                   
} st7796_vendor_config_t;

st7796_lcd_init_cmd_t st7796_lcd_init_cmds[] = {
    {0x11, (uint8_t []){ 0x00 }, 0, 120},
    {0x3A, (uint8_t []){ 0x05 }, 1, 0},
    {0xF0, (uint8_t []){ 0xC3 }, 1, 0},
    {0xF0, (uint8_t []){ 0x96 }, 1, 0},
    {0xB4, (uint8_t []){ 0x01 }, 1, 0},
    {0xB7, (uint8_t []){ 0xC6 }, 1, 0},
    {0xC0, (uint8_t []){ 0x80, 0x45 }, 2, 0},
    {0xC1, (uint8_t []){ 0x13 }, 1, 0},
    {0xC2, (uint8_t []){ 0xA7 }, 1, 0},
    {0xC5, (uint8_t []){ 0x0A }, 1, 0},
    {0xE8, (uint8_t []){ 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 8, 0},
    {0xE0, (uint8_t []){ 0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33, 0x47, 0x17, 0x13, 0x13, 0x2B, 0x31}, 14, 0},
    {0xE1, (uint8_t []){ 0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F, 0x33, 0x47, 0x38, 0x15, 0x16, 0x2C, 0x32},14, 0},
    {0xF0, (uint8_t []){ 0x3C }, 1, 0},
    {0xF0, (uint8_t []){ 0x69 }, 1, 120},
    {0x21, (uint8_t []){ 0x00 }, 0, 0},
    {0x29, (uint8_t []){ 0x00 }, 0, 0},
};
#endif

#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
static esp_err_t create_aipi_panel(esp_lcd_panel_io_handle_t io_handle,
                                   esp_lcd_panel_dev_config_t *panel_config)
{
    ESP_LOGI(TAG, "LVGL init: create AIPI panel");

    st7735_vendor_config_t aipi_vendor_config = {
        .init_cmds = NULL,
        .init_cmds_size = 0,
    };

    panel_config->vendor_config = &aipi_vendor_config;

    esp_err_t err = esp_lcd_new_panel_st7735(io_handle, panel_config, &panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7735 failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "LVGL init: create AIPI panel OK");
    return ESP_OK;
}
#elif defined(WAVESHARE_BSP_BOARD) && WAVESHARE_BSP_BOARD
// Panel creation is owned by the managed BSP (see init_lvgl below).
#else
static esp_err_t create_freenove_panel(esp_lcd_panel_io_handle_t io_handle,
                                       esp_lcd_panel_dev_config_t *panel_config)
{
    st7796_vendor_config_t st7796_vendor_config = {
        .init_cmds = st7796_lcd_init_cmds,
        .init_cmds_size = sizeof(st7796_lcd_init_cmds) / sizeof(st7796_lcd_init_cmd_t),
    };

    panel_config->vendor_config = &st7796_vendor_config;

    ESP_LOGI(TAG, "LVGL init: create Freenove panel");
    esp_err_t err = esp_lcd_new_panel_st7789(io_handle, panel_config, &panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "LVGL init: create Freenove panel OK");
    return ESP_OK;
}
#endif

#if !(defined(WAVESHARE_BSP_BOARD) && WAVESHARE_BSP_BOARD)
/**********************
 * @brief Initialize backlight PWM control
 **********************/
void backlight_init(void)
{
    ledc_timer_config_t timer={
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BACKLIGHT_RESOLUTION,
        .timer_num = BACKLIGHT_PWM_TIMER,
        .freq_hz = BACKLIGHT_PWM_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel={
        .gpio_num = DISPLAY_BACKLIGHT_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BACKLIGHT_PWM_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BACKLIGHT_PWM_TIMER,
        .duty = 0,
        .hpoint = 0,
    #if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
        // GPIO3 is a strapping pin on AIPI-Lite; don't require sleep keep-alive there.
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
    #else
        .sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE,
    #endif
        .flags = {.output_invert = false}
    };
    ledc_channel_config(&channel);
}

/**********************
 * @brief Set backlight brightness
 * @param brightness - percentage (0 ~ 100)
 **********************/
void set_backlight_brightness(int brightness)
{
    int duty_max = (1 << BACKLIGHT_RESOLUTION) - 1;
    int duty = (brightness * duty_max) / 100;

    if (duty > duty_max)
        duty = duty_max;
    else if (duty < 0)
        duty = 0;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_PWM_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_PWM_CHANNEL);
}

void reset_lcd(void) {
    // Pulse the panel reset line, then leave it driven HIGH.
    // (Driving high keeps the panel out of reset; the previous open-drain
    //  config left RST floating, which can hold ST7735 panels in reset.)
    gpio_config_t rst_conf = {};
    rst_conf.intr_type = GPIO_INTR_DISABLE;
    rst_conf.mode = GPIO_MODE_OUTPUT;
    rst_conf.pin_bit_mask = (1ULL << LCD_RST_PIN);
    rst_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    rst_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&rst_conf);
    gpio_set_level(LCD_RST_PIN, 0); // Set pin to low level
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}
#endif

#if defined(WAVESHARE_BSP_BOARD) && WAVESHARE_BSP_BOARD
#if LVGL_VERSION_MAJOR >= 9
// The Waveshare QSPI AMOLED panels (SH8601/CO5300) write 16-bit-aligned
// regions; round invalidated areas to even coordinates (same callback as the
// Waveshare BSP).
static void lcd_rounder_event_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;
    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;
    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}
#endif
#endif

esp_err_t init_lvgl(void)
{
#if defined(WAVESHARE_BSP_BOARD) && WAVESHARE_BSP_BOARD
    // Waveshare ESP32-S3 Touch AMOLED boards (1.8 V1.0: SH8601 + FT3168;
    // 2.06: CO5300 + FT3168). Brightness is a panel command here (no LEDC
    // backlight pin).
    //
    // We replicate bsp_display_start() with the BSP's public APIs instead of
    // calling it directly: this project compiles with assertions disabled
    // (NDEBUG), which turns ESP_ERROR_CHECK into a no-op, and the BSP treats
    // a failed touch probe as fatal (NULL touch handle -> panic / reboot
    // loop). Here the touch probe is retried and the app boots without touch
    // input if it still fails, so a marginal probe can never crash the boot.
#if defined(WAVESHARE_AMOLED_2_06_BOARD) && WAVESHARE_AMOLED_2_06_BOARD
    ESP_LOGI(TAG, "LVGL init: Waveshare AMOLED 2.06 (BSP display sequence)");
#else
    ESP_LOGI(TAG, "LVGL init: Waveshare AMOLED 1.8 (BSP display sequence)");
#endif

    // 1. LVGL port: mutex + timer + render task
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    esp_err_t err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();

#if defined(WAVESHARE_AMOLED_1_8_BOARD) && WAVESHARE_AMOLED_1_8_BOARD
    // 2. (1.8 V1.0 only) Power up the FT3168 touch controller and AMOLED
    //    panel: on this board revision their reset/power lines are gated by
    //    the TCA9554 IO expander (I2C 0x20). Neither the V1.0 nor the V2.0
    //    BSP configures the expander, so the touch never ACKs on I2C and the
    //    panel may stay off. This pulse sequence mirrors Waveshare's Arduino
    //    reference (02_Drawing_board demo): drive pins 0-2 low, then high.
    //    The 2.06 board does not use the expander (touch reset = GPIO9).
    esp_io_expander_handle_t expander = NULL;
    esp_err_t e = esp_io_expander_new_i2c_tca9554(i2c_bus, BSP_IO_EXPANDER_I2C_ADDRESS, &expander);
    if (e == ESP_OK && expander != NULL) {
        const uint32_t pins = IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2;
        esp_io_expander_set_dir(expander, pins, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(expander, pins, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_io_expander_set_level(expander, pins, 1);
        ESP_LOGI(TAG, "TCA9554 power-up pulse (pins 0-2) done");
    } else {
        ESP_LOGW(TAG, "TCA9554 expander init failed: %s", esp_err_to_name(e));
    }
#endif

    // 3. Panel + QSPI IO (same call the BSP makes internally). max_transfer_sz
    //    must be > 0: the 2.06 BSP asserts it in bsp_display_new().
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_io_handle_t io_handle = NULL;
    bsp_display_config_t disp_config = {
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * 2,
    };
    err = bsp_display_new(&disp_config, &panel_handle, &io_handle);
    if (err != ESP_OK || panel_handle == NULL || io_handle == NULL) {
        ESP_LOGE(TAG, "bsp_display_new failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // 4. Register the LVGL display. The buffer config follows this project's
    // proven SPI-panel convention (see the AIPI-Lite/Freenove path): DMA
    // capable draw buffer + no software rotation + 20-row buffer. The BSP's
    // own defaults (plain-RAM 100-row buffer, sw_rotate) make the SPI driver
    // allocate a ~74KB DMA bounce buffer on every flush, which fails once
    // WebRTC/TLS have consumed the internal heap ("Failed to allocate priv TX
    // buffer" -> blank screen).
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = DISPLAY_WIDTH * 20,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .swap_bytes = true,
        },
    };
    // NOTE: use lvgl_port_add_disp() (not lvgl_port_add_disp_rgb()) here: this is
    // a QSPI/SPI panel, and the *_rgb variant sets the display type to RGB. In
    // that mode esp_lvgl_port's flush callback calls lv_disp_flush_ready()
    // immediately after queueing the (async) SPI transaction, so LVGL reuses the
    // single draw buffer while the SPI DMA is still reading it. The DMA then
    // captures a mix of two adjacent 20-row chunks, leaving a corrupted
    // horizontal strip at each buffer boundary -- visible as a white line
    // through text/buttons (white on white is invisible on the container). The
    // plain variant registers the SPI on_color_trans_done callback and only
    // signals flush ready once the transfer has finished, matching the proven
    // AIPI-Lite/Freenove path below.
    disp_handle = lvgl_port_add_disp(&disp_cfg);
    if (disp_handle == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }
#if LVGL_VERSION_MAJOR >= 9
    lv_display_add_event_cb(disp_handle, lcd_rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
#endif

    // 5. Touch: retry the BSP probe, then continue without touch if it fails.
    //    Diagnostic: scan the bus first so a missing touch controller is
    //    distinguishable from a dead I2C bus (expect 0x38 = FT3168 on both
    //    boards; on the 1.8 it only answers after the TCA9554 pulse above).
    if (i2c_bus != NULL) {
        uint8_t found[16] = {0};
        int n = 0;
        for (uint8_t addr = 0x08; addr < 0x78 && n < 16; addr++) {
            if (i2c_master_probe(i2c_bus, addr, 20) == ESP_OK) {
                found[n++] = addr;
            }
        }
        if (n > 0) {
            char buf[80] = "";
            int off = 0;
            for (int i = 0; i < n; i++) {
                off += snprintf(buf + off, sizeof(buf) - off, "%s0x%02X", i ? "," : "", found[i]);
            }
            ESP_LOGI(TAG, "I2C scan (0x08-0x77): %s", buf);
        } else {
            ESP_LOGW(TAG, "I2C scan (0x08-0x77): no devices responded");
        }
    }

    esp_lcd_touch_handle_t tp = NULL;
    for (int attempt = 1; attempt <= 5; attempt++) {
        tp = NULL;
        err = bsp_touch_new(NULL, &tp);
        if (err == ESP_OK && tp != NULL) {
            break;
        }
        if (tp != NULL) {
            esp_lcd_touch_del(tp);
            tp = NULL;
        }
        ESP_LOGW(TAG, "Touch probe attempt %d/5 failed: %s", attempt, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (tp != NULL) {
        lvgl_port_touch_cfg_t touch_cfg = {
            .disp = disp_handle,
            .handle = tp,
        };
        if (lvgl_port_add_touch(&touch_cfg) != NULL) {
            ESP_LOGI(TAG, "Touch ready");
        } else {
            ESP_LOGW(TAG, "lvgl_port_add_touch failed; running without touch input");
        }
    } else {
        ESP_LOGW(TAG, "Touch init failed after retries; running without touch input");
    }

    // 6. Brightness (panel command on this board)
    err = bsp_display_brightness_init();
    if (err == ESP_OK) {
        err = bsp_display_brightness_set(85);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_display_brightness init/set failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "LVGL init: display %dx%d ready", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    return ESP_OK;
#else
    esp_err_t err;

    ESP_LOGI(TAG, "LVGL init: backlight");
    backlight_init();
    set_backlight_brightness(100);

    ESP_LOGI(TAG, "LVGL init: SPI bus");
    spi_bus_config_t spi_bus_io = {};
    spi_bus_io.mosi_io_num = DISPLAY_MOSI_PIN;
    spi_bus_io.miso_io_num = GPIO_NUM_NC;
    spi_bus_io.sclk_io_num = DISPLAY_CLK_PIN;
    spi_bus_io.quadwp_io_num = GPIO_NUM_NC;
    spi_bus_io.quadhd_io_num = GPIO_NUM_NC;
    spi_bus_io.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    err = spi_bus_initialize(LCD_SPI_HOST, &spi_bus_io, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "LVGL init: panel IO");
    esp_lcd_panel_io_handle_t io_handle  = NULL;
    esp_lcd_panel_io_spi_config_t io_config={};
    io_config.cs_gpio_num = DISPLAY_CS_PIN;
    io_config.dc_gpio_num = DISPLAY_DC_PIN;
    io_config.spi_mode = 0;
    io_config.pclk_hz = DISPLAY_PCLK_HZ;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    err = esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "LVGL init: panel config");
    esp_lcd_panel_dev_config_t panel_config={};
    panel_config.reset_gpio_num = LCD_RST_PIN;
    panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
    panel_config.bits_per_pixel = 16;
#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
    err = create_aipi_panel(io_handle, &panel_config);
#else
    err = create_freenove_panel(io_handle, &panel_config);
#endif
    if (err != ESP_OK) {
        return err;
    }
    err = esp_lcd_panel_reset(panel);               // Reset LCD screen
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_reset failed: %s", esp_err_to_name(err));
        return err;
    }

    reset_lcd();
    err = esp_lcd_panel_init(panel);                // Initialize configuration registers
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Panel init OK");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));  // Color inversion
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));       // Display rotation 
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y)); // Mirror

    uint16_t *buffer = (uint16_t *)malloc(DISPLAY_WIDTH * sizeof(uint16_t));
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for display buffer");
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < DISPLAY_WIDTH; i++) {
        buffer[i] = 0xFFFF; // Fill with white color (RGB565 format)
    }
    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, DISPLAY_WIDTH, y + 1, buffer);
    }
    err = esp_lcd_panel_disp_on_off(panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_disp_on_off failed: %s", esp_err_to_name(err));
        free(buffer);
        return err;
    }
    free(buffer);

    ESP_LOGI(TAG, "LVGL init: lv_init");
    lv_init();

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_LOGI(TAG, "LVGL init: lvgl_port_init");
    err = lvgl_port_init(&lvgl_cfg);
    ESP_LOGI(TAG, "lvgl_port_init: %s", err == ESP_OK ? "OK" : "Failed");
    if (err != ESP_OK) {
        return err;
    }

    lvgl_port_display_cfg_t disp_cfg;
    disp_cfg.io_handle = io_handle;
    disp_cfg.panel_handle = panel;
    disp_cfg.control_handle = NULL;
    disp_cfg.buffer_size = DISPLAY_WIDTH * 20;
    disp_cfg.double_buffer = 0;
    disp_cfg.trans_size = 0;
    disp_cfg.hres = DISPLAY_WIDTH;
    disp_cfg.vres = DISPLAY_HEIGHT;
    disp_cfg.monochrome = false;
    disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;

    disp_cfg.rotation.swap_xy = DISPLAY_SWAP_XY;
    disp_cfg.rotation.mirror_x = DISPLAY_MIRROR_X;
    disp_cfg.rotation.mirror_y = DISPLAY_MIRROR_Y;

    disp_cfg.flags.buff_dma = 1;
    disp_cfg.flags.buff_spiram = 0;
    disp_cfg.flags.sw_rotate = 0;
    disp_cfg.flags.swap_bytes = 1;
    disp_cfg.flags.full_refresh = 0;
    disp_cfg.flags.direct_mode = 0;

    disp_handle = lvgl_port_add_disp(&disp_cfg);
    if (disp_handle == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }
    lv_display_set_offset(disp_handle, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y);

    return ESP_OK;
#endif
}

void lvgl_ui(void)
{
    lcd_disp_lock();  // Lock LVGL
    // Create main screen object
    lvgl_screen.screen = lv_obj_create(lv_scr_act());
    lv_obj_set_size(lvgl_screen.screen, DISPLAY_WIDTH, DISPLAY_HEIGHT);

    // Create container
    lvgl_screen.container = lv_obj_create(lvgl_screen.screen);
    lv_obj_set_size(lvgl_screen.container, DISPLAY_WIDTH-10, DISPLAY_HEIGHT-10);
    lv_obj_center(lvgl_screen.container);                                                   // Center
    // Hide the right scrollbar of the container
    lv_obj_set_style_pad_all(lvgl_screen.container, 0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(lvgl_screen.container, lv_color_hex(0xFFFFFF), LV_PART_MAIN); // White background
    lv_obj_set_flex_flow(lvgl_screen.container, LV_FLEX_FLOW_COLUMN);                       // Vertical layout
    lv_obj_set_scroll_dir(lvgl_screen.container, LV_DIR_VER);                               // Vertical scroll
    lv_obj_set_scrollbar_mode(lvgl_screen.container, LV_SCROLLBAR_MODE_AUTO);               // Auto scroll
    lcd_disp_unlock(); // Unlock LVGL
}

// Each time this function is called, create a label in the container with text content.
// The label can wrap lines if needed. The background color of the label is green.
void lvgl_ui_label_set_text(const char *text)
{
    lcd_disp_lock();
    lv_obj_t *btn = lv_btn_create(lvgl_screen.container);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x00FF00), LV_STATE_DEFAULT);
    lv_obj_set_width(btn, lv_pct(98));                                           // Full width
    lv_obj_set_height(btn, LV_SIZE_CONTENT);                                     // Height adapts to content
    lv_obj_set_style_radius(btn, 5, LV_STATE_DEFAULT);                           // Rounded corners

    lv_obj_t *label = lv_label_create(btn);                                      // Create label
    lv_label_set_text(label, text);                                              // Set label text
    lv_obj_set_style_text_color(label, lv_color_hex(0x000000), LV_STATE_DEFAULT); // Set text color to black
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);     // Set font size to 20
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);        // Text align left
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);                           // Auto-wrap
    lv_obj_set_width(label, lv_pct(100));                                        // Full width
    lv_obj_set_height(label, LV_SIZE_CONTENT);                                   // Height adapts to content

    lv_obj_set_style_pad_column(lvgl_screen.container, 10, LV_PART_MAIN);

    const uint8_t max_labels = 5;
    if (lv_obj_get_child_cnt(lvgl_screen.container) > max_labels) {
        lv_obj_t *first_child = lv_obj_get_child(lvgl_screen.container, 0);
        if (first_child) {
            lv_obj_del(first_child);
        }
    }
    lv_obj_update_layout(lvgl_screen.container);

    int index = lv_obj_get_child_cnt(lvgl_screen.container);
    if(index > 0)
    {
        lv_obj_t *last_child = lv_obj_get_child(lvgl_screen.container, index - 1);
        lv_coord_t visible_height = lv_obj_get_content_height(lvgl_screen.container); 
        lv_coord_t y_aligned = lv_obj_get_y(last_child) - (visible_height / 2) + (lv_obj_get_height(last_child) / 2);
        lv_obj_scroll_to_y(lvgl_screen.container, y_aligned, LV_ANIM_OFF);
    }
    lcd_disp_unlock();
}