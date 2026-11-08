#include "net.h"
#include "rtp.h"
#include "status_led.h"

void status_led_pattern(led_effect_t *effect, led_rgb_t *color, uint32_t *period_ms)
{
    rtp_stats_t rtp;
    rtp_get(&rtp);
    *color = (led_rgb_t){ 0 };
    *period_ms = 0;
    if (rtp.play_state == RTP_PLAY_RUNNING) {
        *effect = LED_RAINBOW;
        *period_ms = 6000;
    } else if (net_has_ip()) {
        *effect = LED_OFF;
    } else if (net_reconnect_enabled()) {
        *effect = LED_BLINK;
        *color = (led_rgb_t){ 255, 80, 0 };
        *period_ms = 1000;
    } else {
        *effect = LED_SOLID;
        *color = (led_rgb_t){ 255, 0, 0 };
    }
}
