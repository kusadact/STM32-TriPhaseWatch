#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "board_a_slave.h"
#include "board_a_log_schedule.h"
#include "modbus_crc.h"

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

static uint16_t get_u16_be(const uint8_t *frame, size_t offset)
{
  return (uint16_t)(((uint16_t)frame[offset] << 8U) |
                    (uint16_t)frame[offset + 1U]);
}

static size_t make_read_request(uint8_t *frame,
                                uint8_t address,
                                uint8_t function,
                                uint16_t start,
                                uint16_t quantity)
{
  frame[0] = address;
  frame[1] = function;
  put_u16_be(frame, 2U, start);
  put_u16_be(frame, 4U, quantity);
  return append_crc(frame, 6U);
}

static size_t make_write_single_request(uint8_t *frame,
                                        uint8_t address,
                                        uint16_t register_address,
                                        uint16_t value)
{
  frame[0] = address;
  frame[1] = 0x06U;
  put_u16_be(frame, 2U, register_address);
  put_u16_be(frame, 4U, value);
  return append_crc(frame, 6U);
}

static size_t make_write_multiple_request(uint8_t *frame,
                                          uint8_t address,
                                          uint16_t start,
                                          const uint16_t *values,
                                          uint16_t quantity)
{
  size_t index;

  frame[0] = address;
  frame[1] = 0x10U;
  put_u16_be(frame, 2U, start);
  put_u16_be(frame, 4U, quantity);
  frame[6] = (uint8_t)(quantity * 2U);
  for (index = 0U; index < quantity; ++index) {
    put_u16_be(frame, 7U + (index * 2U), values[index]);
  }
  return append_crc(frame, 7U + ((size_t)quantity * 2U));
}

static size_t exchange(board_a_slave_t *slave,
                       const uint8_t *request,
                       size_t request_length,
                       uint8_t *response,
                       size_t response_capacity)
{
  uint32_t now_us = 1000U;
  size_t index;

  for (index = 0U; index < request_length; ++index) {
    board_a_slave_push_byte(slave, request[index], now_us);
    now_us += 100U;
  }
  return board_a_slave_poll(slave, now_us + 5000U, response,
                            response_capacity);
}

static int response_has_valid_crc(const uint8_t *response, size_t length)
{
  uint16_t received;
  uint16_t calculated;

  if (length < 4U) {
    return 0;
  }
  received = (uint16_t)((uint16_t)response[length - 2U] |
                        ((uint16_t)response[length - 1U] << 8U));
  calculated = modbus_crc16(response, length - 2U);
  return received == calculated;
}

static int read_registers(board_a_slave_t *slave,
                          uint8_t function,
                          uint16_t start,
                          uint16_t quantity,
                          uint16_t *values)
{
  uint8_t request[8];
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  size_t request_length;
  size_t response_length;
  size_t index;

  request_length = make_read_request(request, BOARD_A_SLAVE_ADDRESS,
                                     function, start, quantity);
  response_length = exchange(slave, request, request_length, response,
                             sizeof(response));
  if (response_length != (5U + ((size_t)quantity * 2U))) {
    return 0;
  }
  if ((response[0] != BOARD_A_SLAVE_ADDRESS) ||
      (response[1] != function) ||
      (response[2] != (uint8_t)(quantity * 2U)) ||
      !response_has_valid_crc(response, response_length)) {
    return 0;
  }
  for (index = 0U; index < quantity; ++index) {
    values[index] = get_u16_be(response, 3U + (index * 2U));
  }
  return 1;
}

static uint16_t read_one(board_a_slave_t *slave,
                         uint8_t function,
                         uint16_t address)
{
  uint16_t value = 0xFFFFU;

  if (!read_registers(slave, function, address, 1U, &value)) {
    return 0xFFFFU;
  }
  return value;
}

static int expect_exception(board_a_slave_t *slave,
                            const uint8_t *request,
                            size_t request_length,
                            uint8_t expected_exception)
{
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  size_t response_length = exchange(slave, request, request_length,
                                    response, sizeof(response));

  return (response_length == 5U) &&
      (response[0] == BOARD_A_SLAVE_ADDRESS) &&
      (response[1] == (uint8_t)(request[1] | 0x80U)) &&
      (response[2] == expected_exception) &&
      response_has_valid_crc(response, response_length);
}

static int expect_no_response(board_a_slave_t *slave,
                              const uint8_t *request,
                              size_t request_length)
{
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];

  return exchange(slave, request, request_length, response,
                  sizeof(response)) == 0U;
}

