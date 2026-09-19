#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge_core.h"

static int g_failures;

#define CHECK(condition)                                                        \
  do                                                                            \
  {                                                                             \
    if (!(condition))                                                           \
    {                                                                           \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);      \
      g_failures++;                                                             \
    }                                                                           \
  } while (0)

/* One 8E1 character at 9600 bit/s. */
#define CHAR_US 1146U

/*
 * Chunk gaps used by the fragmentation test must stay below the t3.5-based
 * boundary derived in bridge_config.h (BOARD_B_FRAME_GAP_US = 5157 us).
 */
#define CHUNK_GAP_MIN_US 1000U
#define CHUNK_GAP_RANGE_US 4000U

static const uint8_t kRequest03[] = {0x01U, 0x03U, 0x00U, 0x00U, 0x00U,
                                     0x02U, 0xDEU, 0xADU};
static const uint8_t kResponse03[] = {0x01U, 0x03U, 0x04U, 0x11U, 0x22U,
                                      0x33U, 0x44U, 0xFEU, 0xEDU};
static const uint8_t kRequest04[] = {0x01U, 0x04U, 0x00U, 0x00U, 0x00U,
                                     0x01U, 0xCAU, 0xFEU};
static const uint8_t kException04[] = {0x01U, 0x84U, 0x02U, 0xC0U, 0xF1U};
static const uint8_t kRequest06[] = {0x01U, 0x06U, 0x00U, 0x10U, 0x12U,
                                     0x34U, 0xCAU, 0xFEU};
static const uint8_t kResponse06[] = {0x01U, 0x06U, 0x00U, 0x10U, 0x12U,
                                      0x34U, 0xCAU, 0xFEU};
static const uint8_t kRequest10[] = {0x01U, 0x10U, 0x00U, 0x00U, 0x00U, 0x04U,
                                     0x08U, 0x00U, 0x01U, 0x00U, 0x02U, 0x00U,
                                     0x03U, 0x00U, 0x04U, 0xDEU, 0xADU};
static const uint8_t kResponse10[] = {0x01U, 0x10U, 0x00U, 0x00U, 0x00U,
                                      0x04U, 0xCAU, 0xFEU};

static void expect_tx(const bridge_core_t *ctx, const uint8_t *expected,
                      uint16_t expected_length)
{
  const uint8_t *actual = NULL;
  uint16_t actual_length = 0U;
  int ready = bridge_core_get_tx(ctx, &actual, &actual_length);

  CHECK(ready != 0);
  CHECK(actual_length == expected_length);
  if ((ready != 0) && (actual != NULL) && (actual_length == expected_length))
  {
    CHECK(memcmp(actual, expected, expected_length) == 0);
  }
}

static void expect_no_tx(const bridge_core_t *ctx)
{
  const uint8_t *actual = NULL;
  uint16_t actual_length = 0U;

  CHECK(bridge_core_get_tx(ctx, &actual, &actual_length) == 0);
  CHECK(actual_length == 0U);
}

static void feed_host(bridge_core_t *ctx, const uint8_t *data, uint16_t length,
                      uint32_t *now_us, uint32_t step_us)
{
  uint16_t index;

  for (index = 0U; index < length; index++)
  {
    bridge_core_on_host_byte(ctx, data[index], *now_us);
    *now_us += step_us;
  }
}

static void feed_rs485(bridge_core_t *ctx, const uint8_t *data, uint16_t length,
                       uint32_t *now_us, uint32_t step_us)
{
  uint16_t index;

  for (index = 0U; index < length; index++)
  {
    bridge_core_on_rs485_byte(ctx, data[index], *now_us);
    *now_us += step_us;
  }
}

