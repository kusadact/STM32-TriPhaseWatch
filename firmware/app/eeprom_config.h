#ifndef BUSCOMM_EEPROM_CONFIG_H
#define BUSCOMM_EEPROM_CONFIG_H

#include "config_store.h"

/*
 * Initializes the software I2C bus and the AT24C02 adapter.
 * Call delay_init() first. Config load/save is synchronous and may block
 * for roughly 100 ms, so call it from the storage owner task only.
 */
config_store_status_t eeprom_config_store_init(config_store_t *store);

/*
 * Initializes the EEPROM adapter with a business-schema validator and an
 * independent validator context. The legacy entry point above keeps a NULL
 * validator for compatibility with non-board-A users.
 */
config_store_status_t eeprom_config_store_init_validated(
    config_store_t *store, config_store_validate_fn validate,
    void *validate_context);

/* Last AT24C02 result, retained for status/diagnostic reporting. */
int eeprom_config_last_result(void);

#endif /* BUSCOMM_EEPROM_CONFIG_H */
