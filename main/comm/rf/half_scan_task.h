#ifndef HALF_SCAN_TASK_H
#define HALF_SCAN_TASK_H

/* Start the half scan task (keyboard_button init + NRF PTX init + heartbeat timer).
 * Called from app_main() in the HALF role branch. */
void half_scan_task_start(void);

/* Stop the heartbeat timer + delete the keyboard_button scan (for light-sleep).
 * half_scan_restart_after_wake() recreates them and resets the activity clock so
 * the half resumes in ACTIVE; the recreated scan immediately re-detects a held key. */
void half_scan_stop_for_sleep(void);
void half_scan_restart_after_wake(void);

#ifdef TEST_HOST
/* In test builds: provide a minimal keyboard_btn_data_t stub (matches the real
 * component struct members used by half_diff_emit). */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    uint8_t output_index;   /* col */
    uint8_t input_index;    /* row */
} keyboard_btn_data_t;

/* Expose half_diff_emit for host-side unit tests. */
void half_diff_emit(
    uint8_t *bitmap,
    const keyboard_btn_data_t *pressed,  uint32_t press_cnt,
    const keyboard_btn_data_t *released, uint32_t release_cnt,
    void (*emit_cb)(uint8_t row, uint8_t col, bool pressed, void *ctx),
    void *ctx
);
#endif /* TEST_HOST */

#endif /* HALF_SCAN_TASK_H */
