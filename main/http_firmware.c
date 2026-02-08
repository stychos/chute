#include "http_firmware.h"
#include "http_ui.h"

#include <string.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "http_firmware";

// ---------- Helpers ----------

static bool is_firmware_image(const uint8_t *buf, int len)
{
    if (len < 8) return false;
    // ESP image magic byte
    if (buf[0] != 0xE9) return false;
    // Valid segment count (1-16)
    if (buf[1] < 1 || buf[1] > 16) return false;
    // Entry point in 0x40xxxxxx range (ESP32 IRAM/DRAM)
    uint32_t entry = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (buf[7] << 24);
    if ((entry & 0xFF000000) != 0x40000000) return false;
    return true;
}

// ---------- Handlers ----------

esp_err_t firmware_upload_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    size_t content_len = req->content_len;
    if (content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }

    // Read first chunk to detect file type
    char buf[1024];
    size_t remaining = content_len;
    int received = 0;

    // Need at least 8 bytes for detection
    while (received < 8 && remaining > 0) {
        int r = httpd_req_recv(req, buf + received, (remaining < sizeof(buf) - received) ? remaining : sizeof(buf) - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        received += r;
        remaining -= r;
    }

    bool firmware = is_firmware_image((const uint8_t *)buf, received);
    ESP_LOGI(TAG, "OTA upload: %u bytes, detected as %s (first bytes: %02x %02x %02x %02x %02x %02x %02x %02x)",
             (unsigned)content_len, firmware ? "firmware" : "spiffs",
             received > 0 ? (uint8_t)buf[0] : 0, received > 1 ? (uint8_t)buf[1] : 0,
             received > 2 ? (uint8_t)buf[2] : 0, received > 3 ? (uint8_t)buf[3] : 0,
             received > 4 ? (uint8_t)buf[4] : 0, received > 5 ? (uint8_t)buf[5] : 0,
             received > 6 ? (uint8_t)buf[6] : 0, received > 7 ? (uint8_t)buf[7] : 0);

    esp_err_t err;

    if (firmware) {
        // --- Firmware OTA path ---
        const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
        if (!update_partition) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
            return ESP_FAIL;
        }
        if (content_len > update_partition->size) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File too large for OTA partition");
            return ESP_FAIL;
        }

        esp_ota_handle_t ota_handle;
        err = esp_ota_begin(update_partition, content_len, &ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
            return ESP_FAIL;
        }

        // Write the first chunk we already read
        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }

        // Stream remaining data
        while (remaining > 0) {
            int r = httpd_req_recv(req, buf, (remaining < sizeof(buf)) ? remaining : sizeof(buf));
            if (r <= 0) {
                if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
                return ESP_FAIL;
            }
            err = esp_ota_write(ota_handle, buf, r);
            if (err != ESP_OK) {
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
                return ESP_FAIL;
            }
            remaining -= r;
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

        ESP_LOGI(TAG, "Firmware OTA update successful, rebooting...");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send(req, "{\"ok\":true,\"type\":\"firmware\"}", HTTPD_RESP_USE_STRLEN);

        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return ESP_OK;

    } else {
        // --- SPIFFS image path ---
        const esp_partition_t *spiffs_part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
        if (!spiffs_part) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No SPIFFS partition");
            return ESP_FAIL;
        }
        if (content_len > spiffs_part->size) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File too large for SPIFFS partition");
            return ESP_FAIL;
        }

        // Unmount SPIFFS so we can write raw
        esp_vfs_spiffs_unregister("spiffs");

        // Erase SPIFFS partition in 64KB chunks to avoid triggering task WDT
        {
            const size_t erase_block = 0x10000; // 64KB
            for (size_t off = 0; off < spiffs_part->size; off += erase_block) {
                size_t len = spiffs_part->size - off;
                if (len > erase_block) len = erase_block;
                err = esp_partition_erase_range(spiffs_part, off, len);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "SPIFFS erase failed at 0x%x: %s", (unsigned)off, esp_err_to_name(err));
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erase failed");
                    return ESP_FAIL;
                }
                vTaskDelay(1); // yield to feed watchdog
            }
        }

        // Write the first chunk we already read
        size_t offset = 0;
        err = esp_partition_write(spiffs_part, offset, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS write failed at offset %u: %s", (unsigned)offset, esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }
        offset += received;

        // Stream remaining data
        while (remaining > 0) {
            int r = httpd_req_recv(req, buf, (remaining < sizeof(buf)) ? remaining : sizeof(buf));
            if (r <= 0) {
                if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
                return ESP_FAIL;
            }
            err = esp_partition_write(spiffs_part, offset, buf, r);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "SPIFFS write failed at offset %u: %s", (unsigned)offset, esp_err_to_name(err));
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
                return ESP_FAIL;
            }
            offset += r;
            remaining -= r;
        }

        // Remount SPIFFS
        esp_vfs_spiffs_conf_t spiffs_conf = {
            .base_path = "/www",
            .partition_label = "spiffs",
            .max_files = 3,
            .format_if_mount_failed = false,
        };
        err = esp_vfs_spiffs_register(&spiffs_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS remount failed: %s", esp_err_to_name(err));
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, "{\"ok\":false,\"type\":\"spiffs\",\"error\":\"Remount failed — invalid SPIFFS image?\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }

        // Validate
        size_t total = 0, used = 0;
        esp_spiffs_info("spiffs", &total, &used);
        ESP_LOGI(TAG, "SPIFFS update successful: %u/%u bytes used", (unsigned)used, (unsigned)total);

        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send(req, "{\"ok\":true,\"type\":\"spiffs\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
}

esp_err_t firmware_boot_handler(httpd_req_t *req)
{
    if (!check_auth(req)) return send_auth_required(req);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    if (!next) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "{\"message\":\"No other OTA partition found\"}", HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        char json[128];
        snprintf(json, sizeof(json), "{\"message\":\"Failed to set boot partition: %s\"}", next->label);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, json, strlen(json));
    }

    ESP_LOGI(TAG, "Boot partition switched from %s to %s", running->label, next->label);
    char json[128];
    snprintf(json, sizeof(json), "{\"message\":\"Next boot: %s (reboot to activate)\"}", next->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, strlen(json));
}
