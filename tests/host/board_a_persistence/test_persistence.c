#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_a_model.h"
#include "board_a_persistence.h"
#include "board_a_record_format.h"
#include "board_a_storage_engine.h"

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
      return 1;                                                              \
    }                                                                        \
  } while (0)

static void persisted_config_defaults(board_a_persisted_config_t *config)
{
  memset(config, 0, sizeof(*config));
  board_a_alarm_default_config(&config->alarm);
}

static void put_le16(uint8_t *data, uint16_t value)
{
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8U);
}

static board_a_record_format_record_t make_record(uint32_t sequence,
                                                  uint8_t utc_valid,
                                                  uint32_t utc_seconds)
{
  board_a_record_format_record_t record;
  uint8_t channel;

  memset(&record, 0, sizeof(record));
  record.session_id = 0x12345678U;
  record.sequence = sequence;
  record.trigger = 1U;
  record.planned_ms = 1000U + sequence;
  record.actual_ms = 1005U + sequence;
  record.utc_valid = utc_valid;
  record.utc_seconds = (utc_valid != 0U) ? utc_seconds : 0U;
  record.config_version = 7U;
  record.period_sec = 10U;
  record.channel_mask = 0x000FU;
  record.sample_count = 0U;
  record.source = 1U;
  for (channel = 0U; channel < BOARD_A_RECORD_CHANNEL_COUNT; channel++) {
    record.values[channel] = (uint16_t)((sequence * 10U) + channel);
    record.units[channel] = 1U;
    record.qualities[channel] = 1U;
  }
  return record;
}

static int test_config_payload_codec(void)
{
  static const uint8_t known_rom[8] = {
      0x28U, 0xFFU, 0x64U, 0x1EU, 0x5BU, 0x16U, 0x03U, 0x75U};
  const board_a_config_t cases[] = {
    {10U, 1U, 0U},
    {3600U, 15U, 65535U},
    {1234U, 0x000AU, 42U},
  };
  uint8_t payload[BOARD_A_CONFIG_PAYLOAD_SIZE];
  board_a_persisted_config_t config;
  board_a_persisted_config_t decoded;
  size_t index;

  for (index = 0U; index < (sizeof(cases) / sizeof(cases[0])); index++) {
    persisted_config_defaults(&config);
    config.period_sec = cases[index].period_sec;
    config.channel_mask = cases[index].channel_mask;
    config.record_count = cases[index].record_count;
    CHECK(board_a_config_payload_encode(&config, payload,
                                        sizeof(payload)));
    CHECK(payload[0] == 'B');
    CHECK(payload[1] == '4');
    CHECK(payload[2] == BOARD_A_CONFIG_PAYLOAD_SCHEMA);
    CHECK(payload[3] == 0U);
    CHECK(payload[10] == config.sensor_valid_mask);
    CHECK(payload[11] == 0U);
    CHECK(board_a_config_payload_decode(payload, sizeof(payload), &decoded));
    CHECK(decoded.period_sec == config.period_sec);
    CHECK(decoded.channel_mask == config.channel_mask);
    CHECK(decoded.record_count == config.record_count);
    CHECK(decoded.sensor_valid_mask == config.sensor_valid_mask);
    CHECK(memcmp(&decoded.alarm, &config.alarm, sizeof(config.alarm)) == 0);
  }
  {
    board_a_persisted_config_t mapped;

    persisted_config_defaults(&mapped);
    mapped.period_sec = 10U;
    mapped.channel_mask = 0x0001U;
    mapped.sensor_valid_mask = 0x01U;
    memcpy(mapped.sensor_roms[0], known_rom, sizeof(known_rom));
    mapped.alarm.phase_notice_x16 = 816;
    mapped.alarm.phase_warning_x16 = 912;
    mapped.alarm.phase_critical_x16 = 1232;
    mapped.alarm.delta_notice_x16 = 96;
    mapped.alarm.delta_warning_x16 = 176;
    mapped.alarm.delta_critical_x16 = 256;
    mapped.alarm.rise_notice_x16_per_min = 96;
    mapped.alarm.rise_warning_x16_per_min = 176;
    mapped.alarm.rise_critical_x16_per_min = 336;
    mapped.alarm.assert_samples = 4U;
    mapped.alarm.clear_samples = 6U;
    mapped.alarm.hysteresis_x16 = 48;
    mapped.alarm.rise_window_samples = 5U;
    mapped.alarm.rise_window_min_ms = 3000000U;
    mapped.alarm.buzzer_enable = 0U;
    CHECK(board_a_config_payload_encode(&mapped, payload, sizeof(payload)));
    CHECK(board_a_config_payload_decode(payload, sizeof(payload), &decoded));
    CHECK(decoded.sensor_valid_mask == 0x01U);
    CHECK(memcmp(decoded.sensor_roms[0], known_rom, sizeof(known_rom)) == 0);
    CHECK(memcmp(&decoded.alarm, &mapped.alarm, sizeof(mapped.alarm)) == 0);
  }
  return 0;
}

