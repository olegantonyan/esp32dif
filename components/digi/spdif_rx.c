#include <stdlib.h>
#include <string.h>
#include "driver/rmt_rx.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "i2c_bus.h"
#include "digi.h"

#define CAPTURE_RESOLUTION_HZ (80 * 1000 * 1000)
#define CAPTURE_MAX_BLOCKS 4
#define CAPTURE_MIN_BLOCKS 2
#define CAPTURE_SYMBOLS 24000
#define CAPTURE_MIN_SYMBOLS 3000
#define CAPTURE_MS 20
#define CAPTURE_MIN_NS 40
#define CAPTURE_MAX_NS 100000
#define CELLS_PER_FRAME 128
#define PREAMBLE_CELLS 8
#define SUBFRAME_BITS 28
#define AUDIO_BITS 24
#define BIT_VALIDITY 24
#define BIT_STATUS 26
#define CS_BITS (DIGI_SPDIF_CS_BYTES * 8)
#define HIST_TICKS 512
#define HIST_GLITCH_SHARE 200

static const char *TAG = "spdif_rx";

typedef enum { WAIT_PREAMBLE, PREAMBLE, BITS } parse_state_t;

typedef struct {
    digi_spdif_report_t *out;
    int32_t *samples;
    parse_state_t state;
    uint8_t pre_runs[4];
    int pre_count;
    int pre_cells;
    int bit;
    bool half;
    uint32_t bits;
    char kind;
    bool have_left;
    uint64_t ticks;
    uint32_t cells;
    int32_t left_sample;
    int cs_bit;
    bool mark_breaks;
} parser_t;

static int32_t sign_extend24(uint32_t v)
{
    return (int32_t)(v << 8) >> 8;
}

static char preamble_kind(const uint8_t *runs, int n)
{
    if (n == 4 && runs[1] == 1 && runs[2] == 1 && runs[3] == 3) {
        return 'B';
    }
    if (n == 4 && runs[1] == 3 && runs[2] == 1 && runs[3] == 1) {
        return 'M';
    }
    if (n == 4 && runs[1] == 2 && runs[2] == 1 && runs[3] == 2) {
        return 'W';
    }
    return 0;
}

static void sync_lost(parser_t *p)
{
    digi_spdif_report_t *out = p->out;
    out->sync_errors++;
    if (p->mark_breaks && out->frames && out->frames < DIGI_SPDIF_MAX_FRAMES && p->samples[(out->frames - 1) * 2] != DIGI_SPDIF_BREAK) {
        p->samples[out->frames * 2] = DIGI_SPDIF_BREAK;
        p->samples[out->frames * 2 + 1] = DIGI_SPDIF_BREAK;
        out->frames++;
    }
    p->state = WAIT_PREAMBLE;
    p->have_left = false;
}

static void subframe_done(parser_t *p)
{
    digi_spdif_report_t *out = p->out;
    out->subframes++;
    if (__builtin_popcount(p->bits) & 1) {
        out->parity_errors++;
    }
    if (p->bits >> BIT_VALIDITY & 1) {
        out->validity_set++;
    }
    int32_t sample = sign_extend24(p->bits & ((1u << AUDIO_BITS) - 1));
    out->sample_or |= p->bits & ((1u << AUDIO_BITS) - 1);
    if (p->kind == 'B') {
        out->blocks++;
        if (!out->cs_valid) {
            p->cs_bit = 0;
            memset(out->cs, 0, sizeof(out->cs));
        }
    }
    if (p->kind == 'B' || p->kind == 'M') {
        if (p->cs_bit >= 0 && p->cs_bit < CS_BITS) {
            out->cs[p->cs_bit / 8] |= (p->bits >> BIT_STATUS & 1) << (p->cs_bit % 8);
            if (++p->cs_bit == CS_BITS) {
                out->cs_valid = true;
            }
        }
        p->left_sample = sample;
        p->have_left = true;
        return;
    }
    if (!p->have_left) {
        return;
    }
    p->have_left = false;
    if (sample != p->left_sample) {
        out->lr_mismatch++;
    }
    int32_t frame[2] = { p->left_sample, sample };
    for (int ch = 0; ch < 2; ch++) {
        if (abs(frame[ch]) > out->peak[ch]) {
            out->peak[ch] = abs(frame[ch]);
        }
        if (out->frames < DIGI_SPDIF_MAX_FRAMES) {
            p->samples[out->frames * 2 + ch] = frame[ch];
        }
    }
    out->frames++;
}

