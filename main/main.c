#include "esp_err.h"
#include "esp_log.h"
#include "settings.h"
#include "ota.h"
#include "cmd_ota.h"
#include "cmd_config.h"
#include "web.h"
#include "telnet.h"
#include "led.h"
#include "oled.h"
#include "shell.h"
#include "cmd_system.h"
#include "cmd_led.h"
#include "cmd_oled.h"
#include "digi.h"
#include "cmd_digi.h"
#include "net.h"
#include "cmd_wifi.h"
#include "rtp.h"
#include "cmd_rtp.h"
#include "ir.h"
#include "cmd_ir.h"
#include "remote.h"
#include "status_led.h"
#include "status_oled.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_ERROR_CHECK(settings_init());
    if (ota_init() != ESP_OK) {
        ESP_LOGE(TAG, "ota init failed, continuing without rollback protection");
    }
    if (led_init() == ESP_OK) {
        led_set_auto(status_led_pattern);
    } else {
        ESP_LOGE(TAG, "led failed to start, continuing without it");
    }
    if (oled_init() == ESP_OK) {
        oled_set_auto(status_oled_screen);
    } else {
        ESP_LOGE(TAG, "oled failed to start, continuing without it");
    }
    if (ir_init() != ESP_OK) {
        ESP_LOGE(TAG, "ir receiver failed to start, continuing without it");
    }
    if (digi_init() != ESP_OK) {
        ESP_LOGW(TAG, "digi not ready, continuing without it");
    }
    bool net = net_init() == ESP_OK;
    if (!net) {
        ESP_LOGE(TAG, "network failed to start, continuing without it");
    } else if (rtp_init() != ESP_OK) {
        ESP_LOGE(TAG, "rtp receiver failed to start, continuing without it");
    }
    ESP_ERROR_CHECK(shell_init());
    ESP_ERROR_CHECK(cmd_system_register());
    ESP_ERROR_CHECK(cmd_led_register());
    ESP_ERROR_CHECK(cmd_oled_register());
    ESP_ERROR_CHECK(cmd_digi_register());
    ESP_ERROR_CHECK(cmd_wifi_register());
    ESP_ERROR_CHECK(cmd_rtp_register());
    ESP_ERROR_CHECK(cmd_config_register());
    ESP_ERROR_CHECK(cmd_ota_register());
    ESP_ERROR_CHECK(cmd_ir_register());
    if (net && web_start() != ESP_OK) {
        ESP_LOGE(TAG, "web UI failed to start, continuing without it");
    }
    if (net && telnet_start() != ESP_OK) {
        ESP_LOGE(TAG, "telnet failed to start, continuing without it");
    }
    if (net && remote_init() != ESP_OK) {
        ESP_LOGE(TAG, "remote button server failed to start, continuing without it");
    }
    ESP_ERROR_CHECK(shell_start());
}
