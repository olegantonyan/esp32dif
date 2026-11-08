#include <stdio.h>
#include "esp_check.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota.h"
#include "shell.h"
#include "cmd_ota.h"

static const char *TAG = "cmd_ota";

static int cmd_status(int argc, char **argv)
{
    ota_status_t st;
    ota_status(&st);
    printf("running    %s on %s\n", st.version, st.running);
    if (st.pending_verify) {
        printf("state      pending verify, marked valid in %ds; a reset before that rolls back\n", st.valid_in_s);
    } else {
        printf("state      %s\n", ota_state_str(&st));
    }
    if (st.updating) {
        if (st.total) {
            printf("progress   %zu of %zu KB (%d%%)\n", st.written / 1024, st.total / 1024, (int)(st.written * 100 / st.total));
        } else {
            printf("progress   %zu KB\n", st.written / 1024);
        }
    }
    printf("other slot %s: %s\n", st.other ? st.other : "-", st.other_version[0] ? st.other_version : "empty");
    printf("rollback   %s\n", st.rollback_possible ? "available (ota rollback)" : "not available");
    return 0;
}

static int cmd_rollback(int argc, char **argv)
{
    ota_status_t st;
    ota_status(&st);
    if (st.updating || !st.rollback_possible) {
        fprintf(stderr, "ota: %s\n", st.updating ? "update in progress" : "no previous firmware to roll back to");
        return 1;
    }
    printf("rolling back to %s on %s, rebooting\n", st.other_version[0] ? st.other_version : "previous firmware", st.other);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t err = ota_rollback();
    fprintf(stderr, "ota: rollback failed: %s\n", esp_err_to_name(err));
    return 1;
}

static const shell_subcmd_t s_subcmds[] = {
    { .name = "status", .help = "Running firmware, verify state, other slot, update progress", .func = cmd_status },
    { .name = "rollback", .help = "Reboot into the previous firmware", .func = cmd_rollback },
};

static int cmd_ota(int argc, char **argv)
{
    return shell_subcmd_run("ota", s_subcmds, sizeof(s_subcmds) / sizeof(s_subcmds[0]), argc, argv);
}

esp_err_t cmd_ota_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "ota",
        .help = "Firmware update (upload via web UI or make ota): status | rollback",
        .hint = "<command> [args]",
        .func = cmd_ota,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register ota");
    return ESP_OK;
}
