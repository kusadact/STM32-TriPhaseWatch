#include "board_a_persistence.h"

#include <string.h>

#include "board_a_model.h"

void board_a_persistence_init(board_a_persistence_t *persistence)
{
  if (persistence == NULL) {
    return;
  }

  memset(persistence, 0, sizeof(*persistence));
  persistence->save.state = BOARD_A_SAVE_IDLE;
  persistence->save.error = BOARD_A_SAVE_ERROR_NONE;
  persistence->config_load_state = BOARD_A_CONFIG_LOAD_UNFINISHED;
  persistence->storage_state = BOARD_A_STORAGE_INITIALIZING;
  persistence->storage_error = BOARD_A_STORAGE_ERROR_NONE;
  persistence->storage.drain_state = BOARD_A_DRAIN_NONE;
}

board_a_save_accept_result_t board_a_persistence_accept_save(
    board_a_persistence_t *persistence,
    const board_a_persisted_config_t *config, uint32_t config_version,
    uint32_t command_id)
{
  if ((persistence == NULL) || (config == NULL)) {
    return BOARD_A_SAVE_ACCEPT_INVALID;
  }
  if ((config->period_sec < BOARD_A_PERIOD_MIN_SEC) ||
      (config->period_sec > BOARD_A_PERIOD_MAX_SEC) ||
      (config->channel_mask < BOARD_A_CHANNEL_MASK_MIN) ||
      (config->channel_mask > BOARD_A_CHANNEL_MASK_MAX)) {
    return BOARD_A_SAVE_ACCEPT_INVALID;
  }
  if (persistence->save.state == BOARD_A_SAVE_PENDING) {
    return BOARD_A_SAVE_ACCEPT_BUSY;
  }

  persistence->save.request.config = *config;
  persistence->save.request.config_version = config_version;
  persistence->save.request.command_id = command_id;
  persistence->save.state = BOARD_A_SAVE_PENDING;
  persistence->save.error = BOARD_A_SAVE_ERROR_NONE;
  persistence->save.raw_error = 0U;
  persistence->save.occupied = 1U;
  persistence->save.claimed = 0U;
  return BOARD_A_SAVE_ACCEPT_OK;
}

int board_a_persistence_claim_save(board_a_persistence_t *persistence,
                                   board_a_save_request_t *request)
{
  if ((persistence == NULL) || (request == NULL) ||
      (persistence->save.occupied == 0U) ||
      (persistence->save.claimed != 0U) ||
      (persistence->save.state != BOARD_A_SAVE_PENDING)) {
    return 0;
  }

  *request = persistence->save.request;
  persistence->save.claimed = 1U;
  return 1;
}

void board_a_persistence_complete_save(
    board_a_persistence_t *persistence, int success,
    board_a_save_error_t error, uint32_t raw_error)
{
  if (persistence == NULL) {
    return;
  }

  persistence->save.state =
      (success != 0) ? BOARD_A_SAVE_SUCCESS : BOARD_A_SAVE_FAILED;
  persistence->save.error =
      (success != 0) ? BOARD_A_SAVE_ERROR_NONE : error;
  persistence->save.raw_error = (success != 0) ? 0U : raw_error;
  persistence->save.claimed = 0U;
}

void board_a_persistence_note_load(
    board_a_persistence_t *persistence, board_a_config_load_state_t state,
    uint32_t sequence)
{
  if (persistence == NULL) {
    return;
  }
  persistence->config_load_state = state;
  persistence->load_sequence =
      (state == BOARD_A_CONFIG_LOAD_SUCCESS) ? sequence : 0U;
}

int board_a_persistence_queue_push(
    board_a_persistence_t *persistence,
    const board_a_record_format_record_t *record)
{
  uint16_t tail;

  if ((persistence == NULL) || (record == NULL) ||
      !board_a_record_format_is_valid(record)) {
    return 0;
  }

  persistence->storage.generated++;
  if (persistence->storage.count >= BOARD_A_RECORD_QUEUE_CAPACITY) {
    persistence->storage.dropped++;
    return 0;
  }

  tail = (uint16_t)((persistence->storage.head +
                     persistence->storage.count) %
                    BOARD_A_RECORD_QUEUE_CAPACITY);
  persistence->storage.records[tail] = *record;
  persistence->storage.count++;
  if (persistence->storage.count > persistence->storage.high_water) {
    persistence->storage.high_water = persistence->storage.count;
  }
  return 1;
}

int board_a_persistence_queue_pop(
    board_a_persistence_t *persistence,
    board_a_record_format_record_t *record)
{
  board_a_storage_accounting_t *storage;

  if ((persistence == NULL) || (record == NULL)) {
    return 0;
  }
  storage = &persistence->storage;
  if ((storage->count == 0U) || (storage->in_flight != 0U)) {
    return 0;
  }

  *record = storage->records[storage->head];
  storage->head =
      (uint16_t)((storage->head + 1U) % BOARD_A_RECORD_QUEUE_CAPACITY);
  storage->count--;
  storage->in_flight = 1U;
  return 1;
}

