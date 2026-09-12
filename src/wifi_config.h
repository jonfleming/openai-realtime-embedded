#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include "esp_netif.h"
#include "esp_event.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SoftAP that stays up in AP+STA so the settings page is always reachable. */
#define EXAMPLE_ESP_WIFI_AP_SSID    "OpenAI"
#define EXAMPLE_ESP_WIFI_AP_PASSWD  ""

#define WIFI_CFG_SPEAKER_VOL_MIN    0
#define WIFI_CFG_SPEAKER_VOL_MAX    100
#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
// ES8311 init leaves DAC_REG32 at 0xBF (~75%). Volume 100 maps to 0xFF and
// is far too loud on this speaker; keep the historical codec default.
#define WIFI_CFG_DEFAULT_SPEAKER_VOL 75
#else
#define WIFI_CFG_DEFAULT_SPEAKER_VOL 100
#endif

#define WIFI_CFG_MIC_GAIN_MIN       1
#define WIFI_CFG_MIC_GAIN_MAX       16
#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
#define WIFI_CFG_DEFAULT_MIC_GAIN   12
#else
#define WIFI_CFG_DEFAULT_MIC_GAIN   7
#endif

typedef struct {
    char ssid[128];       // WiFi network name (SSID)
    char password[128];   // WiFi password
    char openai_key[256]; // Optional OpenAI API key
    int  speaker_vol;     // 0..100 speaker volume
    int  mic_gain;        // 1..16 digital mic gain (applied before Opus)
    int  tz_offset_min;   // minutes east of UTC (from the browser; 0 = UTC)
} wifi_config_data_t;

esp_ip4_addr_t get_sta_ip_address(void);
esp_netif_t* start_wifi_ap(const char *ssid, const char *password);
esp_netif_t* start_wifi_sta(const char *ssid, const char *password);
bool wifi_is_sta_connected(void);
const wifi_config_data_t* get_web_wifi_config_data(void);

void start_wifi_config_webserver(void);
void stop_wifi_config_webserver(void);

void wifi_config_apply_defaults(wifi_config_data_t *config);
void wifi_config_sanitize(wifi_config_data_t *config);
bool read_wifi_config_from_nvs(wifi_config_data_t *config);
void write_wifi_config_to_nvs(const wifi_config_data_t *config);
void clear_nvs_config(void);

void wifi_config_init(void);
// Apply the saved timezone (NVS) so localtime() is correct after a
// software reset. The ESP32 RTC keeps UTC across esp_restart(); TZ does not.
void wifi_config_apply_saved_timezone(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_CONFIG_H
