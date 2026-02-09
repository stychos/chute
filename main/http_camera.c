#include "http_camera.h"
#include "http_ui.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "http_camera";

// ---------- Camera Settings Persistence ----------

void loadCameraSettings(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;

    nvs_handle_t h;
    if (nvs_open("camera", NVS_READONLY, &h) != ESP_OK) return;

    int32_t val;
    if (nvs_get_i32(h, "framesize", &val) == ESP_OK) {
        if (s->pixformat == PIXFORMAT_JPEG) s->set_framesize(s, (framesize_t)val);
    }
    if (nvs_get_i32(h, "quality", &val) == ESP_OK)         s->set_quality(s, val);
    if (nvs_get_i32(h, "brightness", &val) == ESP_OK)      s->set_brightness(s, val);
    if (nvs_get_i32(h, "contrast", &val) == ESP_OK)        s->set_contrast(s, val);
    if (nvs_get_i32(h, "saturation", &val) == ESP_OK)      s->set_saturation(s, val);
    if (nvs_get_i32(h, "sharpness", &val) == ESP_OK)       s->set_sharpness(s, val);
    if (nvs_get_i32(h, "special_effect", &val) == ESP_OK)  s->set_special_effect(s, val);
    if (nvs_get_i32(h, "wb_mode", &val) == ESP_OK)         s->set_wb_mode(s, val);
    if (nvs_get_i32(h, "awb", &val) == ESP_OK)             s->set_whitebal(s, val);
    if (nvs_get_i32(h, "awb_gain", &val) == ESP_OK)        s->set_awb_gain(s, val);
    if (nvs_get_i32(h, "aec", &val) == ESP_OK)             s->set_exposure_ctrl(s, val);
    if (nvs_get_i32(h, "aec2", &val) == ESP_OK)            s->set_aec2(s, val);
    if (nvs_get_i32(h, "ae_level", &val) == ESP_OK)        s->set_ae_level(s, val);
    if (nvs_get_i32(h, "aec_value", &val) == ESP_OK)       s->set_aec_value(s, val);
    if (nvs_get_i32(h, "agc", &val) == ESP_OK)             s->set_gain_ctrl(s, val);
    if (nvs_get_i32(h, "agc_gain", &val) == ESP_OK)        s->set_agc_gain(s, val);
    if (nvs_get_i32(h, "gainceiling", &val) == ESP_OK)     s->set_gainceiling(s, (gainceiling_t)val);
    if (nvs_get_i32(h, "bpc", &val) == ESP_OK)             s->set_bpc(s, val);
    if (nvs_get_i32(h, "wpc", &val) == ESP_OK)             s->set_wpc(s, val);
    if (nvs_get_i32(h, "raw_gma", &val) == ESP_OK)         s->set_raw_gma(s, val);
    if (nvs_get_i32(h, "lenc", &val) == ESP_OK)            s->set_lenc(s, val);
    if (nvs_get_i32(h, "hmirror", &val) == ESP_OK)         s->set_hmirror(s, val);
    if (nvs_get_i32(h, "vflip", &val) == ESP_OK)           s->set_vflip(s, val);
    if (nvs_get_i32(h, "dcw", &val) == ESP_OK)             s->set_dcw(s, val);
    if (nvs_get_i32(h, "led_intensity", &val) == ESP_OK)  led_duty = val;
    if (nvs_get_i32(h, "led_stream", &val) == ESP_OK)    led_stream_enabled = (val != 0);

    nvs_close(h);
    ESP_LOGI(TAG, "Camera settings restored from NVS");
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

static int print_reg(char *p, sensor_t *s, uint16_t reg, uint32_t mask)
{
    return sprintf(p, "\"0x%x\":%u,", reg, s->get_reg(s, reg, mask));
}

// ---------- Handlers ----------

esp_err_t camera_info_handler(httpd_req_t *req)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char json[64];
    snprintf(json, sizeof(json), "{\"pid\":%d}", (int)s->id.PID);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, strlen(json));
}

esp_err_t camera_control_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

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
    if (!s) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
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
    else if (!strcmp(variable, "sharpness"))
        res = s->set_sharpness(s, val);
    else {
        ESP_LOGI(TAG, "Unknown command: %s", variable);
        res = -1;
    }

    if (res < 0) {
        return httpd_resp_send_500(req);
    }

    // Persist setting to NVS
    saveCameraSetting(variable, val);

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t camera_status_handler(httpd_req_t *req)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char json_response[1536];
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
    p += sprintf(p, "\"vflip\":%u,", s->status.vflip);
    p += sprintf(p, "\"dcw\":%u,", s->status.dcw);
    p += sprintf(p, "\"colorbar\":%u", s->status.colorbar);
    *p++ = '}';
    *p++ = 0;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, strlen(json_response));
}

esp_err_t camera_capture_handler(httpd_req_t *req)
{
    if (!esp_camera_sensor_get()) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    int64_t fr_start = esp_timer_get_time();

    if (led_stream_enabled) {
        enable_led(true);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    fb = esp_camera_fb_get();
    if (led_stream_enabled && !led_on) {
        enable_led(false);
    }

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
