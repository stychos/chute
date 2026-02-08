#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Persistent settings (globals)
extern volatile int mic_gain;
extern char stored_ssid[64];
extern char stored_password[64];
extern char stored_auth_pass[64];
extern bool wifi_ap_active;

// NVS
void loadSettings(void);
void saveWiFiCredentials(const char *ssid, const char *password);
void saveMicGain(int gain);
void saveAuthPassword(const char *pass);

// WiFi
void initWiFi(void);
void wifiReconnectCheck(void);

// Helpers
void get_current_ip_str(char *buf, size_t len);
int get_wifi_rssi(void);
