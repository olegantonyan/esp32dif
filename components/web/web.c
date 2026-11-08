#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "soc/soc_caps.h"
#include "digi.h"
#include "net.h"
#include "ota.h"
#include "rtp.h"
#include "settings.h"
#include "web.h"

#define USER "admin"
#define JSON_SIZE 2048
#define FORM_SIZE 512
#define SCAN_MAX 24
#define UPLOAD_CHUNK 4096
#define UPLOAD_TIMEOUTS 3
#define AUTH_FAIL_DELAY_MS 1000

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static bool authorized(httpd_req_t *req)
{
    char hdr[160];
    unsigned char cred[128];
    size_t n = 0;
    bool sent = httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK;
    if (sent && strncmp(hdr, "Basic ", 6) == 0 && mbedtls_base64_decode(cred, sizeof(cred) - 1, &n, (unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
        cred[n] = 0;
        const char *pw = strchr((char *)cred, ':');
        if (pw && pw - (char *)cred == strlen(USER) && memcmp(cred, USER, strlen(USER)) == 0 && settings_check_password(pw + 1)) {
            return true;
        }
    }
    if (sent) {
        ESP_LOGW(TAG, "wrong password for %s", req->uri);
        vTaskDelay(pdMS_TO_TICKS(AUTH_FAIL_DELAY_MS));
    }
    settings_t cfg;
    settings_get(&cfg);
    char realm[64];
    snprintf(realm, sizeof(realm), "Basic realm=\"%s\", charset=\"UTF-8\"", cfg.hostname);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", realm);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "login required: user " USER ", the device password (shown on the OLED)");
    return false;
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, msg);
}

typedef struct {
    char *buf;
    size_t len;
    size_t pos;
} out_t;

static void put(out_t *o, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->pos, o->len - o->pos, fmt, ap);
    va_end(ap);
    if (n > 0) {
        o->pos += (size_t)n < o->len - o->pos ? (size_t)n : o->len - o->pos - 1;
    }
}

static void put_str(out_t *o, const char *s)
{
    put(o, "\"");
    for (; *s; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') {
            put(o, "\\%c", c);
        } else if (c < 0x20) {
            put(o, "\\u%04x", c);
        } else {
            put(o, "%c", c);
        }
    }
    put(o, "\"");
}

static void put_ip(out_t *o, uint32_t ip)
{
    struct in_addr a = { .s_addr = ip };
    char s[16];
    inet_ntoa_r(a, s, sizeof(s));
    put_str(o, s);
}

static const char *play_name(rtp_play_state_t s)
{
    switch (s) {
    case RTP_PLAY_BUFFERING: return "buffering";
    case RTP_PLAY_RUNNING: return "playing";
    default: return "idle";
    }
}

static void status_json(out_t *o)
{
    settings_t cfg;
    settings_get(&cfg);
    int64_t now = esp_timer_get_time();
    put(o, "{\"hostname\":");
    put_str(o, cfg.hostname);
    put(o, ",\"version\":");
    put_str(o, esp_app_get_description()->version);
    put(o, ",\"uptime_s\":%" PRId64 ",\"free_heap\":%" PRIu32, now / 1000000, esp_get_free_heap_size());

    digi_state_t d;
    digi_get(&d);
    put(o, ",\"spdif\":{\"ready\":%s,\"rate\":%" PRIu32 ",\"tone_hz\":%" PRIu32 "}", d.ready || d.blind ? "true" : "false", d.rate, d.tone_hz);

    rtp_stats_t r;
    rtp_get(&r);
    put(o, ",\"rtp\":{\"state\":\"%s\",\"active\":%s,\"from\":", play_name(r.play_state), r.active ? "true" : "false");
    put_ip(o, r.src_ip);
    put(o, ",\"rate\":%" PRIu32 ",\"bits\":%" PRIu32 ",\"packets\":%" PRIu32 ",\"lost\":%" PRIu32 ",\"buffer_ms\":%" PRIu32 ",\"denied\":%" PRIu32 ",\"senders\":[",
        r.rate_hz, r.bytes_per_frame * 4, r.packets, r.lost, r.buffer_ms, r.denied);
    for (int i = 0; i < r.leases; i++) {
        put(o, i ? "," : "");
        put_ip(o, r.lease[i].ip);
    }
    put(o, "]}");

    wifi_ap_record_t ap;
    bool associated = net_associated() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    const char *state = net_has_ip() ? "connected" : associated ? "associated" : net_reconnect_enabled() ? "connecting" : "idle";
    put(o, ",\"sta\":{\"state\":\"%s\",\"ssid\":", state);
    put_str(o, associated ? (const char *)ap.ssid : "");
    esp_netif_ip_info_t ip = { 0 };
    if (net_has_ip()) {
        esp_netif_get_ip_info(net_sta(), &ip);
    }
    put(o, ",\"ip\":");
    put_ip(o, ip.ip.addr);
    if (associated) {
        put(o, ",\"rssi\":%d,\"channel\":%d", ap.rssi, ap.primary);
    }
    net_saved_t saved;
    put(o, ",\"saved\":");
    put_str(o, net_saved(&saved) == ESP_OK ? saved.ssid : "");
    put(o, "},\"ap\":{\"active\":%s,\"ssid\":", net_ap_active() ? "true" : "false");
    put_str(o, cfg.hostname);
    put(o, ",\"clients\":%d}", net_ap_clients());

    ota_status_t st;
    ota_status(&st);
    put(o, ",\"ota\":{\"running\":");
    put_str(o, st.running);
    put(o, ",\"state\":");
    put_str(o, ota_state_str(&st));
    put(o, ",\"other\":");
    put_str(o, st.other ? st.other : "");
    put(o, ",\"other_version\":");
    put_str(o, st.other_version);
    put(o, ",\"rollback\":%s}}\n", st.rollback_possible ? "true" : "false");
}

