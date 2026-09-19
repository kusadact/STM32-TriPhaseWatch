#ifndef BUSCOMM_AT24C02_H
#define BUSCOMM_AT24C02_H

#include <stdint.h>

#define AT24C02_CAPACITY_BYTES 256U
#define AT24C02_PAGE_SIZE_BYTES 8U
#define AT24C02_WRITE_TIMEOUT_MS 6U

typedef enum {
  AT24C02_OK = 0,
  AT24C02_ERROR_ARGUMENT,
  AT24C02_ERROR_BUS,
  AT24C02_ERROR_NACK,
  AT24C02_ERROR_WRITE_TIMEOUT,
  AT24C02_ERROR_NOT_READY,
  AT24C02_ERROR_BUSY
} at24c02_result_t;

at24c02_result_t at24c02_init(void);
at24c02_result_t at24c02_probe(void);
at24c02_result_t at24c02_read(uint16_t address, uint8_t *data,
                              uint16_t length);
at24c02_result_t at24c02_write(uint16_t address, const uint8_t *data,
                               uint16_t length);

#endif /* BUSCOMM_AT24C02_H */
