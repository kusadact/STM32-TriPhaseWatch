#include "board_a_storage_engine.h"

#include <string.h>

static int deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
  return (int32_t)(now_ms - deadline_ms) >= 0;
}

static uint32_t earlier_deadline(uint32_t left, uint32_t right)
{
  return (deadline_reached(right, left)) ? left : right;
}

static board_a_storage_error_t map_open_error(
    board_a_storage_io_result_t result)
{
  switch (result) {
    case BOARD_A_STORAGE_IO_NOT_READY:
      return BOARD_A_STORAGE_ERROR_MOUNT;
    case BOARD_A_STORAGE_IO_FULL:
      return BOARD_A_STORAGE_ERROR_FULL;
    case BOARD_A_STORAGE_IO_TIMEOUT:
      return BOARD_A_STORAGE_ERROR_TIMEOUT;
    case BOARD_A_STORAGE_IO_EXISTS:
      return BOARD_A_STORAGE_ERROR_NAME_EXHAUSTED;
    default:
      return BOARD_A_STORAGE_ERROR_CREATE;
  }
}

static board_a_storage_error_t map_write_error(
    board_a_storage_io_result_t result)
{
  switch (result) {
    case BOARD_A_STORAGE_IO_FULL:
      return BOARD_A_STORAGE_ERROR_FULL;
    case BOARD_A_STORAGE_IO_TIMEOUT:
      return BOARD_A_STORAGE_ERROR_TIMEOUT;
    default:
      return BOARD_A_STORAGE_ERROR_WRITE;
  }
}

static void close_best_effort(board_a_storage_engine_t *engine,
                              const board_a_storage_io_ops_t *ops,
                              uint32_t deadline_ms)
{
  uint32_t raw_error = 0U;
  board_a_storage_io_result_t result;

  if ((engine == NULL) || (ops == NULL) || (engine->open_handle == NULL)) {
    return;
  }

  result = ops->close(ops->context, engine->open_handle, deadline_ms,
                      &raw_error);
  engine->open_handle = NULL;
  engine->open_date = 0U;
  engine->open_file_id = 0U;
  if ((result != BOARD_A_STORAGE_IO_OK) &&
      (engine->last_error == BOARD_A_STORAGE_ERROR_NONE)) {
    engine->last_error = BOARD_A_STORAGE_ERROR_CLOSE;
    engine->raw_error = raw_error;
  }
  if (result != BOARD_A_STORAGE_IO_OK) {
    engine->close_failed = 1U;
  }
}

static board_a_storage_io_result_t open_unique(
    board_a_storage_engine_t *engine, const board_a_storage_io_ops_t *ops,
    uint32_t file_date, uint32_t deadline_ms)
{
  uint8_t checked;

  for (checked = 0U; checked < BOARD_A_STORAGE_NAME_BATCH; checked++) {
    uint32_t raw_error = 0U;
    uint32_t file_id = engine->next_file_id;
    void *handle = NULL;
    board_a_storage_io_result_t result;

    if (deadline_reached(ops->now_ms(ops->context), deadline_ms)) {
      return BOARD_A_STORAGE_IO_TIMEOUT;
    }
    if (file_id == 0U) {
      file_id = 1U;
    }

    result = ops->open_new(ops->context, file_date, file_id, deadline_ms,
                           &handle, &raw_error);
    if (result == BOARD_A_STORAGE_IO_OK) {
      engine->open_handle = handle;
      engine->open_date = file_date;
      engine->open_file_id = file_id;
      engine->next_file_id = file_id + 1U;
      if (engine->next_file_id == 0U) {
        engine->next_file_id = 1U;
      }
      return BOARD_A_STORAGE_IO_OK;
    }
    if (result != BOARD_A_STORAGE_IO_EXISTS) {
      engine->raw_error = raw_error;
      return result;
    }

    engine->next_file_id = file_id + 1U;
    if (engine->next_file_id == 0U) {
      engine->next_file_id = 1U;
    }
  }

  if (ops->yield != NULL) {
    ops->yield(ops->context);
  }
  return BOARD_A_STORAGE_IO_EXISTS;
}

