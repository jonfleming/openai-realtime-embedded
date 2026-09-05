#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "esp_http_server.h"
#include "wifi_config.h"
#include "esp_lcd_panel_io.h"
#include "lcd.h"
#include "esp_lvgl_port.h"
#include "main.h"

static const char *TAG = "wifi_config";

#define NVS_NAMESPACE "wifi_config"

static wifi_config_data_t s_current{};
static httpd_handle_t wifi_config_server = NULL;
static bool sta_is_connected = false;
static bool s_init_complete = false;
static bool s_sta_should_connect = false;
static int s_retry_num = 0;
static esp_ip4_addr_t sta_ip = {0};
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool s_sntp_started = false;

#define TIME_VALID_AFTER 1577836800  // 2020-01-01 UTC; epoch means "not set"

static void sta_apply_and_connect(const char *ssid, const char *password);
static void restart_after_response_task(void *arg);
static void apply_timezone(int tz_offset_min);
static void format_local_time(char *out, size_t len);
static void start_sntp_if_needed(void);

void wifi_config_apply_defaults(wifi_config_data_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->speaker_vol = WIFI_CFG_DEFAULT_SPEAKER_VOL;
    config->mic_gain = WIFI_CFG_DEFAULT_MIC_GAIN;
}

void wifi_config_sanitize(wifi_config_data_t *config)
{
    if (!config) {
        return;
    }
    if (config->speaker_vol < WIFI_CFG_SPEAKER_VOL_MIN) {
        config->speaker_vol = WIFI_CFG_SPEAKER_VOL_MIN;
    } else if (config->speaker_vol > WIFI_CFG_SPEAKER_VOL_MAX) {
        config->speaker_vol = WIFI_CFG_SPEAKER_VOL_MAX;
    }
    if (config->mic_gain < WIFI_CFG_MIC_GAIN_MIN) {
        config->mic_gain = WIFI_CFG_MIC_GAIN_MIN;
    } else if (config->mic_gain > WIFI_CFG_MIC_GAIN_MAX) {
        config->mic_gain = WIFI_CFG_MIC_GAIN_MAX;
    }
    if (config->tz_offset_min < -14 * 60) {
        config->tz_offset_min = -14 * 60;
    } else if (config->tz_offset_min > 14 * 60) {
        config->tz_offset_min = 14 * 60;
    }
}

static void apply_timezone(int tz_offset_min)
{
    // POSIX TZ offset is what you add to local time to get UTC (hours west).
    int west_min = -tz_offset_min;
    int hours = west_min / 60;
    int mins = west_min % 60;
    if (mins < 0) {
        mins = -mins;
    }
    char tz[32];
    if (mins == 0) {
        snprintf(tz, sizeof(tz), "UTC%+d", hours);
    } else {
        snprintf(tz, sizeof(tz), "UTC%+d:%02d", hours, mins);
    }
    setenv("TZ", tz, 1);
    tzset();
}

