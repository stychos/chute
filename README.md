# Chute

MJPEG video + WAV audio streaming webserver for ESP32-CAM boards.
- Pure ESP-IDF, no Arduino dependencies
- Modern UI for playback and configuration
- Ability to protect configuration with the password
- Ability to fallback to AP mode when failed to connect to WiFi
- OTA firmware updates

## Supported Hardware

Theoretically, any ESP with I2S pins available, OV camera and I2S mic.
Tested on AI-Thinker ESP32-CAM + OV3660 + INMP441.

## Microphone Wiring (AI-Thinker + INMP441)

The AI-Thinker ESP32-CAM has no built-in microphone. Connect an INMP441 I2S MEMS mic as follows:

| INMP441 Pin | ESP32-CAM Pin | Notes |
|-------------|---------------|-------|
| VDD | 3V3 | Power supply |
| GND | GND | Ground |
| SD | GPIO15 | Serial data |
| SCK | GPIO14 | Serial clock |
| WS | GPIO2 | Word select (left/right clock) |
| L/R | GND | Tie to GND for left channel |

The INMP441 runs on I2S port 1 (port 0 is used internally by the camera).

## Prerequisites

- **[ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/)** (tested with v5.5.2) — the official Espressif IoT Development Framework. Follow the installation guide for your OS, then source the environment before running any build commands:
  ```bash
  source ~/path-to-esp-idf/export.sh
  ```
- **CMake 3.16+** — included with ESP-IDF, but also required if building standalone
- **Node.js 18+** and **npm** — for building the frontend (Vue 3 + Vite)

## Build & Flash

```bash
# 1. Build the frontend (outputs to spiffs_data/)
cd frontend && npm install && npm run build && cd ..

# 2. Set chip target (once per project, or after switching boards)
idf.py set-target esp32        # AI-Thinker

# 3. Build firmware + SPIFFS image
idf.py build

# 4. Flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

After changing the partition table or switching targets, do a full erase first:

```bash
idf.py -p /dev/ttyUSB0 erase-flash
```

### Switching Boards

The default target is AI-Thinker ESP32-CAM. To build for XIAO ESP32S3 Sense:

1. **Camera model** in `main/CMakeLists.txt`:
   ```cmake
   target_compile_definitions(${COMPONENT_LIB} PRIVATE CAMERA_MODEL_XIAO_ESP32S3)
   ```

2. **I2S config** in `main/http_audio_stream.h` — swap the active defines:

   | Define | AI-Thinker | XIAO ESP32S3 |
   |--------|-----------|--------------|
   | `I2S_MIC_WS` | 2 | 42 |
   | `I2S_MIC_SCK` | 14 | -1 |
   | `I2S_MIC_SD` | 15 | 41 |
   | `I2S_MIC_PORT` | 1 | 0 |
   | `SAMPLE_BITS` | 32 | 16 |

3. **I2S mode** in `main/http_audio_stream.c` — use `driver/i2s_pdm.h` and `i2s_pdm_rx_config_t` for XIAO instead of I2S standard mode.

## First-Time Setup

On first boot (no saved WiFi credentials), the device starts in **AP mode**:

- SSID: `Chute-Setup` (open, no password)
- IP: `192.168.4.1`

Connect to the AP, open `http://192.168.4.1`, configure your WiFi credentials, and save. The device reboots and connects to your network.

In Auto mode, the device tries to connect to the saved network for 3 minutes. If it fails, it switches to AP mode with no further reconnection attempts — reboot to retry.

## Web Interface

| URL | Description |
|-----|-------------|
| `http://<ip>/` | Player (video + audio playback, settings panel) |
| `http://<ip>/#/config` | Settings panel (camera, audio, WiFi, firmware) |
| `http://<ip>:81/stream` | Raw MJPEG video stream |
| `http://<ip>:82/audio` | Raw WAV audio stream |

### Player

When idle, the player shows a live camera snapshot (refreshed every 30 seconds). Click to start streaming.

![Idle player with snapshot preview](/img/pause.png)

During playback, hover to reveal the pause overlay and volume slider.

![Active playback with pause overlay](/img/playback.png)

### Settings Panel

Click the gear icon to open the settings panel. It slides over the player so the stream keeps running while you adjust settings. If a password is set, authentication is required.

![Authentication prompt](/img/config-auth.png)

The panel has tabs for each section:

**Status** — Network info, system stats (heap, PSRAM, SPIFFS), camera/audio details, and partition info.

![Status tab](/img/config-status.png)

**WiFi** — SSID, password, connection mode (Auto/STA/AP), hostname, and WiFi network scan.

![WiFi tab](/img/config-wifi.png)

**Audio** — Microphone gain, sample rate, and WAV bit depth. Changes restart the audio stream automatically.

![Audio tab](/img/config-audio.png)

**Camera** — Resolution, JPEG quality, brightness, contrast, saturation, and auto-controls (AWB, AEC, AGC). Sensor-adaptive — controls adjust based on detected sensor (OV2640, OV3660, OV5640).

![Camera tab](/img/config-camera.png)

**Flash** — LED flash intensity control with option to enable the LED during streaming.

![Flash tab](/img/config-led-flash.png)

**Password** — Optional password to protect the settings panel. Leave empty to disable. Password is per-session — closing the browser tab requires re-authentication.

![Password tab](/img/config-password.png)

**Firmware** — OTA firmware upload, boot partition switching, device reboot, and factory reset.

![Firmware tab](/img/config-firmware.png)

## go2rtc Integration

For low-latency combined playback or integration with other systems, use [go2rtc](https://github.com/AlexxIT/go2rtc) as a relay:

```yaml
streams:
  chute:
    - ffmpeg:http://192.168.1.42:81/stream#video=h264
    - ffmpeg:http://192.168.1.42:82/audio#audio=opus
```

### Home Assistant

Add a Webpage card pointing at your go2rtc stream:

```yaml
type: iframe
url: https://go2rtc.local/stream.html?src=chute
aspect_ratio: 100%
```
## Architecture

Three HTTP servers on separate ports avoid connection limits and allow independent streaming:

| Port | Module | Purpose |
|------|--------|---------|
| 80 | `http_ui.c` | SPA + JSON API (camera control, settings, OTA) |
| 81 | `http_video_stream.c` | MJPEG stream (async FreeRTOS task per client) |
| 82 | `http_audio_stream.c` | WAV audio stream (I2S mic capture) |

Video and audio handlers use `httpd_req_async_handler_begin()` to spawn dedicated FreeRTOS tasks, freeing HTTP server threads during long-running streams.

### Partition Table

| Name | Type | Offset | Size |
|------|------|--------|------|
| nvs | NVS | 0x9000 | 20 KB |
| otadata | OTA data | 0xE000 | 8 KB |
| app0 | OTA_0 | 0x10000 | 1664 KB |
| app1 | OTA_1 | 0x1B0000 | 1664 KB |
| spiffs | SPIFFS | 0x350000 | 704 KB |

Dual OTA partitions enable safe firmware updates via the web UI.
