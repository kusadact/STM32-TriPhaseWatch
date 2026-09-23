#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "board_a_event_buffer.h"

static unsigned int checks;
static unsigned int failures;

#define CHECK(condition)                                                     \
  do {                                                                       \
    checks++;                                                                \
    if (!(condition)) {                                                      \
      failures++;                                                            \
      printf("FAIL %s:%u: %s\n", __FILE__, __LINE__, #condition);           \
    }                                                                        \
  } while (0)

static void snapshot_init(board_a_sensor_snapshot_t *snapshot,
                          uint32_t sample_id,
                          uint64_t sample_time_us)
{
  uint8_t phase;

  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->sample_id = sample_id;
  snapshot->sample_time_us = sample_time_us;
  for (phase = 0U; phase < BOARD_A_SENSOR_COUNT; ++phase) {
    snapshot->sensors[phase].sensor_id = phase;
    snapshot->sensors[phase].sensor_type = BOARD_A_SENSOR_TYPE_DS18B20;
    snapshot->sensors[phase].quality = BOARD_A_QUALITY_NOT_PRESENT;
  }
}

static void snapshot_set_all(board_a_sensor_snapshot_t *snapshot,
                             int16_t a_x16,
                             int16_t b_x16,
                             int16_t c_x16)
{
  uint8_t phase;

  for (phase = 0U; phase < BOARD_A_SENSOR_COUNT; ++phase) {
    snapshot->sensors[phase].has_value = true;
    snapshot->sensors[phase].quality = BOARD_A_QUALITY_OK;
    snapshot->valid_mask |= (uint16_t)(1U << phase);
  }
  snapshot->sensors[0].temperature_x16 = a_x16;
  snapshot->sensors[1].temperature_x16 = b_x16;
  snapshot->sensors[2].temperature_x16 = c_x16;
}

static bool push_sample(board_a_event_buffer_t *buffer,
                        uint32_t sample_id,
                        uint64_t time_ms,
                        int16_t a_x16,
                        int16_t b_x16,
                        int16_t c_x16)
{
  board_a_sensor_snapshot_t snapshot;

  snapshot_init(&snapshot, sample_id, time_ms * 1000ULL);
  snapshot_set_all(&snapshot, a_x16, b_x16, c_x16);
  return board_a_event_buffer_push_snapshot(buffer, &snapshot);
}

static board_a_alarm_result_t make_result(
    bool event,
    board_a_alarm_event_type_t event_type,
    uint32_t event_id,
    uint32_t sample_id,
    uint64_t time_ms,
    board_a_alarm_level_t level,
    board_a_alarm_reason_t reason,
    board_a_alarm_phase_t alarm_phase)
{
  board_a_alarm_result_t result;
  uint8_t phase;

  memset(&result, 0, sizeof(result));
  result.event = event;
  result.event_type = event_type;
  result.event_id = event_id;
  result.event_time_ms = time_ms;
  result.state.valid = true;
  result.state.sample_id = sample_id;
  result.state.sample_time_ms = time_ms;
  result.state.display_mask = 0x0007U;
  result.state.comparison_mask = 0x0007U;
  result.state.delta_valid = true;
  result.state.maximum_delta_x16 = 32;
  result.state.hottest_phase = BOARD_A_ALARM_PHASE_A;
  result.state.coldest_phase = BOARD_A_ALARM_PHASE_B;
  result.state.trigger_phase = alarm_phase;
  result.state.level = level;
  result.state.reason = reason;
  result.state.event_id = event_id;
  for (phase = 0U; phase < BOARD_A_ALARM_PHASE_COUNT; ++phase) {
    result.state.temperature_x16[phase] = 400;
    result.state.quality[phase] = BOARD_A_QUALITY_OK;
  }
  if (level == BOARD_A_ALARM_SENSOR_FAULT) {
    result.state.fault_mask = 0x0001U;
    result.state.display_mask = 0x0006U;
    result.state.comparison_mask = 0x0006U;
    result.state.delta_valid = true;
    result.state.maximum_delta_x16 = 0;
    result.state.quality[0] = BOARD_A_QUALITY_CRC_ERROR;
  }
  return result;
}

static bool pull_phase(board_a_event_buffer_t *buffer,
                       board_a_event_phase_t phase,
                       board_a_event_buffer_record_t *record)
{
  board_a_event_buffer_record_t candidate;

  while (board_a_event_buffer_pull_record(buffer, &candidate)) {
    if (candidate.phase == phase) {
      if (record != NULL) {
        *record = candidate;
      }
      return true;
    }
  }
  return false;
}

static void drain_buffer(board_a_event_buffer_t *buffer)
{
  board_a_event_buffer_record_t record;

  while (board_a_event_buffer_pull_record(buffer, &record)) {
  }
}

static void test_pre_boundary_and_short(void)
{
  board_a_event_buffer_t buffer;
  board_a_event_buffer_status_t status;
  board_a_event_buffer_record_t record;
  board_a_alarm_result_t result;
  uint32_t index;

  board_a_event_buffer_init(&buffer);
  for (index = 1U; index <= 9U; ++index) {
    CHECK(push_sample(&buffer, index, (uint64_t)(index - 1U) * 1000ULL,
                      400, 400, 400));
  }
  board_a_event_buffer_status(&buffer, &status);
  CHECK(status.pre_available == 9U);
  CHECK(status.pre_short);
  CHECK(!status.pre_wrapped);

  result = make_result(true, BOARD_A_ALARM_EVENT_RAISED, 1U, 10U, 9000U,
                       BOARD_A_ALARM_WARNING,
                       BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH,
                       BOARD_A_ALARM_PHASE_A);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  board_a_event_buffer_status(&buffer, &status);
  CHECK(status.event_open);
  CHECK(status.event_id == 1U);
  CHECK(status.pre_available == 9U);
  CHECK(status.pre_exported == 9U);
  CHECK(status.pre_short);

  for (index = 0U; index < 9U; ++index) {
    CHECK(board_a_event_buffer_pull_record(&buffer, &record));
    CHECK(record.phase == BOARD_A_EVENT_PHASE_PRE);
    CHECK(record.event_id == 1U);
    CHECK(record.sample_id == (index + 1U));
    CHECK(record.time_ms == (uint64_t)index * 1000ULL);
  }
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_TRIGGER);
  CHECK(record.sample_id == 10U);
  CHECK(record.time_ms == 9000U);
  CHECK(record.level == BOARD_A_ALARM_WARNING);
  CHECK((record.flags & BOARD_A_EVENT_FLAG_PRE_SHORT) != 0U);
  CHECK(board_a_event_buffer_force_close(&buffer, 9500U));
  CHECK(pull_phase(&buffer, BOARD_A_EVENT_PHASE_CLOSE, &record));
  CHECK((record.flags & BOARD_A_EVENT_FLAG_INCOMPLETE) != 0U);
  CHECK((record.flags & BOARD_A_EVENT_FLAG_FORCED_CLOSE) != 0U);

  board_a_event_buffer_init(&buffer);
  for (index = 1U; index <= 10U; ++index) {
    CHECK(push_sample(&buffer, index, (uint64_t)(index - 1U) * 1000ULL,
                      400, 400, 400));
  }
  result = make_result(true, BOARD_A_ALARM_EVENT_RAISED, 2U, 11U, 10000U,
                       BOARD_A_ALARM_WARNING,
                       BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH,
                       BOARD_A_ALARM_PHASE_A);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  board_a_event_buffer_status(&buffer, &status);
  CHECK(status.pre_available == 10U);
  CHECK(status.pre_exported == 10U);
  CHECK(!status.pre_short);
  for (index = 0U; index < 10U; ++index) {
    CHECK(board_a_event_buffer_pull_record(&buffer, &record));
    CHECK(record.phase == BOARD_A_EVENT_PHASE_PRE);
    CHECK(record.sample_id == (index + 1U));
  }
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_TRIGGER);
  CHECK(record.sample_id == 11U);
}

