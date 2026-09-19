#ifndef BOARD_A_MODBUS_CRC_H
#define BOARD_A_MODBUS_CRC_H

#include <stddef.h>
#include <stdint.h>

uint16_t modbus_crc16(const uint8_t *data, size_t length);

#endif /* BOARD_A_MODBUS_CRC_H */
