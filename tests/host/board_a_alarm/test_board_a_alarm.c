#include <stdio.h>
#include <string.h>

#include "board_a_alarm.h"

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

static void snapshot_set_phase(board_a_sensor_snapshot_t *snapshot,
                               uint8_t phase,
                               int16_t temperature_x16,
                               uint16_t quality,
                               bool has_value)
{
  snapshot->sensors[phase].temperature_x16 = temperature_x16;
  snapshot->sensors[phase].quality = quality;
  snapshot->sensors[phase].has_value = has_value;
  if (has_value) {
    snapshot->valid_mask |= (uint16_t)(1U << phase);
  }
}

static void snapshot_set_all(board_a_sensor_snapshot_t *snapshot,
                             int16_t a_x16,
                             int16_t b_x16,
                             int16_t c_x16)
{
  snapshot_set_phase(snapshot, 0U, a_x16, BOARD_A_QUALITY_OK, true);
  snapshot_set_phase(snapshot, 1U, b_x16, BOARD_A_QUALITY_OK, true);
  snapshot_set_phase(snapshot, 2U, c_x16, BOARD_A_QUALITY_OK, true);
}

static board_a_alarm_result_t run_sample(board_a_alarm_t *alarm,
                                         uint32_t sample_id,
                                         uint64_t sample_time_us,
                                         int16_t a_x16,
                                         int16_t b_x16,
                                         int16_t c_x16)
{
  board_a_sensor_snapshot_t snapshot;
  board_a_alarm_result_t result;

  snapshot_init(&snapshot, sample_id, sample_time_us);
  snapshot_set_all(&snapshot, a_x16, b_x16, c_x16);
  CHECK(board_a_alarm_update(alarm, &snapshot, &result));
  return result;
}

static void test_config_validation(void)
{
  board_a_alarm_t alarm;
  board_a_alarm_config_t config;
  board_a_alarm_config_t invalid;

  board_a_alarm_default_config(&config);
  CHECK(board_a_alarm_validate_config(&config));
  CHECK(board_a_alarm_config_equal(&config, &config));
  CHECK(!board_a_alarm_config_equal(&config, NULL));
  board_a_alarm_init(&alarm);
  CHECK(board_a_alarm_set_config(&alarm, &config));
  CHECK(alarm.state.buzzer_enable);

  invalid = config;
  invalid.phase_warning_x16 = invalid.phase_notice_x16;
  CHECK(!board_a_alarm_validate_config(&invalid));
  CHECK(!board_a_alarm_set_config(&alarm, &invalid));
  CHECK(alarm.config.phase_warning_x16 == config.phase_warning_x16);

  invalid = config;
  invalid.assert_samples = 0U;
  CHECK(!board_a_alarm_validate_config(&invalid));
  invalid = config;
  invalid.clear_samples = 0U;
  CHECK(!board_a_alarm_validate_config(&invalid));
  invalid = config;
  invalid.rise_window_min_ms = 0U;
  CHECK(!board_a_alarm_validate_config(&invalid));
  invalid = config;
  invalid.buzzer_enable = 2U;
  CHECK(!board_a_alarm_validate_config(&invalid));
  invalid = config;
  invalid.clear_samples++;
  CHECK(!board_a_alarm_config_equal(&config, &invalid));
  invalid = config;
  invalid.buzzer_enable = 0U;
  CHECK(board_a_alarm_validate_config(&invalid));
  CHECK(board_a_alarm_set_config(&alarm, &invalid));
  CHECK(!alarm.state.buzzer_enable);
}

static void test_config_field_equality(void)
{
  board_a_alarm_config_t left;
  board_a_alarm_config_t right;

  memset(&left, 0xA5, sizeof(left));
  memset(&right, 0x5A, sizeof(right));
  board_a_alarm_default_config(&left);
  board_a_alarm_default_config(&right);
  CHECK(board_a_alarm_config_equal(&left, &right));
  CHECK(memcmp(&left, &right, sizeof(left)) != 0);

  right.assert_samples++;
  CHECK(!board_a_alarm_config_equal(&left, &right));
  right = left;
  right.buzzer_enable = 0U;
  CHECK(!board_a_alarm_config_equal(&left, &right));
  CHECK(!board_a_alarm_config_equal(NULL, &right));
  CHECK(!board_a_alarm_config_equal(&left, NULL));
}