static void test_post_30s_boundary_and_close(void)
{
  board_a_event_buffer_t buffer;
  board_a_event_buffer_status_t status;
  board_a_event_buffer_record_t record;
  board_a_alarm_result_t result;
  uint32_t post_records = 0U;
  uint64_t time_ms;

  board_a_event_buffer_init(&buffer);
  CHECK(push_sample(&buffer, 1U, 0U, 400, 400, 400));
  result = make_result(true, BOARD_A_ALARM_EVENT_RAISED, 3U, 2U, 1000U,
                       BOARD_A_ALARM_WARNING,
                       BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH,
                       BOARD_A_ALARM_PHASE_A);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  drain_buffer(&buffer);

  CHECK(push_sample(&buffer, 3U, 2000U, 400, 400, 400));
  board_a_event_buffer_step(&buffer, 2000U);
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_ACTIVE);
  CHECK(record.time_ms == 2000U);

  result = make_result(true, BOARD_A_ALARM_EVENT_RECOVERED, 3U, 4U, 3000U,
                       BOARD_A_ALARM_NORMAL,
                       BOARD_A_ALARM_REASON_NONE,
                       BOARD_A_ALARM_PHASE_NONE);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_POST);
  CHECK(record.time_ms == 3000U);
  post_records++;

  for (time_ms = 4000U; time_ms <= 32000U; time_ms += 1000U) {
    CHECK(push_sample(&buffer, (uint32_t)(time_ms / 1000U), time_ms,
                      400, 400, 400));
    board_a_event_buffer_step(&buffer, time_ms);
    CHECK(board_a_event_buffer_pull_record(&buffer, &record));
    CHECK(record.phase == BOARD_A_EVENT_PHASE_POST);
    CHECK(record.time_ms == time_ms);
    post_records++;
  }
  CHECK(post_records == 30U);

  board_a_event_buffer_step(&buffer, 32999U);
  CHECK(!board_a_event_buffer_pull_record(&buffer, &record));
  board_a_event_buffer_step(&buffer, 33000U);
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_CLOSE);
  CHECK(record.time_ms == 33000U);
  CHECK((record.flags & BOARD_A_EVENT_FLAG_INCOMPLETE) == 0U);
  CHECK((record.flags & BOARD_A_EVENT_FLAG_FORCED_CLOSE) == 0U);
  board_a_event_buffer_status(&buffer, &status);
  CHECK(!status.event_open);
  board_a_event_buffer_step(&buffer, 34000U);
  CHECK(!board_a_event_buffer_pull_record(&buffer, &record));
}

