// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "http_ui.h"
#include "http_camera.h"
#include "http_firmware.h"
#include "config.h"
#include "http_audio_stream.h"
#include "http_video_stream.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_camera.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "driver/ledc.h"
#include "driver/temperature_sensor.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "http_ui";

// LED state
#define LED_LEDC_TIMER    LEDC_TIMER_1
#define LED_LEDC_CHANNEL  LEDC_CHANNEL_2
#define LED_LEDC_SPEED    LEDC_LOW_SPEED_MODE
#define LED_MAX_INTENSITY 255

int led_duty = 0;
bool led_on = false;
bool led_stream_enabled = true;
static int led_pin = -1;
bool isStreaming = false;
bool camera_available = false;
bool mic_available = false;

// ---------- Safe Restart ----------

void safe_restart(void)
{
    ESP_LOGI(TAG, "Shutting down before restart...");
    stop_video_stream();
    stop_audio_stream();
    esp_camera_deinit();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

// ---------- LED ----------

void setupLedFlash(int pin)
{
    led_pin = pin;

    ledc_timer_config_t timer_conf = {
        .speed_mode      = LED_LEDC_SPEED,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LED_LEDC_TIMER,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t ch_conf = {
        .gpio_num   = pin,
        .speed_mode = LED_LEDC_SPEED,
        .channel    = LED_LEDC_CHANNEL,
        .timer_sel  = LED_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch_conf);
}

void enable_led(bool en)
{
    if (led_pin < 0) return;
    int duty = en ? led_duty : 0;
    if (en && isStreaming && (led_duty > LED_MAX_INTENSITY)) {
        duty = LED_MAX_INTENSITY;
    }
    ledc_set_duty(LED_LEDC_SPEED, LED_LEDC_CHANNEL, duty);
    ledc_update_duty(LED_LEDC_SPEED, LED_LEDC_CHANNEL);
    ESP_LOGI(TAG, "Set LED intensity to %d", duty);
}

// ---------- Helpers ----------

static int read_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len >= (int)buf_size) return -1;

    int cur_len = 0;
    while (cur_len < total_len) {
        int received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return -1;
        }
        cur_len += received;
    }
    buf[cur_len] = '\0';
    return cur_len;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    const char *str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, str, strlen(str));
    free((void *)str);
    cJSON_Delete(root);
    return ret;
}

static const char *cjson_get_string(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(item)) return item->valuestring;
    return NULL;
}

// ---------- Auth ----------

bool check_auth(httpd_req_t *req)
{
    if (stored_auth_pass[0] == '\0') return true;

    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len == 0) return false;

    char *hdr = (char *)malloc(hdr_len + 1);
    if (!hdr) return false;

    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, hdr_len + 1) != ESP_OK) {
        free(hdr);
        return false;
    }

    if (strncmp(hdr, "Basic ", 6) != 0) {
        free(hdr);
        return false;
    }

    const char *b64 = hdr + 6;
    size_t b64_len = strlen(b64);
    size_t decoded_len = 0;
    unsigned char decoded[128];

    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                               (const unsigned char *)b64, b64_len) != 0) {
        free(hdr);
        return false;
    }
    decoded[decoded_len] = '\0';
    free(hdr);

    const char *colon = strchr((const char *)decoded, ':');
    const char *pass = colon ? colon + 1 : (const char *)decoded;

    return strcmp(pass, stored_auth_pass) == 0;
}

esp_err_t send_auth_required(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Chute\"");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"error\":\"auth_required\"}", HTTPD_RESP_USE_STRLEN);
}

// ---------- CORS ----------

esp_err_t cors_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization, Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---------- SPIFFS File Serving ----------

static const char *get_mime_type(const char *path)
{
    if (strstr(path, ".html")) return "text/html";
    if (strstr(path, ".css"))  return "text/css";
    if (strstr(path, ".js"))   return "application/javascript";
    if (strstr(path, ".ico"))  return "image/x-icon";
    return "application/octet-stream";
}