static int test_config_payload_rejections(void)
{
  uint8_t payload[BOARD_A_CONFIG_PAYLOAD_SIZE];
  board_a_persisted_config_t config;
  board_a_persisted_config_t decoded;
  board_a_alarm_config_t default_alarm;

  persisted_config_defaults(&config);
  CHECK(!board_a_config_payload_encode(&config, payload,
                                       sizeof(payload) - 1U));
  config.alarm.phase_warning_x16 = config.alarm.phase_notice_x16;
  CHECK(!board_a_config_payload_encode(&config, payload, sizeof(payload)));
  board_a_alarm_default_config(&config.alarm);
  config.period_sec = 9U;
  CHECK(!board_a_config_payload_encode(&config, payload, sizeof(payload)));
  config.period_sec = 10U;
  config.channel_mask = 0U;
  CHECK(!board_a_config_payload_encode(&config, payload, sizeof(payload)));
  config.channel_mask = 1U;
  CHECK(board_a_config_payload_encode(&config, payload, sizeof(payload)));

  CHECK(!board_a_config_payload_validate(payload, 0U));
  CHECK(!board_a_config_payload_validate(payload, 11U));
  CHECK(!board_a_config_payload_validate(payload, 13U));
  CHECK(!board_a_config_payload_validate(
      payload, BOARD_A_CONFIG_PAYLOAD_SCHEMA2_SIZE - 1U));
  CHECK(!board_a_config_payload_validate(
      payload, BOARD_A_CONFIG_PAYLOAD_SCHEMA2_SIZE + 1U));
  payload[0] = 'X';
  CHECK(!board_a_config_payload_validate(payload, sizeof(payload)));
  payload[0] = 'B';
  payload[2] = 4U;
  CHECK(!board_a_config_payload_validate(payload, sizeof(payload)));
  payload[2] = BOARD_A_CONFIG_PAYLOAD_SCHEMA;
  payload[3] = 1U;
  CHECK(!board_a_config_payload_validate(payload, sizeof(payload)));
  payload[3] = 0U;
  payload[10] = 1U;
  CHECK(!board_a_config_payload_validate(payload, sizeof(payload)));
  payload[10] = 0U;
  payload[11] = 1U;
  CHECK(!board_a_config_payload_validate(payload, sizeof(payload)));
  payload[11] = 0U;
  payload[61] = 1U;
  CHECK(!board_a_config_payload_validate(payload, sizeof(payload)));
  payload[61] = 0U;
  payload[4] = 9U;
  CHECK(!board_a_config_payload_validate(payload, sizeof(payload)));
  CHECK(!board_a_config_payload_decode(payload, sizeof(payload), &decoded));
  {
    uint8_t legacy[BOARD_A_CONFIG_PAYLOAD_SCHEMA1_SIZE] = {0U};
    legacy[0] = (uint8_t)'B';
    legacy[1] = (uint8_t)'4';
    legacy[2] = 1U;
    legacy[4] = 10U;
    legacy[6] = 1U;
    CHECK(board_a_config_payload_validate(legacy, sizeof(legacy)));
    CHECK(board_a_config_payload_decode(legacy, sizeof(legacy), &decoded));
    CHECK(decoded.sensor_valid_mask == 0U);
    memset(&default_alarm, 0, sizeof(default_alarm));
    board_a_alarm_default_config(&default_alarm);
    CHECK(memcmp(&decoded.alarm, &default_alarm,
                 sizeof(default_alarm)) == 0);
  }
  {
    uint8_t legacy[BOARD_A_CONFIG_PAYLOAD_SCHEMA2_SIZE] = {0U};

    legacy[0] = (uint8_t)'B';
    legacy[1] = (uint8_t)'4';
    legacy[2] = 2U;
    put_le16(&legacy[4], 20U);
    put_le16(&legacy[6], 0x0003U);
    put_le16(&legacy[8], 7U);
    CHECK(board_a_config_payload_validate(legacy, sizeof(legacy)));
    CHECK(board_a_config_payload_decode(legacy, sizeof(legacy), &decoded));
    CHECK(decoded.period_sec == 20U);
    CHECK(decoded.channel_mask == 0x0003U);
    CHECK(decoded.record_count == 7U);
    CHECK(memcmp(&decoded.alarm, &default_alarm,
                 sizeof(default_alarm)) == 0);
  }
  return 0;
}