static void test_ring_wrap_reports_actual_count(void)
{
  board_a_event_buffer_t buffer;
  board_a_event_buffer_status_t status;
  board_a_event_buffer_record_t record;
  board_a_alarm_result_t result;
  uint32_t index;

  board_a_event_buffer_init(&buffer);
  for (index = 1U; index <= 20U; ++index) {
    CHECK(push_sample(&buffer, index, (uint64_t)(index - 1U) * 1000ULL,
                      400, 400, 400));
  }
  board_a_event_buffer_status(&buffer, &status);
  CHECK(status.pre_available == BOARD_A_EVENT_BUFFER_PRE_CAPACITY);
  CHECK(status.pre_wrapped);
  CHECK(!status.pre_short);

  result = make_result(true, BOARD_A_ALARM_EVENT_RAISED, 4U, 21U, 20000U,
                       BOARD_A_ALARM_WARNING,
                       BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH,
                       BOARD_A_ALARM_PHASE_A);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  board_a_event_buffer_status(&buffer, &status);
  CHECK(status.pre_available == BOARD_A_EVENT_BUFFER_PRE_CAPACITY);
  CHECK(status.pre_exported == BOARD_A_EVENT_BUFFER_PRE_CAPACITY);
  CHECK(status.pre_wrapped);

  for (index = 0U; index < BOARD_A_EVENT_BUFFER_PRE_CAPACITY; ++index) {
    CHECK(board_a_event_buffer_pull_record(&buffer, &record));
    CHECK(record.phase == BOARD_A_EVENT_PHASE_PRE);
    CHECK(record.sample_id == (index + 5U));
  }
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_TRIGGER);
  CHECK(record.sample_id == 21U);
  CHECK((record.flags & BOARD_A_EVENT_FLAG_PRE_WRAPPED) != 0U);
}