static esp_err_t send_json(httpd_req_t *req, void (*render)(out_t *))
{
    out_t o = { .buf = malloc(JSON_SIZE), .len = JSON_SIZE };
    if (!o.buf) {
        return reply(req, "500 Internal Server Error", "out of memory");
    }
    o.buf[0] = 0;
    render(&o);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, o.buf, o.pos);
    free(o.buf);
    return err;
}

static esp_err_t handle_index(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t handle_status(httpd_req_t *req)
{
    return authorized(req) ? send_json(req, status_json) : ESP_OK;
}

static wifi_ap_record_t *s_scan;
static uint16_t s_scan_n;

static void scan_json(out_t *o)
{
    put(o, "[");
    for (uint16_t i = 0; i < s_scan_n; i++) {
        const wifi_ap_record_t *ap = &s_scan[i];
        put(o, i ? ",{\"ssid\":" : "{\"ssid\":");
        put_str(o, (const char *)ap->ssid);
        put(o, ",\"rssi\":%d,\"channel\":%d,\"auth\":\"%s\"}", ap->rssi, ap->primary, net_auth_str(ap->authmode));
    }
    put(o, "]\n");
}

static esp_err_t handle_scan(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "scan failed: %s (busy connecting? try again)", esp_err_to_name(err));
        return reply(req, "503 Service Unavailable", msg);
    }
    uint16_t n = SCAN_MAX;
    s_scan = calloc(n, sizeof(*s_scan));
    if (!s_scan) {
        esp_wifi_clear_ap_list();
        return reply(req, "500 Internal Server Error", "out of memory");
    }
    esp_wifi_scan_get_ap_records(&n, s_scan);
    esp_wifi_clear_ap_list();
    s_scan_n = n;
    err = send_json(req, scan_json);
    free(s_scan);
    s_scan = NULL;
    return err;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        unsigned int c;
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && s[1] && s[2] && sscanf(s + 1, "%2x", &c) == 1) {
            *o++ = c;
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static bool read_form(httpd_req_t *req, char *buf, size_t size)
{
    if (req->content_len >= size) {
        reply(req, "413 Content Too Large", "form too large");
        return false;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < UPLOAD_TIMEOUTS) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        got += n;
    }
    buf[got] = 0;
    return true;
}

static bool field(const char *form, const char *key, char *out, size_t len)
{
    if (httpd_query_key_value(form, key, out, len) != ESP_OK) {
        out[0] = 0;
        return false;
    }
    url_decode(out);
    return true;
}

static esp_err_t handle_wifi(httpd_req_t *req)
{
    char form[FORM_SIZE], ssid[100], pass[200], band_s[8];
    if (!authorized(req)) {
        return ESP_OK;
    }
    if (!read_form(req, form, sizeof(form))) {
        return ESP_FAIL;
    }
    field(form, "ssid", ssid, sizeof(ssid));
    bool has_pass = field(form, "password", pass, sizeof(pass)) && pass[0];
    field(form, "band", band_s, sizeof(band_s));
    wifi_band_mode_t band = strcmp(band_s, "2.4") == 0 ? WIFI_BAND_MODE_2G_ONLY : strcmp(band_s, "5") == 0 ? WIFI_BAND_MODE_5G_ONLY : WIFI_BAND_MODE_AUTO;
    if (!ssid[0] || strlen(ssid) > 32 || strlen(pass) > 64) {
        return reply(req, "400 Bad Request", "ssid must be 1-32 bytes, password up to 64");
    }
    net_disconnect();
    esp_err_t err = net_connect(ssid, has_pass ? pass : NULL, band, true);
    if (err != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "connect failed: %s", esp_err_to_name(err));
        return reply(req, "400 Bad Request", msg);
    }
    return reply(req, "200 OK", "connecting; saved once it gets an IP");
}

