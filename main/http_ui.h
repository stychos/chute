#pragma once

#include <stdbool.h>
#include "esp_http_server.h"

void start_http_ui(void);
void setupLedFlash(int pin);
void enable_led(bool en);

// Auth/CORS helpers (shared with http_camera.c, http_firmware.c)
bool check_auth(httpd_req_t *req);
esp_err_t send_auth_required(httpd_req_t *req);
esp_err_t cors_handler(httpd_req_t *req);

// Shared LED state (needed by http_video_stream)
extern int led_duty;
extern bool led_on;
extern bool led_stream_enabled;
extern bool isStreaming;

// Hardware availability (set during init in main.c / http_audio_stream.c)
extern bool camera_available;
extern bool mic_available;
