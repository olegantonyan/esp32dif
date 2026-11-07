#include <stdlib.h>
#include <string.h>
#include "digi.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "rtp_priv.h"

#define PLAY_CHUNK_FRAMES 240
#define PLAY_MAX_BYTES_PER_FRAME 8
#define PLAY_WRITE_TIMEOUT_MS 100
#define PLAY_WAIT_MS 10
#define PLAY_TASK_STACK 4096
#define PLAY_TASK_PRIO 7
#define DRIFT_TASK_STACK 3072
#define DRIFT_TASK_PRIO 3
#define DRIFT_PERIOD_MS 1000
#define DRIFT_SETTLE_PERIODS 3
#define DRIFT_TP_S 300
#define DRIFT_TI_S 3600
#define DRIFT_MAX_PPB 300000
#define DRIFT_OFFSET_MAX_PPB 250000
#define DRIFT_STEP_PPB 5000
#define DRIFT_EMA_PERIODS 10
#define DRIFT_FAIL_LIMIT 3
#define DRIFT_RETRY_PERIODS 300
#define DRIFT_REFRESH_PERIODS 60
#define DRIFT_MSB_HOLD_PERIODS 60
#define DRIFT_INTEGRATE_MS 20
#define DRIFT_WRITE_GAP_PERIODS 10

static const char *TAG = "rtp_play";

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static uint8_t *s_ring;
static size_t s_size;
static bool s_psram;
static size_t s_cap;
static size_t s_head;
static size_t s_fill;
static uint32_t s_bpf;
static uint32_t s_rate;
static bool s_rate_checked;
static rtp_play_state_t s_state;
static uint32_t s_debt;
static bool s_short;
static uint32_t s_running_periods;
static uint32_t s_fill_peak;
static bool s_fill_seen;
static uint32_t s_level;
static uint32_t s_underruns;
static uint32_t s_rebuffers;
static uint64_t s_late_dropped;
static uint32_t s_overflows;
static uint32_t s_write_errors;
static int32_t s_trim;
static int64_t s_offset;
static uint32_t s_trim_writes;
static uint32_t s_trim_errors;
static uint32_t s_msb_hold;
static int64_t s_level_x16;
static uint32_t s_refresh;
static uint32_t s_fail_streak;
static uint32_t s_retry_wait;
static uint32_t s_write_wait;
static int32_t s_slip_ppb;
static int64_t s_slip_acc;
static uint32_t s_slipped;

static size_t target_bytes(void)
{
    size_t target = (size_t)s_rate * CONFIG_APP_RTP_LATENCY_MS / 1000 * s_bpf;
    return target < s_cap / 2 ? target : s_cap / 2 / s_bpf * s_bpf;
}

static void ring_copy_in(const uint8_t *data, size_t n)
{
    size_t tail = (s_head + s_fill) % s_cap;
    size_t first = n < s_cap - tail ? n : s_cap - tail;
    if (data) {
        memcpy(s_ring + tail, data, first);
        memcpy(s_ring, data + first, n - first);
    } else {
        memset(s_ring + tail, 0, first);
        memset(s_ring, 0, n - first);
    }
    s_fill += n;
}

static void ring_copy_out(uint8_t *out, size_t n)
{
    size_t first = n < s_cap - s_head ? n : s_cap - s_head;
    memcpy(out, s_ring + s_head, first);
    memcpy(out + first, s_ring, n - first);
    s_head = (s_head + n) % s_cap;
    s_fill -= n;
}

static void push_locked(const uint8_t *data, size_t bytes)
{
    if (s_state == RTP_PLAY_IDLE || !s_cap) {
        return;
    }
    bytes -= bytes % s_bpf;
    size_t late = (size_t)s_debt * s_bpf < bytes ? (size_t)s_debt * s_bpf : bytes;
    s_debt -= late / s_bpf;
    s_late_dropped += late / s_bpf;
    bytes -= late;
    if (data) {
        data += late;
    }
    if (s_fill + bytes > s_cap) {
        s_overflows++;
        return;
    }
    ring_copy_in(data, bytes);
}