static void test_single_frame_is_byte_transparent(void)
{
  bridge_core_t ctx;
  uint32_t now_us = 0U;

  bridge_core_init(&ctx);
  feed_host(&ctx, kRequest03, sizeof(kRequest03), &now_us, CHAR_US);

  CHECK(ctx.state == BRIDGE_STATE_SEND_REQUEST);
  CHECK(ctx.stats.request_frames_completed == 1U);
  CHECK(ctx.stats.request_frames_forwarded == 0U);
  CHECK(ctx.stats.unknown_request_frames == 0U);
  CHECK(ctx.stats.host_gap_resyncs == 0U);
  CHECK(ctx.stats.host_max_frame_gap_us == CHAR_US);
  expect_tx(&ctx, kRequest03, sizeof(kRequest03));

  bridge_core_tx_complete(&ctx, now_us);
  CHECK(ctx.state == BRIDGE_STATE_WAIT_RESPONSE);
  CHECK(ctx.stats.request_frames_forwarded == 1U);
  CHECK(ctx.stats.crc_bypass_frames == 1U);

  /*
   * The response is deliberately CRC-invalid too. The bridge must not turn
   * CRC validation or frame repair into a hidden business behavior.
   */
  feed_rs485(&ctx, kResponse03, sizeof(kResponse03), &now_us, CHAR_US);
  CHECK(ctx.state == BRIDGE_STATE_SEND_RESPONSE);
  CHECK(ctx.stats.response_frames_completed == 1U);
  expect_tx(&ctx, kResponse03, sizeof(kResponse03));

  bridge_core_tx_complete(&ctx, now_us);
  CHECK(ctx.state == BRIDGE_STATE_IDLE);
  CHECK(ctx.stats.response_frames_forwarded == 1U);
  CHECK(ctx.stats.recovery_entries == 0U);
  CHECK(ctx.stats.crc_bypass_frames == 2U);
  expect_no_tx(&ctx);

  CHECK(ctx.stats.host_rx_bytes == sizeof(kRequest03));
  CHECK(ctx.stats.rs485_rx_bytes == sizeof(kResponse03));
  CHECK(ctx.stats.host_bytes_dropped == 0U);
  CHECK(ctx.stats.rs485_bytes_dropped == 0U);
  CHECK(ctx.stats.response_timeouts == 0U);
}

static void test_broadcast_and_bad_crc_are_forwarded_transparently(void)
{
  bridge_core_t ctx;
  uint32_t now_us = 0U;
  uint8_t broadcast[8] = {0x00U, 0x06U, 0x00U, 0x10U,
                          0x12U, 0x34U, 0xDEU, 0xADU};

  bridge_core_init(&ctx);
  feed_host(&ctx, broadcast, sizeof(broadcast), &now_us, CHAR_US);

  CHECK(ctx.state == BRIDGE_STATE_SEND_REQUEST);
  CHECK(ctx.stats.host_gap_resyncs == 0U);
  expect_tx(&ctx, broadcast, sizeof(broadcast));
}

/*
 * Contract decision 2026-09-20 (option A'): a supported frame may still be
 * split into arbitrary chunks by the USB path, but only while consecutive
 * bytes stay closer than BOARD_B_FRAME_GAP_US. The previous behaviour, which
 * kept a frame open through 55 ms gaps and 5..29 ms chunk gaps, was replaced
 * by the t3.5-based boundary; see local review record 20260920-0230.
 */
static void test_fragmentation_within_frame_gap_keeps_frame(void)
{
  bridge_core_t ctx;
  uint32_t seed;

  srand(0x5A17U);
  for (seed = 0U; seed < 100U; seed++)
  {
    uint32_t now_us = 0U;
    uint16_t position = 0U;

    bridge_core_init(&ctx);
    while (position < sizeof(kRequest10))
    {
      uint16_t chunk = (uint16_t)(1U + (uint16_t)(rand() % 4));
      uint16_t end = (uint16_t)(position + chunk);

      if (end > sizeof(kRequest10))
      {
        end = (uint16_t)sizeof(kRequest10);
      }

      while (position < end)
      {
        bridge_core_on_host_byte(&ctx, kRequest10[position], now_us);
        position++;
        now_us += CHAR_US;
      }

      if (position < sizeof(kRequest10))
      {
        /* now_us already points one character past the chunk's last byte. */
        now_us = (now_us - CHAR_US) + CHUNK_GAP_MIN_US +
                 (uint32_t)(rand() % CHUNK_GAP_RANGE_US);
      }
    }

    CHECK(ctx.state == BRIDGE_STATE_SEND_REQUEST);
    CHECK(ctx.stats.host_gap_resyncs == 0U);
    CHECK(ctx.stats.incomplete_requests == 0U);
    CHECK(ctx.stats.oversize_frames == 0U);
    expect_tx(&ctx, kRequest10, sizeof(kRequest10));
  }
}

static void test_address_only_prefix_resynchronises(void)
{
  bridge_core_t ctx;
  uint32_t now_us = 0U;

  bridge_core_init(&ctx);
  bridge_core_on_host_byte(&ctx, 0x01U, now_us);
  CHECK(ctx.request_kind == REQUEST_KIND_PENDING);

  now_us += BOARD_B_FRAME_GAP_US;
  feed_host(&ctx, kRequest06, sizeof(kRequest06), &now_us, CHAR_US);

  CHECK(ctx.stats.host_gap_resyncs == 1U);
  CHECK(ctx.stats.incomplete_requests == 1U);
  CHECK(ctx.state == BRIDGE_STATE_SEND_REQUEST);
  expect_tx(&ctx, kRequest06, sizeof(kRequest06));
}