static void format_local_time(char *out, size_t len)
{
    time_t now = time(NULL);
    if (now < TIME_VALID_AFTER) {
        snprintf(out, len, "not set");
        return;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(out, len, "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    char now[40];
    format_local_time(now, sizeof(now));
    ESP_LOGI(TAG, "SNTP synced, local time %s", now);
}

static void start_sntp_if_needed(void)
{
    if (s_sntp_started) {
        return;
    }
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.wait_for_sync = false;
    config.sync_cb = sntp_sync_cb;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        return;
    }
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started (pool.ntp.org)");
}

static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_len; ++i) {
        if (src[i] == '+') {
            dst[j++] = ' ';
        } else if (src[i] == '%' && src[i + 1] != '\0' && src[i + 2] != '\0') {
            char hex[3] = {src[i + 1], src[i + 2], '\0'};
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static void html_escape(char *dst, const char *src, size_t dst_len)
{
    size_t j = 0;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_len; ++i) {
        const char *rep = NULL;
        if (src[i] == '&') {
            rep = "&amp;";
        } else if (src[i] == '<') {
            rep = "&lt;";
        } else if (src[i] == '>') {
            rep = "&gt;";
        } else if (src[i] == '"') {
            rep = "&quot;";
        }
        if (rep) {
            size_t l = strlen(rep);
            if (j + l >= dst_len) {
                break;
            }
            memcpy(dst + j, rep, l);
            j += l;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    const size_t key_len = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *found = strstr(p, key);
        if (!found) {
            return false;
        }
        bool at_start = (found == body) || (found[-1] == '&');
        if (at_start && found[key_len] == '=') {
            const char *val = found + key_len + 1;
            const char *end = strchr(val, '&');
            size_t n = end ? (size_t)(end - val) : strlen(val);
            char tmp[512];
            if (n >= sizeof(tmp)) {
                n = sizeof(tmp) - 1;
            }
            memcpy(tmp, val, n);
            tmp[n] = '\0';
            url_decode(out, tmp, out_len);
            return true;
        }
        p = found + key_len;
    }
    return false;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_sta_should_connect && strlen(s_current.ssid) > 0) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        sta_is_connected = false;
        if (s_sta_should_connect && s_retry_num < 5) {
            s_retry_num++;
            ESP_LOGI(TAG, "STA retry %d/5", s_retry_num);
            esp_wifi_connect();
        } else {
            ESP_LOGI(TAG, "STA disconnected. SoftAP portal still at http://192.168.4.1");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        sta_ip = event->ip_info.ip;
        sta_is_connected = true;
        s_retry_num = 0;
        start_sntp_if_needed();
    }
}

esp_ip4_addr_t get_sta_ip_address(void)
{
    return sta_ip;
}

esp_netif_t* start_wifi_ap(const char *ssid, const char *password)
{
    wifi_config_t ap_config = {};
    ap_config.ap.ssid_len = (uint8_t)strlen(ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    if (password == NULL || strlen(password) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        strlcpy((char *)ap_config.ap.password, password, sizeof(ap_config.ap.password));
    }
    strlcpy((char *)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid));
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    return s_ap_netif;
}

bool wifi_is_sta_connected(void)
{
    return sta_is_connected;
}

esp_netif_t* start_wifi_sta(const char *ssid, const char *password)
{
    sta_apply_and_connect(ssid, password);
    return s_sta_netif;
}

const wifi_config_data_t* get_web_wifi_config_data(void)
{
    return &s_current;
}

static void sta_apply_and_connect(const char *ssid, const char *password)
{
    wifi_config_t sta_config = {};
    sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_config.sta.failure_retry_cnt = 5;
    if (password != NULL && password[0] != '\0') {
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        strlcpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password));
    } else {
        sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    strlcpy((char *)sta_config.sta.ssid, ssid ? ssid : "", sizeof(sta_config.sta.ssid));

    sta_is_connected = false;
    s_retry_num = 0;
    s_sta_should_connect = (ssid != NULL && ssid[0] != '\0');
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (s_sta_should_connect) {
        esp_wifi_connect();
    }
}

