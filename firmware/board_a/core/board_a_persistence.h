#ifndef BOARD_A_PERSISTENCE_H
#define BOARD_A_PERSISTENCE_H

#include <stdint.h>

#include "board_a_record_format.h"

#define BOARD_A_PERSISTENCE_CONTRACT_REVISION 1U
#define BOARD_A_RECORD_QUEUE_CAPACITY 32U
#define BOARD_A_STORAGE_OPERATION_TIMEOUT_MS 2000U
#define BOARD_A_STORAGE_RETRY_PERIOD_MS 5000U
#define BOARD_A_STORAGE_DRAIN_TIMEOUT_MS 5000U

typedef enum {
  BOARD_A_SAVE_IDLE = 0,
  BOARD_A_SAVE_PENDING = 1,
  BOARD_A_SAVE_SUCCESS = 2,
  BOARD_A_SAVE_FAILED = 3
} board_a_save_state_t;

typedef enum {
  BOARD_A_SAVE_ERROR_NONE = 0,
  BOARD_A_SAVE_ERROR_NO_VALID_RECORD = 1,
  BOARD_A_SAVE_ERROR_INVALID_DATA = 2,
  BOARD_A_SAVE_ERROR_IO = 3,
  BOARD_A_SAVE_ERROR_CONFLICT = 4,
  BOARD_A_SAVE_ERROR_VERIFY = 5,
  BOARD_A_SAVE_ERROR_NOT_READY = 6,
  BOARD_A_SAVE_ERROR_BUSY = 7,
  BOARD_A_SAVE_ERROR_TIMEOUT = 8,
  BOARD_A_SAVE_ERROR_OTHER = 9
} board_a_save_error_t;

typedef enum {
  BOARD_A_CONFIG_LOAD_UNFINISHED = 0,
  BOARD_A_CONFIG_LOAD_SUCCESS = 1,
  BOARD_A_CONFIG_LOAD_DEFAULT_NO_RECORD = 2,
  BOARD_A_CONFIG_LOAD_DEFAULT_ERROR = 3
} board_a_config_load_state_t;

typedef enum {
  BOARD_A_STORAGE_INITIALIZING = 0,
  BOARD_A_STORAGE_READY = 1,
  BOARD_A_STORAGE_UNAVAILABLE = 2,
  BOARD_A_STORAGE_FULL = 3,
  BOARD_A_STORAGE_IO_ERROR = 4,
  BOARD_A_STORAGE_DRAINING = 5,
  BOARD_A_STORAGE_CLOSED = 6
} board_a_storage_state_t;

typedef enum {
  BOARD_A_STORAGE_ERROR_NONE = 0,
  BOARD_A_STORAGE_ERROR_INIT = 1,
  BOARD_A_STORAGE_ERROR_MOUNT = 2,
  BOARD_A_STORAGE_ERROR_CREATE = 3,
  BOARD_A_STORAGE_ERROR_FULL = 4,
  BOARD_A_STORAGE_ERROR_WRITE = 5,
  BOARD_A_STORAGE_ERROR_SYNC = 6,
  BOARD_A_STORAGE_ERROR_CLOSE = 7,
  BOARD_A_STORAGE_ERROR_TIMEOUT = 8,
  BOARD_A_STORAGE_ERROR_NAME_EXHAUSTED = 9
} board_a_storage_error_t;

typedef enum {
  BOARD_A_DRAIN_NONE = 0,
  BOARD_A_DRAIN_PENDING = 1,
  BOARD_A_DRAIN_DONE = 2,
  BOARD_A_DRAIN_FAILED = 3
} board_a_drain_state_t;

typedef enum {
  BOARD_A_SAVE_ACCEPT_OK = 0,
  BOARD_A_SAVE_ACCEPT_BUSY,
  BOARD_A_SAVE_ACCEPT_INVALID
} board_a_save_accept_result_t;

typedef enum {
  BOARD_A_RECORD_COMPLETE_SYNCED = 1,
  BOARD_A_RECORD_COMPLETE_UNCERTAIN
} board_a_record_complete_result_t;