static int write_single(board_a_slave_t *slave,
                        uint8_t address,
                        uint16_t register_address,
                        uint16_t value)
{
  uint8_t request[8];
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  size_t request_length = make_write_single_request(
      request, address, register_address, value);
  size_t response_length = exchange(slave, request, request_length,
                                    response, sizeof(response));

  return (response_length == request_length) &&
      (memcmp(response, request, request_length) == 0);
}

static int write_multiple(board_a_slave_t *slave,
                          uint16_t start,
                          const uint16_t *values,
                          uint16_t quantity)
{
  uint8_t request[7U + (2U * MODBUS_RTU_MAX_WRITE_REGISTERS) + 2U];
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  size_t request_length = make_write_multiple_request(
      request, BOARD_A_SLAVE_ADDRESS, start, values, quantity);
  size_t response_length = exchange(slave, request, request_length,
                                    response, sizeof(response));

  return (response_length == 8U) &&
      (response[0] == BOARD_A_SLAVE_ADDRESS) &&
      (response[1] == 0x10U) &&
      (get_u16_be(response, 2U) == start) &&
      (get_u16_be(response, 4U) == quantity) &&
      response_has_valid_crc(response, response_length);
}

static int command(board_a_slave_t *slave, uint16_t command_code)
{
  return write_single(slave, BOARD_A_SLAVE_ADDRESS,
                      BOARD_A_HOLDING_COMMAND, command_code);
}

static int submit_single(board_a_slave_t *slave, uint32_t command_id)
{
  uint16_t id_values[2];

  id_values[0] = (uint16_t)(command_id >> 16U);
  id_values[1] = (uint16_t)command_id;
  if (!write_multiple(slave, BOARD_A_HOLDING_COMMAND_ID_HI,
                      id_values, 2U)) {
    return 0;
  }
  return command(slave, BOARD_A_COMMAND_SINGLE);
}

static void test_crc_standard_vector(void)
{
  static const uint8_t vector[] = "123456789";

  CHECK(modbus_crc16(vector, 9U) == 0x4B37U);
}

static void test_normal_reads(void)
{
  board_a_slave_t slave;
  uint16_t values[4];
  uint16_t value;

  board_a_slave_init(&slave, 0x00000001U);
  CHECK(read_registers(&slave, 0x03U, BOARD_A_HOLDING_CFG_PERIOD_SEC,
                       3U, values));
  CHECK(values[0] == 10U);
  CHECK(values[1] == 0x0001U);
  CHECK(values[2] == 0U);

  CHECK(read_registers(&slave, 0x04U, BOARD_A_INPUT_DEVICE_TYPE,
                       1U, &value));
  CHECK(value == 0x0001U);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_ACTIVE_CONFIG_VALID) == 1U);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_PERSISTENCE_STATUS) == 0U);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_STORAGE_STATUS) == 0U);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_RTOS_STATUS) == 0U);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_PROTOCOL_VERSION) == 3U);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_TIME_STATUS) ==
        BOARD_A_TIME_STATUS_UNCALIBRATED);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_SCHEDULE_STATE) ==
        BOARD_A_SCHEDULE_STATE_NONE);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_CURRENT_UTC_SECONDS_HI) ==
        0xFFFFU);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_CURRENT_UTC_SECONDS_LO) ==
        0xFFFFU);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI) ==
        0xFFFFU);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_ARMED_START_UTC_SECONDS_LO) ==
        0xFFFFU);
}

static void test_single_write_and_snapshot(void)
{
  board_a_slave_t slave;
  uint16_t values[12];
  uint16_t mask = 0x0003U;

  board_a_slave_init(&slave, 0x00000002U);
  CHECK(write_multiple(&slave, BOARD_A_HOLDING_CFG_CHANNEL_MASK, &mask, 1U));
  CHECK(command(&slave, BOARD_A_COMMAND_APPLY_CONFIG));
  CHECK(submit_single(&slave, 0x00000001U));

  CHECK(read_registers(&slave, 0x04U, BOARD_A_INPUT_RECORD_SEQUENCE_HI,
                       12U, values));
  CHECK(values[0] == 0U);
  CHECK(values[1] == 1U);
  CHECK(values[2] == BOARD_A_DATA_SOURCE_TEST);
  CHECK(values[3] == 2U);
  CHECK(values[4] == 10U);
  CHECK(values[5] == 11U);
  CHECK(values[6] == 0U);
  CHECK(values[7] == 0U);
  CHECK(values[8] == BOARD_A_QUALITY_TEST_VALID);
  CHECK(values[9] == BOARD_A_QUALITY_TEST_VALID);
  CHECK(values[10] == BOARD_A_QUALITY_UNAVAILABLE);
  CHECK(values[11] == BOARD_A_QUALITY_UNAVAILABLE);

  CHECK(command(&slave, BOARD_A_COMMAND_SINGLE));
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_RECORD_SEQUENCE_LO) == 1U);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_COMMAND_RESULT) ==
        BOARD_A_COMMAND_RESULT_DUPLICATE);

  CHECK(submit_single(&slave, 0x00000002U));
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_RECORD_SEQUENCE_LO) == 2U);

  /*
   * Regression for the original defect: ID 1 is older than ID 2 but remains
   * in the bounded window, so resubmitting ID 1 must not create a third record.
   */
  CHECK(submit_single(&slave, 0x00000001U));
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_RECORD_SEQUENCE_LO) == 2U);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_COMMAND_RESULT) ==
        BOARD_A_COMMAND_RESULT_DUPLICATE);
}

