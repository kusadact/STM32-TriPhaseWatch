#include "ds18b20.h"

#include <stddef.h>
#include <string.h>

#define DS18B20_CMD_SEARCH_ROM 0xF0U
#define DS18B20_CMD_READ_ROM 0x33U
#define DS18B20_CMD_MATCH_ROM 0x55U
#define DS18B20_CMD_SKIP_ROM 0xCCU
#define DS18B20_CMD_CONVERT_T 0x44U
#define DS18B20_CMD_READ_SCRATCHPAD 0xBEU

#define DS18B20_TEMPERATURE_MIN_X16 (-880)
#define DS18B20_TEMPERATURE_MAX_X16 2000

static uint64_t ds18b20_now_us(const ds18b20_t *device)
{
  return device->port->now_us(device->port->context);
}

static void ds18b20_delay_us(const ds18b20_t *device, uint32_t delay_us)
{
  device->port->delay_us(device->port->context, delay_us);
}

static bool ds18b20_port_is_valid(const ds18b20_port_t *port)
{
  return (port != NULL) && (port->now_us != NULL) &&
      (port->delay_us != NULL) && (port->drive_low != NULL) &&
      (port->release_bus != NULL) && (port->read_level != NULL);
}

static ds18b20_status_t ds18b20_wait_high(
    const ds18b20_t *device, uint32_t timeout_us)
{
  uint64_t started_us = ds18b20_now_us(device);

  while (!device->port->read_level(device->port->context)) {
    if ((ds18b20_now_us(device) - started_us) >= timeout_us) {
      return DS18B20_STATUS_BUS_STUCK_LOW;
    }
    ds18b20_delay_us(device, 1U);
  }
  return DS18B20_STATUS_OK;
}

static ds18b20_status_t ds18b20_write_bit(
    ds18b20_t *device, bool value)
{
  device->port->drive_low(device->port->context);
  if (value) {
    ds18b20_delay_us(device, DS18B20_WRITE_ONE_LOW_US);
    device->port->release_bus(device->port->context);
    ds18b20_delay_us(device, DS18B20_SLOT_US - DS18B20_WRITE_ONE_LOW_US);
  } else {
    ds18b20_delay_us(device, DS18B20_WRITE_ZERO_LOW_US);
    device->port->release_bus(device->port->context);
    ds18b20_delay_us(device, DS18B20_SLOT_US - DS18B20_WRITE_ZERO_LOW_US);
  }
  return DS18B20_STATUS_OK;
}

static bool ds18b20_read_bit(ds18b20_t *device)
{
  bool value;

  device->port->drive_low(device->port->context);
  ds18b20_delay_us(device, DS18B20_READ_LOW_US);
  device->port->release_bus(device->port->context);
  ds18b20_delay_us(device, DS18B20_READ_SAMPLE_US - DS18B20_READ_LOW_US);
  value = device->port->read_level(device->port->context);
  ds18b20_delay_us(
      device, DS18B20_SLOT_US - DS18B20_READ_SAMPLE_US);
  return value;
}

static void ds18b20_write_byte(ds18b20_t *device, uint8_t value)
{
  uint8_t bit;

  for (bit = 0U; bit < 8U; ++bit) {
    (void)ds18b20_write_bit(device, (value & (uint8_t)(1U << bit)) != 0U);
  }
}

static uint8_t ds18b20_read_byte(ds18b20_t *device)
{
  uint8_t value = 0U;
  uint8_t bit;

  for (bit = 0U; bit < 8U; ++bit) {
    if (ds18b20_read_bit(device)) {
      value |= (uint8_t)(1U << bit);
    }
  }
  return value;
}

static uint8_t ds18b20_rom_bit(
    const ds18b20_rom_t *rom, uint8_t bit_index)
{
  return (uint8_t)((rom->bytes[bit_index / 8U] >>
                    (bit_index % 8U)) & 1U);
}

static void ds18b20_rom_set_bit(
    ds18b20_rom_t *rom, uint8_t bit_index, bool value)
{
  uint8_t mask = (uint8_t)(1U << (bit_index % 8U));

  if (value) {
    rom->bytes[bit_index / 8U] |= mask;
  } else {
    rom->bytes[bit_index / 8U] &= (uint8_t)~mask;
  }
}

