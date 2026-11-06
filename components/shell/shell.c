#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include "argtable3/argtable3.h"
#include "esp_check.h"
#include "esp_console.h"
#include "shell.h"

static const char *TAG = "shell";

static esp_console_repl_t *s_repl;

esp_err_t shell_init(void)
{
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "c5> ";
    cfg.task_stack_size = 8192;
    cfg.max_cmdline_length = 256;
    cfg.max_cmdline_args = 16;
    cfg.max_history_len = 32;

    ESP_RETURN_ON_ERROR(esp_console_new_repl_stdio(&cfg, &s_repl), TAG, "create REPL");
    return esp_console_register_help_command();
}

esp_err_t shell_start(void)
{
    ESP_RETURN_ON_FALSE(s_repl, ESP_ERR_INVALID_STATE, TAG, "shell_init not called");
    return esp_console_start_repl(s_repl);
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
    return read(fd, &c, 1) == 1 ? c : -1;
}
