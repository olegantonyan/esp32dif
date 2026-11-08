#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include "argtable3/argtable3.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_stdio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "shell.h"

#define PROMPT "esp32dif> "
#define MAX_CMDLINE 256

static const char *TAG = "shell";

static SemaphoreHandle_t s_lock;

esp_err_t shell_init(void)
{
    esp_console_config_t cfg = ESP_CONSOLE_CONFIG_DEFAULT();
    cfg.max_cmdline_length = MAX_CMDLINE;
    cfg.max_cmdline_args = 16;
    ESP_RETURN_ON_ERROR(esp_console_init(&cfg), TAG, "console init");
    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "register help");
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(&esp_console_get_completion);
    linenoiseSetHintsCallback((linenoiseHintsCallback *)&esp_console_get_hint);
    linenoiseHistorySetMaxLen(32);
    linenoiseSetMaxLineLen(MAX_CMDLINE);
    fflush(stdout);
    fsync(fileno(stdout));
    return esp_stdio_install_io_driver();
}

int shell_exec(const char *line)
{
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        printf("waiting for command on other session...\n");
        fflush(stdout);
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    int ret = 0;
    esp_err_t err = esp_console_run(line, &ret);
    if (err == ESP_ERR_NOT_FOUND) {
        printf("Unrecognized command\n");
    } else if (err == ESP_OK && ret != 0) {
        printf("Command returned non-zero error code: 0x%x (%s)\n", ret, esp_err_to_name(ret));
    } else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
        printf("Internal error: %s\n", esp_err_to_name(err));
    }
    fflush(stdout);
    fflush(stderr);
    xSemaphoreGive(s_lock);
    return ret;
}

static void uart_task(void *arg)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    printf("\nType 'help' to get the list of commands.\n");
    if (linenoiseIsDumbMode()) {
        printf("Terminal doesn't support escape sequences: line editing and history are off.\n");
    }
    for (;;) {
        char *line = linenoise(PROMPT);
        if (!line) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (line[0]) {
            linenoiseHistoryAdd(line);
        }
        shell_exec(line);
        linenoiseFree(line);
    }
}

esp_err_t shell_start(void)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "shell_init not called");
    if (linenoiseProbe() != 0) {
        linenoiseSetDumbMode(1);
    }
    ESP_RETURN_ON_FALSE(xTaskCreate(uart_task, "shell", 8192, NULL, 2, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

static struct arg_end *argtable_end(void **table)
{
    size_t i = 0;
    while (!(((struct arg_hdr *)table[i])->flag & ARG_TERMINATOR)) {
        i++;
    }
    return table[i];
}

bool shell_args_ok(int argc, char **argv, void *argtable)
{
    if (arg_parse(argc, argv, argtable) == 0) {
        return true;
    }
    arg_print_errors(stderr, argtable_end(argtable), argv[0]);
    return false;
}

static void print_subcmd_usage(const char *cmd, const shell_subcmd_t *subs, size_t n)
{
    printf("usage: %s <command> [args]\n", cmd);
    for (size_t i = 0; i < n; i++) {
        printf("  %s %s", cmd, subs[i].name);
        if (subs[i].argtable) {
            arg_print_syntax(stdout, subs[i].argtable, "");
        }
        printf("\n      %s\n", subs[i].help);
    }
}

int shell_subcmd_run(const char *cmd, const shell_subcmd_t *subs, size_t n, int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        print_subcmd_usage(cmd, subs, n);
        return argc < 2;
    }
    for (size_t i = 0; i < n; i++) {
        if (strcmp(argv[1], subs[i].name) == 0) {
            return subs[i].func(argc - 1, argv + 1);
        }
    }
    fprintf(stderr, "%s: unknown command '%s'\n", cmd, argv[1]);
    print_subcmd_usage(cmd, subs, n);
    return 1;
}

int shell_getc(int timeout_ms)
{
    int fd = fileno(stdin);
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
        return -1;
    }
    unsigned char c;
    return read(fd, &c, 1) == 1 ? c : SHELL_CTRL_C;
}
