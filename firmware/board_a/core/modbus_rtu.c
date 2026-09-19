#include "modbus_rtu.h"

#include "modbus_crc.h"

#define MODBUS_FUNCTION_READ_HOLDING 0x03U
#define MODBUS_FUNCTION_READ_INPUT 0x04U
#define MODBUS_FUNCTION_WRITE_SINGLE 0x06U
#define MODBUS_FUNCTION_WRITE_MULTIPLE 0x10U

#define MODBUS_EXCEPTION_ILLEGAL_FUNCTION 0x01U
#define MODBUS_EXCEPTION_ILLEGAL_ADDRESS 0x02U
#define MODBUS_EXCEPTION_ILLEGAL_VALUE 0x03U
#define MODBUS_EXCEPTION_DEVICE_FAILURE 0x04U

static uint16_t read_u16_be(const uint8_t *data)
{
  return (uint16_t)(((uint16_t)data[0] << 8U) | (uint16_t)data[1]);
}

static void write_u16_be(uint8_t *data, uint16_t value)
{
  data[0] = (uint8_t)(value >> 8U);
  data[1] = (uint8_t)value;
}

static void append_crc(uint8_t *response, size_t length_without_crc)
{
  uint16_t crc = modbus_crc16(response, length_without_crc);

  response[length_without_crc] = (uint8_t)(crc & 0xFFU);
  response[length_without_crc + 1U] = (uint8_t)(crc >> 8U);
}

static size_t build_exception(modbus_rtu_server_t *server,
                              uint8_t address,
                              uint8_t function,
                              uint8_t exception,
                              uint8_t *response,
                              size_t response_capacity)
{
  if (response_capacity < 5U) {
    server->stats.internal_errors++;
    return 0U;
  }

  response[0] = address;
  response[1] = (uint8_t)(function | 0x80U);
  response[2] = exception;
  append_crc(response, 3U);
  server->stats.exception_responses++;
  return 5U;
}

static uint8_t exception_for_result(modbus_result_t result)
{
  switch (result) {
    case MODBUS_RESULT_ILLEGAL_ADDRESS:
      return MODBUS_EXCEPTION_ILLEGAL_ADDRESS;
    case MODBUS_RESULT_ILLEGAL_VALUE:
      return MODBUS_EXCEPTION_ILLEGAL_VALUE;
    case MODBUS_RESULT_DEVICE_FAILURE:
      return MODBUS_EXCEPTION_DEVICE_FAILURE;
    case MODBUS_RESULT_OK:
    default:
      return MODBUS_EXCEPTION_DEVICE_FAILURE;
  }
}

static int request_is_valid_crc(const uint8_t *request, size_t length)
{
  uint16_t received;
  uint16_t calculated;

  received = (uint16_t)((uint16_t)request[length - 2U] |
                        ((uint16_t)request[length - 1U] << 8U));
  calculated = modbus_crc16(request, length - 2U);
  return received == calculated;
}

