#include "nk_out.h"

#include <stdarg.h>
#include <stdio.h>

static void (*s_tap)(const char *data, size_t n);
static int s_uart_echo = 1;

void nk_out_set_tap(void (*fn)(const char *data, size_t n))
{
    s_tap = fn;
}

void nk_out_set_uart_echo(int on)
{
    s_uart_echo = on ? 1 : 0;
}

void nk_write(const char *data, size_t n)
{
    if (!data || !n) {
        return;
    }
    if (s_uart_echo) {
        fwrite(data, 1, n, stdout);
    }
    if (s_tap) {
        s_tap(data, n);
    }
}

int nk_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return n;
    }
    size_t w = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    nk_write(buf, w);
    return n;
}