static void test_normal_and_confirmation(void)
{
  board_a_alarm_t alarm;
  board_a_alarm_result_t result;

  board_a_alarm_init(&alarm);
  result = run_sample(&alarm, 1U, 0U, 400, 400, 400);
  CHECK(result.state.level == BOARD_A_ALARM_NORMAL);
  CHECK(!result.event);
  result = run_sample(&alarm, 2U, 100000U, 480, 400, 400);
  CHECK(result.state.level == BOARD_A_ALARM_NORMAL);
  CHECK(!result.event);
  result = run_sample(&alarm, 3U, 200000U, 480, 400, 400);
  CHECK(result.state.level == BOARD_A_ALARM_NORMAL);
  result = run_sample(&alarm, 4U, 300000U, 480, 400, 400);
  CHECK(result.state.level == BOARD_A_ALARM_NOTICE);
  CHECK(result.state.reason == BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH);
  CHECK(result.state.trigger_phase == BOARD_A_ALARM_PHASE_A);
  CHECK(result.state.delta_valid);
  CHECK(result.state.maximum_delta_x16 == 80);
  CHECK(result.state.notice_count == 1U);
  CHECK(result.event);
  CHECK(result.event_type == BOARD_A_ALARM_EVENT_RAISED);
  CHECK(result.event_id == 1U);
  result = run_sample(&alarm, 5U, 400000U, 480, 400, 400);
  CHECK(!result.event);
  CHECK(result.event_id == 1U);
  CHECK(result.state.buzzer_enable);
}

static void test_delta_warning_and_phase_pair(void)
{
  board_a_alarm_t alarm;
  board_a_alarm_result_t result;

  board_a_alarm_init(&alarm);
  (void)run_sample(&alarm, 1U, 0U, 400, 400, 400);
  result = run_sample(&alarm, 2U, 100000U, 560, 400, 400);
  CHECK(result.state.level == BOARD_A_ALARM_NORMAL);
  result = run_sample(&alarm, 3U, 200000U, 560, 400, 400);
  CHECK(result.state.level == BOARD_A_ALARM_NORMAL);
  result = run_sample(&alarm, 4U, 300000U, 560, 400, 400);
  CHECK(result.state.level == BOARD_A_ALARM_WARNING);
  CHECK(result.state.reason == BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH);
  CHECK(result.state.hottest_phase == BOARD_A_ALARM_PHASE_A);
  CHECK(result.state.coldest_phase == BOARD_A_ALARM_PHASE_B);
  CHECK(result.state.maximum_delta_x16 == 160);
}

static void test_critical_temperature(void)
{
  board_a_alarm_t alarm;
  board_a_alarm_result_t result;

  board_a_alarm_init(&alarm);
  (void)run_sample(&alarm, 1U, 0U, 400, 400, 400);
  result = run_sample(&alarm, 2U, 100000U, 1200, 400, 400);
  CHECK(result.state.level == BOARD_A_ALARM_NORMAL);
  result = run_sample(&alarm, 3U, 200000U, 1200, 400, 400);
  CHECK(result.state.level == BOARD_A_ALARM_NORMAL);
  result = run_sample(&alarm, 4U, 300000U, 1200, 400, 400);
  CHECK(result.state.level == BOARD_A_ALARM_CRITICAL);
  CHECK(result.state.reason ==
        BOARD_A_ALARM_REASON_PHASE_TEMPERATURE_HIGH);
  CHECK(result.state.trigger_phase == BOARD_A_ALARM_PHASE_A);
  CHECK(result.state.hottest_temperature_x16 == 1200);
  CHECK(result.state.critical_count == 1U);
}

static void test_delta_critical(void)
{
  board_a_alarm_t alarm;
  board_a_alarm_result_t result;

  board_a_alarm_init(&alarm);
  (void)run_sample(&alarm, 1U, 0U, 400, 400, 400);
  result = run_sample(&alarm, 2U, 100000U, 640, 400, 400);
  CHECK(result.state.level == BOARD_A_ALARM_NORMAL);
  result = run_sample(&alarm, 3U, 200000U, 640, 400, 400);
  CHECK(result.state.level == BOARD_A_ALARM_NORMAL);
  result = run_sample(&alarm, 4U, 300000U, 640, 400, 400);
  CHECK(result.state.level == BOARD_A_ALARM_CRITICAL);
  CHECK(result.state.reason == BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH);
  CHECK(result.state.maximum_delta_x16 == 240);
}

