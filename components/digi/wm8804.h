#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t wm8804_attach(i2c_master_bus_handle_t bus, uint8_t addr);
esp_err_t wm8804_read_id(uint16_t *id, uint8_t *rev);
esp_err_t wm8804_reset(void);
esp_err_t wm8804_reg_read(uint8_t reg, uint8_t *val);
esp_err_t wm8804_reg_write(uint8_t reg, uint8_t val);
bool wm8804_rate_supported(uint32_t rate);
esp_err_t wm8804_start(uint32_t xtal_hz, uint32_t rate);
esp_err_t wm8804_trim(int32_t ppb, bool rewrite, uint32_t *k);