static const char *SETTINGS_HTML_HEAD =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "    <meta charset=\"UTF-8\">\n"
    "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=no\">\n"
    "    <title>Assistant Settings</title>\n"
    "    <style>\n"
    "        body {\n"
    "            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;\n"
    "            background: linear-gradient(to right, #f8f9fa, #e9ecef);\n"
    "            margin: 0; padding: 0;\n"
    "            display: flex;\n"
    "            justify-content: center;\n"
    "            align-items: center;\n"
    "            min-height: 100vh;\n"
    "            color: #333;\n"
    "        }\n"
    "        .container {\n"
    "            background-color: white;\n"
    "            border-radius: 12px;\n"
    "            box-shadow: 0 8px 16px rgba(0,0,0,0.15);\n"
    "            padding: 32px;\n"
    "            width: 90%; max-width: 420px;\n"
    "            margin: 20px 0;\n"
    "        }\n"
    "        h2 { text-align: center; margin: 0 0 16px; font-size: 1.45em; color: #4a4a4a; }\n"
    "        .status {\n"
    "            background: #f1f3f5; border-radius: 8px; padding: 10px 12px;\n"
    "            font-size: 0.9em; line-height: 1.45; margin-bottom: 18px; color: #444;\n"
    "        }\n"
    "        .status strong { color: #222; }\n"
    "        label { display: block; margin: 14px 0 6px; font-weight: bold; font-size: 0.95em; }\n"
    "        input[type=\"text\"], input[type=\"password\"], input[type=\"number\"] {\n"
    "            width: 95%;\n"
    "            padding: 10px 12px;\n"
    "            border: 1px solid #ccc;\n"
    "            border-radius: 6px;\n"
    "            font-size: 1em;\n"
    "            display: block;\n"
    "            margin: 0 auto;\n"
    "            box-sizing: border-box;\n"
    "        }\n"
    "        input[type=\"range\"] { width: 95%; display: block; margin: 0 auto; }\n"
    "        .range-row { display: flex; align-items: center; justify-content: space-between; width: 95%; margin: 0 auto 4px; }\n"
    "        .range-row output { min-width: 2.2em; text-align: right; font-weight: 600; }\n"
    "        .hint { font-size: 0.85em; color: #666; margin: 16px 0 0; line-height: 1.4; }\n"
    "        input[type=\"submit\"] {\n"
    "            width: 95%;\n"
    "            background-color: #007bff;\n"
    "            color: white;\n"
    "            padding: 12px;\n"
    "            font-size: 1.1em;\n"
    "            border: none;\n"
    "            border-radius: 6px;\n"
    "            cursor: pointer;\n"
    "            display: block;\n"
    "            margin: 24px auto 0;\n"
    "        }\n"
    "        input[type=\"submit\"]:hover { background-color: #0056b3; }\n"
    "    </style>\n"
    "</head>\n"
    "<body>\n"
    "    <div class=\"container\">\n"
    "        <h2>Assistant Settings</h2>\n";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    char ssid_e[256], pass_e[256], key_e[1024];
    html_escape(ssid_e, s_current.ssid, sizeof(ssid_e));
    html_escape(pass_e, s_current.password, sizeof(pass_e));
    html_escape(key_e, s_current.openai_key, sizeof(key_e));

    char clock_buf[40];
    format_local_time(clock_buf, sizeof(clock_buf));

    char status[512];
    if (sta_is_connected) {
        snprintf(status, sizeof(status),
                 "<strong>Wi-Fi:</strong> connected to %s (" IPSTR ")<br>"
                 "<strong>Setup page:</strong> always on at http://192.168.4.1 (join \"%s\")<br>"
                 "<strong>Clock:</strong> <span id=\"clock\">%s</span>",
                 ssid_e[0] ? ssid_e : "?",
                 IP2STR(&sta_ip),
                 EXAMPLE_ESP_WIFI_AP_SSID, clock_buf);
    } else if (s_current.ssid[0]) {
        snprintf(status, sizeof(status),
                 "<strong>Wi-Fi:</strong> connecting to %s&hellip;<br>"
                 "<strong>Setup page:</strong> http://192.168.4.1 (join \"%s\")<br>"
                 "<strong>Clock:</strong> <span id=\"clock\">%s</span>",
                 ssid_e, EXAMPLE_ESP_WIFI_AP_SSID, clock_buf);
    } else {
        snprintf(status, sizeof(status),
                 "<strong>Wi-Fi:</strong> not configured<br>"
                 "<strong>Setup page:</strong> http://192.168.4.1 (you are here)<br>"
                 "<strong>Clock:</strong> <span id=\"clock\">%s</span>",
                 clock_buf);
    }

    char *page = (char *)malloc(8192);
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int n = snprintf(page, 8192,
        "%s"
        "        <div class=\"status\">%s</div>\n"
        "        <form action=\"/configure\" method=\"POST\">\n"
        "            <label for=\"ssid\">Wi-Fi name (SSID)</label>\n"
        "            <input type=\"text\" id=\"ssid\" name=\"ssid\" value=\"%s\" placeholder=\"Home network\">\n"
        "            <label for=\"password\">Wi-Fi password</label>\n"
        "            <input type=\"password\" id=\"password\" name=\"password\" value=\"%s\" placeholder=\"Network password\">\n"
        "            <label for=\"openai_key\">OpenAI API key</label>\n"
        "            <input type=\"text\" id=\"openai_key\" name=\"openai_key\" value=\"%s\" placeholder=\"sk-...\">\n"
        "            <label for=\"speaker_vol\">Speaker volume (0–100)</label>\n"
        "            <div class=\"range-row\"><input type=\"range\" id=\"speaker_vol\" name=\"speaker_vol\" min=\"0\" max=\"100\" value=\"%d\" oninput=\"this.nextElementSibling.value=this.value\"><output>%d</output></div>\n"
        "            <label for=\"mic_gain\">Microphone gain (1–16)</label>\n"
        "            <div class=\"range-row\"><input type=\"range\" id=\"mic_gain\" name=\"mic_gain\" min=\"1\" max=\"16\" value=\"%d\" oninput=\"this.nextElementSibling.value=this.value\"><output>%d</output></div>\n"
        "            <input type=\"submit\" value=\"Save\">\n"
        "        </form>\n"
        "        <p class=\"hint\">Volume and mic gain apply immediately. Changing Wi-Fi or the API key reconnects and reboots so the voice session picks them up. This page stays available on the %s network at 192.168.4.1. Opening this page sets the watch clock from your phone; after Wi-Fi connects it also syncs from NTP.</p>\n"
        "        <script>(function(){var b='epoch_ms='+Date.now()+'&tz_offset_min='+(-new Date().getTimezoneOffset());fetch('/time',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}).then(function(r){return r.text();}).then(function(t){var e=document.getElementById('clock');if(e)e.textContent=t;}).catch(function(){});})();</script>\n"
        "    </div>\n"
        "</body>\n"
        "</html>",
        SETTINGS_HTML_HEAD, status, ssid_e, pass_e, key_e,
        s_current.speaker_vol, s_current.speaker_vol,
        s_current.mic_gain, s_current.mic_gain,
        EXAMPLE_ESP_WIFI_AP_SSID);

    httpd_resp_set_type(req, "text/html");
    if (n < 0 || n >= 8192) {
        free(page);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Page too large");
        return ESP_FAIL;
    }
    httpd_resp_send(req, page, n);
    free(page);
    return ESP_OK;
}

