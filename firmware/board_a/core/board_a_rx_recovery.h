#ifndef BOARD_A_RX_RECOVERY_H
#define BOARD_A_RX_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t last_seen_us;
  uint32_t t35_us;
  bool discarding;
} board_a_rx_recovery_t;

void board_a_rx_recovery_init(board_a_rx_recovery_t *recovery, uint32_t t35_us);
void board_a_rx_recovery_begin(board_a_rx_recovery_t *recovery,
                               uint32_t fault_us);
bool board_a_rx_recovery_accept(board_a_rx_recovery_t *recovery,
                                uint32_t event_us);
bool board_a_rx_recovery_can_poll(board_a_rx_recovery_t *recovery,
                                  uint32_t now_us);
bool board_a_rx_recovery_is_discarding(const board_a_rx_recovery_t *recovery);

#endif /* BOARD_A_RX_RECOVERY_H */