static void test_single_dedup_window(void)
{
  board_a_slave_t slave;
  uint32_t id;

  board_a_slave_init(&slave, 0x0000000BU);

  for (id = 1U; id <= BOARD_A_SINGLE_DEDUP_CAPACITY; ++id) {
    CHECK(submit_single(&slave, id));
  }
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_RECORD_SEQUENCE_LO) ==
        BOARD_A_SINGLE_DEDUP_CAPACITY);

  /* The oldest still-present ID remains deduplicated. */
  CHECK(submit_single(&slave, 1U));
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_RECORD_SEQUENCE_LO) ==
        BOARD_A_SINGLE_DEDUP_CAPACITY);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_COMMAND_RESULT) ==
        BOARD_A_COMMAND_RESULT_DUPLICATE);

  /* Fill beyond capacity so ID 1 is evicted by the FIFO. */
  CHECK(submit_single(&slave, BOARD_A_SINGLE_DEDUP_CAPACITY + 1U));
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_RECORD_SEQUENCE_LO) ==
        BOARD_A_SINGLE_DEDUP_CAPACITY + 1U);

  /*
   * Explicit boundary: an evicted ID is no longer known and executes again.
   * This is the documented behavior of a bounded session window, not a
   * guarantee of permanent idempotency.
   */
  CHECK(submit_single(&slave, 1U));
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_RECORD_SEQUENCE_LO) ==
        BOARD_A_SINGLE_DEDUP_CAPACITY + 2U);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_COMMAND_RESULT) ==
        BOARD_A_COMMAND_RESULT_ACCEPTED);

  /* The most recently submitted IDs remain in the window. */
  CHECK(submit_single(&slave, BOARD_A_SINGLE_DEDUP_CAPACITY + 1U));
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_RECORD_SEQUENCE_LO) ==
        BOARD_A_SINGLE_DEDUP_CAPACITY + 2U);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_COMMAND_RESULT) ==
        BOARD_A_COMMAND_RESULT_DUPLICATE);
}

static void test_ds18b20_snapshot_modbus_block(void)
{
  board_a_slave_t slave;
  board_a_sensor_snapshot_t sensors;
  uint16_t config[3] = {10U, 0x0007U, 0U};
  uint16_t values[24];
  uint8_t index;

  board_a_slave_init(&slave, 0x00000003U);
  memset(&sensors, 0, sizeof(sensors));
  sensors.sample_id = 42U;
  sensors.valid_mask = 0x0007U;
  for (index = 0U; index < BOARD_A_SENSOR_COUNT; ++index) {
    sensors.sensors[index].sensor_id = index;
    sensors.sensors[index].sensor_type = BOARD_A_SENSOR_TYPE_DS18B20;
    sensors.sensors[index].has_value = true;
    sensors.sensors[index].temperature_x16 =
        (index == 1U) ? -160 : (int16_t)(250U + (index * 10U));
    sensors.sensors[index].quality = BOARD_A_QUALITY_OK;
    sensors.sensors[index].error = BOARD_A_SENSOR_ERROR_NONE;
    sensors.sensors[index].rom_short = (uint16_t)(0x1200U + index);
    sensors.sensors[index].sample_time_ms = 1000U + index;
  }
  CHECK(board_a_model_set_data_source(
      &slave.model, BOARD_A_DATA_SOURCE_REAL_DS18B20));
  board_a_model_publish_sensor_snapshot(&slave.model, &sensors);
  CHECK(write_multiple(&slave, BOARD_A_HOLDING_CFG_PERIOD_SEC, config, 3U));
  CHECK(command(&slave, BOARD_A_COMMAND_APPLY_CONFIG));
  CHECK(command(&slave, BOARD_A_COMMAND_START));
  board_a_slave_tick(&slave, 1000000U);

  CHECK(read_registers(&slave, 0x04U,
                       BOARD_A_INPUT_DS18B20_CONTRACT_REVISION,
                       (uint16_t)(sizeof(values) / sizeof(values[0])),
                       values));
  CHECK(values[0] == 2U);
  CHECK(values[1] == BOARD_A_DATA_SOURCE_REAL_DS18B20);
  CHECK(values[2] == 0x0007U);
  CHECK(((uint32_t)values[3] << 16U | values[4]) == 42U);
  CHECK(values[5] == 250U);
  CHECK(values[6] == (uint16_t)-160);
  CHECK(values[7] == 270U);
  CHECK(values[8] == BOARD_A_QUALITY_OK);
  CHECK(values[10] == BOARD_A_QUALITY_OK);
  CHECK(values[11] == BOARD_A_SENSOR_ERROR_NONE);
  CHECK(((uint32_t)values[14] << 16U | values[15]) == 1000U);
  CHECK(((uint32_t)values[18] << 16U | values[19]) == 1002U);
  CHECK(values[20] == BOARD_A_SENSOR_TYPE_DS18B20);
  CHECK(values[21] == 0x1200U);
  CHECK(values[23] == 0x1202U);
}