static int test_record_csv_and_identity(void)
{
  board_a_record_format_record_t record = make_record(1U, 1U, 1709164800U);
  uint8_t buffer[BOARD_A_RECORD_CSV_MAX_BYTES];
  uint8_t small[BOARD_A_RECORD_CSV_MAX_BYTES];
  size_t length = 0U;
  size_t commas = 0U;
  size_t index;
  uint32_t file_date = 0U;
  char path[BOARD_A_RECORD_PATH_MAX_BYTES];
  static const char expected_header[] =
      "schema,session,seq,trigger,planned_ms,actual_ms,utc_valid,utc_s,"
      "config_version,period_s,mask,sample_count,source,v0,v1,v2,v3,u0,u1,"
      "u2,u3,q0,q1,q2,q3,file_id,file_date,reserved,"
      "ds18b20_valid_mask,ds18b20_sample_id,"
      "ds18b20_0_temp_x16,ds18b20_1_temp_x16,ds18b20_2_temp_x16,"
      "ds18b20_0_quality,ds18b20_1_quality,ds18b20_2_quality,"
      "ds18b20_0_error,ds18b20_1_error,ds18b20_2_error,"
      "ds18b20_0_rom_short,ds18b20_1_rom_short,ds18b20_2_rom_short,"
      "ds18b20_0_sample_ms,ds18b20_1_sample_ms,ds18b20_2_sample_ms\n";

  CHECK(strcmp(board_a_record_format_csv_header(), expected_header) == 0);
  CHECK(board_a_record_format_encode_csv(&record, buffer, sizeof(buffer),
                                         &length));
  CHECK(length > 0U);
  CHECK(length < sizeof(buffer));
  CHECK(buffer[length - 1U] == '\n');
  for (index = 0U; index < length; index++) {
    if (buffer[index] == ',') {
      commas++;
    }
  }
  CHECK(commas == 44U);
  CHECK(memchr(buffer, '\r', length) == NULL);
  CHECK(!board_a_record_format_encode_csv(&record, small, length - 1U,
                                          &index));

  CHECK(board_a_record_format_date_from_utc(0U, &file_date));
  CHECK(file_date == 19700101U);
  CHECK(board_a_record_format_date_from_utc(1709164800U, &file_date));
  CHECK(file_date == 20240229U);
  CHECK(board_a_record_format_make_path(path, sizeof(path), file_date, 1U));
  CHECK(strcmp(path, "0:/LOG/20240229/00000001.CSV") == 0);
  CHECK(board_a_record_format_date_from_utc(0xFFFFFFFEU, &file_date));
  CHECK(file_date == 21060207U);
  CHECK(board_a_record_format_make_path(path, sizeof(path), 0U,
                                        0x89ABCDEFU));
  CHECK(strcmp(path, "0:/LOG/UNSET/89ABCDEF.CSV") == 0);
  CHECK(!board_a_record_format_make_path(path, 8U, file_date, 1U));
  CHECK(!board_a_record_format_make_path(path, sizeof(path), file_date, 0U));
  return 0;
}

static int test_maximum_csv_record(void)
{
  board_a_record_format_record_t record;
  uint8_t buffer[BOARD_A_RECORD_CSV_MAX_BYTES];
  size_t length = 0U;
  uint8_t channel;

  memset(&record, 0xFF, sizeof(record));
  record.sequence = UINT32_MAX;
  record.trigger = 2U;
  record.utc_valid = 1U;
  record.period_sec = 3600U;
  record.channel_mask = 0x000FU;
  record.source = 1U;
  record.planned_ms = UINT64_MAX;
  record.actual_ms = UINT64_MAX;
  for (channel = 0U; channel < BOARD_A_RECORD_CHANNEL_COUNT; channel++) {
    record.units[channel] = 1U;
    record.qualities[channel] = 1U;
  }
  record.ds18b20_valid_mask = 0U;
  record.ds18b20_sample_id = 0U;
  for (channel = 0U; channel < BOARD_A_RECORD_DS18B20_COUNT; channel++) {
    record.ds18b20_temperature_x16[channel] = 0;
    record.ds18b20_quality[channel] = 0U;
    record.ds18b20_error[channel] = 0U;
    record.ds18b20_rom_short[channel] = 0U;
    record.ds18b20_sample_time_ms[channel] = 0U;
  }
  CHECK(board_a_record_format_is_valid(&record));
  CHECK(board_a_record_format_encode_csv(&record, buffer, sizeof(buffer),
                                         &length));
  CHECK(length < sizeof(buffer));
  CHECK(buffer[length - 1U] == '\n');
  record.sequence = 0U;
  CHECK(board_a_record_format_is_valid(&record));
  CHECK(board_a_record_format_encode_csv(&record, buffer, sizeof(buffer),
                                         &length));
  return 0;
}