static void to_i2s(const uint8_t *raw, int32_t *frames, uint32_t bpf)
{
    for (int i = 0; i < PLAY_CHUNK_FRAMES * 2; i++) {
        const uint8_t *s = raw + i * (bpf / 2);
        switch (bpf) {
        case 4: frames[i] = (int32_t)((uint32_t)s[0] << 24 | (uint32_t)s[1] << 16); break;
        case 6: frames[i] = (int32_t)((uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8); break;
        default: frames[i] = (int32_t)((uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | s[3]); break;
        }
    }
}

static bool rate_ok(uint32_t rate)
{
    digi_state_t st;
    digi_get(&st);
    if (st.rate == rate) {
        return true;
    }
    esp_err_t err = st.ready ? digi_set_rate(rate) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stream is %lu Hz but the WM8804 runs at %lu Hz and can't switch: %s", (unsigned long)rate, (unsigned long)st.rate, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static size_t take_chunk_locked(uint8_t *raw, size_t chunk)
{
    size_t prefill = target_bytes();
    if (s_state == RTP_PLAY_BUFFERING && s_fill >= prefill && s_fill >= chunk) {
        s_state = RTP_PLAY_RUNNING;
        s_debt = 0;
        s_short = false;
    }
    if (s_state != RTP_PLAY_RUNNING) {
        return 0;
    }
    s_slip_acc += (int64_t)s_slip_ppb * PLAY_CHUNK_FRAMES;
    int slip = s_slip_acc >= 1000000000LL ? 1 : s_slip_acc <= -1000000000LL ? -1 : 0;
    s_slip_acc -= slip * 1000000000LL;
    size_t want = chunk + slip * s_bpf;
    size_t got = s_fill < want ? s_fill : want;
    ring_copy_out(raw, got);
    if (slip < 0 && got == want) {
        memcpy(raw + want, raw + want - s_bpf, s_bpf);
        got = chunk;
    }
    got = got < chunk ? got : chunk;
    s_slipped += slip != 0 && got == chunk;
    memset(raw + got, 0, chunk - got);
    if (got < chunk) {
        s_debt += (chunk - got) / s_bpf;
        s_underruns += !s_short;
        s_short = true;
        if ((size_t)s_debt * s_bpf > prefill) {
            s_state = RTP_PLAY_BUFFERING;
            s_debt = 0;
            s_rebuffers++;
        }
    } else {
        s_short = false;
    }
    s_fill_peak = s_fill / s_bpf > s_fill_peak ? s_fill / s_bpf : s_fill_peak;
    s_fill_seen = true;
    return chunk;
}

static void play_task(void *arg)
{
    static uint8_t raw[(PLAY_CHUNK_FRAMES + 1) * PLAY_MAX_BYTES_PER_FRAME];
    static int32_t frames[PLAY_CHUNK_FRAMES * 2];
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t bpf = s_bpf, rate = s_rate;
        bool check = !s_rate_checked && s_state != RTP_PLAY_IDLE && rate;
        size_t got = check ? 0 : take_chunk_locked(raw, PLAY_CHUNK_FRAMES * bpf);
        xSemaphoreGive(s_lock);
        if (check) {
            bool ok = rate_ok(rate);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_rate_checked = ok && s_rate == rate;
            xSemaphoreGive(s_lock);
            if (!ok) {
                vTaskDelay(pdMS_TO_TICKS(DRIFT_PERIOD_MS));
            }
            continue;
        }
        if (!got) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PLAY_WAIT_MS));
            continue;
        }
        to_i2s(raw, frames, bpf);
        if (digi_write(frames, PLAY_CHUNK_FRAMES, PLAY_WRITE_TIMEOUT_MS) != ESP_OK) {
            s_write_errors++;
        }
    }
}

static int32_t clamp(int64_t v, int64_t lim)
{
    return v > lim ? lim : v < -lim ? -lim : v;
}

static void drift_step(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool running = s_state == RTP_PLAY_RUNNING && s_rate_checked && s_fill_seen;
    uint32_t peak = running ? s_fill_peak : 0;
    int64_t target = running ? (int64_t)(target_bytes() / s_bpf) : 0;
    uint32_t rate = s_rate;
    s_fill_peak = 0;
    s_fill_seen = false;
    s_running_periods = running ? s_running_periods + 1 : 0;
    xSemaphoreGive(s_lock);
    if (s_running_periods < DRIFT_SETTLE_PERIODS) {
        s_level_x16 = (int64_t)peak * 16;
        s_level = peak;
        return;
    }
    s_level_x16 += ((int64_t)peak * 16 - s_level_x16) / DRIFT_EMA_PERIODS;
    s_level = s_level_x16 / 16;
    int64_t error = s_level_x16 / 16 - target;
    int64_t p = error * 1000000000LL / ((int64_t)DRIFT_TP_S * rate);
    bool settled = llabs(error) * 1000 <= (int64_t)DRIFT_INTEGRATE_MS * rate;
    int64_t offset = settled ? clamp(s_offset + p * DRIFT_PERIOD_MS / (DRIFT_TI_S * 1000LL), DRIFT_OFFSET_MAX_PPB) : s_offset;
    int32_t want = clamp(offset + p, DRIFT_MAX_PPB);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_offset = offset;
    xSemaphoreGive(s_lock);
    s_msb_hold = s_msb_hold ? s_msb_hold - 1 : 0;
    digi_state_t st;
    digi_get(&st);
    s_retry_wait = s_retry_wait ? s_retry_wait - 1 : 0;
    s_write_wait = s_write_wait ? s_write_wait - 1 : 0;
    bool trim_usable = st.ready && !s_retry_wait;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_slip_ppb = trim_usable ? 0 : want - (st.ready ? st.trim_ppb : 0);
    xSemaphoreGive(s_lock);
    if (!trim_usable || s_write_wait || llabs((int64_t)want - st.trim_ppb) < DRIFT_STEP_PPB) {
        s_trim = st.trim_ppb;
        return;
    }
    if (++s_refresh >= DRIFT_REFRESH_PERIODS) {
        s_refresh = 0;
        if (digi_trim_refresh() != ESP_OK) {
            s_trim_errors++;
            s_fail_streak++;
        }
    }
    if (s_msb_hold) {
        want = digi_trim_keep_msb(want);
    } else if (digi_trim_changes_msb(want)) {
        s_msb_hold = DRIFT_MSB_HOLD_PERIODS;
    }
    if (llabs((int64_t)want - st.trim_ppb) < DRIFT_STEP_PPB) {
        return;
    }
    esp_err_t err = digi_trim(want);
    s_write_wait = DRIFT_WRITE_GAP_PERIODS;
    s_trim_writes++;
    s_trim_errors += err != ESP_OK;
    s_trim = err == ESP_OK ? want : st.trim_ppb;
    s_fail_streak = err == ESP_OK ? 0 : s_fail_streak + 1;
    if (s_fail_streak >= DRIFT_FAIL_LIMIT) {
        s_fail_streak = 0;
        s_retry_wait = DRIFT_RETRY_PERIODS;
        ESP_LOGW(TAG, "PLL trim writes keep failing (I2C), correcting drift by sample slip; retry in %d s", DRIFT_RETRY_PERIODS * DRIFT_PERIOD_MS / 1000);
    }
}

