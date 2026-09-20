#ifndef SD_SPI_H
#define SD_SPI_H

#include <stdint.h>

#define SD_SPI_BLOCK_SIZE 512u

typedef enum {
  SD_SPI_OK = 0,
  SD_SPI_ERR_PARAM,
  SD_SPI_ERR_NO_CARD,
  SD_SPI_ERR_RESPONSE,
  SD_SPI_ERR_TIMEOUT,
  SD_SPI_ERR_WRITE,
  SD_SPI_ERR_NOT_READY
} sd_spi_result_t;

typedef struct {
  uint8_t initialized;
  uint8_t card_type;   /* 1 = SD v1.x, 2 = SD v2.0 or later */
  uint8_t ccs;         /* 1 = block (LBA) addressing, 0 = byte addressing */
  uint8_t ocr[4];
  uint8_t csd[16];
  uint32_t sector_count;
} sd_spi_info_t;

typedef uint32_t (*sd_spi_now_ms_fn)(void *context);

/*
 * Optional operation deadline. The owner sets this before an SD/FatFs call
 * chain; sdcard driver polling loops and disk_initialize retries honor it.
 */
void sd_spi_set_deadline(sd_spi_now_ms_fn now_ms, void *context,
                         uint32_t deadline_ms);
void sd_spi_clear_deadline(void);
uint8_t sd_spi_deadline_expired(void);

sd_spi_result_t sd_spi_init(sd_spi_info_t *info);
sd_spi_result_t sd_spi_read_blocks(uint32_t lba, uint8_t *buf, uint32_t count);
sd_spi_result_t sd_spi_write_blocks(uint32_t lba, const uint8_t *buf, uint32_t count);
uint8_t sd_spi_ready(void);
uint32_t sd_spi_sector_count(void);
const char *sd_spi_result_str(sd_spi_result_t result);

#endif /* SD_SPI_H */