static int test_queue_drop_and_conservation(void)
{
  board_a_persistence_t persistence;
  board_a_record_format_record_t record;
  board_a_record_format_record_t out;
  uint32_t index;

  board_a_persistence_init(&persistence);
  for (index = 1U; index <= BOARD_A_RECORD_QUEUE_CAPACITY; index++) {
    record = make_record(index, 0U, 0U);
    CHECK(board_a_persistence_queue_push(&persistence, &record) == 1);
    CHECK(board_a_persistence_invariant_holds(&persistence));
  }
  CHECK(persistence.storage.count == BOARD_A_RECORD_QUEUE_CAPACITY);
  CHECK(persistence.storage.high_water == BOARD_A_RECORD_QUEUE_CAPACITY);
  CHECK(persistence.storage.generated == BOARD_A_RECORD_QUEUE_CAPACITY);

  record = make_record(33U, 0U, 0U);
  CHECK(board_a_persistence_queue_push(&persistence, &record) == 0);
  CHECK(persistence.storage.dropped == 1U);
  CHECK(persistence.storage.count == BOARD_A_RECORD_QUEUE_CAPACITY);
  CHECK(board_a_persistence_invariant_holds(&persistence));

  CHECK(board_a_persistence_queue_pop(&persistence, &out));
  CHECK(out.sequence == 1U);
  CHECK(persistence.storage.in_flight == 1U);
  CHECK(board_a_persistence_invariant_holds(&persistence));
  board_a_persistence_queue_requeue(&persistence, &out);
  CHECK(persistence.storage.in_flight == 0U);
  CHECK(persistence.storage.count == BOARD_A_RECORD_QUEUE_CAPACITY);
  CHECK(board_a_persistence_invariant_holds(&persistence));

  CHECK(board_a_persistence_queue_pop(&persistence, &out));
  board_a_persistence_complete_record(
      &persistence, &out, BOARD_A_RECORD_COMPLETE_SYNCED);
  CHECK(persistence.storage.synced == 1U);
  CHECK(persistence.storage.last_synced_seq == 1U);
  CHECK(board_a_persistence_invariant_holds(&persistence));

  CHECK(board_a_persistence_queue_pop(&persistence, &out));
  board_a_persistence_complete_record(
      &persistence, &out, BOARD_A_RECORD_COMPLETE_UNCERTAIN);
  CHECK(persistence.storage.uncertain == 1U);
  CHECK(board_a_persistence_invariant_holds(&persistence));

  while (persistence.storage.count != 0U) {
    CHECK(board_a_persistence_queue_pop(&persistence, &out));
    board_a_persistence_complete_record(
        &persistence, &out, BOARD_A_RECORD_COMPLETE_SYNCED);
    CHECK(board_a_persistence_invariant_holds(&persistence));
  }
  record = make_record(34U, 0U, 0U);
  CHECK(board_a_persistence_queue_push(&persistence, &record) == 1);
  CHECK(board_a_persistence_queue_pop(&persistence, &out));
  CHECK(out.sequence == 34U);
  board_a_persistence_complete_record(
      &persistence, &out, BOARD_A_RECORD_COMPLETE_SYNCED);
  CHECK(board_a_persistence_invariant_holds(&persistence));
  return 0;
}

typedef struct {
  uint32_t now_ms;
  uint32_t exists_remaining;
  uint32_t last_open_date;
  uint32_t last_open_id;
  uint16_t last_length;
  uint16_t header_length;
  uint8_t header[BOARD_A_RECORD_CSV_MAX_BYTES];
  uint8_t bytes[BOARD_A_RECORD_CSV_MAX_BYTES];
  unsigned int open_calls;
  unsigned int probe_calls;
  unsigned int write_calls;
  unsigned int sync_calls;
  unsigned int close_calls;
  unsigned int yield_calls;
  board_a_storage_io_result_t open_result;
  board_a_storage_io_result_t probe_result;
  board_a_storage_io_result_t write_result;
  board_a_storage_io_result_t sync_result;
  board_a_storage_io_result_t close_result;
  uint8_t short_write;
  uint8_t write_fail_call;
  uint8_t short_write_call;
} fake_storage_io_t;

static board_a_storage_io_result_t fake_probe(
    void *context, uint32_t deadline_ms, uint32_t *raw_error)
{
  fake_storage_io_t *fake = context;

  (void)deadline_ms;
  fake->probe_calls++;
  *raw_error = 0x44U;
  return fake->probe_result;
}

static board_a_storage_io_result_t fake_open_new(
    void *context, uint32_t file_date, uint32_t file_id,
    uint32_t deadline_ms, void **handle, uint32_t *raw_error)
{
  fake_storage_io_t *fake = context;

  (void)deadline_ms;
  fake->open_calls++;
  fake->last_open_date = file_date;
  fake->last_open_id = file_id;
  *raw_error = fake->open_calls;
  if (fake->exists_remaining != 0U) {
    fake->exists_remaining--;
    return BOARD_A_STORAGE_IO_EXISTS;
  }
  if (fake->open_result != BOARD_A_STORAGE_IO_OK) {
    return fake->open_result;
  }
  *handle = (void *)&fake->last_open_id;
  return BOARD_A_STORAGE_IO_OK;
}

static board_a_storage_io_result_t fake_write(
    void *context, void *handle, const uint8_t *data, uint16_t length,
    uint32_t deadline_ms, uint16_t *written, uint32_t *raw_error)
{
  fake_storage_io_t *fake = context;

  (void)handle;
  (void)deadline_ms;
  fake->write_calls++;
  fake->last_length = length;
  if (fake->write_calls == 1U) {
    fake->header_length = length;
    memcpy(fake->header, data, length);
  }
  if (length <= sizeof(fake->bytes)) {
    memcpy(fake->bytes, data, length);
  }
  if ((fake->write_fail_call != 0U) &&
      (fake->write_calls == fake->write_fail_call)) {
    *written = 0U;
    *raw_error = 0x55U;
    return fake->write_result;
  }
  *written =
      ((fake->short_write != 0U) &&
       ((fake->short_write_call == 0U) ||
        (fake->write_calls == fake->short_write_call))) ?
      (uint16_t)(length - 1U) : length;
  *raw_error = 0x55U;
  return BOARD_A_STORAGE_IO_OK;
}

