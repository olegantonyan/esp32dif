#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#define OLED_WIDTH 128
#define OLED_COLS 21
#define OLED_MAX_ROWS 8
#define OLED_BIG_COLS 10
#define OLED_ICON_PLAY '\x80'
#define OLED_ICON_PAUSE '\x81'
#define OLED_ICON_STOP '\x82'

typedef enum {
    OLED_BACK,
    OLED_FRONT,
    OLED_COUNT,
} oled_id_t;

typedef char oled_lines_t[OLED_MAX_ROWS][OLED_COLS + 1];

typedef void (*oled_auto_fn_t)(oled_lines_t lines, int rows);

typedef struct {
    uint8_t addr;
    uint8_t height;
    bool ready;
    bool auto_mode;
    bool on;
    bool asleep;
    uint8_t contrast;
    uint32_t idle_s;
} oled_state_t;

esp_err_t oled_init(void);
const char *oled_name(oled_id_t id);
int oled_rows(oled_id_t id);
void oled_set_auto(oled_id_t id, oled_auto_fn_t fn, uint8_t big_rows);
bool oled_auto(oled_id_t id);
void oled_print(oled_id_t id, int row, const char *text);
void oled_clear(oled_id_t id);
esp_err_t oled_power(oled_id_t id, bool on);
esp_err_t oled_contrast(oled_id_t id, uint8_t level);
void oled_idle(oled_id_t id, uint32_t seconds);
void oled_wake(void);
void oled_get(oled_id_t id, oled_state_t *out);
size_t oled_from_utf8(char *dst, size_t len, const char *src);