static esp_err_t serve_spiffs_file(httpd_req_t *req, const char *filename)
{
    char gz_path[64], path[64];
    snprintf(gz_path, sizeof(gz_path), "/www/%s.gz", filename);
    snprintf(path, sizeof(path), "/www/%s", filename);

    struct stat st;
    bool gzipped = (stat(gz_path, &st) == 0);
    const char *actual = gzipped ? gz_path : path;
    if (!gzipped && stat(path, &st) != 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    FILE *f = fopen(actual, "r");
    if (!f) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, get_mime_type(filename));
    if (gzipped) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// ---------- SPA + Static Handlers ----------

static esp_err_t spa_handler(httpd_req_t *req)
{
    return serve_spiffs_file(req, "index.html");
}

static esp_err_t app_js_handler(httpd_req_t *req)
{
    return serve_spiffs_file(req, "app.js");
}

static esp_err_t app_css_handler(httpd_req_t *req)
{
    return serve_spiffs_file(req, "app.css");
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    return serve_spiffs_file(req, "favicon.ico");
}

// ---------- API Handlers ----------

static esp_err_t api_info_handler(httpd_req_t *req)
{
    char ip[16];
    get_current_ip_str(ip, sizeof(ip));
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "wifi_mode", wifi_ap_active ? "AP" : "STA");
    cJSON_AddStringToObject(root, "wifi_mode_pref", stored_wifi_mode);
    cJSON_AddStringToObject(root, "ssid", stored_ssid);
    cJSON_AddStringToObject(root, "ap_ssid", stored_ap_ssid);
    if (stored_auth_pass[0] == '\0') {
        cJSON_AddStringToObject(root, "password", stored_password);
        cJSON_AddStringToObject(root, "ap_password", stored_ap_password);
    } else {
        cJSON_AddBoolToObject(root, "password_set", stored_password[0] != '\0');
        cJSON_AddBoolToObject(root, "ap_password_set", stored_ap_password[0] != '\0');
    }
    cJSON_AddStringToObject(root, "hostname", stored_hostname);
    cJSON_AddNumberToObject(root, "rssi", get_wifi_rssi());
    cJSON_AddNumberToObject(root, "mic_gain", (int)mic_gain);
    cJSON_AddBoolToObject(root, "auth_enabled", stored_auth_pass[0] != '\0');
    cJSON_AddStringToObject(root, "running_partition", run ? run->label : "?");
    cJSON_AddStringToObject(root, "boot_partition", boot ? boot->label : "?");
    cJSON_AddNumberToObject(root, "stream_port", 81);
    cJSON_AddNumberToObject(root, "audio_port", 82);
    cJSON_AddBoolToObject(root, "camera", camera_available);
    cJSON_AddBoolToObject(root, "mic", mic_available);

    return send_json(req, root);
}

static float read_internal_temp(void)
{
    static temperature_sensor_handle_t temp_handle = NULL;
    if (!temp_handle) {
        temperature_sensor_config_t conf = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&conf, &temp_handle) != ESP_OK) return -999;
        if (temperature_sensor_enable(temp_handle) != ESP_OK) return -999;
    }
    float t = 0;
    if (temperature_sensor_get_celsius(temp_handle, &t) != ESP_OK) return -999;
    return t;
}

static esp_err_t api_system_info_handler(httpd_req_t *req)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    size_t spiffs_total = 0, spiffs_used = 0;
    esp_spiffs_info("spiffs", &spiffs_total, &spiffs_used);

    char ip[16];
    get_current_ip_str(ip, sizeof(ip));
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();

    char chip_str[48];
    snprintf(chip_str, sizeof(chip_str), "%s rev %u.%u (%d cores)",
        CONFIG_IDF_TARGET, chip_info.revision / 100, chip_info.revision % 100, chip_info.cores);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "psram_free", esp_psram_is_initialized() ?
        esp_psram_get_size() - (esp_get_free_heap_size() - esp_get_free_internal_heap_size()) : 0);
    cJSON_AddNumberToObject(root, "psram_total", esp_psram_is_initialized() ? esp_psram_get_size() : 0);
    cJSON_AddNumberToObject(root, "spiffs_total", spiffs_total);
    cJSON_AddNumberToObject(root, "spiffs_used", spiffs_used);
    cJSON_AddStringToObject(root, "chip", chip_str);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    float temp_c = read_internal_temp();
    if (temp_c > -999) cJSON_AddNumberToObject(root, "temp_c", temp_c);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "wifi_mode", wifi_ap_active ? "AP" : "STA");
    cJSON_AddStringToObject(root, "wifi_mode_pref", stored_wifi_mode);
    cJSON_AddStringToObject(root, "ssid", stored_ssid);
    cJSON_AddNumberToObject(root, "rssi", get_wifi_rssi());
    cJSON_AddStringToObject(root, "running_partition", run ? run->label : "?");
    cJSON_AddStringToObject(root, "boot_partition", boot ? boot->label : "?");

    return send_json(req, root);
}

