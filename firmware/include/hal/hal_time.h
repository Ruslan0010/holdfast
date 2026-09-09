/*
 * hal_time.h — clocks.
 *
 * Two clocks, never confused with each other:
 *   monotonic  for intervals: heartbeats, rate limiting, timeouts. Never
 *              jumps, meaningless as a date.
 *   wall       for timestamps: Unix epoch nanoseconds. May step when the
 *              reference (NTP, GPS) corrects it.
 *
 * Clock quality tells the server how much to trust the wall clock. On the
 * host it follows whether the system clock is synchronised; on the MCU it
 * follows the PPS discipline state.
 */
#ifndef HAL_TIME_H
#define HAL_TIME_H

#include <stdint.h>

uint64_t hal_time_mono_ns(void);
uint64_t hal_time_wall_ns(void);

/* One of PKT_CLOCK_FREE, PKT_CLOCK_HOLDOVER, PKT_CLOCK_LOCKED (packet.h). */
uint8_t hal_time_clock_quality(void);

#endif /* HAL_TIME_H */
