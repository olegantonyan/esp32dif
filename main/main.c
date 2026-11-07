#include "esp_err.h"
#include "esp_log.h"
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
#include "status_led.h"
#include "status_oled.h"

static const char *TAG = "main";

void app_main(void)
{
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
    if (digi_init() != ESP_OK) {
        ESP_LOGW(TAG, "digi not ready, continuing without it");
    }
    if (net_init() != ESP_OK) {
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
    ESP_ERROR_CHECK(shell_start());
}
