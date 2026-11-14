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

static oled_id_t s_first, s_last;
static bool s_named;

typedef int (*each_fn_t)(oled_id_t id);

static int each(each_fn_t fn)
{
    int done = 0, rc = 0;
    for (oled_id_t id = s_first; id <= s_last; id++) {
        oled_state_t st;
        oled_get(id, &st);
        if (st.ready) {
            rc |= fn(id);
            done++;
        }
    }
    if (!done) {
        fprintf(stderr, "oled: %s%s not found, check wiring (see 'oled status')\n", s_named ? oled_name(s_first) : "no display", s_named ? " display" : "");
        return 1;
    }
    return rc;
}

static int status_one(oled_id_t id)
{
    oled_state_t st;
    oled_get(id, &st);
    printf("%-5s    SSD1306 128x%u at 0x%02x, %s\n", oled_name(id), st.height, st.addr, st.ready ? "found" : "not found");
    if (st.ready) {
        printf("  mode     %s\n", st.auto_mode ? "auto (status screen)" : "manual");
        printf("  power    %s\n", !st.on ? "off" : st.asleep ? "asleep (idle)" : "on");
        printf("  contrast %u\n", st.contrast);
        if (st.idle_s) {
            printf("  idle     off after %lu s\n", (unsigned long)st.idle_s);
        } else {
            printf("  idle     never off\n");
        }
        printf("  text     %d rows x %d cols\n", oled_rows(id), OLED_COLS);
    }
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    printf("pins     SDA GPIO%d, SCL GPIO%d\n", CONFIG_APP_I2C_SDA, CONFIG_APP_I2C_SCL);
    for (oled_id_t id = s_first; id <= s_last; id++) {
        status_one(id);
    }
    return 0;
}

static int auto_one(oled_id_t id)
{
    if (!oled_auto(id)) {
        fprintf(stderr, "oled: no auto mode handler for %s display\n", oled_name(id));
        return 1;
    }
    return 0;
}

static int cmd_auto(int argc, char **argv)
{
    return each(auto_one);
}

static int s_row;
static char s_text[OLED_COLS + 1];

static int print_one(oled_id_t id)
{
    if (s_row >= oled_rows(id)) {
        if (s_named) {
            fprintf(stderr, "oled: %s display rows are 0-%d\n", oled_name(id), oled_rows(id) - 1);
            return 1;
        }
        return 0;
    }
    oled_print(id, s_row, s_text);
    return 0;
}

static int cmd_print(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_print_args)) {
        return 1;
    }
    s_row = s_print_args.row->ival[0];
    if (s_row < 0 || s_row >= OLED_MAX_ROWS) {
        fprintf(stderr, "oled: row must be 0-%d\n", OLED_MAX_ROWS - 1);
        return 1;
    }
    s_text[0] = '\0';
    for (int i = 0; i < s_print_args.words->count; i++) {
        if (i) {
            strlcat(s_text, " ", sizeof(s_text));
        }
        strlcat(s_text, s_print_args.words->sval[i], sizeof(s_text));
    }
    return each(print_one);
}

static int clear_one(oled_id_t id)
{
    oled_clear(id);
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    return each(clear_one);
}

static int on_one(oled_id_t id)
{
    return oled_power(id, true) == ESP_OK ? 0 : 1;
}

static int off_one(oled_id_t id)
{
    return oled_power(id, false) == ESP_OK ? 0 : 1;
}

static int cmd_on(int argc, char **argv)
{
    return each(on_one);
}

static int cmd_off(int argc, char **argv)
{
    return each(off_one);
}

static int s_level;

static int contrast_one(oled_id_t id)
{
    return oled_contrast(id, s_level) == ESP_OK ? 0 : 1;
}

static int cmd_contrast(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_contrast_args)) {
        return 1;
    }
    s_level = s_contrast_args.level->ival[0];
    if (s_level < 0 || s_level > 255) {
        fprintf(stderr, "oled: contrast must be 0-255\n");
        return 1;
    }
    return each(contrast_one);
}

static int s_seconds;

static int idle_one(oled_id_t id)
{
    oled_idle(id, s_seconds);
    return 0;
}

static int cmd_idle(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_idle_args)) {
        return 1;
    }
    s_seconds = s_idle_args.seconds->ival[0];
    if (s_seconds < 0 || s_seconds > 86400) {
        fprintf(stderr, "oled: idle must be 0-86400\n");
        return 1;
    }
    return each(idle_one);
}

static const shell_subcmd_t s_subcmds[] = {
    { .name = "status", .help = "Displays, pins, mode, power and contrast", .func = cmd_status },
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
    s_first = 0;
    s_last = OLED_COUNT - 1;
    s_named = false;
    for (oled_id_t id = 0; argc > 1 && id < OLED_COUNT; id++) {
        if (!strcmp(argv[1], oled_name(id))) {
            s_first = s_last = id;
            s_named = true;
            argv[1] = argv[0];
            argc--;
            argv++;
            break;
        }
    }
    return shell_subcmd_run("oled [back|front]", s_subcmds, sizeof(s_subcmds) / sizeof(s_subcmds[0]), argc, argv);
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
        .help = "OLED displays, both unless back or front is given: status | auto | print | clear | on | off | contrast | idle",
        .hint = "[back|front] <command> [args]",
        .func = cmd_oled,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register oled");
    return ESP_OK;
}