const char *ds18b20_status_name(ds18b20_status_t status)
{
  switch (status) {
    case DS18B20_STATUS_OK:
      return "OK";
    case DS18B20_STATUS_RESET_TIMEOUT:
      return "RESET_TIMEOUT";
    case DS18B20_STATUS_BUS_STUCK_LOW:
      return "BUS_STUCK_LOW";
    case DS18B20_STATUS_ROM_CRC_ERROR:
      return "ROM_CRC_ERROR";
    case DS18B20_STATUS_ROM_FAMILY_ERROR:
      return "ROM_FAMILY_ERROR";
    case DS18B20_STATUS_SEARCH_ERROR:
      return "SEARCH_ERROR";
    case DS18B20_STATUS_NO_MORE_DEVICES:
      return "NO_MORE_DEVICES";
    case DS18B20_STATUS_SCRATCHPAD_CRC_ERROR:
      return "SCRATCHPAD_CRC_ERROR";
    case DS18B20_STATUS_RANGE_ERROR:
      return "RANGE_ERROR";
    case DS18B20_STATUS_INVALID_ARGUMENT:
      return "INVALID_ARGUMENT";
    case DS18B20_STATUS_NO_RESPONSE:
      return "NO_RESPONSE";
    default:
      return "UNKNOWN";
  }
}

uint8_t ds18b20_crc8(const uint8_t *data, uint8_t length)
{
  uint8_t crc = 0U;
  uint8_t index;

  if (data == NULL) {
    return 0U;
  }
  for (index = 0U; index < length; ++index) {
    uint8_t bit;
    uint8_t value = data[index];

    for (bit = 0U; bit < 8U; ++bit) {
      uint8_t mix = (uint8_t)((crc ^ value) & 0x01U);
      crc >>= 1U;
      if (mix != 0U) {
        crc ^= 0x8CU;
      }
      value >>= 1U;
    }
  }
  return crc;
}

bool ds18b20_rom_is_valid(const ds18b20_rom_t *rom)
{
  if (rom == NULL) {
    return false;
  }
  if (rom->bytes[0] != DS18B20_FAMILY_CODE) {
    return false;
  }
  return ds18b20_crc8(rom->bytes, DS18B20_ROM_SIZE - 1U) ==
      rom->bytes[DS18B20_ROM_SIZE - 1U];
}

void ds18b20_init(ds18b20_t *device, const ds18b20_port_t *port)
{
  if (device == NULL) {
    return;
  }
  device->port = port;
  device->last_discrepancy = 0U;
  device->last_device = false;
}

ds18b20_status_t ds18b20_reset(ds18b20_t *device)
{
  ds18b20_status_t status;

  if ((device == NULL) || !ds18b20_port_is_valid(device->port)) {
    return DS18B20_STATUS_INVALID_ARGUMENT;
  }

  device->port->drive_low(device->port->context);
  ds18b20_delay_us(device, DS18B20_RESET_LOW_US);
  device->port->release_bus(device->port->context);
  ds18b20_delay_us(device, DS18B20_PRESENCE_WAIT_US);

  if (device->port->read_level(device->port->context)) {
    return DS18B20_STATUS_RESET_TIMEOUT;
  }
  status = ds18b20_wait_high(device, DS18B20_PRESENCE_TIMEOUT_US);
  if (status != DS18B20_STATUS_OK) {
    return status;
  }
  ds18b20_delay_us(device, DS18B20_SLOT_US);
  return DS18B20_STATUS_OK;
}

void ds18b20_search_start(ds18b20_t *device)
{
  if (device == NULL) {
    return;
  }
  device->last_discrepancy = 0U;
  memset(&device->search_rom, 0, sizeof(device->search_rom));
  device->last_device = false;
}