static void drift_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DRIFT_PERIOD_MS));
        drift_step();
    }
}

esp_err_t play_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");
    s_size = CONFIG_APP_RTP_PSRAM_BUFFER_KB * 1024;
    s_ring = heap_caps_malloc(s_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_psram = s_ring != NULL;
    if (!s_ring) {
        s_size = CONFIG_APP_RTP_BUFFER_KB * 1024;
        s_ring = heap_caps_malloc(s_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(s_ring, ESP_ERR_NO_MEM, TAG, "%d KB buffer", CONFIG_APP_RTP_BUFFER_KB);
        ESP_LOGW(TAG, "no PSRAM, %d KB buffer in internal RAM", CONFIG_APP_RTP_BUFFER_KB);
    }
    ESP_RETURN_ON_FALSE(xTaskCreate(play_task, "rtp_play", PLAY_TASK_STACK, NULL, PLAY_TASK_PRIO, &s_task) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    ESP_RETURN_ON_FALSE(xTaskCreate(drift_task, "rtp_drift", DRIFT_TASK_STACK, NULL, DRIFT_TASK_PRIO, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "drift task");
    return ESP_OK;
}

void play_format(uint32_t bytes_per_frame, uint32_t rate)
{
    if (bytes_per_frame > PLAY_MAX_BYTES_PER_FRAME || bytes_per_frame % 2) {
        ESP_LOGW(TAG, "can't play %lu bytes/frame", (unsigned long)bytes_per_frame);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_rate_checked = s_rate_checked && s_rate == rate;
    s_bpf = bytes_per_frame;
    s_rate = rate;
    s_cap = s_size / bytes_per_frame * bytes_per_frame;
    s_head = 0;
    s_fill = 0;
    s_debt = 0;
    s_state = RTP_PLAY_BUFFERING;
    xSemaphoreGive(s_lock);
}

void play_push(const uint8_t *data, size_t bytes)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    push_locked(data, bytes);
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

void play_silence(uint32_t frames)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    push_locked(NULL, (size_t)frames * s_bpf);
    xSemaphoreGive(s_lock);
}

void play_rebase(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_debt = 0;
    xSemaphoreGive(s_lock);
}

uint32_t play_max_gap_frames(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t frames = s_bpf ? target_bytes() / s_bpf : 0;
    xSemaphoreGive(s_lock);
    return frames;
}

void play_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = RTP_PLAY_IDLE;
    s_fill = 0;
    s_debt = 0;
    xSemaphoreGive(s_lock);
}

static uint32_t to_ms(uint64_t frames)
{
    return s_rate ? (uint32_t)(frames * 1000 / s_rate) : 0;
}

void play_get(rtp_stats_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->play_state = s_state;
    out->buffer_psram = s_psram;
    out->buffer_ms = s_bpf ? to_ms(s_fill / s_bpf) : 0;
    out->buffer_level_ms = to_ms(s_level);
    out->target_ms = s_bpf ? to_ms(target_bytes() / s_bpf) : 0;
    out->buffer_max_ms = s_bpf ? to_ms(s_cap / s_bpf) : 0;
    out->underruns = s_underruns;
    out->rebuffers = s_rebuffers;
    out->late_dropped_ms = to_ms(s_late_dropped);
    out->overflows = s_overflows;
    out->write_errors = s_write_errors;
    out->trim_ppb = s_trim;
    out->offset_ppb = s_offset;
    out->trim_writes = s_trim_writes;
    out->trim_errors = s_trim_errors;
    out->slip_ppb = s_slip_ppb;
    out->slipped = s_slipped;
    xSemaphoreGive(s_lock);
}
