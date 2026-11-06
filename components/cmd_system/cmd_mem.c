#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "argtable3/argtable3.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "shell.h"
#include "cmd_system_priv.h"

static const char *TAG = "cmd_mem";

static struct {
    struct arg_lit *human;
    struct arg_end *end;
} s_free_args;

static const char *fmt_size(char *buf, size_t len, size_t bytes, bool human)
{
    if (human && bytes >= 1024 * 1024) {
        snprintf(buf, len, "%.1fM", bytes / (1024.0 * 1024.0));
    } else if (human && bytes >= 1024) {
        snprintf(buf, len, "%.1fK", bytes / 1024.0);
    } else {
        snprintf(buf, len, "%zu", bytes);
    }
    return buf;
}

static int cmd_free(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_free_args)) {
        return 1;
    }
    bool human = s_free_args.human->count > 0;

    static const struct {
        const char *name;
        uint32_t caps;
    } regions[] = {
        { "internal", MALLOC_CAP_INTERNAL },
        { "dma", MALLOC_CAP_DMA },
        { "spiram", MALLOC_CAP_SPIRAM },
    };

    printf("%-10s %10s %10s %10s %10s\n", "", "total", "free", "min-free", "largest");
    for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
        size_t total = heap_caps_get_total_size(regions[i].caps);
        if (total == 0) {
            continue;
        }
        multi_heap_info_t info;
        heap_caps_get_info(&info, regions[i].caps);
        char t[16], f[16], m[16], l[16];
        printf("%-10s %10s %10s %10s %10s\n", regions[i].name,
               fmt_size(t, sizeof(t), total, human),
               fmt_size(f, sizeof(f), info.total_free_bytes, human),
               fmt_size(m, sizeof(m), info.minimum_free_bytes, human),
               fmt_size(l, sizeof(l), info.largest_free_block, human));
    }
    return 0;
}

static const char *task_state_str(eTaskState s)
{
    switch (s) {
    case eRunning: return "run";
    case eReady: return "ready";
    case eBlocked: return "block";
    case eSuspended: return "susp";
    case eDeleted: return "del";
    default: return "?";
    }
}

static int by_task_number(const void *a, const void *b)
{
    const TaskStatus_t *x = a, *y = b;
    return (x->xTaskNumber > y->xTaskNumber) - (x->xTaskNumber < y->xTaskNumber);
}

static int cmd_ps(int argc, char **argv)
{
    UBaseType_t cap = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *tasks = malloc(cap * sizeof(*tasks));
    if (!tasks) {
        fprintf(stderr, "ps: out of memory\n");
        return 1;
    }
    configRUN_TIME_COUNTER_TYPE total_runtime = 0;
    UBaseType_t n = uxTaskGetSystemState(tasks, cap, &total_runtime);
    qsort(tasks, n, sizeof(*tasks), by_task_number);

    printf("%4s %-16s %-5s %4s %4s %6s", "id", "name", "state", "prio", "core", "stack");
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    printf(" %6s", "cpu%");
#endif
    printf("\n");

    for (UBaseType_t i = 0; i < n; i++) {
        const TaskStatus_t *t = &tasks[i];
        BaseType_t core = xTaskGetCoreID(t->xHandle);
        char core_str[8];
        snprintf(core_str, sizeof(core_str), core == tskNO_AFFINITY ? "-" : "%d", (int)core);

        printf("%4u %-16s %-5s %4u %4s %6u", (unsigned)t->xTaskNumber, t->pcTaskName,
               task_state_str(t->eCurrentState), (unsigned)t->uxCurrentPriority, core_str,
               (unsigned)t->usStackHighWaterMark);
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        printf(" %5.1f%%", total_runtime ? 100.0 * t->ulRunTimeCounter / total_runtime : 0.0);
#endif
        printf("\n");
    }
    free(tasks);
    return 0;
}

esp_err_t cmd_mem_register(void)
{
    s_free_args.human = arg_lit0("h", "human", "human-readable sizes");
    s_free_args.end = arg_end(2);

    const esp_console_cmd_t cmds[] = {
        { .command = "free", .help = "Heap usage per memory region", .func = cmd_free, .argtable = &s_free_args },
        { .command = "ps", .help = "FreeRTOS tasks; stack = min free bytes ever, cpu% = since boot", .func = cmd_ps },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "register %s", cmds[i].command);
    }
    return ESP_OK;
}