static size_t process_read_registers(modbus_rtu_server_t *server,
                                     const uint8_t *request,
                                     size_t request_length,
                                     uint8_t address,
                                     uint8_t function,
                                     int broadcast,
                                     uint8_t *response,
                                     size_t response_capacity)
{
  uint16_t start;
  uint16_t quantity;
  uint16_t values[MODBUS_RTU_MAX_READ_REGISTERS];
  modbus_register_space_t space;
  modbus_result_t result;
  size_t response_length;
  size_t index;

  if (broadcast) {
    server->stats.broadcast_reads++;
    return 0U;
  }

  if (request_length != 8U) {
    server->stats.malformed_frames++;
    return build_exception(server, address, function,
                           MODBUS_EXCEPTION_ILLEGAL_VALUE,
                           response, response_capacity);
  }

  start = read_u16_be(&request[2]);
  quantity = read_u16_be(&request[4]);
  if ((quantity == 0U) || (quantity > MODBUS_RTU_MAX_READ_REGISTERS)) {
    return build_exception(server, address, function,
                           MODBUS_EXCEPTION_ILLEGAL_VALUE,
                           response, response_capacity);
  }
  if (((uint32_t)start + (uint32_t)quantity) > 0x10000UL) {
    return build_exception(server, address, function,
                           MODBUS_EXCEPTION_ILLEGAL_ADDRESS,
                           response, response_capacity);
  }

  space = (function == MODBUS_FUNCTION_READ_HOLDING) ?
      MODBUS_REGISTER_HOLDING : MODBUS_REGISTER_INPUT;
  result = server->ops->read_registers(server->context, space, start,
                                       quantity, values);
  if (result != MODBUS_RESULT_OK) {
    return build_exception(server, address, function,
                           exception_for_result(result),
                           response, response_capacity);
  }

  response_length = 3U + ((size_t)quantity * 2U) + 2U;
  if (response_length > response_capacity) {
    server->stats.internal_errors++;
    return 0U;
  }

  response[0] = address;
  response[1] = function;
  response[2] = (uint8_t)((uint16_t)quantity * 2U);
  for (index = 0U; index < quantity; ++index) {
    write_u16_be(&response[3U + (index * 2U)], values[index]);
  }
  append_crc(response, response_length - 2U);
  return response_length;
}

static size_t process_write_single(modbus_rtu_server_t *server,
                                   const uint8_t *request,
                                   size_t request_length,
                                   uint8_t address,
                                   uint8_t function,
                                   int broadcast,
                                   uint8_t *response,
                                   size_t response_capacity)
{
  uint16_t register_address;
  uint16_t value;
  modbus_result_t result;

  if (request_length != 8U) {
    server->stats.malformed_frames++;
    if (broadcast) {
      return 0U;
    }
    return build_exception(server, address, function,
                           MODBUS_EXCEPTION_ILLEGAL_VALUE,
                           response, response_capacity);
  }

  register_address = read_u16_be(&request[2]);
  value = read_u16_be(&request[4]);
  result = server->ops->write_holding_registers(
      server->context, register_address, &value, 1U);

  if (result != MODBUS_RESULT_OK) {
    if (broadcast) {
      return 0U;
    }
    return build_exception(server, address, function,
                           exception_for_result(result),
                           response, response_capacity);
  }

  if (broadcast) {
    server->stats.broadcast_writes++;
    return 0U;
  }

  if (response_capacity < request_length) {
    server->stats.internal_errors++;
    return 0U;
  }

  for (size_t index = 0U; index < request_length; ++index) {
    response[index] = request[index];
  }
  return request_length;
}