static esp_err_t api_auth_check_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "auth_enabled", stored_auth_pass[0] != '\0');
    cJSON_AddBoolToObject(root, "valid", check_auth(req));
    return send_json(req, root);
}

static esp_err_t api_auth_password_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const char *password = cjson_get_string(root, "password");
    saveAuthPassword(password ? password : "");
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(req, resp);
}

static esp_err_t api_wifi_config_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const char *ssid = cjson_get_string(root, "ssid");
    const char *password = cjson_get_string(root, "password");
    const char *wifi_mode = cjson_get_string(root, "wifi_mode");
    const char *ap_ssid = cjson_get_string(root, "ap_ssid");
    const char *ap_password = cjson_get_string(root, "ap_password");
    const char *hostname = cjson_get_string(root, "hostname");

    ESP_LOGI(TAG, "WiFi config: ssid='%s', pass='%s', mode='%s'",
             ssid ? ssid : "(null)",
             password ? password : "(null)",
             wifi_mode ? wifi_mode : "(null)");

    if ((!ssid || ssid[0] == '\0') && (!wifi_mode || strcmp(wifi_mode, "ap") != 0)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    saveWiFiCredentials(ssid ? ssid : "", password ? password : "");
    if (wifi_mode) saveWiFiMode(wifi_mode);
    if (ap_ssid && ap_ssid[0]) saveApSsid(ap_ssid);
    if (wifi_mode && strcmp(wifi_mode, "ap") == 0)
        saveApPassword(ap_password ? ap_password : "");
    if (hostname && hostname[0]) saveHostname(hostname);

    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    send_json(req, resp);

    vTaskDelay(pdMS_TO_TICKS(1000));
    safe_restart();
    return ESP_OK;
}

static esp_err_t api_wifi_scan_handler(httpd_req_t *req)
{
    // WiFi scan requires STA (or APSTA) mode. If in pure AP mode,
    // temporarily switch to APSTA so the radio can scan.
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    bool was_ap_only = (mode == WIFI_MODE_AP);
    if (was_ap_only) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);

    // Restore pure AP mode if that's what we had
    if (was_ap_only) {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");

    if (err != ESP_OK) {
        return send_json(req, root);
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;

    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!ap_list) {
        return send_json(req, root);
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    for (int i = 0; i < ap_count; i++) {
        const char *auth_str;
        switch (ap_list[i].authmode) {
            case WIFI_AUTH_OPEN: auth_str = "Open"; break;
            case WIFI_AUTH_WEP: auth_str = "WEP"; break;
            case WIFI_AUTH_WPA_PSK: auth_str = "WPA"; break;
            case WIFI_AUTH_WPA2_PSK: auth_str = "WPA2"; break;
            case WIFI_AUTH_WPA_WPA2_PSK: auth_str = "WPA/2"; break;
            case WIFI_AUTH_WPA3_PSK: auth_str = "WPA3"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK: auth_str = "WPA2/3"; break;
            default: auth_str = "Other"; break;
        }
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", (char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", ap_list[i].rssi);
        cJSON_AddStringToObject(ap, "auth", auth_str);
        cJSON_AddItemToArray(networks, ap);
    }

    free(ap_list);
    return send_json(req, root);
}