static void test_hysteresis_and_recovery(void)
{
  board_a_alarm_t alarm;
  board_a_alarm_result_t result;
  uint32_t sample_id = 1U;
  uint64_t time_us = 0U;
  uint8_t index;

  board_a_alarm_init(&alarm);
  result = run_sample(&alarm, sample_id++, time_us, 1280, 1200, 1200);
  time_us += 100000U;
  for (index = 0U; index < 2U; ++index) {
    result = run_sample(&alarm, sample_id++, time_us, 1280, 1200, 1200);
    time_us += 100000U;
  }
  CHECK(result.state.level == BOARD_A_ALARM_CRITICAL);

  for (index = 0U; index < 4U; ++index) {
    result = run_sample(&alarm, sample_id++, time_us, 1184, 1184, 1184);
    time_us += 100000U;
  }
  CHECK(result.state.level == BOARD_A_ALARM_CRITICAL);
  result = run_sample(&alarm, sample_id++, time_us, 1152, 1152, 1152);
  time_us += 100000U;
  CHECK(result.state.level == BOARD_A_ALARM_CRITICAL);
  for (index = 0U; index < 4U; ++index) {
    result = run_sample(&alarm, sample_id++, time_us, 1152, 1152, 1152);
    time_us += 100000U;
  }
  CHECK(result.state.level == BOARD_A_ALARM_WARNING);
  CHECK(result.event_type == BOARD_A_ALARM_EVENT_UPDATED);

  for (index = 0U; index < 4U; ++index) {
    result = run_sample(&alarm, sample_id++, time_us, 784, 784, 784);
    time_us += 100000U;
  }
  CHECK(result.state.level == BOARD_A_ALARM_WARNING);
  result = run_sample(&alarm, sample_id++, time_us, 784, 784, 784);
  CHECK(result.state.level == BOARD_A_ALARM_NORMAL);
  CHECK(result.event);
  CHECK(result.event_type == BOARD_A_ALARM_EVENT_RECOVERED);
}

static void test_rise_rate_and_window(void)
{
  board_a_alarm_t alarm;
  board_a_alarm_result_t result;
  uint32_t sample_id = 1U;
  uint64_t time_us = 0U;

  board_a_alarm_init(&alarm);
  result = run_sample(&alarm, sample_id++, time_us, 320, 336, 352);
  CHECK(result.state.level == BOARD_A_ALARM_NORMAL);
  time_us += 60000000U;
  result = run_sample(&alarm, sample_id++, time_us, 480, 496, 512);
  CHECK(!result.event);
  time_us += 60000000U;
  result = run_sample(&alarm, sample_id++, time_us, 640, 656, 672);
  CHECK(!result.event);
  time_us += 60000000U;
  result = run_sample(&alarm, sample_id++, time_us, 800, 816, 832);
  CHECK(result.state.level == BOARD_A_ALARM_WARNING);
  CHECK(result.state.reason == BOARD_A_ALARM_REASON_RISE_RATE_HIGH);
  CHECK(result.state.maximum_rise_x16_per_min >= 160);
  CHECK(result.state.rise_valid_mask == 0x0007U);

  board_a_alarm_init(&alarm);
  result = run_sample(&alarm, 1U, 0U, 320, 321, 322);
  result = run_sample(&alarm, 2U, 100000U, 480, 481, 482);
  result = run_sample(&alarm, 3U, 200000U, 480, 481, 482);
  result = run_sample(&alarm, 4U, 300000U, 480, 481, 482);
  CHECK(result.state.level == BOARD_A_ALARM_NORMAL);
  CHECK(result.state.rise_valid_mask == 0U);
}

