#ifndef SD_SPI_BUS_H
#define SD_SPI_BUS_H

#include <stdint.h>

/*
 * Low-level bus used by the software-SPI SD card driver (sd_spi.c).
 *
 * Target wiring uses the on-board SD socket, bit-banged in SPI mode:
 *   PC11 = SD DAT3 -> CS   (output, idle high)
 *   PD2  = SD CMD  -> MOSI (output)
 *   PC8  = SD DAT0 -> MISO (input, 47k pull-up on the board)
 *   PC12 = SD CLK  -> SCK  (output, idle low)
 *   PC9/PC10 (DAT1/DAT2) stay inputs and are unused in SPI mode.
 *
 * The host test suite provides a second implementation of this interface
 * (tests/host/sd_spi/host_sd_bus.c) so sd_spi.c can be exercised without
 * hardware.
 */

void sd_bus_init(void);
void sd_bus_set_slow(uint8_t slow);
void sd_bus_cs(uint8_t level);
uint8_t sd_bus_xfer(uint8_t tx);
void sd_bus_delay_us(uint32_t us);
void sd_bus_delay_ms(uint32_t ms);

#endif /* SD_SPI_BUS_H */
