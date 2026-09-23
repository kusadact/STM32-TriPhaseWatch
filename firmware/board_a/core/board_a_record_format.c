#include "board_a_record_format.h"

#include <string.h>

#include "board_a_model.h"

static const char BOARD_A_RECORD_CSV_HEADER[] =
    "schema,session,seq,trigger,planned_ms,actual_ms,utc_valid,utc_s,"
    "config_version,period_s,mask,sample_count,source,v0,v1,v2,v3,u0,u1,u2,"
    "u3,q0,q1,q2,q3,file_id,file_date,reserved,"
    "ds18b20_valid_mask,ds18b20_sample_id,"
    "ds18b20_0_temp_x16,ds18b20_1_temp_x16,ds18b20_2_temp_x16,"
    "ds18b20_0_quality,ds18b20_1_quality,ds18b20_2_quality,"
    "ds18b20_0_error,ds18b20_1_error,ds18b20_2_error,"
    "ds18b20_0_rom_short,ds18b20_1_rom_short,ds18b20_2_rom_short,"
    "ds18b20_0_sample_ms,ds18b20_1_sample_ms,ds18b20_2_sample_ms,"
    "event_id,event_phase,event_level,event_reason,event_trigger_phase,"
    "event_max_delta_x16,event_delta_valid,event_flags\n";

_Static_assert(sizeof(BOARD_A_RECORD_CSV_HEADER) <=
                   BOARD_A_RECORD_CSV_MAX_BYTES,
               "CSV header must fit the record buffer");

const char *board_a_record_format_csv_header(void)
{
  return BOARD_A_RECORD_CSV_HEADER;
}

typedef struct {
  uint8_t *buffer;
  size_t capacity;
  size_t length;
  int overflow;
} csv_writer_t;

static void csv_put_byte(csv_writer_t *writer, uint8_t value)
{
  if (writer->length < writer->capacity) {
    writer->buffer[writer->length] = value;
  } else {
    writer->overflow = 1;
  }
  writer->length++;
}

static void csv_put_u64(csv_writer_t *writer, uint64_t value)
{
  uint8_t digits[20];
  uint8_t count = 0U;

  if (value == 0U) {
    csv_put_byte(writer, (uint8_t)'0');
    return;
  }

  while (value != 0U) {
    digits[count] = (uint8_t)('0' + (value % 10U));
    value /= 10U;
    count++;
  }
  while (count != 0U) {
    count--;
    csv_put_byte(writer, digits[count]);
  }
}

static void csv_put_u32(csv_writer_t *writer, uint32_t value)
{
  csv_put_u64(writer, (uint64_t)value);
}

static void csv_put_u16(csv_writer_t *writer, uint16_t value)
{
  csv_put_u64(writer, (uint64_t)value);
}

static void csv_put_i16(csv_writer_t *writer, int16_t value)
{
  uint32_t magnitude;

  if (value < 0) {
    csv_put_byte(writer, (uint8_t)'-');
    magnitude = (uint32_t)(-(int32_t)value);
  } else {
    magnitude = (uint32_t)value;
  }
  csv_put_u32(writer, magnitude);
}

static void csv_put_comma(csv_writer_t *writer)
{
  csv_put_byte(writer, (uint8_t)',');
}

static uint16_t read_le16(const uint8_t *data)
{
  return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

static void write_le16(uint8_t *data, uint16_t value)
{
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8U);
}

static uint32_t read_le32(const uint8_t *data)
{
  return (uint32_t)data[0] |
      ((uint32_t)data[1] << 8U) |
      ((uint32_t)data[2] << 16U) |
      ((uint32_t)data[3] << 24U);
}

static uint64_t read_le64(const uint8_t *data)
{
  return (uint64_t)read_le32(data) |
      ((uint64_t)read_le32(&data[4]) << 32U);
}

static void write_le32(uint8_t *data, uint32_t value)
{
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8U);
  data[2] = (uint8_t)(value >> 16U);
  data[3] = (uint8_t)(value >> 24U);
}

static void write_le64(uint8_t *data, uint64_t value)
{
  write_le32(data, (uint32_t)(value & 0xFFFFFFFFULL));
  write_le32(&data[4], (uint32_t)(value >> 32U));
}

