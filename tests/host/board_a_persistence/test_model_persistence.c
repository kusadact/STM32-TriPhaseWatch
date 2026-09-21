#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_a_model.h"
#include "board_a_persistence.h"

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
      return 1;                                                              \
    }                                                                        \
  } while (0)

static modbus_result_t write_config(board_a_model_t *model,
                                    uint16_t period,
                                    uint16_t mask,
                                    uint16_t count)
{
  uint16_t values[3] = {period, mask, count};

  return board_a_model_write_registers(
      model, BOARD_A_HOLDING_CFG_PERIOD_SEC, values, 3U, 1000U);
}

static modbus_result_t execute_command(board_a_model_t *model,
                                       uint16_t command,
                                       uint32_t command_id,
                                       uint64_t now_us)
{
  uint16_t values[3] = {
    command,
    (uint16_t)(command_id >> 16U),
    (uint16_t)command_id,
  };

  return board_a_model_write_registers(
      model, BOARD_A_HOLDING_COMMAND, values, 3U, now_us);
}

static int test_save_mailbox_capture_and_busy(void)
{
  board_a_model_t model;
  board_a_save_request_t request;
  board_a_persistence_status_t status;
  uint16_t persistence_registers[48];

  board_a_model_init(&model, 1U);
  CHECK(write_config(&model, 20U, 3U, 4U) == MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_APPLY_CONFIG, 0U, 1000U) ==
        MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_SAVE_CONFIG,
                        0x11223344U, 1100U) == MODBUS_RESULT_OK);
  CHECK(model.persistence.save.state == BOARD_A_SAVE_PENDING);
  CHECK(model.persistence.save.request.command_id == 0x11223344U);
  CHECK(model.persistence.save.request.config.period_sec == 20U);
  CHECK(execute_command(&model, BOARD_A_COMMAND_SAVE_CONFIG,
                        0x55667788U, 1200U) == MODBUS_RESULT_SLAVE_BUSY);
  CHECK(model.persistence.save.request.command_id == 0x11223344U);

  CHECK(write_config(&model, 30U, 7U, 5U) == MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_APPLY_CONFIG, 0U, 1300U) ==
        MODBUS_RESULT_OK);
  CHECK(board_a_model_claim_save(&model, &request));
  CHECK(request.config.period_sec == 20U);
  board_a_model_complete_save(&model, 1, BOARD_A_SAVE_ERROR_NONE, 0U);
  CHECK(model.persistence.save.state == BOARD_A_SAVE_SUCCESS);
  CHECK(model.active_config.config.period_sec == 30U);
  CHECK(model.active_config.config.channel_mask == 7U);

  board_a_persistence_status(&model.persistence,
                             model.active_config.version, &status);
  CHECK(status.save_state == BOARD_A_SAVE_SUCCESS);
  CHECK(status.save_command_id == 0x11223344U);
  CHECK(status.captured_period == 20U);
  CHECK(status.captured_mask == 3U);
  CHECK(status.captured_count == 4U);
  CHECK(status.active_config_version == 2U);
  CHECK(board_a_model_read_registers(
      &model, MODBUS_REGISTER_INPUT, BOARD_A_INPUT_CONTRACT_REVISION,
      48U, persistence_registers, 0U) == MODBUS_RESULT_OK);
  CHECK(((uint32_t)persistence_registers[2] << 16U |
         persistence_registers[3]) == 0x11223344U);
  CHECK(((uint32_t)persistence_registers[4] << 16U |
         persistence_registers[5]) == 1U);
  CHECK(persistence_registers[0x24] == 0U);
  CHECK(persistence_registers[0x25] == 20U);
  CHECK(persistence_registers[0x26] == 3U);
  CHECK(persistence_registers[0x27] == 4U);
  return 0;
}