static void test_write_single_and_multiple(void)
{
  board_a_slave_t slave;
  uint16_t values[3];
  uint16_t written[3] = {30U, 0x000FU, 7U};
  uint16_t invalid[3] = {50U, 0x0010U, 9U};

  board_a_slave_init(&slave, 0x00000003U);
  CHECK(write_single(&slave, BOARD_A_SLAVE_ADDRESS,
                     BOARD_A_HOLDING_CFG_PERIOD_SEC, 20U));
  CHECK(read_registers(&slave, 0x03U, BOARD_A_HOLDING_CFG_PERIOD_SEC,
                       1U, values));
  CHECK(values[0] == 20U);

  CHECK(write_multiple(&slave, BOARD_A_HOLDING_CFG_PERIOD_SEC,
                       written, 3U));
  CHECK(read_registers(&slave, 0x03U, BOARD_A_HOLDING_CFG_PERIOD_SEC,
                       3U, values));
  CHECK(values[0] == 30U);
  CHECK(values[1] == 0x000FU);
  CHECK(values[2] == 7U);

  /* The invalid third register must leave all three previous values intact. */
  {
    uint8_t request[7U + (2U * 3U) + 2U];
    size_t request_length = make_write_multiple_request(
        request, BOARD_A_SLAVE_ADDRESS, BOARD_A_HOLDING_CFG_PERIOD_SEC,
        invalid, 3U);
    CHECK(expect_exception(&slave, request, request_length, 0x03U));
  }
  CHECK(read_registers(&slave, 0x03U, BOARD_A_HOLDING_CFG_PERIOD_SEC,
                       3U, values));
  CHECK(values[0] == 30U);
  CHECK(values[1] == 0x000FU);
  CHECK(values[2] == 7U);
}

static void test_exception_responses(void)
{
  board_a_slave_t slave;
  uint8_t request[32];
  size_t request_length;
  uint16_t zero_quantity[1] = {0U};

  board_a_slave_init(&slave, 0x00000004U);

  request[0] = BOARD_A_SLAVE_ADDRESS;
  request[1] = 0x45U;
  request_length = append_crc(request, 2U);
  CHECK(expect_exception(&slave, request, request_length, 0x01U));

  request_length = make_read_request(request, BOARD_A_SLAVE_ADDRESS, 0x03U,
                                     0x0003U, 1U);
  CHECK(expect_exception(&slave, request, request_length, 0x02U));

  request_length = make_read_request(request, BOARD_A_SLAVE_ADDRESS, 0x03U,
                                     0x0000U, 0U);
  CHECK(expect_exception(&slave, request, request_length, 0x03U));

  request_length = make_read_request(request, BOARD_A_SLAVE_ADDRESS, 0x03U,
                                     0x0000U, 126U);
  CHECK(expect_exception(&slave, request, request_length, 0x03U));

  request_length = make_write_single_request(
      request, BOARD_A_SLAVE_ADDRESS, BOARD_A_HOLDING_CFG_PERIOD_SEC, 9U);
  CHECK(expect_exception(&slave, request, request_length, 0x03U));

  request_length = make_write_multiple_request(
      request, BOARD_A_SLAVE_ADDRESS, 0xFFFFU, zero_quantity, 0U);
  CHECK(expect_exception(&slave, request, request_length, 0x03U));

  request[0] = BOARD_A_SLAVE_ADDRESS;
  request[1] = 0x03U;
  request[2] = 0x00U;
  request[3] = 0x00U;
  request[4] = 0x00U;
  request_length = append_crc(request, 5U);
  CHECK(expect_exception(&slave, request, request_length, 0x03U));
}