static void test_truncated_prefix_resynchronises(void)
{
  static const uint32_t gaps_us[3] = {20000U, 100000U, 500000U};
  bridge_core_t ctx;
  uint32_t index;

  for (index = 0U; index < 3U; index++)
  {
    uint32_t now_us = 0U;
    uint8_t partial[3] = {0x01U, 0x03U, 0x00U};

    bridge_core_init(&ctx);
    feed_host(&ctx, partial, sizeof(partial), &now_us, CHAR_US);
    CHECK(ctx.state == BRIDGE_STATE_RECEIVE_REQUEST);

    now_us += gaps_us[index];
    feed_host(&ctx, kRequest03, sizeof(kRequest03), &now_us, CHAR_US);

    CHECK(ctx.state == BRIDGE_STATE_SEND_REQUEST);
    CHECK(ctx.stats.incomplete_requests == 1U);
    CHECK(ctx.stats.host_gap_resyncs == 1U);
    CHECK(ctx.stats.host_bytes_dropped == 0U);
    CHECK(ctx.request_length == sizeof(kRequest03));
    expect_tx(&ctx, kRequest03, sizeof(kRequest03));
  }
}

static void test_frame_gap_boundary(void)
{
  bridge_core_t ctx;
  uint32_t now_us;
  uint8_t partial[3] = {0x01U, 0x03U, 0x00U};

  /* One microsecond below the boundary: the frame keeps assembling. */
  bridge_core_init(&ctx);
  now_us = 0U;
  feed_host(&ctx, partial, sizeof(partial), &now_us, CHAR_US);
  now_us = (now_us - CHAR_US) + BOARD_B_FRAME_GAP_US - 1U;
  bridge_core_on_host_byte(&ctx, 0x00U, now_us);
  CHECK(ctx.state == BRIDGE_STATE_RECEIVE_REQUEST);
  CHECK(ctx.stats.host_gap_resyncs == 0U);
  CHECK(ctx.stats.incomplete_requests == 0U);
  CHECK(ctx.request_length == 4U);

  /* At the boundary: the prefix is dropped and the byte starts a new frame. */
  bridge_core_init(&ctx);
  now_us = 0U;
  feed_host(&ctx, partial, sizeof(partial), &now_us, CHAR_US);
  now_us = (now_us - CHAR_US) + BOARD_B_FRAME_GAP_US;
  bridge_core_on_host_byte(&ctx, 0x01U, now_us);
  CHECK(ctx.stats.host_gap_resyncs == 1U);
  CHECK(ctx.stats.incomplete_requests == 1U);
  CHECK(ctx.request_length == 1U);
  CHECK(ctx.request_data[0] == 0x01U);

  /* One microsecond above the boundary behaves the same. */
  bridge_core_init(&ctx);
  now_us = 0U;
  feed_host(&ctx, partial, sizeof(partial), &now_us, CHAR_US);
  now_us = (now_us - CHAR_US) + BOARD_B_FRAME_GAP_US + 1U;
  bridge_core_on_host_byte(&ctx, 0x01U, now_us);
  CHECK(ctx.stats.host_gap_resyncs == 1U);
  CHECK(ctx.stats.incomplete_requests == 1U);
  CHECK(ctx.request_length == 1U);
}

static void test_write_multiple_partial_header_resynchronises(void)
{
  bridge_core_t ctx;
  uint32_t now_us = 0U;
  uint8_t partial[4] = {0x01U, 0x10U, 0x00U, 0x00U};

  bridge_core_init(&ctx);
  feed_host(&ctx, partial, sizeof(partial), &now_us, CHAR_US);
  CHECK(ctx.request_kind == REQUEST_KIND_SUPPORTED);
  CHECK(ctx.request_expected_length == 0U);

  now_us += BOARD_B_FRAME_GAP_US;
  feed_host(&ctx, kRequest06, sizeof(kRequest06), &now_us, CHAR_US);

  CHECK(ctx.stats.incomplete_requests == 1U);
  CHECK(ctx.state == BRIDGE_STATE_SEND_REQUEST);
  expect_tx(&ctx, kRequest06, sizeof(kRequest06));
}

