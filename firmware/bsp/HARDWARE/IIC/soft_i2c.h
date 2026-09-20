#ifndef BUSCOMM_SOFT_I2C_H
#define BUSCOMM_SOFT_I2C_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  SOFT_I2C_OK = 0,
  SOFT_I2C_ERROR_ARGUMENT,
  SOFT_I2C_ERROR_NACK,
  SOFT_I2C_ERROR_BUS,
  SOFT_I2C_ERROR_NOT_READY,
  SOFT_I2C_ERROR_BUSY
} soft_i2c_result_t;

/*
 * Platform readiness hook. The weak default in soft_i2c.c checks the
 * legacy SysTick delay backend; targets that use a different delay source
 * (for example board A with TIM2) provide a strong override.
 */
bool soft_i2c_platform_ready(void);

soft_i2c_result_t soft_i2c_init(void);
soft_i2c_result_t soft_i2c_recover(void);
soft_i2c_result_t soft_i2c_probe(uint8_t address);
soft_i2c_result_t soft_i2c_write(uint8_t address, const uint8_t *data,
                                 uint16_t length);
soft_i2c_result_t soft_i2c_write_read(uint8_t address,
                                      const uint8_t *write_data,
                                      uint16_t write_length,
                                      uint8_t *read_data,
                                      uint16_t read_length);

#endif /* BUSCOMM_SOFT_I2C_H */
