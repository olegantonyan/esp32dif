#include <stdio.h>
#include "argtable3/argtable3.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "ir.h"
#include "lwip/inet.h"
#include "remote.h"
#include "shell.h"
#include "cmd_ir.h"

static const char *TAG = "cmd_ir";

static struct {
    struct arg_lit *raw;
    struct arg_end *end;
} s_watch_args;

static ir_frame_t s_frame;

static void print_frame(const ir_frame_t *f, bool raw)
{
    printf("%-7s", ir_proto_name(f->proto));
    if (f->proto == IR_UNKNOWN) {
        printf(" %u pulses%s\n", f->n, f->overflow ? " (truncated)" : "");
        raw = true;
    } else if (f->proto == IR_REPEAT && !f->code) {
        printf("\n");
    } else {
        const remote_key_t *key = remote_lookup(f);
        printf(" addr 0x%04x cmd 0x%02x code 0x%08lx%s", f->addr, f->cmd, (unsigned long)f->code, f->valid ? "" : " (bad checksum)");
        printf(key ? "  %s %s\n" : "\n", key ? key->remote : "", key ? key->action : "");
    }
    if (!raw) {
        return;
    }
    for (int i = 0; i < f->n; i++) {
        bool mark = (i % 2 == 0) == f->first_low;
        printf("%c%u%s", mark ? '+' : '-', f->us[i], i % 16 == 15 || i == f->n - 1 ? "\n" : " ");
    }
}

static int cmd_status(int argc, char **argv)
{
    ir_get(&s_frame);
    int level = ir_level();
    printf("gpio    %d, line %s\n", CONFIG_APP_IR_GPIO, level ? "high (idle)" : "LOW (receiver active or wiring fault)");
    printf("frames  %lu\n", (unsigned long)s_frame.seq);
    printf("noise   %lu\n", (unsigned long)s_frame.noise);
    if (s_frame.seq) {
        printf("last    %lld ms ago: ", (esp_timer_get_time() - s_frame.at_us) / 1000);
        print_frame(&s_frame, false);
    }
    remote_status_t r;
    remote_get(&r);
    printf("clients %d (TCP port %d)", r.clients, CONFIG_APP_REMOTE_PORT);
    for (int i = 0; i < r.clients; i++) {
        char ip[16];
        printf(" %s", inet_ntoa_r(r.client_ip[i], ip, sizeof(ip)));
    }
    printf("\nsent    %lu\n", (unsigned long)r.sent);
    if (r.last_action) {
        char ip[16];
        printf("last    %s, %lld s ago, to %s\n", r.last_action, (esp_timer_get_time() - r.last_us) / 1000000, r.last_ip ? inet_ntoa_r(r.last_ip, ip, sizeof(ip)) : "nobody");
    }
    return 0;
}

static int cmd_watch(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_watch_args)) {
        return 1;
    }
    bool raw = s_watch_args.raw->count;
    ir_get(&s_frame);
    uint32_t seq = s_frame.seq, noise = s_frame.noise;
    printf("press remote buttons (Ctrl+C to stop)\n");
    while (shell_getc(50) != SHELL_CTRL_C) {
        ir_get(&s_frame);
        if (s_frame.noise != noise) {
            printf("noise   %lu short bursts\n", (unsigned long)(s_frame.noise - noise));
            noise = s_frame.noise;
        }
        if (s_frame.seq != seq) {
            if (s_frame.seq - seq > 1) {
                printf("missed  %lu frames\n", (unsigned long)(s_frame.seq - seq - 1));
            }
            seq = s_frame.seq;
            print_frame(&s_frame, raw);
            fflush(stdout);
        }
    }
    return 0;
}

static int cmd_keys(int argc, char **argv)
{
    size_t n;
    const remote_key_t *keys = remote_keys(&n);
    for (size_t i = 0; i < n; i++) {
        printf("%-20s %-7s addr 0x%04x cmd 0x%02x  %s\n", keys[i].remote, ir_proto_name(keys[i].proto), keys[i].addr, keys[i].cmd, keys[i].action);
    }
    return 0;
}

static const shell_subcmd_t s_subcmds[] = {
    { .name = "status", .help = "Line level, frame count and the last frame", .func = cmd_status },
    { .name = "keys", .help = "Known remote buttons and their actions", .func = cmd_keys },
    { .name = "watch", .help = "Print frames as they arrive (-r: pulse widths in us, + = IR on)", .func = cmd_watch, .argtable = &s_watch_args },
};

static int cmd_ir(int argc, char **argv)
{
    return shell_subcmd_run("ir", s_subcmds, sizeof(s_subcmds) / sizeof(s_subcmds[0]), argc, argv);
}

esp_err_t cmd_ir_register(void)
{
    s_watch_args.raw = arg_lit0("r", "raw", "also print pulse widths");
    s_watch_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "ir",
        .help = "IR receiver and remote buttons: status | keys | watch [-r]",
        .hint = "<command> [args]",
        .func = cmd_ir,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register ir");
    return ESP_OK;
}
