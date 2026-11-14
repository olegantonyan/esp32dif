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
#define OLED_BIG_CELL_W 12
#define OLED_CYR_FIRST 0xC0
#define OLED_CYR_YO 0xA8
#define OLED_CYR_YO_SMALL 0xB8
#define SSD1306_CMD_SET_CONTRAST 0x81

#ifdef CONFIG_APP_OLED_BACK_FLIP
#define OLED_BACK_FLIP true
#else
#define OLED_BACK_FLIP false
#endif
#ifdef CONFIG_APP_OLED_FRONT_FLIP
#define OLED_FRONT_FLIP true
#else
#define OLED_FRONT_FLIP false
#endif

static const char *TAG = "oled";

typedef struct {
    const char *name;
    bool flip;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    oled_lines_t lines;
    oled_lines_t shown;
    bool stale[OLED_MAX_ROWS];
    oled_state_t state;
    oled_auto_fn_t auto_fn;
    uint8_t big_rows;
    bool refresh;
    volatile bool wake;
    int64_t last_auto;
    int64_t last_activity;
} display_t;

static display_t s_disp[OLED_COUNT] = {
    [OLED_BACK] = { .name = "back", .flip = OLED_BACK_FLIP, .state = { .addr = CONFIG_APP_OLED_BACK_ADDR, .height = CONFIG_APP_OLED_BACK_HEIGHT } },
    [OLED_FRONT] = { .name = "front", .flip = OLED_FRONT_FLIP, .state = { .addr = CONFIG_APP_OLED_FRONT_ADDR, .height = CONFIG_APP_OLED_FRONT_HEIGHT } },
};
static SemaphoreHandle_t s_lock;
static uint8_t s_fb[2 * OLED_WIDTH];

static int rows(const display_t *d)
{
    return d->state.height / 8;
}

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

static uint16_t stretch(uint8_t column)
{
    uint16_t out = 0;
    for (int bit = 0; bit < 8; bit++) {
        if (column & (1 << bit)) {
            out |= 3 << (2 * bit);
        }
    }
    return out;
}

static void render(const char *text)
{
    uint8_t *out = s_fb;
    for (const char *p = text; *p && out + 5 <= s_fb + OLED_WIDTH; p++, out += OLED_CELL_W) {
        memcpy(out, glyph(*p), 5);
    }
}

static void render_big(const char *text)
{
    int x = 0;
    for (const char *p = text; *p && x + OLED_BIG_CELL_W <= OLED_WIDTH; p++) {
        const uint8_t *g = glyph(*p);
        for (int col = 0; col < 10; col++, x++) {
            uint16_t v = stretch(g[col / 2]);
            s_fb[x] = v;
            s_fb[OLED_WIDTH + x] = v >> 8;
        }
        x += OLED_BIG_CELL_W - 10;
    }
}

static bool big_row(const display_t *d, int row)
{
    return d->state.auto_mode && (d->big_rows & (1 << row)) && row + 1 < rows(d);
}

static void draw_changed(display_t *d)
{
    for (int row = 0; row < rows(d); row++) {
        bool big = big_row(d, row);
        if (!d->stale[row] && !(big && d->stale[row + 1]) && !strcmp(d->shown[row], d->lines[row])) {
            row += big;
            continue;
        }
        int height = big ? 16 : 8;
        memset(s_fb, 0, sizeof(s_fb));
        if (big) {
            render_big(d->lines[row]);
        } else {
            render(d->lines[row]);
        }
        i2c_bus_lock();
        esp_err_t err = esp_lcd_panel_draw_bitmap(d->panel, 0, row * 8, OLED_WIDTH, row * 8 + height, s_fb);
        i2c_bus_unlock();
        strlcpy(d->shown[row], d->lines[row], sizeof(d->shown[row]));
        d->stale[row] = err != ESP_OK;
        if (big) {
            row++;
            d->stale[row] = err != ESP_OK;
        }
    }
}

static void mark_stale(display_t *d)
{
    for (int row = 0; row < OLED_MAX_ROWS; row++) {
        d->stale[row] = true;
    }
}

static void sleep_on_idle(display_t *d, int64_t now)
{
    if (d->wake) {
        d->wake = false;
        d->last_activity = now;
    }
    bool idle = d->state.idle_s && now - d->last_activity >= (int64_t)d->state.idle_s * 1000000;
    if (!d->state.on || idle == d->state.asleep) {
        return;
    }
    i2c_bus_lock();
    esp_err_t err = esp_lcd_panel_disp_on_off(d->panel, !idle);
    i2c_bus_unlock();
    if (err == ESP_OK) {
        d->state.asleep = idle;
    }
}

static void update(display_t *d, int64_t now)
{
    if (d->state.auto_mode && d->auto_fn && (d->refresh || now - d->last_auto >= OLED_AUTO_REFRESH_US)) {
        memset(d->lines, 0, sizeof(d->lines));
        d->auto_fn(d->lines, rows(d));
        d->last_auto = now;
        d->refresh = false;
    }
    sleep_on_idle(d, now);
    if (d->state.on && !d->state.asleep) {
        draw_changed(d);
    }
}