static int test_save_failure_and_startup_load(void)
{
  board_a_model_t model;
  board_a_save_request_t request;
  board_a_persisted_config_t loaded = {45U, 9U, 12U};

  board_a_model_init(&model, 2U);
  board_a_model_apply_loaded_config(&model, &loaded, 41U);
  CHECK(model.pending_config.period_sec == 45U);
  CHECK(model.active_config.config.channel_mask == 9U);
  CHECK(model.persistence.config_load_state ==
        BOARD_A_CONFIG_LOAD_SUCCESS);
  CHECK(model.persistence.load_sequence == 41U);

  CHECK(execute_command(&model, BOARD_A_COMMAND_SAVE_CONFIG,
                        0x00000099U, 2000U) == MODBUS_RESULT_OK);
  CHECK(board_a_model_claim_save(&model, &request));
  board_a_model_complete_save(&model, 0, BOARD_A_SAVE_ERROR_IO, 5U);
  CHECK(model.persistence.save.state == BOARD_A_SAVE_FAILED);
  CHECK(model.persistence.save.error == BOARD_A_SAVE_ERROR_IO);
  CHECK(model.active_config.config.period_sec == 45U);
  CHECK(model.pending_config.period_sec == 45U);

  board_a_model_init(&model, 3U);
  board_a_model_note_config_load(
      &model, BOARD_A_CONFIG_LOAD_DEFAULT_NO_RECORD, 0U);
  CHECK(model.persistence.config_load_state ==
        BOARD_A_CONFIG_LOAD_DEFAULT_NO_RECORD);
  CHECK(model.active_config.config.period_sec == 10U);

  board_a_model_note_config_load(
      &model, BOARD_A_CONFIG_LOAD_DEFAULT_ERROR, 0U);
  CHECK(model.persistence.config_load_state ==
        BOARD_A_CONFIG_LOAD_DEFAULT_ERROR);
  return 0;
}

static int test_single_and_periodic_records(void)
{
  board_a_model_t model;
  board_a_record_format_record_t record;
  uint32_t generated;

  board_a_model_init(&model, 0xCAFEBABEU);
  CHECK(write_config(&model, 10U, 15U, 0U) == MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_APPLY_CONFIG, 0U, 1000U) ==
        MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_SINGLE, 1U, 2500000U) ==
        MODBUS_RESULT_OK);
  generated = model.persistence.storage.generated;
  CHECK(board_a_model_pop_record(&model, &record));
  CHECK(record.trigger == BOARD_A_SAMPLE_TRIGGER_SINGLE);
  CHECK(record.planned_ms == 2500U);
  CHECK(record.actual_ms == 2500U);
  CHECK(record.sequence == 1U);
  CHECK(record.values[0] == 10U);
  CHECK(record.values[3] == 13U);
  CHECK(record.config_version == 1U);
  board_a_model_complete_record(
      &model, &record, BOARD_A_RECORD_COMPLETE_SYNCED);

  CHECK(execute_command(&model, BOARD_A_COMMAND_SINGLE, 1U, 2600000U) ==
        MODBUS_RESULT_OK);
  CHECK(model.persistence.storage.generated == generated);
  CHECK(model.snapshot.sequence == 1U);

  CHECK(execute_command(&model, BOARD_A_COMMAND_START, 0U, 3000000U) ==
        MODBUS_RESULT_OK);
  board_a_model_tick(&model, 3000000U);
  CHECK(board_a_model_pop_record(&model, &record));
  CHECK(record.trigger == BOARD_A_SAMPLE_TRIGGER_PERIODIC);
  CHECK(record.planned_ms == 3000U);
  CHECK(record.actual_ms == 3000U);
  board_a_model_complete_record(
      &model, &record, BOARD_A_RECORD_COMPLETE_SYNCED);

  board_a_model_tick(&model, 13000000U);
  CHECK(board_a_model_pop_record(&model, &record));
  CHECK(record.planned_ms == 13000U);
  CHECK(record.actual_ms == 13000U);
  board_a_model_complete_record(
      &model, &record, BOARD_A_RECORD_COMPLETE_SYNCED);
  return 0;
}

