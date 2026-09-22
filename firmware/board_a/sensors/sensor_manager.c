#include "sensor_manager.h"

#include <stddef.h>
#include <string.h>

static void clear_sample(
    board_a_sensor_sample_t *sample, uint32_t sample_time_ms)
{
  sample->has_value = false;
  sample->temperature_x16 = 0;
  sample->quality = BOARD_A_QUALITY_NOT_PRESENT;
  sample->error = BOARD_A_SENSOR_ERROR_NONE;
  sample->sample_time_ms = sample_time_ms;
}

static uint16_t quality_for_status(ds18b20_status_t status)
{
  switch (status) {
    case DS18B20_STATUS_RESET_TIMEOUT:
    case DS18B20_STATUS_BUS_STUCK_LOW:
      return BOARD_A_QUALITY_TIMEOUT;
    case DS18B20_STATUS_ROM_CRC_ERROR:
    case DS18B20_STATUS_SCRATCHPAD_CRC_ERROR:
      return BOARD_A_QUALITY_CRC_ERROR;
    case DS18B20_STATUS_RANGE_ERROR:
      return BOARD_A_QUALITY_RANGE_ERROR;
    case DS18B20_STATUS_ROM_FAMILY_ERROR:
    case DS18B20_STATUS_SEARCH_ERROR:
    case DS18B20_STATUS_NO_MORE_DEVICES:
    case DS18B20_STATUS_NO_RESPONSE:
    case DS18B20_STATUS_INVALID_ARGUMENT:
    case DS18B20_STATUS_OK:
    default:
      return BOARD_A_QUALITY_NOT_PRESENT;
  }
}

static uint16_t error_for_status(ds18b20_status_t status)
{
  switch (status) {
    case DS18B20_STATUS_RESET_TIMEOUT:
      return BOARD_A_SENSOR_ERROR_RESET_TIMEOUT;
    case DS18B20_STATUS_BUS_STUCK_LOW:
      return BOARD_A_SENSOR_ERROR_BUS_STUCK_LOW;
    case DS18B20_STATUS_ROM_CRC_ERROR:
      return BOARD_A_SENSOR_ERROR_ROM_CRC;
    case DS18B20_STATUS_ROM_FAMILY_ERROR:
      return BOARD_A_SENSOR_ERROR_ROM_FAMILY;
    case DS18B20_STATUS_SEARCH_ERROR:
    case DS18B20_STATUS_NO_MORE_DEVICES:
      return BOARD_A_SENSOR_ERROR_SEARCH;
    case DS18B20_STATUS_NO_RESPONSE:
      return BOARD_A_SENSOR_ERROR_RESET_TIMEOUT;
    case DS18B20_STATUS_SCRATCHPAD_CRC_ERROR:
      return BOARD_A_SENSOR_ERROR_SCRATCHPAD_CRC;
    case DS18B20_STATUS_RANGE_ERROR:
      return BOARD_A_SENSOR_ERROR_RANGE;
    case DS18B20_STATUS_INVALID_ARGUMENT:
    case DS18B20_STATUS_OK:
    default:
      return BOARD_A_SENSOR_ERROR_DRIVER;
  }
}

static bool rom_is_zero(const uint8_t rom[DS18B20_ROM_SIZE])
{
  uint8_t index;

  for (index = 0U; index < DS18B20_ROM_SIZE; ++index) {
    if (rom[index] != 0U) {
      return false;
    }
  }
  return true;
}

static uint16_t rom_short_id(const uint8_t rom[DS18B20_ROM_SIZE])
{
  return (uint16_t)((uint16_t)rom[2] << 8U) | (uint16_t)rom[1];
}

static bool map_is_valid(const board_a_sensor_map_t *map)
{
  uint8_t index;
  uint8_t other;

  if ((map == NULL) || ((map->valid_mask & (uint8_t)~0x07U) != 0U)) {
    return false;
  }
  for (index = 0U; index < BOARD_A_SENSOR_COUNT; ++index) {
    bool bound = (map->valid_mask & (uint8_t)(1U << index)) != 0U;

    if (!bound) {
      if (!rom_is_zero(map->bindings[index].rom)) {
        return false;
      }
      continue;
    }
    if (!ds18b20_rom_is_valid(
            (const ds18b20_rom_t *)&map->bindings[index].rom)) {
      return false;
    }
    for (other = (uint8_t)(index + 1U);
         other < BOARD_A_SENSOR_COUNT; ++other) {
      if ((map->valid_mask & (uint8_t)(1U << other)) != 0U &&
          memcmp(map->bindings[index].rom, map->bindings[other].rom,
                 DS18B20_ROM_SIZE) == 0) {
        return false;
      }
    }
  }
  return true;
}