static size_t process_write_multiple(modbus_rtu_server_t *server,
                                     const uint8_t *request,
                                     size_t request_length,
                                     uint8_t address,
                                     uint8_t function,
                                     int broadcast,
                                     uint8_t *response,
                                     size_t response_capacity)
{
  uint16_t start;
  uint16_t quantity;
  uint8_t byte_count;
  uint16_t values[MODBUS_RTU_MAX_WRITE_REGISTERS];
  size_t expected_length;
  size_t index;
  modbus_result_t result;

  if (request_length < 9U) {
    server->stats.malformed_frames++;
    if (broadcast) {
      return 0U;
    }
    return build_exception(server, address, function,
                           MODBUS_EXCEPTION_ILLEGAL_VALUE,
                           response, response_capacity);
  }

  start = read_u16_be(&request[2]);
  quantity = read_u16_be(&request[4]);
  byte_count = request[6];

  if ((quantity == 0U) || (quantity > MODBUS_RTU_MAX_WRITE_REGISTERS)) {
    if (broadcast) {
      return 0U;
    }
    return build_exception(server, address, function,
                           MODBUS_EXCEPTION_ILLEGAL_VALUE,
                           response, response_capacity);
  }
  if (byte_count != (uint8_t)(quantity * 2U)) {
    server->stats.malformed_frames++;
    if (broadcast) {
      return 0U;
    }
    return build_exception(server, address, function,
                           MODBUS_EXCEPTION_ILLEGAL_VALUE,
                           response, response_capacity);
  }

  expected_length = 9U + ((size_t)quantity * 2U);
  if (request_length != expected_length) {
    server->stats.malformed_frames++;
    if (broadcast) {
      return 0U;
    }
    return build_exception(server, address, function,
                           MODBUS_EXCEPTION_ILLEGAL_VALUE,
                           response, response_capacity);
  }

  if (((uint32_t)start + (uint32_t)quantity) > 0x10000UL) {
    if (broadcast) {
      return 0U;
    }
    return build_exception(server, address, function,
                           MODBUS_EXCEPTION_ILLEGAL_ADDRESS,
                           response, response_capacity);
  }

  for (index = 0U; index < quantity; ++index) {
    values[index] = read_u16_be(&request[7U + (index * 2U)]);
  }

  result = server->ops->write_holding_registers(
      server->context, start, values, quantity);
  if (result != MODBUS_RESULT_OK) {
    if (broadcast) {
      return 0U;
    }
    return build_exception(server, address, function,
                           exception_for_result(result),
                           response, response_capacity);
  }

  if (broadcast) {
    server->stats.broadcast_writes++;
    return 0U;
  }

  if (response_capacity < 8U) {
    server->stats.internal_errors++;
    return 0U;
  }

  response[0] = address;
  response[1] = function;
  write_u16_be(&response[2], start);
  write_u16_be(&response[4], quantity);
  append_crc(response, 6U);
  return 8U;
}

void modbus_rtu_server_init(modbus_rtu_server_t *server,
                            uint8_t address,
                            const modbus_rtu_ops_t *ops,
                            void *context)
{
  server->address = address;
  server->ops = ops;
  server->context = context;
  server->stats.frames_seen = 0U;
  server->stats.crc_errors = 0U;
  server->stats.address_mismatch = 0U;
  server->stats.malformed_frames = 0U;
  server->stats.broadcast_reads = 0U;
  server->stats.broadcast_writes = 0U;
  server->stats.exception_responses = 0U;
  server->stats.internal_errors = 0U;
}

size_t modbus_rtu_server_process(modbus_rtu_server_t *server,
                                 const uint8_t *request,
                                 size_t request_length,
                                 uint8_t *response,
                                 size_t response_capacity)
{
  uint8_t address;
  uint8_t function;
  int broadcast;

  if ((server == NULL) || (request == NULL) || (response == NULL) ||
      (request_length < 4U) || (request_length > MODBUS_RTU_MAX_ADU_SIZE)) {
    if (server != NULL) {
      server->stats.malformed_frames++;
    }
    return 0U;
  }

  server->stats.frames_seen++;
  if (!request_is_valid_crc(request, request_length)) {
    server->stats.crc_errors++;
    return 0U;
  }

  address = request[0];
  function = request[1];
  if ((address != server->address) && (address != 0U)) {
    server->stats.address_mismatch++;
    return 0U;
  }
  broadcast = address == 0U;

  switch (function) {
    case MODBUS_FUNCTION_READ_HOLDING:
    case MODBUS_FUNCTION_READ_INPUT:
      return process_read_registers(server, request, request_length, address,
                                    function, broadcast, response,
                                    response_capacity);

    case MODBUS_FUNCTION_WRITE_SINGLE:
      return process_write_single(server, request, request_length, address,
                                  function, broadcast, response,
                                  response_capacity);

    case MODBUS_FUNCTION_WRITE_MULTIPLE:
      return process_write_multiple(server, request, request_length, address,
                                    function, broadcast, response,
                                    response_capacity);

    default:
      if (broadcast) {
        return 0U;
      }
      return build_exception(server, address, function,
                             MODBUS_EXCEPTION_ILLEGAL_FUNCTION,
                             response, response_capacity);
  }
}