static void test_length_and_byte_count_errors(void)
{
  board_a_slave_t slave;
  uint8_t request[32];
  uint16_t one_value = 25U;
  size_t request_length;

  board_a_slave_init(&slave, 0x00000005U);

  request_length = make_write_multiple_request(
      request, BOARD_A_SLAVE_ADDRESS, BOARD_A_HOLDING_CFG_PERIOD_SEC,
      &one_value, 1U);
  request[request_length] = 0xAAU;
  request[request_length + 1U] = 0x55U;
  request_length = append_crc(request, request_length + 2U);
  CHECK(expect_exception(&slave, request, request_length, 0x03U));

  request[0] = BOARD_A_SLAVE_ADDRESS;
  request[1] = 0x10U;
  request[2] = 0x00U;
  request[3] = 0x00U;
  request[4] = 0x00U;
  request[5] = 0x01U;
  request[6] = 0x04U;
  request[7] = 0x00U;
  request[8] = 0x1EU;
  request_length = append_crc(request, 9U);
  CHECK(expect_exception(&slave, request, request_length, 0x03U));

  request_length = make_read_request(request, BOARD_A_SLAVE_ADDRESS, 0x03U,
                                     0xFFFFU, 2U);
  CHECK(expect_exception(&slave, request, request_length, 0x02U));

  /*
   * 0x0019 became SCHEDULE_STATE in protocol 2, so the still-undefined
   * 0x001E..0x001F pair keeps this "range crosses undefined addresses" check.
   */
  request_length = make_read_request(request, BOARD_A_SLAVE_ADDRESS, 0x04U,
                                     0x001EU, 2U);
  CHECK(expect_exception(&slave, request, request_length, 0x02U));
}

static void test_no_response_cases(void)
{
  board_a_slave_t slave;
  uint8_t request[8];
  size_t request_length;
  uint16_t values[1];

  board_a_slave_init(&slave, 0x00000006U);

  request_length = make_read_request(request, BOARD_A_SLAVE_ADDRESS, 0x03U,
                                     0x0000U, 1U);
  request[request_length - 1U] ^= 0x01U;
  CHECK(expect_no_response(&slave, request, request_length));
  CHECK(slave.server.stats.crc_errors == 1U);

  request_length = make_read_request(request, 2U, 0x03U, 0x0000U, 1U);
  CHECK(expect_no_response(&slave, request, request_length));
  CHECK(slave.server.stats.address_mismatch == 1U);

  request_length = make_read_request(request, 0U, 0x03U, 0x0000U, 1U);
  CHECK(expect_no_response(&slave, request, request_length));
  CHECK(slave.server.stats.broadcast_reads == 1U);

  request_length = make_write_single_request(
      request, 0U, BOARD_A_HOLDING_CFG_PERIOD_SEC, 25U);
  CHECK(expect_no_response(&slave, request, request_length));
  CHECK(slave.server.stats.broadcast_writes == 1U);
  CHECK(read_registers(&slave, 0x03U, BOARD_A_HOLDING_CFG_PERIOD_SEC,
                       1U, values));
  CHECK(values[0] == 25U);
}

static void test_fragmented_frames_and_overlong(void)
{
  board_a_slave_t slave;
  uint8_t request[8];
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  size_t request_length;
  uint32_t now_us = 100000U;
  size_t index;
  size_t response_length;

  board_a_slave_init(&slave, 0x00000007U);
  request_length = make_read_request(request, BOARD_A_SLAVE_ADDRESS, 0x03U,
                                     0x0000U, 1U);

  for (index = 0U; index < request_length; ++index) {
    board_a_slave_push_byte(&slave, request[index], now_us);
    now_us += 1000U;
  }
  CHECK(board_a_slave_poll(&slave, now_us + 3000U, response,
                           sizeof(response)) == 0U);
  response_length = board_a_slave_poll(&slave, now_us + 3011U, response,
                                       sizeof(response));
  CHECK(response_length == 7U);
  CHECK(response_has_valid_crc(response, response_length));

  board_a_slave_init(&slave, 0x00000008U);
  now_us = 200000U;
  for (index = 0U; index < MODBUS_RTU_MAX_ADU_SIZE; ++index) {
    board_a_slave_push_byte(&slave, 0xAAU, now_us);
    now_us += 100U;
  }
  board_a_slave_push_byte(&slave, 0xBBU, now_us);
  now_us += 5000U;
  CHECK(board_a_slave_poll(&slave, now_us, response, sizeof(response)) == 0U);
  CHECK(slave.receiver.overlong_frames == 1U);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_OVERLONG_FRAMES_LO) == 1U);
}