static void test_short_unknown_and_oversize_frames(void)
{
  bridge_core_t ctx;
  uint32_t now_us;
  uint32_t last_us;
  uint8_t unknown[] = {0x01U, 0x45U, 0xAAU, 0xBBU};
  uint8_t unknown_short[] = {0x01U, 0x45U, 0xAAU};
  uint8_t oversize_request[7] = {0x01U, 0x10U, 0x00U, 0x00U, 0x00U, 0x01U, 0xFFU};
  uint32_t index;

  /* Address plus function, then the overall assembly timeout. */
  bridge_core_init(&ctx);
  bridge_core_on_host_byte(&ctx, 0x01U, 0U);
  bridge_core_on_host_byte(&ctx, 0x03U, CHAR_US);
  bridge_core_tick(&ctx, BOARD_B_REQUEST_ASSEMBLY_TIMEOUT_US - 1U);
  CHECK(ctx.state == BRIDGE_STATE_RECEIVE_REQUEST);
  bridge_core_tick(&ctx, BOARD_B_REQUEST_ASSEMBLY_TIMEOUT_US);
  CHECK(ctx.state == BRIDGE_STATE_IDLE);
  CHECK(ctx.stats.incomplete_requests == 1U);
  CHECK(ctx.stats.receive_timeouts == 1U);
  expect_no_tx(&ctx);

  /* A complete unknown-function frame closes on its own 50 ms gap rule. */
  bridge_core_init(&ctx);
  now_us = 0U;
  feed_host(&ctx, unknown, sizeof(unknown), &now_us, CHAR_US);
  last_us = now_us - CHAR_US;
  bridge_core_tick(&ctx, last_us + BOARD_B_UNKNOWN_FRAME_GAP_US - 1U);
  CHECK(ctx.state == BRIDGE_STATE_RECEIVE_REQUEST);
  bridge_core_tick(&ctx, last_us + BOARD_B_UNKNOWN_FRAME_GAP_US);
  CHECK(ctx.state == BRIDGE_STATE_SEND_REQUEST);
  CHECK(ctx.stats.unknown_request_frames == 1U);
  expect_tx(&ctx, unknown, sizeof(unknown));

  /* A too-short unknown frame is dropped, not forwarded. */
  bridge_core_init(&ctx);
  now_us = 0U;
  feed_host(&ctx, unknown_short, sizeof(unknown_short), &now_us, CHAR_US);
  bridge_core_tick(&ctx, now_us + BOARD_B_UNKNOWN_FRAME_GAP_US);
  CHECK(ctx.state == BRIDGE_STATE_IDLE);
  CHECK(ctx.stats.incomplete_requests == 1U);
  CHECK(ctx.stats.unknown_request_frames == 0U);

  /* An oversize 0x10 header is discarded with its whole tail. */
  bridge_core_init(&ctx);
  now_us = 0U;
  feed_host(&ctx, oversize_request, sizeof(oversize_request), &now_us, CHAR_US);
  CHECK(ctx.state == BRIDGE_STATE_RECEIVE_REQUEST);
  CHECK(ctx.stats.oversize_frames == 1U);
  CHECK(ctx.stats.oversize_bytes_dropped == 1U);
  for (index = 0U; index < 249U; index++)
  {
    bridge_core_on_host_byte(&ctx, 0x55U, now_us);
    now_us += 1000U;
  }
  bridge_core_tick(&ctx, now_us + BOARD_B_UNKNOWN_FRAME_GAP_US);
  CHECK(ctx.state == BRIDGE_STATE_IDLE);
  CHECK(ctx.stats.request_frames_completed == 0U);
  CHECK(ctx.stats.oversize_frames == 1U);
  CHECK(ctx.stats.host_bytes_dropped == 250U);

  /* A frame that runs past the ADU limit is dropped as a whole too. */
  bridge_core_init(&ctx);
  now_us = 0U;
  for (index = 0U; index < 257U; index++)
  {
    bridge_core_on_host_byte(&ctx, (uint8_t)(index & 0xFFU), now_us);
    now_us += 1000U;
  }
  CHECK(ctx.stats.oversize_frames == 1U);
  CHECK(ctx.stats.oversize_bytes_dropped == 1U);
  CHECK(ctx.stats.request_frames_completed == 0U);
  expect_no_tx(&ctx);
  bridge_core_tick(&ctx, now_us + BOARD_B_UNKNOWN_FRAME_GAP_US);
  CHECK(ctx.state == BRIDGE_STATE_IDLE);
}

static void test_sticky_frames_are_not_merged(void)
{
  bridge_core_t ctx;
  uint32_t now_us = 0U;

  bridge_core_init(&ctx);
  feed_host(&ctx, kRequest03, sizeof(kRequest03), &now_us, CHAR_US);
  CHECK(ctx.state == BRIDGE_STATE_SEND_REQUEST);

  feed_host(&ctx, kRequest06, sizeof(kRequest06), &now_us, CHAR_US);
  CHECK(ctx.state == BRIDGE_STATE_SEND_REQUEST);
  CHECK(ctx.request_length == sizeof(kRequest03));
  CHECK(ctx.stats.host_bytes_dropped == sizeof(kRequest06));
  expect_tx(&ctx, kRequest03, sizeof(kRequest03));

  bridge_core_tx_complete(&ctx, now_us);
  feed_rs485(&ctx, kResponse03, sizeof(kResponse03), &now_us, CHAR_US);
  CHECK(ctx.state == BRIDGE_STATE_SEND_RESPONSE);
  expect_tx(&ctx, kResponse03, sizeof(kResponse03));
  bridge_core_tx_complete(&ctx, now_us);
  CHECK(ctx.state == BRIDGE_STATE_IDLE);
  expect_no_tx(&ctx);
}

