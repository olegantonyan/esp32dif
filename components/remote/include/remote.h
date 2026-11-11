#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ir.h"

#define REMOTE_MAX_CLIENTS 2
#define REMOTE_TRACK_MAX 128

typedef struct {
    const char *remote;
    ir_proto_t proto;
    uint16_t addr;
    uint8_t cmd;
    const char *action;
} remote_key_t;

typedef struct {
    int clients;
    uint32_t client_ip[REMOTE_MAX_CLIENTS];
    uint32_t sent;
    const char *last_action;
    int64_t last_us;
    uint32_t last_ip;
} remote_status_t;

esp_err_t remote_init(void);
const remote_key_t *remote_lookup(const ir_frame_t *frame);
const remote_key_t *remote_keys(size_t *count);
void remote_get(remote_status_t *out);
bool remote_track(uint32_t ip, char artist[REMOTE_TRACK_MAX], char title[REMOTE_TRACK_MAX]);
