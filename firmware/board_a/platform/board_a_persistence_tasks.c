#include "board_a_persistence_tasks.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "board_a_monotonic.h"
#include "board_a_storage_engine.h"
#include "board_a_storage_port.h"
#include "board_a_rtos.h"
#include "config_store.h"
#include "eeprom_config.h"
#include "task.h"

static config_store_t g_config_store;
static board_a_storage_engine_t g_storage_engine;
static uint32_t g_storage_retry_after_ms;
static uint8_t g_config_ready;

static config_store_status_t board_a_config_store_validate(
    void *context, const uint8_t *payload, uint8_t payload_length)
{
  (void)context;
  return board_a_config_payload_validate(payload, payload_length) ?
      CONFIG_STORE_OK : CONFIG_STORE_INVALID_RECORD;
}

static board_a_save_error_t map_save_error(config_store_status_t status)
{
  switch (status) {
    case CONFIG_STORE_OK:
      return BOARD_A_SAVE_ERROR_NONE;
    case CONFIG_STORE_NO_VALID_RECORD:
      return BOARD_A_SAVE_ERROR_NO_VALID_RECORD;
    case CONFIG_STORE_INVALID_ARGUMENT:
    case CONFIG_STORE_INVALID_RECORD:
      return BOARD_A_SAVE_ERROR_INVALID_DATA;
    case CONFIG_STORE_IO_ERROR:
      return BOARD_A_SAVE_ERROR_IO;
    case CONFIG_STORE_CONFLICT:
      return BOARD_A_SAVE_ERROR_CONFLICT;
    case CONFIG_STORE_VERIFY_FAILED:
      return BOARD_A_SAVE_ERROR_VERIFY;
    case CONFIG_STORE_NOT_READY:
      return BOARD_A_SAVE_ERROR_NOT_READY;
    case CONFIG_STORE_BUSY:
      return BOARD_A_SAVE_ERROR_BUSY;
    default:
      return BOARD_A_SAVE_ERROR_OTHER;
  }
}

static uint32_t persistence_now_ms(void)
{
  return board_a_rtos_now_ms();
}

static int deadline_expired(uint32_t now_ms, uint32_t deadline_ms)
{
  return board_a_deadline_expired(now_ms, deadline_ms);
}

static uint32_t min_deadline(uint32_t left, uint32_t right)
{
  return deadline_expired(right, left) ? left : right;
}

void board_a_persistence_startup(board_a_runtime_t *runtime)
{
  config_store_status_t status;
  config_store_metadata_t metadata;
  uint8_t payload[CONFIG_STORE_PAYLOAD_MAX_BYTES];
  board_a_persisted_config_t config;
  uint32_t started_ms;
  uint32_t finished_ms;

  if (runtime == NULL) {
    return;
  }

  board_a_storage_port_reset();
  board_a_storage_engine_init(&g_storage_engine);
  g_storage_retry_after_ms = 0U;
  g_config_ready = 0U;
  memset(&metadata, 0, sizeof(metadata));
  board_a_runtime_set_storage_state(runtime, BOARD_A_STORAGE_INITIALIZING,
                                    BOARD_A_STORAGE_ERROR_NONE, 0U);

  started_ms = persistence_now_ms();
  status = eeprom_config_store_init_validated(
      &g_config_store, board_a_config_store_validate, NULL);
  if (status == CONFIG_STORE_OK) {
    g_config_ready = 1U;
    status = config_store_load(&g_config_store, payload, sizeof(payload),
                               &metadata);
  }
  finished_ms = persistence_now_ms();

  if ((status == CONFIG_STORE_OK) &&
      deadline_expired(finished_ms,
                       started_ms + BOARD_A_STORAGE_OPERATION_TIMEOUT_MS)) {
    status = CONFIG_STORE_IO_ERROR;
  }

  if (status == CONFIG_STORE_OK) {
    if (board_a_config_payload_decode(payload, metadata.payload_length,
                                      &config)) {
      board_a_runtime_apply_loaded_config(runtime, &config, metadata.sequence);
    } else {
      board_a_runtime_note_config_load(runtime,
                                       BOARD_A_CONFIG_LOAD_DEFAULT_ERROR, 0U);
    }
  } else if (status == CONFIG_STORE_NO_VALID_RECORD) {
    board_a_runtime_note_config_load(
        runtime,
        (metadata.semantic_error_mask != 0U) ?
            BOARD_A_CONFIG_LOAD_DEFAULT_ERROR :
            BOARD_A_CONFIG_LOAD_DEFAULT_NO_RECORD,
        0U);
  } else {
    board_a_runtime_note_config_load(
        runtime, BOARD_A_CONFIG_LOAD_DEFAULT_ERROR, 0U);
  }
}

