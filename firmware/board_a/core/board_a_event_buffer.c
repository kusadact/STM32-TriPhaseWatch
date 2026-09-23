#include "board_a_event_buffer.h"

#include <limits.h>
#include <string.h>

enum {
  BOARD_A_EVENT_RUN_IDLE = 0,
  BOARD_A_EVENT_RUN_ACTIVE = 1,
  BOARD_A_EVENT_RUN_POST = 2
};

static uint64_t add_ms_saturated(uint64_t value, uint64_t delta)
{
  if (UINT64_MAX - value < delta) {
    return UINT64_MAX;
  }
  return value + delta;
}

static bool quality_can_contribute(uint16_t quality)
{
  return (quality == BOARD_A_QUALITY_OK) ||
      (quality == BOARD_A_QUALITY_STALE);
}

static bool alarm_level_is_active(board_a_alarm_level_t level)
{
  return (level == BOARD_A_ALARM_NOTICE) ||
      (level == BOARD_A_ALARM_WARNING) ||
      (level == BOARD_A_ALARM_CRITICAL) ||
      (level == BOARD_A_ALARM_SENSOR_FAULT);
}

static bool record_same_sample(const board_a_event_buffer_record_t *left,
                               const board_a_event_buffer_record_t *right)
{
  return (left->sample_id == right->sample_id) &&
      (left->time_ms == right->time_ms);
}

static void record_apply_alarm_identity(
    board_a_event_buffer_record_t *record,
    const board_a_alarm_state_t *state)
{
  record->level = state->level;
  record->reason = state->reason;
  record->alarm_phase = state->trigger_phase;
}

static void record_apply_alarm_metadata(
    board_a_event_buffer_record_t *record,
    const board_a_alarm_state_t *state)
{
  record_apply_alarm_identity(record, state);
  record->delta_valid = state->delta_valid;
  record->delta_x16 = state->maximum_delta_x16;
}

static void record_apply_alarm_state(
    board_a_event_buffer_record_t *record,
    const board_a_alarm_state_t *state)
{
  uint8_t phase;

  record->sample_id = state->sample_id;
  record->time_us = state->sample_time_us;
  record->time_ms = state->sample_time_ms;
  record->valid_mask = state->display_mask;
  for (phase = 0U; phase < BOARD_A_ALARM_PHASE_COUNT; ++phase) {
    record->temperature_x16[phase] = state->temperature_x16[phase];
    record->quality[phase] = state->quality[phase];
  }
  record_apply_alarm_metadata(record, state);
}

static board_a_event_buffer_record_t record_from_alarm_state(
    const board_a_alarm_state_t *state)
{
  board_a_event_buffer_record_t record;

  memset(&record, 0, sizeof(record));
  record.event_id = 0U;
  record.phase = BOARD_A_EVENT_PHASE_PRE;
  record_apply_alarm_state(&record, state);
  return record;
}

static board_a_event_buffer_record_t record_from_snapshot(
    const board_a_event_buffer_t *buffer,
    const board_a_sensor_snapshot_t *snapshot)
{
  board_a_event_buffer_record_t record;
  uint8_t phase;
  bool have_temperature = false;
  int16_t minimum_x16 = 0;
  int16_t maximum_x16 = 0;
  uint8_t valid_phases = 0U;

  memset(&record, 0, sizeof(record));
  record.event_id = 0U;
  record.phase = BOARD_A_EVENT_PHASE_PRE;
  record.sample_id = snapshot->sample_id;
  record.time_us = snapshot->sample_time_us;
  record.time_ms = snapshot->sample_time_us / 1000ULL;
  record.level = buffer->current_level;
  record.reason = buffer->current_reason;
  record.alarm_phase = buffer->current_alarm_phase;

  for (phase = 0U; phase < BOARD_A_ALARM_PHASE_COUNT; ++phase) {
    const board_a_sensor_sample_t *source = &snapshot->sensors[phase];

    record.quality[phase] = source->quality;
    if (!source->has_value) {
      continue;
    }
    record.valid_mask |= (uint16_t)(1U << phase);
    record.temperature_x16[phase] = source->temperature_x16;
    if (!quality_can_contribute(source->quality)) {
      continue;
    }
    if (!have_temperature) {
      minimum_x16 = source->temperature_x16;
      maximum_x16 = source->temperature_x16;
      have_temperature = true;
    } else {
      if (source->temperature_x16 < minimum_x16) {
        minimum_x16 = source->temperature_x16;
      }
      if (source->temperature_x16 > maximum_x16) {
        maximum_x16 = source->temperature_x16;
      }
    }
    valid_phases++;
  }

  if (valid_phases >= 2U) {
    int32_t delta_x16 = (int32_t)maximum_x16 - (int32_t)minimum_x16;

    if (delta_x16 > INT16_MAX) {
      delta_x16 = INT16_MAX;
    } else if (delta_x16 < INT16_MIN) {
      delta_x16 = INT16_MIN;
    }
    record.delta_valid = true;
    record.delta_x16 = (int16_t)delta_x16;
  }
  return record;
}