static board_a_storage_io_result_t fake_sync(
    void *context, void *handle, uint32_t deadline_ms, uint32_t *raw_error)
{
  fake_storage_io_t *fake = context;

  (void)handle;
  (void)deadline_ms;
  fake->sync_calls++;
  *raw_error = 0x66U;
  return fake->sync_result;
}

static board_a_storage_io_result_t fake_close(
    void *context, void *handle, uint32_t deadline_ms, uint32_t *raw_error)
{
  fake_storage_io_t *fake = context;

  (void)handle;
  (void)deadline_ms;
  fake->close_calls++;
  *raw_error = 0x77U;
  return fake->close_result;
}

static uint32_t fake_now_ms(void *context)
{
  return ((fake_storage_io_t *)context)->now_ms;
}

static void fake_yield(void *context)
{
  fake_storage_io_t *fake = context;

  fake->yield_calls++;
}

static const board_a_storage_io_ops_t FAKE_STORAGE_OPS = {
  .probe = fake_probe,
  .open_new = fake_open_new,
  .write = fake_write,
  .sync = fake_sync,
  .close = fake_close,
  .now_ms = fake_now_ms,
  .yield = fake_yield,
};

static void fake_storage_init(fake_storage_io_t *fake)
{
  memset(fake, 0, sizeof(*fake));
  fake->open_result = BOARD_A_STORAGE_IO_OK;
  fake->probe_result = BOARD_A_STORAGE_IO_OK;
  fake->write_result = BOARD_A_STORAGE_IO_OK;
  fake->sync_result = BOARD_A_STORAGE_IO_OK;
  fake->close_result = BOARD_A_STORAGE_IO_OK;
}

static int test_storage_engine_success_and_drain(void)
{
  fake_storage_io_t fake;
  board_a_storage_engine_t engine;
  board_a_storage_io_ops_t ops = FAKE_STORAGE_OPS;
  board_a_record_format_record_t record = make_record(1U, 1U, 1709164800U);

  fake_storage_init(&fake);
  ops.context = &fake;
  board_a_storage_engine_init(&engine);
  CHECK(board_a_storage_engine_probe(&engine, &ops, 2000U));
  CHECK(fake.probe_calls == 1U);
  CHECK(board_a_storage_engine_process(&engine, &ops, &record, 2000U) ==
        BOARD_A_STORAGE_RECORD_SYNCED);
  CHECK(fake.last_open_date == 20240229U);
  CHECK(fake.last_open_id == 1U);
  CHECK(fake.write_calls == 2U);
  CHECK(fake.sync_calls == 1U);
  CHECK(engine.open_file_id == 1U);
  CHECK(fake.last_length > 0U);
  CHECK(fake.header_length ==
        (uint16_t)strlen(board_a_record_format_csv_header()));
  CHECK(memcmp(fake.header, board_a_record_format_csv_header(),
               fake.header_length) == 0);
  CHECK(board_a_storage_engine_drain(&engine, &ops, 2000U));
  CHECK(fake.close_calls == 1U);
  CHECK(engine.last_error == BOARD_A_STORAGE_ERROR_NONE);
  return 0;
}

static int test_storage_engine_collision_and_date_switch(void)
{
  fake_storage_io_t fake;
  board_a_storage_engine_t engine;
  board_a_storage_io_ops_t ops = FAKE_STORAGE_OPS;
  board_a_record_format_record_t first = make_record(1U, 1U, 1709164800U);
  board_a_record_format_record_t second = make_record(2U, 1U, 1709251200U);

  fake_storage_init(&fake);
  fake.exists_remaining = 3U;
  ops.context = &fake;
  board_a_storage_engine_init(&engine);
  CHECK(board_a_storage_engine_process(&engine, &ops, &first, 2000U) ==
        BOARD_A_STORAGE_RECORD_SYNCED);
  CHECK(fake.last_open_id == 4U);
  CHECK(board_a_storage_engine_process(&engine, &ops, &second, 2000U) ==
        BOARD_A_STORAGE_RECORD_SYNCED);
  CHECK(fake.close_calls == 1U);
  CHECK(fake.last_open_date == 20240301U);
  CHECK(fake.last_open_id == 5U);
  CHECK(first.file_id == 4U);
  CHECK(first.file_date == 20240229U);
  CHECK(second.file_id == 5U);
  CHECK(second.file_date == 20240301U);
  return 0;
}

