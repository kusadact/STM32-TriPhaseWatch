#include "board_a_runtime.h"

static bool runtime_lock(board_a_runtime_t *runtime)
{
  if ((runtime->ops == NULL) || (runtime->ops->lock == NULL)) {
    return false;
  }
  return runtime->ops->lock(runtime->context);
}

static void runtime_unlock(board_a_runtime_t *runtime)
{
  if ((runtime->ops != NULL) && (runtime->ops->unlock != NULL)) {
    runtime->ops->unlock(runtime->context);
  }
}

void board_a_runtime_init(board_a_runtime_t *runtime,
                          uint32_t session_id,
                          const board_a_runtime_ops_t *ops,
                          void *context)
{
  board_a_slave_init(&runtime->slave, session_id);
  runtime->ops = ops;
  runtime->context = context;
}

void board_a_runtime_push_byte(board_a_runtime_t *runtime,
                               uint8_t byte,
                               uint32_t now_us)
{
  board_a_slave_push_byte(&runtime->slave, byte, now_us);
}

size_t board_a_runtime_poll_observe(board_a_runtime_t *runtime,
                                    uint32_t now_us,
                                    uint8_t *response,
                                    size_t response_capacity,
                                    bool *scheduling_state_changed)
{
  size_t response_length = 0U;
  uint32_t version_before;
  uint16_t run_state_before;
  bool start_pending_before;
  uint64_t next_sample_before;

  if (scheduling_state_changed != NULL) {
    *scheduling_state_changed = false;
  }
  if (!runtime_lock(runtime)) {
    return 0U;
  }
  version_before = runtime->slave.model.active_config.version;
  run_state_before = (uint16_t)runtime->slave.model.run_state;
  start_pending_before = runtime->slave.model.start_pending;
  next_sample_before = runtime->slave.model.next_sample_us;
  response_length = board_a_slave_poll(&runtime->slave, now_us, response,
                                       response_capacity);
  if (scheduling_state_changed != NULL) {
    *scheduling_state_changed =
        (version_before != runtime->slave.model.active_config.version) ||
        (run_state_before != (uint16_t)runtime->slave.model.run_state) ||
        (start_pending_before != runtime->slave.model.start_pending) ||
        (next_sample_before != runtime->slave.model.next_sample_us);
  }
  runtime_unlock(runtime);
  return response_length;
}

size_t board_a_runtime_poll(board_a_runtime_t *runtime,
                            uint32_t now_us,
                            uint8_t *response,
                            size_t response_capacity)
{
  return board_a_runtime_poll_observe(runtime, now_us, response,
                                      response_capacity, NULL);
}

void board_a_runtime_tick(board_a_runtime_t *runtime, uint64_t now_us)
{
  if (!runtime_lock(runtime)) {
    return;
  }
  board_a_slave_tick(&runtime->slave, now_us);
  runtime_unlock(runtime);
}

bool board_a_runtime_copy_status(board_a_runtime_t *runtime,
                                 board_a_runtime_status_t *status)
{
  if ((status == NULL) || !runtime_lock(runtime)) {
    return false;
  }

  status->run_state = (uint16_t)runtime->slave.model.run_state;
  status->last_command = runtime->slave.model.last_command;
  status->command_result = runtime->slave.model.command_result;
  status->last_command_id = runtime->slave.model.last_command_id;
  status->records_this_run = runtime->slave.model.records_this_run;
  status->sequence = runtime->slave.model.snapshot.sequence;
  status->next_sample_us = runtime->slave.model.next_sample_us;
  status->start_pending = runtime->slave.model.start_pending;
  status->stats = runtime->slave.model.stats;

  runtime_unlock(runtime);
  return true;
}

bool board_a_runtime_read_snapshot(board_a_runtime_t *runtime,
                                   board_a_snapshot_t *snapshot)
{
  if ((snapshot == NULL) || !runtime_lock(runtime)) {
    return false;
  }

  *snapshot = runtime->slave.model.snapshot;
  runtime_unlock(runtime);
  return true;
}
