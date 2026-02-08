#pragma once

#include "esp_http_server.h"

esp_err_t firmware_upload_handler(httpd_req_t *req);
esp_err_t firmware_boot_handler(httpd_req_t *req);
