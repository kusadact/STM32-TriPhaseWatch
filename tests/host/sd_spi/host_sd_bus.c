#include "sd_spi_bus.h"

#include <string.h>

/*
 * Host-side stand-in for the software-SPI bus and the SD card behind it.
 *
 * Only the bit-level bus and the electrical layer are replaced. sd_spi.c,
 * sd_diskio.c, sd_selftest.c and FatFs all run as production code. The
 * simulated card answers byte-level SPI transactions: it checks the CRC7 of
 * CMD0/CMD8, holds ACMD41 busy for two rounds, reports a CSD v2.0 capacity
 * of 2 MiB and implements the single-block CMD17/CMD24 transfers.
 */

#define SIM_SECTOR_SIZE 512u
#define SIM_SECTORS     4096u
#define SIM_RESP_MAX    600u

static uint8_t sim_media[SIM_SECTORS * SIM_SECTOR_SIZE];
static uint8_t sim_resp[SIM_RESP_MAX];
static uint16_t sim_resp_len;
static uint16_t sim_resp_pos;
static uint8_t sim_cs = 1u;
static uint8_t sim_cmd[6];
static uint8_t sim_cmd_len;
static uint8_t sim_r1_state = 0x01u;
static uint32_t sim_acmd41_rounds;
static uint8_t sim_write_phase; /* 0: none, 1: waiting for data token, 2: capturing */
static uint32_t sim_write_pos;
static uint32_t sim_write_lba;
static uint8_t sim_fail_read_once;

/*
 * CSD v2.0 for a 2 MiB card:
 *   CSD_STRUCTURE = 01b, C_SIZE = 3 -> (3 + 1) * 512 KiB = 2 MiB.
 * C_SIZE occupies bits [69:48], i.e. csd[7] bits [5:0], csd[8] and csd[9]
 * (CSD_Register.pdf, "CSD Register"; SD physical layer chapter 5).
 */
static const uint8_t sim_csd[16] = {
  0x40u, 0x0Eu, 0x00u, 0x32u, 0x5Bu, 0x59u, 0x00u, 0x00u,
  0x00u, 0x03u, 0x7Fu, 0x80u, 0x0Au, 0x80u, 0x00u, 0x00u
};

static void sim_queue(uint8_t value)
{
  if (sim_resp_len < SIM_RESP_MAX) {
    sim_resp[sim_resp_len++] = value;
  }
}

static void sim_queue_u32(uint32_t value)
{
  sim_queue((uint8_t)(value >> 24));
  sim_queue((uint8_t)(value >> 16));
  sim_queue((uint8_t)(value >> 8));
  sim_queue((uint8_t)value);
}