/*
 * The platform samples now_us in the main loop before the USART2 ISR may
 * push a byte, so poll() can be called with a timestamp that is slightly
 * older than the newest byte's timestamp. The receiver must treat that as
 * "no gap yet" instead of wrapping to a huge unsigned interval and splitting
 * the frame into one-byte fragments.
 */
static void test_poll_timestamp_older_than_isr_timestamp(void)
{
  board_a_slave_t slave;
  uint8_t request[8];
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  size_t request_length;
  uint32_t sampled_us;
  uint32_t byte_us;
  size_t index;
  size_t response_length;

  board_a_slave_init(&slave, 0x0000000CU);
  request_length = make_read_request(request, BOARD_A_SLAVE_ADDRESS, 0x03U,
                                     0x0000U, 1U);

  sampled_us = 300000U;
  byte_us = sampled_us;
  for (index = 0U; index < request_length; ++index) {
    byte_us += 1150U;
    board_a_slave_push_byte(&slave, request[index], byte_us);
    /* Poll with the stale sample taken before this byte arrived. */
    CHECK(board_a_slave_poll(&slave, sampled_us, response,
                             sizeof(response)) == 0U);
    CHECK(slave.receiver.frames_ready == 0U);
    sampled_us += 1150U;
  }

  response_length = board_a_slave_poll(&slave, byte_us + 4011U, response,
                                       sizeof(response));
  CHECK(response_length == 7U);
  CHECK(response_has_valid_crc(response, response_length));
  CHECK(slave.receiver.frames_ready == 1U);
}

/*
 * A partial frame cannot survive a long idle: the bare-metal main loop keeps
 * polling, so the receiver publishes an abandoned fragment about T3.5 after
 * its last byte, long before the 32-bit timestamp difference turns negative
 * (more than 2^31 us, about 35.8 min). A compliant request arriving after such
 * an idle must therefore start from an empty buffer and be answered cleanly.
 */
static void test_partial_frame_cannot_survive_long_idle(void)
{
  board_a_slave_t slave;
  uint8_t request[8];
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  size_t request_length;
  uint32_t now_us;
  uint32_t idle_us;
  size_t index;
  size_t response_length;
  unsigned int idle_responses = 0U;

  board_a_slave_init(&slave, 0x0000000DU);
  request_length = make_read_request(request, BOARD_A_SLAVE_ADDRESS, 0x03U,
                                     0x0000U, 1U);

  /* Three orphan bytes of a truncated request. */
  now_us = 100000U;
  for (index = 0U; index < 3U; ++index) {
    board_a_slave_push_byte(&slave, request[index], now_us);
    now_us += 1150U;
  }
  CHECK(slave.receiver.active_length == 3U);

  /* The next sample is past T3.5, so the fragment is published on its own. */
  CHECK(board_a_slave_poll(&slave, now_us + 4011U, response,
                           sizeof(response)) == 0U);
  CHECK(slave.receiver.active_length == 0U);
  CHECK(slave.receiver.frames_ready == 1U);
  CHECK(slave.server.stats.malformed_frames == 1U);

  /* 36 minutes of silence, sampled coarsely compared with the real loop. */
  for (idle_us = 1000000U; idle_us <= 36U * 60U * 1000000U;
       idle_us += 1000000U) {
    if (board_a_slave_poll(&slave, now_us + idle_us, response,
                           sizeof(response)) != 0U) {
      idle_responses++;
    }
  }
  CHECK(idle_responses == 0U);
  CHECK(slave.receiver.frames_ready == 1U);
  CHECK(slave.receiver.active_length == 0U);

  /* A request inside the negative-difference range is parsed from scratch. */
  now_us += (uint32_t)(36ULL * 60ULL * 1000000ULL);
  for (index = 0U; index < request_length; ++index) {
    board_a_slave_push_byte(&slave, request[index], now_us);
    now_us += 1150U;
  }
  response_length = board_a_slave_poll(&slave, now_us + 4011U, response,
                                       sizeof(response));
  CHECK(response_length == 7U);
  CHECK(response_has_valid_crc(response, response_length));
  CHECK(slave.receiver.frames_ready == 2U);
  CHECK(slave.server.stats.frames_seen == 1U);
  CHECK(slave.server.stats.crc_errors == 0U);
}

