#include "bridge_core.h"

#include <stddef.h>

static int elapsed_at_least(uint32_t now_us, uint32_t start_us,
                            uint32_t interval_us)
{
  return (uint32_t)(now_us - start_us) >= interval_us;
}

/*
 * A completion-to-completion gap of at least BOARD_B_FRAME_GAP_US means the
 * line was silent for more than t3.5 (derivation in bridge_config.h), so the
 * bytes collected so far cannot belong to the frame that starts with the
 * current byte.
 */
static int frame_gap_elapsed(uint32_t now_us, uint32_t last_byte_us)
{
  return (uint32_t)(now_us - last_byte_us) >= BOARD_B_FRAME_GAP_US;
}

static int is_supported_request_function(uint8_t function)
{
  return (function == 0x03U) || (function == 0x04U) ||
         (function == 0x06U) || (function == 0x10U);
}

static void clear_request(bridge_core_t *ctx)
{
  ctx->request_length = 0U;
  ctx->request_expected_length = 0U;
  ctx->request_function = 0U;
  ctx->request_kind = REQUEST_KIND_PENDING;
  ctx->request_discarding = 0U;
  ctx->request_started_us = 0U;
  ctx->request_last_byte_us = 0U;
}

static void clear_response(bridge_core_t *ctx)
{
  ctx->response_length = 0U;
  ctx->response_expected_length = 0U;
  ctx->response_function = 0U;
  ctx->response_kind = RESPONSE_KIND_PENDING;
  ctx->response_discarding = 0U;
  ctx->response_started_us = 0U;
  ctx->response_last_byte_us = 0U;
}

/*
 * Drop an in-progress response without touching the request identity or the
 * response deadline: a truncated response must not extend the transaction.
 */
static void clear_response_assembly(bridge_core_t *ctx)
{
  ctx->response_length = 0U;
  ctx->response_expected_length = 0U;
  ctx->response_function = 0U;
  ctx->response_kind = RESPONSE_KIND_PENDING;
  ctx->response_discarding = 0U;
  ctx->response_started_us = 0U;
  ctx->response_last_byte_us = 0U;
}

static void enter_recovery(bridge_core_t *ctx, uint32_t now_us,
                           uint32_t duration_us)
{
  ctx->state = BRIDGE_STATE_RECOVERY;
  ctx->recovery_started_us = now_us;
  ctx->recovery_duration_us = duration_us;
  ctx->stats.recovery_entries++;
  clear_response(ctx);
}

static uint16_t expected_request_length(const bridge_core_t *ctx)
{
  if (ctx->request_length < 2U)
  {
    return 0U;
  }

  switch (ctx->request_function)
  {
    case 0x03U:
    case 0x04U:
    case 0x06U:
      return 8U;

    case 0x10U:
      if (ctx->request_length < 7U)
      {
        return 0U;
      }
      return (uint16_t)(9U + ctx->request_data[6]);

    default:
      return 0U;
  }
}

static void mark_request_oversize(bridge_core_t *ctx)
{
  ctx->stats.oversize_frames++;
  ctx->request_discarding = 1U;
  ctx->request_length = 0U;
  ctx->request_expected_length = 0U;
}

static void mark_response_oversize(bridge_core_t *ctx)
{
  ctx->stats.oversize_frames++;
  ctx->response_discarding = 1U;
  ctx->response_length = 0U;
  ctx->response_expected_length = 0U;
}

static void mark_response_mismatch(bridge_core_t *ctx)
{
  ctx->stats.response_mismatches++;
  ctx->response_discarding = 1U;
  ctx->response_length = 0U;
  ctx->response_expected_length = 0U;
}

static void finish_request(bridge_core_t *ctx)
{
  ctx->stats.request_frames_completed++;
  if (ctx->request_kind == REQUEST_KIND_UNKNOWN)
  {
    ctx->stats.unknown_request_frames++;
  }
  ctx->state = BRIDGE_STATE_SEND_REQUEST;
}

static void finish_unknown_request_after_gap(bridge_core_t *ctx)
{
  if (ctx->request_length >= BOARD_B_MIN_RTU_ADU_BYTES)
  {
    finish_request(ctx);
  }
  else
  {
    ctx->stats.incomplete_requests++;
    clear_request(ctx);
    ctx->state = BRIDGE_STATE_IDLE;
  }
}

static void finish_response(bridge_core_t *ctx)
{
  ctx->stats.response_frames_completed++;
  ctx->state = BRIDGE_STATE_SEND_RESPONSE;
}

