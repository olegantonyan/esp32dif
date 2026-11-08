#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "settings.h"

#define NVS_NS "cfg"

_Static_assert(sizeof(CONFIG_APP_DEFAULT_PASSWORD) >= 9 && sizeof(CONFIG_APP_DEFAULT_PASSWORD) <= 64, "CONFIG_APP_DEFAULT_PASSWORD must be 8-63 characters");

static const char *TAG = "settings";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static settings_t s_cur;
static settings_cb_t s_cb;

static void load(const char *key, char *buf, size_t len, const char *def)
{
    nvs_handle_t h;
    size_t n = len;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_str(h, key, buf, &n);
        nvs_close(h);
        if (err == ESP_OK) {
            return;
        }
    }
    strlcpy(buf, def, len);
}

static esp_err_t store(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");
    load("host", s_cur.hostname, sizeof(s_cur.hostname), CONFIG_APP_NET_HOSTNAME);
    load("pass", s_cur.password, sizeof(s_cur.password), CONFIG_APP_DEFAULT_PASSWORD);
    return ESP_OK;
}

void settings_get(settings_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_cur;
    taskEXIT_CRITICAL(&s_lock);
}

static void changed(void)
{
    if (s_cb) {
        settings_t s;
        settings_get(&s);
        s_cb(&s);
    }
}

static const char *check_hostname(const char *h)
{
    size_t len = strlen(h);
    if (len < 1 || len > SETTINGS_HOSTNAME_MAX) {
        return "hostname must be 1-32 characters";
    }
    for (size_t i = 0; i < len; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return "hostname may only contain a-z, 0-9 and '-'";
        }
    }
    return h[0] == '-' || h[len - 1] == '-' ? "hostname can't start or end with '-'" : NULL;
}

static const char *check_password(const char *p)
{
    size_t len = strlen(p);
    if (len < 8 || len > SETTINGS_PASSWORD_MAX) {
        return "password must be 8-63 characters";
    }
    for (size_t i = 0; i < len; i++) {
        if (p[i] < 0x20 || p[i] > 0x7e) {
            return "password may only contain printable ASCII";
        }
    }
    return NULL;
}

static const char *set(const char *key, char *field, size_t len, const char *val, const char *err)
{
    if (err) {
        return err;
    }
    if (store(key, val) != ESP_OK) {
        return "saving to flash failed";
    }
    taskENTER_CRITICAL(&s_lock);
    strlcpy(field, val, len);
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "%s changed", key);
    changed();
    return NULL;
}

const char *settings_set_hostname(const char *hostname)
{
    return set("host", s_cur.hostname, sizeof(s_cur.hostname), hostname, check_hostname(hostname));
}

const char *settings_set_password(const char *password)
{
    return set("pass", s_cur.password, sizeof(s_cur.password), password, check_password(password));
}

bool settings_check_password(const char *password)
{
    settings_t s;
    settings_get(&s);
    size_t a = strlen(password), b = strlen(s.password);
    unsigned char diff = a != b;
    for (size_t i = 0; i < sizeof(s.password); i++) {
        diff |= (unsigned char)(i < a ? password[i] : 0) ^ (unsigned char)(i < b ? s.password[i] : 0);
    }
    return !diff;
}

void settings_on_change(settings_cb_t cb)
{
    s_cb = cb;
}
