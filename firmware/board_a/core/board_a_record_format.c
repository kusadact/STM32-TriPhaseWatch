#include "board_a_record_format.h"

#include <string.h>

#include "board_a_model.h"

static const char BOARD_A_RECORD_CSV_HEADER[] =
    "schema,session,seq,trigger,planned_ms,actual_ms,utc_valid,utc_s,"
    "config_version,period_s,mask,sample_count,source,v0,v1,v2,v3,u0,u1,u2,"
    "u3,q0,q1,q2,q3,file_id,file_date,reserved\n";

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

int board_a_config_payload_validate(const uint8_t *payload, size_t length)
{
  board_a_persisted_config_t config;

  return board_a_config_payload_decode(payload, length, &config);
}

int board_a_config_payload_decode(const uint8_t *payload, size_t length,
                                  board_a_persisted_config_t *config)
{
  if ((payload == NULL) || (config == NULL) ||
      (length != BOARD_A_CONFIG_PAYLOAD_SIZE) ||
      (payload[0] != (uint8_t)'B') || (payload[1] != (uint8_t)'4') ||
      (payload[2] != BOARD_A_CONFIG_PAYLOAD_SCHEMA) || (payload[3] != 0U) ||
      (payload[10] != 0U) || (payload[11] != 0U)) {
    return 0;
  }

  config->period_sec = read_le16(&payload[4]);
  config->channel_mask = read_le16(&payload[6]);
  config->record_count = read_le16(&payload[8]);
  if ((config->period_sec < BOARD_A_PERIOD_MIN_SEC) ||
      (config->period_sec > BOARD_A_PERIOD_MAX_SEC) ||
      (config->channel_mask < BOARD_A_CHANNEL_MASK_MIN) ||
      (config->channel_mask > BOARD_A_CHANNEL_MASK_MAX)) {
    return 0;
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
      (config->channel_mask > BOARD_A_CHANNEL_MASK_MAX)) {
    return 0;
  }

  memset(payload, 0, BOARD_A_CONFIG_PAYLOAD_SIZE);
  payload[0] = (uint8_t)'B';
  payload[1] = (uint8_t)'4';
  payload[2] = BOARD_A_CONFIG_PAYLOAD_SCHEMA;
  write_le16(&payload[4], config->period_sec);
  write_le16(&payload[6], config->channel_mask);
  write_le16(&payload[8], config->record_count);
  return 1;
}

int board_a_record_format_is_valid(
    const board_a_record_format_record_t *record)
{
  uint8_t channel;

  if ((record == NULL) ||
      ((record->trigger != BOARD_A_SAMPLE_TRIGGER_PERIODIC) &&
       (record->trigger != BOARD_A_SAMPLE_TRIGGER_SINGLE)) ||
      (record->utc_valid > 1U) ||
      ((record->utc_valid == 0U) && (record->utc_seconds != 0U)) ||
      (record->period_sec < BOARD_A_PERIOD_MIN_SEC) ||
      (record->period_sec > BOARD_A_PERIOD_MAX_SEC) ||
      (record->channel_mask < BOARD_A_CHANNEL_MASK_MIN) ||
      (record->channel_mask > BOARD_A_CHANNEL_MASK_MAX) ||
      (record->source != BOARD_A_DATA_SOURCE_TEST)) {
    return 0;
  }

  for (channel = 0U; channel < BOARD_A_RECORD_CHANNEL_COUNT; channel++) {
    if ((record->units[channel] != BOARD_A_UNIT_COUNT) ||
        (record->qualities[channel] >
         BOARD_A_QUALITY_TEST_VALID) ||
        (((record->channel_mask & (uint16_t)(1U << channel)) == 0U) &&
         ((record->values[channel] != 0U) ||
          (record->qualities[channel] !=
           BOARD_A_QUALITY_UNAVAILABLE)))) {
      return 0;
    }
  }

  return 1;
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
