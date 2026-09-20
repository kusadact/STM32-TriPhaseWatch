#ifndef BOARD_A_SLAVE_H
#define BOARD_A_SLAVE_H

#include <stddef.h>
#include <stdint.h>

#include "board_a_model.h"
#include "modbus_rtu_rx.h"

typedef struct {
  board_a_model_t model;
  modbus_rtu_rx_t receiver;
  modbus_rtu_server_t server;
  /*
   * Monotonic microsecond instant of the frame being processed. The owner
   * refreshes it before polling; the model binds SET_TIME/ARM_START and
   * evaluates CURRENT_UTC_SECONDS against it.
   */
  uint64_t frame_now_us;
} board_a_slave_t;

void board_a_slave_init(board_a_slave_t *slave, uint32_t session_id);

void board_a_slave_push_byte(board_a_slave_t *slave,
                             uint8_t byte,
                             uint32_t now_us);

/*
 * Polls for a completed RTU frame and processes at most one frame.
 * Returns the response length, or zero when no response is required.
 */
size_t board_a_slave_poll(board_a_slave_t *slave,
                          uint32_t now_us,
                          uint8_t *response,
                          size_t response_capacity);

void board_a_slave_tick(board_a_slave_t *slave, uint64_t now_us);

#endif /* BOARD_A_SLAVE_H */