/*
 * The heartbeat gate used to compare a 64-bit clock against a 32-bit deadline
 * ((uint32_t)now_us >= next_log_us, next_log_us += 5000000U). Both sides
 * wrapped every ~71.6 minutes and the deadline fell behind the clock, so the
 * firmware printed back-to-back lines for seconds and then went silent for
 * most of the next cycle. Keep the new schedule honest:
 *   - exactly one line per period, never two inside one period,
 *   - behaviour is identical across the 32-bit microsecond wrap,
 *   - a long stall prints once and re-phases instead of replaying.
 */
static bool legacy_heartbeat_due(uint32_t *next_log_us, uint64_t now_us)
{
  if ((uint32_t)now_us >= *next_log_us) {
    *next_log_us += 5000000U;
    return true;
  }
  return false;
}

static void test_log_schedule_period_and_32bit_wrap(void)
{
  board_a_log_schedule_t schedule;
  uint32_t legacy_next = 5000000U;
  uint64_t now_us;
  uint64_t last_new_print_us = 0U;
  uint64_t last_legacy_print_us = 0U;
  uint64_t new_min_interval_us = UINT64_MAX;
  uint64_t legacy_min_interval_us = UINT64_MAX;
  unsigned int new_prints = 0U;
  unsigned int legacy_prints = 0U;

  /* Plain 5 s period. */
  board_a_log_schedule_init(&schedule, 5000000U, 0U);
  CHECK(!board_a_log_schedule_due(&schedule, 4999999U));
  CHECK(board_a_log_schedule_due(&schedule, 5000000U));
  CHECK(!board_a_log_schedule_due(&schedule, 9999999U));
  CHECK(board_a_log_schedule_due(&schedule, 10000000U));

  /* A 60 s stall prints once and re-phases, it does not replay. */
  board_a_log_schedule_init(&schedule, 5000000U, 0U);
  CHECK(!board_a_log_schedule_due(&schedule, 4999999U));
  CHECK(board_a_log_schedule_due(&schedule, 60000000U));
  CHECK(!board_a_log_schedule_due(&schedule, 60000000U));
  CHECK(!board_a_log_schedule_due(&schedule, 64999999U));
  CHECK(board_a_log_schedule_due(&schedule, 65000000U));

  /*
   * Drive both the new schedule and the old 32-bit gate from 0 across the
   * 4294.967296 s wrap in 1 ms steps; the old gate must show a burst (prints
   * 1 ms apart) while the new one keeps its 5 s spacing.
   */
  board_a_log_schedule_init(&schedule, 5000000U, 0U);
  for (now_us = 0U; now_us <= 4310000000ULL; now_us += 1000U) {
    if (board_a_log_schedule_due(&schedule, now_us)) {
      new_prints++;
      if ((last_new_print_us != 0U) &&
          ((now_us - last_new_print_us) < new_min_interval_us)) {
        new_min_interval_us = now_us - last_new_print_us;
      }
      last_new_print_us = now_us;
    }
    if (legacy_heartbeat_due(&legacy_next, now_us)) {
      legacy_prints++;
      if ((last_legacy_print_us != 0U) &&
          ((now_us - last_legacy_print_us) < legacy_min_interval_us)) {
        legacy_min_interval_us = now_us - last_legacy_print_us;
      }
      last_legacy_print_us = now_us;
    }
  }

  CHECK(new_prints == (unsigned int)(4310000000ULL / 5000000ULL));
  CHECK(new_min_interval_us == 5000000U);
  CHECK(legacy_prints > new_prints);
  CHECK(legacy_min_interval_us <= 1000U);
}

