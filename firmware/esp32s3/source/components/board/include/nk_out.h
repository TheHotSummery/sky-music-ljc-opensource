#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void nk_out_set_tap(void (*fn)(const char *data, size_t n));
void nk_out_set_uart_echo(int on);
void nk_write(const char *data, size_t n);
int nk_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
