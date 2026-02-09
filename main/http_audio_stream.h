#pragma once

#include <stdint.h>

// I2S pin configuration (AI-Thinker with INMP441)
#define I2S_MIC_WS            2
#define I2S_MIC_SCK           14
#define I2S_MIC_SD            15
// XIAO_ESP32S3:
// #define I2S_MIC_WS         42
// #define I2S_MIC_SCK        -1
// #define I2S_MIC_SD         41

// I2S peripheral (AI-Thinker uses I2S_NUM_1 to avoid camera conflict)
#define I2S_MIC_PORT          1
// XIAO_ESP32S3:
// #define I2S_MIC_PORT       0

// Sampling parameters
#define SAMPLE_RATE       22050
#define SAMPLE_BITS       32
// XIAO_ESP32S3:
// #define SAMPLE_BITS    16
#define DMA_BUF_COUNT     32
#define DMA_BUF_LEN       1024

struct WAVHeader {
    char chunkId[4];
    uint32_t chunkSize;
    char format[4];
    char subchunk1Id[4];
    uint32_t subchunk1Size;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char subchunk2Id[4];
    uint32_t subchunk2Size;
};

void start_http_audio_stream(void);
void mic_i2s_reinit(void);