static void oled_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < OLED_COUNT; i++) {
            if (s_disp[i].state.ready) {
                update(&s_disp[i], now);
            }
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(OLED_TICK_MS));
    }
}

static esp_err_t start(display_t *d, i2c_master_bus_handle_t bus)
{
    if (i2c_master_probe(bus, d->state.addr, 100) != ESP_OK) {
        ESP_LOGW(TAG, "no %s display at 0x%02x (SDA GPIO%d, SCL GPIO%d)", d->name, d->state.addr, CONFIG_APP_I2C_SDA, CONFIG_APP_I2C_SCL);
        return ESP_ERR_NOT_FOUND;
    }
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = d->state.addr,
        .scl_speed_hz = 400 * 1000,
        .transaction_timeout_ms = 1000,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &d->io), TAG, "%s panel io", d->name);
    esp_lcd_panel_ssd1306_config_t ssd_cfg = { .height = d->state.height };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(d->io, &panel_cfg, &d->panel), TAG, "%s panel", d->name);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(d->panel), TAG, "%s reset", d->name);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(d->panel), TAG, "%s init", d->name);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(d->panel, d->flip, d->flip), TAG, "%s mirror", d->name);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(d->panel, true), TAG, "%s on", d->name);
    mark_stale(d);
    d->state.on = true;
    d->state.contrast = 0x7F;
    d->state.idle_s = CONFIG_APP_OLED_IDLE_S;
    d->refresh = true;
    d->wake = true;
    d->state.ready = true;
    return ESP_OK;
}

esp_err_t oled_init(void)
{
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_bus_get(&bus), TAG, "i2c bus");
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");
    int found = 0;
    for (int i = 0; i < OLED_COUNT; i++) {
        found += start(&s_disp[i], bus) == ESP_OK;
    }
    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_FALSE(xTaskCreate(oled_task, "oled", 4096, NULL, 1, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

const char *oled_name(oled_id_t id)
{
    return id < OLED_COUNT ? s_disp[id].name : "?";
}

int oled_rows(oled_id_t id)
{
    return id < OLED_COUNT ? rows(&s_disp[id]) : 0;
}

static display_t *lock(oled_id_t id)
{
    if (id >= OLED_COUNT || !s_disp[id].state.ready) {
        return NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return &s_disp[id];
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

void oled_set_auto(oled_id_t id, oled_auto_fn_t fn, uint8_t big_rows)
{
    display_t *d = lock(id);
    if (d) {
        d->auto_fn = fn;
        d->big_rows = big_rows;
        mark_stale(d);
        d->state.auto_mode = true;
        d->refresh = true;
        unlock();
    }
}

bool oled_auto(oled_id_t id)
{
    display_t *d = lock(id);
    if (!d) {
        return false;
    }
    bool ok = d->auto_fn != NULL;
    if (ok) {
        if (!d->state.auto_mode) {
            mark_stale(d);
        }
        d->state.auto_mode = true;
        d->refresh = true;
        d->wake = true;
    }
    unlock();
    return ok;
}

static void set_manual(display_t *d)
{
    if (d->state.auto_mode) {
        d->state.auto_mode = false;
        memset(d->lines, 0, sizeof(d->lines));
        mark_stale(d);
    }
    d->wake = true;
}

void oled_print(oled_id_t id, int row, const char *text)
{
    if (row < 0 || row >= oled_rows(id)) {
        return;
    }
    display_t *d = lock(id);
    if (d) {
        set_manual(d);
        strlcpy(d->lines[row], text, sizeof(d->lines[row]));
        unlock();
    }
}

void oled_clear(oled_id_t id)
{
    display_t *d = lock(id);
    if (d) {
        set_manual(d);
        memset(d->lines, 0, sizeof(d->lines));
        unlock();
    }
}

esp_err_t oled_power(oled_id_t id, bool on)
{
    display_t *d = lock(id);
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_STATE, TAG, "no display");
    i2c_bus_lock();
    esp_err_t err = esp_lcd_panel_disp_on_off(d->panel, on);
    i2c_bus_unlock();
    if (err == ESP_OK) {
        d->state.on = on;
        d->state.asleep = false;
        d->wake |= on;
    }
    unlock();
    return err;
}

esp_err_t oled_contrast(oled_id_t id, uint8_t level)
{
    display_t *d = lock(id);
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_STATE, TAG, "no display");
    i2c_bus_lock();
    esp_err_t err = esp_lcd_panel_io_tx_param(d->io, SSD1306_CMD_SET_CONTRAST, &level, 1);
    i2c_bus_unlock();
    if (err == ESP_OK) {
        d->state.contrast = level;
    }
    unlock();
    return err;
}

void oled_idle(oled_id_t id, uint32_t seconds)
{
    display_t *d = lock(id);
    if (d) {
        d->state.idle_s = seconds;
        d->wake = true;
        unlock();
    }
}

void oled_wake(void)
{
    for (int i = 0; i < OLED_COUNT; i++) {
        s_disp[i].wake = true;
    }
}

void oled_get(oled_id_t id, oled_state_t *out)
{
    display_t *d = lock(id);
    if (d) {
        *out = d->state;
        unlock();
    } else if (id < OLED_COUNT) {
        *out = s_disp[id].state;
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
