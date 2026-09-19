#include <stdio.h>
#include <string.h>

#include "diskio.h"
#include "ff.h"
#include "sd_selftest.h"
#include "sd_spi.h"

void host_sim_fail_next_read(void);

/*
 * Full-stack host test for the software-SPI SD card bring-up. The simulated
 * card replaces the GPIO bit-banging only; the SD protocol driver, the FatFs
 * disk I/O glue and the append/read-back self test are the production files
 * that are compiled into firmware/app/sdcard.
 */

#define SIM_SECTORS 4096u

static FATFS test_fs;
static uint8_t block[SD_SPI_BLOCK_SIZE * 2u];
static uint8_t pattern[SD_SPI_BLOCK_SIZE];

static unsigned checks_run;
static unsigned checks_failed;

static void check(int condition, const char *name)
{
  checks_run++;
  if (condition != 0) {
    printf("PASS  %s\n", name);
  } else {
    checks_failed++;
    printf("FAIL  %s\n", name);
  }
}

static int file_matches(const char *path, const char *expected)
{
  FIL file;
  FRESULT fr;
  UINT read = 0u;
  char buffer[128];
  size_t expected_len = strlen(expected);

  if (expected_len >= sizeof(buffer)) {
    return 0;
  }

  fr = f_open(&file, path, FA_READ);
  if (fr != FR_OK) {
    return 0;
  }
  fr = f_read(&file, buffer, (UINT)expected_len, &read);
  (void)f_close(&file);

  return (fr == FR_OK) && (read == (UINT)expected_len) &&
         (memcmp(buffer, expected, expected_len) == 0);
}

int main(void)
{
  static const char line1[] = "HOST-SD-SPI line one\r\n";
  static const char line2[] = "HOST-SD-SPI line two\r\n";
  static const char both_lines[] = "HOST-SD-SPI line one\r\nHOST-SD-SPI line two\r\n";
  sd_spi_info_t info;
  sd_spi_result_t result;
  sd_selftest_result_t test;
  FATFS *fs_out = NULL;
  DWORD free_clusters = 0u;
  FRESULT fr;

  memset(pattern, 0xA5, sizeof(pattern));
  memset(&info, 0, sizeof(info));

  result = sd_spi_init(&info);
  check(result == SD_SPI_OK, "sd_spi_init returns ok on the simulated card");
  check(info.card_type == 2u, "CMD8 identifies an SD v2.0 card");
  check(info.ccs == 1u, "CMD58 reports block (LBA) addressing");
  check(info.sector_count == SIM_SECTORS, "CSD v2.0 parsing reports 4096 sectors");

  memset(block, 0x00, sizeof(block));
  result = sd_spi_read_blocks(0u, block, 1u);
  check((result == SD_SPI_OK) && (block[0] == 0xFFu) && (block[511] == 0xFFu),
        "raw single-block read of LBA 0");

  result = sd_spi_write_blocks(100u, pattern, 1u);
  check(result == SD_SPI_OK, "raw single-block write to LBA 100");
  memset(block, 0x00, sizeof(block));
  result = sd_spi_read_blocks(100u, block, 1u);
  check((result == SD_SPI_OK) && (memcmp(block, pattern, sizeof(pattern)) == 0),
        "raw write/read payload matches at LBA 100");

  memset(block, 0x00, sizeof(block));
  result = sd_spi_read_blocks(100u, block, 2u);
  check((result == SD_SPI_OK) && (block[0] == 0xA5u) && (block[511] == 0xA5u) &&
        (block[512] == 0xFFu), "multi-sector read spans block boundaries");

  memset(block, 0x00, sizeof(block));
  check(disk_initialize(0u) == 0u, "disk_initialize registers the drive status");
  host_sim_fail_next_read();
  check((disk_read(0u, block, 200u, 1u) == RES_OK) && (block[0] == 0xFFu),
        "disk_read recovers from one injected error and reports the real data");

  /* Register the work area first; f_mkfs needs a FATFS object for the drive. */
  fr = f_mount(&test_fs, "0:", 0u);
  check(fr == FR_OK, "f_mount registers the work area without mounting");

  fr = f_mkfs("0:", 0u, 512u);
  if (fr != FR_OK) {
    printf("      f_mkfs returned FRESULT %u\n", (unsigned)fr);
  }
  check(fr == FR_OK, "f_mkfs creates a volume on the simulated card");

  fr = f_mount(&test_fs, "0:", 1u);
  if (fr != FR_OK) {
    printf("      f_mount returned FRESULT %u\n", (unsigned)fr);
  }
  check(fr == FR_OK, "f_mount succeeds");

  fr = f_getfree("0:", &free_clusters, &fs_out);
  check((fr == FR_OK) && (fs_out != NULL) && (free_clusters > 0u),
        "f_getfree reports a mounted volume");

  check(sd_selftest_append("0:SDTEST.TXT", line1, &test) == 0,
        "FatFs append of line 1 is read back identical");
  check(test.verified == 1u, "line 1 verification flag is set");

  check(sd_selftest_append("0:SDTEST.TXT", line2, &test) == 0,
        "FatFs append of line 2 is read back identical");
  check(test.size_after == (uint32_t)(strlen(line1) + strlen(line2)),
        "appended size equals the two line lengths");
  check(file_matches("0:SDTEST.TXT", both_lines),
        "file content contains both lines in order");

  fr = f_mount(NULL, "0:", 0u);
  check(fr == FR_OK, "unmount for the persistence check");
  fr = f_mount(&test_fs, "0:", 1u);
  check(fr == FR_OK, "re-mount the simulated card");
  check(file_matches("0:SDTEST.TXT", both_lines),
        "file content survives unmount/re-mount");

  printf("\n%u checks, %u failed\n", checks_run, checks_failed);

  return (checks_failed == 0u) ? 0 : 1;
}
