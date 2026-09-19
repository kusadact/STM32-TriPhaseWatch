#include "soft_i2c.h"

#include <stddef.h>

#include "delay.h"
#include "stm32f4xx.h"

#define SOFT_I2C_GPIO GPIOB
#define SOFT_I2C_SCL_PIN GPIO_Pin_8
#define SOFT_I2C_SDA_PIN GPIO_Pin_9
#define SOFT_I2C_HALF_PERIOD_US 2U
#define SOFT_I2C_RECOVERY_CLOCKS 9U

static volatile uint8_t soft_i2c_transaction_active;
static volatile uint8_t soft_i2c_initialized;

static soft_i2c_result_t soft_i2c_enter(void)
{
  uint32_t primask = __get_PRIMASK();

  __disable_irq();
  if (soft_i2c_transaction_active != 0U) {
    __set_PRIMASK(primask);
    return SOFT_I2C_ERROR_BUSY;
  }
  soft_i2c_transaction_active = 1U;
  __set_PRIMASK(primask);
  return SOFT_I2C_OK;
}

static void soft_i2c_leave(void)
{
  uint32_t primask = __get_PRIMASK();

  __disable_irq();
  soft_i2c_transaction_active = 0U;
  __set_PRIMASK(primask);
}

static soft_i2c_result_t soft_i2c_enter_ready(void)
{
  soft_i2c_result_t result = soft_i2c_enter();

  if (result != SOFT_I2C_OK) {
    return result;
  }
  if (soft_i2c_initialized == 0U) {
    soft_i2c_leave();
    return SOFT_I2C_ERROR_NOT_READY;
  }
  return SOFT_I2C_OK;
}

static void soft_i2c_scl_release(void)
{
  GPIO_SetBits(SOFT_I2C_GPIO, SOFT_I2C_SCL_PIN);
}

static void soft_i2c_scl_low(void)
{
  GPIO_ResetBits(SOFT_I2C_GPIO, SOFT_I2C_SCL_PIN);
}

static void soft_i2c_sda_release(void)
{
  GPIO_SetBits(SOFT_I2C_GPIO, SOFT_I2C_SDA_PIN);
}

static void soft_i2c_sda_low(void)
{
  GPIO_ResetBits(SOFT_I2C_GPIO, SOFT_I2C_SDA_PIN);
}

static uint8_t soft_i2c_scl_is_high(void)
{
  return GPIO_ReadInputDataBit(SOFT_I2C_GPIO, SOFT_I2C_SCL_PIN) != 0U;
}

static uint8_t soft_i2c_sda_is_high(void)
{
  return GPIO_ReadInputDataBit(SOFT_I2C_GPIO, SOFT_I2C_SDA_PIN) != 0U;
}

static void soft_i2c_stop(void)
{
  soft_i2c_scl_low();
  soft_i2c_sda_low();
  delay_us(SOFT_I2C_HALF_PERIOD_US);
  soft_i2c_scl_release();
  delay_us(SOFT_I2C_HALF_PERIOD_US);
  soft_i2c_sda_release();
  delay_us(SOFT_I2C_HALF_PERIOD_US);
}

soft_i2c_result_t soft_i2c_init(void)
{
  GPIO_InitTypeDef gpio = {0};
  soft_i2c_result_t result;

  if ((SysTick->CTRL & SysTick_CTRL_ENABLE_Msk) == 0U) {
    return SOFT_I2C_ERROR_NOT_READY;
  }

  result = soft_i2c_enter();
  if (result != SOFT_I2C_OK) {
    return result;
  }

  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);

  gpio.GPIO_Pin = SOFT_I2C_SCL_PIN | SOFT_I2C_SDA_PIN;
  gpio.GPIO_Mode = GPIO_Mode_OUT;
  gpio.GPIO_OType = GPIO_OType_OD;
  gpio.GPIO_PuPd = GPIO_PuPd_UP;
  gpio.GPIO_Speed = GPIO_Speed_25MHz;
  GPIO_Init(SOFT_I2C_GPIO, &gpio);

  soft_i2c_scl_release();
  soft_i2c_sda_release();
  delay_us(SOFT_I2C_HALF_PERIOD_US);
  soft_i2c_initialized = 1U;
  soft_i2c_leave();
  return SOFT_I2C_OK;
}

soft_i2c_result_t soft_i2c_recover(void)
{
  uint8_t clock_count = 0U;
  soft_i2c_result_t result = soft_i2c_enter_ready();

  if (result != SOFT_I2C_OK) {
    return result;
  }

  soft_i2c_scl_release();
  soft_i2c_sda_release();
  delay_us(SOFT_I2C_HALF_PERIOD_US);

  while ((!soft_i2c_sda_is_high()) &&
         (clock_count < SOFT_I2C_RECOVERY_CLOCKS)) {
    soft_i2c_scl_low();
    delay_us(SOFT_I2C_HALF_PERIOD_US);
    soft_i2c_scl_release();
    delay_us(SOFT_I2C_HALF_PERIOD_US);
    clock_count++;
  }

  soft_i2c_stop();

  if (!soft_i2c_scl_is_high() || !soft_i2c_sda_is_high()) {
    result = SOFT_I2C_ERROR_BUS;
  }

  soft_i2c_leave();
  return result;
}

