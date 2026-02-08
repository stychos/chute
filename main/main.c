#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_psram.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "esp_ota_ops.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_pins.h"
#include "settings_manager.h"
#include "http_ui.h"
#include "http_camera.h"
#include "http_video_stream.h"
#include "http_audio_stream.h"

static const char *TAG = "main";

void app_main(void)
{
    // Chip info
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU core(s), %s%s%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return;
    }

    printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());

    // 0. Mark OTA partition as valid (prevents rollback on crash)
    esp_ota_mark_app_valid_cancel_rollback();

    // 1. NVS init (with erase-and-retry on corruption)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition corrupted, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. SPIFFS init
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/www",
        .partition_label = "spiffs",
        .max_files = 3,
        .format_if_mount_failed = false,
    };
    esp_err_t ret_spiffs = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret_spiffs != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS init failed: %s", esp_err_to_name(ret_spiffs));
    } else {
        size_t total = 0, used = 0;
        esp_spiffs_info("spiffs", &total, &used);
        ESP_LOGI(TAG, "SPIFFS: %d/%d bytes used", (int)used, (int)total);
    }

    // 3. Camera configuration
    camera_config_t config = {0};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.frame_size = FRAMESIZE_UXGA;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 10;
    config.fb_count = 2;

    if (esp_psram_is_initialized()) {
        config.jpeg_quality = 10;
        config.fb_count = 2;
        config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
        config.frame_size = FRAMESIZE_SVGA;
        config.fb_location = CAMERA_FB_IN_DRAM;
    }

    // 3. GPIO for ESP_EYE
#if defined(CAMERA_MODEL_ESP_EYE)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << 13) | (1ULL << 14),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
#endif

    // 4. Camera init
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x (continuing without camera)", err);
    } else {
        camera_available = true;
        // 5. Sensor defaults
        sensor_t *s = esp_camera_sensor_get();
        if (s->id.PID == OV3660_PID) {
            s->set_vflip(s, 1);
            s->set_brightness(s, 1);
            s->set_saturation(s, -2);
        }
        if (config.pixel_format == PIXFORMAT_JPEG) {
            s->set_framesize(s, FRAMESIZE_VGA);
        }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
        s->set_vflip(s, 1);
        s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
        s->set_vflip(s, 1);
#endif

        // 6. Restore user camera settings from NVS (overrides defaults above)
        loadCameraSettings();
    }

    // 6. LED flash
#if defined(LED_GPIO_NUM)
    setupLedFlash(LED_GPIO_NUM);
#endif

    // 7. WiFi
    initWiFi();

    // 8-10. Start HTTP servers
    start_http_ui();           // port 80
    start_http_video_stream(); // port 81
    start_http_audio_stream(); // port 82

    char ip_str[16];
    get_current_ip_str(ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, "Camera Ready! Use 'http://%s' to connect", ip_str);

    // Main loop — all servers are async, only need WiFi reconnect check
    while (1) {
        wifiReconnectCheck();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