static int test_synced_identity_updates_persistence(void)
{
  fake_storage_io_t fake;
  board_a_storage_engine_t engine;
  board_a_storage_io_ops_t ops = FAKE_STORAGE_OPS;
  board_a_persistence_t persistence;
  board_a_record_format_record_t first = make_record(1U, 1U, 1709164800U);
  board_a_record_format_record_t second = make_record(2U, 1U, 1709251200U);
  board_a_record_format_record_t queued;
  board_a_persistence_status_t status;
  uint32_t generation;

  fake_storage_init(&fake);
  ops.context = &fake;
  board_a_storage_engine_init(&engine);
  board_a_persistence_init(&persistence);
  board_a_persistence_request_drain(&persistence);
  generation = persistence.storage.drain_generation;

  CHECK(board_a_persistence_queue_push(&persistence, &first) == 1);
  CHECK(board_a_persistence_queue_pop(&persistence, &queued));
  CHECK(board_a_storage_engine_process(&engine, &ops, &queued, 2000U) ==
        BOARD_A_STORAGE_RECORD_SYNCED);
  CHECK(queued.file_id == 1U);
  CHECK(queued.file_date == 20240229U);
  board_a_persistence_complete_record(
      &persistence, &queued, BOARD_A_RECORD_COMPLETE_SYNCED);
  board_a_persistence_status(&persistence, 0U, &status);
  CHECK(status.last_synced_seq == 1U);
  CHECK(status.last_synced_file == 1U);
  CHECK(status.last_synced_date == 20240229U);

  CHECK(board_a_persistence_queue_push(&persistence, &second) == 1);
  CHECK(board_a_persistence_queue_pop(&persistence, &queued));
  CHECK(board_a_storage_engine_process(&engine, &ops, &queued, 2000U) ==
        BOARD_A_STORAGE_RECORD_SYNCED);
  CHECK(queued.file_id == 2U);
  CHECK(queued.file_date == 20240301U);
  board_a_persistence_complete_record(
      &persistence, &queued, BOARD_A_RECORD_COMPLETE_SYNCED);
  board_a_persistence_status(&persistence, 0U, &status);
  CHECK(status.last_synced_seq == 2U);
  CHECK(status.last_synced_file == 2U);
  CHECK(status.last_synced_date == 20240301U);
  CHECK(fake.close_calls == 1U);

  board_a_persistence_complete_drain(&persistence, generation, 1);
  CHECK(persistence.storage.drain_state == BOARD_A_DRAIN_DONE);
  CHECK(board_a_persistence_invariant_holds(&persistence));
  return 0;
}

static int test_storage_engine_failures(void)
{
  fake_storage_io_t fake;
  board_a_storage_engine_t engine;
  board_a_storage_io_ops_t ops = FAKE_STORAGE_OPS;
  board_a_record_format_record_t record = make_record(1U, 0U, 0U);

  fake_storage_init(&fake);
  fake.probe_result = BOARD_A_STORAGE_IO_NOT_READY;
  ops.context = &fake;
  board_a_storage_engine_init(&engine);
  CHECK(!board_a_storage_engine_probe(&engine, &ops, 2000U));
  CHECK(engine.last_error == BOARD_A_STORAGE_ERROR_MOUNT);

  fake_storage_init(&fake);
  fake.open_result = BOARD_A_STORAGE_IO_NOT_READY;
  ops.context = &fake;
  board_a_storage_engine_init(&engine);
  CHECK(board_a_storage_engine_process(&engine, &ops, &record, 2000U) ==
        BOARD_A_STORAGE_RECORD_UNWRITTEN);
  CHECK(engine.last_error == BOARD_A_STORAGE_ERROR_MOUNT);
  CHECK(fake.write_calls == 0U);

  fake_storage_init(&fake);
  fake.write_result = BOARD_A_STORAGE_IO_WRITE;
  fake.write_fail_call = 2U;
  ops.context = &fake;
  board_a_storage_engine_init(&engine);
  CHECK(board_a_storage_engine_process(&engine, &ops, &record, 2000U) ==
        BOARD_A_STORAGE_RECORD_UNCERTAIN);
  CHECK(engine.last_error == BOARD_A_STORAGE_ERROR_WRITE);
  CHECK(fake.close_calls == 1U);

  fake_storage_init(&fake);
  fake.short_write = 1U;
  fake.short_write_call = 2U;
  ops.context = &fake;
  board_a_storage_engine_init(&engine);
  CHECK(board_a_storage_engine_process(&engine, &ops, &record, 2000U) ==
        BOARD_A_STORAGE_RECORD_UNCERTAIN);
  CHECK(fake.sync_calls == 0U);

  fake_storage_init(&fake);
  fake.write_result = BOARD_A_STORAGE_IO_WRITE;
  fake.write_fail_call = 1U;
  ops.context = &fake;
  board_a_storage_engine_init(&engine);
  CHECK(board_a_storage_engine_process(&engine, &ops, &record, 2000U) ==
        BOARD_A_STORAGE_RECORD_UNWRITTEN);
  CHECK(fake.close_calls == 1U);

  fake_storage_init(&fake);
  fake.sync_result = BOARD_A_STORAGE_IO_SYNC;
  ops.context = &fake;
  board_a_storage_engine_init(&engine);
  CHECK(board_a_storage_engine_process(&engine, &ops, &record, 2000U) ==
        BOARD_A_STORAGE_RECORD_UNCERTAIN);
  CHECK(engine.last_error == BOARD_A_STORAGE_ERROR_SYNC);

  fake_storage_init(&fake);
  fake.close_result = BOARD_A_STORAGE_IO_CLOSE;
  ops.context = &fake;
  board_a_storage_engine_init(&engine);
  CHECK(board_a_storage_engine_process(&engine, &ops, &record, 2000U) ==
        BOARD_A_STORAGE_RECORD_SYNCED);
  CHECK(!board_a_storage_engine_drain(&engine, &ops, 2000U));
  CHECK(engine.last_error == BOARD_A_STORAGE_ERROR_CLOSE);
  fake.close_result = BOARD_A_STORAGE_IO_OK;
  CHECK(board_a_storage_engine_probe(&engine, &ops, 2000U));
  CHECK(board_a_storage_engine_drain(&engine, &ops, 2000U));
  return 0;
}