static void record_from_latest(const board_a_event_buffer_t *buffer,
                               board_a_event_buffer_record_t *record)
{
  if (buffer->has_latest) {
    *record = buffer->latest;
  } else {
    memset(record, 0, sizeof(*record));
    record->time_ms = 0U;
    record->time_us = 0U;
    record->level = buffer->current_level;
    record->reason = buffer->current_reason;
    record->alarm_phase = buffer->current_alarm_phase;
  }
}

static void note_emitted_sample(
    board_a_event_buffer_t *buffer,
    const board_a_event_buffer_record_t *record)
{
  buffer->has_last_emitted = true;
  buffer->last_emitted_sample_id = record->sample_id;
  buffer->last_emitted_time_ms = record->time_ms;
}

static bool record_is_repeat(const board_a_event_buffer_t *buffer,
                             const board_a_event_buffer_record_t *record)
{
  return buffer->has_last_emitted &&
      (buffer->last_emitted_sample_id == record->sample_id) &&
      (buffer->last_emitted_time_ms == record->time_ms);
}

static bool queue_record(board_a_event_buffer_t *buffer,
                         const board_a_event_buffer_record_t *record)
{
  uint16_t tail;

  if (buffer->output_count >= BOARD_A_EVENT_BUFFER_OUTPUT_CAPACITY) {
    buffer->queue_full_count++;
    buffer->incomplete = true;
    buffer->event_flags |= BOARD_A_EVENT_FLAG_INCOMPLETE;
    return false;
  }

  tail = (uint16_t)((buffer->output_head + buffer->output_count) %
                    BOARD_A_EVENT_BUFFER_OUTPUT_CAPACITY);
  buffer->output[tail] = *record;
  buffer->output[tail].flags |= buffer->event_flags;
  buffer->output_count++;
  return true;
}

static bool queue_record_force(board_a_event_buffer_t *buffer,
                               const board_a_event_buffer_record_t *record)
{
  if (buffer->output_count < BOARD_A_EVENT_BUFFER_OUTPUT_CAPACITY) {
    return queue_record(buffer, record);
  }

  /*
   * Preserve the terminal CLOSE row by sacrificing the oldest queued row.
   * The drop is explicit so the event cannot appear complete.
   */
  buffer->output_head =
      (uint16_t)((buffer->output_head + 1U) %
                 BOARD_A_EVENT_BUFFER_OUTPUT_CAPACITY);
  buffer->output_count--;
  buffer->queue_full_count++;
  buffer->event_dropped++;
  buffer->incomplete = true;
  buffer->event_flags |= BOARD_A_EVENT_FLAG_INCOMPLETE;
  return queue_record(buffer, record);
}

static board_a_event_buffer_record_t ring_record_at(
    const board_a_event_buffer_t *buffer, uint16_t offset)
{
  uint16_t index = (uint16_t)((buffer->ring_head + offset) %
                              BOARD_A_EVENT_BUFFER_PRE_CAPACITY);

  return buffer->ring[index];
}

static board_a_event_buffer_record_t select_sample_for_deadline(
    const board_a_event_buffer_t *buffer, uint64_t deadline_ms)
{
  uint16_t offset;
  board_a_event_buffer_record_t record;

  if (buffer->ring_count != 0U) {
    for (offset = buffer->ring_count; offset != 0U; --offset) {
      record = ring_record_at(buffer, (uint16_t)(offset - 1U));
      if (record.time_ms <= deadline_ms) {
        return record;
      }
    }
  }

  record_from_latest(buffer, &record);
  if ((buffer->ring_count == 0U) && !buffer->has_latest) {
    record.time_ms = deadline_ms;
  }
  return record;
}

