#include "sensor_manager.h"

#include <stddef.h>
#include <string.h>

static void clear_sample(board_a_sensor_sample_t *sample)
{
  sample->has_value = false;
  sample->temperature_x10 = 0U;
  sample->humidity_x10 = 0U;
  sample->quality = BOARD_A_QUALITY_NOT_PRESENT;
  sample->error = BOARD_A_SENSOR_ERROR_NONE;
  sample->sample_time_ms = 0U;
}

void board_a_sensor_manager_init(
    board_a_sensor_manager_t *manager, const dht11_port_t *port)
{
  uint8_t index;

  if (manager == NULL) {
    return;
  }

  memset(manager, 0, sizeof(*manager));
  for (index = 0U; index < BOARD_A_SENSOR_COUNT; ++index) {
    dht11_init(&manager->drivers[index], port, index);
    manager->snapshot.sensors[index].sensor_id = index;
    manager->snapshot.sensors[index].sensor_type =
        BOARD_A_SENSOR_TYPE_DHT11;
    clear_sample(&manager->snapshot.sensors[index]);
  }
  manager->next_scan_us = 0U;
}

static uint16_t quality_for_status(dht11_status_t status)
{
  switch (status) {
    case DHT11_STATUS_TIMEOUT_RESPONSE:
    case DHT11_STATUS_TIMEOUT_BIT:
      return BOARD_A_QUALITY_TIMEOUT;
    case DHT11_STATUS_CHECKSUM_ERROR:
      return BOARD_A_QUALITY_CHECKSUM_ERROR;
    case DHT11_STATUS_RANGE_ERROR:
      return BOARD_A_QUALITY_RANGE_ERROR;
    case DHT11_STATUS_TOO_SOON:
      return BOARD_A_QUALITY_STALE;
    case DHT11_STATUS_INVALID_ARGUMENT:
      return BOARD_A_QUALITY_NOT_PRESENT;
    case DHT11_STATUS_OK:
    default:
      return BOARD_A_QUALITY_NOT_PRESENT;
  }
}

static uint16_t error_for_status(dht11_status_t status)
{
  switch (status) {
    case DHT11_STATUS_TIMEOUT_RESPONSE:
      return BOARD_A_SENSOR_ERROR_TIMEOUT_RESPONSE;
    case DHT11_STATUS_TIMEOUT_BIT:
      return BOARD_A_SENSOR_ERROR_TIMEOUT_BIT;
    case DHT11_STATUS_CHECKSUM_ERROR:
      return BOARD_A_SENSOR_ERROR_CHECKSUM;
    case DHT11_STATUS_RANGE_ERROR:
      return BOARD_A_SENSOR_ERROR_RANGE;
    case DHT11_STATUS_TOO_SOON:
      return BOARD_A_SENSOR_ERROR_TOO_SOON;
    case DHT11_STATUS_INVALID_ARGUMENT:
      return BOARD_A_SENSOR_ERROR_DRIVER;
    case DHT11_STATUS_OK:
    default:
      return BOARD_A_SENSOR_ERROR_NONE;
  }
}

bool board_a_sensor_manager_scan(
    board_a_sensor_manager_t *manager, uint64_t now_us,
    board_a_sensor_snapshot_t *snapshot)
{
  uint8_t index;
  uint16_t valid_mask = 0U;
  const dht11_port_t *port;

  if ((manager == NULL) || (snapshot == NULL) ||
      (now_us < manager->next_scan_us)) {
    return false;
  }
  port = manager->drivers[0].port;
  if ((port == NULL) || (port->now_us == NULL)) {
    return false;
  }

  manager->snapshot.sample_id++;
  manager->next_scan_us = now_us + BOARD_A_SENSOR_SCAN_PERIOD_US;

  for (index = 0U; index < BOARD_A_SENSOR_COUNT; ++index) {
    board_a_sensor_sample_t *state = &manager->snapshot.sensors[index];
    dht11_sample_t result;
    dht11_status_t status =
        dht11_read(&manager->drivers[index], &result);
    uint32_t sample_time_ms =
        (uint32_t)(manager->drivers[index].port->now_us(
            manager->drivers[index].port->context) / 1000ULL);

    if (status == DHT11_STATUS_OK) {
      state->has_value = true;
      state->temperature_x10 = result.temperature_x10;
      state->humidity_x10 = result.humidity_x10;
      state->quality = BOARD_A_QUALITY_OK;
      state->error = BOARD_A_SENSOR_ERROR_NONE;
      state->sample_time_ms = result.sample_time_ms;
      manager->consecutive_failures[index] = 0U;
    } else if (status != DHT11_STATUS_TOO_SOON) {
      manager->consecutive_failures[index]++;
      state->error = error_for_status(status);
      if (manager->consecutive_failures[index] >=
          BOARD_A_SENSOR_NOT_PRESENT_FAILURES) {
        clear_sample(state);
        state->sample_time_ms = sample_time_ms;
        state->error = error_for_status(status);
      } else if (state->has_value) {
        state->quality = BOARD_A_QUALITY_STALE;
      } else {
        state->temperature_x10 = 0U;
        state->humidity_x10 = 0U;
        state->quality = quality_for_status(status);
        state->sample_time_ms = sample_time_ms;
      }
    }

    if (state->has_value) {
      valid_mask |= (uint16_t)(1U << index);
    }
  }

  manager->snapshot.valid_mask = valid_mask;
  manager->snapshot.sample_time_us = port->now_us(port->context);
  *snapshot = manager->snapshot;
  return true;
}

uint64_t board_a_sensor_manager_next_scan_us(
    const board_a_sensor_manager_t *manager)
{
  return (manager == NULL) ? 0U : manager->next_scan_us;
}