static const httpd_uri_t root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
};

static void restart_after_response_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "Restarting to apply Wi-Fi / API key changes");
    esp_restart();
}

static esp_err_t configure_post_handler(httpd_req_t *req)
{
    char buf[1536];
    int remaining = req->content_len;

    if (remaining <= 0 || remaining >= (int)sizeof(buf)) {
        ESP_LOGE(TAG, "Invalid content length: %d", remaining);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, buf, MIN(remaining, (int)sizeof(buf) - 1));
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal Server Error");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    wifi_config_data_t incoming = s_current;
    char field[256];
    char vol_str[16];
    char gain_str[16];

    if (form_get(buf, "ssid", field, sizeof(field))) {
        strlcpy(incoming.ssid, field, sizeof(incoming.ssid));
    }
    if (form_get(buf, "password", field, sizeof(field))) {
        strlcpy(incoming.password, field, sizeof(incoming.password));
    }
    if (form_get(buf, "openai_key", field, sizeof(incoming.openai_key))) {
        strlcpy(incoming.openai_key, field, sizeof(incoming.openai_key));
    }
    if (form_get(buf, "speaker_vol", vol_str, sizeof(vol_str))) {
        incoming.speaker_vol = atoi(vol_str);
    }
    if (form_get(buf, "mic_gain", gain_str, sizeof(gain_str))) {
        incoming.mic_gain = atoi(gain_str);
    }
    wifi_config_sanitize(&incoming);

    if (incoming.ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
        return ESP_FAIL;
    }

    bool wifi_changed = (strcmp(incoming.ssid, s_current.ssid) != 0) ||
                        (strcmp(incoming.password, s_current.password) != 0);
    bool key_changed = (strcmp(incoming.openai_key, s_current.openai_key) != 0);
    bool need_restart = s_init_complete && (wifi_changed || key_changed);

    s_current = incoming;
    write_wifi_config_to_nvs(&s_current);
    oai_apply_audio_settings(s_current.speaker_vol, s_current.mic_gain);

    if (wifi_changed || !sta_is_connected) {
        ESP_LOGI(TAG, "Applying STA credentials for SSID=%s", s_current.ssid);
        sta_apply_and_connect(s_current.ssid, s_current.password);
    }

    const char *success_html =
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "    <meta charset=\"UTF-8\">\n"
        "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "    <title>Saved</title>\n"
        "    <style>\n"
        "        body { font-family: sans-serif; text-align: center; margin-top: 10%; background-color: #f0fff0; }\n"
        "        h1 { color: green; }\n"
        "        p { font-size: 1.1em; }\n"
        "    </style>\n"
        "</head>\n"
        "<body>\n"
        "    <h1>Settings saved</h1>\n"
        "    <p>Volume and microphone gain are active now.</p>\n"
        "    <p>The setup page stays at <a href=\"/\">http://192.168.4.1</a> on the OpenAI network.</p>\n"
        "</body>\n"
        "</html>";

    httpd_resp_set_type(req, "text/html");
    if (need_restart) {
        httpd_resp_send(req,
            "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><title>Saved</title>"
            "<style>body{font-family:sans-serif;text-align:center;margin-top:10%;background:#f0fff0}"
            "h1{color:green}</style></head><body>"
            "<h1>Settings saved</h1>"
            "<p>Wi-Fi or API key changed. The device will reboot in a moment.</p>"
            "<p>Rejoin <b>OpenAI</b> and open http://192.168.4.1 any time to edit again.</p>"
            "</body></html>",
            HTTPD_RESP_USE_STRLEN);
        xTaskCreate(restart_after_response_task, "cfg_restart", 2048, NULL, 5, NULL);
    } else {
        httpd_resp_send(req, success_html, HTTPD_RESP_USE_STRLEN);
    }

    return ESP_OK;
}

