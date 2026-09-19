#include <stdio.h>
#include <string.h>

#include "config.h"
#include "debug_uart.h"
#include "delay.h"
#include "ff.h"
#include "led.h"
#include "sd_selftest.h"
#include "sd_spi.h"

/*
 * SD card bring-up firmware for board A (the monitoring host, which owns
 * the card socket). It runs once at boot and can be re-run by resetting or
 * by inserting a card within SD_RETRY_INTERVAL_MS.
 *
 * The test is non-destructive to existing card contents:
 *   1. software-SPI card initialization and card identity,
 *   2. raw single-block read of LBA 0 (no raw writes at all),
 *   3. FatFs mount plus append-and-read-back of a test line in SDTEST.TXT.
 * The write path is exercised through FatFs only, so no other file and no
 * raw sector is ever overwritten.
 */

#define SD_RETRY_INTERVAL_MS 5000u

static FATFS sd_fs;
static uint8_t raw_block[SD_SPI_BLOCK_SIZE] __attribute__((aligned(4)));

static int sd_run_bringup(void)
{
  sd_spi_info_t info;
  sd_spi_result_t sd_result;
  sd_selftest_result_t test;
  FRESULT fr;
  FATFS *fs = NULL;
  DWORD free_clusters = 0u;
  char line[SD_SELFTEST_MAX_LINE];
  int line_len;

  sd_result = sd_spi_init(&info);
  if (sd_result != SD_SPI_OK) {
    printf("[sd] init FAIL: %s\r\n", sd_spi_result_str(sd_result));
    return -1;
  }
  printf("[sd] init OK: type=v%u, %s, sectors=%lu\r\n",
         (unsigned)info.card_type,
         (info.ccs != 0u) ? "block addressed" : "byte addressed",
         (unsigned long)info.sector_count);

  sd_result = sd_spi_read_blocks(0u, raw_block, 1u);
  if (sd_result != SD_SPI_OK) {
    printf("[sd] raw read LBA0 FAIL: %s\r\n", sd_spi_result_str(sd_result));
    return -1;
  }
  printf("[sd] raw read LBA0 OK, boot signature=%s\r\n",
         ((raw_block[510] == 0x55u) && (raw_block[511] == 0xAAu)) ? "0x55AA" : "none");

  fr = f_mount(&sd_fs, "0:", 1);
  if (fr != FR_OK) {
    printf("[sd] FatFs mount FAIL: fr=%u\r\n", (unsigned)fr);
    return -1;
  }
  printf("[sd] FatFs mount OK\r\n");

  fr = f_getfree("0:", &free_clusters, &fs);
  if (fr == FR_OK) {
    DWORD total_kb = (DWORD)(((fs->n_fatent - 2u) * fs->csize) / 2u);
    DWORD free_kb = (DWORD)((free_clusters * fs->csize) / 2u);

    printf("[sd] volume: total=%lu KiB, free=%lu KiB\r\n",
           (unsigned long)total_kb, (unsigned long)free_kb);
  } else {
    printf("[sd] f_getfree FAIL: fr=%u\r\n", (unsigned)fr);
  }

  line_len = snprintf(line, sizeof(line), "TEST-SD-SPI build=%s %s\r\n",
                      __DATE__, __TIME__);
  if ((line_len <= 0) || ((size_t)line_len >= sizeof(line))) {
    printf("[sd] test line too long\r\n");
    return -1;
  }

  if (sd_selftest_append("0:SDTEST.TXT", line, &test) == 0) {
    printf("[sd] file test PASS: SDTEST.TXT %lu -> %lu bytes, verified=%u\r\n",
           (unsigned long)test.size_before, (unsigned long)test.size_after,
           (unsigned)test.verified);
    return 0;
  }

  printf("[sd] file test FAIL: stage=%u fr=%u written=%u read=%u\r\n",
         (unsigned)test.stage, (unsigned)test.fr,
         (unsigned)test.written, (unsigned)test.read_back);
  return -1;
}

int main(void)
{
  uint8_t sd_ok = 0u;
  uint32_t tick_ms = 0u;
  uint32_t next_try_ms = 0u;

  delay_init(168U);
  debug_uart_init(DEBUG_UART_BAUD);
  LED_Init();

  printf("\r\n");
  printf("[water-monitor] SD SPI bring-up, build %s %s\r\n", __DATE__, __TIME__);
  printf("[water-monitor] CS=PC11 MOSI=PD2 MISO=PC8 SCK=PC12\r\n");

  while (1) {
    if ((sd_ok == 0u) && (tick_ms >= next_try_ms)) {
      sd_ok = (sd_run_bringup() == 0) ? 1u : 0u;
      next_try_ms = tick_ms + SD_RETRY_INTERVAL_MS;
    }

    LED0 = 0u;
    delay_ms(500u);
    LED0 = 1u;
    delay_ms(500u);
    tick_ms += 1000u;
  }
}