static int ds18b20_error_matches_quality(uint16_t quality, uint16_t error)
{
  if (quality == BOARD_A_QUALITY_OK) {
    return error == BOARD_A_SENSOR_ERROR_NONE;
  }
  if (quality == BOARD_A_QUALITY_TIMEOUT) {
    return (error == BOARD_A_SENSOR_ERROR_RESET_TIMEOUT) ||
        (error == BOARD_A_SENSOR_ERROR_BUS_STUCK_LOW);
  }
  if (quality == BOARD_A_QUALITY_CRC_ERROR) {
    return (error == BOARD_A_SENSOR_ERROR_ROM_CRC) ||
        (error == BOARD_A_SENSOR_ERROR_SCRATCHPAD_CRC);
  }
  if (quality == BOARD_A_QUALITY_RANGE_ERROR) {
    return error == BOARD_A_SENSOR_ERROR_RANGE;
  }
  if (quality == BOARD_A_QUALITY_STALE) {
    return error != BOARD_A_SENSOR_ERROR_NONE;
  }
  if (quality == BOARD_A_QUALITY_NOT_PRESENT) {
    return (error == BOARD_A_SENSOR_ERROR_NONE) ||
        (error == BOARD_A_SENSOR_ERROR_RESET_TIMEOUT) ||
        (error == BOARD_A_SENSOR_ERROR_BUS_STUCK_LOW) ||
        (error == BOARD_A_SENSOR_ERROR_ROM_CRC) ||
        (error == BOARD_A_SENSOR_ERROR_ROM_FAMILY) ||
        (error == BOARD_A_SENSOR_ERROR_SEARCH) ||
        (error == BOARD_A_SENSOR_ERROR_SCRATCHPAD_CRC) ||
        (error == BOARD_A_SENSOR_ERROR_RANGE) ||
        (error == BOARD_A_SENSOR_ERROR_DRIVER);
  }
  return 0;
}

static int ds18b20_rom_is_zero(const uint8_t rom[8])
{
  uint8_t index;

  for (index = 0U; index < 8U; ++index) {
    if (rom[index] != 0U) {
      return 0;
    }
  }
  return 1;
}

static int ds18b20_map_is_valid(const board_a_persisted_config_t *config)
{
  uint8_t index;
  uint8_t other;

  if ((config->sensor_valid_mask & (uint8_t)~0x07U) != 0U) {
    return 0;
  }
  for (index = 0U; index < BOARD_A_RECORD_DS18B20_COUNT; ++index) {
    if ((config->sensor_valid_mask & (uint8_t)(1U << index)) == 0U) {
      if (!ds18b20_rom_is_zero(config->sensor_roms[index])) {
        return 0;
      }
      continue;
    }
    if (!ds18b20_rom_is_valid(
            (const ds18b20_rom_t *)&config->sensor_roms[index])) {
      return 0;
    }
    for (other = (uint8_t)(index + 1U);
         other < BOARD_A_RECORD_DS18B20_COUNT; ++other) {
      if ((config->sensor_valid_mask & (uint8_t)(1U << other)) != 0U &&
          memcmp(config->sensor_roms[index], config->sensor_roms[other], 8U) ==
              0) {
        return 0;
      }
    }
  }
  return 1;
}

int board_a_config_payload_validate(const uint8_t *payload, size_t length)
{
  board_a_persisted_config_t config;

  return board_a_config_payload_decode(payload, length, &config);
}

