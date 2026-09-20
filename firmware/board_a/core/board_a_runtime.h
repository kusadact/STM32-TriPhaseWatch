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

#endif /* BOARD_A_RUNTIME_H */
