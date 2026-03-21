/* cpu_time.c
 * Implementation of CPU time snapshot and stats helpers.
 * See cpu_time.h for API and notes.
 */

#include "cpu_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


struct cpu_time_snapshot {
    UBaseType_t task_count;
    TaskStatus_t *tasks;
    uint32_t total_run_time;
};

#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && (CONFIG_FREERTOS_USE_TRACE_FACILITY == 1)

cpu_time_snapshot_t *cpu_time_take_snapshot(void) {
    UBaseType_t count = uxTaskGetNumberOfTasks();
    if (count == 0) return NULL;

    TaskStatus_t *array = malloc(sizeof(TaskStatus_t) * count);
    if (!array) return NULL;

    uint32_t total = 0;
    UBaseType_t ret = uxTaskGetSystemState(array, count, &total);
    if (ret == 0) {
        free(array);
        return NULL;
    }

    cpu_time_snapshot_t *snap = malloc(sizeof(cpu_time_snapshot_t));
    if (!snap) {
        free(array);
        return NULL;
    }
    snap->task_count = ret;
    snap->tasks = array;
    snap->total_run_time = total;
    return snap;
}

void cpu_time_free_snapshot(cpu_time_snapshot_t *snap) {
    if (!snap) return;
    if (snap->tasks) free(snap->tasks);
    free(snap);
}

static const TaskStatus_t *find_task(const cpu_time_snapshot_t *snap, TaskHandle_t handle) {
    if (!snap || !snap->tasks) return NULL;
    for (UBaseType_t i = 0; i < snap->task_count; ++i) {
        if (snap->tasks[i].xHandle == handle) return &snap->tasks[i];
    }
    return NULL;
}

int cpu_time_compute_stats(const cpu_time_snapshot_t *before,
                           const cpu_time_snapshot_t *after,
                           char *out, size_t len) {
    if (!before || !after || !out || len == 0) return -1;
    uint64_t total_delta = 0;
    for (UBaseType_t i = 0; i < after->task_count; ++i) {
        const TaskStatus_t *a = &after->tasks[i];
        const TaskStatus_t *b = find_task(before, a->xHandle);
        uint64_t before_ticks = b ? (uint64_t)b->ulRunTimeCounter : 0;
        uint64_t after_ticks = (uint64_t)a->ulRunTimeCounter;
        uint64_t delta = (after_ticks >= before_ticks) ? (after_ticks - before_ticks) : 0;
        total_delta += delta;
    }

    int written = 0;
    int ret;
    ret = snprintf(out, len, "Task                 Ticks    %%\n");
    if (ret < 0) return -1;
    written += ret;
    size_t remain = (written < (int)len) ? (len - written) : 0;

    if (total_delta == 0) {
        ret = snprintf(out + written, remain, "No run-time activity detected in interval.\n");
        return (ret < 0) ? -1 : 0;
    }

    for (UBaseType_t i = 0; i < after->task_count; ++i) {
        const TaskStatus_t *a = &after->tasks[i];
        const TaskStatus_t *b = find_task(before, a->xHandle);
        uint64_t before_ticks = b ? (uint64_t)b->ulRunTimeCounter : 0;
        uint64_t after_ticks = (uint64_t)a->ulRunTimeCounter;
        uint64_t delta = (after_ticks >= before_ticks) ? (after_ticks - before_ticks) : 0;
        double pct = ((double)delta * 100.0) / (double)total_delta;
        ret = snprintf(out + written, remain, "%-20s %8" PRIu64 " %6.2f\n",
                       a->pcTaskName ? a->pcTaskName : "<no-name>", delta, pct);
        if (ret < 0) return -1;
        written += ret;
        remain = (written < (int)len) ? (len - written) : 0;
        if (remain == 0) break;
    }

    if (len > 0) out[len-1] = '\0';
    return 0;
}

int cpu_time_measure_period(unsigned period_ms, char *out, size_t len) {
    if (!out || len == 0) return -1;
    cpu_time_snapshot_t *s1 = cpu_time_take_snapshot();
    if (!s1) return -1;
    vTaskDelay(pdMS_TO_TICKS(period_ms));
    cpu_time_snapshot_t *s2 = cpu_time_take_snapshot();
    if (!s2) {
        cpu_time_free_snapshot(s1);
        return -1;
    }
    int r = cpu_time_compute_stats(s1, s2, out, len);
    cpu_time_free_snapshot(s1);
    cpu_time_free_snapshot(s2);
    return r;
}

#else // CONFIG_FREERTOS_USE_TRACE_FACILITY

cpu_time_snapshot_t *cpu_time_take_snapshot(void) {
    (void)0;
    return NULL;
}

void cpu_time_free_snapshot(cpu_time_snapshot_t *snap) {
    (void)snap;
}

int cpu_time_compute_stats(const cpu_time_snapshot_t *before,
                           const cpu_time_snapshot_t *after,
                           char *out, size_t len) {
    if (!out || len == 0) return -1;
    snprintf(out, len, "FreeRTOS run-time stats not enabled in sdkconfig.\n"
                     "Enable CONFIG_FREERTOS_USE_TRACE_FACILITY and\n"
                     "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS to use this feature.\n");
    return -2;
}

int cpu_time_measure_period(unsigned period_ms, char *out, size_t len) {
    (void)period_ms;
    return cpu_time_compute_stats(NULL, NULL, out, len);
}

#endif // CONFIG_FREERTOS_USE_TRACE_FACILITY
