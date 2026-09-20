#include "diskio.h"

#include <stddef.h>

#include "sd_spi.h"

/*
 * FatFs low-level disk I/O over the software-SPI SD driver.
 *
 * The vendor FatFs example retries disk_read/disk_write errors in an
 * unbounded while loop; this port re-initializes the card at most once and
 * then reports the failure to FatFs, so a missing or damaged card can never
 * block a caller forever.
 */

#define SD_DISK_DRIVE 0u

static DSTATUS sd_disk_status = STA_NOINIT;

DSTATUS disk_initialize(BYTE pdrv)
{
  sd_spi_info_t info;

  if (pdrv != SD_DISK_DRIVE) {
    return STA_NOINIT;
  }

  if (sd_spi_ready() != 0u) {
    sd_disk_status = 0u;
    return sd_disk_status;
  }

  if (sd_spi_init(&info) == SD_SPI_OK) {
    sd_disk_status = 0u;
  } else {
    sd_disk_status = STA_NOINIT;
  }

  return sd_disk_status;
}

DSTATUS disk_status(BYTE pdrv)
{
  return (pdrv == SD_DISK_DRIVE) ? sd_disk_status : STA_NOINIT;
}

static DRESULT sd_disk_transfer(BYTE pdrv, BYTE *buff, DWORD sector, UINT count,
                                uint8_t write)
{
  uint8_t attempt;

  if ((pdrv != SD_DISK_DRIVE) || (buff == NULL) || (count == 0u)) {
    return RES_PARERR;
  }
  if ((sd_disk_status & STA_NOINIT) != 0u) {
    return RES_NOTRDY;
  }

  for (attempt = 0u; attempt < 2u; attempt++) {
    sd_spi_result_t result;

    if (sd_spi_deadline_expired() != 0u) {
      return RES_ERROR;
    }
    if (write != 0u) {
      result = sd_spi_write_blocks((uint32_t)sector, (const uint8_t *)buff,
                                   (uint32_t)count);
    } else {
      result = sd_spi_read_blocks((uint32_t)sector, (uint8_t *)buff,
                                  (uint32_t)count);
    }
    if (result == SD_SPI_OK) {
      return RES_OK;
    }

    if (attempt == 0u) {
      if (sd_spi_init(NULL) != SD_SPI_OK) {
        sd_disk_status = STA_NOINIT;
        break;
      }
    }
  }

  return RES_ERROR;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
  return sd_disk_transfer(pdrv, buff, sector, count, 0u);
}

#if _USE_WRITE
DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
  return sd_disk_transfer(pdrv, (BYTE *)buff, sector, count, 1u);
}
#endif

#if _USE_IOCTL
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
  if (pdrv != SD_DISK_DRIVE) {
    return RES_PARERR;
  }

  switch (cmd) {
    case CTRL_SYNC:
      return RES_OK;

    case GET_SECTOR_SIZE:
      if (buff == NULL) {
        return RES_PARERR;
      }
      *(WORD *)buff = (WORD)SD_SPI_BLOCK_SIZE;
      return RES_OK;

    case GET_BLOCK_SIZE:
      if (buff == NULL) {
        return RES_PARERR;
      }
      *(DWORD *)buff = 1u;
      return RES_OK;

    case GET_SECTOR_COUNT: {
      uint32_t sectors = sd_spi_sector_count();

      if ((buff == NULL) || (sectors == 0u)) {
        return RES_PARERR;
      }
      *(DWORD *)buff = (DWORD)sectors;
      return RES_OK;
    }

    default:
      return RES_PARERR;
  }
}
#endif

/*
 * FatFs calls this when a file timestamp is needed. There is no RTC in this
 * increment yet, so timestamps are written as "invalid" (FatFs stores
 * 1980-01-01). The SD bring-up record carries its build stamp in the file
 * content instead.
 */
DWORD get_fattime(void)
{
  return 0u;
}
