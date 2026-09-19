#ifndef BUSCOMM_TEST_FAKE_SOFT_I2C_H
#define BUSCOMM_TEST_FAKE_SOFT_I2C_H

#include <stddef.h>
#include <stdint.h>

#include "soft_i2c.h"

void fake_i2c_reset(void);
uint8_t *fake_i2c_memory(void);
void fake_i2c_set_busy_polls_after_write(uint8_t polls);
void fake_i2c_fail_next_init(soft_i2c_result_t result);
void fake_i2c_fail_next_write(soft_i2c_result_t result);
void fake_i2c_fail_next_read(soft_i2c_result_t result);
size_t fake_i2c_page_write_count(void);
uint8_t fake_i2c_page_address(size_t index);
uint8_t fake_i2c_page_length(size_t index);

#endif /* BUSCOMM_TEST_FAKE_SOFT_I2C_H */
