#ifndef BOARD_A_MODBUS_RTU_H
#define BOARD_A_MODBUS_RTU_H

#include <stddef.h>
#include <stdint.h>

#define MODBUS_RTU_MAX_ADU_SIZE 256U
#define MODBUS_RTU_MAX_READ_REGISTERS 125U
#define MODBUS_RTU_MAX_WRITE_REGISTERS 123U

typedef enum {
  MODBUS_REGISTER_HOLDING = 0,
  MODBUS_REGISTER_INPUT = 1
} modbus_register_space_t;

typedef enum {
  MODBUS_RESULT_OK = 0,
  MODBUS_RESULT_ILLEGAL_ADDRESS,
  MODBUS_RESULT_ILLEGAL_VALUE,
  MODBUS_RESULT_DEVICE_FAILURE
} modbus_result_t;

typedef modbus_result_t (*modbus_read_registers_fn)(
    void *context,
    modbus_register_space_t space,
    uint16_t address,
    uint16_t quantity,
    uint16_t *values);

typedef modbus_result_t (*modbus_write_registers_fn)(
    void *context,
    uint16_t address,
    const uint16_t *values,
    uint16_t quantity);

typedef struct {
  modbus_read_registers_fn read_registers;
  modbus_write_registers_fn write_holding_registers;
} modbus_rtu_ops_t;

typedef struct {
  uint32_t frames_seen;
  uint32_t crc_errors;
  uint32_t address_mismatch;
  uint32_t malformed_frames;
  uint32_t broadcast_reads;
  uint32_t broadcast_writes;
  uint32_t exception_responses;
  uint32_t internal_errors;
} modbus_rtu_stats_t;

typedef struct {
  uint8_t address;
  const modbus_rtu_ops_t *ops;
  void *context;
  modbus_rtu_stats_t stats;
} modbus_rtu_server_t;

void modbus_rtu_server_init(modbus_rtu_server_t *server,
                            uint8_t address,
                            const modbus_rtu_ops_t *ops,
                            void *context);

/*
 * Processes one complete RTU frame and writes a response when one is required.
 * A return value of zero means the frame must be ignored without a response.
 */
size_t modbus_rtu_server_process(modbus_rtu_server_t *server,
                                 const uint8_t *request,
                                 size_t request_length,
                                 uint8_t *response,
                                 size_t response_capacity);

#endif /* BOARD_A_MODBUS_RTU_H */
