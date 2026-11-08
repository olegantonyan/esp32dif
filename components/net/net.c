#include <inttypes.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mdns.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "settings.h"
#include "net.h"

#define NET_ASSOCIATED BIT0
#define NET_GOT_IP     BIT1
#define NVS_NS         "wifi"

static const char *TAG = "net";

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static esp_timer_handle_t s_ap_timer;
static volatile bool s_ap_on;
static bool s_had_ip;
static int64_t s_since_us;
static volatile bool s_reconnect;
static volatile bool s_leaving;
static volatile uint8_t s_last_reason;
static esp_timer_handle_t s_retry_timer;
static uint32_t s_retries;
static net_saved_t s_pending;
static volatile bool s_save_pending;

static uint32_t retry_delay_s(uint32_t retries)
{
    if (retries < 3) {
        return 0;
    }
    return retries - 3 < 4 ? 5u << (retries - 3) : 60;
}

static void on_retry(void *arg)
{
    if (s_reconnect) {
        esp_wifi_connect();
    }
}

static void save_pending(void)
{
    s_save_pending = false;
    net_saved_t cur;
    if (net_saved(&cur) == ESP_OK && strcmp(cur.ssid, s_pending.ssid) == 0
        && strcmp(cur.password, s_pending.password) == 0 && cur.band == s_pending.band) {
        return;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "ssid", s_pending.ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(h, "pass", s_pending.password);
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(h, "band", s_pending.band);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved network '%s'", s_pending.ssid);
    } else {
        ESP_LOGE(TAG, "save network: %s", esp_err_to_name(err));
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_last_reason = 0;
        xEventGroupSetBits(s_events, NET_ASSOCIATED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = data;
        xEventGroupClearBits(s_events, NET_ASSOCIATED | NET_GOT_IP);
        if (s_leaving) {
            s_leaving = false;
            return;
        }
        if (!s_reconnect) {
            return;
        }
        s_last_reason = e->reason;
        uint32_t delay_s = retry_delay_s(s_retries++);
        if (delay_s == 0) {
            ESP_LOGW(TAG, "disconnected: %s (%d), reconnecting", net_reason_str(e->reason), e->reason);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "disconnected: %s (%d), retry in %" PRIu32 " s", net_reason_str(e->reason), e->reason, delay_s);
            esp_timer_start_once(s_retry_timer, (uint64_t)delay_s * 1000000);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        settings_t cfg;
        settings_get(&cfg);
        ESP_LOGI(TAG, "got ip " IPSTR ", %s.local", IP2STR(&e->ip_info.ip), cfg.hostname);
        s_retries = 0;
        xEventGroupSetBits(s_events, NET_GOT_IP);
        if (s_save_pending) {
            save_pending();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_events, NET_GOT_IP);
    }
}

static void on_shutdown(void)
{
    s_reconnect = false;
    s_leaving = true;
}

static wifi_band_mode_t current_band(void)
{
    wifi_band_mode_t band = WIFI_BAND_MODE_AUTO;
#if SOC_WIFI_SUPPORT_5G
    esp_wifi_get_band_mode(&band);
#endif
    return band;
}

static esp_err_t ap_configure(wifi_band_mode_t band)
{
    settings_t cfg;
    settings_get(&cfg);
    wifi_config_t wc = {
        .ap = {
            .ssid_len = strlen(cfg.hostname),
            .channel = band == WIFI_BAND_MODE_5G_ONLY ? 36 : 1,
            .max_connection = CONFIG_APP_NET_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    memcpy(wc.ap.ssid, cfg.hostname, wc.ap.ssid_len);
    strlcpy((char *)wc.ap.password, cfg.password, sizeof(wc.ap.password));
    return esp_wifi_set_config(WIFI_IF_AP, &wc);
}

static void ap_set(bool on)
{
    esp_err_t err = esp_wifi_set_mode(on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err == ESP_OK && on) {
        err = ap_configure(current_band());
    }
    static esp_err_t s_last_err;
    if (err != ESP_OK) {
        if (on) {
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
        if (err != s_last_err) {
            ESP_LOGE(TAG, "fallback AP %s: %s", on ? "on" : "off", esp_err_to_name(err));
        }
        s_last_err = err;
        return;
    }
    s_last_err = ESP_OK;
    s_ap_on = on;
    ESP_LOGI(TAG, "fallback AP %s", on ? "on, 192.168.4.1" : "off");
}

static void ap_check(void *arg)
{
    int64_t now = esp_timer_get_time();
    bool ip = net_has_ip();
    if (ip != s_had_ip) {
        s_had_ip = ip;
        s_since_us = now;
    }
    bool want = ip ? s_ap_on && now - s_since_us < CONFIG_APP_NET_AP_LINGER_S * 1000000LL
                   : s_ap_on || !s_reconnect || now - s_since_us >= CONFIG_APP_NET_AP_FALLBACK_S * 1000000LL;
    if (want != s_ap_on) {
        ap_set(want);
    }
}

static void on_settings(const settings_t *cfg)
{
    esp_netif_set_hostname(s_sta, cfg->hostname);
    mdns_hostname_set(cfg->hostname);
    if (s_ap_on) {
        ap_configure(current_band());
    }
}

static esp_err_t mdns_start(const char *hostname)
{
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns init");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(hostname), TAG, "mdns hostname");
    return mdns_instance_name_set(CONFIG_APP_NET_INSTANCE);
}

esp_err_t net_init(void)
{
    settings_t set;
    settings_get(&set);
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "event group");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "event loop");

    s_sta = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_sta, ESP_FAIL, TAG, "sta netif");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(s_sta, set.hostname), TAG, "hostname");
    s_ap = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_ap, ESP_FAIL, TAG, "ap netif");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL), TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL), TAG, "ip events");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(ap_configure(WIFI_BAND_MODE_AUTO), TAG, "ap config");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "power save");
    ESP_RETURN_ON_ERROR(mdns_start(set.hostname), TAG, "mdns");

    esp_log_level_set("wifi", ESP_LOG_ERROR);
    ESP_RETURN_ON_ERROR(esp_register_shutdown_handler(on_shutdown), TAG, "shutdown handler");
    const esp_timer_create_args_t timer = { .callback = on_retry, .name = "wifi_retry" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer, &s_retry_timer), TAG, "retry timer");
    settings_on_change(on_settings);

    net_saved_t saved;
    if (net_saved(&saved) == ESP_OK) {
        ESP_LOGI(TAG, "connecting to saved network '%s'", saved.ssid);
        err = net_connect(saved.ssid, saved.password[0] ? saved.password : NULL, saved.band, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "connect to saved network: %s", esp_err_to_name(err));
        }
    }
    const esp_timer_create_args_t ap_timer = { .callback = ap_check, .name = "wifi_ap" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&ap_timer, &s_ap_timer), TAG, "ap timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_ap_timer, 1000000), TAG, "ap timer start");
    return ESP_OK;
}