static void test_sensor_fault_priority_and_ack(void)
{
  board_a_alarm_t alarm;
  board_a_sensor_snapshot_t snapshot;
  board_a_alarm_result_t result;

  board_a_alarm_init(&alarm);
  (void)run_sample(&alarm, 1U, 0U, 400, 400, 400);
  snapshot_init(&snapshot, 2U, 100000U);
  snapshot_set_phase(&snapshot, 0U, 1280, BOARD_A_QUALITY_CRC_ERROR, false);
  snapshot_set_phase(&snapshot, 1U, 1280, BOARD_A_QUALITY_OK, true);
  snapshot_set_phase(&snapshot, 2U, 1280, BOARD_A_QUALITY_OK, true);
  CHECK(board_a_alarm_update(&alarm, &snapshot, &result));
  CHECK(result.state.level == BOARD_A_ALARM_SENSOR_FAULT);
  CHECK(result.state.reason == BOARD_A_ALARM_REASON_SENSOR_CRC_ERROR);
  CHECK(result.state.trigger_phase == BOARD_A_ALARM_PHASE_A);
  CHECK(result.state.fault_mask == 0x0001U);
  CHECK(result.state.delta_valid);
  CHECK(result.state.maximum_delta_x16 == 0);
  board_a_alarm_ack(&alarm);
  CHECK(alarm.state.acknowledged);
  CHECK(alarm.state.level == BOARD_A_ALARM_SENSOR_FAULT);
}

static void test_all_phase_faults_beat_large_delta(void)
{
  static const board_a_alarm_phase_t expected_phase[3] = {
    BOARD_A_ALARM_PHASE_A,
    BOARD_A_ALARM_PHASE_B,
    BOARD_A_ALARM_PHASE_C
  };
  uint8_t fault_phase;

  for (fault_phase = 0U; fault_phase < 3U; ++fault_phase) {
    board_a_alarm_t alarm;
    board_a_sensor_snapshot_t snapshot;
    board_a_alarm_result_t result;
    uint8_t phase;
    bool heated = false;

    board_a_alarm_init(&alarm);
    snapshot_init(&snapshot, 1U, 0U);
    for (phase = 0U; phase < 3U; ++phase) {
      if (phase == fault_phase) {
        snapshot_set_phase(
            &snapshot, phase, 0, BOARD_A_QUALITY_CRC_ERROR, false);
      } else {
        snapshot_set_phase(
            &snapshot, phase, heated ? 400 : 720,
            BOARD_A_QUALITY_OK, true);
        heated = true;
      }
    }
    CHECK(board_a_alarm_update(&alarm, &snapshot, &result));
    CHECK(result.state.level == BOARD_A_ALARM_SENSOR_FAULT);
    CHECK(result.state.reason == BOARD_A_ALARM_REASON_SENSOR_CRC_ERROR);
    CHECK(result.state.trigger_phase == expected_phase[fault_phase]);
    CHECK(result.state.fault_mask ==
          (uint16_t)(1U << fault_phase));
    CHECK(result.state.delta_valid);
    CHECK(result.state.maximum_delta_x16 == 320);
    CHECK(result.state.display_mask ==
          (uint16_t)(0x0007U & (uint16_t)~(1U << fault_phase)));
  }
}

static void test_configured_clear_samples_and_reevaluation(void)
{
  board_a_alarm_t alarm;
  board_a_sensor_snapshot_t snapshot;
  board_a_alarm_config_t config;
  board_a_alarm_result_t result;
  uint32_t sample_id = 1U;
  uint64_t time_us = 0U;
  uint8_t index;

  board_a_alarm_default_config(&config);
  config.assert_samples = 2U;
  config.clear_samples = 3U;
  board_a_alarm_init(&alarm);
  CHECK(board_a_alarm_set_config(&alarm, &config));

  (void)run_sample(&alarm, sample_id++, time_us, 400, 400, 400);
  time_us += 100000U;
  snapshot_init(&snapshot, sample_id++, time_us);
  snapshot_set_phase(&snapshot, 0U, 0, BOARD_A_QUALITY_CRC_ERROR, false);
  snapshot_set_phase(&snapshot, 1U, 400, BOARD_A_QUALITY_OK, true);
  snapshot_set_phase(&snapshot, 2U, 400, BOARD_A_QUALITY_OK, true);
  CHECK(board_a_alarm_update(&alarm, &snapshot, &result));
  CHECK(result.state.level == BOARD_A_ALARM_SENSOR_FAULT);
  time_us += 100000U;

  for (index = 0U; index < 2U; ++index) {
    result = run_sample(&alarm, sample_id++, time_us, 400, 400, 400);
    time_us += 100000U;
    CHECK(result.state.level == BOARD_A_ALARM_SENSOR_FAULT);
  }
  result = run_sample(&alarm, sample_id++, time_us, 400, 400, 400);
  time_us += 100000U;
  CHECK(result.state.level == BOARD_A_ALARM_NORMAL);

  result = run_sample(&alarm, sample_id++, time_us, 800, 400, 400);
  time_us += 100000U;
  CHECK(result.state.level == BOARD_A_ALARM_NORMAL);
  result = run_sample(&alarm, sample_id++, time_us, 800, 400, 400);
  CHECK(result.state.level == BOARD_A_ALARM_CRITICAL);
  CHECK(result.state.reason == BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH);
}