int board_a_config_payload_decode(const uint8_t *payload, size_t length,
                                  board_a_persisted_config_t *config)
{
  if ((payload == NULL) || (config == NULL) ||
      ((length != BOARD_A_CONFIG_PAYLOAD_SCHEMA1_SIZE) &&
       (length != BOARD_A_CONFIG_PAYLOAD_SCHEMA2_SIZE) &&
       (length != BOARD_A_CONFIG_PAYLOAD_SCHEMA3_SIZE) &&
       (length != BOARD_A_CONFIG_PAYLOAD_SIZE)) ||
      (payload[0] != (uint8_t)'B') || (payload[1] != (uint8_t)'4') ||
      (payload[3] != 0U)) {
    return 0;
  }

  memset(config, 0, sizeof(*config));
  config->period_sec = read_le16(&payload[4]);
  config->channel_mask = read_le16(&payload[6]);
  config->record_count = read_le16(&payload[8]);
  if ((config->period_sec < BOARD_A_PERIOD_MIN_SEC) ||
      (config->period_sec > BOARD_A_PERIOD_MAX_SEC) ||
      (config->channel_mask < BOARD_A_CHANNEL_MASK_MIN) ||
      (config->channel_mask > BOARD_A_CHANNEL_MASK_MAX)) {
    return 0;
  }

  if ((length == BOARD_A_CONFIG_PAYLOAD_SCHEMA1_SIZE) &&
      (payload[2] == 1U) && (payload[10] == 0U) && (payload[11] == 0U)) {
    board_a_alarm_default_config(&config->alarm);
    return 1;
  }

  if (((length == BOARD_A_CONFIG_PAYLOAD_SCHEMA2_SIZE) &&
       (payload[2] != 2U)) ||
      ((length == BOARD_A_CONFIG_PAYLOAD_SCHEMA3_SIZE) &&
       (payload[2] != 3U)) ||
      ((length == BOARD_A_CONFIG_PAYLOAD_SIZE) &&
       (payload[2] != BOARD_A_CONFIG_PAYLOAD_SCHEMA))) {
    return 0;
  }
  if (payload[11] != 0U) {
    return 0;
  }
  if (((length == BOARD_A_CONFIG_PAYLOAD_SCHEMA3_SIZE) ||
       (length == BOARD_A_CONFIG_PAYLOAD_SIZE)) &&
      (payload[61] != 0U)) {
    return 0;
  }
  config->sensor_valid_mask = payload[10];
  memcpy(config->sensor_roms, &payload[12], sizeof(config->sensor_roms));
  if (!ds18b20_map_is_valid(config)) {
    return 0;
  }

  if (length == BOARD_A_CONFIG_PAYLOAD_SCHEMA2_SIZE) {
    board_a_alarm_default_config(&config->alarm);
    return 1;
  }

  config->alarm.phase_notice_x16 = (int16_t)read_le16(&payload[36]);
  config->alarm.phase_warning_x16 = (int16_t)read_le16(&payload[38]);
  config->alarm.phase_critical_x16 = (int16_t)read_le16(&payload[40]);
  config->alarm.delta_notice_x16 = (int16_t)read_le16(&payload[42]);
  config->alarm.delta_warning_x16 = (int16_t)read_le16(&payload[44]);
  config->alarm.delta_critical_x16 = (int16_t)read_le16(&payload[46]);
  config->alarm.rise_notice_x16_per_min =
      (int16_t)read_le16(&payload[48]);
  config->alarm.rise_warning_x16_per_min =
      (int16_t)read_le16(&payload[50]);
  config->alarm.rise_critical_x16_per_min =
      (int16_t)read_le16(&payload[52]);
  config->alarm.assert_samples = read_le16(&payload[54]);
  config->alarm.clear_samples = read_le16(&payload[56]);
  config->alarm.hysteresis_x16 = (int16_t)read_le16(&payload[58]);
  config->alarm.buzzer_enable = payload[60];
  config->alarm.rise_window_samples = read_le16(&payload[62]);
  config->alarm.rise_window_min_ms = read_le32(&payload[64]);
  if (!board_a_alarm_validate_config(&config->alarm)) {
    return 0;
  }
  if (length == BOARD_A_CONFIG_PAYLOAD_SIZE) {
    config->event_open = payload[68] != 0U;
    config->event_id = read_le32(&payload[72]);
    config->event_start_us = read_le64(&payload[76]);
    if ((payload[68] > 1U) || (payload[69] != 0U) ||
        (payload[70] != 0U) || (payload[71] != 0U) ||
        (!config->event_open &&
         ((config->event_id != 0U) ||
          (config->event_start_us != 0U))) ||
        (config->event_open &&
         ((config->event_id == 0U) ||
          (config->event_start_us == 0U)))) {
      return 0;
    }
  }
  return 1;
}

