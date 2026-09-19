/*
 * Host tests for the receive queue and the event-ordered dispatch.
 *
 * These exercise the production queue, dispatch and bridge core together: the
 * point is that arrival timestamps survive a batch dequeue, that timeouts are
 * judged at the event's arrival time rather than at the time the main loop got
 * around to it, and that a queue overflow invalidates the frame instead of
 * assembling a spliced one.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_b_rx_queue.h"
#include "bridge_core.h"
#include "bridge_dispatch.h"

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

#define CHAR_US 1146U

static const uint8_t kRequest03[] = {0x01U, 0x03U, 0x00U, 0x00U, 0x00U,
                                     0x02U, 0xDEU, 0xADU};
static const uint8_t kRequest06[] = {0x01U, 0x06U, 0x00U, 0x10U, 0x12U,
                                     0x34U, 0xCAU, 0xFEU};
static const uint8_t kResponse03[] = {0x01U, 0x03U, 0x04U, 0x11U, 0x22U,
                                      0x33U, 0x44U, 0xFEU, 0xEDU};

static void push_bytes(board_b_rx_queue_t *queue, const uint8_t *data,
                       uint16_t length, uint32_t *now_us, uint32_t step_us)
{
  uint16_t index;

  for (index = 0U; index < length; index++)
  {
    CHECK(board_b_rx_queue_push(queue, data[index], *now_us) != 0);
    *now_us += step_us;
  }
}

static void expect_tx(const bridge_core_t *core, const uint8_t *expected,
                      uint16_t expected_length)
{
  const uint8_t *actual = NULL;
  uint16_t actual_length = 0U;
  int ready = bridge_core_get_tx(core, &actual, &actual_length);

  CHECK(ready != 0);
  CHECK(actual_length == expected_length);
  if ((ready != 0) && (actual != NULL) && (actual_length == expected_length))
  {
    CHECK(memcmp(actual, expected, expected_length) == 0);
  }
}

static void test_queue_keeps_timestamps_and_reports_overflow(void)
{
  board_b_rx_queue_t queue;
  board_b_rx_event_t event;
  uint32_t index;
  uint32_t lost_at_us = 0U;

  board_b_rx_queue_init(&queue);

  CHECK(board_b_rx_queue_push(&queue, 0x11U, 1234U) != 0);
  CHECK(board_b_rx_queue_peek(&queue, &event) != 0);
  CHECK(event.byte == 0x11U);
  CHECK(event.arrival_us == 1234U);
  CHECK(board_b_rx_queue_pop(&queue, &event) != 0);
  CHECK(board_b_rx_queue_empty(&queue) != 0);

  for (index = 0U; index < (BOARD_B_RX_QUEUE_SLOTS - 1U); index++)
  {
    CHECK(board_b_rx_queue_push(&queue, (uint8_t)index, index * 100U) != 0);
  }
  CHECK(board_b_rx_queue_push(&queue, 0xEEU, 77777U) == 0);
  CHECK(board_b_rx_queue_dropped(&queue) == 1U);
  CHECK(board_b_rx_queue_take_continuity_lost(&queue, &lost_at_us) != 0);
  CHECK(lost_at_us == 77777U);
  CHECK(board_b_rx_queue_take_continuity_lost(&queue, &lost_at_us) == 0);
}

static void test_batched_drain_keeps_arrival_times(void)
{
  bridge_core_t core;
  board_b_rx_queue_t host_queue;
  board_b_rx_queue_t rs485_queue;
  uint32_t now_us = 0U;
  uint8_t partial[3] = {0x01U, 0x03U, 0x00U};

  bridge_core_init(&core);
  board_b_rx_queue_init(&host_queue);
  board_b_rx_queue_init(&rs485_queue);

  push_bytes(&host_queue, partial, sizeof(partial), &now_us, CHAR_US);
  now_us += 20000U;
  push_bytes(&host_queue, kRequest03, sizeof(kRequest03), &now_us, CHAR_US);

  /* The main loop drains everything long after the bytes arrived. */
  bridge_dispatch_process(&core, &host_queue, &rs485_queue, now_us + 500000U);

  CHECK(core.state == BRIDGE_STATE_SEND_REQUEST);
  CHECK(core.stats.incomplete_requests == 1U);
  CHECK(core.stats.host_gap_resyncs == 1U);
  CHECK(core.stats.host_bytes_dropped == 0U);
  expect_tx(&core, kRequest03, sizeof(kRequest03));
}

