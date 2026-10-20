/* rf-scope, half side — the glue: ring, drain task, UART0, sleep hook.
 *
 * Records reach the host over the console UART0 (the ESP-Prog), as KR frames
 * of command 0xBA (rf_trace.h), interleaved with the console text. Tracing is
 * off at boot; a KS_CMD_RF_TRACE request on UART0 RX starts it for a duration
 * and it goes off on its own (rf_trace_half.h: rf_trace_ctl_*). Details and
 * the UART0 RX choice: rf_trace_uart.c. */
#ifndef RF_TRACE_UART_H
#define RF_TRACE_UART_H

#include "rf_trace.h"

/* Non-zero while tracing: the session number. The ONLY thing the radio owner's
 * hot path reads when tracing is off. */
extern volatile uint32_t g_rf_trace_session;

/* Radio owner, after a transmission, lock held, tracing on: stamps t_us and
 * the emitter, copies the record into the ring, counts it for the periodic
 * record, wakes the drain task. Never blocks, never logs; a full ring counts
 * a drop. */
void rf_trace_uart_tx(const rf_trace_tx_t *r);

/* Once, from radio_owner_init AFTER the radio's own sleep hook (so the
 * SLEEPING record follows the radio's power-down: no TX can come after it):
 * drain task, UART0 RX interrupt, sleep hook. Tracing stays off. */
void rf_trace_uart_init(void);

#endif /* RF_TRACE_UART_H */