int board_a_config_payload_encode(const board_a_persisted_config_t *config,
                                  uint8_t *payload, size_t capacity)
{
  if ((config == NULL) || (payload == NULL) ||
      (capacity < BOARD_A_CONFIG_PAYLOAD_SIZE) ||
      (config->period_sec < BOARD_A_PERIOD_MIN_SEC) ||
      (config->period_sec > BOARD_A_PERIOD_MAX_SEC) ||
      (config->channel_mask < BOARD_A_CHANNEL_MASK_MIN) ||
      (config->channel_mask > BOARD_A_CHANNEL_MASK_MAX) ||
      !ds18b20_map_is_valid(config) ||
      !board_a_alarm_validate_config(&config->alarm) ||
      ((!config->event_open) &&
       ((config->event_id != 0U) || (config->event_start_us != 0U))) ||
      (config->event_open &&
       ((config->event_id == 0U) || (config->event_start_us == 0U)))) {
    return 0;
  }

  memset(payload, 0, BOARD_A_CONFIG_PAYLOAD_SIZE);
  payload[0] = (uint8_t)'B';
  payload[1] = (uint8_t)'4';
  payload[2] = BOARD_A_CONFIG_PAYLOAD_SCHEMA;
  write_le16(&payload[4], config->period_sec);
  write_le16(&payload[6], config->channel_mask);
  write_le16(&payload[8], config->record_count);
  payload[10] = config->sensor_valid_mask;
  memcpy(&payload[12], config->sensor_roms, sizeof(config->sensor_roms));
  write_le16(&payload[36], (uint16_t)config->alarm.phase_notice_x16);
  write_le16(&payload[38], (uint16_t)config->alarm.phase_warning_x16);
  write_le16(&payload[40], (uint16_t)config->alarm.phase_critical_x16);
  write_le16(&payload[42], (uint16_t)config->alarm.delta_notice_x16);
  write_le16(&payload[44], (uint16_t)config->alarm.delta_warning_x16);
  write_le16(&payload[46], (uint16_t)config->alarm.delta_critical_x16);
  write_le16(&payload[48],
             (uint16_t)config->alarm.rise_notice_x16_per_min);
  write_le16(&payload[50],
             (uint16_t)config->alarm.rise_warning_x16_per_min);
  write_le16(&payload[52],
             (uint16_t)config->alarm.rise_critical_x16_per_min);
  write_le16(&payload[54], config->alarm.assert_samples);
  write_le16(&payload[56], config->alarm.clear_samples);
  write_le16(&payload[58], (uint16_t)config->alarm.hysteresis_x16);
  payload[60] = config->alarm.buzzer_enable;
  write_le16(&payload[62], config->alarm.rise_window_samples);
  write_le32(&payload[64], config->alarm.rise_window_min_ms);
  payload[68] = config->event_open ? 1U : 0U;
  write_le32(&payload[72], config->event_id);
  write_le64(&payload[76], config->event_start_us);
  return 1;
}

static int event_phase_is_valid(uint16_t phase)
{
  return (phase >= BOARD_A_RECORD_EVENT_PHASE_PRE) &&
      (phase <= BOARD_A_RECORD_EVENT_PHASE_CLOSE);
}

