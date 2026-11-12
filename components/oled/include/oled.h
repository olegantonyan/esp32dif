#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#define OLED_WIDTH 128
#define OLED_HEIGHT CONFIG_APP_OLED_HEIGHT
#define OLED_COLS 21
#define OLED_ROWS (OLED_HEIGHT / 8)
#define OLED_ICON_PLAY '\x80'
#define OLED_ICON_PAUSE '\x81'
#define OLED_ICON_STOP '\x82'

typedef char oled_lines_t[OLED_ROWS][OLED_COLS + 1];

typedef void (*oled_auto_fn_t)(oled_lines_t lines);

typedef struct {
    bool ready;
    bool auto_mode;
    bool on;
    bool asleep;
    uint8_t contrast;
    uint32_t idle_s;
} oled_state_t;

esp_err_t oled_init(void);
void oled_set_auto(oled_auto_fn_t fn);
bool oled_auto(void);
void oled_print(int row, const char *text);
void oled_clear(void);
esp_err_t oled_power(bool on);
esp_err_t oled_contrast(uint8_t level);
void oled_idle(uint32_t seconds);
void oled_wake(void);
void oled_get(oled_state_t *out);
size_t oled_from_utf8(char *dst, size_t len, const char *src);