static void test_success_response_returns_to_idle_and_accepts_fast_next_request(void)
{
  bridge_core_t ctx;
  uint32_t now_us = 100000U;

  bridge_core_init(&ctx);
  feed_host(&ctx, kRequest03, sizeof(kRequest03), &now_us, CHAR_US);
  bridge_core_tx_complete(&ctx, now_us);

  now_us += 20000U;
  feed_rs485(&ctx, kResponse03, sizeof(kResponse03), &now_us, CHAR_US);
  expect_tx(&ctx, kResponse03, sizeof(kResponse03));
  bridge_core_tx_complete(&ctx, now_us);

  CHECK(ctx.state == BRIDGE_STATE_IDLE);
  CHECK(ctx.stats.recovery_entries == 0U);
  CHECK(ctx.stats.host_rx_bytes == sizeof(kRequest03));
  CHECK(ctx.stats.host_bytes_dropped == 0U);
  expect_no_tx(&ctx);

  /* A compliant master may send immediately after its RTU t3.5 silence. */
  now_us += BOARD_B_FRAME_GAP_US;
  feed_host(&ctx, kRequest06, sizeof(kRequest06), &now_us, CHAR_US);
  CHECK(ctx.state == BRIDGE_STATE_SEND_REQUEST);
  CHECK(ctx.stats.host_rx_bytes == sizeof(kRequest03) + sizeof(kRequest06));
  CHECK(ctx.stats.host_bytes_dropped == 0U);
  CHECK(ctx.stats.host_gap_resyncs == 0U);
  expect_tx(&ctx, kRequest06, sizeof(kRequest06));
}

static void test_idle_late_rs485_does_not_pollute_next_transaction(void)
{
  bridge_core_t ctx;
  uint32_t now_us = 200000U;
  uint8_t late_response[] = {0x01U, 0x03U, 0x02U, 0xAAU, 0xBBU, 0xEEU, 0xEEU};
  uint8_t new_response[] = {0x01U, 0x03U, 0x02U, 0x11U, 0x22U, 0xCAU, 0xFEU};

  bridge_core_init(&ctx);
  feed_host(&ctx, kRequest03, sizeof(kRequest03), &now_us, CHAR_US);
  bridge_core_tx_complete(&ctx, now_us);
  feed_rs485(&ctx, kResponse03, sizeof(kResponse03), &now_us, CHAR_US);
  expect_tx(&ctx, kResponse03, sizeof(kResponse03));
  bridge_core_tx_complete(&ctx, now_us);
  CHECK(ctx.state == BRIDGE_STATE_IDLE);

  feed_rs485(&ctx, late_response, sizeof(late_response), &now_us, CHAR_US);
  CHECK(ctx.state == BRIDGE_STATE_IDLE);
  CHECK(ctx.stats.unsolicited_rs485_bytes == sizeof(late_response));
  CHECK(ctx.stats.rs485_bytes_dropped == sizeof(late_response));
  CHECK(ctx.stats.late_response_bytes == 0U);
  expect_no_tx(&ctx);

  now_us += BOARD_B_FRAME_GAP_US;
  feed_host(&ctx, kRequest03, sizeof(kRequest03), &now_us, CHAR_US);
  expect_tx(&ctx, kRequest03, sizeof(kRequest03));
  bridge_core_tx_complete(&ctx, now_us);
  feed_rs485(&ctx, new_response, sizeof(new_response), &now_us, CHAR_US);
  CHECK(ctx.state == BRIDGE_STATE_SEND_RESPONSE);
  expect_tx(&ctx, new_response, sizeof(new_response));
  bridge_core_tx_complete(&ctx, now_us);
  CHECK(ctx.state == BRIDGE_STATE_IDLE);
}

static void test_maximum_supported_adu_size_is_forwarded(void)
{
  bridge_core_t ctx;
  uint8_t request[BOARD_B_MAX_ADU_BYTES];
  uint32_t now_us = 0U;
  uint32_t index;

  for (index = 0U; index < sizeof(request); index++)
  {
    request[index] = 0x5AU;
  }

  request[0] = 0x01U;
  request[1] = 0x10U;
  request[2] = 0x00U;
  request[3] = 0x00U;
  request[4] = 0x00U;
  request[5] = 0x7BU;
  request[6] = 0xF7U; /* 247 bytes: 9 + 247 = 256 */

  bridge_core_init(&ctx);
  feed_host(&ctx, request, sizeof(request), &now_us, CHAR_US);
  CHECK(ctx.state == BRIDGE_STATE_SEND_REQUEST);
  CHECK(ctx.request_length == BOARD_B_MAX_ADU_BYTES);
  CHECK(ctx.stats.oversize_frames == 0U);
  CHECK(ctx.stats.host_gap_resyncs == 0U);
  expect_tx(&ctx, request, sizeof(request));
}

