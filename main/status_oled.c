#include <stdio.h>
#include <string.h>
#include "digi.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "ir.h"
#include "net.h"
#include "oled.h"
#include "remote.h"
#include "rtp.h"
#include "settings.h"
#include "status_oled.h"

#define DIGI_BITS 24
#define MARQUEE_STEP_US (250 * 1000)
#define MARQUEE_PAUSE_STEPS 6
#define MARQUEE_GAP 3

typedef struct {
    char song[2 * REMOTE_TRACK_MAX + 3];
    char player[REMOTE_PLAYER_MAX + REMOTE_ID_MAX + 3];
    char icon;
} track_t;

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

typedef struct {
    uint32_t ir_seq;
    bool digi_ready, digi_blind;
    uint32_t rate, tone_hz;
    rtp_play_state_t play_state;
    bool has_ip, associated, reconnect, ap_active;
    int ap_clients;
    settings_t cfg;
    track_t track;
} activity_t;

static void wake_on_activity(const settings_t *cfg, const track_t *track)
{
    static activity_t last, a;
    memset(&a, 0, sizeof(a));
    static ir_frame_t ir;
    ir_get(&ir);
    digi_state_t digi;
    digi_get(&digi);
    rtp_stats_t rtp;
    rtp_get(&rtp);
    a.ir_seq = ir.seq;
    a.digi_ready = digi.ready;
    a.digi_blind = digi.blind;
    a.rate = digi.rate;
    a.tone_hz = digi.tone_hz;
    a.play_state = rtp.play_state;
    a.has_ip = net_has_ip();
    a.associated = net_associated();
    a.reconnect = net_reconnect_enabled();
    a.ap_active = net_ap_active();
    a.ap_clients = net_ap_clients();
    a.cfg = *cfg;
    a.track = *track;
    if (memcmp(&a, &last, sizeof(a))) {
        last = a;
        oled_wake();
    }
}

static char status_icon(const char *status)
{
    if (!strcmp(status, "Playing")) {
        return OLED_ICON_PLAY;
    }
    if (!strcmp(status, "Paused")) {
        return OLED_ICON_PAUSE;
    }
    return strcmp(status, "Stopped") ? 0 : OLED_ICON_STOP;
}

static bool current_track(track_t *t)
{
    static rtp_stats_t rtp;
    static remote_track_t raw;
    static char text[sizeof(t->song)];
    memset(t, 0, sizeof(*t));
    rtp_get(&rtp);
    if (!remote_track(rtp.src_ip, &raw)) {
        return false;
    }
    snprintf(text, sizeof(text), "%s%s%s", raw.artist, raw.artist[0] && raw.title[0] ? " - " : "", raw.title);
    oled_from_utf8(t->song, sizeof(t->song), text);
    snprintf(text, sizeof(text), raw.id[0] ? "%s (%s)" : "%s", raw.player, raw.id);
    oled_from_utf8(t->player, sizeof(t->player), text);
    t->icon = status_icon(raw.status);
    return true;
}

static void marquee(char *dst, size_t len, const char *text, int64_t elapsed_us)
{
    size_t n = strlen(text), cols = len - 1;
    if (n <= cols) {
        strlcpy(dst, text, len);
        return;
    }
    size_t loop = n + MARQUEE_GAP;
    size_t step = elapsed_us / MARQUEE_STEP_US % (loop + MARQUEE_PAUSE_STEPS);
    size_t start = step < MARQUEE_PAUSE_STEPS ? 0 : step - MARQUEE_PAUSE_STEPS;
    for (size_t i = 0; i < cols; i++) {
        size_t k = (start + i) % loop;
        dst[i] = k < n ? text[k] : ' ';
    }
    dst[cols] = '\0';
}

static void icon_marquee(char *dst, size_t len, char icon, const char *text, int64_t elapsed_us)
{
    if (icon) {
        dst[0] = icon;
        dst[1] = ' ';
        dst += 2;
        len -= 2;
    }
    marquee(dst, len, text, elapsed_us);
}

void status_oled_screen(oled_lines_t lines)
{
    static track_t track, shown;
    static int64_t track_since_us;
    int64_t now = esp_timer_get_time();
    uint32_t s = now / 1000000;
    settings_t cfg;
    settings_get(&cfg);
    bool has_track = current_track(&track);
    shown.icon = track.icon;
    if (memcmp(&track, &shown, sizeof(track))) {
        shown = track;
        track_since_us = now;
    }
    wake_on_activity(&cfg, &track);

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
    if (has_track) {
        icon_marquee(lines[6], sizeof(lines[6]), track.icon, track.player, now - track_since_us);
        marquee(lines[7], sizeof(lines[7]), track.song, now - track_since_us);
    } else {
        snprintf(lines[6], sizeof(lines[6]), "up %ud %02u:%02u:%02u", (unsigned)(s / 86400), (unsigned)(s / 3600 % 24), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
        snprintf(lines[7], sizeof(lines[7]), "heap %u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));
    }
#else
    snprintf(lines[3], sizeof(lines[3]), "pw %.18s", cfg.password);
    if (has_track) {
        icon_marquee(lines[1], sizeof(lines[1]), track.icon, track.song, now - track_since_us);
    }
#endif
}