void board_a_persistence_queue_requeue(
    board_a_persistence_t *persistence,
    const board_a_record_format_record_t *record)
{
  board_a_storage_accounting_t *storage;

  if ((persistence == NULL) || (record == NULL)) {
    return;
  }
  storage = &persistence->storage;
  if ((storage->in_flight == 0U) ||
      (storage->count >= BOARD_A_RECORD_QUEUE_CAPACITY)) {
    return;
  }

  storage->head =
      (uint16_t)((storage->head + BOARD_A_RECORD_QUEUE_CAPACITY - 1U) %
                 BOARD_A_RECORD_QUEUE_CAPACITY);
  storage->records[storage->head] = *record;
  storage->count++;
  if (storage->count > storage->high_water) {
    storage->high_water = storage->count;
  }
  storage->in_flight = 0U;
}

void board_a_persistence_complete_record(
    board_a_persistence_t *persistence,
    const board_a_record_format_record_t *record,
    board_a_record_complete_result_t result)
{
  board_a_storage_accounting_t *storage;

  if ((persistence == NULL) || (record == NULL)) {
    return;
  }
  storage = &persistence->storage;
  if (storage->in_flight == 0U) {
    return;
  }

  storage->in_flight = 0U;
  if (result == BOARD_A_RECORD_COMPLETE_SYNCED) {
    storage->synced++;
    storage->last_synced_valid = 1U;
    storage->last_synced_seq = record->sequence;
    storage->last_synced_file = record->file_id;
    storage->last_synced_date = record->file_date;
  } else {
    storage->uncertain++;
  }
}

void board_a_persistence_request_drain(board_a_persistence_t *persistence)
{
  if (persistence == NULL) {
    return;
  }
  persistence->storage.drain_generation++;
  persistence->storage.drain_state = BOARD_A_DRAIN_PENDING;
}

void board_a_persistence_complete_drain(
    board_a_persistence_t *persistence, uint32_t generation, int success)
{
  if ((persistence == NULL) ||
      (persistence->storage.drain_state != BOARD_A_DRAIN_PENDING) ||
      (generation != persistence->storage.drain_generation)) {
    return;
  }
  persistence->storage.drain_state =
      (success != 0) ? BOARD_A_DRAIN_DONE : BOARD_A_DRAIN_FAILED;
}

void board_a_persistence_set_storage_state(
    board_a_persistence_t *persistence, board_a_storage_state_t state,
    board_a_storage_error_t error, uint32_t raw_error)
{
  if (persistence == NULL) {
    return;
  }
  persistence->storage_state = state;
  persistence->storage_error = error;
  persistence->storage_raw_error = raw_error;
}

void board_a_persistence_note_storage_error(
    board_a_persistence_t *persistence, board_a_storage_error_t error,
    uint32_t raw_error)
{
  if (persistence == NULL) {
    return;
  }
  persistence->storage.storage_errors++;
  persistence->storage_error = error;
  persistence->storage_raw_error = raw_error;
}

void board_a_persistence_status(
    const board_a_persistence_t *persistence, uint32_t active_config_version,
    board_a_persistence_status_t *status)
{
  const board_a_storage_accounting_t *storage;

  if ((persistence == NULL) || (status == NULL)) {
    return;
  }
  storage = &persistence->storage;
  memset(status, 0, sizeof(*status));
  status->save_state = (uint16_t)persistence->save.state;
  status->save_command_id =
      (persistence->save.state == BOARD_A_SAVE_IDLE) ?
      0U : persistence->save.request.command_id;
  status->save_config_version =
      (persistence->save.state == BOARD_A_SAVE_IDLE) ?
      0U : persistence->save.request.config_version;
  status->save_error = (uint16_t)persistence->save.error;
  status->config_load_state = (uint16_t)persistence->config_load_state;
  status->storage_state = (uint16_t)persistence->storage_state;
  status->storage_error = (uint16_t)persistence->storage_error;
  status->queued = storage->count;
  status->queue_high_water = storage->high_water;
  status->generated = storage->generated;
  status->synced = storage->synced;
  status->dropped = storage->dropped;
  status->uncertain = storage->uncertain;
  status->in_flight = storage->in_flight;
  status->drain_state = (uint16_t)storage->drain_state;
  status->last_synced_seq =
      storage->last_synced_valid ? storage->last_synced_seq : 0U;
  status->last_synced_file =
      storage->last_synced_valid ? storage->last_synced_file : 0U;
  status->last_synced_date =
      storage->last_synced_valid ? storage->last_synced_date : 0U;
  status->active_config_version = active_config_version;
  status->drain_generation = storage->drain_generation;
  status->storage_errors = storage->storage_errors;
  status->load_sequence = persistence->load_sequence;
  if (persistence->save.state != BOARD_A_SAVE_IDLE) {
    status->captured_period = persistence->save.request.config.period_sec;
    status->captured_mask = persistence->save.request.config.channel_mask;
    status->captured_count = persistence->save.request.config.record_count;
  }
}

int board_a_persistence_invariant_holds(
    const board_a_persistence_t *persistence)
{
  uint32_t sum;

  if (persistence == NULL) {
    return 0;
  }
  sum = persistence->storage.synced + persistence->storage.dropped +
        persistence->storage.uncertain + persistence->storage.count +
        persistence->storage.in_flight;
  return sum == persistence->storage.generated;
}
