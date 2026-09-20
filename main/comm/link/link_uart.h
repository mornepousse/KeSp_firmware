/* Transport for the TRRS wired link (brick B2) — see link_uart.c. */
#pragma once
#include <stdbool.h>

/* Initializes LINK_5V_EN to LOW, UART1 (with the swap if the board declares
 * it) and starts the task that runs the handshake. */
void link_uart_start(void);

/* Is the 5 V closed on our side? Serves as a lock on sleep: a half that is
 * charging must stay awake to keep responding to probes. */
bool link_uart_active(void);
