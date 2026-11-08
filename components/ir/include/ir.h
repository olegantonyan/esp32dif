#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define IR_MAX_PULSES 160

typedef enum { IR_NONE, IR_NEC, IR_SAMSUNG, IR_REPEAT, IR_UNKNOWN } ir_proto_t;

typedef struct {
    uint32_t seq;
    uint32_t noise;
    int64_t at_us;
    ir_proto_t proto;
    uint32_t code;
    uint16_t addr;
    uint8_t cmd;
    bool valid;
    bool first_low;
    bool overflow;
    uint16_t n;
    uint16_t us[IR_MAX_PULSES];
} ir_frame_t;

typedef void (*ir_handler_t)(const ir_frame_t *frame);

esp_err_t ir_init(void);
void ir_set_handler(ir_handler_t fn);
void ir_get(ir_frame_t *out);
int ir_level(void);
const char *ir_proto_name(ir_proto_t proto);
