#include "sd_spi.h"
#include "sd_spi_bus.h"

#include <string.h>

/*
 * SD card SPI-mode driver, single-block commands only.
 *
 * Protocol facts used here come from the local vendor material archive:
 *   SD card physical-layer protocol PDF
 *     chapter 7 "SPI Mode" (bus protocol, command/response packets, data
 *     tokens, timing) and chapter 5 (CSD/OCR register layout).
 *   CSD_Register.pdf (CSD fields).
 *
 * Deliberate limits of this first increment, to be revisited before P4:
 *   - CMD17/CMD24 are used for every block; CMD18/CMD25 multi-block
 *     transfers are not implemented yet.
 *   - CMD1 (MMC) is not implemented; only SD cards are supported.
 *   - SPI mode ignores the block CRC on reads and accepts the dummy CRC
 *     sent during writes.
 */

#define SD_SPI_CMD_TRIES        8u
#define SD_SPI_ACMD41_TRIES     1000u
/*
 * Bounded polling budgets for the data token and the card busy release.
 * The SD physical layer allows on the order of 100 ms read access and
 * 250 ms write programming time; at fast-mode byte time these budgets are
 * meant to cover both with margin. The values are design estimates and
 * still need hardware confirmation (see the local SD bring-up notes).
 */
#define SD_SPI_TOKEN_TRIES      0x1FFFFu
#define SD_SPI_WRITE_BUSY_TRIES 0x3FFFFu
#define SD_SPI_DATA_TOKEN       0xFEu
#define SD_SPI_WRITE_ACCEPTED   0x05u
#define SD_SPI_IDLE_BYTE        0xFFu

#define SD_CMD_GO_IDLE_STATE     0u
#define SD_CMD_SEND_IF_COND      8u
#define SD_CMD_SEND_CSD          9u
#define SD_CMD_SET_BLOCKLEN      16u
#define SD_CMD_READ_SINGLE_BLOCK 17u
#define SD_CMD_WRITE_BLOCK       24u
#define SD_CMD_APP_CMD           55u
#define SD_CMD_READ_OCR          58u
#define SD_ACMD_SEND_OP_COND     41u

static sd_spi_info_t sd_info;
static sd_spi_now_ms_fn sd_now_ms;
static void *sd_now_context;
static uint32_t sd_deadline_ms;
static uint8_t sd_deadline_enabled;

void sd_spi_set_deadline(sd_spi_now_ms_fn now_ms, void *context,
                         uint32_t deadline_ms)
{
  sd_now_ms = now_ms;
  sd_now_context = context;
  sd_deadline_ms = deadline_ms;
  sd_deadline_enabled = (now_ms != NULL) ? 1u : 0u;
}

void sd_spi_clear_deadline(void)
{
  sd_deadline_enabled = 0u;
  sd_now_ms = NULL;
  sd_now_context = NULL;
  sd_deadline_ms = 0u;
}

uint8_t sd_spi_deadline_expired(void)
{
  uint32_t now;

  if (sd_deadline_enabled == 0u || sd_now_ms == NULL) {
    return 0u;
  }
  now = sd_now_ms(sd_now_context);
  return ((int32_t)(now - sd_deadline_ms) >= 0) ? 1u : 0u;
}

/* CRC7 over the first five command bytes (SD physical layer, 7.2.2). */
static uint8_t sd_crc7(const uint8_t *data, uint8_t len)
{
  uint8_t crc = 0u;
  uint8_t i;
  uint8_t bit;

  for (i = 0u; i < len; i++) {
    for (bit = 0u; bit < 8u; bit++) {
      uint8_t in_bit = (uint8_t)((data[i] >> (7u - bit)) & 0x01u);
      uint8_t msb = (uint8_t)((crc >> 6) & 0x01u);

      crc = (uint8_t)((crc << 1) & 0x7Fu);
      if ((in_bit ^ msb) != 0u) {
        crc ^= 0x09u;
      }
    }
  }

  return crc;
}

static uint8_t sd_cmd_crc7(const uint8_t *frame)
{
  return (uint8_t)((uint8_t)(sd_crc7(frame, 5u) << 1) | 0x01u);
}

static void sd_cmd_frame(uint8_t cmd, uint32_t arg, uint8_t *frame)
{
  frame[0] = (uint8_t)(0x40u | cmd);
  frame[1] = (uint8_t)(arg >> 24);
  frame[2] = (uint8_t)(arg >> 16);
  frame[3] = (uint8_t)(arg >> 8);
  frame[4] = (uint8_t)arg;
  frame[5] = sd_cmd_crc7(frame);
}

