#ifndef BOARD_A_LOG_SCHEDULE_H
#define BOARD_A_LOG_SCHEDULE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Periodic debug-log deadline keeper.
 *
 * The platform clock is a 64-bit microsecond counter that never wraps, so the
 * deadline is stored in the same width: truncating either side to 32 bits made
 * the deadline fall behind the clock every ~71.6 minutes and produced bursts
 * of back-to-back log lines (see local review record 20260920-0245).
 *
 * A missed deadline is skipped, not replayed: when the caller has been busy
 * longer than one period, at most one line is printed and the next deadline is
 * moved past the current time. That keeps a stall from turning into a burst.
 */
typedef struct {
  uint64_t next_deadline_us;
  uint32_t period_us;
} board_a_log_schedule_t;

void board_a_log_schedule_init(board_a_log_schedule_t *schedule,
                               uint32_t period_us,
                               uint64_t now_us);

bool board_a_log_schedule_due(board_a_log_schedule_t *schedule,
                              uint64_t now_us);

#endif /* BOARD_A_LOG_SCHEDULE_H */
