#ifndef BOARD_A_MONOTONIC_H
#define BOARD_A_MONOTONIC_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t last_raw_us;
  uint64_t accumulated_us;
  bool initialized;
} board_a_monotonic_t;

void board_a_monotonic_init(board_a_monotonic_t *clock, uint32_t raw_us);
uint64_t board_a_monotonic_update(board_a_monotonic_t *clock, uint32_t raw_us);
uint64_t board_a_monotonic_value(const board_a_monotonic_t *clock);

#endif /* BOARD_A_MONOTONIC_H */
