#include "board_a_rx_recovery.h"

static bool silent_gap_elapsed(const board_a_rx_recovery_t *recovery,
                               uint32_t now_us)
{
  int32_t elapsed_us = (int32_t)(now_us - recovery->last_seen_us);

  return elapsed_us >= (int32_t)recovery->t35_us;
}

void board_a_rx_recovery_init(board_a_rx_recovery_t *recovery, uint32_t t35_us)
{
  recovery->last_seen_us = 0U;
  recovery->t35_us = t35_us;
  recovery->discarding = false;
}

void board_a_rx_recovery_begin(board_a_rx_recovery_t *recovery,
                               uint32_t fault_us)
{
  recovery->last_seen_us = fault_us;
  recovery->discarding = true;
}

bool board_a_rx_recovery_accept(board_a_rx_recovery_t *recovery,
                                uint32_t event_us)
{
  bool gap_elapsed;

  if (!recovery->discarding) {
    return true;
  }

  gap_elapsed = silent_gap_elapsed(recovery, event_us);
  recovery->last_seen_us = event_us;
  if (gap_elapsed) {
    recovery->discarding = false;
    return true;
  }
  return false;
}

bool board_a_rx_recovery_can_poll(board_a_rx_recovery_t *recovery,
                                  uint32_t now_us)
{
  if (!recovery->discarding) {
    return true;
  }

  if (silent_gap_elapsed(recovery, now_us)) {
    recovery->discarding = false;
    return true;
  }
  return false;
}

bool board_a_rx_recovery_is_discarding(const board_a_rx_recovery_t *recovery)
{
  return recovery->discarding;
}
