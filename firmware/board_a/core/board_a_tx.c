#include <stddef.h>

#include "board_a_tx.h"

static bool state_is_active(board_a_tx_state_t state)
{
  return (state == BOARD_A_TX_SENDING) ||
         (state == BOARD_A_TX_WAIT_TC);
}

void board_a_tx_init(board_a_tx_t *tx)
{
  tx->data = NULL;
  tx->length = 0U;
  tx->next_index = 0U;
  tx->started_us = 0U;
  tx->budget_us = 0U;
  tx->state = BOARD_A_TX_IDLE;
}

bool board_a_tx_start(board_a_tx_t *tx,
                      const uint8_t *data,
                      uint16_t length,
                      uint32_t now_us,
                      uint32_t budget_us)
{
  if ((tx == NULL) || (data == NULL) || (length == 0U) ||
      (tx->state != BOARD_A_TX_IDLE)) {
    return false;
  }

  tx->data = data;
  tx->length = length;
  tx->next_index = 0U;
  tx->started_us = now_us;
  tx->budget_us = budget_us;
  tx->state = BOARD_A_TX_SENDING;
  return true;
}

bool board_a_tx_on_txe(board_a_tx_t *tx, uint8_t *byte)
{
  if ((tx == NULL) || (byte == NULL) ||
      (tx->state != BOARD_A_TX_SENDING)) {
    return false;
  }

  if (tx->next_index >= tx->length) {
    tx->state = BOARD_A_TX_WAIT_TC;
    return false;
  }

  *byte = tx->data[tx->next_index];
  tx->next_index++;
  if (tx->next_index >= tx->length) {
    tx->state = BOARD_A_TX_WAIT_TC;
  }
  return true;
}

bool board_a_tx_on_tc(board_a_tx_t *tx)
{
  if ((tx == NULL) || (tx->state != BOARD_A_TX_WAIT_TC)) {
    return false;
  }

  tx->state = BOARD_A_TX_COMPLETE;
  return true;
}

bool board_a_tx_poll_timeout(board_a_tx_t *tx, uint32_t now_us)
{
  uint32_t elapsed_us;

  if ((tx == NULL) || !state_is_active(tx->state)) {
    return false;
  }

  elapsed_us = (uint32_t)(now_us - tx->started_us);
  if (elapsed_us < tx->budget_us) {
    return false;
  }

  tx->state = BOARD_A_TX_TIMED_OUT;
  tx->data = NULL;
  return true;
}

bool board_a_tx_take_completion(board_a_tx_t *tx)
{
  if ((tx == NULL) || (tx->state != BOARD_A_TX_COMPLETE)) {
    return false;
  }

  board_a_tx_init(tx);
  return true;
}

bool board_a_tx_take_timeout(board_a_tx_t *tx)
{
  if ((tx == NULL) || (tx->state != BOARD_A_TX_TIMED_OUT)) {
    return false;
  }

  board_a_tx_init(tx);
  return true;
}

bool board_a_tx_abort(board_a_tx_t *tx)
{
  if ((tx == NULL) || !state_is_active(tx->state)) {
    return false;
  }

  board_a_tx_init(tx);
  return true;
}

bool board_a_tx_is_active(const board_a_tx_t *tx)
{
  return (tx != NULL) && state_is_active(tx->state);
}