static esp_err_t handle_forget(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    esp_err_t err = net_forget();
    return err == ESP_OK ? reply(req, "200 OK", "saved network erased; current connection stays up") : reply(req, "500 Internal Server Error", esp_err_to_name(err));
}

static esp_err_t handle_settings(httpd_req_t *req)
{
    char form[FORM_SIZE], host[100], pass[200];
    if (!authorized(req)) {
        return ESP_OK;
    }
    if (!read_form(req, form, sizeof(form))) {
        return ESP_FAIL;
    }
    const char *err = NULL;
    if (field(form, "hostname", host, sizeof(host)) && host[0]) {
        err = settings_set_hostname(host);
    }
    if (!err && field(form, "password", pass, sizeof(pass)) && pass[0]) {
        err = settings_set_password(pass);
    }
    return err ? reply(req, "400 Bad Request", err) : reply(req, "200 OK", "saved");
}

static esp_err_t handle_reboot(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    reply(req, "200 OK", "rebooting");
    ota_reboot(1000);
    return ESP_OK;
}

static uint32_t peer_ip(httpd_req_t *req)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0) {
        return 0;
    }
    if (addr.ss_family == AF_INET6) {
        return ((struct sockaddr_in6 *)&addr)->sin6_addr.un.u32_addr[3];
    }
    return ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
}

static esp_err_t handle_rtp_allow(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    uint32_t ip = peer_ip(req);
    if (!ip) {
        return reply(req, "500 Internal Server Error", "no peer address");
    }
    rtp_allow(ip);
    char msg[32];
    snprintf(msg, sizeof(msg), "allowed for %d s", CONFIG_APP_RTP_LEASE_S);
    return reply(req, "200 OK", msg);
}

static esp_err_t upload_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_hdr(req, "Connection", "close");
    reply(req, status, msg);
    return ESP_FAIL;
}

static esp_err_t handle_update(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_FAIL;
    }
    if (req->content_len == 0) {
        return upload_error(req, "400 Bad Request", "empty body, POST the .bin file");
    }
    const char *err = ota_begin(req->content_len);
    if (err) {
        return upload_error(req, "409 Conflict", err);
    }
    char *buf = malloc(UPLOAD_CHUNK);
    if (!buf) {
        ota_finish(false);
        return upload_error(req, "500 Internal Server Error", "out of memory");
    }
    size_t left = req->content_len;
    int timeouts = 0;
    while (left) {
        int n = httpd_req_recv(req, buf, left < UPLOAD_CHUNK ? left : UPLOAD_CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < UPLOAD_TIMEOUTS) {
            continue;
        }
        if (n <= 0) {
            free(buf);
            ota_finish(false);
            ESP_LOGW(TAG, "upload interrupted, %zu bytes missing", left);
            return ESP_FAIL;
        }
        timeouts = 0;
        left -= n;
        if (!err) {
            err = ota_write(buf, n);
        }
    }
    free(buf);
    const char *fin = ota_finish(!err);
    if (err || fin) {
        return upload_error(req, "400 Bad Request", err ? err : fin);
    }
    ota_status_t st;
    ota_status(&st);
    char msg[96];
    snprintf(msg, sizeof(msg), "installed %s on %s, rebooting", st.other_version, st.other);
    reply(req, "200 OK", msg);
    ota_reboot(1000);
    return ESP_OK;
}

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 4;
    cfg.max_uri_handlers = 12;
    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "httpd start");

    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = handle_index },
        { .uri = "/api/status", .method = HTTP_GET, .handler = handle_status },
        { .uri = "/api/scan", .method = HTTP_GET, .handler = handle_scan },
        { .uri = "/api/wifi", .method = HTTP_POST, .handler = handle_wifi },
        { .uri = "/api/wifi/forget", .method = HTTP_POST, .handler = handle_forget },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = handle_settings },
        { .uri = "/api/reboot", .method = HTTP_POST, .handler = handle_reboot },
        { .uri = "/api/rtp/allow", .method = HTTP_POST, .handler = handle_rtp_allow },
        { .uri = "/api/update", .method = HTTP_POST, .handler = handle_update },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uris[i]), TAG, "register %s", uris[i].uri);
    }
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    ESP_LOGI(TAG, "listening on port %d", cfg.server_port);
    return ESP_OK;
}
