#include <stdio.h>
#include <string.h>

#include "at24c02.h"
#include "fake_at24c02_port.h"
#include "fake_soft_i2c.h"

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static int test_cross_page_write(void)
{
  uint8_t pattern[14];
  uint8_t *memory;
  uint16_t index;

  fake_i2c_reset();
  fake_at24c02_port_reset();
  fake_i2c_set_busy_polls_after_write(1U);

  for (index = 0U; index < sizeof(pattern); index++) {
    pattern[index] = (uint8_t)(0xA0U + index);
  }

  CHECK(at24c02_write(5U, pattern, sizeof(pattern)) == AT24C02_OK);
  CHECK(fake_i2c_page_write_count() == 3U);
  CHECK(fake_i2c_page_address(0U) == 5U);
  CHECK(fake_i2c_page_length(0U) == 3U);
  CHECK(fake_i2c_page_address(1U) == 8U);
  CHECK(fake_i2c_page_length(1U) == 8U);
  CHECK(fake_i2c_page_address(2U) == 16U);
  CHECK(fake_i2c_page_length(2U) == 3U);
  CHECK(fake_at24c02_port_delay_ms_total() == 3U);
  CHECK(fake_at24c02_port_yield_count() == 3U);

  memory = fake_i2c_memory();
  CHECK(memcmp(&memory[5], pattern, sizeof(pattern)) == 0);
  return 0;
}

static int test_full_read_and_write_boundaries(void)
{
  uint8_t pattern[AT24C02_CAPACITY_BYTES];
  uint8_t readback[6];
  uint8_t *memory;
  uint16_t index;

  fake_i2c_reset();
  fake_at24c02_port_reset();

  for (index = 0U; index < AT24C02_CAPACITY_BYTES; index++) {
    pattern[index] = (uint8_t)index;
  }

  CHECK(at24c02_write(0U, pattern, sizeof(pattern)) == AT24C02_OK);
  CHECK(fake_i2c_page_write_count() == 32U);
  CHECK(at24c02_read(250U, readback, sizeof(readback)) == AT24C02_OK);

  memory = fake_i2c_memory();
  CHECK(memcmp(&memory[250], &pattern[250], sizeof(readback)) == 0);
  CHECK(memcmp(readback, &pattern[250], sizeof(readback)) == 0);

  CHECK(at24c02_write(255U, pattern, 2U) == AT24C02_ERROR_ARGUMENT);
  CHECK(at24c02_read(255U, readback, 2U) == AT24C02_ERROR_ARGUMENT);
  CHECK(at24c02_write(0U, NULL, 1U) == AT24C02_ERROR_ARGUMENT);
  CHECK(at24c02_read(0U, NULL, 1U) == AT24C02_ERROR_ARGUMENT);
  CHECK(at24c02_write(0U, pattern, 0U) == AT24C02_ERROR_ARGUMENT);
  return 0;
}

static int test_error_propagation_and_timeout(void)
{
  uint8_t value = 0x5AU;

  fake_i2c_reset();
  fake_at24c02_port_reset();
  fake_i2c_fail_next_init(SOFT_I2C_ERROR_NOT_READY);
  CHECK(at24c02_init() == AT24C02_ERROR_NOT_READY);
  CHECK(at24c02_init() == AT24C02_OK);

  fake_i2c_fail_next_write(SOFT_I2C_ERROR_NACK);
  CHECK(at24c02_write(0U, &value, 1U) == AT24C02_ERROR_NACK);
  CHECK(fake_i2c_page_write_count() == 0U);

  fake_i2c_fail_next_read(SOFT_I2C_ERROR_BUS);
  CHECK(at24c02_read(0U, &value, 1U) == AT24C02_ERROR_BUS);

  fake_i2c_reset();
  fake_at24c02_port_reset();
  fake_i2c_set_busy_polls_after_write(
      (uint8_t)(AT24C02_WRITE_TIMEOUT_MS + 1U));
  CHECK(at24c02_write(0U, &value, 1U) == AT24C02_ERROR_WRITE_TIMEOUT);
  CHECK(fake_at24c02_port_delay_ms_total() == AT24C02_WRITE_TIMEOUT_MS);
  return 0;
}

int main(void)
{
  CHECK(test_cross_page_write() == 0);
  CHECK(test_full_read_and_write_boundaries() == 0);
  CHECK(test_error_propagation_and_timeout() == 0);
  puts("PASS test_at24c02");
  return 0;
}
