#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_a_monotonic.h"
#include "board_a_runtime.h"
#include "board_a_rx_recovery.h"
#include "board_a_tx.h"
#include "modbus_crc.h"
#include "modbus_rtu_rx.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static unsigned int g_checks;
static unsigned int g_failures;

static void check_true(int condition, const char *expression, int line)
{
  g_checks++;
  if (!condition) {
    g_failures++;
    printf("FAIL line %d: %s\n", line, expression);
  }
}

#define CHECK(condition) check_true((condition) != 0, #condition, __LINE__)

typedef struct {
  pthread_mutex_t mutex;
  uint64_t now_us;
} fake_lock_t;

static bool fake_lock(void *context)
{
  fake_lock_t *lock = (fake_lock_t *)context;

  return pthread_mutex_lock(&lock->mutex) == 0;
}

static void fake_unlock(void *context)
{
  fake_lock_t *lock = (fake_lock_t *)context;

  (void)pthread_mutex_unlock(&lock->mutex);
}

static uint64_t fake_now_us(void *context)
{
  return ((fake_lock_t *)context)->now_us;
}

static const board_a_runtime_ops_t FAKE_OPS = {
  fake_lock,
  fake_unlock,
  fake_now_us
};

static size_t append_crc(uint8_t *frame, size_t length)
{
  uint16_t crc = modbus_crc16(frame, length);

  frame[length] = (uint8_t)(crc & 0xFFU);
  frame[length + 1U] = (uint8_t)(crc >> 8U);
  return length + 2U;
}

static void put_u16_be(uint8_t *frame, size_t offset, uint16_t value)
{
  frame[offset] = (uint8_t)(value >> 8U);
  frame[offset + 1U] = (uint8_t)value;
}

static size_t make_write_single(uint8_t *frame,
                                uint16_t address,
                                uint16_t value)
{
  frame[0] = BOARD_A_SLAVE_ADDRESS;
  frame[1] = 0x06U;
  put_u16_be(frame, 2U, address);
  put_u16_be(frame, 4U, value);
  return append_crc(frame, 6U);
}

static size_t make_write_multiple(uint8_t *frame,
                                  uint16_t start,
                                  const uint16_t *values,
                                  uint16_t quantity)
{
  size_t index;

  frame[0] = BOARD_A_SLAVE_ADDRESS;
  frame[1] = 0x10U;
  put_u16_be(frame, 2U, start);
  put_u16_be(frame, 4U, quantity);
  frame[6] = (uint8_t)(quantity * 2U);
  for (index = 0U; index < quantity; ++index) {
    put_u16_be(frame, 7U + (index * 2U), values[index]);
  }
  return append_crc(frame, 7U + ((size_t)quantity * 2U));
}

static size_t runtime_exchange(board_a_runtime_t *runtime,
                               const uint8_t *request,
                               size_t request_length,
                               uint8_t *response,
                               size_t response_capacity)
{
  uint32_t now_us = 1000U;
  size_t index;

  for (index = 0U; index < request_length; ++index) {
    board_a_runtime_push_byte(runtime, request[index], now_us);
    now_us += 100U;
  }
  return board_a_runtime_poll(runtime, now_us + 5000U, response,
                              response_capacity);
}

static bool runtime_command(board_a_runtime_t *runtime, uint16_t command)
{
  uint8_t request[8];
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  size_t request_length = make_write_single(request, BOARD_A_HOLDING_COMMAND,
                                            command);
  size_t response_length = runtime_exchange(runtime, request, request_length,
                                            response, sizeof(response));

  return (response_length == request_length) &&
         (memcmp(response, request, request_length) == 0);
}

static bool runtime_finish_drain(board_a_runtime_t *runtime)
{
  board_a_persistence_status_t status;

  return board_a_runtime_persistence_status(runtime, &status) &&
         (status.drain_state == BOARD_A_DRAIN_PENDING) &&
         (board_a_runtime_complete_drain(runtime, status.drain_generation,
                                         1),
          true);
}

static bool runtime_submit_single(board_a_runtime_t *runtime, uint32_t id)
{
  uint8_t request[64];
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  uint16_t values[2];
  size_t request_length;

  values[0] = (uint16_t)(id >> 16U);
  values[1] = (uint16_t)id;
  request_length = make_write_multiple(request,
                                       BOARD_A_HOLDING_COMMAND_ID_HI,
                                       values, 2U);
  if (runtime_exchange(runtime, request, request_length, response,
                       sizeof(response)) != 8U) {
    return false;
  }
  return runtime_command(runtime, BOARD_A_COMMAND_SINGLE);
}

static size_t make_read_request(uint8_t *frame,
                                uint8_t function,
                                uint16_t address,
                                uint16_t quantity)
{
  frame[0] = BOARD_A_SLAVE_ADDRESS;
  frame[1] = function;
  put_u16_be(frame, 2U, address);
  put_u16_be(frame, 4U, quantity);
  return append_crc(frame, 6U);
}

static bool runtime_read_block(board_a_runtime_t *runtime,
                               uint8_t function,
                               uint16_t address,
                               uint16_t quantity,
                               uint16_t *values)
{
  uint8_t request[8];
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  size_t request_length = make_read_request(request, function, address,
                                            quantity);
  size_t response_length = runtime_exchange(runtime, request, request_length,
                                            response, sizeof(response));
  uint16_t received_crc;
  size_t index;

  if (response_length != (5U + ((size_t)quantity * 2U))) {
    return false;
  }
  received_crc = (uint16_t)((uint16_t)response[response_length - 2U] |
                            ((uint16_t)response[response_length - 1U] << 8U));
  if ((response[0] != BOARD_A_SLAVE_ADDRESS) ||
      (response[1] != function) ||
      (response[2] != (uint8_t)(quantity * 2U)) ||
      (received_crc != modbus_crc16(response, response_length - 2U))) {
    return false;
  }
  for (index = 0U; index < quantity; ++index) {
    values[index] =
        (uint16_t)(((uint16_t)response[3U + (index * 2U)] << 8U) |
                   (uint16_t)response[4U + (index * 2U)]);
  }
  return true;
}

static bool runtime_read_u16(board_a_runtime_t *runtime,
                             uint8_t function,
                             uint16_t address,
                             uint16_t *value)
{
  return runtime_read_block(runtime, function, address, 1U, value);
}

static bool runtime_read_u32(board_a_runtime_t *runtime,
                             uint8_t function,
                             uint16_t hi_address,
                             uint32_t *value)
{
  uint16_t words[2];

  if (!runtime_read_block(runtime, function, hi_address, 2U, words)) {
    return false;
  }
  *value = ((uint32_t)words[0] << 16U) | (uint32_t)words[1];
  return true;
}

