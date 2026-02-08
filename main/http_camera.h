#pragma once

#include "esp_http_server.h"

void loadCameraSettings(void);

esp_err_t camera_info_handler(httpd_req_t *req);
esp_err_t camera_status_handler(httpd_req_t *req);
esp_err_t camera_control_handler(httpd_req_t *req);
esp_err_t camera_capture_handler(httpd_req_t *req);
