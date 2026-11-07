#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "rtp.h"

bool sap_parse(const uint8_t *p, int len, uint32_t origin, rtp_session_t *out, bool *deleted);

esp_err_t play_init(void);
void play_format(uint32_t bytes_per_frame, uint32_t rate);
void play_push(const uint8_t *data, size_t bytes);
void play_silence(uint32_t frames);
void play_rebase(void);
uint32_t play_max_gap_frames(void);
void play_stop(void);
void play_get(rtp_stats_t *out);