static bool runtime_expect_exception(board_a_runtime_t *runtime,
                                     const uint8_t *request,
                                     size_t request_length,
                                     uint8_t expected_exception)
{
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  size_t response_length = runtime_exchange(runtime, request, request_length,
                                            response, sizeof(response));

  return (response_length == 5U) &&
      (response[0] == BOARD_A_SLAVE_ADDRESS) &&
      (response[1] == (uint8_t)(request[1] | 0x80U)) &&
      (response[2] == expected_exception);
}

static bool runtime_write_pending_seconds(board_a_runtime_t *runtime,
                                          uint16_t hi_address,
                                          uint32_t seconds)
{
  uint8_t request[64];
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  uint16_t values[2];
  size_t request_length;

  values[0] = (uint16_t)(seconds >> 16U);
  values[1] = (uint16_t)seconds;
  request_length = make_write_multiple(request, hi_address, values, 2U);
  return runtime_exchange(runtime, request, request_length, response,
                          sizeof(response)) == 8U;
}

static bool runtime_command_expect_exception(board_a_runtime_t *runtime,
                                             uint16_t command,
                                             uint8_t expected_exception)
{
  uint8_t request[8];
  size_t request_length = make_write_single(request, BOARD_A_HOLDING_COMMAND,
                                            command);

  return runtime_expect_exception(runtime, request, request_length,
                                  expected_exception);
}

static void p3b_runtime_setup(board_a_runtime_t *runtime,
                              fake_lock_t *context,
                              uint32_t session_id)
{
  memset(context, 0, sizeof(*context));
  CHECK(pthread_mutex_init(&context->mutex, NULL) == 0);
  board_a_runtime_init(runtime, session_id, &FAKE_OPS, context);
}

/* Calibrate the software UTC clock at one controllable monotonic instant. */
static bool p3b_set_time(board_a_runtime_t *runtime,
                         fake_lock_t *context,
                         uint64_t now_us,
                         uint32_t utc_seconds)
{
  context->now_us = now_us;
  if (!runtime_write_pending_seconds(
          runtime, BOARD_A_HOLDING_PENDING_UTC_SECONDS_HI, utc_seconds)) {
    return false;
  }
  return runtime_command(runtime, BOARD_A_COMMAND_SET_TIME);
}

static bool p3b_arm_start(board_a_runtime_t *runtime,
                          fake_lock_t *context,
                          uint64_t now_us,
                          uint32_t target_seconds)
{
  context->now_us = now_us;
  if (!runtime_write_pending_seconds(
          runtime, BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_HI,
          target_seconds)) {
    return false;
  }
  return runtime_command(runtime, BOARD_A_COMMAND_ARM_START);
}

static void test_monotonic_wrap(void)
{
  board_a_monotonic_t clock = {0};

  CHECK(board_a_monotonic_update(&clock, 0xFFFFFFF0U) == 0U);
  CHECK(board_a_monotonic_update(&clock, 0xFFFFFFF8U) == 8U);
  CHECK(board_a_monotonic_update(&clock, 0x00000010U) == 32U);
  CHECK(board_a_monotonic_value(&clock) == 32U);
}

static void test_monotonic_ms_across_tim2_wrap(void)
{
  board_a_monotonic_t clock = {0};
  uint32_t deadline_ms;

  /* 4294.0 s: three milliseconds before the 32-bit microsecond wrap. */
  board_a_monotonic_init(&clock, 4294000000U);
  CHECK(board_a_monotonic_ms(&clock) == 0U);

  /* Three seconds later the raw counter wrapped; the extended clock must not. */
  CHECK(board_a_monotonic_update(&clock, 2032704U) == 3000000U);
  CHECK(board_a_monotonic_ms(&clock) == 3000U);

  deadline_ms = 2000U;
  CHECK((int32_t)(board_a_monotonic_ms(&clock) - deadline_ms) >= 0);
  CHECK((int32_t)(1000U - deadline_ms) < 0);
}

static void test_deadline_predicate_across_ms_wrap(void)
{
  board_a_monotonic_t clock = {0};

  CHECK(!board_a_deadline_expired(0xFFFFFF00U, 0x00000100U));
  CHECK(board_a_deadline_expired(0x00000100U, 0x00000100U));
  CHECK(board_a_deadline_expired(0x00000100U, 0xFFFFFF00U));
  CHECK(board_a_deadline_expired(1000U, 1000U));
  CHECK(!board_a_deadline_expired(999U, 1000U));

  clock.accumulated_us = 4294967295000ULL; /* 4294967295 ms */
  clock.initialized = true;
  CHECK(board_a_monotonic_ms(&clock) == 0xFFFFFFFFU);
  CHECK(board_a_monotonic_update(&clock, 2000000U) == 4294969295000ULL);
  CHECK(board_a_monotonic_ms(&clock) == 1999U); /* natural u32 ms wrap */
  CHECK(board_a_deadline_expired(board_a_monotonic_ms(&clock), 1000U));
  CHECK(!board_a_deadline_expired(board_a_monotonic_ms(&clock), 3000U));
}

static void test_tx_state_machine(void)
{
  static const uint8_t data[] = {0x11U, 0x22U, 0x33U};
  board_a_tx_t tx;
  uint8_t byte;

  board_a_tx_init(&tx);
  CHECK(board_a_tx_start(&tx, data, sizeof(data), 100U, 1000U));
  CHECK(board_a_tx_on_txe(&tx, &byte) && byte == 0x11U);
  CHECK(board_a_tx_on_txe(&tx, &byte) && byte == 0x22U);
  CHECK(board_a_tx_on_txe(&tx, &byte) && byte == 0x33U);
  CHECK(tx.state == BOARD_A_TX_WAIT_TC);
  CHECK(!board_a_tx_on_txe(&tx, &byte));
  CHECK(!board_a_tx_poll_timeout(&tx, 1099U));
  CHECK(board_a_tx_on_tc(&tx));
  CHECK(tx.state == BOARD_A_TX_COMPLETE);
  CHECK(board_a_tx_take_completion(&tx));
  CHECK(tx.state == BOARD_A_TX_IDLE);

  CHECK(board_a_tx_start(&tx, data, sizeof(data), 200U, 100U));
  CHECK(board_a_tx_poll_timeout(&tx, 300U));
  CHECK(tx.state == BOARD_A_TX_TIMED_OUT);
  CHECK(board_a_tx_take_timeout(&tx));
  CHECK(tx.state == BOARD_A_TX_IDLE);

  CHECK(board_a_tx_start(&tx, data, sizeof(data), 400U, 100U));
  CHECK(board_a_tx_abort(&tx));
  CHECK(tx.state == BOARD_A_TX_IDLE);
  CHECK(!board_a_tx_on_tc(&tx));
}