static void test_response_timeout_and_late_response_do_not_pollute_next_transaction(void)
{
  bridge_core_t ctx;
  uint32_t now_us = 0U;
  uint32_t request_sent_at;
  uint32_t recovery_at;
  uint8_t late_response[] = {0x01U, 0x03U, 0x02U, 0xAAU, 0xBBU, 0xEEU, 0xEEU};
  uint8_t new_response[] = {0x01U, 0x03U, 0x02U, 0x11U, 0x22U, 0xCAU, 0xFEU};
  const uint8_t *tx_data = NULL;
  uint16_t tx_length = 0U;

  bridge_core_init(&ctx);
  feed_host(&ctx, kRequest03, sizeof(kRequest03), &now_us, CHAR_US);
  request_sent_at = now_us;
  bridge_core_tx_complete(&ctx, request_sent_at);

  bridge_core_tick(&ctx, request_sent_at + BOARD_B_RESPONSE_TIMEOUT_US - 1U);
  CHECK(ctx.state == BRIDGE_STATE_WAIT_RESPONSE);
  recovery_at = request_sent_at + BOARD_B_RESPONSE_TIMEOUT_US;
  bridge_core_tick(&ctx, recovery_at);
  CHECK(ctx.state == BRIDGE_STATE_RECOVERY);
  CHECK(ctx.stats.response_timeouts == 1U);
  CHECK(bridge_core_get_tx(&ctx, &tx_data, &tx_length) == 0);

  /* Recovery is a failure-only state and intentionally drops host requests. */
  now_us = recovery_at;
  feed_host(&ctx, kRequest06, sizeof(kRequest06), &now_us, CHAR_US);
  CHECK(ctx.state == BRIDGE_STATE_RECOVERY);
  CHECK(ctx.stats.host_bytes_dropped == sizeof(kRequest06));

  /* The late frame arrives during the recovery window and must be discarded. */
  feed_rs485(&ctx, late_response, sizeof(late_response), &now_us, CHAR_US);
  CHECK(ctx.stats.late_response_bytes == sizeof(late_response));
  CHECK(ctx.stats.response_frames_forwarded == 0U);

  bridge_core_tick(&ctx, recovery_at + BOARD_B_RESPONSE_RECOVERY_US - 1U);
  CHECK(ctx.state == BRIDGE_STATE_RECOVERY);
  bridge_core_tick(&ctx, recovery_at + BOARD_B_RESPONSE_RECOVERY_US);
  CHECK(ctx.state == BRIDGE_STATE_IDLE);

  /* A fresh transaction must carry only its own response. */
  now_us = recovery_at + BOARD_B_RESPONSE_RECOVERY_US + 10U;
  feed_host(&ctx, kRequest03, sizeof(kRequest03), &now_us, CHAR_US);
  expect_tx(&ctx, kRequest03, sizeof(kRequest03));
  bridge_core_tx_complete(&ctx, now_us);
  feed_rs485(&ctx, new_response, sizeof(new_response), &now_us, CHAR_US);
  CHECK(ctx.state == BRIDGE_STATE_SEND_RESPONSE);
  expect_tx(&ctx, new_response, sizeof(new_response));
  bridge_core_tx_complete(&ctx, now_us);
  CHECK(ctx.stats.late_response_bytes == sizeof(late_response));
  CHECK(ctx.stats.response_frames_forwarded == 1U);
}

static void test_exception_response_is_five_bytes(void)
{
  bridge_core_t ctx;
  uint32_t now_us = 0U;

  bridge_core_init(&ctx);
  feed_host(&ctx, kRequest04, sizeof(kRequest04), &now_us, CHAR_US);
  bridge_core_tx_complete(&ctx, now_us);
  feed_rs485(&ctx, kException04, sizeof(kException04), &now_us, CHAR_US);
  CHECK(ctx.state == BRIDGE_STATE_SEND_RESPONSE);
  CHECK(ctx.response_length == 5U);
  expect_tx(&ctx, kException04, sizeof(kException04));
}