static void test_level_reason_and_sensor_fault_update(void)
{
  board_a_event_buffer_t buffer;
  board_a_event_buffer_record_t record;
  board_a_alarm_result_t result;

  board_a_event_buffer_init(&buffer);
  CHECK(push_sample(&buffer, 1U, 0U, 400, 400, 400));
  result = make_result(true, BOARD_A_ALARM_EVENT_RAISED, 7U, 2U, 1000U,
                       BOARD_A_ALARM_WARNING,
                       BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH,
                       BOARD_A_ALARM_PHASE_A);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  drain_buffer(&buffer);

  result = make_result(true, BOARD_A_ALARM_EVENT_UPDATED, 7U, 3U, 1500U,
                       BOARD_A_ALARM_CRITICAL,
                       BOARD_A_ALARM_REASON_PHASE_TEMPERATURE_HIGH,
                       BOARD_A_ALARM_PHASE_A);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  CHECK(push_sample(&buffer, 3U, 2000U, 1200, 400, 400));
  board_a_event_buffer_step(&buffer, 2000U);
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_ACTIVE);
  CHECK(record.event_id == 7U);
  CHECK(record.level == BOARD_A_ALARM_CRITICAL);
  CHECK(record.reason == BOARD_A_ALARM_REASON_PHASE_TEMPERATURE_HIGH);
  CHECK((record.flags & BOARD_A_EVENT_FLAG_LEVEL_UPDATED) != 0U);
  CHECK((record.flags & BOARD_A_EVENT_FLAG_REASON_UPDATED) != 0U);

  result = make_result(true, BOARD_A_ALARM_EVENT_UPDATED, 7U, 4U, 2500U,
                       BOARD_A_ALARM_SENSOR_FAULT,
                       BOARD_A_ALARM_REASON_SENSOR_CRC_ERROR,
                       BOARD_A_ALARM_PHASE_A);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  CHECK(push_sample(&buffer, 4U, 3000U, 0, 400, 400));
  board_a_event_buffer_step(&buffer, 3000U);
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.event_id == 7U);
  CHECK(record.level == BOARD_A_ALARM_SENSOR_FAULT);
  CHECK(record.reason == BOARD_A_ALARM_REASON_SENSOR_CRC_ERROR);
  CHECK(record.alarm_phase == BOARD_A_ALARM_PHASE_A);
  CHECK((record.flags & BOARD_A_EVENT_FLAG_SENSOR_FAULT) != 0U);
  CHECK((record.flags & BOARD_A_EVENT_FLAG_REASON_UPDATED) != 0U);
}

static void test_post_retrigger_keeps_event_identity(void)
{
  board_a_event_buffer_t buffer;
  board_a_event_buffer_record_t record;
  board_a_alarm_result_t result;
  board_a_event_buffer_status_t status;

  board_a_event_buffer_init(&buffer);
  result = make_result(true, BOARD_A_ALARM_EVENT_RAISED, 11U, 1U, 0U,
                       BOARD_A_ALARM_WARNING,
                       BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH,
                       BOARD_A_ALARM_PHASE_A);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  drain_buffer(&buffer);

  result = make_result(true, BOARD_A_ALARM_EVENT_RECOVERED, 11U, 2U, 1000U,
                       BOARD_A_ALARM_NORMAL,
                       BOARD_A_ALARM_REASON_NONE,
                       BOARD_A_ALARM_PHASE_NONE);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_POST);
  CHECK(record.event_id == 11U);

  result = make_result(true, BOARD_A_ALARM_EVENT_RAISED, 12U, 3U, 2000U,
                       BOARD_A_ALARM_CRITICAL,
                       BOARD_A_ALARM_REASON_PHASE_TEMPERATURE_HIGH,
                       BOARD_A_ALARM_PHASE_A);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  board_a_event_buffer_status(&buffer, &status);
  CHECK(status.event_open);
  CHECK(status.event_id == 11U);
  board_a_event_buffer_step(&buffer, 2000U);
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_ACTIVE);
  CHECK(record.event_id == 11U);
  CHECK(record.level == BOARD_A_ALARM_CRITICAL);
  CHECK((record.flags & BOARD_A_EVENT_FLAG_POST_RETRIGGER) != 0U);

  result = make_result(true, BOARD_A_ALARM_EVENT_RECOVERED, 12U, 4U, 3000U,
                       BOARD_A_ALARM_NORMAL,
                       BOARD_A_ALARM_REASON_NONE,
                       BOARD_A_ALARM_PHASE_NONE);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_POST);
  CHECK(record.time_ms == 3000U);
  board_a_event_buffer_step(&buffer, 32999U);
  while (board_a_event_buffer_pull_record(&buffer, &record)) {
    CHECK(record.phase == BOARD_A_EVENT_PHASE_POST);
    CHECK(record.time_ms < 33000U);
  }
  board_a_event_buffer_step(&buffer, 33000U);
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_CLOSE);
  CHECK(record.event_id == 11U);
  CHECK(record.time_ms == 33000U);
}

