#ifndef BUSCOMM_EEPROM_CONFIG_H
#define BUSCOMM_EEPROM_CONFIG_H

#include "config_store.h"

/*
 * Initializes the software I2C bus and the AT24C02 adapter.
 * Call delay_init() first. Config load/save is synchronous and may block
 * for roughly 100 ms, so call it from the storage owner task only.
 */
config_store_status_t eeprom_config_store_init(config_store_t *store);

#endif /* BUSCOMM_EEPROM_CONFIG_H */