static void test_rx_recovery(void)
{
  board_a_rx_recovery_t recovery;
  modbus_rtu_rx_t receiver;

  board_a_rx_recovery_init(&recovery, 4011U);
  board_a_rx_recovery_begin(&recovery, 10000U);
  CHECK(board_a_rx_recovery_is_discarding(&recovery));
  CHECK(!board_a_rx_recovery_accept(&recovery, 10001U));
  CHECK(!board_a_rx_recovery_accept(&recovery, 14010U));
  CHECK(board_a_rx_recovery_accept(&recovery, 18022U));
  CHECK(!board_a_rx_recovery_is_discarding(&recovery));

  board_a_rx_recovery_begin(&recovery, 0xFFFFFF00U);
  CHECK(!board_a_rx_recovery_accept(&recovery, 0xFFFFFF10U));
  CHECK(!board_a_rx_recovery_accept(&recovery, 0x00000000U));
  CHECK(board_a_rx_recovery_accept(&recovery, 0x00000FADU));

  board_a_rx_recovery_begin(&recovery, 0U);
  CHECK(!board_a_rx_recovery_can_poll(&recovery, 4010U));
  CHECK(board_a_rx_recovery_can_poll(&recovery, 4011U));

  modbus_rtu_rx_init(&receiver, 4011U);
  modbus_rtu_rx_push(&receiver, 0x01U, 10U);
  modbus_rtu_rx_push(&receiver, 0x02U, 20U);
  receiver.overlong_frames = 3U;
  receiver.frames_ready = 4U;
  modbus_rtu_rx_discard(&receiver);
  CHECK(receiver.active_length == 0U);
  CHECK(!receiver.ready_valid);
  CHECK(receiver.overlong_frames == 3U);
  CHECK(receiver.frames_ready == 4U);
}

static void test_model_commands_and_scheduler(void)
{
  fake_lock_t context;
  board_a_runtime_t runtime;
  board_a_runtime_status_t status;
  board_a_snapshot_t snapshot;
  uint8_t request[64];
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  uint16_t config[3] = {10U, 0x000FU, 2U};
  size_t request_length;

  memset(&context, 0, sizeof(context));
  CHECK(pthread_mutex_init(&context.mutex, NULL) == 0);
  board_a_runtime_init(&runtime, 1U, &FAKE_OPS, &context);

  request_length = make_write_multiple(request, BOARD_A_HOLDING_CFG_PERIOD_SEC,
                                       config, 3U);
  CHECK(runtime_exchange(&runtime, request, request_length, response,
                         sizeof(response)) == 8U);
  CHECK(runtime_command(&runtime, BOARD_A_COMMAND_APPLY_CONFIG));
  CHECK(runtime_command(&runtime, BOARD_A_COMMAND_START));
  board_a_runtime_tick(&runtime, 1000000ULL);
  board_a_runtime_tick(&runtime, 11000000ULL);
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.sequence == 2U);
  CHECK(status.run_state == BOARD_A_RUN_STOPPED);
  CHECK(status.records_this_run == 2U);

  CHECK(runtime_command(&runtime, BOARD_A_COMMAND_STOP));
  CHECK(runtime_finish_drain(&runtime));
  board_a_runtime_tick(&runtime, 21000000ULL);
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.sequence == 2U);

  CHECK(runtime_submit_single(&runtime, 0x00000001U));
  CHECK(runtime_submit_single(&runtime, 0x00000001U));
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.sequence == 3U);
  CHECK(status.last_command == BOARD_A_COMMAND_SINGLE);
  CHECK(status.command_result == BOARD_A_COMMAND_RESULT_DUPLICATE);

  CHECK(board_a_runtime_read_snapshot(&runtime, &snapshot));
  CHECK(snapshot.valid);
  CHECK(snapshot.sequence == 3U);
  CHECK(snapshot.channel_count == 4U);
  CHECK(snapshot.channel_values[0] == 30U);
  CHECK(snapshot.channel_values[1] == 31U);
  CHECK(snapshot.channel_values[2] == 32U);
  CHECK(snapshot.channel_values[3] == 33U);

  pthread_mutex_destroy(&context.mutex);
}

#define P3B_ANCHOR_US 1000000ULL
#define P3B_UTC 1000U

static bool runtime_write_holding_single(board_a_runtime_t *runtime,
                                         uint16_t address,
                                         uint16_t value)
{
  uint8_t request[8];
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  size_t request_length = make_write_single(request, address, value);

  return runtime_exchange(runtime, request, request_length, response,
                          sizeof(response)) == request_length;
}

static bool runtime_write_multiple_config(board_a_runtime_t *runtime,
                                          const uint16_t *values,
                                          uint16_t quantity)
{
  uint8_t request[64];
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  size_t request_length = make_write_multiple(
      request, BOARD_A_HOLDING_CFG_PERIOD_SEC, values, quantity);

  return runtime_exchange(runtime, request, request_length, response,
                          sizeof(response)) == 8U;
}