static void test_write_multiple_response_is_eight_bytes(void)
{
  bridge_core_t ctx;
  uint32_t now_us = 0U;

  bridge_core_init(&ctx);
  feed_host(&ctx, kRequest10, sizeof(kRequest10), &now_us, CHAR_US);
  bridge_core_tx_complete(&ctx, now_us);
  feed_rs485(&ctx, kResponse10, sizeof(kResponse10), &now_us, CHAR_US);
  CHECK(ctx.state == BRIDGE_STATE_SEND_RESPONSE);
  CHECK(ctx.response_length == 8U);
  expect_tx(&ctx, kResponse10, sizeof(kResponse10));
}

static void test_unknown_request_and_response_use_gap_boundary(void)
{
  bridge_core_t ctx;
  uint32_t now_us = 0U;
  uint8_t request[] = {0x01U, 0x45U, 0xAAU, 0xBBU};
  uint8_t response[] = {0x01U, 0x45U, 0xCCU, 0xDDU, 0xEEU, 0xFFU};

  bridge_core_init(&ctx);
  feed_host(&ctx, request, sizeof(request), &now_us, CHAR_US);
  bridge_core_tick(&ctx, now_us + BOARD_B_UNKNOWN_FRAME_GAP_US);
  CHECK(ctx.state == BRIDGE_STATE_SEND_REQUEST);
  bridge_core_tx_complete(&ctx, now_us);

  feed_rs485(&ctx, response, sizeof(response), &now_us, CHAR_US);
  bridge_core_tick(&ctx, now_us + BOARD_B_UNKNOWN_FRAME_GAP_US);
  CHECK(ctx.state == BRIDGE_STATE_SEND_RESPONSE);
  expect_tx(&ctx, response, sizeof(response));
}

static void test_partial_response_resynchronises(void)
{
  bridge_core_t ctx;
  uint32_t now_us = 0U;
  uint16_t index;

  bridge_core_init(&ctx);
  feed_host(&ctx, kRequest03, sizeof(kRequest03), &now_us, CHAR_US);
  bridge_core_tx_complete(&ctx, now_us);

  for (index = 0U; index < 2U; index++)
  {
    bridge_core_on_rs485_byte(&ctx, kResponse03[index], now_us);
    now_us += CHAR_US;
  }

  now_us += BOARD_B_FRAME_GAP_US;
  feed_rs485(&ctx, kResponse03, sizeof(kResponse03), &now_us, CHAR_US);

  CHECK(ctx.state == BRIDGE_STATE_SEND_RESPONSE);
  CHECK(ctx.stats.incomplete_responses == 1U);
  CHECK(ctx.stats.rs485_gap_resyncs == 1U);
  expect_tx(&ctx, kResponse03, sizeof(kResponse03));
}

static void test_response_deadline_is_not_extended_by_partials(void)
{
  bridge_core_t ctx;
  uint32_t now_us = 0U;
  uint32_t sent_at;

  bridge_core_init(&ctx);
  feed_host(&ctx, kRequest03, sizeof(kRequest03), &now_us, CHAR_US);
  sent_at = now_us;
  bridge_core_tx_complete(&ctx, sent_at);

  bridge_core_on_rs485_byte(&ctx, 0x01U, sent_at + 100000U);
  bridge_core_on_rs485_byte(&ctx, 0x03U, sent_at + 101146U);
  bridge_core_on_rs485_byte(&ctx, 0x04U, sent_at + 500000U);
  CHECK(ctx.stats.incomplete_responses == 1U);

  bridge_core_tick(&ctx, sent_at + BOARD_B_RESPONSE_TIMEOUT_US - 1U);
  CHECK(ctx.state == BRIDGE_STATE_WAIT_RESPONSE);
  bridge_core_tick(&ctx, sent_at + BOARD_B_RESPONSE_TIMEOUT_US);
  CHECK(ctx.state == BRIDGE_STATE_RECOVERY);
  CHECK(ctx.stats.response_timeouts == 1U);
}

static void test_partial_frame_ticks_do_not_double_count(void)
{
  bridge_core_t ctx;
  uint32_t now_us = 0U;
  uint8_t partial[3] = {0x01U, 0x03U, 0x00U};
  uint32_t index;

  bridge_core_init(&ctx);
  feed_host(&ctx, partial, sizeof(partial), &now_us, CHAR_US);

  for (index = 0U; index < 20U; index++)
  {
    now_us += 10000U;
    bridge_core_tick(&ctx, now_us);
    CHECK(ctx.state == BRIDGE_STATE_RECEIVE_REQUEST);
  }
  CHECK(ctx.stats.incomplete_requests == 0U);
  CHECK(ctx.stats.receive_timeouts == 0U);

  now_us += BOARD_B_FRAME_GAP_US;
  feed_host(&ctx, kRequest03, sizeof(kRequest03), &now_us, CHAR_US);
  CHECK(ctx.stats.incomplete_requests == 1U);
  CHECK(ctx.stats.receive_timeouts == 0U);
  expect_tx(&ctx, kRequest03, sizeof(kRequest03));
}

