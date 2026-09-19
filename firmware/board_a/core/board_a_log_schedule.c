#include "board_a_log_schedule.h"

void board_a_log_schedule_init(board_a_log_schedule_t *schedule,
                               uint32_t period_us,
                               uint64_t now_us)
{
  schedule->period_us = period_us;
  schedule->next_deadline_us = now_us + (uint64_t)period_us;
}

bool board_a_log_schedule_due(board_a_log_schedule_t *schedule,
                              uint64_t now_us)
{
  uint64_t missed;

  if (schedule->period_us == 0U) {
    return false;
  }

  if (now_us < schedule->next_deadline_us) {
    return false;
  }

  missed = (now_us - schedule->next_deadline_us) / (uint64_t)schedule->period_us;
  schedule->next_deadline_us += (missed + 1U) * (uint64_t)schedule->period_us;
  return true;
}