static void finish_unknown_response_after_gap(bridge_core_t *ctx)
{
  if (ctx->response_length >= BOARD_B_MIN_RTU_ADU_BYTES)
  {
    finish_response(ctx);
  }
  else
  {
    ctx->stats.incomplete_responses++;
    ctx->response_discarding = 1U;
    ctx->response_length = 0U;
    ctx->response_expected_length = 0U;
  }
}

void bridge_core_init(bridge_core_t *ctx)
{
  uint8_t *bytes = (uint8_t *)ctx;
  uint32_t index;

  for (index = 0U; index < sizeof(*ctx); index++)
  {
    bytes[index] = 0U;
  }
  ctx->state = BRIDGE_STATE_IDLE;
}

void bridge_core_on_host_byte(bridge_core_t *ctx, uint8_t byte, uint32_t now_us)
{
  uint16_t expected;
  uint32_t gap_us;

  ctx->stats.host_rx_bytes++;

  if (ctx->state == BRIDGE_STATE_IDLE)
  {
    clear_request(ctx);
    ctx->state = BRIDGE_STATE_RECEIVE_REQUEST;
    ctx->request_started_us = now_us;
  }

  if (ctx->state != BRIDGE_STATE_RECEIVE_REQUEST)
  {
    ctx->stats.host_bytes_dropped++;
    return;
  }

  /*
   * Decide the boundary before the previous timestamp is overwritten.
   *
   * While an oversize frame is being discarded, a t3.5 gap is the reliable
   * boundary after which the host may start a new request; shorter gaps keep
   * dropping the tail of the bad frame.
   *
   * For an in-progress frame, supported function codes (and an address-only
   * prefix) are truncated by such a gap and are discarded. Unknown function
   * codes keep their separate 50 ms closing rule, because a known-length rule
   * cannot apply to them and the decision of 2026-09-20 only changed the
   * known-frame boundary.
   */
  if (ctx->request_discarding != 0U)
  {
    if (!frame_gap_elapsed(now_us, ctx->request_last_byte_us))
    {
      ctx->request_last_byte_us = now_us;
      ctx->stats.host_bytes_dropped++;
      return;
    }

    clear_request(ctx);
    ctx->request_started_us = now_us;
  }
  else if ((ctx->request_length != 0U) &&
           (ctx->request_kind != REQUEST_KIND_UNKNOWN))
  {
    gap_us = (uint32_t)(now_us - ctx->request_last_byte_us);

    if (gap_us >= BOARD_B_FRAME_GAP_US)
    {
      ctx->stats.host_gap_resyncs++;
      ctx->stats.incomplete_requests++;
      clear_request(ctx);
      ctx->request_started_us = now_us;
    }
    else if (gap_us > ctx->stats.host_max_frame_gap_us)
    {
      ctx->stats.host_max_frame_gap_us = gap_us;
    }
  }

  ctx->request_last_byte_us = now_us;

  if (ctx->request_length >= BOARD_B_MAX_ADU_BYTES)
  {
    mark_request_oversize(ctx);
    ctx->stats.host_bytes_dropped++;
    ctx->stats.oversize_bytes_dropped++;
    return;
  }

  ctx->request_data[ctx->request_length] = byte;
  ctx->request_length++;

  if (ctx->request_length == 1U)
  {
    return;
  }

  if (ctx->request_length == 2U)
  {
    ctx->request_function = byte;
    ctx->request_kind = is_supported_request_function(byte) ? REQUEST_KIND_SUPPORTED
                                                            : REQUEST_KIND_UNKNOWN;
  }

  if (ctx->request_kind == REQUEST_KIND_SUPPORTED)
  {
    expected = expected_request_length(ctx);
    if (expected > BOARD_B_MAX_ADU_BYTES)
    {
      mark_request_oversize(ctx);
      ctx->stats.host_bytes_dropped++;
      ctx->stats.oversize_bytes_dropped++;
      return;
    }

    ctx->request_expected_length = expected;
    if ((expected != 0U) && (ctx->request_length == expected))
    {
      finish_request(ctx);
    }
  }
}

/*
 * The receive queue lost bytes, so the current assembly is not contiguous.
 * Drop it (counting it once) and let the next byte start a fresh request; the
 * lost bytes are reported through the queue's own dropped counter.
 */
