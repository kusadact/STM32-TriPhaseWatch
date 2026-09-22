#ifndef BOARD_A_RUNTIME_H
#define BOARD_A_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_a_slave.h"

typedef bool (*board_a_runtime_lock_fn)(void *context);
typedef void (*board_a_runtime_unlock_fn)(void *context);
typedef uint64_t (*board_a_runtime_now_fn)(void *context);

typedef struct {
  board_a_runtime_lock_fn lock;
  board_a_runtime_unlock_fn unlock;
  board_a_runtime_now_fn now_us;
} board_a_runtime_ops_t;

typedef struct {
  uint16_t run_state;
  uint16_t data_source;
  uint16_t last_command;
  uint16_t command_result;
  uint32_t last_command_id;
  uint32_t records_this_run;
  uint32_t sequence;
  uint64_t next_sample_us;
  uint64_t schedule_deadline_us;
  bool start_pending;
  bool schedule_armed;
  uint32_t schedule_start_late_us;
  uint32_t schedule_start_count;
  board_a_model_stats_t stats;
} board_a_runtime_status_t;

typedef struct {
  board_a_slave_t slave;
  const board_a_runtime_ops_t *ops;
  void *context;
} board_a_runtime_t;

void board_a_runtime_init(board_a_runtime_t *runtime,
                          uint32_t session_id,
                          const board_a_runtime_ops_t *ops,
                          void *context);

bool board_a_runtime_set_data_source(board_a_runtime_t *runtime,
                                     uint16_t source);

void board_a_runtime_publish_sensor_snapshot(
    board_a_runtime_t *runtime,
    const board_a_sensor_snapshot_t *snapshot);

bool board_a_runtime_publish_sensor_map(
    board_a_runtime_t *runtime, const board_a_sensor_map_t *map);

bool board_a_runtime_copy_sensor_map(
    board_a_runtime_t *runtime, board_a_sensor_map_t *map);

void board_a_runtime_publish_alarm_state(
    board_a_runtime_t *runtime, const board_a_alarm_state_t *state);

bool board_a_runtime_copy_alarm_state(
    board_a_runtime_t *runtime, board_a_alarm_state_t *state);

void board_a_runtime_publish_alarm_result(
    board_a_runtime_t *runtime, const board_a_alarm_result_t *result);

bool board_a_runtime_copy_alarm_event(
    board_a_runtime_t *runtime, board_a_alarm_result_t *result);

bool board_a_runtime_copy_alarm_config(
    board_a_runtime_t *runtime, board_a_alarm_config_t *config);

void board_a_runtime_publish_alarm_buzzer_active(
    board_a_runtime_t *runtime, bool active);

bool board_a_runtime_take_alarm_ack_request(board_a_runtime_t *runtime);

bool board_a_runtime_request_sensor_map_save(
    board_a_runtime_t *runtime, uint32_t command_id);

/* Only the single CommTask/test owner may call this function. */
void board_a_runtime_push_byte(board_a_runtime_t *runtime,
                               uint8_t byte,
                               uint32_t now_us);

/* The lock callback must provide mutual exclusion for the complete call. */
size_t board_a_runtime_poll(board_a_runtime_t *runtime,
                            uint32_t now_us,
                            uint8_t *response,
                            size_t response_capacity);

size_t board_a_runtime_poll_observe(board_a_runtime_t *runtime,
                                    uint32_t now_us,
                                    uint8_t *response,
                                    size_t response_capacity,
                                    bool *scheduling_state_changed);

/* The lock callback must provide mutual exclusion for the complete call. */
void board_a_runtime_tick(board_a_runtime_t *runtime, uint64_t now_us);

bool board_a_runtime_copy_status(board_a_runtime_t *runtime,
                                 board_a_runtime_status_t *status);

bool board_a_runtime_read_snapshot(board_a_runtime_t *runtime,
                                   board_a_snapshot_t *snapshot);

bool board_a_runtime_claim_save(board_a_runtime_t *runtime,
                                board_a_save_request_t *request);

void board_a_runtime_complete_save(board_a_runtime_t *runtime, int success,
                                   board_a_save_error_t error,
                                   uint32_t raw_error);

bool board_a_runtime_pop_record(
    board_a_runtime_t *runtime,
    board_a_record_format_record_t *record);

void board_a_runtime_requeue_record(
    board_a_runtime_t *runtime,
    const board_a_record_format_record_t *record);

void board_a_runtime_complete_record(
    board_a_runtime_t *runtime,
    const board_a_record_format_record_t *record,
    board_a_record_complete_result_t result);

void board_a_runtime_set_storage_state(
    board_a_runtime_t *runtime, board_a_storage_state_t state,
    board_a_storage_error_t error, uint32_t raw_error);

void board_a_runtime_note_storage_error(
    board_a_runtime_t *runtime, board_a_storage_error_t error,
    uint32_t raw_error);

void board_a_runtime_complete_drain(board_a_runtime_t *runtime,
                                    uint32_t generation, int success);

bool board_a_runtime_persistence_status(
    board_a_runtime_t *runtime, board_a_persistence_status_t *status);

void board_a_runtime_apply_loaded_config(
    board_a_runtime_t *runtime, const board_a_persisted_config_t *config,
    uint32_t sequence);

void board_a_runtime_note_config_load(
    board_a_runtime_t *runtime, board_a_config_load_state_t state,
    uint32_t sequence);

#endif /* BOARD_A_RUNTIME_H */