static void update_current_alarm_metadata(
    board_a_event_buffer_t *buffer,
    const board_a_alarm_state_t *state)
{
  board_a_alarm_level_t previous_level = buffer->current_level;
  board_a_alarm_reason_t previous_reason = buffer->current_reason;
  board_a_alarm_phase_t previous_phase = buffer->current_alarm_phase;

  if ((previous_level != state->level) ||
      (previous_reason != state->reason) ||
      (previous_phase != state->trigger_phase)) {
    if (previous_level != state->level) {
      buffer->pending_flags |= BOARD_A_EVENT_FLAG_LEVEL_UPDATED;
    }
    if ((previous_reason != state->reason) ||
        (previous_phase != state->trigger_phase)) {
      buffer->pending_flags |= BOARD_A_EVENT_FLAG_REASON_UPDATED;
    }
  }
  buffer->current_level = state->level;
  buffer->current_reason = state->reason;
  buffer->current_alarm_phase = state->trigger_phase;

  if (buffer->has_latest &&
      (buffer->latest.sample_id == state->sample_id) &&
      (buffer->latest.time_ms == state->sample_time_ms)) {
    record_apply_alarm_state(&buffer->latest, state);
  } else if (buffer->has_latest) {
    record_apply_alarm_identity(&buffer->latest, state);
  }
}

static board_a_event_buffer_record_t make_event_record(
    const board_a_event_buffer_t *buffer,
    const board_a_event_buffer_record_t *source,
    board_a_event_phase_t phase,
    uint16_t extra_flags)
{
  board_a_event_buffer_record_t record = *source;

  record.event_id = buffer->event_id;
  record.phase = phase;
  record.flags = buffer->event_flags | buffer->pending_flags |
      extra_flags;
  if (record.level == BOARD_A_ALARM_SENSOR_FAULT) {
    record.flags |= BOARD_A_EVENT_FLAG_SENSOR_FAULT;
  }
  return record;
}

static bool start_event(board_a_event_buffer_t *buffer,
                        const board_a_alarm_result_t *result)
{
  board_a_event_buffer_record_t trigger;
  board_a_event_buffer_record_t record;
  uint16_t offset;
  uint16_t available;
  uint16_t exported = 0U;
  bool trigger_in_ring;
  uint64_t oldest_ms = 0U;
  uint64_t newest_ms = 0U;

  if (result->state.valid) {
    trigger = record_from_alarm_state(&result->state);
  } else {
    record_from_latest(buffer, &trigger);
    trigger.sample_id = result->state.sample_id;
    trigger.time_ms = result->state.sample_time_ms;
  }

  buffer->event_open = true;
  buffer->run_phase = BOARD_A_EVENT_RUN_ACTIVE;
  buffer->event_id = result->event_id;
  buffer->event_start_us = trigger.time_us;
  buffer->event_start_ms = trigger.time_ms;
  buffer->active_next_ms = add_ms_saturated(
      trigger.time_ms, BOARD_A_EVENT_BUFFER_ACTIVE_PERIOD_MS);
  buffer->post_start_ms = 0U;
  buffer->post_next_ms = 0U;
  buffer->post_end_ms = 0U;
  buffer->pre_wrapped = buffer->ring_wrapped;
  buffer->event_flags = 0U;
  buffer->pending_flags = 0U;
  buffer->incomplete = false;
  buffer->has_last_emitted = false;

  available = buffer->ring_count;
  trigger_in_ring = false;
  if (available != 0U) {
    record = ring_record_at(buffer, (uint16_t)(available - 1U));
    trigger_in_ring = record_same_sample(&record, &trigger);
  }
  if (trigger_in_ring && (available != 0U)) {
    available--;
  }
  buffer->pre_available = available;
  buffer->pre_short = available < BOARD_A_EVENT_BUFFER_PRE_MIN_SAMPLES;
  if (buffer->pre_short) {
    buffer->event_flags |= BOARD_A_EVENT_FLAG_PRE_SHORT;
  }
  if (buffer->pre_wrapped) {
    buffer->event_flags |= BOARD_A_EVENT_FLAG_PRE_WRAPPED;
  }
  buffer->pre_span_ms = 0U;
  if (available != 0U) {
    oldest_ms = ring_record_at(buffer, 0U).time_ms;
    newest_ms = ring_record_at(buffer, (uint16_t)(available - 1U)).time_ms;
    if (newest_ms >= oldest_ms) {
      buffer->pre_span_ms = newest_ms - oldest_ms;
    }
  }

  for (offset = 0U; offset < buffer->ring_count; ++offset) {
    record = ring_record_at(buffer, offset);
    if (trigger_in_ring && record_same_sample(&record, &trigger)) {
      continue;
    }
    record = make_event_record(buffer, &record, BOARD_A_EVENT_PHASE_PRE, 0U);
    if (!queue_record(buffer, &record)) {
      break;
    }
    exported++;
  }
  buffer->pre_exported = exported;

  trigger = make_event_record(buffer, &trigger, BOARD_A_EVENT_PHASE_TRIGGER,
                              0U);
  if (queue_record_force(buffer, &trigger)) {
    note_emitted_sample(buffer, &trigger);
  }
  buffer->pending_flags = 0U;
  return true;
}