static int event_record_is_valid(
    const board_a_record_format_record_t *record)
{
  uint8_t channel;
  uint8_t sensor;

  if ((record->source != BOARD_A_DATA_SOURCE_REAL_DS18B20) ||
      (record->trigger != BOARD_A_SAMPLE_TRIGGER_NONE) ||
      (record->planned_ms != record->actual_ms) ||
      !event_phase_is_valid(record->event_phase) ||
      (record->event_level > BOARD_A_ALARM_UNKNOWN) ||
      (record->event_reason > BOARD_A_ALARM_REASON_CONFIG_INVALID) ||
      (record->event_trigger_phase > BOARD_A_ALARM_PHASE_C) ||
      (record->event_delta_valid > 1U) ||
      ((record->event_delta_valid == 0U) &&
       (record->event_max_delta_x16 != 0)) ||
      ((record->event_flags & (uint16_t)~BOARD_A_EVENT_FLAG_KNOWN_MASK) !=
       0U) ||
      ((record->ds18b20_valid_mask & (uint16_t)~0x0007U) != 0U)) {
    return 0;
  }

  for (channel = 0U; channel < BOARD_A_RECORD_CHANNEL_COUNT; channel++) {
    if ((record->units[channel] != BOARD_A_UNIT_TEMPERATURE_X16) ||
        (record->values[channel] != 0U) ||
        (record->qualities[channel] != BOARD_A_QUALITY_UNAVAILABLE)) {
      return 0;
    }
  }

  for (sensor = 0U; sensor < BOARD_A_RECORD_DS18B20_COUNT; ++sensor) {
    bool has_value;
    uint16_t quality = record->ds18b20_quality[sensor];

    if (quality > BOARD_A_QUALITY_NOT_PRESENT) {
      return 0;
    }
    has_value = (quality == BOARD_A_QUALITY_OK) ||
        (quality == BOARD_A_QUALITY_STALE);
    if (((record->ds18b20_valid_mask &
          (uint16_t)(1U << sensor)) != 0U) !=
        has_value) {
      return 0;
    }
    if (!has_value &&
        (record->ds18b20_temperature_x16[sensor] != 0)) {
      return 0;
    }
  }
  return 1;
}

int board_a_record_format_is_valid(
    const board_a_record_format_record_t *record)
{
  uint8_t channel;
  uint8_t sensor;

  if ((record == NULL) ||
      (record->utc_valid > 1U) ||
      ((record->utc_valid == 0U) && (record->utc_seconds != 0U)) ||
      (record->period_sec < BOARD_A_PERIOD_MIN_SEC) ||
      (record->period_sec > BOARD_A_PERIOD_MAX_SEC) ||
      (record->channel_mask < BOARD_A_CHANNEL_MASK_MIN) ||
      (record->channel_mask > BOARD_A_CHANNEL_MASK_MAX)) {
    return 0;
  }

  if (record->event_phase != BOARD_A_RECORD_EVENT_PHASE_NONE) {
    return event_record_is_valid(record);
  }

  if ((record->event_id != 0U) ||
      (record->event_level != 0U) ||
      (record->event_reason != 0U) ||
      (record->event_trigger_phase != 0U) ||
      (record->event_max_delta_x16 != 0) ||
      (record->event_delta_valid != 0U) ||
      (record->event_flags != 0U) ||
      ((record->trigger != BOARD_A_SAMPLE_TRIGGER_PERIODIC) &&
       (record->trigger != BOARD_A_SAMPLE_TRIGGER_SINGLE)) ||
      ((record->source != BOARD_A_DATA_SOURCE_TEST) &&
       (record->source != BOARD_A_DATA_SOURCE_REAL_DS18B20)) ||
      ((record->ds18b20_valid_mask & (uint16_t)~0x0007U) != 0U)) {
    return 0;
  }

  for (channel = 0U; channel < BOARD_A_RECORD_CHANNEL_COUNT; channel++) {
    if (record->source == BOARD_A_DATA_SOURCE_TEST) {
      if ((record->units[channel] != BOARD_A_UNIT_COUNT) ||
          (record->qualities[channel] >
           BOARD_A_QUALITY_TEST_VALID) ||
          (((record->channel_mask & (uint16_t)(1U << channel)) == 0U) &&
           ((record->values[channel] != 0U) ||
            (record->qualities[channel] !=
             BOARD_A_QUALITY_UNAVAILABLE)))) {
        return 0;
      }
    } else if ((record->units[channel] != BOARD_A_UNIT_TEMPERATURE_X16) ||
               (record->values[channel] != 0U) ||
               (record->qualities[channel] !=
                BOARD_A_QUALITY_UNAVAILABLE)) {
      return 0;
    }
  }

  for (sensor = 0U; sensor < BOARD_A_RECORD_DS18B20_COUNT; ++sensor) {
    bool has_value;
    uint16_t quality = record->ds18b20_quality[sensor];

    if (record->source == BOARD_A_DATA_SOURCE_TEST) {
      if ((record->ds18b20_valid_mask != 0U) ||
          (record->ds18b20_sample_id != 0U) ||
          (record->ds18b20_temperature_x16[sensor] != 0) ||
          (record->ds18b20_quality[sensor] != 0U) ||
          (record->ds18b20_error[sensor] != 0U) ||
          (record->ds18b20_rom_short[sensor] != 0U) ||
          (record->ds18b20_sample_time_ms[sensor] != 0U)) {
        return 0;
      }
      continue;
    }
    if (record->ds18b20_sample_id == 0U) {
      return 0;
    }

    if ((quality != BOARD_A_QUALITY_OK) &&
        (quality != BOARD_A_QUALITY_TIMEOUT) &&
        (quality != BOARD_A_QUALITY_CRC_ERROR) &&
        (quality != BOARD_A_QUALITY_RANGE_ERROR) &&
        (quality != BOARD_A_QUALITY_STALE) &&
        (quality != BOARD_A_QUALITY_NOT_PRESENT)) {
      return 0;
    }
    has_value = (quality == BOARD_A_QUALITY_OK) ||
        (quality == BOARD_A_QUALITY_STALE);
    if (((record->ds18b20_valid_mask &
          (uint16_t)(1U << sensor)) != 0U) !=
        has_value) {
      return 0;
    }
    if (!has_value &&
        (record->ds18b20_temperature_x16[sensor] != 0)) {
      return 0;
    }
    if ((quality == BOARD_A_QUALITY_OK) &&
        (record->ds18b20_error[sensor] != BOARD_A_SENSOR_ERROR_NONE)) {
      return 0;
    }
    if ((quality == BOARD_A_QUALITY_STALE) &&
        (record->ds18b20_error[sensor] == BOARD_A_SENSOR_ERROR_NONE)) {
      return 0;
    }
    if (!ds18b20_error_matches_quality(
            quality, record->ds18b20_error[sensor])) {
      return 0;
    }
  }

  return 1;
}

