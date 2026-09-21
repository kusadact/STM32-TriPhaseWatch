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

bool board_a_runtime_set_data_source(board_a_runtime_t *runtime,
                                     uint16_t source)
{
  bool updated = false;

  if (!runtime_lock(runtime)) {
    return false;
  }
  updated = board_a_model_set_data_source(&runtime->slave.model, source);
  runtime_unlock(runtime);
  return updated;
}

void board_a_runtime_publish_sensor_snapshot(
    board_a_runtime_t *runtime,
    const board_a_sensor_snapshot_t *snapshot)
{
  if ((snapshot == NULL) || !runtime_lock(runtime)) {
    return;
  }
  board_a_model_publish_sensor_snapshot(&runtime->slave.model, snapshot);
  runtime_unlock(runtime);
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
  bool schedule_armed_before;
  uint64_t next_sample_before;
  uint64_t schedule_deadline_before;

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
  schedule_armed_before = runtime->slave.model.time.schedule_armed;
  schedule_deadline_before = runtime->slave.model.time.schedule_deadline_us;
  /*
   * Commands bind the monotonic instant of their own execution, so sample the
   * 64-bit clock once for this frame instead of reusing the 32-bit framing
   * timestamp.
   */
  if (runtime->ops->now_us != NULL) {
    runtime->slave.frame_now_us = runtime->ops->now_us(runtime->context);
  }
  response_length = board_a_slave_poll(&runtime->slave, now_us, response,
                                       response_capacity);
  if (scheduling_state_changed != NULL) {
    *scheduling_state_changed =
        (version_before != runtime->slave.model.active_config.version) ||
        (run_state_before != (uint16_t)runtime->slave.model.run_state) ||
        (start_pending_before != runtime->slave.model.start_pending) ||
        (next_sample_before != runtime->slave.model.next_sample_us) ||
        (schedule_armed_before != runtime->slave.model.time.schedule_armed) ||
        (schedule_deadline_before !=
         runtime->slave.model.time.schedule_deadline_us);
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
  status->data_source = runtime->slave.model.data_source;
  status->last_command = runtime->slave.model.last_command;
  status->command_result = runtime->slave.model.command_result;
  status->last_command_id = runtime->slave.model.last_command_id;
  status->records_this_run = runtime->slave.model.records_this_run;
  status->sequence = runtime->slave.model.snapshot.sequence;
  status->next_sample_us = runtime->slave.model.next_sample_us;
  status->start_pending = runtime->slave.model.start_pending;
  status->schedule_deadline_us = runtime->slave.model.time.schedule_deadline_us;
  status->schedule_armed = runtime->slave.model.time.schedule_armed;
  status->schedule_start_late_us =
      runtime->slave.model.time.schedule_start_late_us;
  status->schedule_start_count =
      runtime->slave.model.time.schedule_start_count;
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

bool board_a_runtime_claim_save(board_a_runtime_t *runtime,
                                board_a_save_request_t *request)
{
  bool claimed = false;

  if ((request == NULL) || !runtime_lock(runtime)) {
    return false;
  }
  claimed = board_a_model_claim_save(&runtime->slave.model, request);
  runtime_unlock(runtime);
  return claimed;
}

void board_a_runtime_complete_save(board_a_runtime_t *runtime, int success,
                                   board_a_save_error_t error,
                                   uint32_t raw_error)
{
  if (!runtime_lock(runtime)) {
    return;
  }
  board_a_model_complete_save(&runtime->slave.model, success, error,
                              raw_error);
  runtime_unlock(runtime);
}

bool board_a_runtime_pop_record(
    board_a_runtime_t *runtime,
    board_a_record_format_record_t *record)
{
  bool popped = false;

  if ((record == NULL) || !runtime_lock(runtime)) {
    return false;
  }
  popped = board_a_model_pop_record(&runtime->slave.model, record);
  runtime_unlock(runtime);
  return popped;
}

void board_a_runtime_requeue_record(
    board_a_runtime_t *runtime,
    const board_a_record_format_record_t *record)
{
  if ((record == NULL) || !runtime_lock(runtime)) {
    return;
  }
  board_a_model_requeue_record(&runtime->slave.model, record);
  runtime_unlock(runtime);
}

void board_a_runtime_complete_record(
    board_a_runtime_t *runtime,
    const board_a_record_format_record_t *record,
    board_a_record_complete_result_t result)
{
  if ((record == NULL) || !runtime_lock(runtime)) {
    return;
  }
  board_a_model_complete_record(&runtime->slave.model, record, result);
  runtime_unlock(runtime);
}

void board_a_runtime_set_storage_state(
    board_a_runtime_t *runtime, board_a_storage_state_t state,
    board_a_storage_error_t error, uint32_t raw_error)
{
  if (!runtime_lock(runtime)) {
    return;
  }
  board_a_model_set_storage_state(&runtime->slave.model, state, error,
                                  raw_error);
  runtime_unlock(runtime);
}

void board_a_runtime_note_storage_error(
    board_a_runtime_t *runtime, board_a_storage_error_t error,
    uint32_t raw_error)
{
  if (!runtime_lock(runtime)) {
    return;
  }
  board_a_model_note_storage_error(&runtime->slave.model, error, raw_error);
  runtime_unlock(runtime);
}

void board_a_runtime_complete_drain(board_a_runtime_t *runtime,
                                    uint32_t generation, int success)
{
  if (!runtime_lock(runtime)) {
    return;
  }
  board_a_model_complete_drain(&runtime->slave.model, generation, success);
  runtime_unlock(runtime);
}

bool board_a_runtime_persistence_status(
    board_a_runtime_t *runtime, board_a_persistence_status_t *status)
{
  if ((status == NULL) || !runtime_lock(runtime)) {
    return false;
  }
  board_a_persistence_status(&runtime->slave.model.persistence,
                             runtime->slave.model.active_config.version,
                             status);
  runtime_unlock(runtime);
  return true;
}

void board_a_runtime_apply_loaded_config(
    board_a_runtime_t *runtime, const board_a_persisted_config_t *config,
    uint32_t sequence)
{
  if ((config == NULL) || !runtime_lock(runtime)) {
    return;
  }
  board_a_model_apply_loaded_config(&runtime->slave.model, config, sequence);
  runtime_unlock(runtime);
}

void board_a_runtime_note_config_load(
    board_a_runtime_t *runtime, board_a_config_load_state_t state,
    uint32_t sequence)
{
  if (!runtime_lock(runtime)) {
    return;
  }
  board_a_model_note_config_load(&runtime->slave.model, state, sequence);
  runtime_unlock(runtime);
}