static void test_timestamps_wrap_safely(void)
{
  bridge_core_t ctx;
  uint32_t now_us = 0xFFFFF000U;
  uint8_t partial[3] = {0x01U, 0x03U, 0x00U};

  bridge_core_init(&ctx);
  feed_host(&ctx, partial, sizeof(partial), &now_us, CHAR_US);
  now_us += 20000U; /* crosses the 2^32 us wrap */
  feed_host(&ctx, kRequest03, sizeof(kRequest03), &now_us, CHAR_US);

  CHECK(ctx.state == BRIDGE_STATE_SEND_REQUEST);
  CHECK(ctx.stats.incomplete_requests == 1U);
  CHECK(ctx.stats.host_gap_resyncs == 1U);
  expect_tx(&ctx, kRequest03, sizeof(kRequest03));

  bridge_core_tx_complete(&ctx, now_us);
  feed_rs485(&ctx, kResponse03, sizeof(kResponse03), &now_us, CHAR_US);
  CHECK(ctx.state == BRIDGE_STATE_SEND_RESPONSE);
  expect_tx(&ctx, kResponse03, sizeof(kResponse03));
}

static void test_counters_on_one_transaction(void)
{
  bridge_core_t ctx;
  uint32_t now_us = 0U;

  bridge_core_init(&ctx);
  feed_host(&ctx, kRequest06, sizeof(kRequest06), &now_us, CHAR_US);
  bridge_core_tx_complete(&ctx, now_us);
  feed_rs485(&ctx, kResponse06, sizeof(kResponse06), &now_us, CHAR_US);
  bridge_core_tx_complete(&ctx, now_us);

  CHECK(ctx.stats.host_rx_bytes == sizeof(kRequest06));
  CHECK(ctx.stats.rs485_rx_bytes == sizeof(kResponse06));
  CHECK(ctx.stats.request_frames_completed == 1U);
  CHECK(ctx.stats.request_frames_forwarded == 1U);
  CHECK(ctx.stats.response_frames_completed == 1U);
  CHECK(ctx.stats.response_frames_forwarded == 1U);
  CHECK(ctx.stats.unknown_request_frames == 0U);
  CHECK(ctx.stats.host_gap_resyncs == 0U);
  CHECK(ctx.stats.rs485_gap_resyncs == 0U);
  CHECK(ctx.stats.host_max_frame_gap_us == CHAR_US);
  CHECK(ctx.stats.oversize_frames == 0U);
  CHECK(ctx.stats.incomplete_requests == 0U);
  CHECK(ctx.stats.incomplete_responses == 0U);
  CHECK(ctx.stats.receive_timeouts == 0U);
  CHECK(ctx.stats.response_timeouts == 0U);
  CHECK(ctx.stats.response_mismatches == 0U);
  CHECK(ctx.stats.host_bytes_dropped == 0U);
  CHECK(ctx.stats.rs485_bytes_dropped == 0U);
  CHECK(ctx.stats.late_response_bytes == 0U);
  CHECK(ctx.stats.unsolicited_rs485_bytes == 0U);
  CHECK(ctx.stats.recovery_entries == 0U);
  CHECK(ctx.stats.crc_bypass_frames == 2U);
}

int main(void)
{
  test_single_frame_is_byte_transparent();
  test_broadcast_and_bad_crc_are_forwarded_transparently();
  test_fragmentation_within_frame_gap_keeps_frame();
  test_address_only_prefix_resynchronises();
  test_truncated_prefix_resynchronises();
  test_frame_gap_boundary();
  test_write_multiple_partial_header_resynchronises();
  test_short_unknown_and_oversize_frames();
  test_sticky_frames_are_not_merged();
  test_success_response_returns_to_idle_and_accepts_fast_next_request();
  test_idle_late_rs485_does_not_pollute_next_transaction();
  test_maximum_supported_adu_size_is_forwarded();
  test_response_timeout_and_late_response_do_not_pollute_next_transaction();
  test_exception_response_is_five_bytes();
  test_write_multiple_response_is_eight_bytes();
  test_unknown_request_and_response_use_gap_boundary();
  test_partial_response_resynchronises();
  test_response_deadline_is_not_extended_by_partials();
  test_partial_frame_ticks_do_not_double_count();
  test_timestamps_wrap_safely();
  test_counters_on_one_transaction();

  if (g_failures != 0)
  {
    fprintf(stderr, "board_b bridge core host tests: %d failure(s)\n", g_failures);
    return 1;
  }

  printf("board_b bridge core host tests: PASS\n");
  return 0;
}
