/*
 * hal_log.h — diagnostics output.
 *
 * printf-style. On the host this is stderr; on the MCU it is the console
 * USART. The core logs sparingly and never from a hot path: a log line per
 * packet at 100 Hz would itself be a bandwidth problem on a 9600 baud UART.
 */
#ifndef HAL_LOG_H
#define HAL_LOG_H

#define HAL_LOG_ERROR 0
#define HAL_LOG_WARN  1
#define HAL_LOG_INFO  2
#define HAL_LOG_DEBUG 3

#if defined(__GNUC__)
#define HAL_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define HAL_PRINTF(fmt_idx, arg_idx)
#endif

void hal_log(int level, const char *fmt, ...) HAL_PRINTF(2, 3);
void hal_log_set_level(int level);

#endif /* HAL_LOG_H */
