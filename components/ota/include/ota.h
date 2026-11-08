#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    const char *running;
    char version[32];
    bool pending_verify;
    int valid_in_s;
    const char *other;
    char other_version[32];
    bool rollback_possible;
    bool updating;
    size_t written;
    size_t total;
} ota_status_t;

esp_err_t ota_init(void);
const char *ota_begin(size_t size);
const char *ota_write(const void *buf, size_t len);
const char *ota_finish(bool ok);
void ota_reboot(int delay_ms);
void ota_status(ota_status_t *out);
const char *ota_state_str(const ota_status_t *st);
esp_err_t ota_rollback(void);
