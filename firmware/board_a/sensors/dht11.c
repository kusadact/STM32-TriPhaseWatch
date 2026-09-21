#include "dht11.h"

#include <stddef.h>

#define DHT11_ONE_MIN_HIGH_US 45U
#define DHT11_WAIT_GUARD 200000U
#define DHT11_HUMIDITY_MIN_X10 200U
#define DHT11_HUMIDITY_MAX_X10 900U
#define DHT11_TEMPERATURE_MIN_C 0
#define DHT11_TEMPERATURE_MAX_C 50

static uint64_t dht11_now_us(const dht11_t *device)
{
  return device->port->now_us(device->port->context);
}

static bool dht11_port_is_valid(const dht11_port_t *port)
{
  return (port != NULL) && (port->now_us != NULL) &&
      (port->delay_us != NULL) && (port->drive_low != NULL) &&
      (port->release_input != NULL) &&
      (port->read_level != NULL);
}

static dht11_status_t dht11_wait_level(
    const dht11_t *device, bool level, uint32_t timeout_us,
    dht11_status_t timeout_status, uint32_t *elapsed_us)
{
  uint64_t started_us = dht11_now_us(device);
  uint32_t guard = 0U;

  for (;;) {
    uint64_t now_us = dht11_now_us(device);
    uint64_t elapsed = now_us - started_us;

    if (device->port->read_level(device->port->context,
                                 device->sensor_id) == level) {
      if (elapsed_us != NULL) {
        *elapsed_us = (elapsed > UINT32_MAX) ?
            UINT32_MAX : (uint32_t)elapsed;
      }
      return DHT11_STATUS_OK;
    }
    if ((elapsed >= timeout_us) || (guard++ >= DHT11_WAIT_GUARD)) {
      return timeout_status;
    }
  }
}

static dht11_status_t dht11_measure_high_bit(
    const dht11_t *device, bool *is_one)
{
  uint64_t started_us = dht11_now_us(device);
  uint32_t guard = 0U;

  for (;;) {
    uint64_t now_us = dht11_now_us(device);
    uint64_t elapsed = now_us - started_us;

    if (!device->port->read_level(device->port->context,
                                  device->sensor_id)) {
      *is_one = false;
      return DHT11_STATUS_OK;
    }
    if (elapsed >= DHT11_ONE_MIN_HIGH_US) {
      *is_one = true;
      return DHT11_STATUS_OK;
    }
    if ((elapsed >= DHT11_BIT_TIMEOUT_US) ||
        (guard++ >= DHT11_WAIT_GUARD)) {
      return DHT11_STATUS_TIMEOUT_BIT;
    }
  }
}

const char *dht11_status_name(dht11_status_t status)
{
  switch (status) {
    case DHT11_STATUS_OK:
      return "OK";
    case DHT11_STATUS_TIMEOUT_RESPONSE:
      return "TIMEOUT_RESPONSE";
    case DHT11_STATUS_TIMEOUT_BIT:
      return "TIMEOUT_BIT";
    case DHT11_STATUS_CHECKSUM_ERROR:
      return "CHECKSUM_ERROR";
    case DHT11_STATUS_RANGE_ERROR:
      return "RANGE_ERROR";
    case DHT11_STATUS_TOO_SOON:
      return "TOO_SOON";
    case DHT11_STATUS_INVALID_ARGUMENT:
      return "INVALID_ARGUMENT";
    default:
      return "UNKNOWN";
  }
}

void dht11_init(dht11_t *device, const dht11_port_t *port,
                uint8_t sensor_id)
{
  if (device == NULL) {
    return;
  }

  device->port = port;
  device->sensor_id = sensor_id;
  device->last_start_us = 0U;
  device->has_started = false;
}

