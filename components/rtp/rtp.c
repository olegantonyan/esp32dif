#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "rtp_priv.h"

#define RTP_VERSION 2
#define RTP_HEADER_BYTES 12
#define RTP_MAX_PACKET 1500
#define RTP_RECV_TIMEOUT_MS 200
#define RTP_TASK_STACK 4096
#define RTP_TASK_PRIO 8
#define RTP_DRIFT_MIN_US (1000 * 1000)
#define RTP_JITTER_SHIFT 4
#define RTP_JUMP_FRAMES 48000
#define SAP_GROUP "224.0.0.56"

static const char *TAG = "rtp";

typedef struct {
    uint16_t next_seq;
    uint32_t prev_ts;
    uint32_t prev_payload;
    uint64_t ts_total;
    int64_t first_us;
    int64_t prev_transit_us;
    int64_t jitter_x16;
    uint32_t write_ts;
    bool have_prev;
    bool playing;
    bool timeline;
    bool warned;
} track_t;

static SemaphoreHandle_t s_lock;
static rtp_stats_t s_stats;
static track_t s_track;
static rtp_session_t s_session;
static rtp_lease_t s_leases[RTP_LEASES];
static uint32_t s_denied;

static bool allowed(uint32_t ip, int64_t now)
{
    for (int i = 0; i < RTP_LEASES; i++) {
        if (s_leases[i].ip == ip && s_leases[i].until_us > now) {
            return true;
        }
    }
    return false;
}

