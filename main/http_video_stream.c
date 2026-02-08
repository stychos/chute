#include "http_video_stream.h"
#include "http_ui.h"

#include <string.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "http_video";

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %lld.%06ld\r\n\r\n";

// Running average filter for frame rate logging
typedef struct {
    size_t size;
    size_t index;
    size_t count;
    int sum;
    int *values;
} ra_filter_t;

static ra_filter_t ra_filter;

static ra_filter_t *ra_filter_init(ra_filter_t *filter, size_t sample_size)
{
    memset(filter, 0, sizeof(ra_filter_t));
    filter->values = (int *)malloc(sample_size * sizeof(int));
    if (!filter->values) {
        return NULL;
    }
    memset(filter->values, 0, sample_size * sizeof(int));
    filter->size = sample_size;
    return filter;
}

static int ra_filter_run(ra_filter_t *filter, int value)
{
    if (!filter->values) {
        return value;
    }
    filter->sum -= filter->values[filter->index];
    filter->values[filter->index] = value;
    filter->sum += filter->values[filter->index];
    filter->index++;
    filter->index = filter->index % filter->size;
    if (filter->count < filter->size) {
        filter->count++;
    }
    return filter->sum / filter->count;
}

// Async streaming state
static volatile bool s_stream_stop = false;
static volatile TaskHandle_t s_stream_task = NULL;

static void video_stream_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    camera_fb_t *fb = NULL;
    struct timeval _timestamp;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char part_buf[128];

    int64_t last_frame = esp_timer_get_time();

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        goto cleanup;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "60");
    httpd_resp_set_hdr(req, "Accept-Ranges", "none");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");

    isStreaming = true;
    enable_led(true);

    while (!s_stream_stop) {
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
        } else {
            _timestamp.tv_sec = fb->timestamp.tv_sec;
            _timestamp.tv_usec = fb->timestamp.tv_usec;
            if (fb->format != PIXFORMAT_JPEG) {
                bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
                esp_camera_fb_return(fb);
                fb = NULL;
                if (!jpeg_converted) {
                    ESP_LOGE(TAG, "JPEG compression failed");
                    res = ESP_FAIL;
                }
            } else {
                _jpg_buf_len = fb->len;
                _jpg_buf = fb->buf;
            }
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if (res == ESP_OK) {
            size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART,
                                   _jpg_buf_len, (long long)_timestamp.tv_sec,
                                   (long)_timestamp.tv_usec);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if (fb) {
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        } else if (_jpg_buf) {
            free(_jpg_buf);
            _jpg_buf = NULL;
        }
        if (res != ESP_OK) {
            ESP_LOGI(TAG, "Stream send failed, client disconnected");
            break;
        }
        int64_t fr_end = esp_timer_get_time();
        int64_t frame_time = fr_end - last_frame;
        last_frame = fr_end;
        frame_time /= 1000;
        uint32_t avg_frame_time = ra_filter_run(&ra_filter, frame_time);
        ESP_LOGD(TAG, "MJPG: %uB %ums (%.1ffps), AVG: %ums (%.1ffps)",
                 (uint32_t)_jpg_buf_len,
                 (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time,
                 avg_frame_time, 1000.0 / avg_frame_time);
    }

cleanup:
    isStreaming = false;
    enable_led(false);
    httpd_req_async_handler_complete(req);
    ESP_LOGI(TAG, "Video stream ended");
    s_stream_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    // Stop any existing stream task
    if (s_stream_task) {
        ESP_LOGI(TAG, "Stopping previous stream");
        s_stream_stop = true;
        for (int i = 0; i < 30 && s_stream_task; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_stream_task) {
            ESP_LOGW(TAG, "Previous stream task did not stop in time");
        }
    }

    s_stream_stop = false;

    // Create async copy of the request — frees the httpd server task
    httpd_req_t *async_req = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_req_async_handler_begin failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // Spawn streaming in a dedicated FreeRTOS task
    BaseType_t ret = xTaskCreate(video_stream_task, "vid_stream", 4096,
                                 async_req, 5, (TaskHandle_t *)&s_stream_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create video stream task");
        httpd_req_async_handler_complete(async_req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Video stream started (async)");
    return ESP_OK;
}

void start_http_video_stream(void)
{
    ra_filter_init(&ra_filter, 20);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;
    config.ctrl_port = 32769;
    config.max_open_sockets = 2;
    config.lru_purge_enable = true;
    config.send_wait_timeout = 2;
    config.recv_wait_timeout = 2;

    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting video stream server on port %d", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start video stream server");
        return;
    }

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &stream_uri);
}