static void reset_snapshot(board_a_sensor_snapshot_t *snapshot)
{
  uint8_t index;

  memset(snapshot, 0, sizeof(*snapshot));
  for (index = 0U; index < BOARD_A_SENSOR_COUNT; ++index) {
    snapshot->sensors[index].sensor_id = index;
    snapshot->sensors[index].sensor_type = BOARD_A_SENSOR_TYPE_DS18B20;
    clear_sample(&snapshot->sensors[index], 0U);
  }
}

void board_a_sensor_manager_init(
    board_a_sensor_manager_t *manager, const ds18b20_port_t *port)
{
  if (manager == NULL) {
    return;
  }
  memset(manager, 0, sizeof(*manager));
  ds18b20_init(&manager->bus, port);
  reset_snapshot(&manager->snapshot);
  manager->next_step_us = 0U;
}

bool board_a_sensor_manager_set_map(
    board_a_sensor_manager_t *manager,
    const board_a_sensor_map_t *map)
{
  if ((manager == NULL) || !map_is_valid(map)) {
    return false;
  }
  manager->map = *map;
  manager->map_dirty = false;
  manager->discovery_complete =
      (map->valid_mask == BOARD_A_SENSOR_ALL_BOUND_MASK);
  manager->next_discovery_us = 0U;
  return true;
}

bool board_a_sensor_manager_copy_map(
    const board_a_sensor_manager_t *manager,
    board_a_sensor_map_t *map)
{
  if ((manager == NULL) || (map == NULL)) {
    return false;
  }
  *map = manager->map;
  return true;
}

bool board_a_sensor_manager_take_map_dirty(
    board_a_sensor_manager_t *manager)
{
  bool dirty;

  if (manager == NULL) {
    return false;
  }
  dirty = manager->map_dirty;
  manager->map_dirty = false;
  return dirty;
}

void board_a_sensor_manager_mark_map_dirty(
    board_a_sensor_manager_t *manager)
{
  if (manager != NULL) {
    manager->map_dirty = true;
  }
}

uint64_t board_a_sensor_manager_next_step_us(
    const board_a_sensor_manager_t *manager)
{
  return (manager == NULL) ? 0U : manager->next_step_us;
}

static int free_slot_for(const board_a_sensor_map_t *map)
{
  uint8_t index;

  for (index = 0U; index < BOARD_A_SENSOR_COUNT; ++index) {
    if ((map->valid_mask & (uint8_t)(1U << index)) == 0U) {
      return (int)index;
    }
  }
  return -1;
}

static bool rom_is_bound(const board_a_sensor_map_t *map,
                         const ds18b20_rom_t *rom)
{
  uint8_t index;

  for (index = 0U; index < BOARD_A_SENSOR_COUNT; ++index) {
    if (((map->valid_mask & (uint8_t)(1U << index)) != 0U) &&
        (memcmp(map->bindings[index].rom, rom->bytes,
                DS18B20_ROM_SIZE) == 0)) {
      return true;
    }
  }
  return false;
}

/*
 * Merge scan: existing bindings are never renamed, never moved and never
 * dropped; a ROM that is not bound yet only ever fills the lowest free slot.
 * That keeps logical names stable while still letting a probe added later
 * take an empty slot instead of staying not-present forever.
 */
static bool discover_devices(
    board_a_sensor_manager_t *manager, uint64_t now_us)
{
  ds18b20_rom_t rom;
  ds18b20_status_t status;
  bool added = false;
  bool search_failed = false;

  ds18b20_search_start(&manager->bus);
  for (;;) {
    int slot;

    status = ds18b20_search_next(&manager->bus, &rom);
    if (status == DS18B20_STATUS_NO_MORE_DEVICES) {
      break;
    }
    if (status != DS18B20_STATUS_OK) {
      search_failed = true;
      break;
    }
    if (rom_is_bound(&manager->map, &rom)) {
      continue;
    }
    slot = free_slot_for(&manager->map);
    if (slot < 0) {
      break;
    }
    memcpy(manager->map.bindings[slot].rom, rom.bytes, DS18B20_ROM_SIZE);
    manager->map.bindings[slot].rom_short = rom_short_id(rom.bytes);
    manager->map.bindings[slot].bound = true;
    manager->map.valid_mask |= (uint8_t)(1U << (uint8_t)slot);
    added = true;
  }

  if (added) {
    manager->map_dirty = true;
  }
  manager->discovery_complete =
      (manager->map.valid_mask == BOARD_A_SENSOR_ALL_BOUND_MASK);
  if ((manager->map.valid_mask == 0U) || search_failed) {
    manager->next_discovery_us = now_us + BOARD_A_SENSOR_RETRY_PERIOD_US;
  } else {
    manager->next_discovery_us =
        now_us + BOARD_A_SENSOR_DISCOVERY_RETRY_US;
  }
  if (manager->map.valid_mask == 0U) {
    manager->next_step_us = manager->next_discovery_us;
  }
  return manager->map.valid_mask != 0U;
}

