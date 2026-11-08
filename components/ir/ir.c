#include <string.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "ir.h"

#define FRAME_END_US 15000
#define FRAME_MIN_PULSES 3

static const char *TAG = "ir";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static bool s_active;
static bool s_first_low;
static bool s_overflow;
static int64_t s_last_us;
static uint16_t s_n;
static uint16_t s_us[IR_MAX_PULSES];

static SemaphoreHandle_t s_lock;
static ir_handler_t s_handler;
static ir_frame_t s_frame;

static void edge_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    portENTER_CRITICAL_ISR(&s_mux);
    if (!s_active) {
        s_active = true;
        s_n = 0;
        s_overflow = false;
        s_first_low = gpio_get_level(CONFIG_APP_IR_GPIO) == 0;
        vTaskNotifyGiveFromISR(s_task, &woken);
    } else if (s_n < IR_MAX_PULSES) {
        int64_t d = now - s_last_us;
        s_us[s_n++] = d > UINT16_MAX ? UINT16_MAX : d;
    } else {
        s_overflow = true;
    }
    s_last_us = now;
    portEXIT_CRITICAL_ISR(&s_mux);
    portYIELD_FROM_ISR(woken);
}

static bool near(uint32_t v, uint32_t ref)
{
    return v > ref * 7 / 10 && v < ref * 13 / 10;
}

static void decode(ir_frame_t *f, const ir_frame_t *prev)
{
    const uint16_t *d = f->us;
    if (near(d[0], 9000) && near(d[1], 2250) && f->n < 5) {
        f->proto = IR_REPEAT;
        if (prev->proto == IR_NEC || prev->proto == IR_REPEAT) {
            f->code = prev->code;
            f->addr = prev->addr;
            f->cmd = prev->cmd;
            f->valid = prev->valid;
        }
        return;
    }
    f->proto = IR_UNKNOWN;
    ir_proto_t proto = near(d[0], 9000) && near(d[1], 4500) ? IR_NEC : near(d[0], 4500) && near(d[1], 4500) ? IR_SAMSUNG : IR_NONE;
    if (proto == IR_NONE || f->n < 67) {
        return;
    }
    uint32_t code = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t mark = d[2 + 2 * i], space = d[3 + 2 * i];
        if (mark < 300 || mark > 900 || space < 300 || space > 2300) {
            return;
        }
        code |= (uint32_t)(space > 1125) << i;
    }
    uint8_t a0 = code, a1 = code >> 8;
    f->proto = proto;
    f->code = code;
    f->addr = (proto == IR_NEC && a1 == (uint8_t)~a0) || (proto == IR_SAMSUNG && a1 == a0) ? a0 : a0 | a1 << 8;
    f->cmd = code >> 16;
    f->valid = (uint8_t)(code >> 24) == (uint8_t)~f->cmd;
}

static void ir_task(void *arg)
{
    static ir_frame_t f;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool done = false;
        while (!done) {
            vTaskDelay(pdMS_TO_TICKS(5));
            portENTER_CRITICAL(&s_mux);
            if (esp_timer_get_time() - s_last_us >= FRAME_END_US) {
                f.n = s_n;
                memcpy(f.us, s_us, s_n * sizeof(s_us[0]));
                f.first_low = s_first_low;
                f.overflow = s_overflow;
                f.at_us = s_last_us;
                s_active = false;
                done = true;
            }
            portEXIT_CRITICAL(&s_mux);
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool frame = f.n >= FRAME_MIN_PULSES;
        if (!frame) {
            s_frame.noise++;
        } else {
            f.seq = s_frame.seq + 1;
            f.noise = s_frame.noise;
            f.code = f.addr = f.cmd = 0;
            f.valid = false;
            decode(&f, &s_frame);
            s_frame = f;
        }
        xSemaphoreGive(s_lock);
        if (frame && s_handler) {
            s_handler(&f);
        }
    }
}

esp_err_t ir_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "lock");
    ESP_RETURN_ON_FALSE(xTaskCreate(ir_task, "ir", 4096, NULL, 5, &s_task) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_APP_IR_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");
    esp_err_t err = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "isr service");
    return gpio_isr_handler_add(CONFIG_APP_IR_GPIO, edge_isr, NULL);
}

void ir_get(ir_frame_t *out)
{
    if (!s_lock) {
        *out = (ir_frame_t){ 0 };
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_frame;
    xSemaphoreGive(s_lock);
}

void ir_set_handler(ir_handler_t fn)
{
    s_handler = fn;
}

int ir_level(void)
{
    return gpio_get_level(CONFIG_APP_IR_GPIO);
}

const char *ir_proto_name(ir_proto_t proto)
{
    static const char *names[] = {
        [IR_NONE] = "none", [IR_NEC] = "NEC", [IR_SAMSUNG] = "Samsung", [IR_REPEAT] = "repeat", [IR_UNKNOWN] = "unknown",
    };
    return names[proto];
}
