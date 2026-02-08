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
#include "settings_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "driver/ledc.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "http_ui";

// LED state
#define LED_LEDC_TIMER    LEDC_TIMER_1
#define LED_LEDC_CHANNEL  LEDC_CHANNEL_2
#define LED_LEDC_SPEED    LEDC_LOW_SPEED_MODE
#define LED_MAX_INTENSITY 255

int led_duty = 0;
static int led_pin = -1;
bool isStreaming = false;

// Forward declarations
static bool check_auth(httpd_req_t *req);
static esp_err_t send_auth_required(httpd_req_t *req);

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

typedef struct {
    httpd_req_t *req;
    size_t len;
} jpg_chunking_t;

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len)
{
    jpg_chunking_t *j = (jpg_chunking_t *)arg;
    if (!index) {
        j->len = 0;
    }
    if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK) {
        return 0;
    }
    j->len += len;
    return len;
}

static esp_err_t parse_get(httpd_req_t *req, char **obuf)
{
    char *buf = NULL;
    size_t buf_len = 0;

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = (char *)malloc(buf_len);
        if (!buf) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            *obuf = buf;
            return ESP_OK;
        }
        free(buf);
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static int parse_get_var(char *buf, const char *key, int def)
{
    char _int[16];
    if (httpd_query_key_value(buf, key, _int, sizeof(_int)) != ESP_OK) {
        return def;
    }
    return atoi(_int);
}

static int print_reg(char *p, sensor_t *s, uint16_t reg, uint32_t mask)
{
    return sprintf(p, "\"0x%x\":%u,", reg, s->get_reg(s, reg, mask));
}

// ---------- Auth ----------

static bool check_auth(httpd_req_t *req)
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

static esp_err_t send_auth_required(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32-CAM\"");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req,
        "<!DOCTYPE html><html><body style='font-family:sans-serif;text-align:center;padding:50px;'>"
        "<h2>Authentication Required</h2><p>Please enter the settings password.</p>"
        "</body></html>", HTTPD_RESP_USE_STRLEN);
}

// ---------- SPIFFS File Serving ----------

static const char *get_mime_type(const char *path)
{
    if (strstr(path, ".html")) return "text/html";
    if (strstr(path, ".css"))  return "text/css";
    if (strstr(path, ".js"))   return "application/javascript";
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
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");

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

// ---------- Handlers ----------

static esp_err_t player_handler(httpd_req_t *req)
{
    return serve_spiffs_file(req, "player.html");
}

static esp_err_t settings_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);
    return serve_spiffs_file(req, "settings.html");
}

static esp_err_t index_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);
    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        if (s->id.PID == OV3660_PID) {
            return serve_spiffs_file(req, "camera_ov3660.html");
        } else if (s->id.PID == OV5640_PID) {
            return serve_spiffs_file(req, "camera_ov5640.html");
        } else {
            return serve_spiffs_file(req, "camera_ov2640.html");
        }
    } else {
        ESP_LOGE(TAG, "Camera sensor not found");
        return httpd_resp_send_500(req);
    }
}

static esp_err_t css_handler(httpd_req_t *req)
{
    return serve_spiffs_file(req, "common.css");
}