void board_a_storage_engine_init(board_a_storage_engine_t *engine)
{
  if (engine == NULL) {
    return;
  }
  memset(engine, 0, sizeof(*engine));
  engine->next_file_id = 1U;
  engine->last_error = BOARD_A_STORAGE_ERROR_NONE;
}

int board_a_storage_engine_probe(board_a_storage_engine_t *engine,
                                 const board_a_storage_io_ops_t *ops,
                                 uint32_t deadline_ms)
{
  board_a_storage_io_result_t result;
  uint32_t raw_error = 0U;

  if ((engine == NULL) || (ops == NULL)) {
    return 0;
  }
  if (ops->probe == NULL) {
    engine->last_error = BOARD_A_STORAGE_ERROR_NONE;
    engine->raw_error = 0U;
    return 1;
  }

  result = ops->probe(ops->context, deadline_ms, &raw_error);
  if (result != BOARD_A_STORAGE_IO_OK) {
    engine->last_error = map_open_error(result);
    engine->raw_error = raw_error;
    return 0;
  }
  engine->last_error = BOARD_A_STORAGE_ERROR_NONE;
  engine->raw_error = 0U;
  engine->close_failed = 0U;
  return 1;
}

board_a_storage_record_result_t board_a_storage_engine_process(
    board_a_storage_engine_t *engine, const board_a_storage_io_ops_t *ops,
    board_a_record_format_record_t *input_record,
    uint32_t operation_deadline_ms)
{
  board_a_record_format_record_t record;
  board_a_storage_io_result_t io_result;
  uint32_t now_ms;
  uint32_t write_deadline;
  uint16_t written = 0U;
  uint32_t raw_error = 0U;
  size_t line_length = 0U;
  int opened_new = 0;

  if ((engine == NULL) || (ops == NULL) || (input_record == NULL) ||
      (ops->open_new == NULL) || (ops->write == NULL) || (ops->sync == NULL) ||
      (ops->close == NULL) || (ops->now_ms == NULL)) {
    return BOARD_A_STORAGE_RECORD_UNWRITTEN;
  }

  record = *input_record;
  if (!board_a_record_format_is_valid(&record)) {
    engine->last_error = BOARD_A_STORAGE_ERROR_CREATE;
    engine->raw_error = 0U;
    return BOARD_A_STORAGE_RECORD_UNWRITTEN;
  }

  if ((record.utc_valid != 0U) &&
      !board_a_record_format_date_from_utc(record.utc_seconds,
                                           &record.file_date)) {
    engine->last_error = BOARD_A_STORAGE_ERROR_TIMEOUT;
    engine->raw_error = 0U;
    return BOARD_A_STORAGE_RECORD_UNWRITTEN;
  }
  if (record.utc_valid == 0U) {
    record.file_date = 0U;
  }

  now_ms = ops->now_ms(ops->context);
  if ((engine->open_handle != NULL) &&
      (engine->open_date != record.file_date) &&
      !board_a_storage_engine_drain(
          engine, ops,
          earlier_deadline(operation_deadline_ms,
                           now_ms + BOARD_A_STORAGE_OPERATION_TIMEOUT_MS))) {
    return BOARD_A_STORAGE_RECORD_UNWRITTEN;
  }

  if (engine->open_handle == NULL) {
    io_result = open_unique(
        engine, ops, record.file_date,
        earlier_deadline(operation_deadline_ms,
                         now_ms + BOARD_A_STORAGE_OPERATION_TIMEOUT_MS));
    if (io_result != BOARD_A_STORAGE_IO_OK) {
      engine->last_error = map_open_error(io_result);
      return BOARD_A_STORAGE_RECORD_UNWRITTEN;
    }
    opened_new = 1;
  }

  if (opened_new != 0) {
    const char *header = board_a_record_format_csv_header();
    uint16_t header_length = (uint16_t)strlen(header);

    now_ms = ops->now_ms(ops->context);
    write_deadline =
        earlier_deadline(operation_deadline_ms,
                         now_ms + BOARD_A_STORAGE_OPERATION_TIMEOUT_MS);
    io_result = ops->write(ops->context, engine->open_handle,
                           (const uint8_t *)header, header_length,
                           write_deadline, &written, &raw_error);
    if ((io_result != BOARD_A_STORAGE_IO_OK) ||
        (written != header_length)) {
      engine->last_error = map_write_error(io_result);
      engine->raw_error = raw_error;
      close_best_effort(engine, ops, write_deadline);
      return BOARD_A_STORAGE_RECORD_UNWRITTEN;
    }
  }

  record.file_id = engine->open_file_id;
  if (!board_a_record_format_encode_csv(
          &record, engine->line, sizeof(engine->line), &line_length)) {
    engine->last_error = BOARD_A_STORAGE_ERROR_CREATE;
    engine->raw_error = 0U;
    return BOARD_A_STORAGE_RECORD_UNWRITTEN;
  }

  now_ms = ops->now_ms(ops->context);
  write_deadline =
      earlier_deadline(operation_deadline_ms,
                       now_ms + BOARD_A_STORAGE_OPERATION_TIMEOUT_MS);
  io_result = ops->write(ops->context, engine->open_handle, engine->line,
                         (uint16_t)line_length, write_deadline, &written,
                         &raw_error);
  if ((io_result != BOARD_A_STORAGE_IO_OK) ||
      (written != (uint16_t)line_length)) {
    engine->last_error = map_write_error(io_result);
    engine->raw_error = raw_error;
    close_best_effort(engine, ops, write_deadline);
    return BOARD_A_STORAGE_RECORD_UNCERTAIN;
  }

  io_result = ops->sync(ops->context, engine->open_handle, write_deadline,
                        &raw_error);
  if (io_result != BOARD_A_STORAGE_IO_OK) {
    engine->last_error = (io_result == BOARD_A_STORAGE_IO_TIMEOUT) ?
        BOARD_A_STORAGE_ERROR_TIMEOUT : BOARD_A_STORAGE_ERROR_SYNC;
    engine->raw_error = raw_error;
    close_best_effort(engine, ops, write_deadline);
    return BOARD_A_STORAGE_RECORD_UNCERTAIN;
  }

  engine->last_error = BOARD_A_STORAGE_ERROR_NONE;
  engine->raw_error = 0U;
  *input_record = record;
  return BOARD_A_STORAGE_RECORD_SYNCED;
}

