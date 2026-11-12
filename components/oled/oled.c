#include <string.h>
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "font5x7.h"
#include "i2c_bus.h"
#include "oled.h"

#define OLED_TICK_MS 100
#define OLED_AUTO_REFRESH_US (250 * 1000)
#define OLED_CELL_W 6
#define OLED_CYR_FIRST 0xC0
#define OLED_CYR_YO 0xA8
#define OLED_CYR_YO_SMALL 0xB8
#define SSD1306_CMD_SET_CONTRAST 0x81

static const char *TAG = "oled";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_lock;
static oled_lines_t s_lines;
static oled_lines_t s_shown;
static bool s_stale[OLED_ROWS];
static uint8_t s_fb[OLED_WIDTH];
static oled_state_t s_state = { .on = true, .contrast = 0x7F, .idle_s = CONFIG_APP_OLED_IDLE_S };
static oled_auto_fn_t s_auto_fn;
static bool s_refresh = true;
static volatile bool s_wake = true;

static const uint8_t *glyph(unsigned char c)
{
    if (c >= OLED_CYR_FIRST) {
        return font5x7_cyr[c - OLED_CYR_FIRST];
    }
    if (c >= (unsigned char)OLED_ICON_PLAY && c < (unsigned char)OLED_ICON_PLAY + FONT5X7_ICON_COUNT) {
        return font5x7_icons[c - (unsigned char)OLED_ICON_PLAY];
    }
    if (c == OLED_CYR_YO || c == OLED_CYR_YO_SMALL) {
        return font5x7_cyr[FONT5X7_CYR_COUNT - (c == OLED_CYR_YO ? 2 : 1)];
    }
    if (c < FONT5X7_FIRST || c > FONT5X7_LAST) {
        c = '?';
    }
    return font5x7[c - FONT5X7_FIRST];
}

static void draw_changed(void)
{
    for (int row = 0; row < OLED_ROWS; row++) {
        if (!s_stale[row] && !strcmp(s_shown[row], s_lines[row])) {
            continue;
        }
        memset(s_fb, 0, sizeof(s_fb));
        uint8_t *out = s_fb;
        for (const char *p = s_lines[row]; *p; p++, out += OLED_CELL_W) {
            memcpy(out, glyph(*p), 5);
        }
        i2c_bus_lock();
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, row * 8, OLED_WIDTH, row * 8 + 8, s_fb);
        i2c_bus_unlock();
        s_stale[row] = err != ESP_OK;
        strlcpy(s_shown[row], s_lines[row], sizeof(s_shown[row]));
    }
}

static void sleep_on_idle(int64_t now, int64_t *last_activity)
{
    if (s_wake) {
        s_wake = false;
        *last_activity = now;
    }
    bool idle = s_state.idle_s && now - *last_activity >= (int64_t)s_state.idle_s * 1000000;
    if (!s_state.on || idle == s_state.asleep) {
        return;
    }
    i2c_bus_lock();
    esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, !idle);
    i2c_bus_unlock();
    if (err == ESP_OK) {
        s_state.asleep = idle;
    }
}

static void oled_task(void *arg)
{
    int64_t last_auto = 0;
    int64_t last_activity = 0;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int64_t now = esp_timer_get_time();
        if (s_state.auto_mode && s_auto_fn && (s_refresh || now - last_auto >= OLED_AUTO_REFRESH_US)) {
            memset(s_lines, 0, sizeof(s_lines));
            s_auto_fn(s_lines);
            last_auto = now;
            s_refresh = false;
        }
        sleep_on_idle(now, &last_activity);
        if (s_state.on && !s_state.asleep) {
            draw_changed();
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(OLED_TICK_MS));
    }
}