static void test_response_within_deadline_survives_late_drain(void)
{
  bridge_core_t core;
  board_b_rx_queue_t host_queue;
  board_b_rx_queue_t rs485_queue;
  uint32_t now_us = 0U;
  uint32_t sent_at;
  uint32_t response_us;

  bridge_core_init(&core);
  board_b_rx_queue_init(&host_queue);
  board_b_rx_queue_init(&rs485_queue);

  push_bytes(&host_queue, kRequest03, sizeof(kRequest03), &now_us, CHAR_US);
  bridge_dispatch_process(&core, &host_queue, &rs485_queue, now_us);
  CHECK(core.state == BRIDGE_STATE_SEND_REQUEST);
  sent_at = now_us;
  bridge_core_tx_complete(&core, sent_at);

  /* The response arrives at 900 ms and is only consumed at 1100 ms. */
  response_us = sent_at + 900000U;
  push_bytes(&rs485_queue, kResponse03, sizeof(kResponse03), &response_us,
             CHAR_US);
  bridge_dispatch_process(&core, &host_queue, &rs485_queue, sent_at + 1100000U);

  CHECK(core.state == BRIDGE_STATE_SEND_RESPONSE);
  CHECK(core.stats.response_timeouts == 0U);
  CHECK(core.stats.late_response_bytes == 0U);
  expect_tx(&core, kResponse03, sizeof(kResponse03));
}

static void test_response_after_deadline_is_rejected_by_arrival_time(void)
{
  bridge_core_t core;
  board_b_rx_queue_t host_queue;
  board_b_rx_queue_t rs485_queue;
  uint32_t now_us = 0U;
  uint32_t sent_at;
  uint32_t response_us;

  bridge_core_init(&core);
  board_b_rx_queue_init(&host_queue);
  board_b_rx_queue_init(&rs485_queue);

  push_bytes(&host_queue, kRequest03, sizeof(kRequest03), &now_us, CHAR_US);
  bridge_dispatch_process(&core, &host_queue, &rs485_queue, now_us);
  sent_at = now_us;
  bridge_core_tx_complete(&core, sent_at);

  /* First response byte arrives 1 us after the 1000 ms deadline. */
  response_us = sent_at + BOARD_B_RESPONSE_TIMEOUT_US + 1U;
  push_bytes(&rs485_queue, kResponse03, sizeof(kResponse03), &response_us,
             CHAR_US);
  bridge_dispatch_process(&core, &host_queue, &rs485_queue,
                          sent_at + BOARD_B_RESPONSE_TIMEOUT_US + 20000U);

  CHECK(core.state == BRIDGE_STATE_RECOVERY);
  CHECK(core.stats.response_timeouts == 1U);
  CHECK(core.stats.late_response_bytes == sizeof(kResponse03));
  CHECK(core.stats.response_frames_forwarded == 0U);
}

static void test_stale_rs485_byte_before_request_is_unsolicited(void)
{
  bridge_core_t core;
  board_b_rx_queue_t host_queue;
  board_b_rx_queue_t rs485_queue;
  uint32_t now_us = 0U;

  bridge_core_init(&core);
  board_b_rx_queue_init(&host_queue);
  board_b_rx_queue_init(&rs485_queue);

  CHECK(board_b_rx_queue_push(&rs485_queue, 0xAAU, now_us) != 0);
  now_us += 1000U;
  push_bytes(&host_queue, kRequest03, sizeof(kRequest03), &now_us, CHAR_US);

  bridge_dispatch_process(&core, &host_queue, &rs485_queue, now_us);

  CHECK(core.state == BRIDGE_STATE_SEND_REQUEST);
  CHECK(core.stats.unsolicited_rs485_bytes == 1U);
  CHECK(core.stats.late_response_bytes == 0U);
  expect_tx(&core, kRequest03, sizeof(kRequest03));
}

static void test_clock_never_moves_backwards(void)
{
  bridge_core_t core;
  board_b_rx_queue_t host_queue;
  board_b_rx_queue_t rs485_queue;
  uint32_t now_us = 100000U;

  bridge_core_init(&core);
  board_b_rx_queue_init(&host_queue);
  board_b_rx_queue_init(&rs485_queue);

  push_bytes(&host_queue, kRequest06, sizeof(kRequest06), &now_us, CHAR_US);

  /* A caller that sampled the clock before the bytes arrived must not make the
     core see a huge negative interval. */
  bridge_dispatch_process(&core, &host_queue, &rs485_queue, 50000U);

  CHECK(core.stats.host_gap_resyncs == 0U);
  CHECK(core.state == BRIDGE_STATE_SEND_REQUEST);
  expect_tx(&core, kRequest06, sizeof(kRequest06));
}

