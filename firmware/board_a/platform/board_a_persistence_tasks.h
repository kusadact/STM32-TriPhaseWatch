#ifndef BOARD_A_PERSISTENCE_TASKS_H
#define BOARD_A_PERSISTENCE_TASKS_H

#include "board_a_runtime.h"

void board_a_persistence_startup(board_a_runtime_t *runtime);

void board_a_config_task(void *argument);
void board_a_storage_task(void *argument);

#endif /* BOARD_A_PERSISTENCE_TASKS_H */
