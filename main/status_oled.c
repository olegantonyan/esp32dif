#include <stdio.h>
#include "digi.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "led.h"
#include "status_oled.h"

#define DIGI_BITS 24

static void audio_lines(char *format, char *source, size_t len)
{
    digi_state_t st;
    digi_get(&st);
    if (!st.ready && !st.blind) {
        snprintf(format, len, "S/PDIF off");
        snprintf(source, len, "no WM8804");
        return;
    }
    unsigned khz = st.rate / 1000, tenths = st.rate % 1000 / 100;
    if (tenths) {
        snprintf(format, len, "%u.%ukHz %d-bit", khz, tenths, DIGI_BITS);
    } else {
        snprintf(format, len, "%ukHz %d-bit", khz, DIGI_BITS);
    }
    if (st.tone_hz) {
        snprintf(source, len, "tone %luHz %u%%%s", (unsigned long)st.tone_hz, st.tone_level, st.blind ? " noI2C" : "");
    } else {
        snprintf(source, len, "no tone%s", st.blind ? " noI2C" : "");
    }
}

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

    audio_lines(lines[0], lines[1], sizeof(lines[0]));
    snprintf(lines[2], sizeof(lines[2]), "up %ud %02u:%02u:%02u", (unsigned)(s / 86400), (unsigned)(s / 3600 % 24), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    snprintf(lines[3], sizeof(lines[3]), "led %s%s%s", led_effect_name(led.effect), colored ? " " : "", colored ? name : "");
#if OLED_ROWS > 4
    snprintf(lines[4], sizeof(lines[4]), "heap %u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));
#endif
}
