#pragma once

#include <stdbool.h>

void start_http_ui(void);
void setupLedFlash(int pin);
void enable_led(bool en);

// Shared LED state (needed by http_video_stream)
extern int led_duty;
extern bool isStreaming;