void bridge_core_on_host_continuity_lost(bridge_core_t *ctx, uint32_t now_us)
{
  (void)now_us;

  if (ctx->state != BRIDGE_STATE_RECEIVE_REQUEST)
  {
    return;
  }

  if ((ctx->request_length == 0U) && (ctx->request_discarding == 0U))
  {
    return;
  }

  if (ctx->request_discarding == 0U)
  {
    ctx->stats.incomplete_requests++;
  }
  clear_request(ctx);
  ctx->state = BRIDGE_STATE_IDLE;
}

static void process_supported_response_byte(bridge_core_t *ctx)
{
  uint16_t expected;

  if (ctx->response_kind != RESPONSE_KIND_SUPPORTED)
  {
    return;
  }

  if (ctx->response_length == 2U)
  {
    if (ctx->response_function == (uint8_t)(ctx->request_function | 0x80U))
    {
      ctx->response_expected_length = 5U;
      return;
    }

    if (ctx->response_function != ctx->request_function)
    {
      mark_response_mismatch(ctx);
      return;
    }

    if ((ctx->request_function == 0x06U) || (ctx->request_function == 0x10U))
    {
      ctx->response_expected_length = 8U;
      return;
    }
  }

  if ((ctx->response_function == ctx->request_function) &&
      ((ctx->request_function == 0x03U) || (ctx->request_function == 0x04U)))
  {
    if (ctx->response_length == 3U)
    {
      expected = (uint16_t)(5U + ctx->response_data[2]);
      if (expected > BOARD_B_MAX_ADU_BYTES)
      {
        mark_response_oversize(ctx);
        return;
      }
      ctx->response_expected_length = expected;
    }
  }

  expected = ctx->response_expected_length;
  if ((expected != 0U) && (ctx->response_length == expected))
  {
    finish_response(ctx);
  }
}

void bridge_core_on_rs485_byte(bridge_core_t *ctx, uint8_t byte, uint32_t now_us)
{
  ctx->stats.rs485_rx_bytes++;

  if (ctx->state == BRIDGE_STATE_RECOVERY)
  {
    ctx->stats.late_response_bytes++;
    ctx->stats.rs485_bytes_dropped++;
    return;
  }

  if (ctx->state != BRIDGE_STATE_WAIT_RESPONSE)
  {
    ctx->stats.unsolicited_rs485_bytes++;
    ctx->stats.rs485_bytes_dropped++;
    return;
  }

  if (ctx->response_discarding != 0U)
  {
    ctx->stats.rs485_bytes_dropped++;
    return;
  }

  /*
   * A t3.5 gap in front of this byte means the response being assembled was
   * truncated. Drop the partial (counted once) but keep the request identity
   * and wait_started_us: a late fragment must not restart the 1000 ms
   * transaction deadline.
   */
  if ((ctx->response_length != 0U) &&
      (ctx->response_kind != RESPONSE_KIND_UNKNOWN) &&
      frame_gap_elapsed(now_us, ctx->response_last_byte_us))
  {
    ctx->stats.rs485_gap_resyncs++;
    ctx->stats.incomplete_responses++;
    clear_response_assembly(ctx);
  }

  ctx->response_last_byte_us = now_us;

  if (ctx->response_length >= BOARD_B_MAX_ADU_BYTES)
  {
    mark_response_oversize(ctx);
    ctx->stats.rs485_bytes_dropped++;
    ctx->stats.oversize_bytes_dropped++;
    return;
  }

  ctx->response_data[ctx->response_length] = byte;
  ctx->response_length++;

  if (ctx->response_length == 1U)
  {
    return;
  }

  if (ctx->response_length == 2U)
  {
    ctx->response_function = byte;

    if (ctx->request_function < 0x80U &&
        byte == (uint8_t)(ctx->request_function | 0x80U))
    {
      ctx->response_kind = RESPONSE_KIND_SUPPORTED;
      ctx->response_expected_length = 5U;
      return;
    }

    if (byte == ctx->request_function)
    {
      ctx->response_kind = is_supported_request_function(ctx->request_function)
                               ? RESPONSE_KIND_SUPPORTED
                               : RESPONSE_KIND_UNKNOWN;
    }
    else
    {
      mark_response_mismatch(ctx);
      ctx->stats.rs485_bytes_dropped++;
      return;
    }
  }

  if (ctx->response_kind == RESPONSE_KIND_SUPPORTED)
  {
    process_supported_response_byte(ctx);
  }
}

/*
 * The RS485 receive queue lost bytes: the response being assembled has a hole
 * in it, so it can never be forwarded as-is. Keep the request identity and the
 * deadline; the slave may still produce a complete response inside the
 * remaining window.
 */
