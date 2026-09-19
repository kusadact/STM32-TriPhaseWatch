#ifndef BOARD_B_IO_H
#define BOARD_B_IO_H

#include <stdint.h>

#include "board_b_rx_queue.h"

typedef struct
{
  uint32_t host_rx_overruns;
  uint32_t rs485_rx_overruns;
  uint32_t host_uart_errors;
  uint32_t rs485_uart_errors;
} board_b_io_stats_t;

extern volatile board_b_io_stats_t g_board_b_io_stats;

/*
 * Receive queues: the USART interrupts are the single producer of each queue,
 * the main loop is the single consumer.
 */
extern board_b_rx_queue_t g_board_b_host_rx_queue;
extern board_b_rx_queue_t g_board_b_rs485_rx_queue;

void board_b_io_init(void);

/*
 * Free-running 1 MHz microsecond counter (TIM2 CNT). It wraps every
 * 4294.967296 s; every consumer only uses unsigned differences.
 */
uint32_t board_b_micros(void);

void board_b_host_send(const uint8_t *data, uint16_t length);
void board_b_rs485_send(const uint8_t *data, uint16_t length);

#endif /* BOARD_B_IO_H */