/* T01: uncalibrated/past/invalid targets are rejected without partial state. */
static void test_t01_rejection_paths(void)
{
  fake_lock_t context;
  board_a_runtime_t runtime;
  board_a_runtime_status_t status;
  uint8_t request[64];
  size_t request_length;
  uint16_t value;
  uint32_t value32;

  p3b_runtime_setup(&runtime, &context, 3U);

  /* Uncalibrated ARM_START: 0x04 and no state change at all. */
  CHECK(runtime_write_pending_seconds(
      &runtime, BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_HI, P3B_UTC + 5U));
  context.now_us = P3B_ANCHOR_US;
  request_length = make_write_single(request, BOARD_A_HOLDING_COMMAND,
                                     BOARD_A_COMMAND_ARM_START);
  CHECK(runtime_expect_exception(&runtime, request, request_length, 0x04U));
  CHECK(runtime_read_u16(&runtime, 0x04U, BOARD_A_INPUT_TIME_STATUS, &value) &&
        (value == BOARD_A_TIME_STATUS_UNCALIBRATED));
  CHECK(runtime_read_u16(&runtime, 0x04U, BOARD_A_INPUT_SCHEDULE_STATE,
                         &value) &&
        (value == BOARD_A_SCHEDULE_STATE_NONE));
  CHECK(runtime_read_u16(&runtime, 0x04U, BOARD_A_INPUT_RUN_STATE, &value) &&
        (value == BOARD_A_RUN_STOPPED));
  CHECK(runtime_read_u32(&runtime, 0x04U,
                         BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI,
                         &value32) &&
        (value32 == BOARD_A_TIME_INVALID_SECONDS));
  CHECK(runtime_read_u32(&runtime, 0x04U,
                         BOARD_A_INPUT_CURRENT_UTC_SECONDS_HI, &value32) &&
        (value32 == BOARD_A_TIME_INVALID_SECONDS));
  CHECK(runtime_read_u32(&runtime, 0x03U,
                         BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_HI,
                         &value32) &&
        (value32 == P3B_UTC + 5U));
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.sequence == 0U);
  CHECK(status.records_this_run == 0U);
  CHECK(status.last_command == BOARD_A_COMMAND_ARM_START);
  CHECK(status.command_result == BOARD_A_COMMAND_RESULT_REJECTED);

  /* Calibrate, then latch one valid future target. */
  CHECK(p3b_set_time(&runtime, &context, P3B_ANCHOR_US, P3B_UTC));
  CHECK(p3b_arm_start(&runtime, &context, P3B_ANCHOR_US, P3B_UTC + 5U));
  CHECK(runtime_read_u16(&runtime, 0x04U, BOARD_A_INPUT_SCHEDULE_STATE,
                         &value) &&
        (value == BOARD_A_SCHEDULE_STATE_WAITING));
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.schedule_deadline_us == P3B_ANCHOR_US + 5000000ULL);

  /* A target seconds before the anchor is past and leaves the latch intact. */
  context.now_us = P3B_ANCHOR_US + 2000000ULL;
  CHECK(runtime_write_pending_seconds(
      &runtime, BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_HI, P3B_UTC - 10U));
  CHECK(runtime_command_expect_exception(&runtime, BOARD_A_COMMAND_ARM_START,
                                         0x03U));
  CHECK(runtime_read_u32(&runtime, 0x04U,
                         BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI,
                         &value32) &&
        (value32 == P3B_UTC + 5U));
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.schedule_deadline_us == P3B_ANCHOR_US + 5000000ULL);
  CHECK(status.sequence == 0U);
  CHECK(status.records_this_run == 0U);
  CHECK(status.command_result == BOARD_A_COMMAND_RESULT_REJECTED);

  /*
   * A target inside the already-elapsed part of the current UTC second is
   * past: its boundary began before "anchor + two seconds".
   */
  CHECK(runtime_write_pending_seconds(
      &runtime, BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_HI, P3B_UTC + 1U));
  CHECK(runtime_command_expect_exception(&runtime, BOARD_A_COMMAND_ARM_START,
                                         0x03U));
  CHECK(runtime_read_u32(&runtime, 0x04U,
                         BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI,
                         &value32) &&
        (value32 == P3B_UTC + 5U));

  /* The anchor second itself is not a future boundary either. */
  context.now_us = P3B_ANCHOR_US + 500000ULL;
  CHECK(runtime_write_pending_seconds(
      &runtime, BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_HI, P3B_UTC));
  CHECK(runtime_command_expect_exception(&runtime, BOARD_A_COMMAND_ARM_START,
                                         0x03U));
  CHECK(runtime_read_u32(&runtime, 0x04U,
                         BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI,
                         &value32) &&
        (value32 == P3B_UTC + 5U));

  /* 0xFFFFFFFF is reserved: the whole staging write is rejected. */
  {
    uint16_t invalid[2] = {0xFFFFU, 0xFFFFU};

    request_length = make_write_multiple(
        request, BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_HI, invalid, 2U);
    CHECK(runtime_expect_exception(&runtime, request, request_length, 0x03U));
  }
  CHECK(runtime_read_u32(&runtime, 0x03U,
                         BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_HI,
                         &value32) &&
        (value32 == P3B_UTC));

  /* A single word write that would complete the sentinel is rejected too. */
  CHECK(runtime_write_pending_seconds(
      &runtime, BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_HI, 0x0000FFFFU));
  request_length = make_write_single(
      request, BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_HI, 0xFFFFU);
  CHECK(runtime_expect_exception(&runtime, request, request_length, 0x03U));
  CHECK(runtime_read_u32(&runtime, 0x03U,
                         BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_HI,
                         &value32) &&
        (value32 == 0x0000FFFFU));
  CHECK(runtime_read_u32(&runtime, 0x04U,
                         BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI,
                         &value32) &&
        (value32 == P3B_UTC + 5U));

  pthread_mutex_destroy(&context.mutex);
}

/* T02: a five-second future target starts once, with no pre-target record. */
static void test_t02_armed_start_waits_for_target(void)
{
  fake_lock_t context;
  board_a_runtime_t runtime;
  board_a_runtime_status_t status;
  uint16_t value;
  uint32_t value32;

  p3b_runtime_setup(&runtime, &context, 4U);
  CHECK(p3b_set_time(&runtime, &context, P3B_ANCHOR_US, P3B_UTC));
  CHECK(p3b_arm_start(&runtime, &context, P3B_ANCHOR_US, P3B_UTC + 5U));
  CHECK(runtime_read_u16(&runtime, 0x04U, BOARD_A_INPUT_SCHEDULE_STATE,
                         &value) &&
        (value == BOARD_A_SCHEDULE_STATE_WAITING));
  CHECK(runtime_read_u32(&runtime, 0x04U,
                         BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI,
                         &value32) &&
        (value32 == P3B_UTC + 5U));

  /* One microsecond before the target boundary nothing has started. */
  board_a_runtime_tick(&runtime, P3B_ANCHOR_US + 4999999ULL);
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.run_state == BOARD_A_RUN_STOPPED);
  CHECK(status.schedule_armed);
  CHECK(status.sequence == 0U);
  CHECK(status.records_this_run == 0U);

  /* The wake at the target starts once with the active configuration. */
  board_a_runtime_tick(&runtime, P3B_ANCHOR_US + 5000000ULL);
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.run_state == BOARD_A_RUN_RUNNING);
  CHECK(!status.schedule_armed);
  CHECK(status.sequence == 1U);
  CHECK(status.records_this_run == 1U);
  CHECK(status.next_sample_us == P3B_ANCHOR_US + 15000000ULL);
  CHECK(status.schedule_start_count == 1U);
  CHECK(status.schedule_start_late_us == 0U);
  CHECK(runtime_read_u16(&runtime, 0x04U, BOARD_A_INPUT_SCHEDULE_STATE,
                         &value) &&
        (value == BOARD_A_SCHEDULE_STATE_NONE));
  CHECK(runtime_read_u32(&runtime, 0x04U,
                         BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI,
                         &value32) &&
        (value32 == BOARD_A_TIME_INVALID_SECONDS));

  pthread_mutex_destroy(&context.mutex);
}