int board_a_record_format_from_event(
    const board_a_event_buffer_record_t *event_record,
    uint32_t session_id, uint32_t config_version, uint16_t period_sec,
    uint16_t channel_mask, uint16_t sample_count, uint8_t utc_valid,
    uint32_t utc_seconds, board_a_record_format_record_t *record)
{
  uint8_t phase;

  if ((event_record == NULL) || (record == NULL) ||
      (event_record->phase > BOARD_A_EVENT_PHASE_CLOSE) ||
      (utc_valid > 1U) ||
      ((utc_valid == 0U) && (utc_seconds != 0U))) {
    return 0;
  }

  memset(record, 0, sizeof(*record));
  record->session_id = session_id;
  record->sequence = event_record->sample_id;
  record->trigger = BOARD_A_SAMPLE_TRIGGER_NONE;
  record->planned_ms = event_record->time_ms;
  record->actual_ms = event_record->time_ms;
  record->utc_valid = utc_valid;
  record->utc_seconds = utc_seconds;
  record->config_version = config_version;
  record->period_sec = period_sec;
  record->channel_mask = channel_mask;
  record->sample_count = sample_count;
  record->source = BOARD_A_DATA_SOURCE_REAL_DS18B20;
  record->ds18b20_valid_mask = event_record->valid_mask;
  record->ds18b20_sample_id = event_record->sample_id;
  for (phase = 0U; phase < BOARD_A_RECORD_DS18B20_COUNT; ++phase) {
    record->ds18b20_temperature_x16[phase] =
        event_record->temperature_x16[phase];
    record->ds18b20_quality[phase] = event_record->quality[phase];
  }
  for (phase = 0U; phase < BOARD_A_RECORD_CHANNEL_COUNT; ++phase) {
    record->units[phase] = BOARD_A_UNIT_TEMPERATURE_X16;
    record->qualities[phase] = BOARD_A_QUALITY_UNAVAILABLE;
  }
  record->event_id = event_record->event_id;
  record->event_phase = (uint16_t)event_record->phase + 1U;
  record->event_level = (uint16_t)event_record->level;
  record->event_reason = (uint16_t)event_record->reason;
  record->event_trigger_phase = (uint16_t)event_record->alarm_phase;
  record->event_max_delta_x16 = event_record->delta_x16;
  record->event_delta_valid = event_record->delta_valid ? 1U : 0U;
  record->event_flags = event_record->flags;
  return board_a_record_format_is_valid(record);
}