int board_a_storage_engine_drain(board_a_storage_engine_t *engine,
                                 const board_a_storage_io_ops_t *ops,
                                 uint32_t deadline_ms)
{
  board_a_storage_io_result_t result;
  uint32_t raw_error = 0U;

  if ((engine == NULL) || (ops == NULL) || (ops->close == NULL)) {
    return 0;
  }
  if (engine->open_handle == NULL) {
    if (engine->close_failed != 0U) {
      engine->last_error = BOARD_A_STORAGE_ERROR_CLOSE;
      return 0;
    }
    return 1;
  }

  result = ops->close(ops->context, engine->open_handle, deadline_ms,
                      &raw_error);
  engine->open_handle = NULL;
  engine->open_date = 0U;
  engine->open_file_id = 0U;
  if (result != BOARD_A_STORAGE_IO_OK) {
    engine->last_error = (result == BOARD_A_STORAGE_IO_TIMEOUT) ?
        BOARD_A_STORAGE_ERROR_TIMEOUT : BOARD_A_STORAGE_ERROR_CLOSE;
    engine->raw_error = raw_error;
    engine->close_failed = 1U;
    return 0;
  }

  engine->last_error = BOARD_A_STORAGE_ERROR_NONE;
  engine->raw_error = 0U;
  engine->close_failed = 0U;
  return 1;
}
