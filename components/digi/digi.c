#include <math.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/pulse_cnt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "wm8804.h"
#include "digi.h"

#define DIGI_FIND_TRIES 10
#define DIGI_FIND_INTERVAL_MS 50
#define DIGI_CROSSTALK_READS 200
#define DIGI_SETTLE_MS 50
#define DIGI_MEASURE_MS 250
#define DIGI_PCNT_LIMIT 30000
#define DIGI_LRCLK_GLITCH_NS 1000
#define PCNT_UNITS 4
#define DIGI_WRITE_TIMEOUT_MS 100
#define TONE_FRAMES 240
#define TONE_TABLE_BITS 10
#define TONE_TABLE_SIZE (1 << TONE_TABLE_BITS)
#define TONE_PEAK 2147483520.0f
#define PULL_SETTLE_US 50

static const char *TAG = "digi";

static SemaphoreHandle_t s_lock;
static i2s_chan_handle_t s_tx;
static TaskHandle_t s_tone_task;
static digi_state_t s_state = { .rate = DIGI_DEFAULT_RATE, .tone_level = 10 };
static int32_t s_sine[TONE_TABLE_SIZE];

static i2s_std_config_t std_config(uint32_t rate)
{
    return (i2s_std_config_t){
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_APP_DIGI_BCLK,
            .ws = CONFIG_APP_DIGI_WS,
            .dout = CONFIG_APP_DIGI_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
}

static esp_err_t i2s_start(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s channel");
    i2s_std_config_t std_cfg = std_config(s_state.rate);
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s std mode");
    return i2s_channel_enable(s_tx);
}

static void add_frames_sent(size_t bytes)
{
    __atomic_fetch_add(&s_state.frames_sent, bytes / (2 * sizeof(int32_t)), __ATOMIC_RELAXED);
}

static void tone_task(void *arg)
{
    static int32_t buf[TONE_FRAMES * 2];
    uint32_t phase = 0;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t hz = s_state.tone_hz;
        int32_t level = s_state.tone_level;
        uint32_t step = ((uint64_t)hz << 32) / s_state.rate;
        xSemaphoreGive(s_lock);
        if (!hz) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        for (int i = 0; i < TONE_FRAMES; i++) {
            int32_t sample = (int64_t)s_sine[phase >> (32 - TONE_TABLE_BITS)] * level / 100;
            buf[2 * i] = sample;
            buf[2 * i + 1] = sample;
            phase += step;
        }
        size_t written;
        esp_err_t err = i2s_channel_write(s_tx, buf, sizeof(buf), &written, DIGI_WRITE_TIMEOUT_MS);
        add_frames_sent(written);
        if (err == ESP_ERR_INVALID_STATE) {
            vTaskDelay(1);
        }
    }
}

static esp_err_t wait_for_chip(i2c_master_bus_handle_t bus)
{
    for (int i = 0; i < DIGI_FIND_TRIES; i++) {
        if (i2c_master_probe(bus, CONFIG_APP_DIGI_ADDR, DIGI_FIND_INTERVAL_MS) == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(DIGI_FIND_INTERVAL_MS));
    }
    return ESP_ERR_NOT_FOUND;
}

typedef struct {
    int gpio;
    uint32_t glitch_ns;
    uint32_t *per_second;
} edge_input_t;

static esp_err_t count_edges(const edge_input_t *inputs, int n);

static uint32_t running_rate(void)
{
    static const uint32_t rates[] = { 32000, 44100, 48000, 88200, 96000, 176400, 192000 };
    uint32_t lrclk = 0;
    const edge_input_t ws[] = { { CONFIG_APP_DIGI_WS, DIGI_LRCLK_GLITCH_NS, &lrclk } };
    if (count_edges(ws, 1) != ESP_OK) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        if (labs((long)lrclk - (long)rates[i]) * 1000 <= (long)rates[i] * 5) {
            return rates[i];
        }
    }
    return 0;
}