int board_a_record_format_encode_csv(
    const board_a_record_format_record_t *record, uint8_t *buffer,
    size_t capacity, size_t *encoded_length)
{
  csv_writer_t writer;
  uint8_t channel;

  if ((record == NULL) || (buffer == NULL) || (encoded_length == NULL) ||
      !board_a_record_format_is_valid(record) ||
      (capacity == 0U)) {
    return 0;
  }

  writer.buffer = buffer;
  writer.capacity = capacity;
  writer.length = 0U;
  writer.overflow = 0;

  csv_put_u16(&writer, BOARD_A_RECORD_SCHEMA);
  csv_put_comma(&writer);
  csv_put_u32(&writer, record->session_id);
  csv_put_comma(&writer);
  csv_put_u32(&writer, record->sequence);
  csv_put_comma(&writer);
  csv_put_u16(&writer, record->trigger);
  csv_put_comma(&writer);
  csv_put_u64(&writer, record->planned_ms);
  csv_put_comma(&writer);
  csv_put_u64(&writer, record->actual_ms);
  csv_put_comma(&writer);
  csv_put_u16(&writer, record->utc_valid);
  csv_put_comma(&writer);
  csv_put_u32(&writer, record->utc_seconds);
  csv_put_comma(&writer);
  csv_put_u32(&writer, record->config_version);
  csv_put_comma(&writer);
  csv_put_u16(&writer, record->period_sec);
  csv_put_comma(&writer);
  csv_put_u16(&writer, record->channel_mask);
  csv_put_comma(&writer);
  csv_put_u16(&writer, record->sample_count);
  csv_put_comma(&writer);
  csv_put_u16(&writer, record->source);

  for (channel = 0U; channel < BOARD_A_RECORD_CHANNEL_COUNT; channel++) {
    csv_put_comma(&writer);
    csv_put_u16(&writer, record->values[channel]);
  }
  for (channel = 0U; channel < BOARD_A_RECORD_CHANNEL_COUNT; channel++) {
    csv_put_comma(&writer);
    csv_put_u16(&writer, record->units[channel]);
  }
  for (channel = 0U; channel < BOARD_A_RECORD_CHANNEL_COUNT; channel++) {
    csv_put_comma(&writer);
    csv_put_u16(&writer, record->qualities[channel]);
  }

  csv_put_comma(&writer);
  csv_put_u32(&writer, record->file_id);
  csv_put_comma(&writer);
  csv_put_u32(&writer, record->file_date);
  csv_put_comma(&writer);
  csv_put_u16(&writer, 0U);
  csv_put_comma(&writer);
  csv_put_u16(&writer, record->ds18b20_valid_mask);
  csv_put_comma(&writer);
  csv_put_u32(&writer, record->ds18b20_sample_id);
  for (channel = 0U; channel < BOARD_A_RECORD_DS18B20_COUNT; ++channel) {
    csv_put_comma(&writer);
    csv_put_i16(&writer, record->ds18b20_temperature_x16[channel]);
  }
  for (channel = 0U; channel < BOARD_A_RECORD_DS18B20_COUNT; ++channel) {
    csv_put_comma(&writer);
    csv_put_u16(&writer, record->ds18b20_quality[channel]);
  }
  for (channel = 0U; channel < BOARD_A_RECORD_DS18B20_COUNT; ++channel) {
    csv_put_comma(&writer);
    csv_put_u16(&writer, record->ds18b20_error[channel]);
  }
  for (channel = 0U; channel < BOARD_A_RECORD_DS18B20_COUNT; ++channel) {
    csv_put_comma(&writer);
    csv_put_u16(&writer, record->ds18b20_rom_short[channel]);
  }
  for (channel = 0U; channel < BOARD_A_RECORD_DS18B20_COUNT; ++channel) {
    csv_put_comma(&writer);
    csv_put_u32(&writer, record->ds18b20_sample_time_ms[channel]);
  }
  csv_put_comma(&writer);
  csv_put_u32(&writer, record->event_id);
  csv_put_comma(&writer);
  csv_put_u16(&writer, record->event_phase);
  csv_put_comma(&writer);
  csv_put_u16(&writer, record->event_level);
  csv_put_comma(&writer);
  csv_put_u16(&writer, record->event_reason);
  csv_put_comma(&writer);
  csv_put_u16(&writer, record->event_trigger_phase);
  csv_put_comma(&writer);
  csv_put_i16(&writer, record->event_max_delta_x16);
  csv_put_comma(&writer);
  csv_put_u16(&writer, record->event_delta_valid);
  csv_put_comma(&writer);
  csv_put_u16(&writer, record->event_flags);
  csv_put_byte(&writer, (uint8_t)'\n');

  if (writer.overflow != 0) {
    return 0;
  }
  *encoded_length = writer.length;
  return 1;
}

