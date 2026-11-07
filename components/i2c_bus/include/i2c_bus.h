#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t i2c_bus_get(i2c_master_bus_handle_t *out);
void i2c_bus_lock(void);
void i2c_bus_unlock(void);