esp_netif_t *net_sta(void)
{
    return s_sta;
}

esp_netif_t *net_ap(void)
{
    return s_ap;
}

bool net_ap_active(void)
{
    return s_ap_on;
}

int net_ap_clients(void)
{
    wifi_sta_list_t list;
    return s_ap_on && esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : 0;
}

esp_err_t net_connect(const char *ssid, const char *password, wifi_band_mode_t band, bool save)
{
    if (!ssid[0] || strlen(ssid) >= sizeof(s_pending.ssid) || (password && strlen(password) >= sizeof(s_pending.password))) {
        return ESP_ERR_INVALID_SIZE;
    }
    wifi_config_t cfg = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.rssi_5g_adjustment = CONFIG_APP_NET_5G_PREFER_DB,
        },
    };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (password) {
        strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    }
    if (s_ap_on) {
        ap_configure(band);
    }
#if SOC_WIFI_SUPPORT_5G
    ESP_RETURN_ON_ERROR(esp_wifi_set_band_mode(band), TAG, "set band");
#endif
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set config");

    esp_timer_stop(s_retry_timer);
    s_retries = 0;
    s_save_pending = false;
    if (save) {
        strlcpy(s_pending.ssid, ssid, sizeof(s_pending.ssid));
        strlcpy(s_pending.password, password ? password : "", sizeof(s_pending.password));
        s_pending.band = band;
        s_save_pending = true;
    }
    s_last_reason = 0;
    s_reconnect = true;
    return esp_wifi_connect();
}

void net_disconnect(void)
{
    s_reconnect = false;
    s_save_pending = false;
    esp_timer_stop(s_retry_timer);
    s_leaving = net_associated();
    esp_wifi_disconnect();
    xEventGroupClearBits(s_events, NET_ASSOCIATED | NET_GOT_IP);
}

esp_err_t net_saved(net_saved_t *out)
{
    *out = (net_saved_t){ .band = WIFI_BAND_MODE_AUTO };
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = sizeof(out->ssid);
    err = nvs_get_str(h, "ssid", out->ssid, &len);
    if (err == ESP_OK) {
        len = sizeof(out->password);
        err = nvs_get_str(h, "pass", out->password, &len);
    }
    uint8_t band;
    if (err == ESP_OK && nvs_get_u8(h, "band", &band) == ESP_OK) {
        out->band = band;
    }
    nvs_close(h);
    return err;
}

esp_err_t net_forget(void)
{
    s_save_pending = false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool net_associated(void)
{
    return s_events && (xEventGroupGetBits(s_events) & NET_ASSOCIATED);
}

bool net_has_ip(void)
{
    return s_events && (xEventGroupGetBits(s_events) & NET_GOT_IP);
}

bool net_reconnect_enabled(void)
{
    return s_reconnect;
}

uint8_t net_last_reason(void)
{
    return s_last_reason;
}

const char *net_reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE: return "auth expired";
    case WIFI_REASON_ASSOC_LEAVE: return "left";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "handshake timeout, wrong password?";
    case WIFI_REASON_BEACON_TIMEOUT: return "beacon timeout";
    case WIFI_REASON_NO_AP_FOUND: return "network not found";
    case WIFI_REASON_AUTH_FAIL: return "auth failed";
    case WIFI_REASON_ASSOC_FAIL: return "association failed";
    case WIFI_REASON_CONNECTION_FAIL: return "connection failed";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "no network with compatible security";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "no network in auth mode threshold";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: return "no network in rssi threshold";
    default: return "other";
    }
}

const char *net_auth_str(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "wep";
    case WIFI_AUTH_WPA_PSK: return "wpa";
    case WIFI_AUTH_WPA2_PSK: return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
    case WIFI_AUTH_WPA3_PSK: return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
    case WIFI_AUTH_OWE: return "owe";
    case WIFI_AUTH_WAPI_PSK: return "wapi";
    case WIFI_AUTH_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "enterprise";
    default: return "other";
    }
}

const char *net_band_str(uint8_t channel)
{
    return channel > 14 ? "5" : "2.4";
}
