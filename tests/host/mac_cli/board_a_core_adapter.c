/*
 * Host-only binary adapter for the fixed bare-metal board A core.
 *
 * Input records:
 *   0x00                                  exit
 *   0x01 <u16be length> <raw ADU>         process one complete RTU ADU
 *   0x02 <u64be now_us>                   drive the slave scheduler clock
 *   0x03 <u32be session_id>               reinitialize the production slave
 *   0x04 <u16be length> <bytes>           return u16be Modbus CRC
 *
 * Output records:
 *   0x00 <u16be response length> <bytes>  exchange result
 *   0x00                                  clock/reset acknowledgement
 *   <u16be crc>                           CRC query result
 *
 * The framing exists only in this test adapter. It is not a product or
 * bridge protocol.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_a_slave.h"
#include "modbus_crc.h"
#include "modbus_rtu.h"

#define ADAPTER_EXCHANGE 0x01U
#define ADAPTER_TICK 0x02U
#define ADAPTER_RESET 0x03U
#define ADAPTER_CRC 0x04U

static board_a_slave_t g_slave;
static uint64_t g_adapter_now_us;

static void reset_core(uint32_t session_id)
{
  board_a_slave_init(&g_slave, session_id);
  g_adapter_now_us = 0U;
}

static int read_exact(uint8_t *buffer, size_t length)
{
  size_t offset = 0U;

  while (offset < length)
  {
    size_t count = fread(buffer + offset, 1U, length - offset, stdin);
    if (count == 0U)
    {
      return feof(stdin) ? 0 : -1;
    }
    offset += count;
  }
  return 1;
}

static int write_exact(const uint8_t *buffer, size_t length)
{
  size_t offset = 0U;

  while (offset < length)
  {
    size_t count = fwrite(buffer + offset, 1U, length - offset, stdout);
    if (count == 0U)
    {
      return -1;
    }
    offset += count;
  }
  return fflush(stdout) == 0 ? 1 : -1;
}

static uint16_t read_u16_be(const uint8_t *bytes)
{
  return (uint16_t)(((uint16_t)bytes[0] << 8U) | (uint16_t)bytes[1]);
}

static uint32_t read_u32_be(const uint8_t *bytes)
{
  return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
         ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
}

static uint64_t read_u64_be(const uint8_t *bytes)
{
  uint64_t value = 0U;
  size_t index;

  for (index = 0U; index < 8U; ++index)
  {
    value = (value << 8U) | (uint64_t)bytes[index];
  }
  return value;
}

static void write_u16_be(uint8_t *bytes, uint16_t value)
{
  bytes[0] = (uint8_t)(value >> 8U);
  bytes[1] = (uint8_t)value;
}

static int send_exchange_response(const uint8_t *response, uint16_t length)
{
  uint8_t header[3];

  header[0] = 0U;
  write_u16_be(&header[1], length);
  if (write_exact(header, sizeof(header)) < 0)
  {
    return -1;
  }
  if ((length != 0U) && (write_exact(response, length) < 0))
  {
    return -1;
  }
  return 0;
}

static int handle_exchange(void)
{
  uint8_t length_bytes[2];
  uint8_t request[MODBUS_RTU_MAX_ADU_SIZE];
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  uint16_t request_length;
  uint16_t index;
  uint32_t poll_us;
  size_t response_length;

  if (read_exact(length_bytes, sizeof(length_bytes)) != 1)
  {
    return -1;
  }
  request_length = read_u16_be(length_bytes);
  if ((request_length < 4U) ||
      (request_length > MODBUS_RTU_MAX_ADU_SIZE))
  {
    fprintf(stderr, "invalid ADU length %u\n", (unsigned int)request_length);
    return -1;
  }
  if (read_exact(request, request_length) != 1)
  {
    return -1;
  }
  for (index = 0U; index < request_length; ++index)
  {
    uint32_t timestamp_us =
        (uint32_t)(g_adapter_now_us + ((uint64_t)index * 100U));

    board_a_slave_push_byte(&g_slave, request[index], timestamp_us);
  }
  g_adapter_now_us += ((uint64_t)request_length * 100U) + 5000U;
  poll_us = (uint32_t)g_adapter_now_us;
  response_length = board_a_slave_poll(&g_slave, poll_us, response,
                                       sizeof(response));
  return send_exchange_response(response, (uint16_t)response_length);
}

static int handle_tick(void)
{
  uint8_t now_bytes[8];
  uint8_t acknowledgement = 0U;
  uint64_t now_us;

  if (read_exact(now_bytes, sizeof(now_bytes)) != 1)
  {
    return -1;
  }
  now_us = read_u64_be(now_bytes);
  g_adapter_now_us = now_us;
  board_a_slave_tick(&g_slave, now_us);
  return write_exact(&acknowledgement, 1U) < 0 ? -1 : 0;
}

static int handle_reset(void)
{
  uint8_t session_bytes[4];
  uint8_t acknowledgement = 0U;

  if (read_exact(session_bytes, sizeof(session_bytes)) != 1)
  {
    return -1;
  }
  reset_core(read_u32_be(session_bytes));
  return write_exact(&acknowledgement, 1U) < 0 ? -1 : 0;
}

static int handle_crc(void)
{
  uint8_t length_bytes[2];
  uint8_t data[MODBUS_RTU_MAX_ADU_SIZE];
  uint8_t result[2];
  uint16_t length;

  if (read_exact(length_bytes, sizeof(length_bytes)) != 1)
  {
    return -1;
  }
  length = read_u16_be(length_bytes);
  if ((length == 0U) || (length > MODBUS_RTU_MAX_ADU_SIZE))
  {
    return -1;
  }
  if (read_exact(data, length) != 1)
  {
    return -1;
  }
  write_u16_be(result, modbus_crc16(data, length));
  return write_exact(result, sizeof(result)) < 0 ? -1 : 0;
}

int main(void)
{
  uint8_t command;

  if (setvbuf(stdout, NULL, _IONBF, 0U) != 0)
  {
    return EXIT_FAILURE;
  }
  reset_core(1U);

  while (read_exact(&command, 1U) == 1)
  {
    int result;

    switch (command)
    {
      case 0x00U:
        return EXIT_SUCCESS;
      case ADAPTER_EXCHANGE:
        result = handle_exchange();
        break;
      case ADAPTER_TICK:
        result = handle_tick();
        break;
      case ADAPTER_RESET:
        result = handle_reset();
        break;
      case ADAPTER_CRC:
        result = handle_crc();
        break;
      default:
        fprintf(stderr, "unknown adapter command 0x%02X\n",
                (unsigned int)command);
        return EXIT_FAILURE;
    }

    if (result != 0)
    {
      return EXIT_FAILURE;
    }
  }
  return ferror(stdin) ? EXIT_FAILURE : EXIT_SUCCESS;
}