/* T03a: armed + STOP cancels the schedule and stays stopped. */
static void test_t03a_armed_stop_cancels(void)
{
  fake_lock_t context;
  board_a_runtime_t runtime;
  board_a_runtime_status_t status;
  uint32_t value32;

  p3b_runtime_setup(&runtime, &context, 5U);
  CHECK(p3b_set_time(&runtime, &context, P3B_ANCHOR_US, P3B_UTC));
  CHECK(p3b_arm_start(&runtime, &context, P3B_ANCHOR_US, P3B_UTC + 5U));
  context.now_us = P3B_ANCHOR_US + 1000000ULL;
  CHECK(runtime_command(&runtime, BOARD_A_COMMAND_STOP));
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(!status.schedule_armed);
  CHECK(status.run_state == BOARD_A_RUN_STOPPED);
  CHECK(runtime_read_u32(&runtime, 0x04U,
                         BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI,
                         &value32) &&
        (value32 == BOARD_A_TIME_INVALID_SECONDS));

  /* The cancelled target must not start anything later. */
  board_a_runtime_tick(&runtime, P3B_ANCHOR_US + 5000000ULL);
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.run_state == BOARD_A_RUN_STOPPED);
  CHECK(status.sequence == 0U);

  pthread_mutex_destroy(&context.mutex);
}

/* T03b: armed + START cancels the schedule and starts immediately. */
static void test_t03b_armed_start_cancels(void)
{
  fake_lock_t context;
  board_a_runtime_t runtime;
  board_a_runtime_status_t status;

  p3b_runtime_setup(&runtime, &context, 6U);
  CHECK(p3b_set_time(&runtime, &context, P3B_ANCHOR_US, P3B_UTC));
  CHECK(p3b_arm_start(&runtime, &context, P3B_ANCHOR_US, P3B_UTC + 5U));
  context.now_us = P3B_ANCHOR_US + 1000000ULL;
  CHECK(runtime_command(&runtime, BOARD_A_COMMAND_START));
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(!status.schedule_armed);
  CHECK(status.run_state == BOARD_A_RUN_RUNNING);
  CHECK(status.start_pending);

  board_a_runtime_tick(&runtime, P3B_ANCHOR_US + 1000000ULL);
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.sequence == 1U);
  CHECK(status.records_this_run == 1U);
  CHECK(status.next_sample_us == P3B_ANCHOR_US + 11000000ULL);

  /* The cancelled target must not inject a second start or record. */
  board_a_runtime_tick(&runtime, P3B_ANCHOR_US + 5000000ULL);
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.sequence == 1U);
  CHECK(status.records_this_run == 1U);

  pthread_mutex_destroy(&context.mutex);
}

/* T03c: armed + SINGLE keeps the schedule and does not consume finite periods. */
static void test_t03c_armed_single_keeps_schedule(void)
{
  fake_lock_t context;
  board_a_runtime_t runtime;
  board_a_runtime_status_t status;
  board_a_snapshot_t snapshot;
  uint16_t config[3] = {10U, 0x0001U, 2U};

  p3b_runtime_setup(&runtime, &context, 7U);
  CHECK(runtime_write_multiple_config(&runtime, config, 3U));
  CHECK(runtime_command(&runtime, BOARD_A_COMMAND_APPLY_CONFIG));
  CHECK(p3b_set_time(&runtime, &context, P3B_ANCHOR_US, P3B_UTC));
  CHECK(p3b_arm_start(&runtime, &context, P3B_ANCHOR_US, P3B_UTC + 5U));
  CHECK(runtime_submit_single(&runtime, 0x00000077U));
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.sequence == 1U);
  CHECK(status.records_this_run == 0U);
  CHECK(status.schedule_armed);
  CHECK(board_a_runtime_read_snapshot(&runtime, &snapshot));
  CHECK(snapshot.trigger == BOARD_A_SAMPLE_TRIGGER_SINGLE);

  /* The finite count still pays for both periodic records after expiry. */
  board_a_runtime_tick(&runtime, P3B_ANCHOR_US + 5000000ULL);
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.run_state == BOARD_A_RUN_RUNNING);
  CHECK(status.records_this_run == 1U);
  CHECK(status.sequence == 2U);
  board_a_runtime_tick(&runtime, P3B_ANCHOR_US + 15000000ULL);
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.records_this_run == 2U);
  CHECK(status.sequence == 3U);
  CHECK(status.run_state == BOARD_A_RUN_STOPPED);

  pthread_mutex_destroy(&context.mutex);
}

/* T03d: armed + APPLY_CONFIG keeps the schedule; expiry uses the new config. */
static void test_t03d_armed_apply_keeps_schedule(void)
{
  fake_lock_t context;
  board_a_runtime_t runtime;
  board_a_runtime_status_t status;
  board_a_snapshot_t snapshot;
  uint16_t config[3] = {20U, 0x0003U, 0U};
  uint16_t value;
  uint32_t value32;

  p3b_runtime_setup(&runtime, &context, 8U);
  CHECK(p3b_set_time(&runtime, &context, P3B_ANCHOR_US, P3B_UTC));
  CHECK(p3b_arm_start(&runtime, &context, P3B_ANCHOR_US, P3B_UTC + 5U));
  context.now_us = P3B_ANCHOR_US + 1000000ULL;
  CHECK(runtime_write_multiple_config(&runtime, config, 3U));
  CHECK(runtime_command(&runtime, BOARD_A_COMMAND_APPLY_CONFIG));
  CHECK(runtime_read_u16(&runtime, 0x04U, BOARD_A_INPUT_ACTIVE_PERIOD_SEC,
                         &value) &&
        (value == 20U));
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.schedule_armed);
  CHECK(status.run_state == BOARD_A_RUN_STOPPED);
  CHECK(runtime_read_u32(&runtime, 0x04U,
                         BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI,
                         &value32) &&
        (value32 == P3B_UTC + 5U));

  board_a_runtime_tick(&runtime, P3B_ANCHOR_US + 5000000ULL);
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.run_state == BOARD_A_RUN_RUNNING);
  CHECK(status.sequence == 1U);
  CHECK(status.next_sample_us == P3B_ANCHOR_US + 25000000ULL);
  CHECK(board_a_runtime_read_snapshot(&runtime, &snapshot));
  CHECK(snapshot.channel_count == 2U);

  pthread_mutex_destroy(&context.mutex);
}