esp_err_t oled_init(void)
{
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_bus_get(&bus), TAG, "i2c bus");
    if (i2c_master_probe(bus, CONFIG_APP_OLED_ADDR, 100) != ESP_OK) {
        ESP_LOGW(TAG, "no display at 0x%02x (SDA GPIO%d, SCL GPIO%d)", CONFIG_APP_OLED_ADDR, CONFIG_APP_I2C_SDA, CONFIG_APP_I2C_SCL);
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = CONFIG_APP_OLED_ADDR,
        .scl_speed_hz = 400 * 1000,
        .transaction_timeout_ms = 1000,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &s_io), TAG, "panel io");
    esp_lcd_panel_ssd1306_config_t ssd_cfg = { .height = OLED_HEIGHT };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(s_io, &panel_cfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "on");
    for (int row = 0; row < OLED_ROWS; row++) {
        s_stale[row] = true;
    }

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");
    ESP_RETURN_ON_FALSE(xTaskCreate(oled_task, "oled", 4096, NULL, 1, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    s_state.ready = true;
    return ESP_OK;
}

static bool lock(void)
{
    if (!s_state.ready) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return true;
}

void oled_set_auto(oled_auto_fn_t fn)
{
    if (lock()) {
        s_auto_fn = fn;
        s_state.auto_mode = true;
        s_refresh = true;
        xSemaphoreGive(s_lock);
    }
}

bool oled_auto(void)
{
    if (!lock()) {
        return false;
    }
    bool ok = s_auto_fn != NULL;
    if (ok) {
        s_state.auto_mode = true;
        s_refresh = true;
        s_wake = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

static void set_manual(void)
{
    if (s_state.auto_mode) {
        s_state.auto_mode = false;
        memset(s_lines, 0, sizeof(s_lines));
    }
    s_wake = true;
}

void oled_print(int row, const char *text)
{
    if (row < 0 || row >= OLED_ROWS || !lock()) {
        return;
    }
    set_manual();
    strlcpy(s_lines[row], text, sizeof(s_lines[row]));
    xSemaphoreGive(s_lock);
}

void oled_clear(void)
{
    if (lock()) {
        set_manual();
        memset(s_lines, 0, sizeof(s_lines));
        xSemaphoreGive(s_lock);
    }
}

esp_err_t oled_power(bool on)
{
    ESP_RETURN_ON_FALSE(lock(), ESP_ERR_INVALID_STATE, TAG, "no display");
    esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, on);
    if (err == ESP_OK) {
        s_state.on = on;
        s_state.asleep = false;
        s_wake |= on;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t oled_contrast(uint8_t level)
{
    ESP_RETURN_ON_FALSE(lock(), ESP_ERR_INVALID_STATE, TAG, "no display");
    esp_err_t err = esp_lcd_panel_io_tx_param(s_io, SSD1306_CMD_SET_CONTRAST, &level, 1);
    if (err == ESP_OK) {
        s_state.contrast = level;
    }
    xSemaphoreGive(s_lock);
    return err;
}

void oled_idle(uint32_t seconds)
{
    if (lock()) {
        s_state.idle_s = seconds;
        s_wake = true;
        xSemaphoreGive(s_lock);
    }
}

void oled_wake(void)
{
    s_wake = true;
}

void oled_get(oled_state_t *out)
{
    if (lock()) {
        *out = s_state;
        xSemaphoreGive(s_lock);
    } else {
        *out = s_state;
    }
}

static const char s_latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";

static uint32_t next_codepoint(const unsigned char **p)
{
    const unsigned char *s = *p;
    int extra = *s < 0x80 ? 0 : (*s & 0xE0) == 0xC0 ? 1 : (*s & 0xF0) == 0xE0 ? 2 : (*s & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0) {
        *p = s + 1;
        return '?';
    }
    uint32_t cp = extra ? *s & (0x3F >> extra) : *s;
    for (s++; extra > 0; extra--, s++) {
        if ((*s & 0xC0) != 0x80) {
            *p = s;
            return '?';
        }
        cp = cp << 6 | (*s & 0x3F);
    }
    *p = s;
    return cp;
}

static char display_char(uint32_t cp)
{
    if (cp >= FONT5X7_FIRST && cp <= FONT5X7_LAST) {
        return cp;
    }
    if (cp >= 0xC0 && cp <= 0xFF) {
        return s_latin1[cp - 0xC0];
    }
    if (cp >= 0x410 && cp <= 0x44F) {
        return OLED_CYR_FIRST + (cp - 0x410);
    }
    switch (cp) {
    case 0xA0: return ' ';
    case 0x401: return OLED_CYR_YO;
    case 0x451: return OLED_CYR_YO_SMALL;
    case 0x404: return 'E';
    case 0x454: return 'e';
    case 0x406: case 0x407: return 'I';
    case 0x456: case 0x457: return 'i';
    case 0x490: return OLED_CYR_FIRST + 3;
    case 0x491: return OLED_CYR_FIRST + 35;
    case 0x2010 ... 0x2015: return '-';
    case 0x2018 ... 0x201B: return '\'';
    case 0x201C ... 0x201F: return '"';
    case 0x2026: return '.';
    default: return '?';
    }
}

size_t oled_from_utf8(char *dst, size_t len, const char *src)
{
    const unsigned char *p = (const unsigned char *)src;
    size_t n = 0;
    while (*p && n + 1 < len) {
        dst[n++] = display_char(next_codepoint(&p));
    }
    if (len) {
        dst[n] = '\0';
    }
    return n;
}