void rtp_allow(uint32_t ip)
{
    if (!s_lock) {
        return;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int slot = 0;
    for (int i = 0; i < RTP_LEASES; i++) {
        if (s_leases[i].ip == ip) {
            slot = i;
            break;
        }
        if (s_leases[i].until_us < s_leases[slot].until_us) {
            slot = i;
        }
    }
    if (s_leases[slot].ip != ip || s_leases[slot].until_us <= now) {
        struct in_addr a = { .s_addr = ip };
        char a_str[16];
        inet_ntoa_r(a, a_str, sizeof(a_str));
        ESP_LOGI(TAG, "sender %s allowed", a_str);
    }
    s_leases[slot] = (rtp_lease_t){ .ip = ip, .until_us = now + CONFIG_APP_RTP_LEASE_S * 1000000LL };
    xSemaphoreGive(s_lock);
}

static void stream_start(const struct sockaddr_in *from, uint32_t ssrc, uint8_t pt, uint16_t seq, uint32_t ts, int64_t now)
{
    uint32_t streams = s_stats.streams + 1;
    s_stats = (rtp_stats_t){
        .active = true,
        .streams = streams,
        .src_ip = from->sin_addr.s_addr,
        .src_port = ntohs(from->sin_port),
        .ssrc = ssrc,
        .pt = pt,
        .started_us = now,
    };
    s_track = (track_t){ .next_seq = seq, .prev_ts = ts, .first_us = now };
    play_stop();
    char ip[16];
    inet_ntoa_r(from->sin_addr, ip, sizeof(ip));
    ESP_LOGI(TAG, "stream from %s:%u, ssrc 0x%08lx, payload type %u", ip, s_stats.src_port, (unsigned long)ssrc, pt);
}

static void track_timing(uint32_t ts, uint32_t payload, bool consecutive, int64_t now)
{
    if (s_track.have_prev && ts - s_track.prev_ts > RTP_JUMP_FRAMES) {
        s_track.first_us = now;
        s_track.ts_total = 0;
        s_track.prev_transit_us = 0;
    } else if (s_track.have_prev) {
        uint32_t frames = ts - s_track.prev_ts;
        if (consecutive && frames) {
            s_stats.frames_per_packet = frames;
        }
        s_track.ts_total += frames;
        uint32_t gap = now - s_stats.last_us;
        if (gap > s_stats.max_gap_us) {
            s_stats.max_gap_us = gap;
        }
    }
    s_track.prev_ts = ts;
    s_track.prev_payload = payload;
    s_track.have_prev = true;

    int64_t elapsed = now - s_track.first_us;
    if (elapsed < RTP_DRIFT_MIN_US || !s_stats.rate_hz) {
        return;
    }
    double expected = (double)elapsed * s_stats.rate_hz / 1e6;
    double drift = (s_track.ts_total - expected) * 1e9 / expected;
    s_stats.drift_ppb = drift > INT32_MAX ? INT32_MAX : drift < INT32_MIN ? INT32_MIN : (int32_t)drift;
    int64_t transit = now - (int64_t)(s_track.ts_total * 1000000 / s_stats.rate_hz);
    if (s_track.prev_transit_us) {
        int64_t d = llabs(transit - s_track.prev_transit_us);
        s_track.jitter_x16 += d - (s_track.jitter_x16 >> RTP_JITTER_SHIFT);
        s_stats.jitter_us = s_track.jitter_x16 >> RTP_JITTER_SHIFT;
    }
    s_track.prev_transit_us = transit;
}

static uint32_t session_bytes_per_frame(const rtp_session_t *ses)
{
    int bits = !strcmp(ses->encoding, "L16") ? 16 : !strcmp(ses->encoding, "L24") ? 24 : 0;
    return ses->channels == 2 ? bits / 8 * 2 : 0;
}

static void maybe_play(void)
{
    const rtp_session_t *ses = &s_session;
    if (s_track.playing || !ses->valid || ses->port != CONFIG_APP_RTP_PORT || ses->pt != s_stats.pt || (ses->ssrc && ses->ssrc != s_stats.ssrc)) {
        return;
    }
    uint32_t bpf = session_bytes_per_frame(ses);
    if (!bpf) {
        if (!s_track.warned) {
            s_track.warned = true;
            ESP_LOGW(TAG, "can't play %s/%lu/%u, only L16 or L24 stereo", ses->encoding, (unsigned long)ses->rate, ses->channels);
        }
        return;
    }
    s_track.playing = true;
    s_stats.rate_hz = ses->rate;
    s_stats.bytes_per_frame = bpf;
    ESP_LOGI(TAG, "playing '%s': %s stereo %lu Hz", ses->name, ses->encoding, (unsigned long)ses->rate);
    play_format(bpf, ses->rate);
}

static void place(uint32_t ts, uint16_t seq, const uint8_t *payload, int bytes)
{
    if (s_track.timeline) {
        int32_t gap = (int32_t)(ts - s_track.write_ts);
        if (gap > 0 && (uint32_t)gap <= play_max_gap_frames()) {
            play_silence(gap);
        } else if (gap) {
            s_stats.rebases++;
            play_rebase();
            ESP_LOGI(TAG, "timestamp jump %ld frames at seq %u, rebased", (long)gap, seq);
        }
    }
    s_track.timeline = true;
    play_push(payload, bytes);
    s_track.write_ts = ts + bytes / s_stats.bytes_per_frame;
}

static void on_packet(const uint8_t *p, int len, const struct sockaddr_in *from, int64_t now)
{
    if (len < RTP_HEADER_BYTES || p[0] >> 6 != RTP_VERSION) {
        s_stats.invalid++;
        return;
    }
    int header = RTP_HEADER_BYTES + 4 * (p[0] & 0x0F);
    if ((p[0] & 0x10) && len >= header + 4) {
        header += 4 + 4 * (p[header + 2] << 8 | p[header + 3]);
    }
    int padding = (p[0] & 0x20) ? p[len - 1] : 0;
    int payload = len - header - padding;
    if (payload <= 0) {
        s_stats.invalid++;
        return;
    }
    uint8_t pt = p[1] & 0x7F;
    uint16_t seq = p[2] << 8 | p[3];
    uint32_t ts = (uint32_t)p[4] << 24 | p[5] << 16 | p[6] << 8 | p[7];
    uint32_t ssrc = (uint32_t)p[8] << 24 | p[9] << 16 | p[10] << 8 | p[11];

    if (!s_stats.active || ssrc != s_stats.ssrc || from->sin_addr.s_addr != s_stats.src_ip || ntohs(from->sin_port) != s_stats.src_port) {
        stream_start(from, ssrc, pt, seq, ts, now);
    }
    int16_t ahead = (int16_t)(seq - s_track.next_seq);
    if (ahead < 0) {
        s_stats.late++;
        return;
    }
    s_stats.lost += ahead;
    s_track.next_seq = seq + 1;
    s_stats.packets++;
    s_stats.payload_bytes += payload;
    track_timing(ts, payload, ahead == 0, now);
    maybe_play();
    if (s_track.playing) {
        place(ts, seq, p + header, payload);
    }
    s_stats.last_us = now;
}

static void check_idle(int64_t now)
{
    if (s_stats.active && now - s_stats.last_us > (int64_t)CONFIG_APP_RTP_IDLE_MS * 1000) {
        s_stats.active = false;
        play_stop();
        ESP_LOGI(TAG, "stream stopped: %lu packets, %lu lost, %lu late, max gap %lu ms", (unsigned long)s_stats.packets, (unsigned long)s_stats.lost, (unsigned long)s_stats.late, (unsigned long)s_stats.max_gap_us / 1000);
    }
}

static void on_sap(const uint8_t *p, int len, const struct sockaddr_in *from, int64_t now)
{
    rtp_session_t ses;
    bool deleted;
    if (!sap_parse(p, len, from->sin_addr.s_addr, &ses, &deleted) || ses.port != CONFIG_APP_RTP_PORT) {
        return;
    }
    if (deleted) {
        if (s_session.valid) {
            ESP_LOGI(TAG, "session '%s' ended", s_session.name);
        }
        s_session.valid = false;
        return;
    }
    bool changed = !s_session.valid || strcmp(ses.name, s_session.name) || strcmp(ses.encoding, s_session.encoding) || ses.rate != s_session.rate || ses.channels != s_session.channels || ses.ssrc != s_session.ssrc;
    ses.seen_us = now;
    s_session = ses;
    if (changed) {
        char ip[16];
        inet_ntoa_r(from->sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "session '%s' from %s: %s/%lu/%u, payload type %u, ssrc 0x%08lx", ses.name, ip, ses.encoding, (unsigned long)ses.rate, ses.channels, ses.pt, (unsigned long)ses.ssrc);
    }
}

static int udp_socket(int port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return -1;
    }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }
    return sock;
}