/* T03e: a new valid ARM_START replaces the latched target atomically. */
static void test_t03e_rearm_replaces_target(void)
{
  fake_lock_t context;
  board_a_runtime_t runtime;
  board_a_runtime_status_t status;
  uint16_t value;
  uint32_t value32;

  p3b_runtime_setup(&runtime, &context, 9U);
  CHECK(p3b_set_time(&runtime, &context, P3B_ANCHOR_US, P3B_UTC));
  CHECK(p3b_arm_start(&runtime, &context, P3B_ANCHOR_US, P3B_UTC + 5U));
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.schedule_deadline_us == P3B_ANCHOR_US + 5000000ULL);

  context.now_us = P3B_ANCHOR_US + 1000000ULL;
  CHECK(p3b_arm_start(&runtime, &context, P3B_ANCHOR_US + 1000000ULL,
                      P3B_UTC + 30U));
  CHECK(runtime_read_u16(&runtime, 0x04U, BOARD_A_INPUT_SCHEDULE_STATE,
                         &value) &&
        (value == BOARD_A_SCHEDULE_STATE_WAITING));
  CHECK(runtime_read_u32(&runtime, 0x04U,
                         BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI,
                         &value32) &&
        (value32 == P3B_UTC + 30U));
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.schedule_deadline_us == P3B_ANCHOR_US + 30000000ULL);

  /* The replaced target no longer starts anything. */
  board_a_runtime_tick(&runtime, P3B_ANCHOR_US + 5000000ULL);
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.run_state == BOARD_A_RUN_STOPPED);
  CHECK(status.sequence == 0U);

  board_a_runtime_tick(&runtime, P3B_ANCHOR_US + 30000000ULL);
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.run_state == BOARD_A_RUN_RUNNING);
  CHECK(status.sequence == 1U);

  pthread_mutex_destroy(&context.mutex);
}

/* T04: SET_TIME is rejected while armed and does not move a running period. */
static void test_t04_set_time_rules(void)
{
  {
    fake_lock_t context;
    board_a_runtime_t runtime;
    uint16_t value;
    uint32_t value32;

    p3b_runtime_setup(&runtime, &context, 10U);
    CHECK(p3b_set_time(&runtime, &context, P3B_ANCHOR_US, P3B_UTC));
    CHECK(p3b_arm_start(&runtime, &context, P3B_ANCHOR_US, P3B_UTC + 5U));
    context.now_us = P3B_ANCHOR_US + 1000000ULL;
    CHECK(runtime_write_pending_seconds(
        &runtime, BOARD_A_HOLDING_PENDING_UTC_SECONDS_HI, P3B_UTC + 3600U));
    CHECK(runtime_command_expect_exception(&runtime, BOARD_A_COMMAND_SET_TIME,
                                           0x04U));
    CHECK(runtime_read_u16(&runtime, 0x04U, BOARD_A_INPUT_TIME_STATUS,
                           &value) &&
          (value == BOARD_A_TIME_STATUS_CALIBRATED));
    CHECK(runtime_read_u32(&runtime, 0x04U,
                           BOARD_A_INPUT_CURRENT_UTC_SECONDS_HI, &value32) &&
          (value32 == P3B_UTC + 1U));
    CHECK(runtime_read_u16(&runtime, 0x04U, BOARD_A_INPUT_SCHEDULE_STATE,
                           &value) &&
          (value == BOARD_A_SCHEDULE_STATE_WAITING));
    CHECK(runtime_read_u32(&runtime, 0x04U,
                           BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI,
                           &value32) &&
          (value32 == P3B_UTC + 5U));
    pthread_mutex_destroy(&context.mutex);
  }

  {
    fake_lock_t context;
    board_a_runtime_t runtime;
    board_a_runtime_status_t status;
    uint64_t next_sample_us;
    uint32_t value32;

    p3b_runtime_setup(&runtime, &context, 11U);
    CHECK(p3b_set_time(&runtime, &context, P3B_ANCHOR_US, P3B_UTC));
    context.now_us = P3B_ANCHOR_US;
    CHECK(runtime_command(&runtime, BOARD_A_COMMAND_START));
    board_a_runtime_tick(&runtime, P3B_ANCHOR_US);
    CHECK(board_a_runtime_copy_status(&runtime, &status));
    CHECK(status.run_state == BOARD_A_RUN_RUNNING);
    CHECK(status.records_this_run == 1U);
    next_sample_us = status.next_sample_us;
    CHECK(next_sample_us == P3B_ANCHOR_US + 10000000ULL);

    /* A running time jump re-anchors UTC but keeps the monotonic deadline. */
    context.now_us = P3B_ANCHOR_US + 1000000ULL;
    CHECK(runtime_write_pending_seconds(
        &runtime, BOARD_A_HOLDING_PENDING_UTC_SECONDS_HI, P3B_UTC + 3600U));
    CHECK(runtime_command(&runtime, BOARD_A_COMMAND_SET_TIME));
    CHECK(runtime_read_u32(&runtime, 0x04U,
                           BOARD_A_INPUT_CURRENT_UTC_SECONDS_HI, &value32) &&
          (value32 == P3B_UTC + 3600U));
    context.now_us = P3B_ANCHOR_US + 2000000ULL;
    CHECK(runtime_read_u32(&runtime, 0x04U,
                           BOARD_A_INPUT_CURRENT_UTC_SECONDS_HI, &value32) &&
          (value32 == P3B_UTC + 3601U));
    CHECK(board_a_runtime_copy_status(&runtime, &status));
    CHECK(status.next_sample_us == next_sample_us);
    CHECK(status.records_this_run == 1U);
    CHECK(status.run_state == BOARD_A_RUN_RUNNING);

    board_a_runtime_tick(&runtime, next_sample_us - 1ULL);
    CHECK(board_a_runtime_copy_status(&runtime, &status));
    CHECK(status.records_this_run == 1U);
    board_a_runtime_tick(&runtime, next_sample_us);
    CHECK(board_a_runtime_copy_status(&runtime, &status));
    CHECK(status.records_this_run == 2U);
    CHECK(status.sequence == 2U);
    pthread_mutex_destroy(&context.mutex);
  }
}

