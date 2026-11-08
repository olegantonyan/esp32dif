#include <string.h>
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ota.h"

#define VALID_AFTER_S 60
#define HEAD_LEN (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

static const char *TAG = "ota";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_updating;
static esp_ota_handle_t s_handle;
static const esp_partition_t *s_part;
static size_t s_written, s_total;
static uint8_t s_head[HEAD_LEN];
static size_t s_head_len;
static int64_t s_valid_at_us;

static void mark_valid(void *arg)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "firmware marked valid");
    } else {
        ESP_LOGE(TAG, "mark valid: %s", esp_err_to_name(err));
    }
}

static void restart(void *arg)
{
    esp_restart();
}

static bool pending_verify(void)
{
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

esp_err_t ota_init(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "running %s from %s", esp_app_get_description()->version, run->label);
    if (!pending_verify()) {
        return ESP_OK;
    }
    const esp_timer_create_args_t args = { .callback = mark_valid, .name = "ota_valid" };
    esp_timer_handle_t timer;
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_once(timer, VALID_AFTER_S * 1000000LL), TAG, "timer start");
    s_valid_at_us = esp_timer_get_time() + VALID_AFTER_S * 1000000LL;
    ESP_LOGW(TAG, "new firmware, marked valid in %d s; a reset before that rolls back", VALID_AFTER_S);
    return ESP_OK;
}

const char *ota_begin(size_t size)
{
    taskENTER_CRITICAL(&s_lock);
    bool busy = s_updating;
    s_updating = true;
    taskEXIT_CRITICAL(&s_lock);
    if (busy) {
        return "update already in progress";
    }

    const char *err = NULL;
    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part) {
        err = "no OTA partition, flash the OTA partition table over serial";
    } else if (size > s_part->size) {
        err = "image larger than the app partition";
    } else if (esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_handle) != ESP_OK) {
        err = "cannot start writing the app partition";
    }
    if (err) {
        taskENTER_CRITICAL(&s_lock);
        s_updating = false;
        taskEXIT_CRITICAL(&s_lock);
        return err;
    }
    esp_ota_invalidate_inactive_ota_data_slot();
    taskENTER_CRITICAL(&s_lock);
    s_written = 0;
    s_total = size;
    s_head_len = 0;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "writing %s", s_part->label);
    return NULL;
}

static const char *check_head(void)
{
    const esp_image_header_t *img = (const esp_image_header_t *)s_head;
    const esp_app_desc_t *desc = (const esp_app_desc_t *)(s_head + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (img->magic != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "not an app image";
    }
    if (img->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return "image built for another chip";
    }
    if (strncmp(desc->project_name, esp_app_get_description()->project_name, sizeof(desc->project_name)) != 0) {
        return "image is for another project";
    }
    ESP_LOGI(TAG, "image %s %s", desc->project_name, desc->version);
    return NULL;
}

const char *ota_write(const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t n = len;
    if (s_head_len < HEAD_LEN) {
        size_t take = n < HEAD_LEN - s_head_len ? n : HEAD_LEN - s_head_len;
        memcpy(s_head + s_head_len, p, take);
        s_head_len += take;
        p += take;
        n -= take;
        if (s_head_len < HEAD_LEN) {
            return NULL;
        }
        const char *err = check_head();
        if (err) {
            return err;
        }
        if (esp_ota_write(s_handle, s_head, HEAD_LEN) != ESP_OK) {
            return "flash write failed";
        }
    }
    if (n && esp_ota_write(s_handle, p, n) != ESP_OK) {
        return "flash write failed";
    }
    taskENTER_CRITICAL(&s_lock);
    s_written += len;
    taskEXIT_CRITICAL(&s_lock);
    return NULL;
}

const char *ota_finish(bool ok)
{
    const char *err = NULL;
    if (!ok) {
        esp_ota_abort(s_handle);
    } else if (s_head_len < HEAD_LEN) {
        esp_ota_abort(s_handle);
        err = "image too short";
    } else if (esp_ota_end(s_handle) != ESP_OK) {
        err = "image is corrupt or incomplete";
    } else if (esp_ota_set_boot_partition(s_part) != ESP_OK) {
        err = "cannot select the new partition for boot";
    }
    if (ok) {
        if (err) {
            ESP_LOGE(TAG, "update failed: %s", err);
        } else {
            ESP_LOGI(TAG, "%s updated, boots on next restart", s_part->label);
        }
    }
    taskENTER_CRITICAL(&s_lock);
    s_updating = false;
    taskEXIT_CRITICAL(&s_lock);
    return err;
}

void ota_reboot(int delay_ms)
{
    const esp_timer_create_args_t args = { .callback = restart, .name = "ota_reboot" };
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_once(timer, delay_ms * 1000LL) != ESP_OK) {
        esp_restart();
    }
}

void ota_status(ota_status_t *out)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    *out = (ota_status_t){
        .running = run->label,
        .pending_verify = pending_verify(),
        .other = other ? other->label : NULL,
        .rollback_possible = esp_ota_check_rollback_is_possible(),
    };
    strlcpy(out->version, esp_app_get_description()->version, sizeof(out->version));
    if (out->pending_verify && s_valid_at_us) {
        int64_t left = s_valid_at_us - esp_timer_get_time();
        out->valid_in_s = left > 0 ? left / 1000000 + 1 : 0;
    }
    esp_app_desc_t desc;
    esp_ota_img_states_t state;
    taskENTER_CRITICAL(&s_lock);
    out->updating = s_updating;
    out->written = s_written;
    out->total = s_total;
    taskEXIT_CRITICAL(&s_lock);
    if (other && !out->updating && esp_ota_get_state_partition(other, &state) == ESP_OK && state != ESP_OTA_IMG_INVALID && state != ESP_OTA_IMG_ABORTED && esp_ota_get_partition_description(other, &desc) == ESP_OK) {
        strlcpy(out->other_version, desc.version, sizeof(out->other_version));
    }
}

const char *ota_state_str(const ota_status_t *st)
{
    return st->updating ? "updating" : st->pending_verify ? "pending verify" : "valid";
}

esp_err_t ota_rollback(void)
{
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}