static void test_threshold_strictly_above_boundaries(void)
{
  board_a_alarm_t alarm;
  board_a_alarm_result_t result;
  uint8_t index;

  board_a_alarm_init(&alarm);
  (void)run_sample(&alarm, 1U, 0U, 400, 400, 400);
  for (index = 0U; index < 3U; ++index) {
    result = run_sample(
        &alarm, (uint32_t)(2U + index), (uint64_t)(index + 1U) * 100000U,
        576, 400, 400);
  }
  CHECK(result.state.level == BOARD_A_ALARM_WARNING);
  CHECK(result.state.maximum_delta_x16 == 176);

  board_a_alarm_init(&alarm);
  (void)run_sample(&alarm, 1U, 0U, 400, 400, 400);
  for (index = 0U; index < 3U; ++index) {
    result = run_sample(
        &alarm, (uint32_t)(2U + index), (uint64_t)(index + 1U) * 100000U,
        1216, 1216, 1216);
  }
  CHECK(result.state.level == BOARD_A_ALARM_CRITICAL);
  CHECK(result.state.reason ==
        BOARD_A_ALARM_REASON_PHASE_TEMPERATURE_HIGH);
}

static void test_single_phase_delta_invalid(void)
{
  board_a_alarm_t alarm;
  board_a_sensor_snapshot_t snapshot;
  board_a_alarm_result_t result;

  board_a_alarm_init(&alarm);
  snapshot_init(&snapshot, 1U, 0U);
  snapshot_set_phase(&snapshot, 0U, 400, BOARD_A_QUALITY_OK, true);
  snapshot_set_phase(&snapshot, 1U, 0, BOARD_A_QUALITY_OK, false);
  snapshot_set_phase(&snapshot, 2U, 0, BOARD_A_QUALITY_OK, false);
  CHECK(board_a_alarm_update(&alarm, &snapshot, &result));
  CHECK(result.state.level == BOARD_A_ALARM_SENSOR_FAULT);
  CHECK(result.state.reason ==
        BOARD_A_ALARM_REASON_SENSOR_NOT_PRESENT);
  CHECK(!result.state.delta_valid);
  CHECK(result.state.maximum_delta_x16 == 0);
  CHECK(result.state.hottest_phase == BOARD_A_ALARM_PHASE_A);
  CHECK(result.state.coldest_phase == BOARD_A_ALARM_PHASE_NONE);
}

static void test_ack_reset_on_escalation_and_duration(void)
{
  board_a_alarm_t alarm;
  board_a_alarm_result_t result;
  uint32_t sample_id = 1U;
  uint64_t time_us = 0U;
  uint8_t index;

  board_a_alarm_init(&alarm);
  result = run_sample(&alarm, sample_id++, time_us, 880, 880, 880);
  time_us += 100000U;
  for (index = 0U; index < 2U; ++index) {
    result = run_sample(&alarm, sample_id++, time_us, 880, 880, 880);
    time_us += 100000U;
  }
  CHECK(result.state.level == BOARD_A_ALARM_WARNING);
  board_a_alarm_ack(&alarm);
  CHECK(alarm.state.acknowledged);
  time_us += 3000000U;
  result = run_sample(&alarm, sample_id++, time_us, 880, 880, 880);
  time_us += 100000U;
  CHECK(result.state.duration_sec >= 3U);
  CHECK(result.state.acknowledged);

  result = run_sample(&alarm, sample_id++, time_us, 1200, 1200, 1200);
  time_us += 100000U;
  CHECK(result.state.level == BOARD_A_ALARM_WARNING);
  result = run_sample(&alarm, sample_id++, time_us, 1200, 1200, 1200);
  time_us += 100000U;
  CHECK(result.state.level == BOARD_A_ALARM_WARNING);
  result = run_sample(&alarm, sample_id++, time_us, 1200, 1200, 1200);
  CHECK(result.state.level == BOARD_A_ALARM_CRITICAL);
  CHECK(!result.state.acknowledged);
  CHECK(result.event);
  CHECK(result.event_type == BOARD_A_ALARM_EVENT_UPDATED);
}