static esp_err_t api_info_handler(httpd_req_t *req)
{
    char ip[16];
    get_current_ip_str(ip, sizeof(ip));
    int rssi = get_wifi_rssi();
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    char json[384];
    snprintf(json, sizeof(json),
        "{\"ip\":\"%s\",\"wifi_mode\":\"%s\",\"ssid\":\"%s\","
        "\"rssi\":%d,\"mic_gain\":%d,\"auth_enabled\":%s,"
        "\"running_partition\":\"%s\",\"boot_partition\":\"%s\","
        "\"stream_port\":81,\"audio_port\":82}",
        ip, wifi_ap_active ? "AP" : "STA", stored_ssid, rssi,
        (int)mic_gain, stored_auth_pass[0] ? "true" : "false",
        run ? run->label : "?", boot ? boot->label : "?");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t settings_save_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    char body[256];
    int total_len = req->content_len;
    if (total_len <= 0 || total_len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    int cur_len = 0;
    int received = 0;
    while (cur_len < total_len) {
        received = httpd_req_recv(req, body + cur_len, total_len - cur_len);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        cur_len += received;
    }
    body[cur_len] = '\0';

    char ssid[64] = "";
    char pass[64] = "";
    char admin_pass[64] = "";

    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    httpd_query_key_value(body, "admin_pass", admin_pass, sizeof(admin_pass));

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    saveWiFiCredentials(ssid, pass);
    saveAuthPassword(admin_pass);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Settings Saved</title>"
        "<style>body{font-family:Arial,sans-serif;max-width:480px;margin:40px auto;padding:0 15px;"
        "background:#1a1a2e;color:#e0e0e0;text-align:center;}"
        "</style></head><body>"
        "<h2>WiFi credentials saved!</h2>"
        "<p>The device will reboot in 3 seconds...</p>"
        "</body></html>", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t cmd_handler(httpd_req_t *req)
{
    char *buf = NULL;
    char variable[32];
    char value[32];

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK ||
        httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK) {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    int val = atoi(value);
    ESP_LOGI(TAG, "%s = %d", variable, val);
    sensor_t *s = esp_camera_sensor_get();
    int res = 0;

    if (!strcmp(variable, "framesize")) {
        if (s->pixformat == PIXFORMAT_JPEG) {
            res = s->set_framesize(s, (framesize_t)val);
        }
    }
    else if (!strcmp(variable, "quality"))
        res = s->set_quality(s, val);
    else if (!strcmp(variable, "contrast"))
        res = s->set_contrast(s, val);
    else if (!strcmp(variable, "brightness"))
        res = s->set_brightness(s, val);
    else if (!strcmp(variable, "saturation"))
        res = s->set_saturation(s, val);
    else if (!strcmp(variable, "gainceiling"))
        res = s->set_gainceiling(s, (gainceiling_t)val);
    else if (!strcmp(variable, "colorbar"))
        res = s->set_colorbar(s, val);
    else if (!strcmp(variable, "awb"))
        res = s->set_whitebal(s, val);
    else if (!strcmp(variable, "agc"))
        res = s->set_gain_ctrl(s, val);
    else if (!strcmp(variable, "aec"))
        res = s->set_exposure_ctrl(s, val);
    else if (!strcmp(variable, "hmirror"))
        res = s->set_hmirror(s, val);
    else if (!strcmp(variable, "vflip"))
        res = s->set_vflip(s, val);
    else if (!strcmp(variable, "awb_gain"))
        res = s->set_awb_gain(s, val);
    else if (!strcmp(variable, "agc_gain"))
        res = s->set_agc_gain(s, val);
    else if (!strcmp(variable, "aec_value"))
        res = s->set_aec_value(s, val);
    else if (!strcmp(variable, "aec2"))
        res = s->set_aec2(s, val);
    else if (!strcmp(variable, "dcw"))
        res = s->set_dcw(s, val);
    else if (!strcmp(variable, "bpc"))
        res = s->set_bpc(s, val);
    else if (!strcmp(variable, "wpc"))
        res = s->set_wpc(s, val);
    else if (!strcmp(variable, "raw_gma"))
        res = s->set_raw_gma(s, val);
    else if (!strcmp(variable, "lenc"))
        res = s->set_lenc(s, val);
    else if (!strcmp(variable, "special_effect"))
        res = s->set_special_effect(s, val);
    else if (!strcmp(variable, "wb_mode"))
        res = s->set_wb_mode(s, val);
    else if (!strcmp(variable, "ae_level"))
        res = s->set_ae_level(s, val);
    else if (!strcmp(variable, "led_intensity")) {
        led_duty = val;
        if (isStreaming)
            enable_led(true);
    }
    else if (!strcmp(variable, "mic_gain")) {
        saveMicGain(val);
    }
    else {
        ESP_LOGI(TAG, "Unknown command: %s", variable);
        res = -1;
    }

    if (res < 0) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    static char json_response[1536];

    sensor_t *s = esp_camera_sensor_get();
    char *p = json_response;
    *p++ = '{';

    if (s->id.PID == OV5640_PID || s->id.PID == OV3660_PID) {
        for (int reg = 0x3400; reg < 0x3406; reg += 2) {
            p += print_reg(p, s, reg, 0xFFF);
        }
        p += print_reg(p, s, 0x3406, 0xFF);
        p += print_reg(p, s, 0x3500, 0xFFFF0);
        p += print_reg(p, s, 0x3503, 0xFF);
        p += print_reg(p, s, 0x350a, 0x3FF);
        p += print_reg(p, s, 0x350c, 0xFFFF);

        for (int reg = 0x5480; reg <= 0x5490; reg++) {
            p += print_reg(p, s, reg, 0xFF);
        }
        for (int reg = 0x5380; reg <= 0x538b; reg++) {
            p += print_reg(p, s, reg, 0xFF);
        }
        for (int reg = 0x5580; reg < 0x558a; reg++) {
            p += print_reg(p, s, reg, 0xFF);
        }
        p += print_reg(p, s, 0x558a, 0x1FF);
    } else if (s->id.PID == OV2640_PID) {
        p += print_reg(p, s, 0xd3, 0xFF);
        p += print_reg(p, s, 0x111, 0xFF);
        p += print_reg(p, s, 0x132, 0xFF);
    }

    p += sprintf(p, "\"xclk\":%u,", s->xclk_freq_hz / 1000000);
    p += sprintf(p, "\"pixformat\":%u,", s->pixformat);
    p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
    p += sprintf(p, "\"quality\":%u,", s->status.quality);
    p += sprintf(p, "\"brightness\":%d,", s->status.brightness);
    p += sprintf(p, "\"contrast\":%d,", s->status.contrast);
    p += sprintf(p, "\"saturation\":%d,", s->status.saturation);
    p += sprintf(p, "\"sharpness\":%d,", s->status.sharpness);
    p += sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
    p += sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
    p += sprintf(p, "\"awb\":%u,", s->status.awb);
    p += sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
    p += sprintf(p, "\"aec\":%u,", s->status.aec);
    p += sprintf(p, "\"aec2\":%u,", s->status.aec2);
    p += sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
    p += sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
    p += sprintf(p, "\"agc\":%u,", s->status.agc);
    p += sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
    p += sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
    p += sprintf(p, "\"bpc\":%u,", s->status.bpc);
    p += sprintf(p, "\"wpc\":%u,", s->status.wpc);
    p += sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
    p += sprintf(p, "\"lenc\":%u,", s->status.lenc);
    p += sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
    p += sprintf(p, "\"dcw\":%u,", s->status.dcw);
    p += sprintf(p, "\"colorbar\":%u", s->status.colorbar);
    p += sprintf(p, ",\"led_intensity\":%u", led_duty);
    p += sprintf(p, ",\"mic_gain\":%d", mic_gain);
    *p++ = '}';
    *p++ = 0;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, strlen(json_response));
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    int64_t fr_start = esp_timer_get_time();

    enable_led(true);
    vTaskDelay(pdMS_TO_TICKS(150));
    fb = esp_camera_fb_get();
    enable_led(false);

    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char ts[32];
    snprintf(ts, 32, "%lld.%06ld", (long long)fb->timestamp.tv_sec, (long)fb->timestamp.tv_usec);
    httpd_resp_set_hdr(req, "X-Timestamp", (const char *)ts);

    size_t fb_len = 0;
    if (fb->format == PIXFORMAT_JPEG) {
        fb_len = fb->len;
        res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    } else {
        jpg_chunking_t jchunk = {req, 0};
        res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk) ? ESP_OK : ESP_FAIL;
        httpd_resp_send_chunk(req, NULL, 0);
        fb_len = jchunk.len;
    }
    esp_camera_fb_return(fb);
    int64_t fr_end = esp_timer_get_time();
    ESP_LOGI(TAG, "JPG: %uB %ums", (uint32_t)fb_len, (uint32_t)((fr_end - fr_start) / 1000));
    return res;
}

