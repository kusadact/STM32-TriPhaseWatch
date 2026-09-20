#include "eeprom_config.h"

#include <stddef.h>

#include "at24c02.h"
#include "stm32f4xx.h"

static volatile uint8_t eeprom_config_active;
static volatile at24c02_result_t eeprom_config_last_result_value =
    AT24C02_ERROR_NOT_READY;

static config_store_status_t eeprom_config_lock(void *context)
{
  uint32_t primask = __get_PRIMASK();

  (void)context;
  __disable_irq();
  if (eeprom_config_active != 0U) {
    __set_PRIMASK(primask);
    return CONFIG_STORE_BUSY;
  }
  eeprom_config_active = 1U;
  __set_PRIMASK(primask);
  return CONFIG_STORE_OK;
}

static void eeprom_config_unlock(void *context)
{
  uint32_t primask = __get_PRIMASK();

  (void)context;
  __disable_irq();
  eeprom_config_active = 0U;
  __set_PRIMASK(primask);
}

static config_store_status_t
eeprom_config_map_result(at24c02_result_t result)
{
  switch (result) {
  case AT24C02_OK:
    return CONFIG_STORE_OK;
  case AT24C02_ERROR_NOT_READY:
    return CONFIG_STORE_NOT_READY;
  case AT24C02_ERROR_BUSY:
    return CONFIG_STORE_BUSY;
  default:
    return CONFIG_STORE_IO_ERROR;
  }
}

static config_store_status_t
eeprom_config_read(void *context, uint16_t address, uint8_t *data,
                   uint16_t length)
{
  (void)context;
  at24c02_result_t result = at24c02_read(address, data, length);

  eeprom_config_last_result_value = result;
  return eeprom_config_map_result(result);
}

static config_store_status_t
eeprom_config_write(void *context, uint16_t address, const uint8_t *data,
                    uint16_t length)
{
  (void)context;
  at24c02_result_t result = at24c02_write(address, data, length);

  eeprom_config_last_result_value = result;
  return eeprom_config_map_result(result);
}

static const config_store_backend_t eeprom_config_backend = {
    .read = eeprom_config_read,
    .write = eeprom_config_write,
    .validate = NULL,
    .lock = eeprom_config_lock,
    .unlock = eeprom_config_unlock,
    .context = NULL,
};

config_store_status_t eeprom_config_store_init(config_store_t *store)
{
  return eeprom_config_store_init_validated(store, NULL, NULL);
}

config_store_status_t eeprom_config_store_init_validated(
    config_store_t *store, config_store_validate_fn validate,
    void *validate_context)
{
  config_store_backend_t backend = eeprom_config_backend;
  at24c02_result_t at24c02_result = at24c02_init();
  config_store_status_t result = eeprom_config_map_result(at24c02_result);

  eeprom_config_last_result_value = at24c02_result;

  if (result != CONFIG_STORE_OK) {
    return result;
  }

  backend.validate = validate;
  backend.context = validate_context;
  return config_store_init(store, &backend);
}

int eeprom_config_last_result(void)
{
  return (int)eeprom_config_last_result_value;
}