static const httpd_uri_t configure = {
    .uri       = "/configure",
    .method    = HTTP_POST,
    .handler   = configure_post_handler,
    .user_ctx  = NULL
};

static esp_err_t time_post_handler(httpd_req_t *req)
{
    char buf[128];
    int remaining = req->content_len;
    if (remaining <= 0 || remaining >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad Request");
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, buf, remaining);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Internal Server Error");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char ms_str[32];
    char tz_str[16];
    int64_t epoch_ms = 0;
    int tz = s_current.tz_offset_min;
    if (form_get(buf, "epoch_ms", ms_str, sizeof(ms_str))) {
        epoch_ms = strtoll(ms_str, NULL, 10);
    }
    if (form_get(buf, "tz_offset_min", tz_str, sizeof(tz_str))) {
        tz = atoi(tz_str);
    }

    bool tz_changed = (tz != s_current.tz_offset_min);
    s_current.tz_offset_min = tz;
    wifi_config_sanitize(&s_current);
    apply_timezone(s_current.tz_offset_min);
    if (tz_changed) {
        write_wifi_config_to_nvs(&s_current);
    }

    // Browser time is the fallback when NTP has not run yet (SoftAP-only,
    // or STA just connected). Once the clock is valid, keep NTP's UTC and
    // only refresh the timezone from the phone.
    if (epoch_ms > (int64_t)TIME_VALID_AFTER * 1000 && time(NULL) < TIME_VALID_AFTER) {
        struct timeval tv;
        tv.tv_sec = (time_t)(epoch_ms / 1000);
        tv.tv_usec = (suseconds_t)((epoch_ms % 1000) * 1000);
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "Clock set from browser");
    }

    char now[40];
    format_local_time(now, sizeof(now));
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, now);
    return ESP_OK;
}