static void test_fault_recovery_and_unknown_reset(void)
{
  board_a_alarm_t alarm;
  board_a_sensor_snapshot_t snapshot;
  board_a_alarm_result_t result;
  uint32_t sample_id = 1U;
  uint64_t time_us = 0U;
  uint8_t index;

  board_a_alarm_init(&alarm);
  (void)run_sample(&alarm, sample_id++, time_us, 400, 400, 400);
  time_us += 100000U;
  snapshot_init(&snapshot, sample_id++, time_us);
  snapshot_set_phase(&snapshot, 0U, 400, BOARD_A_QUALITY_CRC_ERROR, false);
  snapshot_set_phase(&snapshot, 1U, 400, BOARD_A_QUALITY_OK, true);
  snapshot_set_phase(&snapshot, 2U, 400, BOARD_A_QUALITY_OK, true);
  CHECK(board_a_alarm_update(&alarm, &snapshot, &result));
  CHECK(result.state.level == BOARD_A_ALARM_SENSOR_FAULT);
  time_us += 100000U;

  for (index = 0U; index < 5U; ++index) {
    result = run_sample(&alarm, sample_id++, time_us, 400, 400, 400);
    time_us += 100000U;
  }
  CHECK(result.state.level == BOARD_A_ALARM_NORMAL);
  board_a_alarm_force_unknown(&alarm);
  CHECK(alarm.state.level == BOARD_A_ALARM_UNKNOWN);
  CHECK(!alarm.state.valid);
  CHECK(alarm.state.trigger_phase == BOARD_A_ALARM_PHASE_NONE);
  CHECK(alarm.state.hottest_phase == BOARD_A_ALARM_PHASE_NONE);
  CHECK(alarm.state.coldest_phase == BOARD_A_ALARM_PHASE_NONE);
  CHECK(!alarm.state.delta_valid);
  CHECK(alarm.state.maximum_delta_x16 == 0);
}

static void test_sample_identity_and_time_reset(void)
{
  board_a_alarm_t alarm;
  board_a_sensor_snapshot_t snapshot;
  board_a_alarm_result_t result;

  board_a_alarm_init(&alarm);
  (void)run_sample(&alarm, 0xFFFFFFFFU, 100000U, 400, 400, 400);
  (void)run_sample(&alarm, 0U, 200000U, 400, 400, 400);
  snapshot_init(&snapshot, 0U, 300000U);
  snapshot_set_all(&snapshot, 400, 400, 400);
  CHECK(!board_a_alarm_update(&alarm, &snapshot, &result));
  snapshot_init(&snapshot, 1U, 200000U);
  snapshot_set_all(&snapshot, 400, 400, 400);
  CHECK(board_a_alarm_update(&alarm, &snapshot, &result));
  CHECK(alarm.history_count == 1U);
}

int main(void)
{
  test_config_validation();
  test_config_field_equality();
  test_normal_and_confirmation();
  test_delta_warning_and_phase_pair();
  test_critical_temperature();
  test_delta_critical();
  test_hysteresis_and_recovery();
  test_rise_rate_and_window();
  test_sensor_fault_priority_and_ack();
  test_all_phase_faults_beat_large_delta();
  test_configured_clear_samples_and_reevaluation();
  test_threshold_strictly_above_boundaries();
  test_single_phase_delta_invalid();
  test_ack_reset_on_escalation_and_duration();
  test_fault_recovery_and_unknown_reset();
  test_sample_identity_and_time_reset();

  printf("board_a_alarm host tests: %u checks, %u failures\n",
         checks, failures);
  return (failures == 0U) ? 0 : 1;
}