static void sim_handle_command(void)
{
  uint8_t cmd = (uint8_t)(sim_cmd[0] & 0x3Fu);
  uint32_t arg = ((uint32_t)sim_cmd[1] << 24) | ((uint32_t)sim_cmd[2] << 16) |
                 ((uint32_t)sim_cmd[3] << 8) | (uint32_t)sim_cmd[4];
  uint8_t crc = sim_cmd[5];
  uint32_t i;
  uint32_t lba;

  /* CMD0 and CMD8 are the two commands whose CRC7 must be valid. */
  if (((cmd == 0u) && (crc != 0x95u)) || ((cmd == 8u) && (crc != 0x87u))) {
    sim_queue(0x09u);
    return;
  }

  switch (cmd) {
    case 0u:
      sim_r1_state = 0x01u;
      sim_acmd41_rounds = 0u;
      sim_queue(0x01u);
      break;

    case 8u:
      sim_queue(0x01u);
      sim_queue_u32(0x000001AAu);
      break;

    case 55u:
      sim_queue(sim_r1_state);
      break;

    case 41u:
      sim_acmd41_rounds++;
      if (sim_acmd41_rounds >= 3u) {
        sim_r1_state = 0x00u;
      }
      sim_queue(sim_r1_state);
      break;

    case 58u:
      sim_queue(0x00u);
      sim_queue(0xC0u);
      sim_queue(0xFFu);
      sim_queue(0x80u);
      sim_queue(0x00u); /* OCR: CCS set, busy cleared */
      break;

    case 16u:
      sim_queue(0x00u);
      break;

    case 9u:
      sim_queue(0x00u);
      sim_queue(0xFEu);
      for (i = 0u; i < 16u; i++) {
        sim_queue(sim_csd[i]);
      }
      sim_queue(0xFFu);
      sim_queue(0xFFu);
      break;

    case 17u:
      lba = arg; /* The simulated card reports block addressing (CCS = 1). */
      if (sim_fail_read_once != 0u) {
        sim_fail_read_once = 0u;
        sim_queue(0x40u); /* injected parameter error for the retry test */
        break;
      }
      if (lba >= SIM_SECTORS) {
        sim_queue(0x40u);
        break;
      }
      sim_queue(0x00u);
      sim_queue(0xFEu);
      for (i = 0u; i < SIM_SECTOR_SIZE; i++) {
        sim_queue(sim_media[(lba * SIM_SECTOR_SIZE) + i]);
      }
      sim_queue(0xFFu);
      sim_queue(0xFFu);
      break;

    case 24u:
      lba = arg;
      if (lba >= SIM_SECTORS) {
        sim_queue(0x40u);
        break;
      }
      sim_write_lba = lba;
      sim_write_pos = 0u;
      sim_write_phase = 1u;
      sim_queue(0x00u);
      break;

    default:
      sim_queue(0x05u); /* illegal command */
      break;
  }
}

void sd_bus_init(void)
{
  memset(sim_media, 0xFF, sizeof(sim_media));
  sim_resp_len = 0u;
  sim_resp_pos = 0u;
  sim_cs = 1u;
  sim_cmd_len = 0u;
  sim_r1_state = 0x01u;
  sim_acmd41_rounds = 0u;
  sim_write_phase = 0u;
  sim_write_pos = 0u;
  sim_write_lba = 0u;
  sim_fail_read_once = 0u;
}

/* Test hook: make the next CMD17 fail once so the disk I/O retry path runs. */
void host_sim_fail_next_read(void)
{
  sim_fail_read_once = 1u;
}

void sd_bus_set_slow(uint8_t slow)
{
  (void)slow;
}

void sd_bus_cs(uint8_t level)
{
  sim_cs = (level != 0u) ? 1u : 0u;
  sim_cmd_len = 0u;
  sim_resp_len = 0u;
  sim_resp_pos = 0u;
  if (sim_cs != 0u) {
    sim_write_phase = 0u;
  }
}

uint8_t sd_bus_xfer(uint8_t tx)
{
  uint8_t rx = 0xFFu;

  if (sim_resp_pos < sim_resp_len) {
    rx = sim_resp[sim_resp_pos++];
  }

  if (sim_write_phase == 2u) {
    if (sim_write_pos < SIM_SECTOR_SIZE) {
      sim_media[(sim_write_lba * SIM_SECTOR_SIZE) + sim_write_pos] = tx;
    }
    sim_write_pos++;
    if (sim_write_pos >= (SIM_SECTOR_SIZE + 2u)) {
      sim_write_phase = 0u;
      sim_queue(0x05u); /* data accepted */
      sim_queue(0x00u); /* card busy */
      sim_queue(0x00u);
      sim_queue(0xFFu);
    }
    return 0xFFu;
  }

  if (sim_write_phase == 1u) {
    if (tx == 0xFEu) {
      sim_write_phase = 2u;
      sim_write_pos = 0u;
    }
    return rx;
  }

  if (sim_cs == 0u) {
    if (sim_cmd_len == 0u) {
      if ((tx & 0x80u) == 0u) {
        sim_cmd[0] = tx;
        sim_cmd_len = 1u;
      }
    } else {
      sim_cmd[sim_cmd_len++] = tx;
      if (sim_cmd_len == 6u) {
        sim_cmd_len = 0u;
        sim_handle_command();
      }
    }
  }

  return rx;
}

void sd_bus_delay_us(uint32_t us)
{
  (void)us;
}

void sd_bus_delay_ms(uint32_t ms)
{
  (void)ms;
}
