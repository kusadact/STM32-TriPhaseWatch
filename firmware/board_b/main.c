#include "board_b_io.h"
#include "bridge_core.h"
#include "bridge_dispatch.h"

bridge_core_t g_board_b_bridge;

int main(void)
{
  const uint8_t *tx_data;
  uint16_t tx_length;

  board_b_io_init();
  bridge_core_init(&g_board_b_bridge);

  while (1)
  {
    /*
     * Both directions share one transaction state, so the dispatch feeds the
     * older pending event first and advances the core clock to that event's
     * arrival time; the current time only moves timeouts when nothing is
     * pending. Dequeue time is never used as an arrival time.
     */
    bridge_dispatch_process(&g_board_b_bridge, &g_board_b_host_rx_queue,
                            &g_board_b_rs485_rx_queue, board_b_micros());

    if (bridge_core_get_tx(&g_board_b_bridge, &tx_data, &tx_length) != 0)
    {
      if (g_board_b_bridge.state == BRIDGE_STATE_SEND_REQUEST)
      {
        board_b_rs485_send(tx_data, tx_length);
      }
      else
      {
        board_b_host_send(tx_data, tx_length);
      }

      bridge_core_tx_complete(&g_board_b_bridge, board_b_micros());
    }
  }
}