typedef struct {
  board_a_persisted_config_t config;
  uint32_t config_version;
  uint32_t command_id;
} board_a_save_request_t;

typedef struct {
  board_a_save_request_t request;
  board_a_save_state_t state;
  board_a_save_error_t error;
  uint32_t raw_error;
  uint8_t occupied;
  uint8_t claimed;
} board_a_save_mailbox_t;

typedef struct {
  board_a_record_format_record_t records[BOARD_A_RECORD_QUEUE_CAPACITY];
  uint16_t head;
  uint16_t count;
  uint16_t high_water;
  uint8_t in_flight;
  uint32_t generated;
  uint32_t synced;
  uint32_t dropped;
  uint32_t uncertain;
  uint32_t storage_errors;
  uint32_t drain_generation;
  board_a_drain_state_t drain_state;
  uint8_t last_synced_valid;
  uint32_t last_synced_seq;
  uint32_t last_synced_file;
  uint32_t last_synced_date;
} board_a_storage_accounting_t;

typedef struct {
  board_a_save_mailbox_t save;
  board_a_config_load_state_t config_load_state;
  uint32_t load_sequence;
  board_a_storage_state_t storage_state;
  board_a_storage_error_t storage_error;
  uint32_t storage_raw_error;
  board_a_storage_accounting_t storage;
} board_a_persistence_t;

typedef struct {
  uint16_t save_state;
  uint32_t save_command_id;
  uint32_t save_config_version;
  uint16_t save_error;
  uint16_t config_load_state;
  uint16_t storage_state;
  uint16_t storage_error;
  uint16_t queued;
  uint16_t queue_high_water;
  uint32_t generated;
  uint32_t synced;
  uint32_t dropped;
  uint32_t uncertain;
  uint16_t in_flight;
  uint16_t drain_state;
  uint32_t last_synced_seq;
  uint32_t last_synced_file;
  uint32_t last_synced_date;
  uint32_t active_config_version;
  uint32_t drain_generation;
  uint32_t storage_errors;
  uint32_t load_sequence;
  uint32_t captured_period;
  uint16_t captured_mask;
  uint16_t captured_count;
} board_a_persistence_status_t;

void board_a_persistence_init(board_a_persistence_t *persistence);

board_a_save_accept_result_t board_a_persistence_accept_save(
    board_a_persistence_t *persistence,
    const board_a_persisted_config_t *config, uint32_t config_version,
    uint32_t command_id);

int board_a_persistence_claim_save(board_a_persistence_t *persistence,
                                   board_a_save_request_t *request);

void board_a_persistence_complete_save(
    board_a_persistence_t *persistence, int success,
    board_a_save_error_t error, uint32_t raw_error);

void board_a_persistence_note_load(
    board_a_persistence_t *persistence, board_a_config_load_state_t state,
    uint32_t sequence);

int board_a_persistence_queue_push(
    board_a_persistence_t *persistence,
    const board_a_record_format_record_t *record);

int board_a_persistence_queue_pop(
    board_a_persistence_t *persistence,
    board_a_record_format_record_t *record);

void board_a_persistence_queue_requeue(
    board_a_persistence_t *persistence,
    const board_a_record_format_record_t *record);

void board_a_persistence_complete_record(
    board_a_persistence_t *persistence,
    const board_a_record_format_record_t *record,
    board_a_record_complete_result_t result);

void board_a_persistence_request_drain(board_a_persistence_t *persistence);

void board_a_persistence_complete_drain(
    board_a_persistence_t *persistence, uint32_t generation, int success);

void board_a_persistence_set_storage_state(
    board_a_persistence_t *persistence, board_a_storage_state_t state,
    board_a_storage_error_t error, uint32_t raw_error);

void board_a_persistence_note_storage_error(
    board_a_persistence_t *persistence, board_a_storage_error_t error,
    uint32_t raw_error);

void board_a_persistence_status(
    const board_a_persistence_t *persistence, uint32_t active_config_version,
    board_a_persistence_status_t *status);

int board_a_persistence_invariant_holds(
    const board_a_persistence_t *persistence);

#endif /* BOARD_A_PERSISTENCE_H */