static void config_task_once(board_a_runtime_t *runtime)
{
  board_a_save_request_t request;
  uint8_t payload[BOARD_A_CONFIG_PAYLOAD_SIZE];
  config_store_metadata_t metadata;
  config_store_status_t status;
  board_a_save_error_t error;
  uint32_t started_ms;
  uint32_t finished_ms;
  uint32_t raw_error = 0U;
  int success;

  if (!board_a_runtime_claim_save(runtime, &request)) {
    return;
  }
  if (g_config_ready == 0U) {
    board_a_runtime_complete_save(runtime, 0,
                                  BOARD_A_SAVE_ERROR_NOT_READY, 0U);
    return;
  }

  if (!board_a_config_payload_encode(&request.config, payload,
                                     sizeof(payload))) {
    board_a_runtime_complete_save(runtime, 0,
                                  BOARD_A_SAVE_ERROR_INVALID_DATA, 0U);
    return;
  }

  memset(&metadata, 0, sizeof(metadata));
  started_ms = persistence_now_ms();
  status = config_store_save(&g_config_store, payload, sizeof(payload),
                             &metadata);
  finished_ms = persistence_now_ms();

  if ((status == CONFIG_STORE_OK) &&
      deadline_expired(finished_ms,
                       started_ms + BOARD_A_STORAGE_OPERATION_TIMEOUT_MS)) {
    error = BOARD_A_SAVE_ERROR_TIMEOUT;
    success = 0;
  } else {
    error = map_save_error(status);
    success = (status == CONFIG_STORE_OK) ? 1 : 0;
  }
  if ((success == 0) &&
      ((error == BOARD_A_SAVE_ERROR_IO) ||
       (error == BOARD_A_SAVE_ERROR_NOT_READY) ||
       (error == BOARD_A_SAVE_ERROR_BUSY) ||
       (error == BOARD_A_SAVE_ERROR_VERIFY))) {
    raw_error = (uint32_t)eeprom_config_last_result();
  }

  board_a_runtime_complete_save(
      runtime, success, error, raw_error);
}

void board_a_config_task(void *argument)
{
  board_a_runtime_t *runtime = (board_a_runtime_t *)argument;
  uint32_t notification_value;

  for (;;) {
    (void)xTaskNotifyWait(0U, 0xFFFFFFFFUL, &notification_value,
                          pdMS_TO_TICKS(1000));
    (void)notification_value;
    board_a_rtos_note_config_stack(
        (uint32_t)uxTaskGetStackHighWaterMark(NULL));
    config_task_once(runtime);
  }
}

static board_a_storage_state_t state_for_error(
    board_a_storage_error_t error)
{
  switch (error) {
    case BOARD_A_STORAGE_ERROR_MOUNT:
    case BOARD_A_STORAGE_ERROR_INIT:
      return BOARD_A_STORAGE_UNAVAILABLE;
    case BOARD_A_STORAGE_ERROR_FULL:
      return BOARD_A_STORAGE_FULL;
    case BOARD_A_STORAGE_ERROR_TIMEOUT:
      return BOARD_A_STORAGE_IO_ERROR;
    default:
      return BOARD_A_STORAGE_IO_ERROR;
  }
}

static int storage_probe(board_a_runtime_t *runtime, uint32_t now_ms)
{
  const board_a_storage_io_ops_t *ops = board_a_storage_port_ops();
  board_a_storage_error_t error;
  int ready = board_a_storage_engine_probe(
      &g_storage_engine, ops,
      now_ms + BOARD_A_STORAGE_OPERATION_TIMEOUT_MS);

  if (ready) {
    board_a_runtime_set_storage_state(runtime, BOARD_A_STORAGE_READY,
                                      BOARD_A_STORAGE_ERROR_NONE, 0U);
    return 1;
  }

  error = g_storage_engine.last_error;
  board_a_runtime_note_storage_error(runtime, error,
                                     g_storage_engine.raw_error);
  board_a_runtime_set_storage_state(runtime, state_for_error(error), error,
                                    g_storage_engine.raw_error);
  return 0;
}

/*
 * Processes one queued record. Returns true when a record was attempted.
 * The caller has already excluded drain mode.
 */
