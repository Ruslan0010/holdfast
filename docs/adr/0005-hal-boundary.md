# 0005 A HAL, and time as a parameter

## Context

The same protocol core must run on a Linux host for development and
integration testing, under Zephyr on an STM32, and against a mock in unit
tests.

## Decision

Four small headers under `firmware/include/hal/` (`hal_net`, `hal_time`,
`hal_log`, `hal_adc`) with a separate implementation per target. The
protocol core calls only `hal_net_send` and `hal_log`; it takes the current
time as an argument to every function rather than reading a clock, and it
never reads the ADC, the application does.

## Consequences

- `station.c` is tested byte-for-byte with a scripted clock and a
  recording network, with no sockets or sleeps in the test.
- Porting is implementing four headers. The core is not touched.
- The application loop (pacing, polling, wiring the ADC to `station_feed`)
  is per target and is small.