static int test_record_config_snapshot_and_utc(void)
{
  board_a_model_t model;
  board_a_record_format_record_t record;
  uint16_t time_words[4] = {0U, 1709164800U >> 16U,
                            0U, 1709164800U & 0xFFFFU};

  board_a_model_init(&model, 7U);
  CHECK(write_config(&model, 20U, 3U, 0U) == MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_APPLY_CONFIG, 0U, 1000U) ==
        MODBUS_RESULT_OK);

  time_words[0] = (uint16_t)(1709164800U >> 16U);
  time_words[1] = (uint16_t)1709164800U;
  CHECK(board_a_model_write_registers(
            &model, BOARD_A_HOLDING_PENDING_UTC_SECONDS_HI,
            time_words, 2U, 2000000U) == MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_SET_TIME, 0U, 2000000U) ==
        MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_SINGLE, 2U, 2500000U) ==
        MODBUS_RESULT_OK);

  CHECK(write_config(&model, 30U, 7U, 5U) == MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_APPLY_CONFIG, 0U, 3000U) ==
        MODBUS_RESULT_OK);
  CHECK(board_a_model_pop_record(&model, &record));
  CHECK(record.utc_valid == 1U);
  CHECK(record.utc_seconds == 1709164800U);
  CHECK(record.period_sec == 20U);
  CHECK(record.channel_mask == 3U);
  CHECK(record.config_version == 1U);
  CHECK(record.utc_seconds == 1709164800U);
  return 0;
}

static int test_queue_full_drop_new(void)
{
  board_a_model_t model;
  uint32_t index;

  board_a_model_init(&model, 8U);
  CHECK(write_config(&model, 10U, 1U, 0U) == MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_APPLY_CONFIG, 0U, 0U) ==
        MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_START, 0U, 0U) ==
        MODBUS_RESULT_OK);

  for (index = 0U; index < 40U; index++) {
    board_a_model_tick(&model,
                       (uint64_t)index * 10000000ULL);
  }
  CHECK(model.persistence.storage.generated == 40U);
  CHECK(model.persistence.storage.dropped == 8U);
  CHECK(model.persistence.storage.count == BOARD_A_RECORD_QUEUE_CAPACITY);
  CHECK(model.persistence.storage.records[0].sequence == 1U);
  CHECK(model.persistence.storage.records[
            BOARD_A_RECORD_QUEUE_CAPACITY - 1U].sequence == 32U);
  CHECK(board_a_persistence_invariant_holds(&model.persistence));
  return 0;
}

static int test_stop_drain_and_finite_stop(void)
{
  board_a_model_t model;
  board_a_persistence_status_t status;
  uint32_t generation;

  board_a_model_init(&model, 9U);
  CHECK(execute_command(&model, BOARD_A_COMMAND_STOP, 0U, 1000U) ==
        MODBUS_RESULT_OK);
  CHECK(model.persistence.storage.drain_state == BOARD_A_DRAIN_PENDING);
  generation = model.persistence.storage.drain_generation;
  CHECK(generation == 1U);
  CHECK(execute_command(&model, BOARD_A_COMMAND_START, 0U, 2000U) ==
        MODBUS_RESULT_SLAVE_BUSY);
  CHECK(execute_command(&model, BOARD_A_COMMAND_SINGLE, 1U, 2000U) ==
        MODBUS_RESULT_SLAVE_BUSY);
  board_a_model_complete_drain(&model, generation, 1);
  CHECK(model.persistence.storage.drain_state == BOARD_A_DRAIN_DONE);
  CHECK(execute_command(&model, BOARD_A_COMMAND_START, 0U, 3000U) ==
        MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_STOP, 0U, 4000U) ==
        MODBUS_RESULT_OK);
  generation = model.persistence.storage.drain_generation;
  board_a_model_complete_drain(&model, generation, 0);
  CHECK(model.persistence.storage.drain_state == BOARD_A_DRAIN_FAILED);

  board_a_model_init(&model, 10U);
  CHECK(write_config(&model, 10U, 1U, 2U) == MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_APPLY_CONFIG, 0U, 0U) ==
        MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_START, 0U, 0U) ==
        MODBUS_RESULT_OK);
  board_a_model_tick(&model, 0U);
  board_a_model_tick(&model, 10000000U);
  CHECK(model.run_state == BOARD_A_RUN_STOPPED);
  CHECK(model.persistence.storage.drain_state == BOARD_A_DRAIN_PENDING);
  CHECK(model.persistence.storage.drain_generation == 1U);
  CHECK(board_a_persistence_invariant_holds(&model.persistence));

  CHECK(board_a_model_read_registers(
      &model, MODBUS_REGISTER_INPUT, BOARD_A_INPUT_CONTRACT_REVISION,
      48U, (uint16_t[48]){0}, 0U) == MODBUS_RESULT_OK);
  board_a_persistence_status(&model.persistence,
                             model.active_config.version, &status);
  CHECK(status.generated == model.persistence.storage.generated);
  CHECK(status.queued == model.persistence.storage.count);
  CHECK(status.drain_state == BOARD_A_DRAIN_PENDING);
  return 0;
}