static void parse_run(parser_t *p, int cells)
{
    if (cells < 1 || cells > 3) {
        sync_lost(p);
        return;
    }
    if (cells == 3 && p->state != PREAMBLE) {
        if (p->state == BITS) {
            sync_lost(p);
        }
        p->state = PREAMBLE;
        p->pre_count = 0;
        p->pre_cells = 0;
    }
    switch (p->state) {
    case WAIT_PREAMBLE:
        return;
    case PREAMBLE:
        p->pre_runs[p->pre_count++] = cells;
        p->pre_cells += cells;
        if (p->pre_cells < PREAMBLE_CELLS && p->pre_count < 4) {
            return;
        }
        p->kind = p->pre_cells == PREAMBLE_CELLS ? preamble_kind(p->pre_runs, p->pre_count) : 0;
        if (!p->kind) {
            sync_lost(p);
            return;
        }
        p->state = BITS;
        p->bit = 0;
        p->half = false;
        p->bits = 0;
        return;
    case BITS:
        if (cells == 2 && !p->half) {
            p->bit++;
        } else if (cells == 1) {
            if (p->half) {
                p->bits |= 1u << p->bit;
                p->bit++;
            }
            p->half = !p->half;
        } else {
            sync_lost(p);
            return;
        }
        if (p->bit == SUBFRAME_BITS) {
            subframe_done(p);
            p->state = WAIT_PREAMBLE;
        }
        return;
    }
}

static size_t symbol_count(const rmt_symbol_word_t *sym, size_t max)
{
    size_t n = 0;
    while (n < max && sym[n].duration0) {
        n++;
    }
    return n;
}

static uint32_t unit_interval_x16(const rmt_symbol_word_t *sym, size_t n)
{
    static uint32_t hist[HIST_TICKS];
    memset(hist, 0, sizeof(hist));
    uint32_t total = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t d[2] = { sym[i].duration0, sym[i].duration1 };
        for (int j = 0; j < 2; j++) {
            if (d[j] && d[j] < HIST_TICKS) {
                hist[d[j]]++;
                total++;
            }
        }
    }
    uint32_t shortest = 0;
    uint32_t below = 0;
    for (uint32_t t = 1; t < HIST_TICKS && !shortest; t++) {
        below += hist[t];
        if (below * HIST_GLITCH_SHARE > total) {
            shortest = t;
        }
    }
    uint64_t sum = 0;
    uint32_t count = 0;
    for (uint32_t t = shortest; shortest && t < shortest * 3 / 2 + 1 && t < HIST_TICKS; t++) {
        sum += (uint64_t)hist[t] * t;
        count += hist[t];
    }
    return count ? (uint32_t)(sum * 16 / count) : 0;
}

static uint32_t estimate_tone(const digi_spdif_report_t *out, const int32_t *samples, int ch)
{
    int frames = out->frames < DIGI_SPDIF_MAX_FRAMES ? out->frames : DIGI_SPDIF_MAX_FRAMES;
    float first = -1, last = -1;
    int crossings = 0;
    for (int i = 0; i + 1 < frames; i++) {
        int32_t a = samples[i * 2 + ch], b = samples[(i + 1) * 2 + ch];
        bool rising = a < 0 && b >= 0;
        bool falling = a >= 0 && b < 0;
        if (!rising && !falling) {
            continue;
        }
        float t = i + (float)a / (float)(a - b);
        if (first < 0) {
            first = t;
        }
        last = t;
        crossings++;
    }
    if (crossings < 2 || !out->rate_hz) {
        return 0;
    }
    return (uint32_t)((crossings - 1) / 2.0f / ((last - first) / out->rate_hz) * 10 + 0.5f);
}

