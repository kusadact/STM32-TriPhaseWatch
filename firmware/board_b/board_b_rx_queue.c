#include "board_b_rx_queue.h"

/*
 * Single-producer/single-consumer ring buffer.
 *
 * Producer: one USART receive interrupt per queue. It writes the event slot,
 * then publishes the new head. Consumer: the main loop reads the head first
 * and only then reads the slot. On Cortex-M both sides are interrupt-driven
 * but never preempt each other on the same queue, and the volatile accesses
 * are ordered by the compiler; the explicit barrier keeps that ordering if
 * this file is ever compiled with a more aggressive optimizer.
 */
static void queue_barrier(void)
{
#if defined(__GNUC__)
  __asm volatile("" ::: "memory");
#endif
}

void board_b_rx_queue_init(board_b_rx_queue_t *queue)
{
  uint16_t index;

  for (index = 0U; index < BOARD_B_RX_QUEUE_SLOTS; index++)
  {
    queue->slots[index].arrival_us = 0U;
    queue->slots[index].byte = 0U;
  }
  queue->head = 0U;
  queue->tail = 0U;
  queue->dropped_bytes = 0U;
  queue->continuity_lost = 0U;
  queue->continuity_lost_at_us = 0U;
}

int board_b_rx_queue_push(board_b_rx_queue_t *queue, uint8_t byte,
                          uint32_t arrival_us)
{
  uint16_t head = queue->head;
  uint16_t next = (uint16_t)((head + 1U) % BOARD_B_RX_QUEUE_SLOTS);

  if (next == queue->tail)
  {
    queue->dropped_bytes++;
    if (queue->continuity_lost == 0U)
    {
      queue->continuity_lost = 1U;
      queue->continuity_lost_at_us = arrival_us;
    }
    return 0;
  }

  queue->slots[head].arrival_us = arrival_us;
  queue->slots[head].byte = byte;
  queue_barrier();
  queue->head = next;
  return 1;
}

int board_b_rx_queue_pop(board_b_rx_queue_t *queue, board_b_rx_event_t *event)
{
  uint16_t tail = queue->tail;

  if (tail == queue->head)
  {
    return 0;
  }

  *event = queue->slots[tail];
  queue_barrier();
  queue->tail = (uint16_t)((tail + 1U) % BOARD_B_RX_QUEUE_SLOTS);
  return 1;
}

int board_b_rx_queue_peek(const board_b_rx_queue_t *queue,
                          board_b_rx_event_t *event)
{
  uint16_t tail = queue->tail;

  if (tail == queue->head)
  {
    return 0;
  }

  *event = queue->slots[tail];
  return 1;
}

int board_b_rx_queue_empty(const board_b_rx_queue_t *queue)
{
  return queue->tail == queue->head;
}

int board_b_rx_queue_continuity_lost_at(const board_b_rx_queue_t *queue,
                                        uint32_t *at_us)
{
  if (queue->continuity_lost == 0U)
  {
    return 0;
  }

  *at_us = queue->continuity_lost_at_us;
  return 1;
}

int board_b_rx_queue_take_continuity_lost(board_b_rx_queue_t *queue,
                                          uint32_t *at_us)
{
  if (board_b_rx_queue_continuity_lost_at(queue, at_us) == 0)
  {
    return 0;
  }

  queue->continuity_lost = 0U;
  return 1;
}

uint32_t board_b_rx_queue_dropped(const board_b_rx_queue_t *queue)
{
  return queue->dropped_bytes;
}