static int test_storage_engine_name_batch_and_deadline(void)
{
  fake_storage_io_t fake;
  board_a_storage_engine_t engine;
  board_a_storage_io_ops_t ops = FAKE_STORAGE_OPS;
  board_a_record_format_record_t record = make_record(1U, 0U, 0U);

  fake_storage_init(&fake);
  fake.exists_remaining = 32U;
  ops.context = &fake;
  board_a_storage_engine_init(&engine);
  CHECK(board_a_storage_engine_process(&engine, &ops, &record, 2000U) ==
        BOARD_A_STORAGE_RECORD_UNWRITTEN);
  CHECK(fake.open_calls == 32U);
  CHECK(fake.yield_calls == 1U);
  CHECK(engine.next_file_id == 33U);
  CHECK(engine.last_error == BOARD_A_STORAGE_ERROR_NAME_EXHAUSTED);

  fake_storage_init(&fake);
  fake.now_ms = 2000U;
  ops.context = &fake;
  board_a_storage_engine_init(&engine);
  CHECK(board_a_storage_engine_process(&engine, &ops, &record, 1999U) ==
        BOARD_A_STORAGE_RECORD_UNWRITTEN);
  CHECK(fake.open_calls == 0U);
  return 0;
}

static int test_queue_requeue_when_full_keeps_old_record(void)
{
  board_a_persistence_t persistence;
  board_a_record_format_record_t record;
  board_a_record_format_record_t out;
  uint32_t index;

  board_a_persistence_init(&persistence);
  for (index = 1U; index <= BOARD_A_RECORD_QUEUE_CAPACITY; index++) {
    record = make_record(index, 0U, 0U);
    CHECK(board_a_persistence_queue_push(&persistence, &record) == 1);
  }

  CHECK(board_a_persistence_queue_pop(&persistence, &out));
  CHECK(out.sequence == 1U);
  CHECK(persistence.storage.count == BOARD_A_RECORD_QUEUE_CAPACITY - 1U);
  CHECK(persistence.storage.in_flight == 1U);

  record = make_record(33U, 0U, 0U);
  CHECK(board_a_persistence_queue_push(&persistence, &record) == 1);
  CHECK(persistence.storage.count == BOARD_A_RECORD_QUEUE_CAPACITY);

  board_a_persistence_queue_requeue(&persistence, &out);
  CHECK(persistence.storage.in_flight == 0U);
  CHECK(persistence.storage.count == BOARD_A_RECORD_QUEUE_CAPACITY);
  CHECK(persistence.storage.dropped == 1U);
  CHECK(persistence.storage.generated == 33U);
  CHECK(board_a_persistence_invariant_holds(&persistence));

  for (index = 1U; index <= BOARD_A_RECORD_QUEUE_CAPACITY; index++) {
    CHECK(board_a_persistence_queue_pop(&persistence, &out));
    CHECK(out.sequence == index);
    board_a_persistence_complete_record(
        &persistence, &out, BOARD_A_RECORD_COMPLETE_SYNCED);
    CHECK(board_a_persistence_invariant_holds(&persistence));
  }
  CHECK(persistence.storage.count == 0U);
  CHECK(persistence.storage.in_flight == 0U);
  CHECK(persistence.storage.synced == BOARD_A_RECORD_QUEUE_CAPACITY);
  CHECK(persistence.storage.dropped == 1U);
  return 0;
}

