#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_console.h"
#include "esp_err.h"

#define SHELL_CTRL_C 0x03
#define SHELL_CTRL_D 0x04

typedef struct {
    const char *name;
    const char *help;
    esp_console_cmd_func_t func;
    void *argtable;
} shell_subcmd_t;

esp_err_t shell_init(void);
esp_err_t shell_start(void);

bool shell_args_ok(int argc, char **argv, void *argtable);
int shell_subcmd_run(const char *cmd, const shell_subcmd_t *subs, size_t n, int argc, char **argv);
int shell_getc(int timeout_ms);