static int storage_process_one(board_a_runtime_t *runtime, uint32_t now_ms)
{
  const board_a_storage_io_ops_t *ops = board_a_storage_port_ops();
  board_a_record_format_record_t record;
  board_a_storage_record_result_t result;
  board_a_storage_error_t error;
  uint32_t deadline_ms =
      now_ms + BOARD_A_STORAGE_OPERATION_TIMEOUT_MS;

  if (!board_a_runtime_pop_record(runtime, &record)) {
    return 0;
  }

  result = board_a_storage_engine_process(&g_storage_engine, ops, &record,
                                          deadline_ms);
  error = g_storage_engine.last_error;
  if (result == BOARD_A_STORAGE_RECORD_SYNCED) {
    board_a_runtime_status_t runtime_status;

    board_a_runtime_complete_record(runtime, &record,
                                    BOARD_A_RECORD_COMPLETE_SYNCED);
    if ((record.trigger == BOARD_A_SAMPLE_TRIGGER_SINGLE) &&
        board_a_runtime_copy_status(runtime, &runtime_status) &&
        (runtime_status.run_state == BOARD_A_RUN_STOPPED)) {
      if (board_a_storage_engine_drain(
              &g_storage_engine, ops,
              persistence_now_ms() +
                  BOARD_A_STORAGE_OPERATION_TIMEOUT_MS)) {
        board_a_runtime_set_storage_state(runtime, BOARD_A_STORAGE_CLOSED,
                                          BOARD_A_STORAGE_ERROR_NONE, 0U);
      } else {
        error = g_storage_engine.last_error;
        board_a_runtime_note_storage_error(runtime, error,
                                           g_storage_engine.raw_error);
        board_a_runtime_set_storage_state(runtime, state_for_error(error),
                                          error,
                                          g_storage_engine.raw_error);
      }
    } else {
      board_a_runtime_set_storage_state(runtime, BOARD_A_STORAGE_READY,
                                        BOARD_A_STORAGE_ERROR_NONE, 0U);
    }
  } else if (result == BOARD_A_STORAGE_RECORD_UNCERTAIN) {
    board_a_runtime_complete_record(runtime, &record,
                                    BOARD_A_RECORD_COMPLETE_UNCERTAIN);
    board_a_runtime_note_storage_error(runtime, error,
                                       g_storage_engine.raw_error);
    board_a_runtime_set_storage_state(runtime, state_for_error(error), error,
                                      g_storage_engine.raw_error);
  } else {
    board_a_runtime_requeue_record(runtime, &record);
    board_a_runtime_note_storage_error(runtime, error,
                                       g_storage_engine.raw_error);
    board_a_runtime_set_storage_state(runtime, state_for_error(error), error,
                                      g_storage_engine.raw_error);
  }

  return 1;
}

static int storage_run_drain(board_a_runtime_t *runtime,
                             const board_a_persistence_status_t *status,
                             uint32_t *now_ms)
{
  uint32_t deadline_ms =
      *now_ms + BOARD_A_STORAGE_DRAIN_TIMEOUT_MS;
  const board_a_storage_io_ops_t *ops = board_a_storage_port_ops();

  board_a_runtime_set_storage_state(runtime, BOARD_A_STORAGE_DRAINING,
                                    BOARD_A_STORAGE_ERROR_NONE, 0U);

  for (;;) {
    board_a_persistence_status_t current;
    board_a_record_format_record_t record;
    board_a_storage_record_result_t result;
    board_a_storage_error_t error;
    uint32_t operation_deadline;

    *now_ms = persistence_now_ms();
    if (!board_a_runtime_persistence_status(runtime, &current) ||
        (current.drain_generation != status->drain_generation) ||
        (current.drain_state != BOARD_A_DRAIN_PENDING)) {
      return 0;
    }
    if ((current.queued == 0U) && (current.in_flight == 0U)) {
      int drained = board_a_storage_engine_drain(
          &g_storage_engine, ops, deadline_ms);
      board_a_runtime_complete_drain(runtime, status->drain_generation,
                                     drained);
      if (drained) {
        board_a_runtime_set_storage_state(runtime, BOARD_A_STORAGE_CLOSED,
                                          BOARD_A_STORAGE_ERROR_NONE, 0U);
      } else {
        error = g_storage_engine.last_error;
        board_a_runtime_note_storage_error(runtime, error,
                                           g_storage_engine.raw_error);
        board_a_runtime_set_storage_state(runtime, state_for_error(error),
                                          error,
                                          g_storage_engine.raw_error);
      }
      return 1;
    }
    if (deadline_expired(*now_ms, deadline_ms) ||
        !board_a_runtime_pop_record(runtime, &record)) {
      board_a_runtime_complete_drain(runtime, status->drain_generation, 0);
      board_a_runtime_note_storage_error(runtime,
                                         BOARD_A_STORAGE_ERROR_TIMEOUT, 0U);
      board_a_runtime_set_storage_state(runtime, BOARD_A_STORAGE_IO_ERROR,
                                        BOARD_A_STORAGE_ERROR_TIMEOUT, 0U);
      return 1;
    }

    operation_deadline =
        min_deadline(
            deadline_ms,
            *now_ms + BOARD_A_STORAGE_OPERATION_TIMEOUT_MS);
    result = board_a_storage_engine_process(
        &g_storage_engine, ops, &record, operation_deadline);
    error = g_storage_engine.last_error;
    if (result == BOARD_A_STORAGE_RECORD_SYNCED) {
      board_a_runtime_complete_record(runtime, &record,
                                      BOARD_A_RECORD_COMPLETE_SYNCED);
      continue;
    }
    if (result == BOARD_A_STORAGE_RECORD_UNCERTAIN) {
      board_a_runtime_complete_record(runtime, &record,
                                      BOARD_A_RECORD_COMPLETE_UNCERTAIN);
    } else {
      board_a_runtime_requeue_record(runtime, &record);
    }
    board_a_runtime_complete_drain(runtime, status->drain_generation, 0);
    board_a_runtime_note_storage_error(runtime, error,
                                       g_storage_engine.raw_error);
    board_a_runtime_set_storage_state(runtime, state_for_error(error), error,
                                      g_storage_engine.raw_error);
    return 1;
  }
}

