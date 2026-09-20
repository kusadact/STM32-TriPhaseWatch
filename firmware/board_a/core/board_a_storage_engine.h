#ifndef BOARD_A_STORAGE_ENGINE_H
#define BOARD_A_STORAGE_ENGINE_H

#include <stdint.h>

#include "board_a_persistence.h"

#define BOARD_A_STORAGE_NAME_BATCH 32U

typedef enum {
  BOARD_A_STORAGE_IO_OK = 0,
  BOARD_A_STORAGE_IO_EXISTS,
  BOARD_A_STORAGE_IO_NOT_READY,
  BOARD_A_STORAGE_IO_FULL,
  BOARD_A_STORAGE_IO_WRITE,
  BOARD_A_STORAGE_IO_SYNC,
  BOARD_A_STORAGE_IO_CLOSE,
  BOARD_A_STORAGE_IO_TIMEOUT,
  BOARD_A_STORAGE_IO_OTHER
} board_a_storage_io_result_t;

typedef struct {
  void *context;
  board_a_storage_io_result_t (*probe)(
      void *context, uint32_t deadline_ms, uint32_t *raw_error);
  board_a_storage_io_result_t (*open_new)(
      void *context, uint32_t file_date, uint32_t file_id,
      uint32_t deadline_ms, void **handle, uint32_t *raw_error);
  board_a_storage_io_result_t (*write)(
      void *context, void *handle, const uint8_t *data, uint16_t length,
      uint32_t deadline_ms, uint16_t *written, uint32_t *raw_error);
  board_a_storage_io_result_t (*sync)(
      void *context, void *handle, uint32_t deadline_ms,
      uint32_t *raw_error);
  board_a_storage_io_result_t (*close)(
      void *context, void *handle, uint32_t deadline_ms,
      uint32_t *raw_error);
  uint32_t (*now_ms)(void *context);
  void (*yield)(void *context);
} board_a_storage_io_ops_t;

typedef enum {
  BOARD_A_STORAGE_RECORD_SYNCED = 0,
  BOARD_A_STORAGE_RECORD_UNWRITTEN,
  BOARD_A_STORAGE_RECORD_UNCERTAIN
} board_a_storage_record_result_t;

typedef struct {
  void *open_handle;
  uint32_t open_date;
  uint32_t open_file_id;
  uint32_t next_file_id;
  board_a_storage_error_t last_error;
  uint32_t raw_error;
  uint8_t close_failed;
  uint8_t line[BOARD_A_RECORD_CSV_MAX_BYTES];
} board_a_storage_engine_t;

void board_a_storage_engine_init(board_a_storage_engine_t *engine);

int board_a_storage_engine_probe(board_a_storage_engine_t *engine,
                                 const board_a_storage_io_ops_t *ops,
                                 uint32_t deadline_ms);

/*
 * On a SYNCED result, record is updated in place with the actual file_date
 * and file_id used for the successful write.
 */
board_a_storage_record_result_t board_a_storage_engine_process(
    board_a_storage_engine_t *engine,
    const board_a_storage_io_ops_t *ops,
    board_a_record_format_record_t *record,
    uint32_t operation_deadline_ms);

int board_a_storage_engine_drain(board_a_storage_engine_t *engine,
                                 const board_a_storage_io_ops_t *ops,
                                 uint32_t deadline_ms);

#endif /* BOARD_A_STORAGE_ENGINE_H */