void bridge_core_on_rs485_continuity_lost(bridge_core_t *ctx, uint32_t now_us)
{
  (void)now_us;

  if (ctx->state != BRIDGE_STATE_WAIT_RESPONSE)
  {
    return;
  }

  if ((ctx->response_length == 0U) && (ctx->response_discarding == 0U))
  {
    return;
  }

  if (ctx->response_discarding == 0U)
  {
    ctx->stats.incomplete_responses++;
  }
  clear_response_assembly(ctx);
}

void bridge_core_tick(bridge_core_t *ctx, uint32_t now_us)
{
  if (ctx->state == BRIDGE_STATE_RECEIVE_REQUEST)
  {
    if (ctx->request_discarding != 0U)
    {
      if (elapsed_at_least(now_us, ctx->request_last_byte_us,
                           BOARD_B_UNKNOWN_FRAME_GAP_US) ||
          elapsed_at_least(now_us, ctx->request_started_us,
                           BOARD_B_REQUEST_ASSEMBLY_TIMEOUT_US))
      {
        clear_request(ctx);
        ctx->state = BRIDGE_STATE_IDLE;
      }
      return;
    }

    if (ctx->request_kind == REQUEST_KIND_UNKNOWN)
    {
      if (elapsed_at_least(now_us, ctx->request_last_byte_us,
                           BOARD_B_UNKNOWN_FRAME_GAP_US) ||
          elapsed_at_least(now_us, ctx->request_started_us,
                           BOARD_B_REQUEST_ASSEMBLY_TIMEOUT_US))
      {
        finish_unknown_request_after_gap(ctx);
      }
      return;
    }

    if (elapsed_at_least(now_us, ctx->request_started_us,
                         BOARD_B_REQUEST_ASSEMBLY_TIMEOUT_US))
    {
      ctx->stats.incomplete_requests++;
      ctx->stats.receive_timeouts++;
      clear_request(ctx);
      ctx->state = BRIDGE_STATE_IDLE;
    }
    return;
  }

  if (ctx->state == BRIDGE_STATE_WAIT_RESPONSE)
  {
    if ((ctx->response_discarding == 0U) &&
        (ctx->response_kind == RESPONSE_KIND_UNKNOWN) &&
        (ctx->response_length != 0U) &&
        elapsed_at_least(now_us, ctx->response_last_byte_us,
                         BOARD_B_UNKNOWN_FRAME_GAP_US))
    {
      finish_unknown_response_after_gap(ctx);
    }

    if (ctx->state == BRIDGE_STATE_WAIT_RESPONSE &&
        elapsed_at_least(now_us, ctx->wait_started_us,
                         BOARD_B_RESPONSE_TIMEOUT_US))
    {
      ctx->stats.response_timeouts++;
      enter_recovery(ctx, now_us, BOARD_B_RESPONSE_RECOVERY_US);
    }
    return;
  }

  if ((ctx->state == BRIDGE_STATE_RECOVERY) &&
      elapsed_at_least(now_us, ctx->recovery_started_us,
                       ctx->recovery_duration_us))
  {
    clear_request(ctx);
    clear_response(ctx);
    ctx->state = BRIDGE_STATE_IDLE;
  }
}

int bridge_core_get_tx(const bridge_core_t *ctx, const uint8_t **data, uint16_t *length)
{
  if (ctx->state == BRIDGE_STATE_SEND_REQUEST)
  {
    *data = ctx->request_data;
    *length = ctx->request_length;
    return (ctx->request_length != 0U);
  }

  if (ctx->state == BRIDGE_STATE_SEND_RESPONSE)
  {
    *data = ctx->response_data;
    *length = ctx->response_length;
    return (ctx->response_length != 0U);
  }

  *data = NULL;
  *length = 0U;
  return 0;
}

void bridge_core_tx_complete(bridge_core_t *ctx, uint32_t now_us)
{
  if (ctx->state == BRIDGE_STATE_SEND_REQUEST)
  {
    ctx->stats.request_frames_forwarded++;
    ctx->stats.crc_bypass_frames++;
    clear_response(ctx);
    ctx->wait_started_us = now_us;
    ctx->state = BRIDGE_STATE_WAIT_RESPONSE;
    return;
  }

  if (ctx->state == BRIDGE_STATE_SEND_RESPONSE)
  {
    ctx->stats.response_frames_forwarded++;
    ctx->stats.crc_bypass_frames++;
    clear_request(ctx);
    clear_response(ctx);
    ctx->state = BRIDGE_STATE_IDLE;
  }
}
