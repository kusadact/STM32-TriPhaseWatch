#include "bridge_dispatch.h"

/* Wrap-safe "a happened before b" for the 32-bit microsecond clock. */
static int time_before(uint32_t a_us, uint32_t b_us)
{
  return (int32_t)(a_us - b_us) < 0;
}

static uint32_t later_of(uint32_t a_us, uint32_t b_us)
{
  return time_before(a_us, b_us) ? b_us : a_us;
}

/*
 * A continuity loss belongs between the queued events: everything that arrived
 * before the dropped byte predates the hole, everything after it must be
 * treated as discontinuous. Apply the loss only once the event stream reaches
 * its timestamp, so a batch that was queued before the drain cannot invalidate
 * frames that arrived earlier.
 */
static int continuity_loss_due(const board_b_rx_queue_t *queue,
                               const board_b_rx_event_t *head_event,
                               int have_event,
                               uint32_t *at_us)
{
  if (board_b_rx_queue_continuity_lost_at(queue, at_us) == 0)
  {
    return 0;
  }

  if (have_event && time_before(head_event->arrival_us, *at_us))
  {
    return 0;
  }

  return 1;
}

void bridge_dispatch_process(bridge_core_t *core,
                             board_b_rx_queue_t *host_queue,
                             board_b_rx_queue_t *rs485_queue,
                             uint32_t now_us)
{
  board_b_rx_event_t host_event;
  board_b_rx_event_t rs485_event;
  uint32_t logical_us = now_us;
  int have_host = board_b_rx_queue_peek(host_queue, &host_event);
  int have_rs485 = board_b_rx_queue_peek(rs485_queue, &rs485_event);

  for (;;)
  {
    int take_host;
    uint32_t lost_at_us;

    if (continuity_loss_due(host_queue, &host_event, have_host, &lost_at_us) != 0)
    {
      (void)board_b_rx_queue_take_continuity_lost(host_queue, &lost_at_us);
      logical_us = later_of(logical_us, lost_at_us);
      bridge_core_tick(core, lost_at_us);
      bridge_core_on_host_continuity_lost(core, lost_at_us);
      have_host = board_b_rx_queue_peek(host_queue, &host_event);
      continue;
    }

    if (continuity_loss_due(rs485_queue, &rs485_event, have_rs485,
                            &lost_at_us) != 0)
    {
      (void)board_b_rx_queue_take_continuity_lost(rs485_queue, &lost_at_us);
      logical_us = later_of(logical_us, lost_at_us);
      bridge_core_tick(core, lost_at_us);
      bridge_core_on_rs485_continuity_lost(core, lost_at_us);
      have_rs485 = board_b_rx_queue_peek(rs485_queue, &rs485_event);
      continue;
    }

    if ((have_host == 0) && (have_rs485 == 0))
    {
      break;
    }

    if ((have_host != 0) && (have_rs485 != 0))
    {
      /* Ties keep the host byte first, matching the byte order on the link. */
      take_host = !time_before(rs485_event.arrival_us, host_event.arrival_us);
    }
    else
    {
      take_host = have_host != 0;
    }

    if (take_host != 0)
    {
      (void)board_b_rx_queue_pop(host_queue, &host_event);
      logical_us = later_of(logical_us, host_event.arrival_us);
      bridge_core_tick(core, host_event.arrival_us);
      bridge_core_on_host_byte(core, host_event.byte, host_event.arrival_us);
      have_host = board_b_rx_queue_peek(host_queue, &host_event);
    }
    else
    {
      (void)board_b_rx_queue_pop(rs485_queue, &rs485_event);
      logical_us = later_of(logical_us, rs485_event.arrival_us);
      bridge_core_tick(core, rs485_event.arrival_us);
      bridge_core_on_rs485_byte(core, rs485_event.byte, rs485_event.arrival_us);
      have_rs485 = board_b_rx_queue_peek(rs485_queue, &rs485_event);
    }
  }

  /* Only ever move forward: a caller-supplied now_us may be older than an
     event that was drained above. */
  bridge_core_tick(core, later_of(logical_us, now_us));
}
