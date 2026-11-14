#include "esp_check.h"
#include "cmd_system.h"
#include "cmd_system_priv.h"

static const char *TAG = "cmd_system";

esp_err_t cmd_system_register(void)
{
    ESP_RETURN_ON_ERROR(cmd_sys_register(), TAG, "sys commands");
    ESP_RETURN_ON_ERROR(cmd_mem_register(), TAG, "mem commands");
    ESP_RETURN_ON_ERROR(cmd_i2c_register(), TAG, "i2c commands");
    return ESP_OK;
}