static esp_err_t start_audio(void)
{
    if (!s_tx) {
        ESP_RETURN_ON_ERROR(i2s_start(), TAG, "i2s");
    }
    if (!s_tone_task) {
        ESP_RETURN_ON_FALSE(xTaskCreate(tone_task, "digi_tone", 3072, NULL, 5, &s_tone_task) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    }
    return ESP_OK;
}

static void restore_i2s_pins_locked(void)
{
    if (!s_tx) {
        return;
    }
    i2s_std_config_t cfg = std_config(s_state.rate);
    i2s_channel_disable(s_tx);
    i2s_channel_reconfig_std_gpio(s_tx, &cfg.gpio_cfg);
    i2s_channel_enable(s_tx);
}

static esp_err_t start_locked(uint32_t rate)
{
    s_state.trim_ppb = 0;
    s_state.pll_k = 0;
    esp_err_t err = wm8804_start(CONFIG_APP_DIGI_XTAL_HZ, rate);
    if (err == ESP_OK) {
        err = wm8804_trim(0, false, &s_state.pll_k);
    }
    return err;
}

static esp_err_t init_locked(void)
{
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_bus_get(&bus), TAG, "i2c bus");
    if (wait_for_chip(bus) != ESP_OK) {
        ESP_LOGW(TAG, "no WM8804 at 0x%02x (SDA GPIO%d, SCL GPIO%d)", CONFIG_APP_DIGI_ADDR, CONFIG_APP_I2C_SDA, CONFIG_APP_I2C_SCL);
        uint32_t rate = s_tx ? 0 : running_rate();
        if (rate) {
            s_state.rate = rate;
            s_state.blind = start_audio() == ESP_OK;
            ESP_LOGW(TAG, "WM8804 still clocking at %lu Hz from an earlier setup: audio started without I2C", (unsigned long)rate);
        }
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(wm8804_attach(bus, CONFIG_APP_DIGI_ADDR), TAG, "attach");
    ESP_RETURN_ON_ERROR(wm8804_reset(), TAG, "reset");
    uint16_t id;
    ESP_RETURN_ON_ERROR(wm8804_read_id(&id, &s_state.rev), TAG, "id");
    ESP_RETURN_ON_FALSE(id == DIGI_CHIP_ID, ESP_ERR_INVALID_RESPONSE, TAG, "device at 0x%02x has id 0x%04x, expected 0x%04x", CONFIG_APP_DIGI_ADDR, id, DIGI_CHIP_ID);
    ESP_RETURN_ON_ERROR(start_locked(s_state.rate), TAG, "start");
    ESP_RETURN_ON_ERROR(start_audio(), TAG, "audio");
    s_state.ready = true;
    s_state.blind = false;
    ESP_LOGI(TAG, "WM8804 rev %u at 0x%02x, %lu Hz", s_state.rev, CONFIG_APP_DIGI_ADDR, (unsigned long)s_state.rate);
    return ESP_OK;
}

esp_err_t digi_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");
        s_state.tone_hz = CONFIG_APP_DIGI_BOOT_TONE_HZ;
        s_state.tone_level = CONFIG_APP_DIGI_BOOT_TONE_LEVEL;
        for (int i = 0; i < TONE_TABLE_SIZE; i++) {
            s_sine[i] = (int32_t)lrintf(sinf(2.0f * (float)M_PI * i / TONE_TABLE_SIZE) * TONE_PEAK);
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = s_state.ready ? ESP_OK : init_locked();
    xSemaphoreGive(s_lock);
    return err;
}

static void counter_free(pcnt_unit_handle_t unit, pcnt_channel_handle_t chan)
{
    if (unit) {
        pcnt_unit_stop(unit);
        pcnt_unit_disable(unit);
    }
    if (chan) {
        pcnt_del_channel(chan);
    }
    if (unit) {
        pcnt_del_unit(unit);
    }
}

static esp_err_t counter_new(int gpio, uint32_t glitch_ns, pcnt_unit_handle_t *unit, pcnt_channel_handle_t *chan)
{
    pcnt_unit_config_t unit_cfg = {
        .low_limit = -DIGI_PCNT_LIMIT,
        .high_limit = DIGI_PCNT_LIMIT,
        .flags.accum_count = true,
    };
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_cfg, unit), TAG, "pcnt unit");
    if (glitch_ns) {
        pcnt_glitch_filter_config_t filter = { .max_glitch_ns = glitch_ns };
        ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(*unit, &filter), TAG, "glitch filter");
    }
    pcnt_chan_config_t chan_cfg = { .edge_gpio_num = gpio, .level_gpio_num = -1 };
    ESP_RETURN_ON_ERROR(pcnt_new_channel(*unit, &chan_cfg, chan), TAG, "pcnt channel");
    ESP_RETURN_ON_ERROR(pcnt_channel_set_edge_action(*chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD), TAG, "edge action");
    ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(*unit, DIGI_PCNT_LIMIT), TAG, "watch point");
    ESP_RETURN_ON_ERROR(pcnt_unit_enable(*unit), TAG, "enable");
    return pcnt_unit_start(*unit);
}