ds18b20_status_t ds18b20_search_next(
    ds18b20_t *device, ds18b20_rom_t *rom)
{
  uint8_t last_zero = 0U;
  uint8_t bit_number;
  ds18b20_status_t status;
  ds18b20_rom_t *search_rom;

  if ((device == NULL) || (rom == NULL) ||
      !ds18b20_port_is_valid(device->port)) {
    return DS18B20_STATUS_INVALID_ARGUMENT;
  }
  if (device->last_device) {
    return DS18B20_STATUS_NO_MORE_DEVICES;
  }

  status = ds18b20_reset(device);
  if (status != DS18B20_STATUS_OK) {
    return status;
  }
  ds18b20_write_byte(device, DS18B20_CMD_SEARCH_ROM);

  /*
   * The discrepancy branch below copies the path taken in the previous pass,
   * so the working ROM must survive between calls. It lives in the device
   * state instead of the caller's buffer and is copied out once the pass is
   * complete. Clearing it every pass made the search revisit the same two
   * ROMs forever for some device sets.
   */
  search_rom = &device->search_rom;

  for (bit_number = 1U; bit_number <= 64U; ++bit_number) {
    uint8_t index = (uint8_t)(bit_number - 1U);
    bool id_bit = ds18b20_read_bit(device);
    bool complement = ds18b20_read_bit(device);
    bool direction;

    if (id_bit && complement) {
      return DS18B20_STATUS_SEARCH_ERROR;
    }
    if (id_bit != complement) {
      direction = id_bit;
    } else {
      if (bit_number < device->last_discrepancy) {
        direction = ds18b20_rom_bit(search_rom, index) != 0U;
      } else if (bit_number == device->last_discrepancy) {
        direction = true;
      } else {
        direction = false;
      }
      if (!direction) {
        last_zero = bit_number;
      }
    }
    ds18b20_rom_set_bit(search_rom, index, direction);
    (void)ds18b20_write_bit(device, direction);
  }

  *rom = *search_rom;
  if (last_zero == 0U) {
    device->last_device = true;
  } else {
    device->last_discrepancy = last_zero;
  }

  if (rom->bytes[0] != DS18B20_FAMILY_CODE) {
    return DS18B20_STATUS_ROM_FAMILY_ERROR;
  }
  if (!ds18b20_rom_is_valid(rom)) {
    return DS18B20_STATUS_ROM_CRC_ERROR;
  }
  return DS18B20_STATUS_OK;
}

ds18b20_status_t ds18b20_match_rom(
    ds18b20_t *device, const ds18b20_rom_t *rom)
{
  uint8_t index;
  ds18b20_status_t status;

  if ((device == NULL) || (rom == NULL) || !ds18b20_rom_is_valid(rom)) {
    return DS18B20_STATUS_INVALID_ARGUMENT;
  }
  status = ds18b20_reset(device);
  if (status != DS18B20_STATUS_OK) {
    return status;
  }
  ds18b20_write_byte(device, DS18B20_CMD_MATCH_ROM);
  for (index = 0U; index < DS18B20_ROM_SIZE; ++index) {
    ds18b20_write_byte(device, rom->bytes[index]);
  }
  return DS18B20_STATUS_OK;
}

ds18b20_status_t ds18b20_skip_rom(ds18b20_t *device)
{
  ds18b20_status_t status;

  if ((device == NULL) || !ds18b20_port_is_valid(device->port)) {
    return DS18B20_STATUS_INVALID_ARGUMENT;
  }
  status = ds18b20_reset(device);
  if (status != DS18B20_STATUS_OK) {
    return status;
  }
  ds18b20_write_byte(device, DS18B20_CMD_SKIP_ROM);
  return DS18B20_STATUS_OK;
}

ds18b20_status_t ds18b20_start_conversion_all(ds18b20_t *device)
{
  ds18b20_status_t status = ds18b20_skip_rom(device);

  if (status != DS18B20_STATUS_OK) {
    return status;
  }
  ds18b20_write_byte(device, DS18B20_CMD_CONVERT_T);
  return DS18B20_STATUS_OK;
}

ds18b20_status_t ds18b20_read_scratchpad(
    ds18b20_t *device, const ds18b20_rom_t *rom, ds18b20_sample_t *sample)
{
  uint8_t index;
  int16_t raw;
  ds18b20_status_t status;
  bool all_ff = true;
  bool all_zero = true;

  if ((device == NULL) || (rom == NULL) || (sample == NULL)) {
    return DS18B20_STATUS_INVALID_ARGUMENT;
  }
  status = ds18b20_match_rom(device, rom);
  if (status != DS18B20_STATUS_OK) {
    return status;
  }
  ds18b20_write_byte(device, DS18B20_CMD_READ_SCRATCHPAD);
  for (index = 0U; index < 9U; ++index) {
    sample->scratchpad[index] = ds18b20_read_byte(device);
    if (sample->scratchpad[index] != 0xFFU) {
      all_ff = false;
    }
    if (sample->scratchpad[index] != 0x00U) {
      all_zero = false;
    }
  }
  if (all_ff || all_zero) {
    return DS18B20_STATUS_NO_RESPONSE;
  }
  if (ds18b20_crc8(sample->scratchpad, 8U) != sample->scratchpad[8]) {
    return DS18B20_STATUS_SCRATCHPAD_CRC_ERROR;
  }

  raw = (int16_t)(((uint16_t)sample->scratchpad[1] << 8U) |
                  (uint16_t)sample->scratchpad[0]);
  if ((raw < DS18B20_TEMPERATURE_MIN_X16) ||
      (raw > DS18B20_TEMPERATURE_MAX_X16)) {
    return DS18B20_STATUS_RANGE_ERROR;
  }
  sample->temperature_x16 = raw;
  return DS18B20_STATUS_OK;
}
