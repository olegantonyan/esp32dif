#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "i2c_bus.h"

#define RECOVERY_CLOCKS 9
#define RECOVERY_HALF_PERIOD_US 5

static const char *TAG = "i2c_bus";

static i2c_master_bus_handle_t s_bus;
static SemaphoreHandle_t s_quiet;

static void recover(void)
{
    const int sda = CONFIG_APP_I2C_SDA;
    const int scl = CONFIG_APP_I2C_SCL;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);
    gpio_config(&cfg);
    esp_rom_delay_us(RECOVERY_HALF_PERIOD_US);
    if (gpio_get_level(sda) && gpio_get_level(scl)) {
        return;
    }
    int clocks = 0;
    while (clocks < RECOVERY_CLOCKS && !gpio_get_level(sda)) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(RECOVERY_HALF_PERIOD_US);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(RECOVERY_HALF_PERIOD_US);
        clocks++;
    }
    gpio_set_level(sda, 0);
    esp_rom_delay_us(RECOVERY_HALF_PERIOD_US);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(RECOVERY_HALF_PERIOD_US);
    ESP_LOGW(TAG, "bus was held low, %d recovery clocks: SDA %s, SCL %s", clocks, gpio_get_level(sda) ? "high" : "LOW", gpio_get_level(scl) ? "high" : "LOW");
}

esp_err_t i2c_bus_get(i2c_master_bus_handle_t *out)
{
    if (!s_bus) {
        s_quiet = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_quiet, ESP_ERR_NO_MEM, TAG, "mutex");
        esp_log_level_set("i2c.common", ESP_LOG_ERROR);
        recover();
        i2c_master_bus_config_t cfg = {
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .i2c_port = 0,
            .sda_io_num = CONFIG_APP_I2C_SDA,
            .scl_io_num = CONFIG_APP_I2C_SCL,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_bus), TAG, "i2c bus");
        gpio_od_disable(CONFIG_APP_I2C_SCL);
    }
    *out = s_bus;
    return ESP_OK;
}

void i2c_bus_lock(void)
{
    if (s_quiet) {
        xSemaphoreTake(s_quiet, portMAX_DELAY);
    }
}

void i2c_bus_unlock(void)
{
    if (s_quiet) {
        xSemaphoreGive(s_quiet);
    }
}