/*
 * Sends one command frame and polls for the R1 response. CS stays asserted;
 * callers either read extra response bytes or a data block and then call
 * sd_cmd_end().
 */
static uint8_t sd_cmd_begin(uint8_t cmd, uint32_t arg)
{
  uint8_t frame[6];
  uint8_t r1 = SD_SPI_IDLE_BYTE;
  uint8_t i;

  sd_cmd_frame(cmd, arg, frame);
  sd_bus_cs(0u);
  sd_bus_xfer(SD_SPI_IDLE_BYTE);
  for (i = 0u; i < 6u; i++) {
    sd_bus_xfer(frame[i]);
  }
  for (i = 0u; i < SD_SPI_CMD_TRIES; i++) {
    r1 = sd_bus_xfer(SD_SPI_IDLE_BYTE);
    if ((r1 & 0x80u) == 0u) {
      break;
    }
  }

  return r1;
}

static void sd_cmd_end(void)
{
  sd_bus_cs(1u);
  sd_bus_xfer(SD_SPI_IDLE_BYTE);
}

/* Reads one data block (start token + payload + CRC) with CS asserted. */
static sd_spi_result_t sd_read_data(uint8_t *buf, uint16_t len)
{
  uint8_t token = SD_SPI_IDLE_BYTE;
  uint32_t i;

  for (i = 0u; i < SD_SPI_TOKEN_TRIES; i++) {
    if (sd_spi_deadline_expired() != 0u) {
      return SD_SPI_ERR_TIMEOUT;
    }
    token = sd_bus_xfer(SD_SPI_IDLE_BYTE);
    if (token != SD_SPI_IDLE_BYTE) {
      break;
    }
  }
  if (token != SD_SPI_DATA_TOKEN) {
    return (token == SD_SPI_IDLE_BYTE) ? SD_SPI_ERR_TIMEOUT : SD_SPI_ERR_RESPONSE;
  }

  for (i = 0u; i < len; i++) {
    buf[i] = sd_bus_xfer(SD_SPI_IDLE_BYTE);
  }
  sd_bus_xfer(SD_SPI_IDLE_BYTE);
  sd_bus_xfer(SD_SPI_IDLE_BYTE);

  return SD_SPI_OK;
}

/* CSD v1.0 uses a byte-based capacity, CSD v2.0 a 512 KB unit count. */
static uint32_t sd_csd_sector_count(const uint8_t *csd)
{
  uint8_t structure = (uint8_t)(csd[0] >> 6);

  if (structure == 1u) {
    uint32_t c_size = ((uint32_t)(csd[7] & 0x3Fu) << 16) |
                      ((uint32_t)csd[8] << 8) |
                      (uint32_t)csd[9];
    uint64_t sectors = ((uint64_t)c_size + 1u) * 1024u;

    return (sectors > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)sectors;
  }

  if (structure == 0u) {
    uint32_t c_size = ((uint32_t)(csd[6] & 0x03u) << 10) |
                      ((uint32_t)csd[7] << 2) |
                      ((uint32_t)csd[8] >> 6);
    uint8_t c_size_mult = (uint8_t)(((csd[9] & 0x03u) << 1) | (csd[10] >> 7));
    uint8_t read_bl_len = (uint8_t)(csd[5] & 0x0Fu);
    uint64_t bytes = ((uint64_t)(c_size + 1u)) << (c_size_mult + 2u + read_bl_len);

    return (uint32_t)(bytes / SD_SPI_BLOCK_SIZE);
  }

  return 0u;
}

static sd_spi_result_t sd_read_csd(void)
{
  uint8_t r1;
  sd_spi_result_t result;

  r1 = sd_cmd_begin(SD_CMD_SEND_CSD, 0u);
  if (r1 != 0x00u) {
    sd_cmd_end();
    return (r1 == SD_SPI_IDLE_BYTE) ? SD_SPI_ERR_TIMEOUT : SD_SPI_ERR_RESPONSE;
  }

  result = sd_read_data(sd_info.csd, 16u);
  sd_cmd_end();
  if (result == SD_SPI_OK) {
    sd_info.sector_count = sd_csd_sector_count(sd_info.csd);
  }

  return result;
}