static void test_continuity_loss_hook_drops_partial_request(void)
{
  bridge_core_t core;
  uint32_t now_us = 0U;

  bridge_core_init(&core);
  bridge_core_on_host_byte(&core, 0x01U, now_us);
  bridge_core_on_host_byte(&core, 0x03U, now_us + CHAR_US);
  CHECK(core.state == BRIDGE_STATE_RECEIVE_REQUEST);

  bridge_core_on_host_continuity_lost(&core, now_us + 2U * CHAR_US);

  CHECK(core.stats.incomplete_requests == 1U);
  CHECK(core.request_length == 0U);
  CHECK(core.state == BRIDGE_STATE_IDLE);
}

static void test_continuity_loss_hook_keeps_response_deadline(void)
{
  bridge_core_t core;
  uint32_t now_us = 0U;
  uint32_t sent_at;

  bridge_core_init(&core);
  bridge_core_on_host_byte(&core, kRequest03[0U], now_us);
  bridge_core_on_host_byte(&core, kRequest03[1U], now_us + CHAR_US);
  bridge_core_on_host_byte(&core, kRequest03[2U], now_us + 2U * CHAR_US);
  bridge_core_on_host_byte(&core, kRequest03[3U], now_us + 3U * CHAR_US);
  bridge_core_on_host_byte(&core, kRequest03[4U], now_us + 4U * CHAR_US);
  bridge_core_on_host_byte(&core, kRequest03[5U], now_us + 5U * CHAR_US);
  bridge_core_on_host_byte(&core, kRequest03[6U], now_us + 6U * CHAR_US);
  bridge_core_on_host_byte(&core, kRequest03[7U], now_us + 7U * CHAR_US);
  sent_at = now_us + 8U * CHAR_US;
  bridge_core_tx_complete(&core, sent_at);

  bridge_core_on_rs485_byte(&core, 0x01U, sent_at + CHAR_US);
  bridge_core_on_rs485_byte(&core, 0x03U, sent_at + 2U * CHAR_US);
  CHECK(core.response_length == 2U);

  bridge_core_on_rs485_continuity_lost(&core, sent_at + 3U * CHAR_US);

  CHECK(core.stats.incomplete_responses == 1U);
  CHECK(core.response_length == 0U);
  CHECK(core.state == BRIDGE_STATE_WAIT_RESPONSE);
  CHECK(core.wait_started_us == sent_at);

  bridge_core_tick(&core, sent_at + BOARD_B_RESPONSE_TIMEOUT_US);
  CHECK(core.state == BRIDGE_STATE_RECOVERY);
}

static void test_queue_continuity_loss_ends_discarding(void)
{
  bridge_core_t core;
  board_b_rx_queue_t host_queue;
  board_b_rx_queue_t rs485_queue;
  uint32_t now_us = 0U;
  uint8_t oversize_header[7] = {0x01U, 0x10U, 0x00U, 0x00U,
                                0x00U, 0x01U, 0xFFU};
  uint32_t filler = 0U;

  bridge_core_init(&core);
  board_b_rx_queue_init(&host_queue);
  board_b_rx_queue_init(&rs485_queue);

  push_bytes(&host_queue, oversize_header, sizeof(oversize_header), &now_us,
             CHAR_US);
  while (board_b_rx_queue_push(&host_queue, 0x55U, now_us) != 0)
  {
    filler++;
    now_us += CHAR_US;
  }
  CHECK(filler + sizeof(oversize_header) == BOARD_B_RX_QUEUE_SLOTS - 1U);
  CHECK(board_b_rx_queue_dropped(&host_queue) == 1U);

  bridge_dispatch_process(&core, &host_queue, &rs485_queue, now_us);

  CHECK(core.stats.oversize_frames == 1U);
  CHECK(core.request_length == 0U);
  CHECK(core.request_discarding == 0U);
  CHECK(core.state == BRIDGE_STATE_IDLE);

  push_bytes(&host_queue, kRequest06, sizeof(kRequest06), &now_us, CHAR_US);
  bridge_dispatch_process(&core, &host_queue, &rs485_queue, now_us);
  CHECK(core.state == BRIDGE_STATE_SEND_REQUEST);
  expect_tx(&core, kRequest06, sizeof(kRequest06));
}

int main(void)
{
  test_queue_keeps_timestamps_and_reports_overflow();
  test_batched_drain_keeps_arrival_times();
  test_response_within_deadline_survives_late_drain();
  test_response_after_deadline_is_rejected_by_arrival_time();
  test_stale_rs485_byte_before_request_is_unsolicited();
  test_clock_never_moves_backwards();
  test_continuity_loss_hook_drops_partial_request();
  test_continuity_loss_hook_keeps_response_deadline();
  test_queue_continuity_loss_ends_discarding();

  if (g_failures != 0)
  {
    fprintf(stderr, "board_b bridge dispatch host tests: %d failure(s)\n",
            g_failures);
    return 1;
  }

  printf("board_b bridge dispatch host tests: PASS\n");
  return 0;
}
