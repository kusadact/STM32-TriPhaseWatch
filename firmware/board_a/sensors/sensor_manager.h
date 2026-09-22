#ifndef BOARD_A_SENSOR_MANAGER_H
#define BOARD_A_SENSOR_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "ds18b20.h"

#define BOARD_A_SENSOR_COUNT 3U
#define BOARD_A_SENSOR_SCAN_PERIOD_US 1000000ULL
#define BOARD_A_SENSOR_RETRY_PERIOD_US 2000000ULL
#define BOARD_A_SENSOR_DISCOVERY_RETRY_US 5000000ULL
#define BOARD_A_SENSOR_NOT_PRESENT_FAILURES 3U
#define BOARD_A_SENSOR_ALL_BOUND_MASK 0x07U
/* N devices need N search passes plus one terminating pass; +1 is margin. */
#define BOARD_A_SENSOR_SEARCH_PASS_LIMIT (BOARD_A_SENSOR_COUNT + 2U)

enum {
  BOARD_A_SENSOR_TYPE_DHT11 = 1,
  BOARD_A_SENSOR_TYPE_DS18B20 = 2
};

enum {
  BOARD_A_QUALITY_UNAVAILABLE = 0,
  BOARD_A_QUALITY_OK = 1,
  BOARD_A_QUALITY_TEST_VALID = 1,
  BOARD_A_QUALITY_TIMEOUT = 2,
  BOARD_A_QUALITY_CRC_ERROR = 3,
  BOARD_A_QUALITY_CHECKSUM_ERROR = BOARD_A_QUALITY_CRC_ERROR,
  BOARD_A_QUALITY_RANGE_ERROR = 4,
  BOARD_A_QUALITY_STALE = 5,
  BOARD_A_QUALITY_NOT_PRESENT = 6
};

enum {
  BOARD_A_SENSOR_ERROR_NONE = 0,
  BOARD_A_SENSOR_ERROR_RESET_TIMEOUT = 1,
  BOARD_A_SENSOR_ERROR_BUS_STUCK_LOW = 2,
  BOARD_A_SENSOR_ERROR_ROM_CRC = 3,
  BOARD_A_SENSOR_ERROR_ROM_FAMILY = 4,
  BOARD_A_SENSOR_ERROR_SEARCH = 5,
  BOARD_A_SENSOR_ERROR_SCRATCHPAD_CRC = 6,
  BOARD_A_SENSOR_ERROR_RANGE = 7,
  BOARD_A_SENSOR_ERROR_DRIVER = 255
};

typedef struct {
  uint8_t rom[DS18B20_ROM_SIZE];
  uint16_t rom_short;
  bool bound;
} board_a_sensor_binding_t;

typedef struct {
  uint8_t valid_mask;
  board_a_sensor_binding_t bindings[BOARD_A_SENSOR_COUNT];
} board_a_sensor_map_t;

typedef struct {
  uint8_t sensor_id;
  uint8_t sensor_type;
  bool has_value;
  int16_t temperature_x16;
  uint16_t quality;
  uint16_t error;
  uint16_t rom_short;
  uint32_t sample_time_ms;
} board_a_sensor_sample_t;

typedef struct {
  uint32_t sample_id;
  uint64_t sample_time_us;
  uint16_t valid_mask;
  board_a_sensor_sample_t sensors[BOARD_A_SENSOR_COUNT];
} board_a_sensor_snapshot_t;

typedef struct {
  ds18b20_t bus;
  board_a_sensor_map_t map;
  board_a_sensor_snapshot_t snapshot;
  uint8_t consecutive_failures[BOARD_A_SENSOR_COUNT];
  uint64_t next_step_us;
  uint64_t next_discovery_us;
  uint64_t conversion_started_us;
  ds18b20_status_t conversion_status;
  bool discovery_complete;
  bool conversion_pending;
  bool map_dirty;
} board_a_sensor_manager_t;

void board_a_sensor_manager_init(
    board_a_sensor_manager_t *manager, const ds18b20_port_t *port);

bool board_a_sensor_manager_set_map(
    board_a_sensor_manager_t *manager,
    const board_a_sensor_map_t *map);

bool board_a_sensor_manager_copy_map(
    const board_a_sensor_manager_t *manager,
    board_a_sensor_map_t *map);

bool board_a_sensor_manager_take_map_dirty(
    board_a_sensor_manager_t *manager);

void board_a_sensor_manager_mark_map_dirty(
    board_a_sensor_manager_t *manager);

uint64_t board_a_sensor_manager_next_step_us(
    const board_a_sensor_manager_t *manager);

bool board_a_sensor_manager_step(
    board_a_sensor_manager_t *manager, uint64_t now_us,
    board_a_sensor_snapshot_t *snapshot);

#endif /* BOARD_A_SENSOR_MANAGER_H */
