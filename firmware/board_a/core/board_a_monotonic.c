#include "board_a_monotonic.h"

void board_a_monotonic_init(board_a_monotonic_t *clock, uint32_t raw_us)
{
  clock->last_raw_us = raw_us;
  clock->accumulated_us = 0U;
  clock->initialized = true;
}

uint64_t board_a_monotonic_update(board_a_monotonic_t *clock, uint32_t raw_us)
{
  uint32_t elapsed;

  if (!clock->initialized) {
    board_a_monotonic_init(clock, raw_us);
    return clock->accumulated_us;
  }

  elapsed = (uint32_t)(raw_us - clock->last_raw_us);
  clock->accumulated_us += elapsed;
  clock->last_raw_us = raw_us;
  return clock->accumulated_us;
}

uint64_t board_a_monotonic_value(const board_a_monotonic_t *clock)
{
  return clock->accumulated_us;
}
