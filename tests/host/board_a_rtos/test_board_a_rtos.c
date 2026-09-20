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

static void test_monotonic_wrap(void)
{
  board_a_monotonic_t clock = {0};

  CHECK(board_a_monotonic_update(&clock, 0xFFFFFFF0U) == 0U);
  CHECK(board_a_monotonic_update(&clock, 0xFFFFFFF8U) == 8U);
  CHECK(board_a_monotonic_update(&clock, 0x00000010U) == 32U);
  CHECK(board_a_monotonic_value(&clock) == 32U);
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
  test_tx_state_machine();
  test_rx_recovery();
  test_model_commands_and_scheduler();
  test_concurrent_snapshot_and_commands();

  printf("board_a RTOS host tests: %u checks, %u failures\n",
         g_checks, g_failures);
  return g_failures == 0U ? 0 : 1;
}
