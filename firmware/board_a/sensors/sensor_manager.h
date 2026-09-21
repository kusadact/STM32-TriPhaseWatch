#ifndef BOARD_A_SENSOR_MANAGER_H
#define BOARD_A_SENSOR_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "dht11.h"

#define BOARD_A_SENSOR_COUNT 3U
#define BOARD_A_SENSOR_SCAN_PERIOD_US 2000000ULL
#define BOARD_A_SENSOR_NOT_PRESENT_FAILURES 3U

enum {
  BOARD_A_SENSOR_TYPE_DHT11 = 1
};

/*
 * Quality codes 0 and 1 keep their existing board-A meanings. The additional
 * values are used by the DHT11 block in the REAL_DHT11 source.
 */
enum {
  BOARD_A_QUALITY_UNAVAILABLE = 0,
  BOARD_A_QUALITY_OK = 1,
  BOARD_A_QUALITY_TEST_VALID = 1,
  BOARD_A_QUALITY_TIMEOUT = 2,
  BOARD_A_QUALITY_CHECKSUM_ERROR = 3,
  BOARD_A_QUALITY_RANGE_ERROR = 4,
  BOARD_A_QUALITY_STALE = 5,
  BOARD_A_QUALITY_NOT_PRESENT = 6
};

enum {
  BOARD_A_SENSOR_ERROR_NONE = 0,
  BOARD_A_SENSOR_ERROR_TIMEOUT_RESPONSE = 1,
  BOARD_A_SENSOR_ERROR_TIMEOUT_BIT = 2,
  BOARD_A_SENSOR_ERROR_CHECKSUM = 3,
  BOARD_A_SENSOR_ERROR_RANGE = 4,
  BOARD_A_SENSOR_ERROR_TOO_SOON = 5,
  BOARD_A_SENSOR_ERROR_DRIVER = 255
};

typedef struct {
  uint8_t sensor_id;
  uint8_t sensor_type;
  bool has_value;
  uint16_t temperature_x10;
  uint16_t humidity_x10;
  uint16_t quality;
  uint16_t error;
  uint32_t sample_time_ms;
} board_a_sensor_sample_t;

typedef struct {
  uint32_t sample_id;
  uint64_t sample_time_us;
  uint16_t valid_mask;
  board_a_sensor_sample_t sensors[BOARD_A_SENSOR_COUNT];
} board_a_sensor_snapshot_t;

typedef struct {
  dht11_t drivers[BOARD_A_SENSOR_COUNT];
  board_a_sensor_snapshot_t snapshot;
  uint8_t consecutive_failures[BOARD_A_SENSOR_COUNT];
  uint64_t next_scan_us;
} board_a_sensor_manager_t;

void board_a_sensor_manager_init(
    board_a_sensor_manager_t *manager, const dht11_port_t *port);

/*
 * Performs one complete serial 0 -> 1 -> 2 scan when due. It returns false
 * without touching the snapshot when the 2 s scan interval has not elapsed.
 */
bool board_a_sensor_manager_scan(
    board_a_sensor_manager_t *manager, uint64_t now_us,
    board_a_sensor_snapshot_t *snapshot);

uint64_t board_a_sensor_manager_next_scan_us(
    const board_a_sensor_manager_t *manager);

#endif /* BOARD_A_SENSOR_MANAGER_H */
