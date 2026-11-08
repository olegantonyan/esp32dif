#include <stdio.h>
#include "argtable3/argtable3.h"
#include "esp_check.h"
#include "esp_console.h"
#include "net.h"
#include "settings.h"
#include "shell.h"
#include "cmd_config.h"

static const char *TAG = "cmd_config";

static struct {
    struct arg_lit *password;
    struct arg_end *end;
} s_show_args;

static struct {
    struct arg_str *value;
    struct arg_end *end;
} s_value_args;

static int cmd_show(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_show_args)) {
        return 1;
    }
    settings_t cfg;
    settings_get(&cfg);
    printf("hostname   %s (%s.local, fallback AP ssid)\n", cfg.hostname, cfg.hostname);
    printf("password   %s (web UI user admin, telnet, fallback AP, RTP senders)\n", s_show_args.password->count ? cfg.password : "********");
    printf("ap         %s\n", net_ap_active() ? "on, 192.168.4.1" : "off");
    return 0;
}

static int set(int argc, char **argv, const char *(*setter)(const char *), const char *done)
{
    if (!shell_args_ok(argc, argv, &s_value_args)) {
        return 1;
    }
    const char *err = setter(s_value_args.value->sval[0]);
    if (err) {
        fprintf(stderr, "config: %s\n", err);
        return 1;
    }
    printf("%s\n", done);
    return 0;
}

static int cmd_hostname(int argc, char **argv)
{
    return set(argc, argv, settings_set_hostname, "saved; mDNS uses it now, DHCP from the next connect");
}

static int cmd_password(int argc, char **argv)
{
    return set(argc, argv, settings_set_password, "saved; esp32dif-sink needs the new SPDIF_PASSWORD");
}

static const shell_subcmd_t s_subcmds[] = {
    { .name = "show", .help = "Hostname, password, fallback AP", .func = cmd_show, .argtable = &s_show_args },
    { .name = "hostname", .help = "Set the hostname: a-z, 0-9, '-', up to 32 characters", .func = cmd_hostname, .argtable = &s_value_args },
    { .name = "password", .help = "Set the password for the web UI, telnet, fallback AP and RTP: 8-63 characters", .func = cmd_password, .argtable = &s_value_args },
};

static int cmd_config(int argc, char **argv)
{
    return shell_subcmd_run("config", s_subcmds, sizeof(s_subcmds) / sizeof(s_subcmds[0]), argc, argv);
}

esp_err_t cmd_config_register(void)
{
    s_show_args.password = arg_lit0("p", "password", "show the password");
    s_show_args.end = arg_end(1);
    s_value_args.value = arg_str1(NULL, NULL, "<value>", NULL);
    s_value_args.end = arg_end(1);

    const esp_console_cmd_t cmd = {
        .command = "config",
        .help = "Device settings: show | hostname | password",
        .hint = "<command> [args]",
        .func = cmd_config,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register config");
    return ESP_OK;
}