int board_a_record_format_date_from_utc(uint32_t utc_seconds,
                                        uint32_t *file_date)
{
  uint32_t days;
  uint32_t z;
  uint32_t era;
  uint32_t doe;
  uint32_t yoe;
  uint32_t year;
  uint32_t doy;
  uint32_t mp;
  uint32_t month;
  uint32_t day;

  if (file_date == NULL) {
    return 0;
  }

  days = utc_seconds / 86400U;
  z = days + 719468U;
  era = z / 146097U;
  doe = z - (era * 146097U);
  yoe = (doe - (doe / 1460U) + (doe / 36524U) - (doe / 146096U)) /
        365U;
  year = yoe + (era * 400U);
  doy = doe - ((365U * yoe) + (yoe / 4U) - (yoe / 100U));
  mp = ((5U * doy) + 2U) / 153U;
  day = doy - (((153U * mp) + 2U) / 5U) + 1U;
  month = (mp < 10U) ? (mp + 3U) : (mp - 9U);
  if (month <= 2U) {
    year++;
  }

  if ((year > 9999U) || (month == 0U) || (month > 12U) ||
      (day == 0U) || (day > 31U)) {
    return 0;
  }

  *file_date = (year * 10000U) + (month * 100U) + day;
  return 1;
}

static void put_hex8(char *output, uint32_t value)
{
  static const char digits[] = "0123456789ABCDEF";
  uint8_t index;

  for (index = 0U; index < 8U; index++) {
    uint8_t shift = (uint8_t)((7U - index) * 4U);
    output[index] = digits[(value >> shift) & 0x0FU];
  }
}

static void put_decimal8(char *output, uint32_t value)
{
  uint8_t index;

  for (index = 0U; index < 8U; index++) {
    uint32_t divisor = 1U;
    uint8_t remaining = (uint8_t)(7U - index);

    while (remaining != 0U) {
      divisor *= 10U;
      remaining--;
    }
    output[index] = (char)('0' + ((value / divisor) % 10U));
  }
}

int board_a_record_format_make_path(char *path, size_t capacity,
                                    uint32_t file_date, uint32_t file_id)
{
  const char *prefix;
  size_t prefix_length;
  size_t required_length;
  char date[9];

  if ((path == NULL) || (file_id == 0U)) {
    return 0;
  }
  if (file_date == 0U) {
    prefix = "0:/LOG/UNSET/";
  } else {
    prefix = "0:/LOG/";
  }
  prefix_length = strlen(prefix);
  required_length = prefix_length + 8U + 4U + 1U;
  if (file_date != 0U) {
    required_length += 9U; /* 8-digit date plus '/' */
  }
  if (capacity < required_length) {
    return 0;
  }

  memcpy(path, prefix, prefix_length);
  if (file_date != 0U) {
    put_decimal8(date, file_date);
    date[8] = '\0';
    memcpy(&path[prefix_length], date, 8U);
    path[prefix_length + 8U] = '/';
    put_hex8(&path[prefix_length + 9U], file_id);
    memcpy(&path[prefix_length + 17U], ".CSV", 4U);
    path[prefix_length + 21U] = '\0';
  } else {
    put_hex8(&path[prefix_length], file_id);
    memcpy(&path[prefix_length + 8U], ".CSV", 4U);
    path[prefix_length + 12U] = '\0';
  }
  return 1;
}
