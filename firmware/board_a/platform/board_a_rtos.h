#ifndef BOARD_A_RTOS_H
#define BOARD_A_RTOS_H

#include <stdint.h>

int board_a_rtos_run(void);
uint32_t board_a_rtos_now_ms(void);
void board_a_rtos_note_config_stack(uint32_t min_words);
void board_a_rtos_note_storage_stack(uint32_t min_words);

#endif /* BOARD_A_RTOS_H */
