#ifndef BOARD_A_TX_H
#define BOARD_A_TX_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  BOARD_A_TX_IDLE = 0,
  BOARD_A_TX_SENDING,
  BOARD_A_TX_WAIT_TC,
  BOARD_A_TX_COMPLETE,
  BOARD_A_TX_TIMED_OUT
} board_a_tx_state_t;

typedef struct {
  const uint8_t *data;
  uint16_t length;
  uint16_t next_index;
  uint32_t started_us;
  uint32_t budget_us;
  board_a_tx_state_t state;
} board_a_tx_t;

void board_a_tx_init(board_a_tx_t *tx);
bool board_a_tx_start(board_a_tx_t *tx,
                      const uint8_t *data,
                      uint16_t length,
                      uint32_t now_us,
                      uint32_t budget_us);
bool board_a_tx_on_txe(board_a_tx_t *tx, uint8_t *byte);
bool board_a_tx_on_tc(board_a_tx_t *tx);
bool board_a_tx_poll_timeout(board_a_tx_t *tx, uint32_t now_us);
bool board_a_tx_take_completion(board_a_tx_t *tx);
bool board_a_tx_take_timeout(board_a_tx_t *tx);
bool board_a_tx_abort(board_a_tx_t *tx);
bool board_a_tx_is_active(const board_a_tx_t *tx);

#endif /* BOARD_A_TX_H */
