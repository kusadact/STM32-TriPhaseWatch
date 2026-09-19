#include "at24c02.h"

#include <stddef.h>

#include "at24c02_port.h"
#include "soft_i2c.h"

#define AT24C02_I2C_ADDRESS_7BIT 0x50U

static at24c02_result_t at24c02_map_result(soft_i2c_result_t result)
{
  switch (result) {
  case SOFT_I2C_OK:
    return AT24C02_OK;
  case SOFT_I2C_ERROR_ARGUMENT:
    return AT24C02_ERROR_ARGUMENT;
  case SOFT_I2C_ERROR_NACK:
    return AT24C02_ERROR_NACK;
  case SOFT_I2C_ERROR_NOT_READY:
    return AT24C02_ERROR_NOT_READY;
  case SOFT_I2C_ERROR_BUSY:
    return AT24C02_ERROR_BUSY;
  case SOFT_I2C_ERROR_BUS:
  default:
    return AT24C02_ERROR_BUS;
  }
}

static at24c02_result_t at24c02_wait_for_write(void)
{
  uint8_t attempt = 0U;

  for (attempt = 0U; attempt <= AT24C02_WRITE_TIMEOUT_MS; attempt++) {
    soft_i2c_result_t result = soft_i2c_probe(AT24C02_I2C_ADDRESS_7BIT);

    if (result == SOFT_I2C_OK) {
      return AT24C02_OK;
    }
    if (result != SOFT_I2C_ERROR_NACK) {
      return at24c02_map_result(result);
    }
    if (attempt < AT24C02_WRITE_TIMEOUT_MS) {
      at24c02_port_delay_ms(1U);
    }
  }

  return AT24C02_ERROR_WRITE_TIMEOUT;
}

static uint8_t at24c02_range_is_valid(uint16_t address, uint16_t length)
{
  if (length == 0U || address >= AT24C02_CAPACITY_BYTES) {
    return 0U;
  }

  return length <= (AT24C02_CAPACITY_BYTES - address);
}

at24c02_result_t at24c02_init(void)
{
  return at24c02_map_result(soft_i2c_init());
}

at24c02_result_t at24c02_probe(void)
{
  return at24c02_map_result(soft_i2c_probe(AT24C02_I2C_ADDRESS_7BIT));
}

at24c02_result_t at24c02_read(uint16_t address, uint8_t *data,
                              uint16_t length)
{
  uint8_t word_address = 0U;
  soft_i2c_result_t result;

  if (data == NULL || !at24c02_range_is_valid(address, length)) {
    return AT24C02_ERROR_ARGUMENT;
  }

  word_address = (uint8_t)address;
  result = soft_i2c_write_read(AT24C02_I2C_ADDRESS_7BIT, &word_address, 1U,
                               data, length);
  return at24c02_map_result(result);
}

at24c02_result_t at24c02_write(uint16_t address, const uint8_t *data,
                               uint16_t length)
{
  uint8_t page[AT24C02_PAGE_SIZE_BYTES + 1U];
  uint16_t remaining = length;
  const uint8_t *cursor = data;
  uint16_t cursor_address = address;

  if (data == NULL || !at24c02_range_is_valid(address, length)) {
    return AT24C02_ERROR_ARGUMENT;
  }

  while (remaining > 0U) {
    uint16_t page_offset =
        (uint16_t)(cursor_address % AT24C02_PAGE_SIZE_BYTES);
    uint16_t page_remaining =
        (uint16_t)(AT24C02_PAGE_SIZE_BYTES - page_offset);
    uint16_t chunk = (remaining < page_remaining) ? remaining : page_remaining;
    uint16_t index = 0U;
    at24c02_result_t result;

    page[0] = (uint8_t)cursor_address;
    while (index < chunk) {
      page[index + 1U] = cursor[index];
      index++;
    }

    result = at24c02_map_result(
        soft_i2c_write(AT24C02_I2C_ADDRESS_7BIT, page, (uint16_t)(chunk + 1U)));
    if (result != AT24C02_OK) {
      return result;
    }

    result = at24c02_wait_for_write();
    if (result != AT24C02_OK) {
      return result;
    }

    cursor_address = (uint16_t)(cursor_address + chunk);
    cursor += chunk;
    remaining = (uint16_t)(remaining - chunk);
    at24c02_port_yield();
  }

  return AT24C02_OK;
}
