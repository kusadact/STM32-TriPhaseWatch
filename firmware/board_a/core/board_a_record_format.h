#ifndef BOARD_A_RECORD_FORMAT_H
#define BOARD_A_RECORD_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../alarm/board_a_alarm.h"
#include "board_a_event_buffer.h"

#define BOARD_A_CONFIG_PAYLOAD_SCHEMA 4U
#define BOARD_A_CONFIG_PAYLOAD_SCHEMA1_SIZE 12U
#define BOARD_A_CONFIG_PAYLOAD_SCHEMA2_SIZE 36U
#define BOARD_A_CONFIG_PAYLOAD_SCHEMA3_SIZE 68U
#define BOARD_A_CONFIG_PAYLOAD_SIZE 84U
#define BOARD_A_RECORD_SCHEMA 4U
#define BOARD_A_RECORD_CHANNEL_COUNT 4U
#define BOARD_A_RECORD_DS18B20_COUNT 3U
#define BOARD_A_RECORD_CSV_MAX_BYTES 1024U
#define BOARD_A_RECORD_PATH_MAX_BYTES 32U

enum {
  BOARD_A_RECORD_EVENT_PHASE_NONE = 0,
  BOARD_A_RECORD_EVENT_PHASE_PRE = 1,
  BOARD_A_RECORD_EVENT_PHASE_TRIGGER = 2,
  BOARD_A_RECORD_EVENT_PHASE_ACTIVE = 3,
  BOARD_A_RECORD_EVENT_PHASE_POST = 4,
  BOARD_A_RECORD_EVENT_PHASE_CLOSE = 5
};

typedef struct {
  uint16_t period_sec;
  uint16_t channel_mask;
  uint16_t record_count;
  uint8_t sensor_valid_mask;
  uint8_t sensor_roms[BOARD_A_RECORD_DS18B20_COUNT][8];
  board_a_alarm_config_t alarm;
  bool event_open;
  uint32_t event_id;
  uint64_t event_start_us;
} board_a_persisted_config_t;

typedef struct {
  uint32_t session_id;
  uint32_t sequence;
  uint16_t trigger;
  uint64_t planned_ms;
  uint64_t actual_ms;
  uint8_t utc_valid;
  uint32_t utc_seconds;
  uint32_t config_version;
  uint16_t period_sec;
  uint16_t channel_mask;
  uint16_t sample_count;
  uint16_t source;
  uint16_t values[BOARD_A_RECORD_CHANNEL_COUNT];
  uint16_t units[BOARD_A_RECORD_CHANNEL_COUNT];
  uint16_t qualities[BOARD_A_RECORD_CHANNEL_COUNT];
  uint16_t ds18b20_valid_mask;
  uint32_t ds18b20_sample_id;
  int16_t ds18b20_temperature_x16[BOARD_A_RECORD_DS18B20_COUNT];
  uint16_t ds18b20_quality[BOARD_A_RECORD_DS18B20_COUNT];
  uint16_t ds18b20_error[BOARD_A_RECORD_DS18B20_COUNT];
  uint16_t ds18b20_rom_short[BOARD_A_RECORD_DS18B20_COUNT];
  uint32_t ds18b20_sample_time_ms[BOARD_A_RECORD_DS18B20_COUNT];
  uint32_t event_id;
  uint16_t event_phase;
  uint16_t event_level;
  uint16_t event_reason;
  uint16_t event_trigger_phase;
  int16_t event_max_delta_x16;
  uint16_t event_delta_valid;
  uint16_t event_flags;
  uint32_t file_id;
  uint32_t file_date;
} board_a_record_format_record_t;

int board_a_config_payload_encode(const board_a_persisted_config_t *config,
                                  uint8_t *payload, size_t capacity);
int board_a_config_payload_decode(const uint8_t *payload, size_t length,
                                  board_a_persisted_config_t *config);
int board_a_config_payload_validate(const uint8_t *payload, size_t length);

int board_a_record_format_is_valid(
    const board_a_record_format_record_t *record);

/*
 * Converts one event-buffer row into the schema-4 CSV representation.
 * Event rows use trigger NONE and planned_ms == actual_ms == event time.
 * Their sequence identity is the DS18B20 sample_id; phases are encoded as
 * 1..5. Sensor error/ROM/sample-time fields stay zero because the event
 * buffer does not provide them.
 */
int board_a_record_format_from_event(
    const board_a_event_buffer_record_t *event_record,
    uint32_t session_id, uint32_t config_version, uint16_t period_sec,
    uint16_t channel_mask, uint16_t sample_count, uint8_t utc_valid,
    uint32_t utc_seconds, board_a_record_format_record_t *record);

/*
 * Encodes exactly one LF-terminated CSV line without a trailing NUL. The
 * caller-owned buffer remains unchanged when encoding does not fit.
 */
int board_a_record_format_encode_csv(
    const board_a_record_format_record_t *record, uint8_t *buffer,
    size_t capacity, size_t *encoded_length);

const char *board_a_record_format_csv_header(void);

int board_a_record_format_date_from_utc(uint32_t utc_seconds,
                                        uint32_t *file_date);

int board_a_record_format_make_path(char *path, size_t capacity,
                                    uint32_t file_date, uint32_t file_id);

#endif /* BOARD_A_RECORD_FORMAT_H */