typedef struct {
    int rtp;
    int sap;
} sockets_t;

static void rtp_task(void *arg)
{
    sockets_t socks = *(sockets_t *)arg;
    free(arg);
    static uint8_t buf[RTP_MAX_PACKET];
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(socks.rtp, &fds);
        FD_SET(socks.sap, &fds);
        struct timeval tv = { .tv_usec = RTP_RECV_TIMEOUT_MS * 1000 };
        int ready = select((socks.rtp > socks.sap ? socks.rtp : socks.sap) + 1, &fds, NULL, NULL, &tv);
        for (int i = 0; i < 2 && ready > 0; i++) {
            int sock = i ? socks.sap : socks.rtp;
            if (!FD_ISSET(sock, &fds)) {
                continue;
            }
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            int len = recvfrom(sock, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &from_len);
            if (len <= 0) {
                continue;
            }
            int64_t now = esp_timer_get_time();
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (!allowed(from.sin_addr.s_addr, now)) {
                s_denied++;
            } else if (sock == socks.rtp) {
                on_packet(buf, len, &from, now);
            } else {
                on_sap(buf, len, &from, now);
            }
            xSemaphoreGive(s_lock);
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        check_idle(esp_timer_get_time());
        xSemaphoreGive(s_lock);
    }
}

static void announce(void)
{
    mdns_txt_item_t txt[] = {
        { "formats", "L16,L24" },
        { "rates", "32000,44100,48000,88200,96000,176400,192000" },
        { "channels", "2" },
    };
    esp_err_t err = mdns_service_add(NULL, RTP_MDNS_SERVICE, RTP_MDNS_PROTO, CONFIG_APP_RTP_PORT, txt, sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS service %s.%s not announced: %s", RTP_MDNS_SERVICE, RTP_MDNS_PROTO, esp_err_to_name(err));
    }
}

esp_err_t rtp_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");
    ESP_RETURN_ON_ERROR(play_init(), TAG, "player");
    sockets_t *socks = malloc(sizeof(*socks));
    ESP_RETURN_ON_FALSE(socks, ESP_ERR_NO_MEM, TAG, "sockets");
    socks->rtp = udp_socket(CONFIG_APP_RTP_PORT);
    socks->sap = udp_socket(CONFIG_APP_RTP_SAP_PORT);
    if (socks->rtp < 0 || socks->sap < 0) {
        int e = errno;
        free(socks);
        ESP_RETURN_ON_FALSE(false, ESP_FAIL, TAG, "bind UDP %d / %d: errno %d", CONFIG_APP_RTP_PORT, CONFIG_APP_RTP_SAP_PORT, e);
    }
    struct ip_mreq group = { .imr_multiaddr.s_addr = inet_addr(SAP_GROUP), .imr_interface.s_addr = htonl(INADDR_ANY) };
    if (setsockopt(socks->sap, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof(group)) != 0) {
        ESP_LOGW(TAG, "join SAP group %s: errno %d, unicast SAP still works", SAP_GROUP, errno);
    }
    ESP_RETURN_ON_FALSE(xTaskCreate(rtp_task, "rtp", RTP_TASK_STACK, socks, RTP_TASK_PRIO, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    announce();
    ESP_LOGI(TAG, "listening on UDP %d, SAP on UDP %d", CONFIG_APP_RTP_PORT, CONFIG_APP_RTP_SAP_PORT);
    return ESP_OK;
}

void rtp_get(rtp_stats_t *out)
{
    if (!s_lock) {
        *out = (rtp_stats_t){ 0 };
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_stats;
    out->session = s_session;
    out->denied = s_denied;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < RTP_LEASES; i++) {
        if (s_leases[i].until_us > now) {
            out->lease[out->leases++] = s_leases[i];
        }
    }
    xSemaphoreGive(s_lock);
    play_get(out);
}

void rtp_reset(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t streams = s_stats.streams;
    s_stats = (rtp_stats_t){ .streams = streams };
    play_stop();
    xSemaphoreGive(s_lock);
}
