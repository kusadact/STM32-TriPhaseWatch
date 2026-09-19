#ifndef BUSCOMM_TEST_FAKE_AT24C02_PORT_H
#define BUSCOMM_TEST_FAKE_AT24C02_PORT_H

#include <stdint.h>

void fake_at24c02_port_reset(void);
uint32_t fake_at24c02_port_delay_ms_total(void);
uint32_t fake_at24c02_port_yield_count(void);

#endif /* BUSCOMM_TEST_FAKE_AT24C02_PORT_H */