void board_a_storage_task(void *argument)
{
  board_a_runtime_t *runtime = (board_a_runtime_t *)argument;
  uint32_t notification_value;

  for (;;) {
    board_a_persistence_status_t status;
    uint32_t now_ms = persistence_now_ms();
    uint32_t wait_ms = BOARD_A_STORAGE_RETRY_PERIOD_MS;
    int did_work = 0;

    board_a_rtos_note_storage_stack(
        (uint32_t)uxTaskGetStackHighWaterMark(NULL));

    if (!board_a_runtime_persistence_status(runtime, &status)) {
      (void)xTaskNotifyWait(0U, 0xFFFFFFFFUL, &notification_value,
                            pdMS_TO_TICKS(1000));
      continue;
    }

    if (status.drain_state == BOARD_A_DRAIN_PENDING) {
      did_work = storage_run_drain(runtime, &status, &now_ms);
    } else if ((status.storage_state == BOARD_A_STORAGE_INITIALIZING) ||
               (((status.storage_state == BOARD_A_STORAGE_UNAVAILABLE) ||
                (status.storage_state == BOARD_A_STORAGE_IO_ERROR)) &&
                board_a_deadline_expired(now_ms, g_storage_retry_after_ms))) {
      if (!storage_probe(runtime, now_ms)) {
        g_storage_retry_after_ms =
            now_ms + BOARD_A_STORAGE_RETRY_PERIOD_MS;
      }
      did_work = 1;
    } else if ((status.queued != 0U) &&
               board_a_deadline_expired(now_ms, g_storage_retry_after_ms)) {
      did_work = storage_process_one(runtime, now_ms);
      if (did_work) {
        board_a_persistence_status_t after;
        if (board_a_runtime_persistence_status(runtime, &after) &&
            ((after.storage_state == BOARD_A_STORAGE_UNAVAILABLE) ||
             (after.storage_state == BOARD_A_STORAGE_FULL) ||
             (after.storage_state == BOARD_A_STORAGE_IO_ERROR))) {
          g_storage_retry_after_ms =
              now_ms + BOARD_A_STORAGE_RETRY_PERIOD_MS;
        }
      }
    }

    if (did_work == 0) {
      if ((status.drain_state != BOARD_A_DRAIN_PENDING) &&
          ((status.queued != 0U) ||
           (status.storage_state == BOARD_A_STORAGE_UNAVAILABLE) ||
           (status.storage_state == BOARD_A_STORAGE_IO_ERROR)) &&
          ((int32_t)(now_ms - g_storage_retry_after_ms) < 0)) {
        wait_ms = g_storage_retry_after_ms - now_ms;
        if (wait_ms > BOARD_A_STORAGE_RETRY_PERIOD_MS) {
          wait_ms = BOARD_A_STORAGE_RETRY_PERIOD_MS;
        }
      }
      (void)xTaskNotifyWait(0U, 0xFFFFFFFFUL, &notification_value,
                            pdMS_TO_TICKS(wait_ms));
    } else {
      taskYIELD();
    }
  }
}
