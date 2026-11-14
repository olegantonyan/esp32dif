#include <stdio.h>
#include "esp_check.h"
#include "esp_console.h"
#include "i2c_bus.h"
#include "sdkconfig.h"
#include "cmd_system_priv.h"

#define I2C_SCAN_FIRST 0x08
#define I2C_SCAN_LAST 0x77
#define I2C_SCAN_TIMEOUT_MS 20

static const char *TAG = "cmd_i2c";

static int cmd_i2cscan(int argc, char **argv)
{
    i2c_master_bus_handle_t bus;
    if (i2c_bus_get(&bus) != ESP_OK) {
        fprintf(stderr, "i2cscan: no I2C bus\n");
        return 1;
    }
    printf("SDA GPIO%d, SCL GPIO%d\n", CONFIG_APP_I2C_SDA, CONFIG_APP_I2C_SCL);
    int found = 0;
    for (uint16_t addr = I2C_SCAN_FIRST; addr <= I2C_SCAN_LAST; addr++) {
        i2c_bus_lock();
        esp_err_t err = i2c_master_probe(bus, addr, I2C_SCAN_TIMEOUT_MS);
        i2c_bus_unlock();
        if (err == ESP_OK) {
            printf("0x%02x\n", addr);
            found++;
        }
    }
    printf("%d device%s\n", found, found == 1 ? "" : "s");
    return 0;
}

esp_err_t cmd_i2c_register(void)
{
    const esp_console_cmd_t cmd = { .command = "i2cscan", .help = "List addresses that answer on the I2C bus", .func = cmd_i2cscan };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register %s", cmd.command);
    return ESP_OK;
}
