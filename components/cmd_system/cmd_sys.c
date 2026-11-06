#include <stdio.h>
#include <inttypes.h>
#include "argtable3/argtable3.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "shell.h"
#include "cmd_system_priv.h"

static const char *TAG = "cmd_sys";

static void print_mac(const char *label, esp_mac_type_t type)
{
    uint8_t mac[8];
    if (esp_read_mac(mac, type) != ESP_OK) {
        return;
    }
    printf("mac %-6s ", label);
    size_t len = esp_mac_addr_len_get(type);
    for (size_t i = 0; i < len; i++) {
        printf(i ? ":%02x" : "%02x", mac[i]);
    }
    printf("\n");
}

static int cmd_sysinfo(int argc, char **argv)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    printf("target     %s\n", CONFIG_IDF_TARGET);
    printf("revision   v%d.%d\n", chip.revision / 100, chip.revision % 100);
    printf("cores      %d\n", chip.cores);
    printf("features  %s%s%s%s%s\n",
           chip.features & CHIP_FEATURE_WIFI_BGN ? " wifi" : "",
           chip.features & CHIP_FEATURE_BLE ? " ble" : "",
           chip.features & CHIP_FEATURE_BT ? " bt" : "",
           chip.features & CHIP_FEATURE_IEEE802154 ? " 802.15.4" : "",
           chip.features & CHIP_FEATURE_EMB_PSRAM ? " psram" : "");
    printf("flash      %" PRIu32 " MB %s\n", flash_size >> 20,
           chip.features & CHIP_FEATURE_EMB_FLASH ? "embedded" : "external");
    printf("cpu        %d MHz\n", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);

    print_mac("base", ESP_MAC_BASE);
#if SOC_WIFI_SUPPORTED
    print_mac("sta", ESP_MAC_WIFI_STA);
    print_mac("ap", ESP_MAC_WIFI_SOFTAP);
#endif
#if SOC_BT_SUPPORTED
    print_mac("bt", ESP_MAC_BT);
#endif
#if SOC_IEEE802154_SUPPORTED
    print_mac("15.4", ESP_MAC_IEEE802154);
#endif
    return 0;
}

static int cmd_version(int argc, char **argv)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char sha[17];
    esp_app_get_elf_sha256(sha, sizeof(sha));

    printf("app        %s %s\n", app->project_name, app->version);
    printf("idf        %s\n", esp_get_idf_version());
    printf("built      %s %s\n", app->date, app->time);
    printf("elf sha    %s\n", sha);
    return 0;
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    static const char *names[] = {
        [ESP_RST_UNKNOWN] = "unknown",
        [ESP_RST_POWERON] = "power-on",
        [ESP_RST_EXT] = "external pin",
        [ESP_RST_SW] = "software",
        [ESP_RST_PANIC] = "panic",
        [ESP_RST_INT_WDT] = "interrupt watchdog",
        [ESP_RST_TASK_WDT] = "task watchdog",
        [ESP_RST_WDT] = "other watchdog",
        [ESP_RST_DEEPSLEEP] = "deep sleep wake",
        [ESP_RST_BROWNOUT] = "brownout",
        [ESP_RST_SDIO] = "sdio",
        [ESP_RST_USB] = "usb",
        [ESP_RST_JTAG] = "jtag",
        [ESP_RST_EFUSE] = "efuse error",
        [ESP_RST_PWR_GLITCH] = "power glitch",
        [ESP_RST_CPU_LOCKUP] = "cpu lockup",
    };
    if ((size_t)r < sizeof(names) / sizeof(names[0]) && names[r]) {
        return names[r];
    }
    return "unknown";
}

static int cmd_uptime(int argc, char **argv)
{
    int64_t s = esp_timer_get_time() / 1000000;
    printf("up %lld days, %02lld:%02lld:%02lld, reset: %s\n",
           s / 86400, s / 3600 % 24, s / 60 % 60, s % 60,
           reset_reason_str(esp_reset_reason()));
    return 0;
}

static struct {
    struct arg_int *delay;
    struct arg_end *end;
} s_reboot_args;

static int cmd_reboot(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_reboot_args)) {
        return 1;
    }
    int delay_ms = s_reboot_args.delay->count ? s_reboot_args.delay->ival[0] : 100;
    if (delay_ms < 0) {
        fprintf(stderr, "reboot: delay must be >= 0\n");
        return 1;
    }
    printf("rebooting in %d ms\n", delay_ms);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

esp_err_t cmd_sys_register(void)
{
    s_reboot_args.delay = arg_int0("d", "delay", "<ms>", "delay before reboot, default 100");
    s_reboot_args.end = arg_end(2);

    const esp_console_cmd_t cmds[] = {
        { .command = "sysinfo", .help = "Chip, flash and MAC address info", .func = cmd_sysinfo },
        { .command = "version", .help = "Firmware and ESP-IDF version", .func = cmd_version },
        { .command = "uptime", .help = "Time since boot and last reset reason", .func = cmd_uptime },
        { .command = "reboot", .help = "Restart the chip", .func = cmd_reboot, .argtable = &s_reboot_args },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "register %s", cmds[i].command);
    }
    return ESP_OK;
}