static void enter_post(board_a_event_buffer_t *buffer,
                       const board_a_alarm_result_t *result)
{
  board_a_event_buffer_record_t recovery;

  if (result->state.valid) {
    recovery = record_from_alarm_state(&result->state);
  } else {
    record_from_latest(buffer, &recovery);
    recovery.sample_id = result->state.sample_id;
    recovery.time_ms = result->state.sample_time_ms;
  }

  buffer->run_phase = BOARD_A_EVENT_RUN_POST;
  buffer->post_start_ms = recovery.time_ms;
  buffer->post_next_ms = add_ms_saturated(
      recovery.time_ms, BOARD_A_EVENT_BUFFER_ACTIVE_PERIOD_MS);
  buffer->post_end_ms = add_ms_saturated(
      recovery.time_ms, BOARD_A_EVENT_BUFFER_POST_DURATION_MS);
  recovery = make_event_record(buffer, &recovery, BOARD_A_EVENT_PHASE_POST,
                               0U);
  if (queue_record(buffer, &recovery)) {
    note_emitted_sample(buffer, &recovery);
  }
  buffer->pending_flags = 0U;
}

static void update_event(board_a_event_buffer_t *buffer,
                         const board_a_alarm_result_t *result)
{
  bool retriggered = buffer->run_phase == BOARD_A_EVENT_RUN_POST;

  update_current_alarm_metadata(buffer, &result->state);
  if (retriggered && alarm_level_is_active(result->state.level)) {
    buffer->run_phase = BOARD_A_EVENT_RUN_ACTIVE;
    buffer->active_next_ms = result->state.sample_time_ms;
    buffer->post_start_ms = 0U;
    buffer->post_next_ms = 0U;
    buffer->post_end_ms = 0U;
    buffer->event_flags |= BOARD_A_EVENT_FLAG_POST_RETRIGGER;
  }
}

static void close_event(board_a_event_buffer_t *buffer, uint64_t now_ms,
                        bool forced)
{
  board_a_event_buffer_record_t close_record;

  record_from_latest(buffer, &close_record);
  close_record.event_id = buffer->event_id;
  close_record.phase = BOARD_A_EVENT_PHASE_CLOSE;
  close_record.time_us = now_ms * 1000ULL;
  close_record.time_ms = now_ms;
  close_record.level = buffer->current_level;
  close_record.reason = buffer->current_reason;
  close_record.alarm_phase = buffer->current_alarm_phase;
  close_record.flags = buffer->event_flags | buffer->pending_flags;
  if (forced) {
    buffer->incomplete = true;
    buffer->event_flags |= BOARD_A_EVENT_FLAG_INCOMPLETE |
        BOARD_A_EVENT_FLAG_FORCED_CLOSE;
    close_record.flags = buffer->event_flags | buffer->pending_flags;
  }
  (void)queue_record_force(buffer, &close_record);
  buffer->pending_flags = 0U;
  buffer->event_open = false;
  buffer->run_phase = BOARD_A_EVENT_RUN_IDLE;
}

