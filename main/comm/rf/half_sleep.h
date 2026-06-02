#ifndef HALF_SLEEP_H
#define HALF_SLEEP_H
/* Enter the SLEEP power tier: quiesce all wake sources, (Task 6: light-sleep
 * until a key GPIO wakes the CPU), then restore. Called from the half scan
 * task's idle loop when half_power_next() returns HALF_POWER_SLEEP. Blocks. */
void half_sleep_enter(void);
#endif /* HALF_SLEEP_H */
