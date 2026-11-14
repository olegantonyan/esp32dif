#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "rtp.h"
#include "sdkconfig.h"
#include "settings.h"
#include "remote.h"

#define AUTH_TIMEOUT_US (5 * 1000 * 1000)
#define PING_US (30 * 1000 * 1000)
#define REPRESS_US (300 * 1000)
#define HOLD_REPEAT_US (200 * 1000)
#define PASSWORD_LINE_MAX 72
#define CLIENT_LINE_MAX 448

typedef struct {
    int sock;
    uint32_t ip;
    bool ready;
    int64_t since_us;
    size_t len;
    char line[CLIENT_LINE_MAX];
    remote_track_t track;
} client_t;

static const char *TAG = "remote";

static const struct {
    const char *name;
    bool repeat;
} s_actions[] = {
    [REMOTE_PREVIOUS] = { "previous", false },
    [REMOTE_PLAY_PAUSE] = { "play-pause", false },
    [REMOTE_NEXT] = { "next", false },
    [REMOTE_VOLUME_UP] = { "volume-up", true },
    [REMOTE_VOLUME_DOWN] = { "volume-down", true },
    [REMOTE_MUTE] = { "mute", false },
};

static SemaphoreHandle_t s_lock;
static client_t s_clients[REMOTE_MAX_CLIENTS];
static remote_status_t s_status;
static const remote_key_t *s_last_key;
static int64_t s_last_frame_us;
static int64_t s_last_sent_us;
static rtp_stats_t s_rtp;

static void reset_track(client_t *c)
{
    memset(&c->track, 0, sizeof(c->track));
    c->track.volume = -1;
}

static void drop(client_t *c, const char *why)
{
    char ip[16];
    inet_ntoa_r(c->ip, ip, sizeof(ip));
    ESP_LOGI(TAG, "%s: %s", ip, why);
    close(c->sock);
    c->sock = -1;
    c->ready = false;
    reset_track(c);
}

static bool send_line(client_t *c, const char *s)
{
    size_t n = strlen(s);
    if (send(c->sock, s, n, 0) == (ssize_t)n && send(c->sock, "\n", 1, 0) == 1) {
        return true;
    }
    drop(c, "send failed");
    return false;
}

const char *remote_action_name(remote_action_t action)
{
    return (size_t)action < sizeof(s_actions) / sizeof(s_actions[0]) ? s_actions[action].name : "?";
}

bool remote_action_from_name(const char *name, remote_action_t *out)
{
    for (size_t i = 0; i < sizeof(s_actions) / sizeof(s_actions[0]); i++) {
        if (!strcmp(name, s_actions[i].name)) {
            *out = i;
            return true;
        }
    }
    return false;
}

int remote_send(remote_action_t action)
{
    int sent = 0;
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    rtp_get(&s_rtp);
    bool source_connected = false;
    for (int i = 0; i < REMOTE_MAX_CLIENTS; i++) {
        source_connected |= s_clients[i].sock >= 0 && s_clients[i].ready && s_clients[i].ip == s_rtp.src_ip;
    }
    s_status.last_ip = 0;
    for (int i = 0; i < REMOTE_MAX_CLIENTS; i++) {
        client_t *c = &s_clients[i];
        if (c->sock < 0 || !c->ready || (source_connected && c->ip != s_rtp.src_ip)) {
            continue;
        }
        if (send_line(c, remote_action_name(action))) {
            sent++;
            s_status.sent++;
            s_status.last_ip = c->ip;
        }
    }
    s_status.last_action = remote_action_name(action);
    s_status.last_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
    return sent;
}

static void on_frame(const ir_frame_t *f)
{
    const remote_key_t *key = remote_lookup(f);
    int64_t now = esp_timer_get_time();
    bool held = key && key == s_last_key && now - s_last_frame_us < REPRESS_US;
    if (key || f->proto == IR_REPEAT) {
        s_last_frame_us = now;
    }
    if (!key) {
        return;
    }
    bool skip = s_actions[key->action].repeat ? (f->proto == IR_REPEAT && !held) || (held && now - s_last_sent_us < HOLD_REPEAT_US) : f->proto == IR_REPEAT || held;
    if (skip) {
        return;
    }
    s_last_key = key;
    s_last_sent_us = now;
    remote_send(key->action);
}

