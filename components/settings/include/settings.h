#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define SETTINGS_HOSTNAME_MAX 32
#define SETTINGS_PASSWORD_MAX 63

typedef struct {
    char hostname[SETTINGS_HOSTNAME_MAX + 1];
    char password[SETTINGS_PASSWORD_MAX + 1];
} settings_t;

typedef void (*settings_cb_t)(const settings_t *s);

esp_err_t settings_init(void);
void settings_get(settings_t *out);
const char *settings_set_hostname(const char *hostname);
const char *settings_set_password(const char *password);
bool settings_check_password(const char *password);
void settings_on_change(settings_cb_t cb);