sd_spi_result_t sd_spi_init(sd_spi_info_t *info)
{
  uint8_t r1;
  uint8_t r7[4];
  uint32_t i;
  uint32_t acmd41_arg;

  memset(&sd_info, 0, sizeof(sd_info));
  if (sd_spi_deadline_expired() != 0u) {
    return SD_SPI_ERR_TIMEOUT;
  }

  sd_bus_init();
  sd_bus_set_slow(1u);
  sd_bus_cs(1u);
  for (i = 0u; i < 10u; i++) {
    sd_bus_xfer(SD_SPI_IDLE_BYTE);
  }

  r1 = sd_cmd_begin(SD_CMD_GO_IDLE_STATE, 0u);
  sd_cmd_end();
  if (r1 != 0x01u) {
    return SD_SPI_ERR_NO_CARD;
  }

  r1 = sd_cmd_begin(SD_CMD_SEND_IF_COND, 0x1AAu);
  if (r1 == 0x01u) {
    for (i = 0u; i < 4u; i++) {
      r7[i] = sd_bus_xfer(SD_SPI_IDLE_BYTE);
    }
    sd_cmd_end();
    if ((r7[2] != 0x01u) || (r7[3] != 0xAAu)) {
      return SD_SPI_ERR_RESPONSE;
    }
    sd_info.card_type = 2u;
  } else if ((r1 & 0x7Fu) == 0x05u) {
    sd_cmd_end();
    sd_info.card_type = 1u;
  } else {
    sd_cmd_end();
    return (r1 == SD_SPI_IDLE_BYTE) ? SD_SPI_ERR_TIMEOUT : SD_SPI_ERR_RESPONSE;
  }

  acmd41_arg = (sd_info.card_type >= 2u) ? 0x40000000u : 0x00000000u;
  for (i = 0u; i < SD_SPI_ACMD41_TRIES; i++) {
    if (sd_spi_deadline_expired() != 0u) {
      return SD_SPI_ERR_TIMEOUT;
    }
    r1 = sd_cmd_begin(SD_CMD_APP_CMD, 0u);
    sd_cmd_end();
    if ((r1 & 0x80u) != 0u) {
      return SD_SPI_ERR_TIMEOUT;
    }
    if ((r1 & 0x7Fu) > 0x01u) {
      return SD_SPI_ERR_RESPONSE;
    }

    r1 = sd_cmd_begin(SD_ACMD_SEND_OP_COND, acmd41_arg);
    sd_cmd_end();
    if (r1 == 0x00u) {
      break;
    }
    if (r1 != 0x01u) {
      return SD_SPI_ERR_RESPONSE;
    }
    sd_bus_delay_ms(1u);
  }
  if (i >= SD_SPI_ACMD41_TRIES) {
    return SD_SPI_ERR_TIMEOUT;
  }

  if (sd_info.card_type >= 2u) {
    r1 = sd_cmd_begin(SD_CMD_READ_OCR, 0u);
    if (r1 != 0x00u) {
      sd_cmd_end();
      return SD_SPI_ERR_RESPONSE;
    }
    for (i = 0u; i < 4u; i++) {
      sd_info.ocr[i] = sd_bus_xfer(SD_SPI_IDLE_BYTE);
    }
    sd_cmd_end();
    sd_info.ccs = (uint8_t)((sd_info.ocr[0] & 0x40u) ? 1u : 0u);
  } else {
    memset(sd_info.ocr, 0, 4u);
    sd_info.ccs = 0u;
  }

  if (sd_info.ccs == 0u) {
    r1 = sd_cmd_begin(SD_CMD_SET_BLOCKLEN, SD_SPI_BLOCK_SIZE);
    sd_cmd_end();
    if (r1 != 0x00u) {
      return SD_SPI_ERR_RESPONSE;
    }
  }

  if (sd_read_csd() != SD_SPI_OK) {
    return SD_SPI_ERR_RESPONSE;
  }

  sd_bus_set_slow(0u);
  sd_info.initialized = 1u;
  if (info != NULL) {
    *info = sd_info;
  }

  return SD_SPI_OK;
}

uint8_t sd_spi_ready(void)
{
  return sd_info.initialized;
}

uint32_t sd_spi_sector_count(void)
{
  return sd_info.sector_count;
}

static uint32_t sd_card_address(uint32_t lba)
{
  return (sd_info.ccs != 0u) ? lba : (lba * SD_SPI_BLOCK_SIZE);
}