static void emit_active_or_post(board_a_event_buffer_t *buffer,
                                uint64_t now_ms)
{
  board_a_event_buffer_record_t record;
  board_a_event_phase_t phase;
  uint64_t *next_ms;
  uint64_t end_ms = UINT64_MAX;

  if (buffer->run_phase == BOARD_A_EVENT_RUN_ACTIVE) {
    phase = BOARD_A_EVENT_PHASE_ACTIVE;
    next_ms = &buffer->active_next_ms;
  } else if (buffer->run_phase == BOARD_A_EVENT_RUN_POST) {
    phase = BOARD_A_EVENT_PHASE_POST;
    next_ms = &buffer->post_next_ms;
    end_ms = buffer->post_end_ms;
  } else {
    return;
  }

  while ((now_ms >= *next_ms) && (*next_ms < end_ms)) {
    uint16_t flags = 0U;

    record = select_sample_for_deadline(buffer, *next_ms);
    if (record_is_repeat(buffer, &record)) {
      flags |= BOARD_A_EVENT_FLAG_SAMPLE_REPEAT;
    }
    if ((*next_ms != 0U) &&
        (now_ms >= add_ms_saturated(*next_ms,
                                    BOARD_A_EVENT_BUFFER_ACTIVE_PERIOD_MS))) {
      flags |= BOARD_A_EVENT_FLAG_MISSED_TICK;
    }
    record = make_event_record(buffer, &record, phase, flags);
    if (!queue_record(buffer, &record)) {
      return;
    }
    note_emitted_sample(buffer, &record);
    buffer->pending_flags = 0U;
    if (*next_ms == UINT64_MAX) {
      return;
    }
    *next_ms = add_ms_saturated(
        *next_ms, BOARD_A_EVENT_BUFFER_ACTIVE_PERIOD_MS);
  }
}

void board_a_event_buffer_init(board_a_event_buffer_t *buffer)
{
  if (buffer == NULL) {
    return;
  }
  memset(buffer, 0, sizeof(*buffer));
  buffer->current_level = BOARD_A_ALARM_UNKNOWN;
  buffer->current_reason = BOARD_A_ALARM_REASON_NONE;
  buffer->current_alarm_phase = BOARD_A_ALARM_PHASE_NONE;
  buffer->run_phase = BOARD_A_EVENT_RUN_IDLE;
}

bool board_a_event_buffer_push_snapshot(
    board_a_event_buffer_t *buffer,
    const board_a_sensor_snapshot_t *snapshot)
{
  board_a_event_buffer_record_t record;
  uint16_t tail;
  uint64_t time_ms;

  if ((buffer == NULL) || (snapshot == NULL)) {
    return false;
  }

  time_ms = snapshot->sample_time_us / 1000ULL;
  if (buffer->has_last_push_time && (time_ms < buffer->last_push_time_ms)) {
    return false;
  }

  record = record_from_snapshot(buffer, snapshot);
  if (buffer->ring_count < BOARD_A_EVENT_BUFFER_PRE_CAPACITY) {
    tail = (uint16_t)((buffer->ring_head + buffer->ring_count) %
                      BOARD_A_EVENT_BUFFER_PRE_CAPACITY);
    buffer->ring_count++;
  } else {
    tail = buffer->ring_head;
    buffer->ring_head =
        (uint16_t)((buffer->ring_head + 1U) %
                   BOARD_A_EVENT_BUFFER_PRE_CAPACITY);
    buffer->ring_wrapped = true;
  }
  buffer->ring[tail] = record;
  buffer->latest = record;
  buffer->has_latest = true;
  buffer->has_last_push_time = true;
  buffer->last_push_time_ms = time_ms;
  return true;
}

bool board_a_event_buffer_note_alarm_result(
    board_a_event_buffer_t *buffer,
    const board_a_alarm_result_t *result)
{
  if ((buffer == NULL) || (result == NULL) || !result->state.valid) {
    return false;
  }

  if (result->event && (result->event_type == BOARD_A_ALARM_EVENT_RAISED) &&
      !buffer->event_open) {
    update_current_alarm_metadata(buffer, &result->state);
    return start_event(buffer, result);
  }
  if (result->event && (result->event_type == BOARD_A_ALARM_EVENT_UPDATED) &&
      !buffer->event_open &&
      alarm_level_is_active(result->state.level)) {
    update_current_alarm_metadata(buffer, &result->state);
    return start_event(buffer, result);
  }
  if (result->event &&
      (result->event_type == BOARD_A_ALARM_EVENT_RECOVERED)) {
    if (!buffer->event_open) {
      return false;
    }
    update_current_alarm_metadata(buffer, &result->state);
    enter_post(buffer, result);
    return true;
  }

  if (!buffer->event_open) {
    update_current_alarm_metadata(buffer, &result->state);
    if (alarm_level_is_active(result->state.level) &&
        (result->event_type != BOARD_A_ALARM_EVENT_RECOVERED)) {
      return start_event(buffer, result);
    }
    return true;
  }

  update_event(buffer, result);
  return true;
}

