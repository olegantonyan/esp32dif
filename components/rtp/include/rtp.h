#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define RTP_MDNS_SERVICE "_spdif-rtp"
#define RTP_MDNS_PROTO "_udp"
#define RTP_LEASES 4

typedef enum {
    RTP_PLAY_IDLE,
    RTP_PLAY_BUFFERING,
    RTP_PLAY_RUNNING,
} rtp_play_state_t;

typedef struct {
    bool valid;
    char name[32];
    char encoding[8];
    uint32_t rate;
    uint8_t channels;
    uint8_t pt;
    uint16_t port;
    uint32_t ssrc;
    uint32_t origin_ip;
    int64_t seen_us;
} rtp_session_t;

typedef struct {
    uint32_t ip;
    int64_t until_us;
} rtp_lease_t;

typedef struct {
    bool active;
    uint32_t streams;
    uint32_t src_ip;
    uint16_t src_port;
    uint32_t ssrc;
    uint8_t pt;
    uint32_t packets;
    uint32_t lost;
    uint32_t late;
    uint32_t invalid;
    uint64_t payload_bytes;
    uint32_t bytes_per_frame;
    uint32_t frames_per_packet;
    uint32_t rate_hz;
    int32_t drift_ppb;
    uint32_t jitter_us;
    uint32_t max_gap_us;
    int64_t started_us;
    int64_t last_us;
    uint32_t rebases;
    rtp_play_state_t play_state;
    bool buffer_psram;
    uint32_t buffer_ms;
    uint32_t buffer_level_ms;
    uint32_t target_ms;
    uint32_t buffer_max_ms;
    uint32_t underruns;
    uint32_t rebuffers;
    uint32_t late_dropped_ms;
    uint32_t overflows;
    uint32_t write_errors;
    int32_t trim_ppb;
    int32_t offset_ppb;
    uint32_t trim_writes;
    uint32_t trim_errors;
    int32_t slip_ppb;
    uint32_t slipped;
    rtp_session_t session;
    uint32_t denied;
    int leases;
    rtp_lease_t lease[RTP_LEASES];
} rtp_stats_t;

esp_err_t rtp_init(void);
void rtp_get(rtp_stats_t *out);
void rtp_reset(void);
void rtp_allow(uint32_t ip);
