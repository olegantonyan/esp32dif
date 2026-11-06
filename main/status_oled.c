#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "led.h"
#include "status_oled.h"

void status_oled_screen(oled_lines_t lines)
{
    uint32_t s = esp_timer_get_time() / 1000000;
    led_state_t led;
    led_get(&led);
    const char *name = led_color_name(led.color);
    char color[8];
    if (!name) {
        snprintf(color, sizeof(color), "#%02x%02x%02x", led.color.r, led.color.g, led.color.b);
        name = color;
    }
    bool colored = led.effect != LED_OFF && led.effect != LED_RAINBOW;

    snprintf(lines[0], sizeof(lines[0]), "ESP32-C5");
    snprintf(lines[1], sizeof(lines[1]), "up %ud %02u:%02u:%02u", (unsigned)(s / 86400), (unsigned)(s / 3600 % 24), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    snprintf(lines[2], sizeof(lines[2]), "led %s%s%s", led_effect_name(led.effect), colored ? " " : "", colored ? name : "");
    snprintf(lines[3], sizeof(lines[3]), "heap %u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));
}
