#pragma once
#include <Preferences.h>
#include <WiFi.h>

// Persistent settings
volatile int mic_gain = 8;
char stored_ssid[64] = "";
char stored_password[64] = "";
char stored_auth_pass[64] = "";
bool wifi_ap_active = false;

static Preferences prefs;
static unsigned long last_reconnect_attempt = 0;
static const unsigned long RECONNECT_INTERVAL = 60000; // 1 minute
static const unsigned long RECONNECT_TIMEOUT = 10000;  // 10 seconds
static bool reconnect_in_progress = false;
static unsigned long reconnect_start_time = 0;

void loadSettings() {
    prefs.begin("esp32cam", true); // read-only
    String s = prefs.getString("ssid", "");
    String p = prefs.getString("password", "");
    String a = prefs.getString("auth_pass", "");
    mic_gain = prefs.getInt("mic_gain", 8);
    prefs.end();

    strncpy(stored_ssid, s.c_str(), sizeof(stored_ssid) - 1);
    stored_ssid[sizeof(stored_ssid) - 1] = '\0';
    strncpy(stored_password, p.c_str(), sizeof(stored_password) - 1);
    stored_password[sizeof(stored_password) - 1] = '\0';
    strncpy(stored_auth_pass, a.c_str(), sizeof(stored_auth_pass) - 1);
    stored_auth_pass[sizeof(stored_auth_pass) - 1] = '\0';

    if (mic_gain < 1) mic_gain = 1;
    if (mic_gain > 32) mic_gain = 32;

    Serial.printf("Settings loaded - SSID: '%s', mic_gain: %d\n", stored_ssid, mic_gain);
}

void saveWiFiCredentials(const char* ssid, const char* password) {
    strncpy(stored_ssid, ssid, sizeof(stored_ssid) - 1);
    stored_ssid[sizeof(stored_ssid) - 1] = '\0';
    strncpy(stored_password, password, sizeof(stored_password) - 1);
    stored_password[sizeof(stored_password) - 1] = '\0';

    prefs.begin("esp32cam", false);
    prefs.putString("ssid", stored_ssid);
    prefs.putString("password", stored_password);
    prefs.end();

    Serial.printf("WiFi credentials saved - SSID: '%s'\n", stored_ssid);
}

void saveMicGain(int gain) {
    if (gain < 1) gain = 1;
    if (gain > 32) gain = 32;
    mic_gain = gain;

    prefs.begin("esp32cam", false);
    prefs.putInt("mic_gain", gain);
    prefs.end();

    Serial.printf("Mic gain saved: %d\n", gain);
}

void saveAuthPassword(const char* pass) {
    strncpy(stored_auth_pass, pass, sizeof(stored_auth_pass) - 1);
    stored_auth_pass[sizeof(stored_auth_pass) - 1] = '\0';

    prefs.begin("esp32cam", false);
    prefs.putString("auth_pass", stored_auth_pass);
    prefs.end();

    Serial.printf("Auth password %s\n", strlen(stored_auth_pass) ? "updated" : "cleared");
}

void initWiFi() {
    loadSettings();

    if (strlen(stored_ssid) == 0) {
        // No credentials saved - start AP only
        Serial.println("No WiFi credentials found, starting AP mode...");
        WiFi.mode(WIFI_AP);
        WiFi.softAP("ESP32-CAM-Setup");
        wifi_ap_active = true;
        delay(100);
        Serial.print("AP IP address: ");
        Serial.println(WiFi.softAPIP());
        return;
    }

    // Try connecting with saved credentials
    Serial.printf("Connecting to WiFi '%s'...\n", stored_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(stored_ssid, stored_password);
    WiFi.setSleep(false);

    // Wait up to 60 seconds
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 120) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
        wifi_ap_active = false;
    } else {
        // Connection failed - start AP+STA fallback
        Serial.println("WiFi connection failed, starting AP+STA fallback...");
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("ESP32-CAM-Setup");
        wifi_ap_active = true;
        delay(100);
        Serial.print("AP IP address: ");
        Serial.println(WiFi.softAPIP());
        last_reconnect_attempt = millis();
    }
}

void wifiReconnectCheck() {
    if (!wifi_ap_active) return;
    if (strlen(stored_ssid) == 0) return;

    unsigned long now = millis();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi reconnected, stopping AP...");
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        wifi_ap_active = false;
        reconnect_in_progress = false;
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
        return;
    }

    if (reconnect_in_progress) {
        // Check timeout
        if (now - reconnect_start_time >= RECONNECT_TIMEOUT) {
            Serial.println("Reconnect timed out, will retry...");
            reconnect_in_progress = false;
            last_reconnect_attempt = now;
        }
        return;
    }

    // Start a new attempt every RECONNECT_INTERVAL
    if (now - last_reconnect_attempt < RECONNECT_INTERVAL) return;

    Serial.printf("Attempting WiFi reconnect to '%s'...\n", stored_ssid);
    WiFi.begin(stored_ssid, stored_password);
    reconnect_in_progress = true;
    reconnect_start_time = now;
}