static esp_err_t api_audio_config_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "mic_gain", (int)mic_gain);
    cJSON_AddNumberToObject(root, "sample_rate", stored_sample_rate);
    cJSON_AddNumberToObject(root, "mic_bits", SAMPLE_BITS);
    cJSON_AddNumberToObject(root, "wav_bits", stored_wav_bits);
    return send_json(req, root);
}

static esp_err_t api_audio_config_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    char body[128];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *gain_item = cJSON_GetObjectItem(root, "mic_gain");
    if (cJSON_IsNumber(gain_item) && gain_item->valueint >= 1 && gain_item->valueint <= 32) {
        saveMicGain(gain_item->valueint);
    }

    cJSON *sr_item = cJSON_GetObjectItem(root, "sample_rate");
    cJSON *wb_item = cJSON_GetObjectItem(root, "wav_bits");
    int new_sr = stored_sample_rate;
    int new_wb = stored_wav_bits;
    bool rate_changed = false;

    if (cJSON_IsNumber(sr_item)) {
        int sr = sr_item->valueint;
        if (sr == 8000 || sr == 11025 || sr == 16000 || sr == 22050 || sr == 44100) {
            if (sr != stored_sample_rate) rate_changed = true;
            new_sr = sr;
        }
    }
    if (cJSON_IsNumber(wb_item)) {
        int wb = wb_item->valueint;
        if (wb == 16 || wb == 24) {
            new_wb = wb;
        }
    }

    cJSON_Delete(root);

    if (new_sr != stored_sample_rate || new_wb != stored_wav_bits) {
        saveAudioConfig(new_sr, new_wb);
    }

    if (rate_changed) {
        mic_i2s_reinit();
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(req, resp);
}

// ---------- LED API ----------

static esp_err_t api_led_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "intensity", led_duty);
    cJSON_AddBoolToObject(root, "on", led_on);
    cJSON_AddBoolToObject(root, "stream_enabled", led_stream_enabled);
    return send_json(req, root);
}

static esp_err_t api_led_control_handler(httpd_req_t *req)
{
    char body[128];
    if (read_body(req, body, sizeof(body)) < 0) {
        return httpd_resp_send_500(req);
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *intensity_item = cJSON_GetObjectItem(root, "intensity");
    cJSON *on_item = cJSON_GetObjectItem(root, "on");
    cJSON *stream_item = cJSON_GetObjectItem(root, "stream_enabled");

    if (cJSON_IsNumber(intensity_item)) {
        int intensity = intensity_item->valueint;
        if (intensity >= 0 && intensity <= 255) {
            led_duty = intensity;
            saveCameraSetting("led_intensity", intensity);
            if (led_on) enable_led(true);
        }
    }
    if (cJSON_IsNumber(on_item)) {
        if (on_item->valueint) {
            led_on = true;
            enable_led(true);
        } else {
            led_on = false;
            if (!isStreaming || !led_stream_enabled)
                enable_led(false);
        }
    }
    if (cJSON_IsNumber(stream_item)) {
        led_stream_enabled = (stream_item->valueint != 0);
        saveCameraSetting("led_stream", led_stream_enabled ? 1 : 0);
        if (isStreaming && !led_stream_enabled && !led_on)
            enable_led(false);
        else if (isStreaming && led_stream_enabled)
            enable_led(true);
    }

    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    return send_json(req, resp);
}

// ---------- System Actions ----------

static esp_err_t api_system_reboot_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(500));
    safe_restart();
    return ESP_OK;
}

static esp_err_t api_system_reset_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(500));
    eraseAllSettings();
    safe_restart();
    return ESP_OK;
}

// ---------- Server Start ----------

