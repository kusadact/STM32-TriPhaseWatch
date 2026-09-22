#ifndef BOARD_A_ALARM_OUTPUT_H
#define BOARD_A_ALARM_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "board_a_alarm.h"

void board_a_alarm_output_init(void);

void board_a_alarm_output_update(const board_a_alarm_state_t *state,
                                 bool run_active,
                                 uint32_t now_ms);

void board_a_alarm_output_force_off(void);

#endif /* BOARD_A_ALARM_OUTPUT_H */