static void test_commands_and_scheduler(void)
{
  board_a_slave_t slave;
  uint16_t config[3] = {10U, 0x0001U, 2U};
  uint16_t value;

  board_a_slave_init(&slave, 0x00000009U);

  {
    CHECK(command(&slave, BOARD_A_COMMAND_SAVE_CONFIG));
    {
      uint8_t request[8];
      size_t request_length = make_write_single_request(
          request, BOARD_A_SLAVE_ADDRESS, BOARD_A_HOLDING_COMMAND,
          BOARD_A_COMMAND_SAVE_CONFIG);
      CHECK(expect_exception(&slave, request, request_length, 0x06U));
    }
  }
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_PERSISTENCE_STATUS) == 1U);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_COMMAND_RESULT) ==
        BOARD_A_COMMAND_RESULT_REJECTED);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_PERSISTENCE_ERRORS_LO) == 1U);

  CHECK(write_multiple(&slave, BOARD_A_HOLDING_CFG_PERIOD_SEC, config, 3U));
  CHECK(command(&slave, BOARD_A_COMMAND_APPLY_CONFIG));
  CHECK(read_one(&slave, 0x04U,
                 BOARD_A_INPUT_ACTIVE_CONFIG_VERSION_LO) == 1U);

  CHECK(command(&slave, BOARD_A_COMMAND_START));
  CHECK(command(&slave, BOARD_A_COMMAND_START));
  board_a_slave_tick(&slave, 1000000ULL);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_RECORD_SEQUENCE_LO) == 1U);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_RUN_STATE) == 1U);
  board_a_slave_tick(&slave, 11000000ULL);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_RECORD_SEQUENCE_LO) == 2U);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_RUN_STATE) == 0U);
  CHECK(read_one(&slave, 0x04U, BOARD_A_INPUT_RECORDS_THIS_RUN_LO) == 2U);

  CHECK(command(&slave, BOARD_A_COMMAND_STOP));
  CHECK(command(&slave, BOARD_A_COMMAND_STOP));
  value = read_one(&slave, 0x04U, BOARD_A_INPUT_RUN_STATE);
  CHECK(value == 0U);
}

static void test_frame_separation(void)
{
  board_a_slave_t slave;
  uint8_t first[8];
  uint8_t second[8];
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  size_t first_length;
  size_t second_length;
  uint32_t now_us = 300000U;
  size_t index;

  board_a_slave_init(&slave, 0x0000000AU);
  first_length = make_read_request(first, BOARD_A_SLAVE_ADDRESS, 0x03U,
                                   0x0000U, 1U);
  second_length = make_read_request(second, BOARD_A_SLAVE_ADDRESS, 0x03U,
                                    0x0001U, 1U);

  for (index = 0U; index < first_length; ++index) {
    board_a_slave_push_byte(&slave, first[index], now_us);
    now_us += 100U;
  }
  now_us += 4011U;
  board_a_slave_push_byte(&slave, second[0], now_us);
  CHECK(board_a_slave_poll(&slave, now_us, response, sizeof(response)) ==
        first_length - 1U);
  now_us += 100U;
  for (index = 1U; index < second_length; ++index) {
    board_a_slave_push_byte(&slave, second[index], now_us);
    now_us += 100U;
  }
  CHECK(board_a_slave_poll(&slave, now_us + 5000U, response,
                           sizeof(response)) == second_length - 1U);
}

static void test_alarm_result_snapshot(void)
{
  board_a_model_t model;
  board_a_alarm_result_t published;
  board_a_alarm_result_t copied;
  board_a_alarm_state_t state;

  board_a_model_init(&model, 7U);
  memset(&published, 0, sizeof(published));
  published.event = true;
  published.event_type = BOARD_A_ALARM_EVENT_RAISED;
  published.event_id = 9U;
  published.event_time_ms = 123456U;
  published.state.valid = true;
  published.state.level = BOARD_A_ALARM_WARNING;
  published.state.reason = BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH;
  published.state.trigger_phase = BOARD_A_ALARM_PHASE_C;
  published.state.delta_valid = true;
  published.state.maximum_delta_x16 = 240;

  board_a_model_publish_alarm_result(&model, &published);
  CHECK(board_a_model_copy_alarm_event(&model, &copied));
  CHECK(copied.event);
  CHECK(copied.event_type == BOARD_A_ALARM_EVENT_RAISED);
  CHECK(copied.event_id == 9U);
  CHECK(copied.event_time_ms == 123456U);
  CHECK(copied.state.level == BOARD_A_ALARM_WARNING);
  CHECK(copied.state.trigger_phase == BOARD_A_ALARM_PHASE_C);
  CHECK(board_a_model_copy_alarm_state(&model, &state));
  CHECK(state.level == BOARD_A_ALARM_WARNING);
  CHECK(state.maximum_delta_x16 == 240);
}

int main(void)
{
  test_crc_standard_vector();
  test_normal_reads();
  test_single_write_and_snapshot();
  test_single_dedup_window();
  test_ds18b20_snapshot_modbus_block();
  test_write_single_and_multiple();
  test_exception_responses();
  test_length_and_byte_count_errors();
  test_no_response_cases();
  test_fragmented_frames_and_overlong();
  test_poll_timestamp_older_than_isr_timestamp();
  test_partial_frame_cannot_survive_long_idle();
  test_log_schedule_period_and_32bit_wrap();
  test_commands_and_scheduler();
  test_frame_separation();
  test_alarm_result_snapshot();

  printf("board_a host tests: %u checks, %u failures\n",
         g_checks, g_failures);
  return g_failures == 0U ? 0 : 1;
}