/* T05: one wake crossing the target starts once and replays nothing. */
static void test_t05_wake_crossing_starts_once(void)
{
  fake_lock_t context;
  board_a_runtime_t runtime;
  board_a_runtime_status_t status;
  uint64_t late_us = P3B_ANCHOR_US + 35000000ULL;

  p3b_runtime_setup(&runtime, &context, 12U);
  CHECK(p3b_set_time(&runtime, &context, P3B_ANCHOR_US, P3B_UTC));
  CHECK(p3b_arm_start(&runtime, &context, P3B_ANCHOR_US, P3B_UTC + 5U));

  board_a_runtime_tick(&runtime, late_us);
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.run_state == BOARD_A_RUN_RUNNING);
  CHECK(!status.schedule_armed);
  CHECK(status.sequence == 1U);
  CHECK(status.records_this_run == 1U);
  CHECK(status.stats.scheduler_missed == 0U);
  CHECK(status.next_sample_us == late_us + 10000000ULL);
  CHECK(status.schedule_start_count == 1U);
  CHECK(status.schedule_start_late_us == 30000000U);

  /* A repeated tick at the same instant must not replay the missed periods. */
  board_a_runtime_tick(&runtime, late_us);
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.sequence == 1U);
  CHECK(status.records_this_run == 1U);
  CHECK(status.schedule_start_count == 1U);

  /* The next periodic record arrives at the re-phased deadline only. */
  board_a_runtime_tick(&runtime, late_us + 10000000ULL);
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.sequence == 2U);
  CHECK(status.records_this_run == 2U);

  pthread_mutex_destroy(&context.mutex);
}

/* T06: a fresh model instance is the MCU-reset default state. */
static void test_t06_reset_defaults(void)
{
  fake_lock_t context;
  board_a_runtime_t runtime;
  board_a_runtime_status_t status;
  uint16_t value;
  uint32_t value32;

  p3b_runtime_setup(&runtime, &context, 13U);
  CHECK(runtime_read_u16(&runtime, 0x04U, BOARD_A_INPUT_TIME_STATUS, &value) &&
        (value == BOARD_A_TIME_STATUS_UNCALIBRATED));
  CHECK(runtime_read_u16(&runtime, 0x04U, BOARD_A_INPUT_SCHEDULE_STATE,
                         &value) &&
        (value == BOARD_A_SCHEDULE_STATE_NONE));
  CHECK(runtime_read_u16(&runtime, 0x04U, BOARD_A_INPUT_RUN_STATE, &value) &&
        (value == BOARD_A_RUN_STOPPED));
  CHECK(runtime_read_u32(&runtime, 0x04U,
                         BOARD_A_INPUT_CURRENT_UTC_SECONDS_HI, &value32) &&
        (value32 == BOARD_A_TIME_INVALID_SECONDS));
  CHECK(runtime_read_u32(&runtime, 0x04U,
                         BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI,
                         &value32) &&
        (value32 == BOARD_A_TIME_INVALID_SECONDS));
  CHECK(runtime_read_u32(&runtime, 0x03U,
                         BOARD_A_HOLDING_PENDING_UTC_SECONDS_HI, &value32) &&
        (value32 == 0U));
  CHECK(runtime_read_u32(&runtime, 0x03U,
                         BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_HI,
                         &value32) &&
        (value32 == 0U));

  /* No stored schedule can fire after reset, whatever the monotonic value. */
  board_a_runtime_tick(&runtime, 1ULL << 40U);
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.run_state == BOARD_A_RUN_STOPPED);
  CHECK(!status.schedule_armed);
  CHECK(status.sequence == 0U);

  pthread_mutex_destroy(&context.mutex);
}

/* T07: protocol 3 identity, unchanged 1..5 behavior, and 6/7 acceptance. */
static void test_t07_protocol_version_and_commands(void)
{
  fake_lock_t context;
  board_a_runtime_t runtime;
  board_a_runtime_status_t status;
  uint16_t value;

  p3b_runtime_setup(&runtime, &context, 14U);
  CHECK(runtime_read_u16(&runtime, 0x04U, BOARD_A_INPUT_PROTOCOL_VERSION,
                         &value) &&
        (value == 3U));

  /* Protocol 1 command values keep their meanings. */
  CHECK(runtime_command(&runtime, BOARD_A_COMMAND_START));
  CHECK(runtime_command(&runtime, BOARD_A_COMMAND_START));
  CHECK(runtime_command(&runtime, BOARD_A_COMMAND_STOP));
  CHECK(runtime_finish_drain(&runtime));
  CHECK(runtime_submit_single(&runtime, 0x00000001U));
  CHECK(runtime_submit_single(&runtime, 0x00000001U));
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.command_result == BOARD_A_COMMAND_RESULT_DUPLICATE);

  /* SAVE_CONFIG is accepted into the asynchronous mailbox. */
  CHECK(runtime_command(&runtime, BOARD_A_COMMAND_SAVE_CONFIG));
  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.command_result == BOARD_A_COMMAND_RESULT_ACCEPTED);

  /* Values outside the supported set are illegal values. */
  CHECK(runtime_command_expect_exception(&runtime, 0U, 0x03U));
  CHECK(runtime_command_expect_exception(&runtime, 8U, 0x03U));
  CHECK(runtime_command_expect_exception(&runtime, 0xFFFFU, 0x03U));

  /* The protocol 2 operations answer normally on a calibrated device. */
  CHECK(p3b_set_time(&runtime, &context, P3B_ANCHOR_US, P3B_UTC));
  CHECK(p3b_arm_start(&runtime, &context, P3B_ANCHOR_US, P3B_UTC + 5U));
  CHECK(runtime_read_u16(&runtime, 0x04U, BOARD_A_INPUT_TIME_STATUS,
                         &value) &&
        (value == BOARD_A_TIME_STATUS_CALIBRATED));
  CHECK(runtime_read_u16(&runtime, 0x04U, BOARD_A_INPUT_SCHEDULE_STATE,
                         &value) &&
        (value == BOARD_A_SCHEDULE_STATE_WAITING));

  pthread_mutex_destroy(&context.mutex);
}

