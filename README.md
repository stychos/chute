# ESP32-CAM Audio

MJPEG video + WAV audio streaming webserver for ESP32-CAM boards. Built with ESP-IDF (no Arduino dependencies).

The firmware runs three HTTP servers on separate ports, streams video and audio independently, and includes a web UI for playback, camera settings, WiFi configuration, and OTA firmware updates.

## Supported Hardware

| Board | Camera | Microphone |
|-------|--------|------------|
| AI-Thinker ESP32-CAM | OV2640, OV3660, OV5640 | INMP441 (external, I2S) |
| XIAO ESP32S3 | OV2640, OV3660, OV5640 | Integrated PDM mic |

## Wiring (AI-Thinker + INMP441)

![Wiring diagram](/img/image-4.png)

| INMP441 | ESP32-CAM |
|---------|-----------|
| GND | GND |
| VDD | VCC (3V3) |
| SD | GPIO15 |
| SCK | GPIO14 |
| WS | GPIO2 |
| L/R | GND |

## Build & Flash

Requires [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/) (tested with v5.5.2).

```bash
# Set chip target (once)
idf.py set-target esp32        # AI-Thinker
idf.py set-target esp32s3      # XIAO

# Build and flash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

After changing the partition table or switching targets, do a full erase first:

```bash
idf.py -p /dev/ttyUSB0 erase-flash
```

### Switching Boards

The default target is AI-Thinker. To build for XIAO ESP32S3, change both the camera model and I2S config:

1. **Camera model** in `main/CMakeLists.txt`:
   ```cmake
   target_compile_definitions(${COMPONENT_LIB} PRIVATE CAMERA_MODEL_XIAO_ESP32S3)
   ```

2. **I2S pins** in `main/http_audio_stream.h` — swap the active defines:

   | Define | AI-Thinker | XIAO ESP32S3 |
   |--------|-----------|--------------|
   | `I2S_MIC_WS` | 2 | 42 |
   | `I2S_MIC_SCK` | 14 | -1 |
   | `I2S_MIC_SD` | 15 | 41 |
   | `I2S_MIC_PORT` | 1 | 0 |
   | `SAMPLE_BITS` | 32 | 16 |

3. **I2S mode** in `main/http_audio_stream.c` — add `I2S_MODE_PDM` for XIAO.

## First-Time Setup

On first boot (no saved WiFi credentials), the device starts in **AP mode**:

- SSID: `ESP32-CAM-Setup` (open, no password)
- IP: `192.168.4.1`

Connect to the AP, open `http://192.168.4.1/settings`, enter your WiFi credentials, and save. The device reboots and connects to your network.

If the saved network is unreachable (60s timeout), the device falls back to **AP+STA mode** — it serves the AP while retrying the saved network every 60 seconds.

## Web Interface

| URL | Description |
|-----|-------------|
| `http://<ip>/` | Player page (video, audio, or combined playback) |
| `http://<ip>/settings` | WiFi config, mic gain, OTA update, boot partition |
| `http://<ip>/camera` | Camera sensor settings (auto-selects by sensor) |
| `http://<ip>:81/stream` | Raw MJPEG video stream |
| `http://<ip>:82/audio` | Raw WAV audio stream |

### Settings Password

An optional password can protect the Settings, Camera, OTA, and Boot Partition pages. Set it on the Settings page. Leave blank to disable. Authentication uses HTTP Basic Auth.

## HTTP API

All endpoints are on port 80 unless noted.

