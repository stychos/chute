#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Persistent settings (globals)
extern volatile int mic_gain;
extern int stored_sample_rate;
extern int stored_wav_bits;
extern char stored_ssid[64];
extern char stored_password[64];
extern char stored_auth_pass[64];
extern char stored_wifi_mode[8];
extern char stored_ap_ssid[32];
extern char stored_ap_password[64];
extern char stored_hostname[32];
extern bool wifi_ap_active;

// NVS
void loadSettings(void);
void saveWiFiCredentials(const char *ssid, const char *password);
void saveMicGain(int gain);
void saveAuthPassword(const char *pass);
void saveWiFiMode(const char *mode);
void saveApSsid(const char *ssid);
void saveApPassword(const char *pass);
void saveHostname(const char *name);
void saveAudioConfig(int sample_rate, int wav_bits);
void saveCameraSetting(const char *var, int val);
void eraseAllSettings(void);

// WiFi
void initWiFi(void);
void wifiReconnectCheck(void);

// Helpers
void get_current_ip_str(char *buf, size_t len);
int get_wifi_rssi(void);