static soft_i2c_result_t soft_i2c_start(void)
{
  soft_i2c_sda_release();
  soft_i2c_scl_release();
  delay_us(SOFT_I2C_HALF_PERIOD_US);

  if (!soft_i2c_scl_is_high() || !soft_i2c_sda_is_high()) {
    return SOFT_I2C_ERROR_BUS;
  }

  soft_i2c_sda_low();
  delay_us(SOFT_I2C_HALF_PERIOD_US);
  soft_i2c_scl_low();
  delay_us(SOFT_I2C_HALF_PERIOD_US);
  return SOFT_I2C_OK;
}

static soft_i2c_result_t soft_i2c_write_byte(uint8_t value)
{
  uint8_t mask = 0x80U;

  while (mask != 0U) {
    if ((value & mask) != 0U) {
      soft_i2c_sda_release();
    } else {
      soft_i2c_sda_low();
    }
    delay_us(SOFT_I2C_HALF_PERIOD_US);
    soft_i2c_scl_release();
    delay_us(SOFT_I2C_HALF_PERIOD_US);
    soft_i2c_scl_low();
    delay_us(SOFT_I2C_HALF_PERIOD_US);
    mask >>= 1U;
  }

  soft_i2c_sda_release();
  delay_us(SOFT_I2C_HALF_PERIOD_US);
  soft_i2c_scl_release();
  delay_us(SOFT_I2C_HALF_PERIOD_US);

  if (!soft_i2c_sda_is_high()) {
    soft_i2c_scl_low();
    delay_us(SOFT_I2C_HALF_PERIOD_US);
    return SOFT_I2C_OK;
  }

  soft_i2c_scl_low();
  delay_us(SOFT_I2C_HALF_PERIOD_US);
  return SOFT_I2C_ERROR_NACK;
}

static uint8_t soft_i2c_read_byte(uint8_t acknowledge)
{
  uint8_t value = 0U;
  uint8_t bit = 0U;

  soft_i2c_sda_release();
  for (bit = 0U; bit < 8U; bit++) {
    delay_us(SOFT_I2C_HALF_PERIOD_US);
    soft_i2c_scl_release();
    delay_us(SOFT_I2C_HALF_PERIOD_US);
    value <<= 1U;
    if (soft_i2c_sda_is_high()) {
      value |= 1U;
    }
    soft_i2c_scl_low();
    delay_us(SOFT_I2C_HALF_PERIOD_US);
  }

  if (acknowledge != 0U) {
    soft_i2c_sda_low();
  } else {
    soft_i2c_sda_release();
  }
  delay_us(SOFT_I2C_HALF_PERIOD_US);
  soft_i2c_scl_release();
  delay_us(SOFT_I2C_HALF_PERIOD_US);
  soft_i2c_scl_low();
  delay_us(SOFT_I2C_HALF_PERIOD_US);
  soft_i2c_sda_release();

  return value;
}

static soft_i2c_result_t soft_i2c_start_write(uint8_t address)
{
  soft_i2c_result_t result = soft_i2c_start();

  if (result != SOFT_I2C_OK) {
    return result;
  }

  return soft_i2c_write_byte((uint8_t)(address << 1U));
}

soft_i2c_result_t soft_i2c_probe(uint8_t address)
{
  soft_i2c_result_t result;

  if (address > 0x7FU) {
    return SOFT_I2C_ERROR_ARGUMENT;
  }

  result = soft_i2c_enter_ready();
  if (result != SOFT_I2C_OK) {
    return result;
  }

  result = soft_i2c_start_write(address);
  soft_i2c_stop();
  soft_i2c_leave();
  return result;
}

soft_i2c_result_t soft_i2c_write(uint8_t address, const uint8_t *data,
                                 uint16_t length)
{
  soft_i2c_result_t result;
  uint16_t index = 0U;

  if (address > 0x7FU || data == NULL || length == 0U) {
    return SOFT_I2C_ERROR_ARGUMENT;
  }

  result = soft_i2c_enter_ready();
  if (result != SOFT_I2C_OK) {
    return result;
  }

  result = soft_i2c_start_write(address);
  if (result == SOFT_I2C_OK) {
    while (index < length) {
      result = soft_i2c_write_byte(data[index]);
      if (result != SOFT_I2C_OK) {
        break;
      }
      index++;
    }
  }

  soft_i2c_stop();
  soft_i2c_leave();
  return result;
}

soft_i2c_result_t soft_i2c_write_read(uint8_t address,
                                      const uint8_t *write_data,
                                      uint16_t write_length,
                                      uint8_t *read_data,
                                      uint16_t read_length)
{
  soft_i2c_result_t result;
  uint16_t index = 0U;

  if (address > 0x7FU || write_data == NULL || write_length == 0U ||
      read_data == NULL || read_length == 0U) {
    return SOFT_I2C_ERROR_ARGUMENT;
  }

  result = soft_i2c_enter_ready();
  if (result != SOFT_I2C_OK) {
    return result;
  }

  result = soft_i2c_start_write(address);
  if (result == SOFT_I2C_OK) {
    while (index < write_length) {
      result = soft_i2c_write_byte(write_data[index]);
      if (result != SOFT_I2C_OK) {
        break;
      }
      index++;
    }
  }

  if (result == SOFT_I2C_OK) {
    result = soft_i2c_start();
  }

  if (result == SOFT_I2C_OK) {
    result = soft_i2c_write_byte((uint8_t)((address << 1U) | 1U));
  }

  if (result == SOFT_I2C_OK) {
    for (index = 0U; index < read_length; index++) {
      read_data[index] =
          soft_i2c_read_byte((uint8_t)(index + 1U < read_length));
    }
  }

  soft_i2c_stop();
  soft_i2c_leave();
  return result;
}