void start_http_ui(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 34;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting UI server on port %d", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start UI server");
        return;
    }

    httpd_uri_t uris[] = {
        // SPA
        { .uri = "/",                       .method = HTTP_GET,  .handler = spa_handler,                  .user_ctx = NULL },
        { .uri = "/app.js",                 .method = HTTP_GET,  .handler = app_js_handler,               .user_ctx = NULL },
        { .uri = "/app.css",                .method = HTTP_GET,  .handler = app_css_handler,              .user_ctx = NULL },
        { .uri = "/favicon.ico",            .method = HTTP_GET,  .handler = favicon_handler,              .user_ctx = NULL },

        // Info APIs
        { .uri = "/api/info",               .method = HTTP_GET,  .handler = api_info_handler,             .user_ctx = NULL },
        { .uri = "/api/system/info",        .method = HTTP_GET,  .handler = api_system_info_handler,      .user_ctx = NULL },

        // Auth APIs
        { .uri = "/api/auth/check",         .method = HTTP_GET,  .handler = api_auth_check_handler,       .user_ctx = NULL },
        { .uri = "/api/auth/password",      .method = HTTP_POST, .handler = api_auth_password_handler,    .user_ctx = NULL },
        { .uri = "/api/auth/password",      .method = HTTP_OPTIONS, .handler = cors_handler,              .user_ctx = NULL },

        // WiFi APIs
        { .uri = "/api/wifi/config",        .method = HTTP_POST, .handler = api_wifi_config_handler,      .user_ctx = NULL },
        { .uri = "/api/wifi/config",        .method = HTTP_OPTIONS, .handler = cors_handler,              .user_ctx = NULL },
        { .uri = "/api/wifi/scan",          .method = HTTP_GET,  .handler = api_wifi_scan_handler,        .user_ctx = NULL },

        // Audio APIs
        { .uri = "/api/audio/config",       .method = HTTP_GET,  .handler = api_audio_config_get_handler, .user_ctx = NULL },
        { .uri = "/api/audio/config",       .method = HTTP_POST, .handler = api_audio_config_post_handler,.user_ctx = NULL },
        { .uri = "/api/audio/config",       .method = HTTP_OPTIONS, .handler = cors_handler,              .user_ctx = NULL },

        // LED APIs
        { .uri = "/api/led/status",         .method = HTTP_GET,  .handler = api_led_status_handler,       .user_ctx = NULL },
        { .uri = "/api/led/control",        .method = HTTP_POST, .handler = api_led_control_handler,      .user_ctx = NULL },
        { .uri = "/api/led/control",        .method = HTTP_OPTIONS, .handler = cors_handler,              .user_ctx = NULL },

        // Camera APIs
        { .uri = "/api/camera/info",        .method = HTTP_GET,  .handler = camera_info_handler,          .user_ctx = NULL },
        { .uri = "/api/camera/status",      .method = HTTP_GET,  .handler = camera_status_handler,        .user_ctx = NULL },
        { .uri = "/api/camera/control",     .method = HTTP_POST, .handler = camera_control_handler,       .user_ctx = NULL },
        { .uri = "/api/camera/control",     .method = HTTP_OPTIONS, .handler = cors_handler,              .user_ctx = NULL },
        { .uri = "/api/camera/capture",     .method = HTTP_GET,  .handler = camera_capture_handler,       .user_ctx = NULL },

        // System action APIs
        { .uri = "/api/system/reboot",     .method = HTTP_POST, .handler = api_system_reboot_handler,     .user_ctx = NULL },
        { .uri = "/api/system/reboot",     .method = HTTP_OPTIONS, .handler = cors_handler,               .user_ctx = NULL },
        { .uri = "/api/system/reset",      .method = HTTP_POST, .handler = api_system_reset_handler,      .user_ctx = NULL },
        { .uri = "/api/system/reset",      .method = HTTP_OPTIONS, .handler = cors_handler,               .user_ctx = NULL },

        // Firmware APIs
        { .uri = "/api/firmware/upload",    .method = HTTP_POST, .handler = firmware_upload_handler,       .user_ctx = NULL },
        { .uri = "/api/firmware/upload",    .method = HTTP_OPTIONS, .handler = cors_handler,               .user_ctx = NULL },
        { .uri = "/api/firmware/boot",      .method = HTTP_POST, .handler = firmware_boot_handler,         .user_ctx = NULL },
        { .uri = "/api/firmware/boot",      .method = HTTP_OPTIONS, .handler = cors_handler,               .user_ctx = NULL },
    };

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}
