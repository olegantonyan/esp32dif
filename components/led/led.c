#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "led.h"

#define LED_TICK_MS 20

static const char *TAG = "led";

static const led_preset_t s_presets[] = {
    { "red", { 255, 0, 0 } },
    { "green", { 0, 255, 0 } },
    { "blue", { 0, 0, 255 } },
    { "white", { 255, 255, 255 } },
    { "yellow", { 255, 180, 0 } },
    { "orange", { 255, 80, 0 } },
    { "cyan", { 0, 255, 255 } },
    { "magenta", { 255, 0, 255 } },
    { "purple", { 128, 0, 255 } },
    { "pink", { 255, 60, 120 } },
};

static const char *s_effect_names[] = {
    [LED_OFF] = "off",
    [LED_SOLID] = "solid",
    [LED_BLINK] = "blink",
    [LED_BREATHE] = "breathe",
    [LED_RAINBOW] = "rainbow",
};

static led_strip_handle_t s_strip;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static led_state_t s_state = { .brightness = 20 };
static led_auto_fn_t s_auto_fn;

static led_rgb_t scale(led_rgb_t c, uint32_t num, uint32_t den)
{
    return (led_rgb_t){ c.r * num / den, c.g * num / den, c.b * num / den };
}

static led_rgb_t hue_to_rgb(uint32_t hue)
{
    uint32_t sector = hue / 60, f = (hue % 60) * 255 / 60;
    switch (sector) {
    case 0: return (led_rgb_t){ 255, f, 0 };
    case 1: return (led_rgb_t){ 255 - f, 255, 0 };
    case 2: return (led_rgb_t){ 0, 255, f };
    case 3: return (led_rgb_t){ 0, 255 - f, 255 };
    case 4: return (led_rgb_t){ f, 0, 255 };
    default: return (led_rgb_t){ 255, 0, 255 - f };
    }
}

static led_rgb_t render(const led_state_t *st, uint32_t now_ms)
{
    uint32_t period = st->period_ms ? st->period_ms : 1000;
    uint32_t phase = now_ms % period;
    switch (st->effect) {
    case LED_SOLID:
        return st->color;
    case LED_BLINK:
        return phase < period / 2 ? st->color : (led_rgb_t){ 0 };
    case LED_BREATHE: {
        uint32_t half = period / 2;
        uint32_t level = phase < half ? phase * 255 / half : (period - phase) * 255 / half;
        return scale(st->color, level * level, 255 * 255);
    }
    case LED_RAINBOW:
        return hue_to_rgb(phase * 360 / period);
    default:
        return (led_rgb_t){ 0 };
    }
}

static void led_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    led_rgb_t shown = { 0 };
    bool first = true;
    for (;;) {
        led_state_t st;
        taskENTER_CRITICAL(&s_lock);
        st = s_state;
        led_auto_fn_t auto_fn = s_auto_fn;
        taskEXIT_CRITICAL(&s_lock);

        if (st.auto_mode && auto_fn) {
            auto_fn(&st.effect, &st.color, &st.period_ms);
            taskENTER_CRITICAL(&s_lock);
            if (s_state.auto_mode) {
                s_state.effect = st.effect;
                s_state.color = st.color;
                s_state.period_ms = st.period_ms;
            }
            taskEXIT_CRITICAL(&s_lock);
        }

        led_rgb_t c = scale(render(&st, esp_timer_get_time() / 1000), st.brightness, 100);
        if (first || memcmp(&c, &shown, sizeof(c)) != 0) {
            led_strip_set_pixel(s_strip, 0, c.r, c.g, c.b);
            led_strip_refresh(s_strip);
            shown = c;
            first = false;
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(LED_TICK_MS));
    }
}

esp_err_t led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_APP_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip), TAG, "rmt device");
    ESP_RETURN_ON_FALSE(xTaskCreate(led_task, "led", 2048, NULL, 1, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void led_set(led_effect_t effect, led_rgb_t color, uint32_t period_ms)
{
    taskENTER_CRITICAL(&s_lock);
    s_state.auto_mode = false;
    s_state.effect = effect;
    s_state.color = color;
    s_state.period_ms = period_ms;
    taskEXIT_CRITICAL(&s_lock);
}

void led_set_auto(led_auto_fn_t fn)
{
    taskENTER_CRITICAL(&s_lock);
    s_auto_fn = fn;
    s_state.auto_mode = true;
    taskEXIT_CRITICAL(&s_lock);
}

bool led_auto(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool ok = s_auto_fn != NULL;
    if (ok) {
        s_state.auto_mode = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    return ok;
}

void led_set_brightness(uint8_t percent)
{
    taskENTER_CRITICAL(&s_lock);
    s_state.brightness = percent > 100 ? 100 : percent;
    taskEXIT_CRITICAL(&s_lock);
}

void led_get(led_state_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_state;
    taskEXIT_CRITICAL(&s_lock);
}

bool led_color_parse(const char *s, led_rgb_t *out)
{
    for (size_t i = 0; i < sizeof(s_presets) / sizeof(s_presets[0]); i++) {
        if (strcasecmp(s, s_presets[i].name) == 0) {
            *out = s_presets[i].color;
            return true;
        }
    }
    unsigned r, g, b;
    char extra;
    if (s[0] == '#' && strlen(s) == 7 && sscanf(s + 1, "%2x%2x%2x%c", &r, &g, &b, &extra) == 3) {
        *out = (led_rgb_t){ r, g, b };
        return true;
    }
    if (sscanf(s, "%u,%u,%u%c", &r, &g, &b, &extra) == 3 && r <= 255 && g <= 255 && b <= 255) {
        *out = (led_rgb_t){ r, g, b };
        return true;
    }
    return false;
}

const char *led_color_name(led_rgb_t color)
{
    for (size_t i = 0; i < sizeof(s_presets) / sizeof(s_presets[0]); i++) {
        if (memcmp(&color, &s_presets[i].color, sizeof(color)) == 0) {
            return s_presets[i].name;
        }
    }
    return NULL;
}

const char *led_effect_name(led_effect_t effect)
{
    return (size_t)effect < sizeof(s_effect_names) / sizeof(s_effect_names[0]) ? s_effect_names[effect] : "?";
}

const led_preset_t *led_presets(size_t *count)
{
    *count = sizeof(s_presets) / sizeof(s_presets[0]);
    return s_presets;
}