static void test_force_close_and_restart(void)
{
  board_a_event_buffer_t buffer;
  board_a_event_buffer_record_t record;
  board_a_event_buffer_status_t status;
  board_a_alarm_result_t result;

  board_a_event_buffer_init(&buffer);
  result = make_result(true, BOARD_A_ALARM_EVENT_RAISED, 21U, 1U, 0U,
                       BOARD_A_ALARM_WARNING,
                       BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH,
                       BOARD_A_ALARM_PHASE_A);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  drain_buffer(&buffer);
  CHECK(board_a_event_buffer_force_close(&buffer, 500U));
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_CLOSE);
  CHECK(record.time_ms == 500U);
  CHECK((record.flags & BOARD_A_EVENT_FLAG_INCOMPLETE) != 0U);
  CHECK((record.flags & BOARD_A_EVENT_FLAG_FORCED_CLOSE) != 0U);
  board_a_event_buffer_status(&buffer, &status);
  CHECK(!status.event_open);
  CHECK(status.incomplete);

  board_a_event_buffer_init(&buffer);
  board_a_event_buffer_status(&buffer, &status);
  CHECK(!status.event_open);
  CHECK(status.pre_available == 0U);
  CHECK(!status.incomplete);
  result = make_result(true, BOARD_A_ALARM_EVENT_RAISED, 22U, 1U, 1000U,
                       BOARD_A_ALARM_WARNING,
                       BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH,
                       BOARD_A_ALARM_PHASE_A);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_TRIGGER);
  CHECK(record.event_id == 22U);
}

static void test_sample_and_event_id_wrap(void)
{
  board_a_event_buffer_t buffer;
  board_a_event_buffer_record_t record;
  board_a_alarm_result_t result;

  board_a_event_buffer_init(&buffer);
  CHECK(push_sample(&buffer, UINT32_MAX, 0U, 400, 400, 400));
  CHECK(push_sample(&buffer, 0U, 1000U, 400, 400, 400));
  result = make_result(true, BOARD_A_ALARM_EVENT_RAISED, UINT32_MAX, 1U,
                       2000U, BOARD_A_ALARM_WARNING,
                       BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH,
                       BOARD_A_ALARM_PHASE_A);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_PRE);
  CHECK(record.sample_id == UINT32_MAX);
  CHECK(record.event_id == UINT32_MAX);
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_PRE);
  CHECK(record.sample_id == 0U);
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_TRIGGER);
  CHECK(record.event_id == UINT32_MAX);
  CHECK(board_a_event_buffer_force_close(&buffer, 2500U));
  drain_buffer(&buffer);

  board_a_event_buffer_init(&buffer);
  result = make_result(true, BOARD_A_ALARM_EVENT_RAISED, 0U, 0U, 0U,
                       BOARD_A_ALARM_WARNING,
                       BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH,
                       BOARD_A_ALARM_PHASE_A);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(record.phase == BOARD_A_EVENT_PHASE_TRIGGER);
  CHECK(record.event_id == 0U);
  CHECK(record.sample_id == 0U);
}

static void test_invalid_and_time_regression(void)
{
  board_a_event_buffer_t buffer;
  board_a_sensor_snapshot_t snapshot;
  board_a_event_buffer_record_t record;
  board_a_alarm_result_t result;

  board_a_event_buffer_init(&buffer);
  CHECK(!board_a_event_buffer_push_snapshot(NULL, &snapshot));
  CHECK(!board_a_event_buffer_note_alarm_result(&buffer, NULL));
  CHECK(!board_a_event_buffer_pull_record(&buffer, &record));
  CHECK(!board_a_event_buffer_force_close(&buffer, 0U));

  snapshot_init(&snapshot, 1U, 1000000ULL);
  snapshot_set_all(&snapshot, 400, 400, 400);
  CHECK(board_a_event_buffer_push_snapshot(&buffer, &snapshot));
  snapshot_init(&snapshot, 2U, 500000ULL);
  snapshot_set_all(&snapshot, 400, 400, 400);
  CHECK(!board_a_event_buffer_push_snapshot(&buffer, &snapshot));

  result = make_result(false, BOARD_A_ALARM_EVENT_NONE, 0U, 1U, 1000U,
                       BOARD_A_ALARM_NORMAL,
                       BOARD_A_ALARM_REASON_NONE,
                       BOARD_A_ALARM_PHASE_NONE);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  CHECK(!board_a_event_buffer_force_close(&buffer, 0U));
}