dht11_status_t dht11_read(dht11_t *device, dht11_sample_t *sample)
{
  uint8_t bytes[5] = {0U, 0U, 0U, 0U, 0U};
  uint64_t started_us;
  uint8_t bit_index;
  uint8_t checksum;
  uint8_t byte_index;
  uint16_t humidity_x10;
  int32_t temperature_c;
  uint16_t temperature_x10;
  dht11_status_t status;

  if ((device == NULL) || (sample == NULL) ||
      !dht11_port_is_valid(device->port)) {
    return DHT11_STATUS_INVALID_ARGUMENT;
  }

  started_us = dht11_now_us(device);
  if (device->has_started &&
      ((started_us - device->last_start_us) < DHT11_MIN_INTERVAL_US)) {
    return DHT11_STATUS_TOO_SOON;
  }
  device->last_start_us = started_us;
  device->has_started = true;

  device->port->drive_low(device->port->context, device->sensor_id);
  device->port->delay_us(device->port->context, DHT11_START_LOW_US);
  if (device->port->begin_read != NULL) {
    device->port->begin_read(device->port->context, device->sensor_id);
  }
  device->port->release_input(device->port->context, device->sensor_id);

  status = dht11_wait_level(
      device, false, DHT11_TIMEOUT_US,
      DHT11_STATUS_TIMEOUT_RESPONSE, NULL);
  if (status == DHT11_STATUS_OK) {
    status = dht11_wait_level(
        device, true, DHT11_TIMEOUT_US,
        DHT11_STATUS_TIMEOUT_RESPONSE, NULL);
  }
  if (status == DHT11_STATUS_OK) {
    status = dht11_wait_level(
        device, false, DHT11_BIT_TIMEOUT_US,
        DHT11_STATUS_TIMEOUT_BIT, NULL);
  }

  for (byte_index = 0U;
       (status == DHT11_STATUS_OK) && (byte_index < 5U);
       ++byte_index) {
    for (bit_index = 0U; bit_index < 8U; ++bit_index) {
      bool is_one;

      status = dht11_wait_level(
          device, true, DHT11_BIT_TIMEOUT_US,
          DHT11_STATUS_TIMEOUT_BIT, NULL);
      if (status != DHT11_STATUS_OK) {
        break;
      }
      status = dht11_measure_high_bit(device, &is_one);
      if (status != DHT11_STATUS_OK) {
        break;
      }

      bytes[byte_index] <<= 1U;
      if ((byte_index == (5U - 1U)) && (bit_index == (8U - 1U))) {
        /*
         * The final bit is followed by the idle-high bus, so a zero cannot be
         * distinguished by waiting for a closing low. The checksum's LSB is
         * known from the first four bytes; use it and reject a deterministic
         * low-before-threshold contradiction.
         */
        bool expected_one =
            (((bytes[0] + bytes[1] + bytes[2] + bytes[3]) & 1U) != 0U);

        if (!is_one && expected_one) {
          status = DHT11_STATUS_CHECKSUM_ERROR;
          break;
        }
        if (expected_one) {
          bytes[byte_index] |= 1U;
        }
      } else {
        if (is_one) {
          bytes[byte_index] |= 1U;
        }
        status = dht11_wait_level(
            device, false, DHT11_BIT_TIMEOUT_US,
            DHT11_STATUS_TIMEOUT_BIT, NULL);
        if (status != DHT11_STATUS_OK) {
          break;
        }
      }
    }
  }

  if (device->port->end_read != NULL) {
    device->port->end_read(device->port->context, device->sensor_id);
  }
  device->port->release_input(device->port->context, device->sensor_id);

  if (status != DHT11_STATUS_OK) {
    return status;
  }

  checksum = (uint8_t)(bytes[0] + bytes[1] + bytes[2] + bytes[3]);
  if (checksum != bytes[4]) {
    return DHT11_STATUS_CHECKSUM_ERROR;
  }

  humidity_x10 = (uint16_t)((uint16_t)bytes[0] * 10U + bytes[1]);
  temperature_c = ((bytes[2] & 0x80U) != 0U) ?
      -((int32_t)(bytes[2] & 0x7FU)) : (int32_t)bytes[2];
  temperature_x10 = (uint16_t)(temperature_c * 10 +
                               (int32_t)bytes[3]);

  if ((humidity_x10 < DHT11_HUMIDITY_MIN_X10) ||
      (humidity_x10 > DHT11_HUMIDITY_MAX_X10) ||
      (temperature_c < DHT11_TEMPERATURE_MIN_C) ||
      (temperature_c > DHT11_TEMPERATURE_MAX_C)) {
    return DHT11_STATUS_RANGE_ERROR;
  }

  sample->temperature_x10 = temperature_x10;
  sample->humidity_x10 = humidity_x10;
  sample->sample_time_ms =
      (uint32_t)(dht11_now_us(device) / 1000ULL);
  return DHT11_STATUS_OK;
}
