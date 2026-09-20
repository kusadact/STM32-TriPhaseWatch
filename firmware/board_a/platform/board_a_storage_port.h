#ifndef BOARD_A_STORAGE_PORT_H
#define BOARD_A_STORAGE_PORT_H

#include "board_a_storage_engine.h"

const board_a_storage_io_ops_t *board_a_storage_port_ops(void);
void board_a_storage_port_reset(void);

#endif /* BOARD_A_STORAGE_PORT_H */
