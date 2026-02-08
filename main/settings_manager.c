#include "settings_manager.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "settings";

// Globals
volatile int mic_gain = 8;
char stored_ssid[64] = "";
char stored_password[64] = "";
char stored_auth_pass[64] = "";
bool wifi_ap_active = false;

// WiFi event group bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;

// Reconnect state
static int64_t last_reconnect_attempt = 0;
static const int64_t RECONNECT_INTERVAL = 60000; // ms
static const int64_t RECONNECT_TIMEOUT = 10000;  // ms
static bool reconnect_in_progress = false;
static int64_t reconnect_start_time = 0;

static int s_retry_num = 0;
#define MAX_RETRY 10

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry WiFi connection (%d/%d)", s_retry_num, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ---------- NVS Functions ----------

void loadSettings(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("esp32cam", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t len;

        len = sizeof(stored_ssid);
        if (nvs_get_str(handle, "ssid", stored_ssid, &len) != ESP_OK) {
            stored_ssid[0] = '\0';
        }

        len = sizeof(stored_password);
        if (nvs_get_str(handle, "password", stored_password, &len) != ESP_OK) {
            stored_password[0] = '\0';
        }

        len = sizeof(stored_auth_pass);
        if (nvs_get_str(handle, "auth_pass", stored_auth_pass, &len) != ESP_OK) {
            stored_auth_pass[0] = '\0';
        }

        int32_t gain = 8;
        if (nvs_get_i32(handle, "mic_gain", &gain) == ESP_OK) {
            mic_gain = (int)gain;
        }

        nvs_close(handle);
    } else {
        ESP_LOGW(TAG, "NVS open failed (first boot?), using defaults");
    }

    if (mic_gain < 1) mic_gain = 1;
    if (mic_gain > 32) mic_gain = 32;

    ESP_LOGI(TAG, "Settings loaded - SSID: '%s', mic_gain: %d", stored_ssid, mic_gain);
}

void saveWiFiCredentials(const char *ssid, const char *password)
{
    strncpy(stored_ssid, ssid, sizeof(stored_ssid) - 1);
    stored_ssid[sizeof(stored_ssid) - 1] = '\0';
    strncpy(stored_password, password, sizeof(stored_password) - 1);
    stored_password[sizeof(stored_password) - 1] = '\0';

    nvs_handle_t handle;
    if (nvs_open("esp32cam", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, "ssid", stored_ssid);
        nvs_set_str(handle, "password", stored_password);
        nvs_commit(handle);
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "WiFi credentials saved - SSID: '%s'", stored_ssid);
}

void saveMicGain(int gain)
{
    if (gain < 1) gain = 1;
    if (gain > 32) gain = 32;
    mic_gain = gain;

    nvs_handle_t handle;
    if (nvs_open("esp32cam", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_i32(handle, "mic_gain", gain);
        nvs_commit(handle);
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "Mic gain saved: %d", gain);
}

void saveAuthPassword(const char *pass)
{
    strncpy(stored_auth_pass, pass, sizeof(stored_auth_pass) - 1);
    stored_auth_pass[sizeof(stored_auth_pass) - 1] = '\0';

    nvs_handle_t handle;
    if (nvs_open("esp32cam", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, "auth_pass", stored_auth_pass);
        nvs_commit(handle);
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "Auth password %s", strlen(stored_auth_pass) ? "updated" : "cleared");
}

// ---------- WiFi Functions ----------

void initWiFi(void)
{
    loadSettings();

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                    &wifi_event_handler, NULL, &instance_got_ip));

    if (strlen(stored_ssid) == 0) {
        // No credentials — AP only
        ESP_LOGI(TAG, "No WiFi credentials found, starting AP mode...");
        wifi_config_t ap_config = {
            .ap = {
                .ssid = "ESP32-CAM-Setup",
                .ssid_len = 0,
                .password = "",
                .max_connection = 4,
                .authmode = WIFI_AUTH_OPEN,
            },
        };
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_ap_active = true;

        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(ap_netif, &ip_info);
        ESP_LOGI(TAG, "AP IP address: " IPSTR, IP2STR(&ip_info.ip));
        return;
    }

    // Try STA connection
    ESP_LOGI(TAG, "Connecting to WiFi '%s'...", stored_ssid);

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, stored_ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, stored_password, sizeof(sta_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for connection or failure (up to 60s)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                        pdFALSE, pdFALSE,
                        pdMS_TO_TICKS(60000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        esp_wifi_set_ps(WIFI_PS_NONE);
        wifi_ap_active = false;
    } else {
        // Fallback to AP+STA
        ESP_LOGW(TAG, "WiFi connection failed, starting AP+STA fallback...");

        wifi_config_t ap_config = {
            .ap = {
                .ssid = "ESP32-CAM-Setup",
                .ssid_len = 0,
                .password = "",
                .max_connection = 4,
                .authmode = WIFI_AUTH_OPEN,
            },
        };
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
        wifi_ap_active = true;
        s_retry_num = 0; // Reset for reconnect attempts

        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(ap_netif, &ip_info);
        ESP_LOGI(TAG, "AP IP address: " IPSTR, IP2STR(&ip_info.ip));
        last_reconnect_attempt = esp_timer_get_time() / 1000;
    }
}

void wifiReconnectCheck(void)
{
    if (!wifi_ap_active) return;
    if (strlen(stored_ssid) == 0) return;

    int64_t now = esp_timer_get_time() / 1000; // ms

    // Check if STA connected while in APSTA mode
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi reconnected, stopping AP...");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        wifi_ap_active = false;
        reconnect_in_progress = false;
        char ip_str[16];
        get_current_ip_str(ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "IP address: %s", ip_str);
        return;
    }

    if (reconnect_in_progress) {
        if (now - reconnect_start_time >= RECONNECT_TIMEOUT) {
            ESP_LOGI(TAG, "Reconnect timed out, will retry...");
            reconnect_in_progress = false;
            last_reconnect_attempt = now;
        }
        return;
    }

    if (now - last_reconnect_attempt < RECONNECT_INTERVAL) return;

    ESP_LOGI(TAG, "Attempting WiFi reconnect to '%s'...", stored_ssid);
    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_connect();
    reconnect_in_progress = true;
    reconnect_start_time = now;
}

// ---------- Helpers ----------

void get_current_ip_str(char *buf, size_t len)
{
    esp_netif_ip_info_t ip_info;
    if (!wifi_ap_active && sta_netif) {
        esp_netif_get_ip_info(sta_netif, &ip_info);
    } else if (ap_netif) {
        esp_netif_get_ip_info(ap_netif, &ip_info);
    } else {
        snprintf(buf, len, "0.0.0.0");
        return;
    }
    snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
}

int get_wifi_rssi(void)
{
    if (wifi_ap_active) return 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}