sd_spi_result_t sd_spi_read_blocks(uint32_t lba, uint8_t *buf, uint32_t count)
{
  uint32_t block;
  uint8_t r1;

  if (sd_spi_ready() == 0u) {
    return SD_SPI_ERR_NOT_READY;
  }
  if ((buf == NULL) || (count == 0u)) {
    return SD_SPI_ERR_PARAM;
  }

  for (block = 0u; block < count; block++, buf += SD_SPI_BLOCK_SIZE) {
    uint32_t lba_now = lba + block;
    sd_spi_result_t result;

    if (sd_spi_deadline_expired() != 0u) {
      return SD_SPI_ERR_TIMEOUT;
    }
    if ((lba_now < lba) || ((sd_info.sector_count != 0u) && (lba_now >= sd_info.sector_count))) {
      return SD_SPI_ERR_PARAM;
    }
    if ((sd_info.ccs == 0u) && (lba_now > (0xFFFFFFFFu / SD_SPI_BLOCK_SIZE))) {
      return SD_SPI_ERR_PARAM;
    }

    r1 = sd_cmd_begin(SD_CMD_READ_SINGLE_BLOCK, sd_card_address(lba_now));
    if (r1 != 0x00u) {
      sd_cmd_end();
      return (r1 == SD_SPI_IDLE_BYTE) ? SD_SPI_ERR_TIMEOUT : SD_SPI_ERR_RESPONSE;
    }

    result = sd_read_data(buf, SD_SPI_BLOCK_SIZE);
    sd_cmd_end();
    if (result != SD_SPI_OK) {
      return result;
    }
  }

  return SD_SPI_OK;
}

sd_spi_result_t sd_spi_write_blocks(uint32_t lba, const uint8_t *buf, uint32_t count)
{
  uint32_t block;
  uint32_t i;
  uint8_t r1;
  uint8_t response;

  if (sd_spi_ready() == 0u) {
    return SD_SPI_ERR_NOT_READY;
  }
  if ((buf == NULL) || (count == 0u)) {
    return SD_SPI_ERR_PARAM;
  }

  for (block = 0u; block < count; block++, buf += SD_SPI_BLOCK_SIZE) {
    uint32_t lba_now = lba + block;

    if (sd_spi_deadline_expired() != 0u) {
      return SD_SPI_ERR_TIMEOUT;
    }
    if ((lba_now < lba) || ((sd_info.sector_count != 0u) && (lba_now >= sd_info.sector_count))) {
      return SD_SPI_ERR_PARAM;
    }
    if ((sd_info.ccs == 0u) && (lba_now > (0xFFFFFFFFu / SD_SPI_BLOCK_SIZE))) {
      return SD_SPI_ERR_PARAM;
    }

    r1 = sd_cmd_begin(SD_CMD_WRITE_BLOCK, sd_card_address(lba_now));
    if (r1 != 0x00u) {
      sd_cmd_end();
      return (r1 == SD_SPI_IDLE_BYTE) ? SD_SPI_ERR_TIMEOUT : SD_SPI_ERR_RESPONSE;
    }

    sd_bus_xfer(SD_SPI_IDLE_BYTE);
    sd_bus_xfer(SD_SPI_DATA_TOKEN);
    for (i = 0u; i < SD_SPI_BLOCK_SIZE; i++) {
      sd_bus_xfer(buf[i]);
    }
    sd_bus_xfer(SD_SPI_IDLE_BYTE);
    sd_bus_xfer(SD_SPI_IDLE_BYTE);

    response = sd_bus_xfer(SD_SPI_IDLE_BYTE);
    if ((response & 0x1Fu) != SD_SPI_WRITE_ACCEPTED) {
      sd_cmd_end();
      return SD_SPI_ERR_WRITE;
    }

    for (i = 0u; i < SD_SPI_WRITE_BUSY_TRIES; i++) {
      if (sd_spi_deadline_expired() != 0u) {
        sd_cmd_end();
        return SD_SPI_ERR_TIMEOUT;
      }
      if (sd_bus_xfer(SD_SPI_IDLE_BYTE) != 0x00u) {
        break;
      }
    }
    if (i >= SD_SPI_WRITE_BUSY_TRIES) {
      sd_cmd_end();
      return SD_SPI_ERR_TIMEOUT;
    }

    sd_cmd_end();
  }

  return SD_SPI_OK;
}

const char *sd_spi_result_str(sd_spi_result_t result)
{
  switch (result) {
    case SD_SPI_OK:
      return "ok";
    case SD_SPI_ERR_PARAM:
      return "bad parameter";
    case SD_SPI_ERR_NO_CARD:
      return "no card or no R1 after CMD0";
    case SD_SPI_ERR_RESPONSE:
      return "unexpected card response";
    case SD_SPI_ERR_TIMEOUT:
      return "card timeout";
    case SD_SPI_ERR_WRITE:
      return "write rejected";
    case SD_SPI_ERR_NOT_READY:
      return "card not initialized";
    default:
      return "unknown error";
  }
}