| Endpoint | Method | Auth | Description |
|----------|--------|------|-------------|
| `/` | GET | No | Player page |
| `/settings` | GET | Yes | Settings page |
| `/settings_save` | POST | Yes | Save WiFi/auth credentials, reboot |
| `/camera` | GET | Yes | Camera settings (sensor-specific) |
| `/api/info` | GET | No | JSON status (IP, WiFi mode, SSID, RSSI, mic gain, partitions, ports) |
| `/control` | GET | No | Set camera or mic params (`?var=X&val=Y`) |
| `/status` | GET | No | JSON dump of all camera register values |
| `/capture` | GET | No | Single JPEG frame |
| `/bmp` | GET | No | Single BMP frame |
| `/xclk` | GET | No | Set sensor clock (`?xclk=20`) |
| `/reg` | GET | No | Set sensor register (`?reg=X&mask=Y&val=Z`) |
| `/greg` | GET | No | Read sensor register (`?reg=X&mask=Y`) |
| `/pll` | GET | No | Set sensor PLL params |
| `/resolution` | GET | No | Set raw sensor window/resolution |
| `/update` | POST | Yes | OTA firmware upload |
| `/switch_boot` | GET | Yes | Toggle boot partition (app0/app1) |
| `:81/stream` | GET | No | MJPEG video stream |
| `:82/audio` | GET | No | WAV audio stream (16-bit, 22050 Hz, mono) |

## go2rtc Integration

The browser's audio buffer adds noticeable delay when playing video + audio together directly. For low-latency combined playback, use [go2rtc](https://github.com/AlexxIT/go2rtc) as a relay.

### Option 1: Separate streams with custom HTML page

Add the streams to your `go2rtc.yaml`:

```yaml
streams:
  ESP32-CAM_video: http://192.168.10.55:81/stream
  ESP32-CAM_audio: ffmpeg:http://192.168.10.55:82/audio#audio=opus
```

![go2rtc config](/img/image-1.png)

Then use `ESP32CAM_stream.html` (included in the repo root) which combines video and audio via iframes. Edit the file to point at your go2rtc instance:

- Video: `https://go2rtc.local/stream.html?src=ESP32-CAM_video&mode=mjpeg`
- Audio: `https://go2rtc.local/stream.html?src=ESP32-CAM_audio`

![Custom HTML page](/img/image-2.png)

### Option 2: Combined stream

```yaml
streams:
  ESP32-CAM:
    - ffmpeg:http://192.168.10.55:81/stream#video=h264
    - ffmpeg:http://192.168.10.55:82/audio#audio=opus
```

Stream URL: `https://go2rtc.local/stream.html?src=ESP32-CAM`

![go2rtc combined stream](/img/image-3.png)

## Home Assistant

Add a Webpage card pointing at your go2rtc stream:

```yaml
type: iframe
url: https://go2rtc.local/stream.html?src=ESP32-CAM
aspect_ratio: 100%
```

## Architecture

Three HTTP servers run on separate ports to avoid connection limits and allow independent streaming:

| Port | Module | Purpose |
|------|--------|---------|
| 80 | `http_ui.c` | Web UI, camera control, settings, OTA |
| 81 | `http_video_stream.c` | MJPEG stream (async FreeRTOS task per client) |
| 82 | `http_audio_stream.c` | WAV audio stream (I2S mic capture) |

### Startup sequence

NVS init &rarr; SPIFFS mount (`/www`) &rarr; Camera init &rarr; Sensor config &rarr; LED PWM &rarr; WiFi &rarr; Start servers &rarr; WiFi reconnect loop

### Web content pipeline

HTML/CSS in `spiffs_data/` is gzipped at build time and packed into a SPIFFS image flashed to the `spiffs` partition. Files are served with `Content-Encoding: gzip` and `Cache-Control: max-age=86400`.

### Partition table

| Name | Type | Offset | Size |
|------|------|--------|------|
| nvs | NVS | 0x9000 | 20 KB |
| otadata | OTA data | 0xE000 | 8 KB |
| app0 | OTA_0 | 0x10000 | 1664 KB |
| app1 | OTA_1 | 0x1B0000 | 1664 KB |
| spiffs | SPIFFS | 0x350000 | 704 KB |

Dual OTA partitions allow safe firmware updates. Upload via `/update` writes to the inactive partition, then sets it as the boot target.
