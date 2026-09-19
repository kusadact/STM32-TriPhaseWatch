#ifndef BOARD_B_BRIDGE_CORE_H
#define BOARD_B_BRIDGE_CORE_H

#include <stdint.h>

#include "bridge_config.h"

typedef enum
{
  BRIDGE_STATE_IDLE = 0,
  BRIDGE_STATE_RECEIVE_REQUEST,
  BRIDGE_STATE_SEND_REQUEST,
  BRIDGE_STATE_WAIT_RESPONSE,
  BRIDGE_STATE_SEND_RESPONSE,
  BRIDGE_STATE_RECOVERY
} bridge_state_t;

typedef struct
{
  uint32_t host_rx_bytes;
  uint32_t rs485_rx_bytes;
  uint32_t request_frames_completed;
  uint32_t request_frames_forwarded;
  uint32_t response_frames_completed;
  uint32_t response_frames_forwarded;
  uint32_t unknown_request_frames;
  uint32_t host_gap_resyncs;
  uint32_t rs485_gap_resyncs;
  uint32_t host_max_frame_gap_us;
  uint32_t oversize_frames;
  uint32_t oversize_bytes_dropped;
  uint32_t incomplete_requests;
  uint32_t incomplete_responses;
  uint32_t receive_timeouts;
  uint32_t response_timeouts;
  uint32_t response_mismatches;
  uint32_t host_bytes_dropped;
  uint32_t rs485_bytes_dropped;
  uint32_t late_response_bytes;
  uint32_t unsolicited_rs485_bytes;
  uint32_t recovery_entries;
  uint32_t crc_bypass_frames;
} bridge_stats_t;

typedef enum
{
  REQUEST_KIND_PENDING = 0,
  REQUEST_KIND_SUPPORTED,
  REQUEST_KIND_UNKNOWN
} request_kind_t;

typedef enum
{
  RESPONSE_KIND_PENDING = 0,
  RESPONSE_KIND_SUPPORTED,
  RESPONSE_KIND_UNKNOWN
} response_kind_t;

typedef struct
{
  bridge_state_t state;
  bridge_stats_t stats;

  uint8_t request_data[BOARD_B_MAX_ADU_BYTES];
  uint16_t request_length;
  uint16_t request_expected_length;
  uint8_t request_function;
  request_kind_t request_kind;
  uint8_t request_discarding;
  uint32_t request_started_us;
  uint32_t request_last_byte_us;

  uint8_t response_data[BOARD_B_MAX_ADU_BYTES];
  uint16_t response_length;
  uint16_t response_expected_length;
  uint8_t response_function;
  response_kind_t response_kind;
  uint8_t response_discarding;
  uint32_t response_started_us;
  uint32_t response_last_byte_us;

  uint32_t wait_started_us;
  uint32_t recovery_started_us;
  uint32_t recovery_duration_us;
} bridge_core_t;

void bridge_core_init(bridge_core_t *ctx);

/*
 * All times are microseconds from the same free-running 1 MHz counter. The
 * core only ever compares unsigned differences, so the counter may wrap at
 * 2^32 us as long as consecutive events are less than half a period apart.
 */
void bridge_core_on_host_byte(bridge_core_t *ctx, uint8_t byte, uint32_t now_us);
void bridge_core_on_rs485_byte(bridge_core_t *ctx, uint8_t byte, uint32_t now_us);
void bridge_core_tick(bridge_core_t *ctx, uint32_t now_us);

/*
 * The receive queue dropped at least one byte in that direction, so whatever
 * was being assembled is no longer a contiguous piece of the wire.
 */
void bridge_core_on_host_continuity_lost(bridge_core_t *ctx, uint32_t now_us);
void bridge_core_on_rs485_continuity_lost(bridge_core_t *ctx, uint32_t now_us);

/*
 * Returns the next frame that the board layer must transmit. A pointer remains
 * valid until the matching bridge_core_tx_complete() call.
 */
int bridge_core_get_tx(const bridge_core_t *ctx, const uint8_t **data, uint16_t *length);

/*
 * Must be called only after the USART TC flag has been observed for the frame
 * returned by bridge_core_get_tx(). This is especially important for USART2:
 * the RS485 direction pin must not be dropped before this call.
 */
void bridge_core_tx_complete(bridge_core_t *ctx, uint32_t now_us);

#endif /* BOARD_B_BRIDGE_CORE_H */
