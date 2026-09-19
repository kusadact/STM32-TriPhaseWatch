#ifndef BOARD_B_BRIDGE_DISPATCH_H
#define BOARD_B_BRIDGE_DISPATCH_H

#include <stdint.h>

#include "board_b_rx_queue.h"
#include "bridge_core.h"

/*
 * Feed both receive directions into the bridge state machine in arrival
 * order, then advance the timeout clock to now_us.
 *
 * Both directions share one transaction state, so "drain host, then drain
 * RS485" is not equivalent: with that order a response that arrived before a
 * new host byte would still be processed after it, or a request could be
 * judged against a clock that already moved past its arrival time. The
 * dispatch therefore always takes the older pending event, advances the core
 * to that event's arrival time first, and never moves the core clock
 * backwards.
 */
void bridge_dispatch_process(bridge_core_t *core,
                             board_b_rx_queue_t *host_queue,
                             board_b_rx_queue_t *rs485_queue,
                             uint32_t now_us);

#endif /* BOARD_B_BRIDGE_DISPATCH_H */