static esp_err_t capture(digi_spdif_report_t *out, void (*before)(void *arg), void *arg, int32_t *samples, bool mark_breaks)
{
    *out = (digi_spdif_report_t){ 0 };
    esp_log_level_set("rmt", ESP_LOG_NONE);
    size_t capacity = CAPTURE_SYMBOLS;
    rmt_symbol_word_t *sym = NULL;
    while (!sym && capacity >= CAPTURE_MIN_SYMBOLS) {
        sym = heap_caps_calloc(capacity, sizeof(rmt_symbol_word_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        capacity = sym ? capacity : capacity / 2;
    }
    if (!sym) {
        return ESP_ERR_NO_MEM;
    }
    rmt_channel_handle_t rx = NULL;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int blocks = CAPTURE_MAX_BLOCKS; blocks >= CAPTURE_MIN_BLOCKS && err != ESP_OK; blocks--) {
        rmt_rx_channel_config_t cfg = {
            .gpio_num = CONFIG_APP_DIGI_SPDIF_MON,
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = CAPTURE_RESOLUTION_HZ,
            .mem_block_symbols = blocks * SOC_RMT_MEM_WORDS_PER_CHANNEL,
        };
        err = rmt_new_rx_channel(&cfg, &rx);
        out->mem_blocks = blocks;
    }
    if (err == ESP_OK) {
        err = rmt_enable(rx);
    }
    if (err == ESP_OK) {
        rmt_receive_config_t rcv = { .signal_range_min_ns = CAPTURE_MIN_NS, .signal_range_max_ns = CAPTURE_MAX_NS };
        i2c_bus_lock();
        if (before) {
            before(arg);
        }
        err = rmt_receive(rx, sym, capacity * sizeof(rmt_symbol_word_t), &rcv);
        vTaskDelay(pdMS_TO_TICKS(CAPTURE_MS));
        i2c_bus_unlock();
        rmt_disable(rx);
    }
    if (rx) {
        rmt_del_channel(rx);
    }
    esp_log_level_set("rmt", ESP_LOG_WARN);
    if (err == ESP_OK) {
        size_t n = symbol_count(sym, capacity);
        out->symbols = n;
        for (size_t i = 0; i < capacity; i++) {
            out->nonzero_symbols += sym[i].val != 0;
        }
        for (int i = 0; i < DIGI_SPDIF_RAW_SYMBOLS; i++) {
            out->raw[i] = sym[i].val;
        }
        uint32_t ui16 = unit_interval_x16(sym, n);
        out->ui_x16 = ui16;
        if (ui16) {
            parser_t p = { .out = out, .samples = samples, .cs_bit = -1, .mark_breaks = mark_breaks };
            for (size_t i = 0; i < n; i++) {
                uint32_t d[2] = { sym[i].duration0, sym[i].duration1 };
                for (int j = 0; j < 2 && d[j]; j++) {
                    int cells = (d[j] * 16 + ui16 / 2) / ui16;
                    out->runs[cells < 4 ? cells : 4]++;
                    p.ticks += d[j];
                    p.cells += cells;
                    parse_run(&p, cells);
                }
            }
            out->rate_hz = p.ticks ? (uint32_t)((uint64_t)CAPTURE_RESOLUTION_HZ * p.cells / (CELLS_PER_FRAME * p.ticks)) : 0;
            for (int ch = 0; ch < 2; ch++) {
                out->tone_hz_x10[ch] = estimate_tone(out, samples, ch);
            }
            memcpy(out->first, samples, sizeof(out->first));
        }
    }
    free(sym);
    ESP_RETURN_ON_ERROR(err, TAG, "capture");
    return ESP_OK;
}

esp_err_t digi_spdif_capture(digi_spdif_report_t *out, void (*before)(void *arg), void *arg)
{
    int32_t *samples = calloc(DIGI_SPDIF_MAX_FRAMES * 2, sizeof(int32_t));
    if (!samples) {
        *out = (digi_spdif_report_t){ 0 };
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = capture(out, before, arg, samples, false);
    free(samples);
    return err;
}

esp_err_t digi_spdif_capture_frames(digi_spdif_report_t *out, int32_t (*frames)[2])
{
    return capture(out, NULL, NULL, &frames[0][0], true);
}
