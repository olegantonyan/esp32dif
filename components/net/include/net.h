#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"

typedef struct {
    char ssid[33];
    char password[65];
    wifi_band_mode_t band;
} net_saved_t;

#define NET_DROPS_KEPT 4

typedef struct {
    int64_t at_us;
    uint8_t bssid[6];
    uint8_t reason;
    int8_t rssi;
} net_drop_t;

esp_err_t net_init(void);

esp_netif_t *net_sta(void);
esp_netif_t *net_ap(void);
bool net_ap_active(void);
int net_ap_clients(void);
esp_err_t net_connect(const char *ssid, const char *password, wifi_band_mode_t band, bool save);
void net_disconnect(void);
esp_err_t net_saved(net_saved_t *out);
esp_err_t net_forget(void);
bool net_associated(void);
bool net_has_ip(void);
bool net_reconnect_enabled(void);
uint8_t net_last_reason(void);
uint32_t net_drops(net_drop_t out[NET_DROPS_KEPT]);

const char *net_reason_str(uint8_t reason);
const char *net_auth_str(wifi_auth_mode_t mode);
const char *net_band_str(uint8_t channel);
