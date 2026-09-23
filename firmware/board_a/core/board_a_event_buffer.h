#ifndef BOARD_A_EVENT_BUFFER_H
#define BOARD_A_EVENT_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

#include "../alarm/board_a_alarm.h"

/*
 * The PRE ring is sample based. At the supported one-second snapshot rate,
 * sixteen slots provide at least ten seconds of pre-trigger history.
 */
#define BOARD_A_EVENT_BUFFER_PRE_CAPACITY 16U
#define BOARD_A_EVENT_BUFFER_PRE_MIN_SAMPLES 10U
#define BOARD_A_EVENT_BUFFER_PRE_WINDOW_MS 10000ULL
#define BOARD_A_EVENT_BUFFER_ACTIVE_PERIOD_MS 1000ULL
#define BOARD_A_EVENT_BUFFER_POST_DURATION_MS 30000ULL
#define BOARD_A_EVENT_BUFFER_OUTPUT_CAPACITY 64U

#if BOARD_A_EVENT_BUFFER_OUTPUT_CAPACITY < \
    (BOARD_A_EVENT_BUFFER_PRE_CAPACITY + 1U)
#error "event output queue must hold one complete PRE snapshot and trigger"
#endif

typedef enum {
  BOARD_A_EVENT_PHASE_PRE = 0,
  BOARD_A_EVENT_PHASE_TRIGGER = 1,
  BOARD_A_EVENT_PHASE_ACTIVE = 2,
  BOARD_A_EVENT_PHASE_POST = 3,
  BOARD_A_EVENT_PHASE_CLOSE = 4
} board_a_event_phase_t;

#define BOARD_A_EVENT_FLAG_KNOWN_MASK 0x03FFU

typedef enum {
  BOARD_A_EVENT_FLAG_PRE_SHORT = 1U << 0,
  BOARD_A_EVENT_FLAG_PRE_WRAPPED = 1U << 1,
  BOARD_A_EVENT_FLAG_INCOMPLETE = 1U << 2,
  BOARD_A_EVENT_FLAG_FORCED_CLOSE = 1U << 3,
  BOARD_A_EVENT_FLAG_LEVEL_UPDATED = 1U << 4,
  BOARD_A_EVENT_FLAG_REASON_UPDATED = 1U << 5,
  BOARD_A_EVENT_FLAG_SENSOR_FAULT = 1U << 6,
  BOARD_A_EVENT_FLAG_POST_RETRIGGER = 1U << 7,
  BOARD_A_EVENT_FLAG_MISSED_TICK = 1U << 8,
  BOARD_A_EVENT_FLAG_SAMPLE_REPEAT = 1U << 9
} board_a_event_buffer_flag_t;

typedef struct {
  uint32_t event_id;
  board_a_event_phase_t phase;
  uint32_t sample_id;
  uint64_t time_us;
  uint64_t time_ms;
  uint16_t valid_mask;
  int16_t temperature_x16[BOARD_A_ALARM_PHASE_COUNT];
  uint16_t quality[BOARD_A_ALARM_PHASE_COUNT];
  bool delta_valid;
  int16_t delta_x16;
  board_a_alarm_level_t level;
  board_a_alarm_reason_t reason;
  board_a_alarm_phase_t alarm_phase;
  uint16_t flags;
} board_a_event_buffer_record_t;

typedef struct {
  bool event_open;
  uint32_t event_id;
  uint64_t event_start_us;
  uint16_t pre_available;
  uint16_t pre_exported;
  uint64_t pre_span_ms;
  bool pre_short;
  bool pre_wrapped;
  uint16_t queued_records;
  /*
   * Number of attempts rejected because the output queue was full. A later
   * step retries an unadvanced tick after the caller drains the queue.
   */
  uint32_t queue_full_count;
  uint32_t event_dropped;
  bool incomplete;
  uint16_t flags;
} board_a_event_buffer_status_t;

