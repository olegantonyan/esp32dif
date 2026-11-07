#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "sdkconfig.h"

#define DIGI_CHIP_ID 0x8805
#define DIGI_DEFAULT_RATE 44100
#define DIGI_SPDIF_CS_BYTES 5
#define DIGI_SPDIF_MAX_FRAMES 1024
#define DIGI_BCLK_PER_FRAME 64
#define DIGI_SPDIF_RAW_SYMBOLS 16
#define DIGI_SPDIF_DUMP_FRAMES 24

typedef struct {
    bool ready;
    bool blind;
    uint8_t rev;
    uint32_t rate;
    uint32_t tone_hz;
    uint8_t tone_level;
    uint32_t frames_sent;
    int32_t trim_ppb;
    uint32_t pll_k;
} digi_state_t;

typedef struct {
    uint32_t bclk_hz;
    uint32_t lrclk_hz;
    uint32_t lrclk_raw_hz;
    uint32_t sda_edges;
    uint32_t scl_edges;
    uint32_t dout_edges;
    uint32_t dout_driven_edges;
    int dout_pulled_up;
    int dout_pulled_down;
} digi_lines_t;

typedef struct {
    digi_lines_t before;
    digi_lines_t after;
    bool acked;
    int clocked_reads;
    int clocked_errors;
    int sda_level;
    int scl_level;
    uint16_t id;
    uint8_t rev;
} digi_probe_t;

typedef struct {
    int mem_blocks;
    uint32_t symbols;
    uint32_t subframes;
    uint32_t frames;
    uint32_t blocks;
    uint32_t sync_errors;
    uint32_t parity_errors;
    uint32_t validity_set;
    uint32_t lr_mismatch;
    int32_t peak[2];
    uint32_t sample_or;
    uint32_t nonzero_symbols;
    uint32_t ui_x16;
    uint32_t runs[5];
    uint32_t raw[DIGI_SPDIF_RAW_SYMBOLS];
    int32_t first[DIGI_SPDIF_DUMP_FRAMES][2];
    uint32_t rate_hz;
    uint32_t tone_hz_x10[2];
    bool cs_valid;
    uint8_t cs[DIGI_SPDIF_CS_BYTES];
} digi_spdif_report_t;

esp_err_t digi_init(void);
esp_err_t digi_probe(digi_probe_t *out);
esp_err_t digi_reg_read(uint8_t reg, uint8_t *val);
esp_err_t digi_reg_write(uint8_t reg, uint8_t val);
esp_err_t digi_set_rate(uint32_t rate);
esp_err_t digi_spdif_edges(uint32_t glitch_ns, uint32_t *per_second);
uint32_t digi_spdif_high_permille(gpio_pull_mode_t pull);
esp_err_t digi_spdif_capture(digi_spdif_report_t *out, void (*before)(void *arg), void *arg);
esp_err_t digi_trim(int32_t ppb);
esp_err_t digi_trim_capture(int32_t ppb, bool rewrite, digi_spdif_report_t *out, esp_err_t *write_err);
esp_err_t digi_bclk_offset(uint32_t ms, int32_t *ppb);
esp_err_t digi_tone(uint32_t hz, uint8_t level);
esp_err_t digi_write(const int32_t *frames, size_t count, uint32_t timeout_ms);
void digi_get(digi_state_t *out);
