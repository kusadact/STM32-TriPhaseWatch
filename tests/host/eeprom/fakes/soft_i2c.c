#include "fake_soft_i2c.h"

#include <string.h>

#define FAKE_I2C_ADDRESS 0x50U
#define FAKE_I2C_CAPACITY 256U
#define FAKE_I2C_MAX_PAGE_WRITES 64U

static uint8_t memory[FAKE_I2C_CAPACITY];
static uint8_t page_addresses[FAKE_I2C_MAX_PAGE_WRITES];
static uint8_t page_lengths[FAKE_I2C_MAX_PAGE_WRITES];
static size_t page_write_count;
static uint8_t busy_polls_after_write;
static uint8_t remaining_busy_polls;
static soft_i2c_result_t next_init_result;
static soft_i2c_result_t next_write_result;
static soft_i2c_result_t next_read_result;

void fake_i2c_reset(void)
{
  memset(memory, 0xFF, sizeof(memory));
  memset(page_addresses, 0, sizeof(page_addresses));
  memset(page_lengths, 0, sizeof(page_lengths));
  page_write_count = 0U;
  busy_polls_after_write = 0U;
  remaining_busy_polls = 0U;
  next_init_result = SOFT_I2C_OK;
  next_write_result = SOFT_I2C_OK;
  next_read_result = SOFT_I2C_OK;
}

uint8_t *fake_i2c_memory(void)
{
  return memory;
}

void fake_i2c_set_busy_polls_after_write(uint8_t polls)
{
  busy_polls_after_write = polls;
}

void fake_i2c_fail_next_init(soft_i2c_result_t result)
{
  next_init_result = result;
}

void fake_i2c_fail_next_write(soft_i2c_result_t result)
{
  next_write_result = result;
}

void fake_i2c_fail_next_read(soft_i2c_result_t result)
{
  next_read_result = result;
}

size_t fake_i2c_page_write_count(void)
{
  return page_write_count;
}

uint8_t fake_i2c_page_address(size_t index)
{
  return (index < page_write_count) ? page_addresses[index] : 0U;
}

uint8_t fake_i2c_page_length(size_t index)
{
  return (index < page_write_count) ? page_lengths[index] : 0U;
}

soft_i2c_result_t soft_i2c_init(void)
{
  soft_i2c_result_t result = next_init_result;

  next_init_result = SOFT_I2C_OK;
  return result;
}

soft_i2c_result_t soft_i2c_recover(void)
{
  return SOFT_I2C_OK;
}

soft_i2c_result_t soft_i2c_probe(uint8_t address)
{
  if (address != FAKE_I2C_ADDRESS) {
    return SOFT_I2C_ERROR_NACK;
  }
  if (remaining_busy_polls > 0U) {
    remaining_busy_polls--;
    return SOFT_I2C_ERROR_NACK;
  }
  return SOFT_I2C_OK;
}

soft_i2c_result_t soft_i2c_write(uint8_t address, const uint8_t *data,
                                 uint16_t length)
{
  uint16_t memory_address;
  uint16_t payload_length;

  if (next_write_result != SOFT_I2C_OK) {
    soft_i2c_result_t result = next_write_result;

    next_write_result = SOFT_I2C_OK;
    return result;
  }
  if (address != FAKE_I2C_ADDRESS || data == NULL || length < 2U) {
    return SOFT_I2C_ERROR_ARGUMENT;
  }

  memory_address = data[0];
  payload_length = (uint16_t)(length - 1U);
  if (memory_address + payload_length > FAKE_I2C_CAPACITY) {
    return SOFT_I2C_ERROR_ARGUMENT;
  }

  if (page_write_count < FAKE_I2C_MAX_PAGE_WRITES) {
    page_addresses[page_write_count] = data[0];
    page_lengths[page_write_count] = (uint8_t)payload_length;
    page_write_count++;
  }

  memcpy(&memory[memory_address], &data[1], payload_length);
  remaining_busy_polls = busy_polls_after_write;
  return SOFT_I2C_OK;
}

soft_i2c_result_t soft_i2c_write_read(uint8_t address,
                                      const uint8_t *write_data,
                                      uint16_t write_length,
                                      uint8_t *read_data,
                                      uint16_t read_length)
{
  uint16_t memory_address;

  if (next_read_result != SOFT_I2C_OK) {
    soft_i2c_result_t result = next_read_result;

    next_read_result = SOFT_I2C_OK;
    return result;
  }
  if (address != FAKE_I2C_ADDRESS || write_data == NULL ||
      write_length != 1U || read_data == NULL || read_length == 0U) {
    return SOFT_I2C_ERROR_ARGUMENT;
  }

  memory_address = write_data[0];
  if (memory_address + read_length > FAKE_I2C_CAPACITY) {
    return SOFT_I2C_ERROR_ARGUMENT;
  }

  memcpy(read_data, &memory[memory_address], read_length);
  return SOFT_I2C_OK;
}
