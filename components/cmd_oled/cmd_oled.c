#include <stdio.h>
#include <string.h>
#include "argtable3/argtable3.h"
#include "esp_check.h"
#include "esp_console.h"
#include "oled.h"
#include "shell.h"
#include "cmd_oled.h"

static const char *TAG = "cmd_oled";

static struct {
    struct arg_int *row;
    struct arg_str *words;
    struct arg_end *end;
} s_print_args;

static struct {
    struct arg_int *level;
    struct arg_end *end;
} s_contrast_args;

static struct {
    struct arg_int *seconds;
    struct arg_end *end;
} s_idle_args;

static bool ready(void)
{
    oled_state_t st;
    oled_get(&st);
    if (!st.ready) {
        fprintf(stderr, "oled: no display, check wiring (see 'oled status')\n");
    }
    return st.ready;
}

static int cmd_status(int argc, char **argv)
{
    oled_state_t st;
    oled_get(&st);
    printf("display  SSD1306 %dx%d at 0x%02x, %s\n", OLED_WIDTH, OLED_HEIGHT, CONFIG_APP_OLED_ADDR, st.ready ? "found" : "not found");
    printf("pins     SDA GPIO%d, SCL GPIO%d\n", CONFIG_APP_I2C_SDA, CONFIG_APP_I2C_SCL);
    if (st.ready) {
        printf("mode     %s\n", st.auto_mode ? "auto (status screen)" : "manual");
        printf("power    %s\n", !st.on ? "off" : st.asleep ? "asleep (idle)" : "on");
        printf("contrast %u\n", st.contrast);
        if (st.idle_s) {
            printf("idle     off after %lu s\n", (unsigned long)st.idle_s);
        } else {
            printf("idle     never off\n");
        }
    }
    printf("text     %d rows x %d cols\n", OLED_ROWS, OLED_COLS);
    return 0;
}

static int cmd_auto(int argc, char **argv)
{
    if (!ready()) {
        return 1;
    }
    if (!oled_auto()) {
        fprintf(stderr, "oled: no auto mode handler installed\n");
        return 1;
    }
    return 0;
}

static int cmd_print(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_print_args) || !ready()) {
        return 1;
    }
    int row = s_print_args.row->ival[0];
    if (row < 0 || row >= OLED_ROWS) {
        fprintf(stderr, "oled: row must be 0-%d\n", OLED_ROWS - 1);
        return 1;
    }
    char text[OLED_COLS + 1] = "";
    for (int i = 0; i < s_print_args.words->count; i++) {
        if (i) {
            strlcat(text, " ", sizeof(text));
        }
        strlcat(text, s_print_args.words->sval[i], sizeof(text));
    }
    oled_print(row, text);
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    if (!ready()) {
        return 1;
    }
    oled_clear();
    return 0;
}

static int set_power(bool on)
{
    return ready() && oled_power(on) == ESP_OK ? 0 : 1;
}

static int cmd_on(int argc, char **argv)
{
    return set_power(true);
}

static int cmd_off(int argc, char **argv)
{
    return set_power(false);
}

static int cmd_contrast(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_contrast_args) || !ready()) {
        return 1;
    }
    int level = s_contrast_args.level->ival[0];
    if (level < 0 || level > 255) {
        fprintf(stderr, "oled: contrast must be 0-255\n");
        return 1;
    }
    return oled_contrast(level) == ESP_OK ? 0 : 1;
}

static int cmd_idle(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_idle_args) || !ready()) {
        return 1;
    }
    int seconds = s_idle_args.seconds->ival[0];
    if (seconds < 0 || seconds > 86400) {
        fprintf(stderr, "oled: idle must be 0-86400\n");
        return 1;
    }
    oled_idle(seconds);
    return 0;
}

static const shell_subcmd_t s_subcmds[] = {
    { .name = "status", .help = "Display, pins, mode, power and contrast", .func = cmd_status },
    { .name = "auto", .help = "Show the status screen (default)", .func = cmd_auto },
    { .name = "print", .help = "Write text to a row, switches to manual mode", .func = cmd_print, .argtable = &s_print_args },
    { .name = "clear", .help = "Blank the screen, switches to manual mode", .func = cmd_clear },
    { .name = "on", .help = "Display on", .func = cmd_on },
    { .name = "off", .help = "Display off (sleep)", .func = cmd_off },
    { .name = "contrast", .help = "Contrast, default 127", .func = cmd_contrast, .argtable = &s_contrast_args },
    { .name = "idle", .help = "Turn off after this many idle seconds, 0 = never", .func = cmd_idle, .argtable = &s_idle_args },
};

static int cmd_oled(int argc, char **argv)
{
    return shell_subcmd_run("oled", s_subcmds, sizeof(s_subcmds) / sizeof(s_subcmds[0]), argc, argv);
}

esp_err_t cmd_oled_register(void)
{
    s_print_args.row = arg_int1(NULL, NULL, "<row>", "0-based text row");
    s_print_args.words = arg_strn(NULL, NULL, "<text>", 1, 16, "words, joined by spaces");
    s_print_args.end = arg_end(2);
    s_contrast_args.level = arg_int1(NULL, NULL, "<0-255>", "level");
    s_contrast_args.end = arg_end(2);
    s_idle_args.seconds = arg_int1(NULL, NULL, "<seconds>", "0 = never");
    s_idle_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "oled",
        .help = "OLED display: status | auto | print | clear | on | off | contrast | idle",
        .hint = "<command> [args]",
        .func = cmd_oled,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register oled");
    return ESP_OK;
}