static const httpd_uri_t time_post = {
    .uri       = "/time",
    .method    = HTTP_POST,
    .handler   = time_post_handler,
    .user_ctx  = NULL
};

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static const httpd_uri_t favicon = {
    .uri       = "/favicon.ico",
    .method    = HTTP_GET,
    .handler   = favicon_get_handler,
    .user_ctx  = NULL
};

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server_handle = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting settings server on port %d", config.server_port);
    if (httpd_start(&server_handle, &config) == ESP_OK) {
        httpd_register_uri_handler(server_handle, &root);
        httpd_register_uri_handler(server_handle, &configure);
        httpd_register_uri_handler(server_handle, &time_post);
        httpd_register_uri_handler(server_handle, &favicon);
        return server_handle;
    }

    ESP_LOGE(TAG, "Error starting settings server");
    return NULL;
}

static void stop_webserver(void)
{
    if (wifi_config_server) {
        httpd_stop(wifi_config_server);
        wifi_config_server = NULL;
    }
}

void start_wifi_config_webserver(void)
{
    if (wifi_config_server == NULL) {
        wifi_config_server = start_webserver();
    }
}

void stop_wifi_config_webserver(void)
{
    stop_webserver();
}

bool read_wifi_config_from_nvs(wifi_config_data_t *config)
{
    wifi_config_apply_defaults(config);

    nvs_handle_t my_nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }

    size_t len = sizeof(config->ssid);
    err = nvs_get_str(my_nvs_handle, "ssid", config->ssid, &len);
    if (err != ESP_OK) {
        nvs_close(my_nvs_handle);
        return false;
    }

    len = sizeof(config->password);
    err = nvs_get_str(my_nvs_handle, "password", config->password, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(my_nvs_handle);
        return false;
    }

    len = sizeof(config->openai_key);
    err = nvs_get_str(my_nvs_handle, "openai_key", config->openai_key, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to read openai_key from NVS");
    }

    int32_t vol = WIFI_CFG_DEFAULT_SPEAKER_VOL;
    if (nvs_get_i32(my_nvs_handle, "spk_vol", &vol) == ESP_OK) {
        config->speaker_vol = (int)vol;
    }
    int32_t gain = WIFI_CFG_DEFAULT_MIC_GAIN;
    if (nvs_get_i32(my_nvs_handle, "mic_gain", &gain) == ESP_OK) {
        config->mic_gain = (int)gain;
    }
    int32_t tz = 0;
    if (nvs_get_i32(my_nvs_handle, "tz_min", &tz) == ESP_OK) {
        config->tz_offset_min = (int)tz;
    }

    nvs_close(my_nvs_handle);
    wifi_config_sanitize(config);
    return true;
}

void write_wifi_config_to_nvs(const wifi_config_data_t *config)
{
    nvs_handle_t my_nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(my_nvs_handle, "ssid", config->ssid);
    err |= nvs_set_str(my_nvs_handle, "password", config->password);
    if (strlen(config->openai_key) > 0) {
        err |= nvs_set_str(my_nvs_handle, "openai_key", config->openai_key);
    }
    err |= nvs_set_i32(my_nvs_handle, "spk_vol", config->speaker_vol);
    err |= nvs_set_i32(my_nvs_handle, "mic_gain", config->mic_gain);
    err |= nvs_set_i32(my_nvs_handle, "tz_min", config->tz_offset_min);
    err |= nvs_commit(my_nvs_handle);
    nvs_close(my_nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(err));
    }
}