static void set_options(int sock)
{
    struct timeval tv = { .tv_sec = 2 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int on = 1, idle = 60, intvl = 10, cnt = 3;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

static void accept_client(int listener)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int sock = accept(listener, (struct sockaddr *)&addr, &addr_len);
    if (sock < 0) {
        ESP_LOGE(TAG, "accept: errno %d", errno);
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }
    client_t *slot = NULL;
    for (int i = 0; i < REMOTE_MAX_CLIENTS && !slot; i++) {
        slot = s_clients[i].sock < 0 ? &s_clients[i] : NULL;
    }
    if (!slot) {
        static const char busy[] = "busy\n";
        send(sock, busy, sizeof(busy) - 1, 0);
        close(sock);
        return;
    }
    set_options(sock);
    *slot = (client_t){ .sock = sock, .ip = addr.sin_addr.s_addr, .since_us = esp_timer_get_time() };
    reset_track(slot);
}

static void check_password(client_t *c)
{
    bool ok = settings_check_password(c->line);
    memset(c->line, 0, sizeof(c->line));
    if (!ok) {
        send_line(c, "denied");
        drop(c, "wrong password");
        return;
    }
    if (send_line(c, "ok")) {
        c->ready = true;
        c->since_us = esp_timer_get_time();
        char ip[16];
        inet_ntoa_r(c->ip, ip, sizeof(ip));
        ESP_LOGI(TAG, "%s: connected", ip);
    }
}

static void copy_utf8(char *dst, size_t size, const char *src, size_t n)
{
    if (n >= size) {
        n = size - 1;
        while (n && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *next_field(char *dst, size_t size, const char *text)
{
    const char *tab = strchr(text, '\t');
    size_t n = tab ? (size_t)(tab - text) : strlen(text);
    copy_utf8(dst, size, text, n);
    return text + n + (tab ? 1 : 0);
}

static void set_track(client_t *c, const char *text)
{
    remote_track_t *t = &c->track;
    text = next_field(t->artist, sizeof(t->artist), text);
    text = next_field(t->title, sizeof(t->title), text);
    text = next_field(t->player, sizeof(t->player), text);
    text = next_field(t->status, sizeof(t->status), text);
    next_field(t->id, sizeof(t->id), text);
}

static void set_volume(client_t *c, const char *text)
{
    char *end;
    long v = strtol(text, &end, 10);
    bool ok = end != text && v >= 0 && v <= 100;
    c->track.volume = ok ? v : -1;
    c->track.muted = ok && !strcmp(end, " muted");
}

static const char *args_of(const char *line, const char *cmd)
{
    size_t n = strlen(cmd);
    if (strncmp(line, cmd, n) || (line[n] != ' ' && line[n] != '\0')) {
        return NULL;
    }
    return line[n] ? line + n + 1 : "";
}

static void handle_line(client_t *c)
{
    const char *args;
    if ((args = args_of(c->line, "track"))) {
        set_track(c, args);
    } else if ((args = args_of(c->line, "volume"))) {
        set_volume(c, args);
    }
}

static void read_client(client_t *c)
{
    char buf[CLIENT_LINE_MAX];
    ssize_t n = recv(c->sock, buf, sizeof(buf), 0);
    if (n <= 0) {
        drop(c, "disconnected");
        return;
    }
    for (ssize_t i = 0; i < n && c->sock >= 0; i++) {
        if (buf[i] == '\r') {
            continue;
        }
        if (buf[i] != '\n') {
            size_t max = c->ready ? sizeof(c->line) : PASSWORD_LINE_MAX;
            if (c->len < max - 1) {
                c->line[c->len++] = buf[i];
            } else if (!c->ready) {
                drop(c, "password line too long");
            }
            continue;
        }
        c->line[c->len] = '\0';
        c->len = 0;
        if (c->ready) {
            handle_line(c);
        } else {
            check_password(c);
        }
    }
}

static void server_task(void *arg)
{
    int listener = (int)(intptr_t)arg;
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listener, &rfds);
        int maxfd = listener;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < REMOTE_MAX_CLIENTS; i++) {
            if (s_clients[i].sock >= 0) {
                FD_SET(s_clients[i].sock, &rfds);
                maxfd = s_clients[i].sock > maxfd ? s_clients[i].sock : maxfd;
            }
        }
        xSemaphoreGive(s_lock);
        struct timeval tv = { .tv_sec = 1 };
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (r > 0 && FD_ISSET(listener, &rfds)) {
            accept_client(listener);
        }
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < REMOTE_MAX_CLIENTS; i++) {
            client_t *c = &s_clients[i];
            if (c->sock >= 0 && r > 0 && FD_ISSET(c->sock, &rfds)) {
                read_client(c);
            }
            if (c->sock < 0) {
                continue;
            }
            if (!c->ready && now - c->since_us > AUTH_TIMEOUT_US) {
                drop(c, "no password");
            } else if (c->ready && now - c->since_us > PING_US) {
                c->since_us = now;
                send_line(c, "ping");
            }
        }
        xSemaphoreGive(s_lock);
    }
}

esp_err_t remote_init(void)
{
    for (int i = 0; i < REMOTE_MAX_CLIENTS; i++) {
        s_clients[i].sock = -1;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "lock");
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    ESP_RETURN_ON_FALSE(sock >= 0, ESP_FAIL, TAG, "socket: errno %d", errno);
    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_APP_REMOTE_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(sock, 1) != 0) {
        ESP_LOGE(TAG, "listen on port %d: errno %d", CONFIG_APP_REMOTE_PORT, errno);
        close(sock);
        return ESP_FAIL;
    }
    if (xTaskCreate(server_task, "remote", 3072, (void *)(intptr_t)sock, 2, NULL) != pdPASS) {
        close(sock);
        return ESP_ERR_NO_MEM;
    }
    ir_set_handler(on_frame);
    ESP_LOGI(TAG, "listening on port %d", CONFIG_APP_REMOTE_PORT);
    return ESP_OK;
}

bool remote_track(uint32_t ip, remote_track_t *out)
{
    bool found = false;
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < REMOTE_MAX_CLIENTS && !found; i++) {
        client_t *c = &s_clients[i];
        const remote_track_t *t = &c->track;
        found = c->sock >= 0 && c->ready && c->ip == ip && (t->artist[0] || t->title[0] || t->player[0] || t->volume >= 0);
        if (found) {
            *out = *t;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

void remote_get(remote_status_t *out)
{
    *out = (remote_status_t){ 0 };
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    for (int i = 0; i < REMOTE_MAX_CLIENTS; i++) {
        if (s_clients[i].sock >= 0 && s_clients[i].ready) {
            out->client_ip[out->clients++] = s_clients[i].ip;
        }
    }
    xSemaphoreGive(s_lock);
}