static esp_err_t count_edges(const edge_input_t *inputs, int n)
{
    pcnt_unit_handle_t units[PCNT_UNITS] = { 0 };
    pcnt_channel_handle_t chans[PCNT_UNITS] = { 0 };
    int counts[PCNT_UNITS] = { 0 };
    esp_err_t err = ESP_OK;
    for (int i = 0; i < n && err == ESP_OK; i++) {
        err = counter_new(inputs[i].gpio, inputs[i].glitch_ns, &units[i], &chans[i]);
    }
    int64_t elapsed_us = 0;
    if (err == ESP_OK) {
        for (int i = 0; i < n; i++) {
            pcnt_unit_clear_count(units[i]);
        }
        int64_t start = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(DIGI_MEASURE_MS));
        for (int i = 0; i < n; i++) {
            pcnt_unit_get_count(units[i], &counts[i]);
        }
        elapsed_us = esp_timer_get_time() - start;
    }
    for (int i = 0; i < n; i++) {
        counter_free(units[i], chans[i]);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "pcnt");
    for (int i = 0; i < n; i++) {
        *inputs[i].per_second = ((int64_t)counts[i] * 1000000 + elapsed_us / 2) / elapsed_us;
    }
    return ESP_OK;
}

static esp_err_t measure_lines(digi_lines_t *out)
{
    const edge_input_t clocks[] = {
        { CONFIG_APP_DIGI_WS, DIGI_LRCLK_GLITCH_NS, &out->lrclk_hz },
        { CONFIG_APP_DIGI_WS, 0, &out->lrclk_raw_hz },
        { CONFIG_APP_DIGI_BCLK, 0, &out->bclk_hz },
    };
    const edge_input_t bus[] = {
        { CONFIG_APP_I2C_SDA, 0, &out->sda_edges },
        { CONFIG_APP_I2C_SCL, 0, &out->scl_edges },
    };
    const edge_input_t dout[] = {
        { CONFIG_APP_DIGI_DOUT, 0, &out->dout_edges },
    };
    const edge_input_t dout_driven[] = {
        { CONFIG_APP_DIGI_DOUT, 0, &out->dout_driven_edges },
    };
    ESP_RETURN_ON_ERROR(count_edges(dout_driven, 1), TAG, "dout driven");
    ESP_RETURN_ON_ERROR(count_edges(clocks, sizeof(clocks) / sizeof(clocks[0])), TAG, "clocks");
    ESP_RETURN_ON_ERROR(count_edges(bus, sizeof(bus) / sizeof(bus[0])), TAG, "bus");
    gpio_set_direction(CONFIG_APP_DIGI_DOUT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(CONFIG_APP_DIGI_DOUT, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(PULL_SETTLE_US);
    out->dout_pulled_up = gpio_get_level(CONFIG_APP_DIGI_DOUT);
    gpio_set_pull_mode(CONFIG_APP_DIGI_DOUT, GPIO_PULLDOWN_ONLY);
    esp_rom_delay_us(PULL_SETTLE_US);
    out->dout_pulled_down = gpio_get_level(CONFIG_APP_DIGI_DOUT);
    esp_err_t err = count_edges(dout, 1);
    gpio_set_pull_mode(CONFIG_APP_DIGI_DOUT, GPIO_FLOATING);
    gpio_set_direction(CONFIG_APP_DIGI_DOUT, GPIO_MODE_OUTPUT);
    return err;
}

static void count_clocked_errors(digi_probe_t *out)
{
    out->clocked_reads = DIGI_CROSSTALK_READS;
    for (int i = 0; i < DIGI_CROSSTALK_READS; i++) {
        uint8_t val;
        if (wm8804_reg_read(1, &val) != ESP_OK || val != DIGI_CHIP_ID >> 8) {
            out->clocked_errors++;
        }
    }
}

esp_err_t digi_probe(digi_probe_t *out)
{
    *out = (digi_probe_t){ 0 };
    esp_err_t err = digi_init();
    out->sda_level = gpio_get_level(CONFIG_APP_I2C_SDA);
    out->scl_level = gpio_get_level(CONFIG_APP_I2C_SCL);
    esp_err_t measure_err = measure_lines(&out->before);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    restore_i2s_pins_locked();
    if (measure_err != ESP_OK || err == ESP_ERR_NOT_FOUND || (err != ESP_OK && err != ESP_ERR_INVALID_RESPONSE)) {
        xSemaphoreGive(s_lock);
        ESP_RETURN_ON_ERROR(measure_err, TAG, "measure");
        return err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
    }
    out->acked = true;
    if (s_state.ready) {
        count_clocked_errors(out);
    }
    err = wm8804_reset();
    if (err == ESP_OK) {
        err = wm8804_read_id(&out->id, &out->rev);
    }
    if (s_state.ready) {
        esp_err_t start_err = start_locked(s_state.rate);
        err = err == ESP_OK ? start_err : err;
    }
    if (err == ESP_OK && s_state.ready) {
        vTaskDelay(pdMS_TO_TICKS(DIGI_SETTLE_MS));
        err = measure_lines(&out->after);
        restore_i2s_pins_locked();
    }
    xSemaphoreGive(s_lock);
    return err;
}

static bool attached(void)
{
    if (!s_lock) {
        return false;
    }
    i2c_master_bus_handle_t bus;
    return i2c_bus_get(&bus) == ESP_OK && wm8804_attach(bus, CONFIG_APP_DIGI_ADDR) == ESP_OK;
}

esp_err_t digi_reg_read(uint8_t reg, uint8_t *val)
{
    ESP_RETURN_ON_FALSE(attached(), ESP_ERR_INVALID_STATE, TAG, "not attached");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = wm8804_reg_read(reg, val);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t digi_reg_write(uint8_t reg, uint8_t val)
{
    ESP_RETURN_ON_FALSE(attached(), ESP_ERR_INVALID_STATE, TAG, "not attached");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = wm8804_reg_write(reg, val);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t digi_set_rate(uint32_t rate)
{
    ESP_RETURN_ON_FALSE(wm8804_rate_supported(rate), ESP_ERR_NOT_SUPPORTED, TAG, "unsupported rate %lu", (unsigned long)rate);
    ESP_RETURN_ON_FALSE(s_state.ready, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    i2s_channel_disable(s_tx);
    esp_err_t err = start_locked(rate);
    if (err == ESP_OK) {
        s_state.rate = rate;
    }
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_state.rate);
    i2s_channel_reconfig_std_clock(s_tx, &clk_cfg);
    i2s_channel_enable(s_tx);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t digi_spdif_edges(uint32_t glitch_ns, uint32_t *per_second)
{
    const edge_input_t mon[] = {
        { CONFIG_APP_DIGI_SPDIF_MON, glitch_ns, per_second },
    };
    return count_edges(mon, 1);
}

uint32_t digi_spdif_high_permille(gpio_pull_mode_t pull)
{
    gpio_set_direction(CONFIG_APP_DIGI_SPDIF_MON, GPIO_MODE_INPUT);
    gpio_set_pull_mode(CONFIG_APP_DIGI_SPDIF_MON, pull);
    esp_rom_delay_us(PULL_SETTLE_US);
    uint32_t high = 0;
    for (int i = 0; i < 100000; i++) {
        high += gpio_get_level(CONFIG_APP_DIGI_SPDIF_MON);
    }
    gpio_set_pull_mode(CONFIG_APP_DIGI_SPDIF_MON, GPIO_FLOATING);
    return high / 100;
}

static esp_err_t trim_locked(int32_t ppb, bool rewrite)
{
    esp_err_t err = wm8804_trim(ppb, rewrite, &s_state.pll_k);
    if (err == ESP_OK) {
        s_state.trim_ppb = ppb;
    }
    return err;
}

bool digi_trim_changes_msb(int32_t ppb)
{
    if (!s_state.ready) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changes = wm8804_trim_changes_msb(ppb);
    xSemaphoreGive(s_lock);
    return changes;
}

int32_t digi_trim_keep_msb(int32_t ppb)
{
    if (!s_state.ready) {
        return ppb;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int32_t kept = wm8804_trim_keep_msb(ppb);
    xSemaphoreGive(s_lock);
    return kept;
}

esp_err_t digi_trim_refresh(void)
{
    ESP_RETURN_ON_FALSE(s_state.ready, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = trim_locked(s_state.trim_ppb, true);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t digi_trim(int32_t ppb)
{
    ESP_RETURN_ON_FALSE(s_state.ready, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = trim_locked(ppb, false);
    xSemaphoreGive(s_lock);
    return err;
}

typedef struct {
    int32_t ppb;
    bool rewrite;
    esp_err_t err;
} trim_write_t;

static void trim_before_capture(void *arg)
{
    trim_write_t *w = arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    w->err = trim_locked(w->ppb, w->rewrite);
    xSemaphoreGive(s_lock);
}

esp_err_t digi_trim_capture(int32_t ppb, bool rewrite, digi_spdif_report_t *out, esp_err_t *write_err)
{
    ESP_RETURN_ON_FALSE(s_state.ready, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    trim_write_t w = { .ppb = ppb, .rewrite = rewrite, .err = ESP_ERR_INVALID_STATE };
    esp_err_t err = digi_spdif_capture(out, trim_before_capture, &w);
    *write_err = w.err;
    return err;
}

static void count_at(pcnt_unit_handle_t unit, int *count, int64_t *us)
{
    vTaskSuspendAll();
    pcnt_unit_get_count(unit, count);
    *us = esp_timer_get_time();
    xTaskResumeAll();
}

esp_err_t digi_bclk_offset(uint32_t ms, int32_t *ppb)
{
    ESP_RETURN_ON_FALSE(s_state.ready || s_state.blind, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    pcnt_unit_handle_t unit = NULL;
    pcnt_channel_handle_t chan = NULL;
    esp_err_t err = counter_new(CONFIG_APP_DIGI_BCLK, 0, &unit, &chan);
    int c0 = 0, c1 = 0;
    int64_t t0 = 0, t1 = 0;
    if (err == ESP_OK) {
        count_at(unit, &c0, &t0);
        count_at(unit, &c0, &t0);
        vTaskDelay(pdMS_TO_TICKS(ms));
        count_at(unit, &c1, &t1);
    }
    counter_free(unit, chan);
    ESP_RETURN_ON_ERROR(err, TAG, "pcnt");
    double expected = (double)s_state.rate * DIGI_BCLK_PER_FRAME * (t1 - t0) / 1e6;
    ESP_RETURN_ON_FALSE(expected > 0 && c1 > c0, ESP_ERR_INVALID_RESPONSE, TAG, "no BCLK");
    *ppb = (int32_t)llround((c1 - c0 - expected) * 1e9 / expected);
    return ESP_OK;
}

esp_err_t digi_tone(uint32_t hz, uint8_t level)
{
    ESP_RETURN_ON_FALSE(s_state.ready || s_state.blind, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(level <= 100, ESP_ERR_INVALID_ARG, TAG, "level 0-100");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = hz < s_state.rate / 2 ? ESP_OK : ESP_ERR_INVALID_ARG;
    if (err == ESP_OK) {
        s_state.tone_hz = hz;
        s_state.tone_level = level;
    }
    xSemaphoreGive(s_lock);
    ESP_RETURN_ON_ERROR(err, TAG, "tone must be below %lu Hz", (unsigned long)s_state.rate / 2);
    xTaskNotifyGive(s_tone_task);
    return ESP_OK;
}

esp_err_t digi_write(const int32_t *frames, size_t count, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_state.ready || s_state.blind, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(!s_state.tone_hz, ESP_ERR_INVALID_STATE, TAG, "tone is playing");
    size_t written;
    esp_err_t err = i2s_channel_write(s_tx, frames, count * 2 * sizeof(int32_t), &written, timeout_ms);
    add_frames_sent(written);
    return err;
}

void digi_get(digi_state_t *out)
{
    if (!s_lock) {
        *out = s_state;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