void wifi_config_apply_saved_timezone(void)
{
    wifi_config_data_t cfg;
    wifi_config_apply_defaults(&cfg);
    read_wifi_config_from_nvs(&cfg);
    apply_timezone(cfg.tz_offset_min);
}

void clear_nvs_config(void)
{
    nvs_handle_t my_nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_nvs_handle);
    if (err == ESP_OK) {
        nvs_erase_all(my_nvs_handle);
        nvs_commit(my_nvs_handle);
        nvs_close(my_nvs_handle);
    }
    ESP_LOGI(TAG, "NVS cleared");
}

void wifi_config_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_apply_defaults(&s_current);
    bool has_saved_config = read_wifi_config_from_nvs(&s_current);
    apply_timezone(s_current.tz_offset_min);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    start_wifi_ap(EXAMPLE_ESP_WIFI_AP_SSID, EXAMPLE_ESP_WIFI_AP_PASSWD);

    if (has_saved_config && s_current.ssid[0] != '\0') {
        wifi_config_t sta_config = {};
        sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        sta_config.sta.failure_retry_cnt = 5;
        if (s_current.password[0] != '\0') {
            sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
            strlcpy((char *)sta_config.sta.password, s_current.password, sizeof(sta_config.sta.password));
        } else {
            sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        }
        strlcpy((char *)sta_config.sta.ssid, s_current.ssid, sizeof(sta_config.sta.ssid));
        s_sta_should_connect = true;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
        ESP_LOGI(TAG, "Saved SSID=%s vol=%d mic_gain=%d",
                 s_current.ssid, s_current.speaker_vol, s_current.mic_gain);
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    start_wifi_config_webserver();

#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
    lvgl_ui_label_set_text("Connect to WiFi\n\"" EXAMPLE_ESP_WIFI_AP_SSID "\"\nthen open\n192.168.4.1");
#else
    lvgl_ui_label_set_text("Join WiFi \"" EXAMPLE_ESP_WIFI_AP_SSID "\" and open 192.168.4.1");
#endif

    if (s_sta_should_connect) {
        char buf[128];
        lv_snprintf(buf, sizeof(buf), "SSID=%s", s_current.ssid);
        lvgl_ui_label_set_text(buf);
        lvgl_ui_label_set_text("Connecting...");
        esp_wifi_connect();
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi. SoftAP \"%s\" portal at http://192.168.4.1",
                 EXAMPLE_ESP_WIFI_AP_SSID);
    }

    // Block until STA has an IP. The SoftAP and settings page stay up the
    // whole time (and after we return) so credentials can be fixed without
    // wiping NVS or rebooting into a loop.
    int wait_ticks = 0;
    while (!sta_is_connected) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait_ticks++;
        if (s_sta_should_connect && !sta_is_connected && s_retry_num >= 5 &&
            (wait_ticks % 30) == 0) {
            s_retry_num = 0;
            ESP_LOGI(TAG, "Retrying STA; portal still at http://192.168.4.1");
#if defined(AIPI_LITE_BOARD) && AIPI_LITE_BOARD
            lvgl_ui_label_set_text("WiFi failed\nre-enter at\n192.168.4.1");
#else
            lvgl_ui_label_set_text("WiFi failed. Re-enter at 192.168.4.1");
#endif
            esp_wifi_connect();
        }
    }

    ESP_LOGI(TAG, "WiFi connection successful. SoftAP remains at http://192.168.4.1");
    lvgl_ui_label_set_text("WiFi connection successful.");
    vTaskDelay(pdMS_TO_TICKS(500));
    lvgl_ui_label_set_text("You can chat now.");
    s_init_complete = true;
}
