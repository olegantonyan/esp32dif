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
#define OLED_AUTO_REFRESH_US (1000 * 1000)
#define OLED_CELL_W 6
#define SSD1306_CMD_SET_CONTRAST 0x81

static const char *TAG = "oled";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_lock;
static oled_lines_t s_lines;
static uint8_t s_fb[OLED_WIDTH * OLED_ROWS];
static oled_state_t s_state = { .on = true, .contrast = 0x7F };
static oled_auto_fn_t s_auto_fn;
static bool s_dirty = true;

static void render(void)
{
    memset(s_fb, 0, sizeof(s_fb));
    for (int row = 0; row < OLED_ROWS; row++) {
        uint8_t *out = s_fb + row * OLED_WIDTH;
        for (const char *p = s_lines[row]; *p; p++, out += OLED_CELL_W) {
            unsigned char c = *p;
            if (c < FONT5X7_FIRST || c > FONT5X7_LAST) {
                c = '?';
            }
            memcpy(out, font5x7[c - FONT5X7_FIRST], 5);
        }
    }
}

static void oled_task(void *arg)
{
    int64_t last_auto = 0;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int64_t now = esp_timer_get_time();
        if (s_state.auto_mode && s_auto_fn && (s_dirty || now - last_auto >= OLED_AUTO_REFRESH_US)) {
            memset(s_lines, 0, sizeof(s_lines));
            s_auto_fn(s_lines);
            last_auto = now;
            s_dirty = true;
        }
        if (s_dirty) {
            render();
            esp_lcd_panel_draw_bitmap(s_panel, 0, 0, OLED_WIDTH, OLED_HEIGHT, s_fb);
            s_dirty = false;
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

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");
    ESP_RETURN_ON_FALSE(xTaskCreate(oled_task, "oled", 3072, NULL, 1, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
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
        s_dirty = true;
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
        s_dirty = true;
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
    s_dirty = true;
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

void oled_get(oled_state_t *out)
{
    if (lock()) {
        *out = s_state;
        xSemaphoreGive(s_lock);
    } else {
        *out = s_state;
    }
}
