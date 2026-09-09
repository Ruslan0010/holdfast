/*
 * hal_adc.h — sample source.
 *
 * The core never calls this; the application loop does, and hands the
 * samples to station_feed(). That keeps the core independent of how samples
 * arrive: polled from a file on the host, pushed from a DMA interrupt on the
 * MCU.
 */
#ifndef HAL_ADC_H
#define HAL_ADC_H

#include <stddef.h>
#include <stdint.h>

/* Host: source is a path to raw little-endian int32 samples; loop says
 * whether to wrap at end of file. MCU: source names the channel. */
int hal_adc_init(const char *source, int loop);

/* Copy up to max samples that are ready now. Returns the count, 0 when none
 * are available yet, negative at end of input (non-looping source). */
int hal_adc_read(int32_t *out, size_t max);

void hal_adc_close(void);

#endif /* HAL_ADC_H */