static void publish_failure(
    board_a_sensor_manager_t *manager, uint8_t index,
    ds18b20_status_t status, uint32_t sample_time_ms)
{
  board_a_sensor_sample_t *sample = &manager->snapshot.sensors[index];

  if (manager->consecutive_failures[index] <
      BOARD_A_SENSOR_NOT_PRESENT_FAILURES) {
    manager->consecutive_failures[index]++;
  }
  sample->error = error_for_status(status);
  if (sample->has_value &&
      manager->consecutive_failures[index] <
          BOARD_A_SENSOR_NOT_PRESENT_FAILURES) {
    sample->quality = BOARD_A_QUALITY_STALE;
    sample->sample_time_ms = sample_time_ms;
    return;
  }
  clear_sample(sample, sample_time_ms);
  sample->quality = quality_for_status(status);
  sample->error = error_for_status(status);
}

bool board_a_sensor_manager_step(
    board_a_sensor_manager_t *manager, uint64_t now_us,
    board_a_sensor_snapshot_t *snapshot)
{
  uint8_t index;
  uint16_t valid_mask = 0U;

  if ((manager == NULL) || (snapshot == NULL) ||
      (now_us < manager->next_step_us)) {
    return false;
  }

  if (!manager->discovery_complete &&
      (now_us >= manager->next_discovery_us)) {
    if (!discover_devices(manager, now_us)) {
      return false;
    }
  }

  if (!manager->conversion_pending) {
    manager->conversion_status = ds18b20_start_conversion_all(&manager->bus);
    manager->conversion_started_us = now_us;
    manager->conversion_pending = true;
    if (manager->conversion_status == DS18B20_STATUS_OK) {
      manager->next_step_us = now_us + DS18B20_CONVERSION_US;
    } else {
      manager->next_step_us = now_us + BOARD_A_SENSOR_RETRY_PERIOD_US;
    }
    return false;
  }

  if (manager->conversion_status != DS18B20_STATUS_OK) {
    for (index = 0U; index < BOARD_A_SENSOR_COUNT; ++index) {
      publish_failure(
          manager, index, manager->conversion_status,
          (uint32_t)(now_us / 1000U));
    }
  } else {
    for (index = 0U; index < BOARD_A_SENSOR_COUNT; ++index) {
      board_a_sensor_sample_t *sample = &manager->snapshot.sensors[index];
      board_a_sensor_binding_t *binding = &manager->map.bindings[index];
      ds18b20_sample_t result;
      ds18b20_status_t status;

      if (!binding->bound) {
        clear_sample(sample, (uint32_t)(now_us / 1000U));
        continue;
      }
      status = ds18b20_read_scratchpad(
          &manager->bus, (const ds18b20_rom_t *)&binding->rom, &result);
      if (status == DS18B20_STATUS_OK) {
        sample->has_value = true;
        sample->temperature_x16 = result.temperature_x16;
        sample->quality = BOARD_A_QUALITY_OK;
        sample->error = BOARD_A_SENSOR_ERROR_NONE;
        sample->sample_time_ms = (uint32_t)(now_us / 1000U);
        sample->rom_short = binding->rom_short;
        manager->consecutive_failures[index] = 0U;
      } else {
        publish_failure(
            manager, index, status, (uint32_t)(now_us / 1000U));
        sample->rom_short = binding->rom_short;
      }
    }
  }

  for (index = 0U; index < BOARD_A_SENSOR_COUNT; ++index) {
    if (manager->snapshot.sensors[index].has_value) {
      valid_mask |= (uint16_t)(1U << index);
    }
  }
  manager->snapshot.sample_id++;
  manager->snapshot.sample_time_us = now_us;
  manager->snapshot.valid_mask = valid_mask;
  *snapshot = manager->snapshot;
  manager->conversion_pending = false;
  manager->next_step_us = now_us + BOARD_A_SENSOR_SCAN_PERIOD_US;
  return true;
}
