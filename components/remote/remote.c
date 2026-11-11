#include <errno.h>
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
#define PASSWORD_LINE_MAX 72
#define CLIENT_LINE_MAX 320
#define TRACK_CMD "track"

typedef struct {
    int sock;
    uint32_t ip;
    bool ready;
    int64_t since_us;
    size_t len;
    char line[CLIENT_LINE_MAX];
    char artist[REMOTE_TRACK_MAX];
    char title[REMOTE_TRACK_MAX];
} client_t;

static const char *TAG = "remote";

static SemaphoreHandle_t s_lock;
static client_t s_clients[REMOTE_MAX_CLIENTS];
static remote_status_t s_status;
static const remote_key_t *s_last_key;
static int64_t s_last_frame_us;

static void drop(client_t *c, const char *why)
{
    char ip[16];
    inet_ntoa_r(c->ip, ip, sizeof(ip));
    ESP_LOGI(TAG, "%s: %s", ip, why);
    close(c->sock);
    c->sock = -1;
    c->ready = false;
    c->artist[0] = c->title[0] = '\0';
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

static void on_frame(const ir_frame_t *f)
{
    const remote_key_t *key = remote_lookup(f);
    int64_t now = esp_timer_get_time();
    bool held = key && key == s_last_key && now - s_last_frame_us < REPRESS_US;
    if (key || f->proto == IR_REPEAT) {
        s_last_frame_us = now;
    }
    if (!key || f->proto == IR_REPEAT || held) {
        return;
    }
    s_last_key = key;

    static rtp_stats_t rtp;
    rtp_get(&rtp);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool source_connected = false;
    for (int i = 0; i < REMOTE_MAX_CLIENTS; i++) {
        source_connected |= s_clients[i].sock >= 0 && s_clients[i].ready && s_clients[i].ip == rtp.src_ip;
    }
    for (int i = 0; i < REMOTE_MAX_CLIENTS; i++) {
        client_t *c = &s_clients[i];
        if (c->sock < 0 || !c->ready || (source_connected && c->ip != rtp.src_ip)) {
            continue;
        }
        if (send_line(c, key->action)) {
            s_status.sent++;
            s_status.last_ip = c->ip;
        }
    }
    s_status.last_action = key->action;
    s_status.last_us = now;
    xSemaphoreGive(s_lock);
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

static void set_track(client_t *c, const char *text)
{
    const char *tab = strchr(text, '\t');
    const char *title = tab ? tab + 1 : text;
    copy_utf8(c->artist, sizeof(c->artist), text, tab ? (size_t)(tab - text) : 0);
    copy_utf8(c->title, sizeof(c->title), title, strlen(title));
}

static void handle_line(client_t *c)
{
    size_t n = strlen(TRACK_CMD);
    if (!strncmp(c->line, TRACK_CMD, n) && (c->line[n] == ' ' || c->line[n] == '\0')) {
        set_track(c, c->line[n] ? c->line + n + 1 : "");
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

bool remote_track(uint32_t ip, char artist[REMOTE_TRACK_MAX], char title[REMOTE_TRACK_MAX])
{
    bool found = false;
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < REMOTE_MAX_CLIENTS && !found; i++) {
        client_t *c = &s_clients[i];
        found = c->sock >= 0 && c->ready && c->ip == ip && (c->artist[0] || c->title[0]);
        if (found) {
            strlcpy(artist, c->artist, REMOTE_TRACK_MAX);
            strlcpy(title, c->title, REMOTE_TRACK_MAX);
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