/* T08: word order, reserved sentinel, day boundary, and highest legal UTC. */
static void test_t08_word_encoding_and_boundaries(void)
{
  {
    fake_lock_t context;
    board_a_runtime_t runtime;
    uint8_t request[8];
    size_t request_length;
    uint16_t words[2];
    uint32_t value32;

    p3b_runtime_setup(&runtime, &context, 15U);
    CHECK(runtime_write_pending_seconds(
        &runtime, BOARD_A_HOLDING_PENDING_UTC_SECONDS_HI, 0x12345678U));
    CHECK(runtime_read_block(&runtime, 0x03U,
                             BOARD_A_HOLDING_PENDING_UTC_SECONDS_HI, 2U,
                             words));
    CHECK(words[0] == 0x1234U);
    CHECK(words[1] == 0x5678U);

    /* A single word write that would complete the sentinel is rejected. */
    CHECK(runtime_write_holding_single(
        &runtime, BOARD_A_HOLDING_PENDING_UTC_SECONDS_HI, 0xFFFFU));
    request_length = make_write_single(
        request, BOARD_A_HOLDING_PENDING_UTC_SECONDS_LO, 0xFFFFU);
    CHECK(runtime_expect_exception(&runtime, request, request_length, 0x03U));
    CHECK(runtime_read_u32(&runtime, 0x03U,
                           BOARD_A_HOLDING_PENDING_UTC_SECONDS_HI, &value32) &&
          (value32 == 0xFFFF5678U));
    pthread_mutex_destroy(&context.mutex);
  }

  {
    fake_lock_t context;
    board_a_runtime_t runtime;
    board_a_runtime_status_t status;
    uint32_t value32;

    /* 23:59:58 + a five-second target crosses into the next day. */
    p3b_runtime_setup(&runtime, &context, 16U);
    CHECK(p3b_set_time(&runtime, &context, P3B_ANCHOR_US, 86398U));
    CHECK(p3b_arm_start(&runtime, &context, P3B_ANCHOR_US, 86403U));
    context.now_us = P3B_ANCHOR_US + 4000000ULL;
    CHECK(runtime_read_u32(&runtime, 0x04U,
                           BOARD_A_INPUT_CURRENT_UTC_SECONDS_HI, &value32) &&
          (value32 == 86402U));
    board_a_runtime_tick(&runtime, P3B_ANCHOR_US + 4999999ULL);
    CHECK(board_a_runtime_copy_status(&runtime, &status));
    CHECK(status.run_state == BOARD_A_RUN_STOPPED);
    CHECK(status.sequence == 0U);
    board_a_runtime_tick(&runtime, P3B_ANCHOR_US + 5000000ULL);
    CHECK(board_a_runtime_copy_status(&runtime, &status));
    CHECK(status.run_state == BOARD_A_RUN_RUNNING);
    CHECK(status.sequence == 1U);
    pthread_mutex_destroy(&context.mutex);
  }

  {
    fake_lock_t context;
    board_a_runtime_t runtime;
    uint8_t request[64];
    size_t request_length;
    uint32_t value32;

    p3b_runtime_setup(&runtime, &context, 17U);
    CHECK(p3b_set_time(&runtime, &context, P3B_ANCHOR_US, 0xFFFFFFF0U));
    CHECK(runtime_read_u32(&runtime, 0x04U,
                           BOARD_A_INPUT_CURRENT_UTC_SECONDS_HI, &value32) &&
          (value32 == 0xFFFFFFF0U));
    CHECK(p3b_arm_start(&runtime, &context, P3B_ANCHOR_US, 0xFFFFFFFEU));
    CHECK(runtime_read_u32(&runtime, 0x04U,
                           BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI,
                           &value32) &&
          (value32 == 0xFFFFFFFEU));
    context.now_us = P3B_ANCHOR_US + 2000000ULL;
    CHECK(runtime_read_u32(&runtime, 0x04U,
                           BOARD_A_INPUT_CURRENT_UTC_SECONDS_HI, &value32) &&
          (value32 == 0xFFFFFFF2U));

    /* One second past the highest legal UTC is the reserved sentinel. */
    {
      uint16_t invalid[2] = {0xFFFFU, 0xFFFFU};

      request_length = make_write_multiple(
          request, BOARD_A_HOLDING_PENDING_UTC_SECONDS_HI, invalid, 2U);
      CHECK(runtime_expect_exception(&runtime, request, request_length, 0x03U));
    }
    CHECK(runtime_read_u32(&runtime, 0x04U,
                           BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI,
                           &value32) &&
          (value32 == 0xFFFFFFFEU));
    pthread_mutex_destroy(&context.mutex);
  }
}

typedef struct {
  board_a_runtime_t *runtime;
  atomic_int *stop;
} tick_thread_args_t;

static void *tick_thread(void *argument)
{
  tick_thread_args_t *args = (tick_thread_args_t *)argument;
  uint64_t now_us = 1000000ULL;

  while (!atomic_load_explicit(args->stop, memory_order_acquire)) {
    board_a_runtime_tick(args->runtime, now_us);
    now_us += 1000ULL;
  }
  return NULL;
}

static void test_concurrent_snapshot_and_commands(void)
{
  fake_lock_t context;
  board_a_runtime_t runtime;
  board_a_runtime_status_t status;
  board_a_snapshot_t snapshot;
  pthread_t ticker;
  atomic_int stop;
  tick_thread_args_t tick_args;
  uint32_t id;
  uint32_t index;

  memset(&context, 0, sizeof(context));
  CHECK(pthread_mutex_init(&context.mutex, NULL) == 0);
  board_a_runtime_init(&runtime, 2U, &FAKE_OPS, &context);
  CHECK(runtime_command(&runtime, BOARD_A_COMMAND_START));

  atomic_init(&stop, 0);
  tick_args.runtime = &runtime;
  tick_args.stop = &stop;
  CHECK(pthread_create(&ticker, NULL, tick_thread, &tick_args) == 0);

  for (id = 1U; id <= 200U; ++id) {
    CHECK(runtime_submit_single(&runtime, id));
  }
  atomic_store_explicit(&stop, 1, memory_order_release);
  CHECK(pthread_join(ticker, NULL) == 0);

  for (index = 0U; index < 1000U; ++index) {
    CHECK(board_a_runtime_read_snapshot(&runtime, &snapshot));
    if (snapshot.valid) {
      CHECK(snapshot.channel_count == 1U);
      CHECK(snapshot.channel_values[0] ==
            (uint16_t)(snapshot.sequence * 10U));
      CHECK(snapshot.channel_quality[0] == BOARD_A_QUALITY_TEST_VALID);
    }
  }

  CHECK(board_a_runtime_copy_status(&runtime, &status));
  CHECK(status.sequence >= 200U);
  pthread_mutex_destroy(&context.mutex);
}

int main(void)
{
  test_monotonic_wrap();
  test_monotonic_ms_across_tim2_wrap();
  test_deadline_predicate_across_ms_wrap();
  test_tx_state_machine();
  test_rx_recovery();
  test_model_commands_and_scheduler();
  test_t01_rejection_paths();
  test_t02_armed_start_waits_for_target();
  test_t03a_armed_stop_cancels();
  test_t03b_armed_start_cancels();
  test_t03c_armed_single_keeps_schedule();
  test_t03d_armed_apply_keeps_schedule();
  test_t03e_rearm_replaces_target();
  test_t04_set_time_rules();
  test_t05_wake_crossing_starts_once();
  test_t06_reset_defaults();
  test_t07_protocol_version_and_commands();
  test_t08_word_encoding_and_boundaries();
  test_concurrent_snapshot_and_commands();

  printf("board_a RTOS host tests: %u checks, %u failures\n",
         g_checks, g_failures);
  return g_failures == 0U ? 0 : 1;
}
