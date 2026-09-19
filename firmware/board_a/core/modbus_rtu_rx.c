#include "modbus_rtu_rx.h"

static bool gap_elapsed(const modbus_rtu_rx_t *rx, uint32_t now_us)
{
  int32_t elapsed_us;

  if (rx->active_length == 0U) {
    return false;
  }

  /*
   * The platform samples its timestamp before an ISR may push the newest
   * byte, so now_us can legitimately be slightly older than last_byte_us.
   * Signed subtraction keeps that case negative (no gap) instead of
   * wrapping to a huge unsigned value that would split a frame apart.
   */
  elapsed_us = (int32_t)(now_us - rx->last_byte_us);
  return elapsed_us >= (int32_t)rx->t35_us;
}

static void publish_active(modbus_rtu_rx_t *rx)
{
  if (rx->active_length == 0U) {
    return;
  }

  if (rx->active_overflowed) {
    rx->overlong_frames++;
    rx->active_length = 0U;
    rx->active_overflowed = false;
    return;
  }

  if (rx->ready_valid) {
    rx->ready_overruns++;
  }

  for (uint16_t index = 0U; index < rx->active_length; ++index) {
    rx->ready_bytes[index] = rx->active_bytes[index];
  }
  rx->ready_length = rx->active_length;
  rx->ready_valid = true;
  rx->frames_ready++;
  rx->active_length = 0U;
  rx->active_overflowed = false;
}

void modbus_rtu_rx_init(modbus_rtu_rx_t *rx, uint32_t t35_us)
{
  rx->active_length = 0U;
  rx->active_overflowed = false;
  rx->ready_length = 0U;
  rx->ready_valid = false;
  rx->last_byte_us = 0U;
  rx->t35_us = t35_us;
  rx->frames_ready = 0U;
  rx->overlong_frames = 0U;
  rx->ready_overruns = 0U;
}

void modbus_rtu_rx_push(modbus_rtu_rx_t *rx, uint8_t byte, uint32_t now_us)
{
  if (gap_elapsed(rx, now_us)) {
    publish_active(rx);
  }

  if (rx->active_overflowed) {
    rx->last_byte_us = now_us;
    return;
  }

  if (rx->active_length >= MODBUS_RTU_MAX_ADU_SIZE) {
    rx->active_overflowed = true;
    rx->last_byte_us = now_us;
    return;
  }

  rx->active_bytes[rx->active_length] = byte;
  rx->active_length++;
  rx->last_byte_us = now_us;
}

void modbus_rtu_rx_poll(modbus_rtu_rx_t *rx, uint32_t now_us)
{
  if (gap_elapsed(rx, now_us)) {
    publish_active(rx);
  }
}

bool modbus_rtu_rx_take(modbus_rtu_rx_t *rx, modbus_rtu_frame_t *frame)
{
  if (!rx->ready_valid) {
    return false;
  }

  for (uint16_t index = 0U; index < rx->ready_length; ++index) {
    frame->bytes[index] = rx->ready_bytes[index];
  }
  frame->length = rx->ready_length;
  frame->overflowed = false;
  rx->ready_valid = false;
  rx->ready_length = 0U;
  return true;
}
