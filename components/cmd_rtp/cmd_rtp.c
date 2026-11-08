#include <stdio.h>
#include <stdlib.h>
#include "esp_check.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "sdkconfig.h"
#include "rtp.h"
#include "settings.h"
#include "shell.h"
#include "cmd_rtp.h"

static const char *TAG = "cmd_rtp";

static const char *play_name(rtp_play_state_t s)
{
    switch (s) {
    case RTP_PLAY_BUFFERING: return "buffering";
    case RTP_PLAY_RUNNING: return "playing";
    default: return "idle";
    }
}

static const char *format_name(uint32_t bpf)
{
    switch (bpf) {
    case 4: return "L16 stereo";
    case 6: return "L24 stereo";
    case 8: return "L32 stereo";
    default: return "unknown";
    }
}

static const char *ppm_str(char buf[16], int32_t ppb)
{
    unsigned long mag = labs((long)ppb);
    snprintf(buf, 16, "%c%lu.%01lu", ppb < 0 ? '-' : '+', mag / 1000, mag % 1000 / 100);
    return buf;
}

static int cmd_status(int argc, char **argv)
{
    rtp_stats_t s;
    rtp_get(&s);
    int64_t now = esp_timer_get_time();
    settings_t cfg;
    settings_get(&cfg);
    printf("listen   RTP UDP %d, SAP UDP %d, mDNS %s.%s on %s.local\n", CONFIG_APP_RTP_PORT, CONFIG_APP_RTP_SAP_PORT, RTP_MDNS_SERVICE, RTP_MDNS_PROTO, cfg.hostname);
    printf("senders  ");
    if (!s.leases) {
        printf("none allowed: tools/esp32dif-sink with SPDIF_PASSWORD gets a %d s lease", CONFIG_APP_RTP_LEASE_S);
    }
    for (int i = 0; i < s.leases; i++) {
        struct in_addr a = { .s_addr = s.lease[i].ip };
        char a_str[16];
        inet_ntoa_r(a, a_str, sizeof(a_str));
        printf("%s%s (%lld s left)", i ? ", " : "", a_str, (s.lease[i].until_us - now) / 1000000);
    }
    printf("; %lu packets denied\n", (unsigned long)s.denied);
    if (s.session.valid) {
        struct in_addr origin = { .s_addr = s.session.origin_ip };
        char origin_str[16];
        inet_ntoa_r(origin, origin_str, sizeof(origin_str));
        printf("session  '%s' %s/%lu/%u from %s, announced %lld s ago\n", s.session.name, s.session.encoding, (unsigned long)s.session.rate, s.session.channels, origin_str, (now - s.session.seen_us) / 1000000);
    } else {
        printf("session  no SAP announcement: playback waits for one (tools/esp32dif-sink sends it)\n");
    }
    if (!s.streams) {
        printf("stream   none yet\n");
        return 0;
    }
    struct in_addr ip = { .s_addr = s.src_ip };
    char ip_str[16];
    inet_ntoa_r(ip, ip_str, sizeof(ip_str));
    if (s.active) {
        printf("stream   from %s:%u for %lld s, ssrc 0x%08lx, payload type %u\n", ip_str, s.src_port, (now - s.started_us) / 1000000, (unsigned long)s.ssrc, s.pt);
    } else {
        printf("stream   stopped %lld s ago, was from %s:%u\n", (now - s.last_us) / 1000000, ip_str, s.src_port);
    }
    if (s.bytes_per_frame) {
        unsigned long us = s.rate_hz ? (unsigned long)((uint64_t)s.frames_per_packet * 1000000 / s.rate_hz) : 0;
        printf("format   %s, %lu Hz, %lu frames/packet (%lu.%01lu ms)\n", format_name(s.bytes_per_frame), (unsigned long)s.rate_hz, (unsigned long)s.frames_per_packet, us / 1000, us % 1000 / 100);
    }
    printf("packets  %lu, lost %lu, late %lu, invalid %lu, %llu KB payload\n", (unsigned long)s.packets, (unsigned long)s.lost, (unsigned long)s.late, (unsigned long)s.invalid, s.payload_bytes / 1024);
    char a[16], b[16], c[16];
    printf("timing   jitter %lu.%01lu ms, max gap %lu ms, timestamp rebases %lu, sender clock %s ppm vs ESP32 (since last rebase)\n", (unsigned long)s.jitter_us / 1000, (unsigned long)s.jitter_us % 1000 / 100, (unsigned long)s.max_gap_us / 1000, (unsigned long)s.rebases, ppm_str(a, s.drift_ppb));
    printf("buffer   %lu ms now, %lu ms level, target %lu ms, capacity %lu ms in %s\n", (unsigned long)s.buffer_ms, (unsigned long)s.buffer_level_ms, (unsigned long)s.target_ms, (unsigned long)s.buffer_max_ms, s.buffer_psram ? "PSRAM" : "internal RAM");
    printf("play     %s, underruns %lu, rebuffers %lu, late audio dropped %lu ms, overflows %lu, write errors %lu\n", play_name(s.play_state), (unsigned long)s.underruns, (unsigned long)s.rebuffers, (unsigned long)s.late_dropped_ms, (unsigned long)s.overflows, (unsigned long)s.write_errors);
    char d[16];
    printf("drift    PLL trim %s ppm, sender clock offset %s ppm, %lu trim writes, %lu failed; sample slip %s ppm, %lu frames slipped\n", ppm_str(b, s.trim_ppb), ppm_str(c, s.offset_ppb), (unsigned long)s.trim_writes, (unsigned long)s.trim_errors, ppm_str(d, s.slip_ppb), (unsigned long)s.slipped);
    return 0;
}

static int cmd_reset(int argc, char **argv)
{
    rtp_reset();
    return 0;
}

static const shell_subcmd_t s_subcmds[] = {
    { .name = "status", .help = "Stream source, format, loss, jitter, sender clock drift, playback buffer", .func = cmd_status },
    { .name = "reset", .help = "Clear the counters and restart playback buffering", .func = cmd_reset },
};

static int cmd_rtp(int argc, char **argv)
{
    return shell_subcmd_run("rtp", s_subcmds, sizeof(s_subcmds) / sizeof(s_subcmds[0]), argc, argv);
}

esp_err_t cmd_rtp_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "rtp",
        .help = "RTP audio receiver: status | reset",
        .hint = "<command>",
        .func = cmd_rtp,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register rtp");
    return ESP_OK;
}
