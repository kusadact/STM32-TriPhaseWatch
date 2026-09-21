#ifndef BOARD_A_RECORD_FORMAT_H
#define BOARD_A_RECORD_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define BOARD_A_CONFIG_PAYLOAD_SCHEMA 1U
#define BOARD_A_CONFIG_PAYLOAD_SIZE 12U
#define BOARD_A_RECORD_SCHEMA 2U
#define BOARD_A_RECORD_CHANNEL_COUNT 4U
#define BOARD_A_RECORD_DHT11_COUNT 3U
#define BOARD_A_RECORD_CSV_MAX_BYTES 512U
#define BOARD_A_RECORD_PATH_MAX_BYTES 32U

typedef struct {
  uint16_t period_sec;
  uint16_t channel_mask;
  uint16_t record_count;
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
  uint16_t dht_valid_mask;
  uint32_t dht_sample_id;
  uint16_t dht_temperature_x10[BOARD_A_RECORD_DHT11_COUNT];
  uint16_t dht_humidity_x10[BOARD_A_RECORD_DHT11_COUNT];
  uint16_t dht_quality[BOARD_A_RECORD_DHT11_COUNT];
  uint16_t dht_error[BOARD_A_RECORD_DHT11_COUNT];
  uint32_t dht_sample_time_ms[BOARD_A_RECORD_DHT11_COUNT];
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