static int test_extended_status_block(void)
{
  board_a_model_t model;
  uint16_t values[48];
  uint8_t index;

  board_a_model_init(&model, 11U);
  CHECK(execute_command(&model, BOARD_A_COMMAND_SINGLE, 0x1234U, 5000U) ==
        MODBUS_RESULT_OK);
  CHECK(board_a_model_read_registers(
      &model, MODBUS_REGISTER_INPUT, BOARD_A_INPUT_CONTRACT_REVISION,
      48U, values, 5000U) == MODBUS_RESULT_OK);
  CHECK(values[0] == BOARD_A_PERSISTENCE_CONTRACT_REVISION);
  CHECK(values[1] == BOARD_A_SAVE_IDLE);
  CHECK(values[10] == 1U);
  CHECK(values[11] == 1U);
  CHECK(((uint32_t)values[12] << 16U | values[13]) == 1U);
  CHECK(((uint32_t)values[16] << 16U | values[17]) == 0U);
  CHECK(values[20] == 0U);
  CHECK(values[21] == BOARD_A_DRAIN_NONE);
  CHECK(values[0x24] == 0U);
  CHECK(values[0x25] == 0U);
  CHECK(values[0x26] == 0U);
  CHECK(values[0x27] == 0U);
  for (index = 0x28U; index <= 0x2FU; index++) {
    CHECK(values[index] == 0U);
  }
  return 0;
}

static int test_real_dht11_record_and_register_link(void)
{
  board_a_model_t model;
  board_a_sensor_snapshot_t sensors;
  board_a_record_format_record_t record;
  uint16_t values[24];
  uint8_t index;

  board_a_model_init(&model, 12U);
  memset(&sensors, 0, sizeof(sensors));
  sensors.sample_id = 42U;
  sensors.valid_mask = 0x0007U;
  sensors.sample_time_us = 1000000U;
  for (index = 0U; index < BOARD_A_SENSOR_COUNT; ++index) {
    sensors.sensors[index].sensor_id = index;
    sensors.sensors[index].sensor_type = BOARD_A_SENSOR_TYPE_DHT11;
    sensors.sensors[index].has_value = true;
    sensors.sensors[index].temperature_x10 =
        (uint16_t)(200U + (index * 10U));
    sensors.sensors[index].humidity_x10 =
        (uint16_t)(400U + (index * 10U));
    sensors.sensors[index].quality = BOARD_A_QUALITY_OK;
    sensors.sensors[index].error = BOARD_A_SENSOR_ERROR_NONE;
    sensors.sensors[index].sample_time_ms = 1000U + index;
  }

  CHECK(board_a_model_set_data_source(
      &model, BOARD_A_DATA_SOURCE_REAL_DHT11));
  board_a_model_publish_sensor_snapshot(&model, &sensors);
  CHECK(board_a_model_read_registers(
      &model, MODBUS_REGISTER_INPUT,
      BOARD_A_INPUT_DHT11_CONTRACT_REVISION, 5U, values, 1000000U) ==
      MODBUS_RESULT_OK);
  CHECK(((uint32_t)values[3] << 16U | values[4]) == 42U);
  CHECK(write_config(&model, 10U, 7U, 0U) == MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_APPLY_CONFIG, 0U, 0U) ==
        MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_START, 0U, 0U) ==
        MODBUS_RESULT_OK);
  board_a_model_tick(&model, 1000000U);
  CHECK(board_a_model_pop_record(&model, &record));
  CHECK(record.source == BOARD_A_DATA_SOURCE_REAL_DHT11);
  CHECK(record.sequence == 1U);
  CHECK(record.dht_valid_mask == 0x0007U);
  CHECK(record.dht_sample_id == 42U);
  CHECK(record.dht_temperature_x10[0] == 200U);
  CHECK(record.dht_temperature_x10[1] == 210U);
  CHECK(record.dht_temperature_x10[2] == 220U);
  CHECK(record.dht_humidity_x10[0] == 400U);
  CHECK(record.dht_humidity_x10[2] == 420U);
  CHECK(record.dht_quality[0] == BOARD_A_QUALITY_OK);
  CHECK(record.dht_sample_time_ms[2] == 1002U);

  CHECK(board_a_model_read_registers(
      &model, MODBUS_REGISTER_INPUT,
      BOARD_A_INPUT_DHT11_CONTRACT_REVISION,
      (uint16_t)(sizeof(values) / sizeof(values[0])), values,
      1000000U) == MODBUS_RESULT_OK);
  CHECK(values[0] == 1U);
  CHECK(values[1] == BOARD_A_DATA_SOURCE_REAL_DHT11);
  CHECK(values[2] == 0x0007U);
  CHECK(((uint32_t)values[3] << 16U | values[4]) ==
        record.dht_sample_id);
  CHECK(values[5] == 200U);
  CHECK(values[8] == 400U);
  CHECK(values[11] == BOARD_A_QUALITY_OK);
  CHECK(values[14] == BOARD_A_SENSOR_ERROR_NONE);
  CHECK(((uint32_t)values[17] << 16U | values[18]) == 1000U);
  CHECK(((uint32_t)values[21] << 16U | values[22]) == 1002U);
  CHECK(values[23] == BOARD_A_SENSOR_TYPE_DHT11);
  return 0;
}