typedef struct {
  board_a_event_buffer_record_t ring[
      BOARD_A_EVENT_BUFFER_PRE_CAPACITY];
  uint16_t ring_head;
  uint16_t ring_count;
  bool ring_wrapped;
  bool has_last_push_time;
  uint64_t last_push_time_ms;

  board_a_event_buffer_record_t latest;
  bool has_latest;
  board_a_alarm_level_t current_level;
  board_a_alarm_reason_t current_reason;
  board_a_alarm_phase_t current_alarm_phase;

  bool event_open;
  uint8_t run_phase;
  uint32_t event_id;
  uint64_t event_start_us;
  uint64_t event_start_ms;
  uint64_t active_next_ms;
  uint64_t post_start_ms;
  uint64_t post_next_ms;
  uint64_t post_end_ms;
  uint64_t last_emitted_time_ms;
  uint32_t last_emitted_sample_id;
  bool has_last_emitted;

  uint16_t pre_available;
  uint16_t pre_exported;
  uint64_t pre_span_ms;
  bool pre_short;
  bool pre_wrapped;
  bool incomplete;
  uint16_t event_flags;
  uint16_t pending_flags;
  uint32_t queue_full_count;
  uint32_t event_dropped;

  board_a_event_buffer_record_t output[
      BOARD_A_EVENT_BUFFER_OUTPUT_CAPACITY];
  uint16_t output_head;
  uint16_t output_count;
} board_a_event_buffer_t;

/*
 * All public time arguments and output time_ms values use a monotonic
 * millisecond clock. push_snapshot converts snapshot->sample_time_us to
 * milliseconds. The caller must not call push_snapshot with time moving
 * backwards; doing so is rejected.
 */
void board_a_event_buffer_init(board_a_event_buffer_t *buffer);

/*
 * Copies one complete sensor snapshot into the PRE ring. The snapshot may be
 * pushed before or after the matching alarm result is noted. sample_id is
 * copied verbatim and may wrap from UINT32_MAX to zero.
 */
bool board_a_event_buffer_push_snapshot(
    board_a_event_buffer_t *buffer,
    const board_a_sensor_snapshot_t *snapshot);

/*
 * Consumes a P7A result. A raised/updated active alarm starts or updates one
 * event without changing its event_id. A recovered result starts POST. If an
 * active alarm returns during POST, the same event re-enters ACTIVE.
 * Pull order is PRE oldest-to-newest, then TRIGGER, ACTIVE, POST and CLOSE.
 */
bool board_a_event_buffer_note_alarm_result(
    board_a_event_buffer_t *buffer,
    const board_a_alarm_result_t *result);

/*
 * Advances the event timeline. ACTIVE and POST records are generated at
 * one-second boundaries. POST starts with the recovery sample and closes
 * exactly 30000 ms after recovery. This function performs no I/O or locking.
 */
void board_a_event_buffer_step(board_a_event_buffer_t *buffer,
                               uint64_t now_ms);

/*
 * Pops one queued record. The queue must be drained regularly by the caller;
 * a full queue sets the INCOMPLETE event flag instead of overwriting data.
 */
bool board_a_event_buffer_pull_record(
    board_a_event_buffer_t *buffer,
    board_a_event_buffer_record_t *record);

bool board_a_event_buffer_peek_record(
    const board_a_event_buffer_t *buffer,
    board_a_event_buffer_record_t *record);

void board_a_event_buffer_note_queue_full(board_a_event_buffer_t *buffer);

/*
 * Forces the current event to CLOSE at now_ms. The emitted CLOSE record and
 * status carry INCOMPLETE and FORCED_CLOSE. If no event is open, returns
 * false. A hard reset cannot recover this RAM state; callers that need a
 * persistent close marker must call this before reset or persist the marker.
 */
bool board_a_event_buffer_force_close(board_a_event_buffer_t *buffer,
                                      uint64_t now_ms);

/*
 * Records that the persistence queue rejected an already-pulled event row.
 * The caller must invoke this once per dropped row. It marks the event
 * INCOMPLETE and increases the explicit event-dropped counter.
 */
void board_a_event_buffer_note_queue_drop(board_a_event_buffer_t *buffer);

void board_a_event_buffer_status(
    const board_a_event_buffer_t *buffer,
    board_a_event_buffer_status_t *status);

#endif /* BOARD_A_EVENT_BUFFER_H */
