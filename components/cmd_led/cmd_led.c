#include <stdio.h>
#include "argtable3/argtable3.h"
#include "esp_check.h"
#include "esp_console.h"
#include "led.h"
#include "shell.h"
#include "cmd_led.h"

#define MIN_PERIOD_MS 40

static const char *TAG = "cmd_led";

static struct {
    struct arg_str *color;
    struct arg_end *end;
} s_color_args;

static struct {
    struct arg_str *color;
    struct arg_int *period;
    struct arg_end *end;
} s_blink_args, s_breathe_args;

static struct {
    struct arg_int *period;
    struct arg_end *end;
} s_rainbow_args;

static struct {
    struct arg_int *percent;
    struct arg_end *end;
} s_brightness_args;

static bool parse_color(const char *s, led_rgb_t *out)
{
    if (led_color_parse(s, out)) {
        return true;
    }
    fprintf(stderr, "led: unknown color '%s', use a preset name, #rrggbb or r,g,b (see 'led presets')\n", s);
    return false;
}

static bool parse_period(struct arg_int *arg, uint32_t def, uint32_t *out)
{
    *out = arg->count ? (uint32_t)arg->ival[0] : def;
    if (arg->count && arg->ival[0] < MIN_PERIOD_MS) {
        fprintf(stderr, "led: period must be >= %d ms\n", MIN_PERIOD_MS);
        return false;
    }
    return true;
}

static int cmd_status(int argc, char **argv)
{
    led_state_t st;
    led_get(&st);
    const char *name = led_color_name(st.color);
    printf("mode       %s\n", st.auto_mode ? "auto (status pattern)" : "manual");
    printf("effect     %s\n", led_effect_name(st.effect));
    if (st.effect != LED_OFF && st.effect != LED_RAINBOW) {
        printf("color      #%02x%02x%02x%s%s%s\n", st.color.r, st.color.g, st.color.b,
               name ? " (" : "", name ? name : "", name ? ")" : "");
    }
    if (st.effect != LED_OFF && st.effect != LED_SOLID) {
        printf("period     %lu ms\n", (unsigned long)st.period_ms);
    }
    printf("brightness %u%%\n", st.brightness);
    return 0;
}

static int cmd_auto(int argc, char **argv)
{
    if (led_auto()) {
        return 0;
    }
    fprintf(stderr, "led: no auto mode handler installed\n");
    return 1;
}

static int cmd_off(int argc, char **argv)
{
    led_set(LED_OFF, (led_rgb_t){ 0 }, 0);
    return 0;
}

static int cmd_color(int argc, char **argv)
{
    led_rgb_t c;
    if (!shell_args_ok(argc, argv, &s_color_args) || !parse_color(s_color_args.color->sval[0], &c)) {
        return 1;
    }
    led_set(LED_SOLID, c, 0);
    return 0;
}

static int set_periodic(int argc, char **argv, void *args, struct arg_str *color, struct arg_int *period,
                        led_effect_t effect, uint32_t def_period)
{
    led_rgb_t c;
    uint32_t p;
    if (!shell_args_ok(argc, argv, args) || !parse_color(color->sval[0], &c) || !parse_period(period, def_period, &p)) {
        return 1;
    }
    led_set(effect, c, p);
    return 0;
}

static int cmd_blink(int argc, char **argv)
{
    return set_periodic(argc, argv, &s_blink_args, s_blink_args.color, s_blink_args.period, LED_BLINK, 500);
}

static int cmd_breathe(int argc, char **argv)
{
    return set_periodic(argc, argv, &s_breathe_args, s_breathe_args.color, s_breathe_args.period, LED_BREATHE, 3000);
}

static int cmd_rainbow(int argc, char **argv)
{
    uint32_t p;
    if (!shell_args_ok(argc, argv, &s_rainbow_args) || !parse_period(s_rainbow_args.period, 5000, &p)) {
        return 1;
    }
    led_set(LED_RAINBOW, (led_rgb_t){ 0 }, p);
    return 0;
}

static int cmd_brightness(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_brightness_args)) {
        return 1;
    }
    int pct = s_brightness_args.percent->ival[0];
    if (pct < 0 || pct > 100) {
        fprintf(stderr, "led: brightness must be 0-100\n");
        return 1;
    }
    led_set_brightness(pct);
    return 0;
}

static int cmd_presets(int argc, char **argv)
{
    size_t n;
    const led_preset_t *presets = led_presets(&n);
    printf("colors:\n");
    for (size_t i = 0; i < n; i++) {
        printf("  %-8s #%02x%02x%02x\n", presets[i].name, presets[i].color.r, presets[i].color.g, presets[i].color.b);
    }
    printf("  or #rrggbb, or r,g,b (0-255)\n");
    printf("effects: off, solid (led color), blink, breathe, rainbow\n");
    return 0;
}

static const shell_subcmd_t s_subcmds[] = {
    { .name = "status", .help = "Current mode, effect, color and brightness", .func = cmd_status },
    { .name = "auto", .help = "Status pattern (default)", .func = cmd_auto },
    { .name = "off", .help = "Turn off", .func = cmd_off },
    { .name = "color", .help = "Solid color", .func = cmd_color, .argtable = &s_color_args },
    { .name = "blink", .help = "Blink a color, default period 500 ms", .func = cmd_blink, .argtable = &s_blink_args },
    { .name = "breathe", .help = "Fade a color in and out, default period 3000 ms", .func = cmd_breathe, .argtable = &s_breathe_args },
    { .name = "rainbow", .help = "Cycle through all hues, default period 5000 ms", .func = cmd_rainbow, .argtable = &s_rainbow_args },
    { .name = "brightness", .help = "Global brightness, default 20", .func = cmd_brightness, .argtable = &s_brightness_args },
    { .name = "presets", .help = "List color names and effects", .func = cmd_presets },
};

static int cmd_led(int argc, char **argv)
{
    return shell_subcmd_run("led", s_subcmds, sizeof(s_subcmds) / sizeof(s_subcmds[0]), argc, argv);
}

esp_err_t cmd_led_register(void)
{
    s_color_args.color = arg_str1(NULL, NULL, "<color>", "preset, #rrggbb or r,g,b");
    s_color_args.end = arg_end(2);
    s_blink_args.color = arg_str1(NULL, NULL, "<color>", "preset, #rrggbb or r,g,b");
    s_blink_args.period = arg_int0("p", "period", "<ms>", "full on+off cycle");
    s_blink_args.end = arg_end(2);
    s_breathe_args.color = arg_str1(NULL, NULL, "<color>", "preset, #rrggbb or r,g,b");
    s_breathe_args.period = arg_int0("p", "period", "<ms>", "full fade in+out cycle");
    s_breathe_args.end = arg_end(2);
    s_rainbow_args.period = arg_int0("p", "period", "<ms>", "full hue cycle");
    s_rainbow_args.end = arg_end(2);
    s_brightness_args.percent = arg_int1(NULL, NULL, "<0-100>", "percent");
    s_brightness_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "led",
        .help = "RGB LED: status | auto | off | color | blink | breathe | rainbow | brightness | presets",
        .hint = "<command> [args]",
        .func = cmd_led,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register led");
    return ESP_OK;
}