static int test_real_dht11_not_present_record(void)
{
  board_a_model_t model;
  board_a_sensor_snapshot_t sensors;
  board_a_record_format_record_t record;
  uint8_t index;

  board_a_model_init(&model, 13U);
  memset(&sensors, 0, sizeof(sensors));
  sensors.sample_id = 7U;
  for (index = 0U; index < BOARD_A_SENSOR_COUNT; ++index) {
    sensors.sensors[index].sensor_id = index;
    sensors.sensors[index].sensor_type = BOARD_A_SENSOR_TYPE_DHT11;
    sensors.sensors[index].quality = BOARD_A_QUALITY_NOT_PRESENT;
    sensors.sensors[index].sample_time_ms = 500U + index;
  }
  CHECK(board_a_model_set_data_source(
      &model, BOARD_A_DATA_SOURCE_REAL_DHT11));
  board_a_model_publish_sensor_snapshot(&model, &sensors);
  CHECK(write_config(&model, 10U, 7U, 0U) == MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_APPLY_CONFIG, 0U, 0U) ==
        MODBUS_RESULT_OK);
  CHECK(execute_command(&model, BOARD_A_COMMAND_START, 0U, 0U) ==
        MODBUS_RESULT_OK);
  board_a_model_tick(&model, 1000000U);
  CHECK(board_a_model_pop_record(&model, &record));
  CHECK(record.source == BOARD_A_DATA_SOURCE_REAL_DHT11);
  CHECK(record.dht_sample_id == 7U);
  CHECK(record.dht_valid_mask == 0U);
  CHECK(record.dht_quality[0] == BOARD_A_QUALITY_NOT_PRESENT);
  CHECK(record.dht_temperature_x10[0] == 0U);
  CHECK(record.dht_humidity_x10[0] == 0U);
  return 0;
}

int main(void)
{
  CHECK(test_save_mailbox_capture_and_busy() == 0);
  CHECK(test_save_failure_and_startup_load() == 0);
  CHECK(test_single_and_periodic_records() == 0);
  CHECK(test_record_config_snapshot_and_utc() == 0);
  CHECK(test_queue_full_drop_new() == 0);
  CHECK(test_stop_drain_and_finite_stop() == 0);
  CHECK(test_extended_status_block() == 0);
  CHECK(test_real_dht11_record_and_register_link() == 0);
  CHECK(test_real_dht11_not_present_record() == 0);
  puts("PASS test_model_persistence");
  return 0;
}
