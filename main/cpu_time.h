/* cpu_time.h
 * Utilities to estimate CPU time consumption per FreeRTOS task.
 * Requires FreeRTOS config options:
 *  - configUSE_TRACE_FACILITY == 1
 *  - configGENERATE_RUN_TIME_STATS == 1
 *  - portCONFIGURE_TIMER_FOR_RUN_TIME_STATS and
 *    portGET_RUN_TIME_COUNTER_VALUE implemented
 */
#ifndef CPU_TIME_H
#define CPU_TIME_H

#include <stddef.h>

typedef struct cpu_time_snapshot cpu_time_snapshot_t;

// Take a snapshot of all tasks' runtime counters. Caller must free the
// returned pointer with cpu_time_free_snapshot(). Returns NULL on failure.
cpu_time_snapshot_t *cpu_time_take_snapshot(void);

// Free a snapshot allocated by cpu_time_take_snapshot().
void cpu_time_free_snapshot(cpu_time_snapshot_t *snap);

// Compute a human-readable report of CPU usage between two snapshots.
// `out` is filled with a NUL-terminated string (up to len bytes).
// Returns 0 on success, negative on error.
int cpu_time_compute_stats(const cpu_time_snapshot_t *before,
                           const cpu_time_snapshot_t *after,
                           char *out, size_t len);

// Convenience: measure CPU usage over `period_ms` milliseconds and write a
// report into `out` (size `len`). This function delays the current task.
// Returns 0 on success.
int cpu_time_measure_period(unsigned period_ms, char *out, size_t len);

#endif // CPU_TIME_H