static esp_err_t bmp_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    int64_t fr_start = esp_timer_get_time();

    fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/x-windows-bmp");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.bmp");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char ts[32];
    snprintf(ts, 32, "%lld.%06ld", (long long)fb->timestamp.tv_sec, (long)fb->timestamp.tv_usec);
    httpd_resp_set_hdr(req, "X-Timestamp", (const char *)ts);

    uint8_t *buf = NULL;
    size_t buf_len = 0;
    bool converted = frame2bmp(fb, &buf, &buf_len);
    esp_camera_fb_return(fb);
    if (!converted) {
        ESP_LOGE(TAG, "BMP Conversion failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    res = httpd_resp_send(req, (const char *)buf, buf_len);
    free(buf);
    int64_t fr_end = esp_timer_get_time();
    ESP_LOGI(TAG, "BMP: %llums, %uB", (uint64_t)((fr_end - fr_start) / 1000), (unsigned)buf_len);
    return res;
}

static esp_err_t xclk_handler(httpd_req_t *req)
{
    char *buf = NULL;
    char _xclk[32];

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_query_key_value(buf, "xclk", _xclk, sizeof(_xclk)) != ESP_OK) {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    int xclk = atoi(_xclk);
    ESP_LOGI(TAG, "Set XCLK: %d MHz", xclk);

    sensor_t *s = esp_camera_sensor_get();
    int res = s->set_xclk(s, LEDC_TIMER_0, xclk);
    if (res) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t reg_handler(httpd_req_t *req)
{
    char *buf = NULL;
    char _reg[32];
    char _mask[32];
    char _val[32];

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_query_key_value(buf, "reg", _reg, sizeof(_reg)) != ESP_OK ||
        httpd_query_key_value(buf, "mask", _mask, sizeof(_mask)) != ESP_OK ||
        httpd_query_key_value(buf, "val", _val, sizeof(_val)) != ESP_OK) {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    int reg = atoi(_reg);
    int mask = atoi(_mask);
    int val = atoi(_val);
    ESP_LOGI(TAG, "Set Register: reg: 0x%02x, mask: 0x%02x, value: 0x%02x", reg, mask, val);

    sensor_t *s = esp_camera_sensor_get();
    int res = s->set_reg(s, reg, mask, val);
    if (res) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t greg_handler(httpd_req_t *req)
{
    char *buf = NULL;
    char _reg[32];
    char _mask[32];

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_query_key_value(buf, "reg", _reg, sizeof(_reg)) != ESP_OK ||
        httpd_query_key_value(buf, "mask", _mask, sizeof(_mask)) != ESP_OK) {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    int reg = atoi(_reg);
    int mask = atoi(_mask);
    sensor_t *s = esp_camera_sensor_get();
    int res = s->get_reg(s, reg, mask);
    if (res < 0) {
        return httpd_resp_send_500(req);
    }
    ESP_LOGI(TAG, "Get Register: reg: 0x%02x, mask: 0x%02x, value: 0x%02x", reg, mask, res);

    char buffer[20];
    snprintf(buffer, sizeof(buffer), "%d", res);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buffer, strlen(buffer));
}

static esp_err_t pll_handler(httpd_req_t *req)
{
    char *buf = NULL;

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }

    int bypass = parse_get_var(buf, "bypass", 0);
    int mul = parse_get_var(buf, "mul", 0);
    int sys = parse_get_var(buf, "sys", 0);
    int root = parse_get_var(buf, "root", 0);
    int pre = parse_get_var(buf, "pre", 0);
    int seld5 = parse_get_var(buf, "seld5", 0);
    int pclken = parse_get_var(buf, "pclken", 0);
    int pclk = parse_get_var(buf, "pclk", 0);
    free(buf);

    ESP_LOGI(TAG, "Set Pll: bypass: %d, mul: %d, sys: %d, root: %d, pre: %d, seld5: %d, pclken: %d, pclk: %d",
             bypass, mul, sys, root, pre, seld5, pclken, pclk);
    sensor_t *s = esp_camera_sensor_get();
    int res = s->set_pll(s, bypass, mul, sys, root, pre, seld5, pclken, pclk);
    if (res) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t win_handler(httpd_req_t *req)
{
    char *buf = NULL;

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }

    int startX = parse_get_var(buf, "sx", 0);
    int startY = parse_get_var(buf, "sy", 0);
    int endX = parse_get_var(buf, "ex", 0);
    int endY = parse_get_var(buf, "ey", 0);
    int offsetX = parse_get_var(buf, "offx", 0);
    int offsetY = parse_get_var(buf, "offy", 0);
    int totalX = parse_get_var(buf, "tx", 0);
    int totalY = parse_get_var(buf, "ty", 0);
    int outputX = parse_get_var(buf, "ox", 0);
    int outputY = parse_get_var(buf, "oy", 0);
    bool scale = parse_get_var(buf, "scale", 0) == 1;
    bool binning = parse_get_var(buf, "binning", 0) == 1;
    free(buf);

    ESP_LOGI(TAG, "Set Window: Start: %d %d, End: %d %d, Offset: %d %d, Total: %d %d, Output: %d %d, Scale: %u, Binning: %u",
             startX, startY, endX, endY, offsetX, offsetY, totalX, totalY, outputX, outputY, scale, binning);
    sensor_t *s = esp_camera_sensor_get();
    int res = s->set_res_raw(s, startX, startY, endX, endY, offsetX, offsetY, totalX, totalY, outputX, outputY, scale, binning);
    if (res) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t update_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    size_t remaining = req->content_len;
    if (remaining == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, remaining, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char buf[1024];
    int received;

    while (remaining > 0) {
        received = httpd_req_recv(req, buf, (remaining < sizeof(buf)) ? remaining : sizeof(buf));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }

        remaining -= received;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update successful, rebooting...");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t switch_boot_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    if (!next) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "No other OTA partition found", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    esp_err_t err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "text/plain");
        char msg[96];
        snprintf(msg, sizeof(msg), "Failed to set boot partition: %s (no valid firmware?)", next->label);
        httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Boot partition switched from %s to %s", running->label, next->label);
    httpd_resp_set_type(req, "text/plain");
    char msg[64];
    snprintf(msg, sizeof(msg), "Next boot: %s (reboot to activate)", next->label);
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------- Server Start ----------

void start_http_ui(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 18;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting UI server on port %d", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start UI server");
        return;
    }

    httpd_uri_t uris[] = {
        { .uri = "/",              .method = HTTP_GET,  .handler = player_handler,        .user_ctx = NULL },
        { .uri = "/settings",      .method = HTTP_GET,  .handler = settings_handler,      .user_ctx = NULL },
        { .uri = "/settings_save", .method = HTTP_POST, .handler = settings_save_handler, .user_ctx = NULL },
        { .uri = "/camera",        .method = HTTP_GET,  .handler = index_handler,         .user_ctx = NULL },
        { .uri = "/control",       .method = HTTP_GET,  .handler = cmd_handler,           .user_ctx = NULL },
        { .uri = "/status",        .method = HTTP_GET,  .handler = status_handler,        .user_ctx = NULL },
        { .uri = "/capture",       .method = HTTP_GET,  .handler = capture_handler,       .user_ctx = NULL },
        { .uri = "/bmp",           .method = HTTP_GET,  .handler = bmp_handler,           .user_ctx = NULL },
        { .uri = "/xclk",          .method = HTTP_GET,  .handler = xclk_handler,          .user_ctx = NULL },
        { .uri = "/reg",           .method = HTTP_GET,  .handler = reg_handler,           .user_ctx = NULL },
        { .uri = "/greg",          .method = HTTP_GET,  .handler = greg_handler,          .user_ctx = NULL },
        { .uri = "/pll",           .method = HTTP_GET,  .handler = pll_handler,           .user_ctx = NULL },
        { .uri = "/resolution",    .method = HTTP_GET,  .handler = win_handler,           .user_ctx = NULL },
        { .uri = "/update",        .method = HTTP_POST, .handler = update_handler,        .user_ctx = NULL },
        { .uri = "/switch_boot",   .method = HTTP_GET,  .handler = switch_boot_handler,   .user_ctx = NULL },
        { .uri = "/api/info",      .method = HTTP_GET,  .handler = api_info_handler,      .user_ctx = NULL },
        { .uri = "/common.css",    .method = HTTP_GET,  .handler = css_handler,           .user_ctx = NULL },
    };

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}