void board_a_event_buffer_step(board_a_event_buffer_t *buffer,
                               uint64_t now_ms)
{
  if ((buffer == NULL) || !buffer->event_open) {
    return;
  }

  if (buffer->run_phase == BOARD_A_EVENT_RUN_POST) {
    if (now_ms >= buffer->post_end_ms) {
      emit_active_or_post(buffer, buffer->post_end_ms);
      close_event(buffer, buffer->post_end_ms, false);
      return;
    }
  }
  emit_active_or_post(buffer, now_ms);
}

bool board_a_event_buffer_pull_record(
    board_a_event_buffer_t *buffer,
    board_a_event_buffer_record_t *record)
{
  if ((buffer == NULL) || (record == NULL) || (buffer->output_count == 0U)) {
    return false;
  }

  *record = buffer->output[buffer->output_head];
  record->flags |= buffer->event_flags;
  buffer->output_head =
      (uint16_t)((buffer->output_head + 1U) %
                 BOARD_A_EVENT_BUFFER_OUTPUT_CAPACITY);
  buffer->output_count--;
  return true;
}

bool board_a_event_buffer_peek_record(
    const board_a_event_buffer_t *buffer,
    board_a_event_buffer_record_t *record)
{
  if ((buffer == NULL) || (record == NULL) || (buffer->output_count == 0U)) {
    return false;
  }

  *record = buffer->output[buffer->output_head];
  record->flags |= buffer->event_flags;
  return true;
}

void board_a_event_buffer_note_queue_full(board_a_event_buffer_t *buffer)
{
  if (buffer == NULL) {
    return;
  }
  buffer->queue_full_count++;
  buffer->incomplete = true;
  buffer->event_flags |= BOARD_A_EVENT_FLAG_INCOMPLETE;
}

bool board_a_event_buffer_force_close(board_a_event_buffer_t *buffer,
                                      uint64_t now_ms)
{
  if ((buffer == NULL) || !buffer->event_open) {
    return false;
  }
  close_event(buffer, now_ms, true);
  return true;
}

void board_a_event_buffer_note_queue_drop(board_a_event_buffer_t *buffer)
{
  if (buffer == NULL) {
    return;
  }
  buffer->event_dropped++;
  buffer->incomplete = true;
  buffer->event_flags |= BOARD_A_EVENT_FLAG_INCOMPLETE;
}

void board_a_event_buffer_status(
    const board_a_event_buffer_t *buffer,
    board_a_event_buffer_status_t *status)
{
  if ((buffer == NULL) || (status == NULL)) {
    return;
  }

  memset(status, 0, sizeof(*status));
  status->event_open = buffer->event_open;
  status->event_id = buffer->event_id;
  status->event_start_us = buffer->event_start_us;
  if (buffer->event_open) {
    status->pre_available = buffer->pre_available;
    status->pre_exported = buffer->pre_exported;
    status->pre_span_ms = buffer->pre_span_ms;
    status->pre_short = buffer->pre_short;
    status->pre_wrapped = buffer->pre_wrapped;
  } else {
    status->pre_available = buffer->ring_count;
    status->pre_short =
        buffer->ring_count < BOARD_A_EVENT_BUFFER_PRE_MIN_SAMPLES;
    status->pre_wrapped = buffer->ring_wrapped;
    if (buffer->ring_count != 0U) {
      uint64_t oldest = ring_record_at(buffer, 0U).time_ms;
      uint64_t newest =
          ring_record_at(buffer, (uint16_t)(buffer->ring_count - 1U)).time_ms;
      if (newest >= oldest) {
        status->pre_span_ms = newest - oldest;
      }
    }
  }
  status->queued_records = buffer->output_count;
  status->queue_full_count = buffer->queue_full_count;
  status->event_dropped = buffer->event_dropped;
  status->incomplete = buffer->incomplete;
  status->flags = buffer->event_flags;
}
