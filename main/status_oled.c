#include <stdio.h>
#include <string.h>
#include "digi.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "net.h"
#include "rtp.h"
#include "settings.h"
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
    const char *suffix = st.blind ? " noI2C" : "";
    rtp_stats_t rtp;
    rtp_get(&rtp);
    if (st.tone_hz) {
        snprintf(source, len, "tone %luHz %u%%%s", (unsigned long)st.tone_hz, st.tone_level, suffix);
    } else if (rtp.play_state == RTP_PLAY_RUNNING) {
        snprintf(source, len, "rtp %lums buf%s", (unsigned long)rtp.buffer_ms, suffix);
    } else if (rtp.play_state == RTP_PLAY_BUFFERING) {
        snprintf(source, len, "rtp buffering%s", suffix);
    } else {
        snprintf(source, len, "idle%s", suffix);
    }
}

static void network_line(char *line, size_t len)
{
    if (net_has_ip()) {
        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(net_sta(), &ip);
        snprintf(line, len, IPSTR, IP2STR(&ip.ip));
    } else if (net_associated()) {
        snprintf(line, len, "wifi getting IP");
    } else if (net_reconnect_enabled()) {
        snprintf(line, len, "wifi connecting");
    } else {
        net_saved_t saved;
        snprintf(line, len, net_saved(&saved) == ESP_OK ? "wifi off" : "wifi not set up");
    }
}

void status_oled_screen(oled_lines_t lines)
{
    uint32_t s = esp_timer_get_time() / 1000000;
    settings_t cfg;
    settings_get(&cfg);

    audio_lines(lines[0], lines[1], sizeof(lines[0]));
    network_line(lines[2], sizeof(lines[2]));
    strlcpy(lines[3], cfg.hostname, sizeof(lines[3]));
    strlcat(lines[3], ".local", sizeof(lines[3]));
#if OLED_ROWS > 4
    if (net_ap_active()) {
        int n = net_ap_clients();
        snprintf(lines[4], sizeof(lines[4]), "AP on, %d client%s", n, n == 1 ? "" : "s");
    } else {
        snprintf(lines[4], sizeof(lines[4]), "AP off");
    }
    snprintf(lines[5], sizeof(lines[5]), "pw %.18s", cfg.password);
    snprintf(lines[6], sizeof(lines[6]), "up %ud %02u:%02u:%02u", (unsigned)(s / 86400), (unsigned)(s / 3600 % 24), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    snprintf(lines[7], sizeof(lines[7]), "heap %u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));
#else
    snprintf(lines[3], sizeof(lines[3]), "pw %.18s", cfg.password);
#endif
}