static int test_record_path_capacity_boundaries(void)
{
  char path[40];

  memset(path, 0xA5, sizeof(path));
  CHECK(!board_a_record_format_make_path(path, 20U, 20260920U, 1U));
  CHECK((unsigned char)path[0] == 0xA5U);

  memset(path, 0xA5, sizeof(path));
  CHECK(!board_a_record_format_make_path(path, 28U, 20260920U, 1U));
  CHECK((unsigned char)path[0] == 0xA5U);

  memset(path, 0xA5, sizeof(path));
  CHECK(board_a_record_format_make_path(path, 29U, 20260920U, 1U));
  CHECK(strcmp(path, "0:/LOG/20260920/00000001.CSV") == 0);
  CHECK((unsigned char)path[29] == 0xA5U);

  memset(path, 0xA5, sizeof(path));
  CHECK(!board_a_record_format_make_path(path, 25U, 0U, 1U));
  CHECK((unsigned char)path[0] == 0xA5U);

  memset(path, 0xA5, sizeof(path));
  CHECK(board_a_record_format_make_path(path, 26U, 0U, 1U));
  CHECK(strcmp(path, "0:/LOG/UNSET/00000001.CSV") == 0);
  CHECK((unsigned char)path[26] == 0xA5U);
  return 0;
}

static int test_ds18b20_csv_payload(void)
{
  fake_storage_io_t fake;
  board_a_storage_engine_t engine;
  board_a_storage_io_ops_t ops = FAKE_STORAGE_OPS;
  board_a_record_format_record_t record = make_record(9U, 1U, 1709164800U);
  uint8_t buffer[BOARD_A_RECORD_CSV_MAX_BYTES + 1U];
  size_t length = 0U;
  uint8_t channel;

  record.source = BOARD_A_DATA_SOURCE_REAL_DS18B20;
  for (channel = 0U; channel < BOARD_A_RECORD_CHANNEL_COUNT; ++channel) {
    record.values[channel] = 0U;
    record.units[channel] = BOARD_A_UNIT_TEMPERATURE_X16;
    record.qualities[channel] = BOARD_A_QUALITY_UNAVAILABLE;
  }
  record.ds18b20_valid_mask = 0x0007U;
  record.ds18b20_sample_id = 42U;
  record.ds18b20_temperature_x16[0] = 230;
  record.ds18b20_temperature_x16[1] = -160;
  record.ds18b20_temperature_x16[2] = 0;
  record.ds18b20_quality[0] = BOARD_A_QUALITY_OK;
  record.ds18b20_quality[1] = BOARD_A_QUALITY_STALE;
  record.ds18b20_quality[2] = BOARD_A_QUALITY_OK;
  record.ds18b20_error[1] = BOARD_A_SENSOR_ERROR_SCRATCHPAD_CRC;
  record.ds18b20_rom_short[0] = 0x1200U;
  record.ds18b20_rom_short[1] = 0x1201U;
  record.ds18b20_rom_short[2] = 0x1202U;
  record.ds18b20_sample_time_ms[0] = 1000U;
  record.ds18b20_sample_time_ms[1] = 2000U;
  record.ds18b20_sample_time_ms[2] = 3000U;

  CHECK(board_a_record_format_is_valid(&record));
  CHECK(board_a_record_format_encode_csv(&record, buffer,
                                         sizeof(buffer) - 1U, &length));
  buffer[length] = 0U;
  CHECK(buffer[0] == (uint8_t)'3');
  CHECK(strstr((const char *)buffer,
               ",7,42,230,-160,0,1,5,1,0,6,0,"
               "4608,4609,4610,1000,2000,3000\n") != NULL);

  fake_storage_init(&fake);
  ops.context = &fake;
  board_a_storage_engine_init(&engine);
  CHECK(board_a_storage_engine_probe(&engine, &ops, 2000U));
  CHECK(board_a_storage_engine_process(&engine, &ops, &record, 2000U) ==
        BOARD_A_STORAGE_RECORD_SYNCED);
  CHECK(fake.last_length > 0U);
  CHECK(fake.last_length < sizeof(fake.bytes));
  fake.bytes[fake.last_length] = 0U;
  CHECK(strstr((const char *)fake.bytes,
               ",7,42,230,-160,0,1,5,1,0,6,0,"
               "4608,4609,4610,1000,2000,3000\n") != NULL);

  record.ds18b20_valid_mask = 0U;
  CHECK(!board_a_record_format_is_valid(&record));
  return 0;
}

int main(void)
{
  CHECK(test_config_payload_codec() == 0);
  CHECK(test_config_payload_rejections() == 0);
  CHECK(test_record_csv_and_identity() == 0);
  CHECK(test_maximum_csv_record() == 0);
  CHECK(test_queue_drop_and_conservation() == 0);
  CHECK(test_storage_engine_success_and_drain() == 0);
  CHECK(test_storage_engine_collision_and_date_switch() == 0);
  CHECK(test_synced_identity_updates_persistence() == 0);
  CHECK(test_storage_engine_failures() == 0);
  CHECK(test_storage_engine_name_batch_and_deadline() == 0);
  CHECK(test_queue_requeue_when_full_keeps_old_record() == 0);
  CHECK(test_record_path_capacity_boundaries() == 0);
  CHECK(test_ds18b20_csv_payload() == 0);
  puts("PASS test_persistence");
  return 0;
}
