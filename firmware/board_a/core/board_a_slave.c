#include "board_a_slave.h"

#define BOARD_A_T35_US 4011U

static void sync_communication_stats(board_a_slave_t *slave)
{
  slave->model.stats.rx_frames = slave->server.stats.frames_seen;
  slave->model.stats.crc_errors = slave->server.stats.crc_errors;
  slave->model.stats.address_mismatch =
      slave->server.stats.address_mismatch;
  slave->model.stats.overlong_frames = slave->receiver.overlong_frames;
  slave->model.stats.malformed_frames =
      slave->server.stats.malformed_frames;
  slave->model.stats.broadcast_writes =
      slave->server.stats.broadcast_writes;
  slave->model.stats.broadcast_reads =
      slave->server.stats.broadcast_reads;
  slave->model.stats.exception_responses =
      slave->server.stats.exception_responses;
}

static modbus_result_t slave_read_registers(
    void *context,
    modbus_register_space_t space,
    uint16_t address,
    uint16_t quantity,
    uint16_t *values)
{
  board_a_slave_t *slave = (board_a_slave_t *)context;

  sync_communication_stats(slave);
  return board_a_model_read_registers(&slave->model, space, address,
                                      quantity, values, slave->frame_now_us);
}

static modbus_result_t slave_write_registers(
    void *context,
    uint16_t address,
    const uint16_t *values,
    uint16_t quantity)
{
  board_a_slave_t *slave = (board_a_slave_t *)context;

  return board_a_model_write_registers(&slave->model, address, values,
                                       quantity, slave->frame_now_us);
}

static const modbus_rtu_ops_t BOARD_A_SLAVE_OPS = {
  slave_read_registers,
  slave_write_registers
};

void board_a_slave_init(board_a_slave_t *slave, uint32_t session_id)
{
  board_a_model_init(&slave->model, session_id);
  slave->frame_now_us = 0U;
  modbus_rtu_rx_init(&slave->receiver, BOARD_A_T35_US);
  modbus_rtu_server_init(&slave->server, BOARD_A_SLAVE_ADDRESS,
                         &BOARD_A_SLAVE_OPS, slave);
}

void board_a_slave_push_byte(board_a_slave_t *slave,
                             uint8_t byte,
                             uint32_t now_us)
{
  modbus_rtu_rx_push(&slave->receiver, byte, now_us);
}

size_t board_a_slave_poll(board_a_slave_t *slave,
                          uint32_t now_us,
                          uint8_t *response,
                          size_t response_capacity)
{
  modbus_rtu_frame_t frame;
  size_t response_length;

  modbus_rtu_rx_poll(&slave->receiver, now_us);
  if (!modbus_rtu_rx_take(&slave->receiver, &frame)) {
    return 0U;
  }

  sync_communication_stats(slave);
  response_length = modbus_rtu_server_process(
      &slave->server, frame.bytes, frame.length, response,
      response_capacity);
  if (response_length != 0U) {
    slave->model.stats.tx_responses++;
  }
  sync_communication_stats(slave);
  return response_length;
}

void board_a_slave_tick(board_a_slave_t *slave, uint64_t now_us)
{
  board_a_model_tick(&slave->model, now_us);
}
