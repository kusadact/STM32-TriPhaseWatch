#ifndef BOARD_B_RX_QUEUE_H
#define BOARD_B_RX_QUEUE_H

#include <stdint.h>

#include "bridge_config.h"

/*
 * Receive event queue shared by the USART interrupts (producer) and the main
 * loop (consumer).
 *
 * Each event keeps the byte together with the timestamp taken in the receive
 * interrupt. Dequeue-time timestamps are not usable: the main loop can be busy
 * long enough for several bytes with real arrival gaps to be popped in one
 * batch, and that gap information cannot be reconstructed afterwards.
 */
typedef struct
{
  uint32_t arrival_us;
  uint8_t byte;
} board_b_rx_event_t;

typedef struct
{
  board_b_rx_event_t slots[BOARD_B_RX_QUEUE_SLOTS];
  volatile uint16_t head;
  volatile uint16_t tail;
  volatile uint32_t dropped_bytes;
  volatile uint32_t continuity_lost;
  volatile uint32_t continuity_lost_at_us;
} board_b_rx_queue_t;

void board_b_rx_queue_init(board_b_rx_queue_t *queue);

/*
 * Producer side (USART interrupt). Returns 0 when the queue is full, in which
 * case the byte is dropped and the continuity-lost latch is raised so the
 * consumer can invalidate whatever frame was being assembled.
 */
int board_b_rx_queue_push(board_b_rx_queue_t *queue, uint8_t byte,
                          uint32_t arrival_us);

/* Consumer side. */
int board_b_rx_queue_pop(board_b_rx_queue_t *queue, board_b_rx_event_t *event);
int board_b_rx_queue_peek(const board_b_rx_queue_t *queue,
                          board_b_rx_event_t *event);
int board_b_rx_queue_empty(const board_b_rx_queue_t *queue);

/*
 * Continuity loss carries the arrival time of the first dropped byte so the
 * consumer can invalidate the frame that was being assembled at that moment,
 * not whatever happens to be the oldest queued event when it notices.
 */
int board_b_rx_queue_continuity_lost_at(const board_b_rx_queue_t *queue,
                                        uint32_t *at_us);
int board_b_rx_queue_take_continuity_lost(board_b_rx_queue_t *queue,
                                          uint32_t *at_us);

uint32_t board_b_rx_queue_dropped(const board_b_rx_queue_t *queue);

#endif /* BOARD_B_RX_QUEUE_H */
