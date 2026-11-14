#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ir.h"

#define REMOTE_MAX_CLIENTS 2
#define REMOTE_TRACK_MAX 128
#define REMOTE_PLAYER_MAX 32
#define REMOTE_STATUS_MAX 12
#define REMOTE_ID_MAX 64

typedef enum {
    REMOTE_PREVIOUS,
    REMOTE_PLAY_PAUSE,
    REMOTE_NEXT,
    REMOTE_VOLUME_UP,
    REMOTE_VOLUME_DOWN,
    REMOTE_MUTE,
} remote_action_t;

typedef struct {
    char artist[REMOTE_TRACK_MAX];
    char title[REMOTE_TRACK_MAX];
    char player[REMOTE_PLAYER_MAX];
    char status[REMOTE_STATUS_MAX];
    char id[REMOTE_ID_MAX];
    int8_t volume;
    bool muted;
} remote_track_t;

typedef struct {
    const char *remote;
    ir_proto_t proto;
    uint16_t addr;
    uint8_t cmd;
    remote_action_t action;
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
const char *remote_action_name(remote_action_t action);
bool remote_action_from_name(const char *name, remote_action_t *out);
int remote_send(remote_action_t action);
const remote_key_t *remote_lookup(const ir_frame_t *frame);
const remote_key_t *remote_keys(size_t *count);
void remote_get(remote_status_t *out);
bool remote_track(uint32_t ip, remote_track_t *out);