static void test_queue_drop_marks_incomplete(void)
{
  board_a_event_buffer_t buffer;
  board_a_event_buffer_status_t status;

  board_a_event_buffer_init(&buffer);
  board_a_event_buffer_note_queue_drop(&buffer);
  board_a_event_buffer_status(&buffer, &status);
  CHECK(status.event_dropped == 1U);
  CHECK(status.incomplete);
  CHECK((status.flags & BOARD_A_EVENT_FLAG_INCOMPLETE) != 0U);
  board_a_event_buffer_note_queue_drop(&buffer);
  board_a_event_buffer_status(&buffer, &status);
  CHECK(status.event_dropped == 2U);
}

static void test_queue_retain_and_forced_close(void)
{
  board_a_event_buffer_t buffer;
  board_a_event_buffer_status_t status;
  board_a_event_buffer_record_t record;
  board_a_event_buffer_record_t last;
  board_a_sensor_snapshot_t snapshot;
  board_a_alarm_result_t result;
  uint32_t index;
  uint32_t pulled = 0U;

  board_a_event_buffer_init(&buffer);
  snapshot_init(&snapshot, 1U, 1000000ULL);
  snapshot_set_all(&snapshot, 400, 400, 400);
  CHECK(board_a_event_buffer_push_snapshot(&buffer, &snapshot));
  result = make_result(true, BOARD_A_ALARM_EVENT_RAISED, 41U, 1U, 1000U,
                       BOARD_A_ALARM_WARNING,
                       BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH,
                       BOARD_A_ALARM_PHASE_A);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  CHECK(board_a_event_buffer_peek_record(&buffer, &record));
  board_a_event_buffer_status(&buffer, &status);
  CHECK(status.queued_records == 1U);
  CHECK(board_a_event_buffer_pull_record(&buffer, &record));

  for (index = 0U; index < 200U; ++index) {
    board_a_event_buffer_step(&buffer, 2000U + (uint64_t)index * 1000ULL);
  }
  board_a_event_buffer_status(&buffer, &status);
  CHECK(status.queued_records == BOARD_A_EVENT_BUFFER_OUTPUT_CAPACITY);
  CHECK(board_a_event_buffer_force_close(&buffer, 300000U));
  board_a_event_buffer_status(&buffer, &status);
  CHECK(status.event_dropped >= 1U);
  CHECK(status.incomplete);

  memset(&last, 0, sizeof(last));
  while (board_a_event_buffer_pull_record(&buffer, &record)) {
    last = record;
    pulled++;
  }
  CHECK(pulled == BOARD_A_EVENT_BUFFER_OUTPUT_CAPACITY);
  CHECK(last.phase == BOARD_A_EVENT_PHASE_CLOSE);
  CHECK((last.flags & BOARD_A_EVENT_FLAG_INCOMPLETE) != 0U);
}

static void test_restart_drops_ram_event_state(void)
{
  board_a_event_buffer_t buffer;
  board_a_event_buffer_status_t status;
  board_a_event_buffer_record_t record;
  board_a_alarm_result_t result;

  /*
   * This records the current architectural boundary: an unfinished event
   * exists only in RAM, so a restart cannot synthesize a persisted CLOSE.
   */
  board_a_event_buffer_init(&buffer);
  result = make_result(true, BOARD_A_ALARM_EVENT_RAISED, 31U, 1U, 0U,
                       BOARD_A_ALARM_WARNING,
                       BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH,
                       BOARD_A_ALARM_PHASE_A);
  CHECK(board_a_event_buffer_note_alarm_result(&buffer, &result));
  board_a_event_buffer_status(&buffer, &status);
  CHECK(status.event_open);

  board_a_event_buffer_init(&buffer);
  board_a_event_buffer_status(&buffer, &status);
  CHECK(!status.event_open);
  CHECK(status.event_id == 0U);
  CHECK(status.queued_records == 0U);
  CHECK(status.flags == 0U);
  CHECK(!board_a_event_buffer_pull_record(&buffer, &record));
}

int main(void)
{
  test_pre_boundary_and_short();
  test_post_30s_boundary_and_close();
  test_ring_wrap_reports_actual_count();
  test_level_reason_and_sensor_fault_update();
  test_post_retrigger_keeps_event_identity();
  test_force_close_and_restart();
  test_sample_and_event_id_wrap();
  test_invalid_and_time_regression();
  test_queue_drop_marks_incomplete();
  test_queue_retain_and_forced_close();
  test_restart_drops_ram_event_state();

  printf("board_a_event_buffer host tests: %u checks, %u failures\n",
         checks, failures);
  return (failures == 0U) ? 0 : 1;
}
