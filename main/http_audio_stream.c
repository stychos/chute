#include "http_audio_stream.h"
#include "settings_manager.h"

#include <string.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "http_audio";

// Persistent I2S channel handle — allocated once at startup
static i2s_chan_handle_t rx_handle = NULL;

// Async streaming state
static volatile bool s_audio_stop = false;
static volatile TaskHandle_t s_audio_task = NULL;

static esp_err_t mic_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_MIC_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = DMA_BUF_LEN / (SAMPLE_BITS / 8);

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_APLL,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(SAMPLE_BITS, I2S_SLOT_MODE_MONO),
        // XIAO_ESP32S3 (PDM): use driver/i2s_pdm.h + i2s_pdm_rx_config_t instead
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_MIC_SCK,
            .ws = I2S_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_MIC_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
        return err;
    }

    ESP_LOGI(TAG, "I2S channel initialized (port %d, rate %d, bits %d)",
             I2S_MIC_PORT, SAMPLE_RATE, SAMPLE_BITS);
    return ESP_OK;
}

static void initializeWAVHeader(struct WAVHeader *header, uint32_t sampleRate,
                                 uint16_t bitsPerSample, uint16_t numChannels)
{
    memcpy(header->chunkId, "RIFF", 4);
    memcpy(header->format, "WAVE", 4);
    memcpy(header->subchunk1Id, "fmt ", 4);
    memcpy(header->subchunk2Id, "data", 4);

    header->chunkSize = 0xFFFFFFFF;
    header->subchunk1Size = 16;
    header->audioFormat = 1; // PCM
    header->numChannels = numChannels;
    header->sampleRate = sampleRate;
    header->bitsPerSample = 16; // Always 16-bit output for browser compatibility
    header->byteRate = (sampleRate * 16 * numChannels) / 8;
    header->blockAlign = (16 * numChannels) / 8;
    header->subchunk2Size = 0xFFFFFFFF;
}

static void audio_stream_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;

    if (!rx_handle) {
        ESP_LOGE(TAG, "I2S not initialized");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "I2S not ready");
        goto done;
    }

    esp_err_t err = i2s_channel_enable(rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "I2S enable failed");
        goto done;
    }

    ESP_LOGI(TAG, "Audio stream started");

    struct WAVHeader wav_header;
    initializeWAVHeader(&wav_header, SAMPLE_RATE, SAMPLE_BITS, 1);

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Accept-Ranges", "none");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");

    // Send WAV header as first chunk
    err = httpd_resp_send_chunk(req, (const char *)&wav_header, sizeof(wav_header));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send WAV header");
        i2s_channel_disable(rx_handle);
        goto done;
    }

    uint8_t i2s_buffer[DMA_BUF_LEN];
    int16_t out_buffer[DMA_BUF_LEN / 4]; // 1 x 16-bit sample per 32-bit I2S frame
    size_t bytes_read = 0;

    while (!s_audio_stop) {
        esp_err_t rd = i2s_channel_read(rx_handle, i2s_buffer, sizeof(i2s_buffer),
                                         &bytes_read, pdMS_TO_TICKS(1000));
        if (rd == ESP_ERR_TIMEOUT) {
            continue; // No data yet, try again
        }
        if (rd != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(rd));
            break;
        }

        if (bytes_read > 0) {
            int samples_read = bytes_read / 4;
            int32_t *samples32 = (int32_t *)i2s_buffer;
            for (int i = 0; i < samples_read; i++) {
                int32_t amplified = (samples32[i] >> 16) * mic_gain;
                if (amplified > 32767) amplified = 32767;
                else if (amplified < -32768) amplified = -32768;
                out_buffer[i] = (int16_t)amplified;
            }
            size_t out_bytes = samples_read * sizeof(int16_t);
            err = httpd_resp_send_chunk(req, (const char *)out_buffer, out_bytes);
            if (err != ESP_OK) {
                ESP_LOGI(TAG, "Audio client disconnected");
                break;
            }
        }
    }

    httpd_resp_send_chunk(req, NULL, 0);
    i2s_channel_disable(rx_handle);

done:
    httpd_req_async_handler_complete(req);
    ESP_LOGI(TAG, "Audio stream ended");
    s_audio_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t audio_stream_handler(httpd_req_t *req)
{
    // Stop any existing audio stream task
    if (s_audio_task) {
        ESP_LOGI(TAG, "Stopping previous audio stream");
        s_audio_stop = true;
        for (int i = 0; i < 30 && s_audio_task; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_audio_task) {
            ESP_LOGW(TAG, "Previous audio task did not stop in time");
        }
    }

    s_audio_stop = false;

    // Create async copy of the request — frees the httpd server task
    httpd_req_t *async_req = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_req_async_handler_begin failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // Spawn streaming in a dedicated FreeRTOS task
    BaseType_t ret = xTaskCreate(audio_stream_task, "aud_stream", 4096,
                                 async_req, 5, (TaskHandle_t *)&s_audio_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio stream task");
        httpd_req_async_handler_complete(async_req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Audio stream started (async)");
    return ESP_OK;
}

void start_http_audio_stream(void)
{
    // Initialize I2S once at startup
    esp_err_t err = mic_i2s_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed, audio will be unavailable");
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 82;
    config.ctrl_port = 32770;
    config.max_open_sockets = 2;
    config.lru_purge_enable = true;
    config.send_wait_timeout = 2;
    config.recv_wait_timeout = 2;

    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting audio stream server on port %d", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio stream server");
        return;
    }

    httpd_uri_t audio_uri = {
        .uri = "/audio",
        .method = HTTP_GET,
        .handler = audio_stream_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &audio_uri);
}
