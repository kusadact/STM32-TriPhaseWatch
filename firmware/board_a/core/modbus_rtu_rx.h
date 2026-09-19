#ifndef BOARD_A_MODBUS_RTU_RX_H
#define BOARD_A_MODBUS_RTU_RX_H

#include <stdbool.h>
#include <stdint.h>

#include "modbus_rtu.h"

typedef struct {
  uint8_t bytes[MODBUS_RTU_MAX_ADU_SIZE];
  uint16_t length;
  bool overflowed;
} modbus_rtu_frame_t;

typedef struct {
  uint8_t active_bytes[MODBUS_RTU_MAX_ADU_SIZE];
  uint16_t active_length;
  bool active_overflowed;
  uint8_t ready_bytes[MODBUS_RTU_MAX_ADU_SIZE];
  uint16_t ready_length;
  bool ready_valid;
  uint32_t last_byte_us;
  uint32_t t35_us;
  uint32_t frames_ready;
  uint32_t overlong_frames;
  uint32_t ready_overruns;
} modbus_rtu_rx_t;

void modbus_rtu_rx_init(modbus_rtu_rx_t *rx, uint32_t t35_us);
void modbus_rtu_rx_push(modbus_rtu_rx_t *rx, uint8_t byte, uint32_t now_us);
void modbus_rtu_rx_poll(modbus_rtu_rx_t *rx, uint32_t now_us);
bool modbus_rtu_rx_take(modbus_rtu_rx_t *rx, modbus_rtu_frame_t *frame);

#endif /* BOARD_A_MODBUS_RTU_RX_H */
