#ifndef BUSCOMM_DEBUG_UART_H
#define BUSCOMM_DEBUG_UART_H

#include <stdint.h>

void debug_uart_init(uint32_t baudrate);
void debug_uart_putc(char c);
void debug_uart_puts(const char *text);

#endif /* BUSCOMM_DEBUG_UART_H */
