#include "board_a_alarm.h"

#include <limits.h>
#include <string.h>

#define BOARD_A_ALARM_MIN_TEMPERATURE_X16 (-880)
#define BOARD_A_ALARM_MAX_TEMPERATURE_X16 2000
#define BOARD_A_ALARM_MAX_ASSERT_SAMPLES 100U
#define BOARD_A_ALARM_MAX_CLEAR_SAMPLES 255U
#define BOARD_A_ALARM_MAX_RISE_WINDOW_MS 3600000U

typedef struct {
  board_a_alarm_level_t level;
  board_a_alarm_reason_t reason;
  board_a_alarm_phase_t phase;
} board_a_alarm_candidate_t;

static board_a_alarm_phase_t phase_from_index(uint8_t index)
{
  switch (index) {
    case 0U:
      return BOARD_A_ALARM_PHASE_A;
    case 1U:
      return BOARD_A_ALARM_PHASE_B;
    case 2U:
      return BOARD_A_ALARM_PHASE_C;
    default:
      return BOARD_A_ALARM_PHASE_NONE;
  }
}

static uint8_t index_from_phase(board_a_alarm_phase_t phase)
{
  if ((phase < BOARD_A_ALARM_PHASE_A) ||
      (phase > BOARD_A_ALARM_PHASE_C)) {
    return BOARD_A_ALARM_PHASE_COUNT;
  }
  return (uint8_t)(phase - BOARD_A_ALARM_PHASE_A);
}

static bool quality_is_displayable(uint16_t quality)
{
  return (quality == BOARD_A_QUALITY_OK) ||
      (quality == BOARD_A_QUALITY_STALE);
}

static bool quality_is_ok(uint16_t quality)
{
  return quality == BOARD_A_QUALITY_OK;
}

static board_a_alarm_reason_t fault_reason_for_quality(uint16_t quality)
{
  switch (quality) {
    case BOARD_A_QUALITY_TIMEOUT:
      return BOARD_A_ALARM_REASON_SENSOR_TIMEOUT;
    case BOARD_A_QUALITY_CRC_ERROR:
      return BOARD_A_ALARM_REASON_SENSOR_CRC_ERROR;
    case BOARD_A_QUALITY_RANGE_ERROR:
      return BOARD_A_ALARM_REASON_SENSOR_RANGE_ERROR;
    case BOARD_A_QUALITY_NOT_PRESENT:
    case BOARD_A_QUALITY_UNAVAILABLE:
    default:
      return BOARD_A_ALARM_REASON_SENSOR_NOT_PRESENT;
  }
}

static uint8_t reason_priority(board_a_alarm_reason_t reason)
{
  switch (reason) {
    case BOARD_A_ALARM_REASON_SENSOR_CRC_ERROR:
      return 4U;
    case BOARD_A_ALARM_REASON_SENSOR_RANGE_ERROR:
      return 3U;
    case BOARD_A_ALARM_REASON_SENSOR_TIMEOUT:
      return 2U;
    case BOARD_A_ALARM_REASON_SENSOR_NOT_PRESENT:
      return 1U;
    default:
      return 0U;
  }
}

static bool candidate_equal(const board_a_alarm_candidate_t *left,
                            const board_a_alarm_candidate_t *right)
{
  return (left->level == right->level) &&
      (left->reason == right->reason) &&
      (left->phase == right->phase);
}

static bool level_is_metric_alarm(board_a_alarm_level_t level)
{
  return (level == BOARD_A_ALARM_NOTICE) ||
      (level == BOARD_A_ALARM_WARNING) ||
      (level == BOARD_A_ALARM_CRITICAL);
}

static int16_t metric_threshold(
    const board_a_alarm_config_t *config,
    board_a_alarm_reason_t reason,
    board_a_alarm_level_t level)
{
  if (reason == BOARD_A_ALARM_REASON_PHASE_TEMPERATURE_HIGH) {
    switch (level) {
      case BOARD_A_ALARM_NOTICE:
        return config->phase_notice_x16;
      case BOARD_A_ALARM_WARNING:
        return config->phase_warning_x16;
      case BOARD_A_ALARM_CRITICAL:
        return config->phase_critical_x16;
      default:
        break;
    }
  } else if (reason == BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH) {
    switch (level) {
      case BOARD_A_ALARM_NOTICE:
        return config->delta_notice_x16;
      case BOARD_A_ALARM_WARNING:
        return config->delta_warning_x16;
      case BOARD_A_ALARM_CRITICAL:
        return config->delta_critical_x16;
      default:
        break;
    }
  } else if (reason == BOARD_A_ALARM_REASON_RISE_RATE_HIGH) {
    switch (level) {
      case BOARD_A_ALARM_NOTICE:
        return config->rise_notice_x16_per_min;
      case BOARD_A_ALARM_WARNING:
        return config->rise_warning_x16_per_min;
      case BOARD_A_ALARM_CRITICAL:
        return config->rise_critical_x16_per_min;
      default:
        break;
    }
  }
  return INT16_MAX;
}

static board_a_alarm_level_t threshold_level(int16_t value,
                                             int16_t notice,
                                             int16_t warning,
                                             int16_t critical)
{
  if (value >= critical) {
    return BOARD_A_ALARM_CRITICAL;
  }
  if (value >= warning) {
    return BOARD_A_ALARM_WARNING;
  }
  if (value >= notice) {
    return BOARD_A_ALARM_NOTICE;
  }
  return BOARD_A_ALARM_NORMAL;
}

static void consider_candidate(board_a_alarm_candidate_t *selected,
                               board_a_alarm_level_t level,
                               board_a_alarm_reason_t reason,
                               board_a_alarm_phase_t phase)
{
  if (level > selected->level) {
    selected->level = level;
    selected->reason = reason;
    selected->phase = phase;
  }
}

static void history_push(board_a_alarm_t *alarm,
                         const board_a_sensor_snapshot_t *snapshot)
{
  board_a_alarm_history_sample_t *sample;
  uint8_t phase;

  sample = &alarm->history[alarm->history_next];
  memset(sample, 0, sizeof(*sample));
  sample->sample_id = snapshot->sample_id;
  sample->time_us = snapshot->sample_time_us;
  for (phase = 0U; phase < BOARD_A_ALARM_PHASE_COUNT; ++phase) {
    const board_a_sensor_sample_t *source = &snapshot->sensors[phase];

    sample->valid[phase] =
        source->has_value && quality_is_ok(source->quality);
    sample->temperature_x16[phase] = source->temperature_x16;
  }
  alarm->history_next =
      (uint8_t)((alarm->history_next + 1U) %
                BOARD_A_ALARM_RISE_HISTORY_CAPACITY);
  if (alarm->history_count < BOARD_A_ALARM_RISE_HISTORY_CAPACITY) {
    alarm->history_count++;
  }
}

static const board_a_alarm_history_sample_t *history_at_age(
    const board_a_alarm_t *alarm, uint8_t age)
{
  uint8_t index;

  if (age >= alarm->history_count) {
    return NULL;
  }
  index = (uint8_t)((alarm->history_next +
                     BOARD_A_ALARM_RISE_HISTORY_CAPACITY - 1U - age) %
                    BOARD_A_ALARM_RISE_HISTORY_CAPACITY);
  return &alarm->history[index];
}

static void compute_rise(board_a_alarm_t *alarm,
                         board_a_alarm_state_t *state)
{
  uint16_t window_samples = alarm->config.rise_window_samples;
  uint8_t phase;

  state->maximum_rise_x16_per_min = 0;
  state->maximum_rise_phase = BOARD_A_ALARM_PHASE_NONE;
  state->rise_valid_mask = 0U;
  if (window_samples > alarm->history_count) {
    window_samples = alarm->history_count;
  }
  if (window_samples < 2U) {
    return;
  }

  for (phase = 0U; phase < BOARD_A_ALARM_PHASE_COUNT; ++phase) {
    uint8_t age;

    for (age = 0U; age < window_samples; ++age) {
      const board_a_alarm_history_sample_t *newest =
          history_at_age(alarm, age);
      const board_a_alarm_history_sample_t *older;
      uint8_t older_age;
      int32_t delta_x16;
      uint64_t elapsed_ms;
      int32_t rate;

      if ((newest == NULL) || !newest->valid[phase]) {
        continue;
      }
      for (older_age = (uint8_t)(age + 1U);
           older_age < window_samples; ++older_age) {
        uint64_t elapsed_us;

        older = history_at_age(alarm, older_age);
        if ((older == NULL) || !older->valid[phase] ||
            (newest->sample_id == older->sample_id) ||
            (newest->time_us <= older->time_us)) {
          continue;
        }
        elapsed_us = newest->time_us - older->time_us;
        elapsed_ms = elapsed_us / 1000U;
        if (elapsed_ms < alarm->config.rise_window_min_ms) {
          continue;
        }
        delta_x16 = (int32_t)newest->temperature_x16[phase] -
            (int32_t)older->temperature_x16[phase];
        if (delta_x16 <= 0) {
          continue;
        }
        rate = (int32_t)(((int64_t)delta_x16 * 60000LL) /
                         (int64_t)elapsed_ms);
        if (rate > INT16_MAX) {
          rate = INT16_MAX;
        }
        state->rise_valid_mask |= (uint16_t)(1U << phase);
        if (rate > state->maximum_rise_x16_per_min) {
          state->maximum_rise_x16_per_min = (int16_t)rate;
          state->maximum_rise_phase = phase_from_index(phase);
        }
        break;
      }
    }
  }
}

static board_a_alarm_candidate_t compute_candidate(
    const board_a_alarm_t *alarm, const board_a_alarm_state_t *state)
{
  board_a_alarm_candidate_t candidate = {
    BOARD_A_ALARM_NORMAL,
    BOARD_A_ALARM_REASON_NONE,
    BOARD_A_ALARM_PHASE_NONE
  };
  board_a_alarm_reason_t fault_reason = BOARD_A_ALARM_REASON_NONE;
  board_a_alarm_phase_t fault_phase = BOARD_A_ALARM_PHASE_NONE;
  uint8_t strongest_fault_priority = 0U;
  uint8_t phase;

  if (!state->valid) {
    candidate.level = BOARD_A_ALARM_UNKNOWN;
    return candidate;
  }

  for (phase = 0U; phase < BOARD_A_ALARM_PHASE_COUNT; ++phase) {
    if (state->fault_mask & (uint16_t)(1U << phase)) {
      board_a_alarm_reason_t reason =
          fault_reason_for_quality(state->quality[phase]);
      uint8_t priority = reason_priority(reason);

      if (priority > strongest_fault_priority) {
        strongest_fault_priority = priority;
        fault_reason = reason;
        fault_phase = phase_from_index(phase);
      }
    }
  }
  if (fault_reason != BOARD_A_ALARM_REASON_NONE) {
    candidate.level = BOARD_A_ALARM_SENSOR_FAULT;
    candidate.reason = fault_reason;
    candidate.phase = fault_phase;
    return candidate;
  }
  if (state->comparison_mask == 0U) {
    candidate.level = BOARD_A_ALARM_UNKNOWN;
    return candidate;
  }
  if ((state->comparison_mask & (state->comparison_mask - 1U)) == 0U) {
    candidate.level = BOARD_A_ALARM_SENSOR_FAULT;
    candidate.reason = BOARD_A_ALARM_REASON_INSUFFICIENT_VALID_PHASES;
    candidate.phase = BOARD_A_ALARM_PHASE_NONE;
    return candidate;
  }

  for (phase = 0U; phase < BOARD_A_ALARM_PHASE_COUNT; ++phase) {
    board_a_alarm_level_t level;

    if ((state->display_mask & (uint16_t)(1U << phase)) == 0U) {
      continue;
    }
    level = threshold_level(
        state->temperature_x16[phase], alarm->config.phase_notice_x16,
        alarm->config.phase_warning_x16,
        alarm->config.phase_critical_x16);
    consider_candidate(&candidate, level,
                       BOARD_A_ALARM_REASON_PHASE_TEMPERATURE_HIGH,
                       phase_from_index(phase));
  }
  if (state->delta_valid) {
    board_a_alarm_level_t level = threshold_level(
        state->maximum_delta_x16, alarm->config.delta_notice_x16,
        alarm->config.delta_warning_x16,
        alarm->config.delta_critical_x16);

    consider_candidate(&candidate, level,
                       BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH,
                       state->hottest_phase);
  }
  if (state->rise_valid_mask != 0U) {
    board_a_alarm_level_t level = threshold_level(
        state->maximum_rise_x16_per_min,
        alarm->config.rise_notice_x16_per_min,
        alarm->config.rise_warning_x16_per_min,
        alarm->config.rise_critical_x16_per_min);

    consider_candidate(&candidate, level,
                       BOARD_A_ALARM_REASON_RISE_RATE_HIGH,
                       state->maximum_rise_phase);
  }
  return candidate;
}

static int16_t metric_for_reason(const board_a_alarm_state_t *state,
                                 board_a_alarm_reason_t reason)
{
  switch (reason) {
    case BOARD_A_ALARM_REASON_PHASE_TEMPERATURE_HIGH:
      return state->hottest_temperature_x16;
    case BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH:
      return state->maximum_delta_x16;
    case BOARD_A_ALARM_REASON_RISE_RATE_HIGH:
      return state->maximum_rise_x16_per_min;
    default:
      return INT16_MIN;
  }
}

static void apply_hysteresis(const board_a_alarm_t *alarm,
                             const board_a_alarm_state_t *state,
                             board_a_alarm_candidate_t *candidate)
{
  board_a_alarm_level_t current_level = state->level;
  board_a_alarm_reason_t current_reason = state->reason;
  int16_t metric;
  int16_t threshold;
  int32_t release;

  if (!level_is_metric_alarm(current_level) ||
      !level_is_metric_alarm(candidate->level) ||
      (current_level <= candidate->level)) {
    return;
  }
  metric = metric_for_reason(state, current_reason);
  threshold = metric_threshold(&alarm->config, current_reason,
                               current_level);
  if ((metric == INT16_MIN) || (threshold == INT16_MAX)) {
    return;
  }
  release = (int32_t)threshold - (int32_t)alarm->config.hysteresis_x16;
  if ((int32_t)metric >= release) {
    candidate->level = current_level;
    candidate->reason = current_reason;
    candidate->phase = state->trigger_phase;
  }
}

static void copy_state_to_result(const board_a_alarm_t *alarm,
                                 board_a_alarm_result_t *result)
{
  memset(result, 0, sizeof(*result));
  result->event_id = alarm->state.event_id;
  result->event_time_ms = alarm->state.sample_time_ms;
  result->state = alarm->state;
}

static bool commit_candidate(board_a_alarm_t *alarm,
                             const board_a_alarm_candidate_t *candidate,
                             board_a_alarm_event_type_t *event_type,
                             uint64_t *event_time_ms)
{
  board_a_alarm_state_t *state = &alarm->state;
  board_a_alarm_level_t previous_level = state->level;
  board_a_alarm_reason_t previous_reason = state->reason;
  board_a_alarm_phase_t previous_phase = state->trigger_phase;
  bool was_active = alarm->event_active;
  bool committed = false;

  if ((candidate->level == previous_level) &&
      (candidate->reason == previous_reason) &&
      (candidate->phase == previous_phase)) {
    return false;
  }

  if (event_type != NULL) {
    *event_type = BOARD_A_ALARM_EVENT_NONE;
  }
  if (event_time_ms != NULL) {
    *event_time_ms = state->sample_time_ms;
  }
  state->level = candidate->level;
  state->reason = candidate->reason;
  state->trigger_phase = candidate->phase;
  state->latched = (candidate->level != BOARD_A_ALARM_NORMAL) &&
      (candidate->level != BOARD_A_ALARM_UNKNOWN);

  if (state->latched) {
    if (!was_active || previous_level == BOARD_A_ALARM_NORMAL ||
        previous_level == BOARD_A_ALARM_UNKNOWN) {
      state->event_id++;
      alarm->event_active = true;
      alarm->event_started_ms = state->sample_time_ms;
      state->duration_sec = 0U;
      state->acknowledged = false;
      if (event_type != NULL) {
        *event_type = BOARD_A_ALARM_EVENT_RAISED;
      }
    } else {
      state->duration_sec =
          (state->sample_time_ms >= alarm->event_started_ms) ?
          (uint32_t)((state->sample_time_ms - alarm->event_started_ms) /
                     1000ULL) : 0U;
      if ((candidate->level > previous_level) ||
          (candidate->reason != previous_reason) ||
          (candidate->phase != previous_phase)) {
        state->acknowledged = false;
      }
      if (event_type != NULL) {
        *event_type = BOARD_A_ALARM_EVENT_UPDATED;
      }
    }
    if (candidate->level > previous_level ||
        previous_level == BOARD_A_ALARM_NORMAL ||
        previous_level == BOARD_A_ALARM_UNKNOWN ||
        previous_level == BOARD_A_ALARM_SENSOR_FAULT) {
      switch (candidate->level) {
        case BOARD_A_ALARM_NOTICE:
          state->notice_count++;
          break;
        case BOARD_A_ALARM_WARNING:
          state->warning_count++;
          break;
        case BOARD_A_ALARM_CRITICAL:
          state->critical_count++;
          break;
        case BOARD_A_ALARM_SENSOR_FAULT:
          state->sensor_fault_count++;
          break;
        default:
          break;
      }
    }
    committed = true;
  } else {
    if (alarm->event_active || (previous_level != candidate->level)) {
      state->duration_sec =
          (state->sample_time_ms >= alarm->event_started_ms) ?
          (uint32_t)((state->sample_time_ms - alarm->event_started_ms) /
                     1000ULL) : state->duration_sec;
      if ((event_type != NULL) && alarm->event_active &&
          (previous_level != BOARD_A_ALARM_NORMAL) &&
          (previous_level != BOARD_A_ALARM_UNKNOWN)) {
        *event_type = BOARD_A_ALARM_EVENT_RECOVERED;
      }
    }
    alarm->event_active = false;
    state->acknowledged = false;
    committed = true;
  }
  return committed;
}

void board_a_alarm_default_config(board_a_alarm_config_t *config)
{
  if (config == NULL) {
    return;
  }
  config->phase_notice_x16 = (int16_t)(50 * 16);
  config->phase_warning_x16 = (int16_t)(55 * 16);
  config->phase_critical_x16 = (int16_t)(75 * 16);
  config->delta_notice_x16 = (int16_t)(5 * 16);
  config->delta_warning_x16 = (int16_t)(10 * 16);
  config->delta_critical_x16 = (int16_t)(15 * 16);
  config->rise_notice_x16_per_min = (int16_t)(5 * 16);
  config->rise_warning_x16_per_min = (int16_t)(10 * 16);
  config->rise_critical_x16_per_min = (int16_t)(20 * 16);
  config->assert_samples = 3U;
  config->clear_samples = 5U;
  config->hysteresis_x16 = (int16_t)(2 * 16);
  config->rise_window_samples = 4U;
  config->rise_window_min_ms = 1000U;
  config->buzzer_enable = 1U;
}

bool board_a_alarm_validate_config(const board_a_alarm_config_t *config)
{
  if (config == NULL) {
    return false;
  }
  if ((config->phase_notice_x16 < BOARD_A_ALARM_MIN_TEMPERATURE_X16) ||
      (config->phase_warning_x16 < BOARD_A_ALARM_MIN_TEMPERATURE_X16) ||
      (config->phase_critical_x16 < BOARD_A_ALARM_MIN_TEMPERATURE_X16) ||
      (config->phase_notice_x16 > BOARD_A_ALARM_MAX_TEMPERATURE_X16) ||
      (config->phase_warning_x16 > BOARD_A_ALARM_MAX_TEMPERATURE_X16) ||
      (config->phase_critical_x16 > BOARD_A_ALARM_MAX_TEMPERATURE_X16)) {
    return false;
  }
  if ((config->phase_notice_x16 >= config->phase_warning_x16) ||
      (config->phase_warning_x16 >= config->phase_critical_x16)) {
    return false;
  }
  if ((config->delta_notice_x16 < 0) ||
      (config->delta_notice_x16 >= config->delta_warning_x16) ||
      (config->delta_warning_x16 >= config->delta_critical_x16)) {
    return false;
  }
  if ((config->rise_notice_x16_per_min < 0) ||
      (config->rise_notice_x16_per_min >=
       config->rise_warning_x16_per_min) ||
      (config->rise_warning_x16_per_min >=
       config->rise_critical_x16_per_min)) {
    return false;
  }
  if ((config->assert_samples == 0U) ||
      (config->assert_samples > BOARD_A_ALARM_MAX_ASSERT_SAMPLES) ||
      (config->clear_samples == 0U) ||
      (config->clear_samples > BOARD_A_ALARM_MAX_CLEAR_SAMPLES)) {
    return false;
  }
  if ((config->hysteresis_x16 < 0) ||
      ((int32_t)config->phase_notice_x16 -
           (int32_t)config->hysteresis_x16 <
       BOARD_A_ALARM_MIN_TEMPERATURE_X16) ||
      ((int32_t)config->delta_notice_x16 -
           (int32_t)config->hysteresis_x16 < 0) ||
      ((int32_t)config->rise_notice_x16_per_min -
           (int32_t)config->hysteresis_x16 < 0)) {
    return false;
  }
  if ((config->rise_window_samples < 2U) ||
      (config->rise_window_samples >
       BOARD_A_ALARM_RISE_HISTORY_CAPACITY) ||
      (config->rise_window_min_ms == 0U) ||
      (config->rise_window_min_ms > BOARD_A_ALARM_MAX_RISE_WINDOW_MS)) {
    return false;
  }
  if (config->buzzer_enable > 1U) {
    return false;
  }
  return true;
}

bool board_a_alarm_config_equal(const board_a_alarm_config_t *left,
                                const board_a_alarm_config_t *right)
{
  if ((left == NULL) || (right == NULL)) {
    return false;
  }
  return (left->phase_notice_x16 == right->phase_notice_x16) &&
      (left->phase_warning_x16 == right->phase_warning_x16) &&
      (left->phase_critical_x16 == right->phase_critical_x16) &&
      (left->delta_notice_x16 == right->delta_notice_x16) &&
      (left->delta_warning_x16 == right->delta_warning_x16) &&
      (left->delta_critical_x16 == right->delta_critical_x16) &&
      (left->rise_notice_x16_per_min ==
       right->rise_notice_x16_per_min) &&
      (left->rise_warning_x16_per_min ==
       right->rise_warning_x16_per_min) &&
      (left->rise_critical_x16_per_min ==
       right->rise_critical_x16_per_min) &&
      (left->assert_samples == right->assert_samples) &&
      (left->clear_samples == right->clear_samples) &&
      (left->hysteresis_x16 == right->hysteresis_x16) &&
      (left->rise_window_samples == right->rise_window_samples) &&
      (left->rise_window_min_ms == right->rise_window_min_ms) &&
      (left->buzzer_enable == right->buzzer_enable);
}

void board_a_alarm_init(board_a_alarm_t *alarm)
{
  if (alarm == NULL) {
    return;
  }
  memset(alarm, 0, sizeof(*alarm));
  board_a_alarm_default_config(&alarm->config);
  alarm->state.level = BOARD_A_ALARM_UNKNOWN;
  alarm->state.buzzer_enable = alarm->config.buzzer_enable != 0U;
  alarm->state.reason = BOARD_A_ALARM_REASON_NONE;
  alarm->state.trigger_phase = BOARD_A_ALARM_PHASE_NONE;
  alarm->state.hottest_phase = BOARD_A_ALARM_PHASE_NONE;
  alarm->state.coldest_phase = BOARD_A_ALARM_PHASE_NONE;
  alarm->state.maximum_rise_phase = BOARD_A_ALARM_PHASE_NONE;
  alarm->pending_level = BOARD_A_ALARM_UNKNOWN;
  alarm->pending_reason = BOARD_A_ALARM_REASON_NONE;
  alarm->pending_phase = BOARD_A_ALARM_PHASE_NONE;
}

bool board_a_alarm_set_config(board_a_alarm_t *alarm,
                              const board_a_alarm_config_t *config)
{
  if ((alarm == NULL) || !board_a_alarm_validate_config(config)) {
    return false;
  }
  alarm->config = *config;
  alarm->state.buzzer_enable = config->buzzer_enable != 0U;
  alarm->history_count = 0U;
  alarm->history_next = 0U;
  alarm->pending_level = BOARD_A_ALARM_UNKNOWN;
  alarm->pending_reason = BOARD_A_ALARM_REASON_NONE;
  alarm->pending_phase = BOARD_A_ALARM_PHASE_NONE;
  alarm->pending_count = 0U;
  return true;
}

bool board_a_alarm_update(board_a_alarm_t *alarm,
                          const board_a_sensor_snapshot_t *snapshot,
                          board_a_alarm_result_t *result)
{
  board_a_alarm_state_t next_state;
  board_a_alarm_candidate_t candidate;
  bool state_changed = false;
  board_a_alarm_event_type_t event_type = BOARD_A_ALARM_EVENT_NONE;
  uint64_t event_time_ms = 0U;
  uint8_t phase;

  if ((alarm == NULL) || (snapshot == NULL) || (result == NULL)) {
    return false;
  }
  if (!alarm->has_last_sample && (snapshot->sample_id == 0U)) {
    return false;
  }
  if (alarm->has_last_sample &&
      (snapshot->sample_id == alarm->last_sample_id)) {
    return false;
  }
  if (alarm->has_last_sample &&
      (snapshot->sample_time_us <= alarm->last_sample_time_us)) {
    alarm->history_count = 0U;
    alarm->history_next = 0U;
  }

  next_state = alarm->state;
  next_state.valid = true;
  next_state.sample_id = snapshot->sample_id;
  next_state.sample_time_ms = snapshot->sample_time_us / 1000U;
  next_state.display_mask = 0U;
  next_state.comparison_mask = 0U;
  next_state.fault_mask = 0U;
  next_state.maximum_delta_x16 = 0;
  next_state.maximum_rise_x16_per_min = 0;
  next_state.maximum_rise_phase = BOARD_A_ALARM_PHASE_NONE;
  next_state.rise_valid_mask = 0U;

  for (phase = 0U; phase < BOARD_A_ALARM_PHASE_COUNT; ++phase) {
    const board_a_sensor_sample_t *sample = &snapshot->sensors[phase];

    next_state.temperature_x16[phase] = sample->temperature_x16;
  next_state.quality[phase] = sample->quality;
    if (sample->has_value && quality_is_displayable(sample->quality)) {
      next_state.display_mask |= (uint16_t)(1U << phase);
    }
    if (sample->has_value && quality_is_displayable(sample->quality)) {
      next_state.comparison_mask |= (uint16_t)(1U << phase);
    }
    if (!sample->has_value || !quality_is_displayable(sample->quality)) {
      next_state.fault_mask |= (uint16_t)(1U << phase);
    }
  }
  next_state.buzzer_enable = alarm->config.buzzer_enable != 0U;

  if (next_state.comparison_mask != 0U) {
    uint8_t first_phase = BOARD_A_ALARM_PHASE_COUNT;
    uint8_t last_phase = BOARD_A_ALARM_PHASE_COUNT;
    uint8_t valid_phases = 0U;

    for (phase = 0U; phase < BOARD_A_ALARM_PHASE_COUNT; ++phase) {
      if (next_state.comparison_mask & (uint16_t)(1U << phase)) {
        if (first_phase == BOARD_A_ALARM_PHASE_COUNT) {
          first_phase = phase;
        }
        last_phase = phase;
        valid_phases++;
      }
    }
    next_state.hottest_phase = phase_from_index(first_phase);
    next_state.coldest_phase = phase_from_index(first_phase);
    next_state.hottest_temperature_x16 =
        next_state.temperature_x16[first_phase];
    for (phase = (uint8_t)(first_phase + 1U); phase <= last_phase; ++phase) {
      if ((next_state.comparison_mask & (uint16_t)(1U << phase)) == 0U) {
        continue;
      }
      if (next_state.temperature_x16[phase] >
          next_state.hottest_temperature_x16) {
        next_state.hottest_temperature_x16 =
            next_state.temperature_x16[phase];
        next_state.hottest_phase = phase_from_index(phase);
      }
      if (next_state.temperature_x16[phase] <
          next_state.temperature_x16[
              index_from_phase(next_state.coldest_phase)]) {
        next_state.coldest_phase = phase_from_index(phase);
      }
    }
    next_state.delta_valid = valid_phases >= 2U;
    if (next_state.delta_valid) {
      next_state.maximum_delta_x16 = (int16_t)(
          (int32_t)next_state.temperature_x16[
              index_from_phase(next_state.hottest_phase)] -
          (int32_t)next_state.temperature_x16[
              index_from_phase(next_state.coldest_phase)]);
    } else {
      next_state.maximum_delta_x16 = 0;
      next_state.coldest_phase = BOARD_A_ALARM_PHASE_NONE;
    }
    history_push(alarm, snapshot);
    compute_rise(alarm, &next_state);
  } else {
    next_state.delta_valid = false;
    next_state.hottest_phase = BOARD_A_ALARM_PHASE_NONE;
    next_state.coldest_phase = BOARD_A_ALARM_PHASE_NONE;
    next_state.hottest_temperature_x16 = 0;
  }

  alarm->last_sample_id = snapshot->sample_id;
  alarm->last_sample_time_us = snapshot->sample_time_us;
  alarm->has_last_sample = true;
  alarm->state = next_state;
  if (alarm->event_active &&
      (alarm->state.sample_time_ms >= alarm->event_started_ms)) {
    alarm->state.duration_sec = (uint32_t)(
        (alarm->state.sample_time_ms - alarm->event_started_ms) / 1000ULL);
  }

  candidate = compute_candidate(alarm, &alarm->state);
  apply_hysteresis(alarm, &alarm->state, &candidate);

  if ((candidate.level == alarm->state.level) &&
      (candidate.reason == alarm->state.reason) &&
      (candidate.phase == alarm->state.trigger_phase)) {
    alarm->pending_count = 0U;
  } else if (candidate.level == BOARD_A_ALARM_SENSOR_FAULT) {
    state_changed = commit_candidate(
        alarm, &candidate, &event_type, &event_time_ms);
    alarm->pending_count = 0U;
  } else if ((alarm->state.level == BOARD_A_ALARM_UNKNOWN) &&
             (candidate.level == BOARD_A_ALARM_NORMAL)) {
    state_changed = commit_candidate(
        alarm, &candidate, &event_type, &event_time_ms);
    alarm->pending_count = 0U;
  } else {
    board_a_alarm_level_t previous_level = alarm->state.level;
    bool moving_up =
        (previous_level == BOARD_A_ALARM_UNKNOWN) ||
        (candidate.level > previous_level) ||
        ((candidate.level == previous_level) &&
         ((candidate.reason != alarm->state.reason) ||
          (candidate.phase != alarm->state.trigger_phase)));
    board_a_alarm_candidate_t pending = {
      alarm->pending_level,
      alarm->pending_reason,
      alarm->pending_phase
    };

    if (!candidate_equal(&pending, &candidate)) {
      alarm->pending_level = candidate.level;
      alarm->pending_reason = candidate.reason;
      alarm->pending_phase = candidate.phase;
      alarm->pending_count = 1U;
    } else if (alarm->pending_count < UINT16_MAX) {
      alarm->pending_count++;
    }

    if (moving_up &&
        (alarm->pending_count >= alarm->config.assert_samples)) {
      state_changed = commit_candidate(
          alarm, &candidate, &event_type, &event_time_ms);
      alarm->pending_count = 0U;
    } else if (!moving_up &&
               (alarm->pending_count >= alarm->config.clear_samples)) {
      state_changed = commit_candidate(
          alarm, &candidate, &event_type, &event_time_ms);
      alarm->pending_count = 0U;
    }
  }

  copy_state_to_result(alarm, result);
  if (state_changed && (event_type != BOARD_A_ALARM_EVENT_NONE)) {
    result->event = true;
    result->event_type = event_type;
    result->event_id = result->state.event_id;
    result->event_time_ms = event_time_ms;
  }
  return true;
}

void board_a_alarm_ack(board_a_alarm_t *alarm)
{
  if ((alarm == NULL) || !alarm->state.valid ||
      (alarm->state.level == BOARD_A_ALARM_NORMAL) ||
      (alarm->state.level == BOARD_A_ALARM_UNKNOWN)) {
    return;
  }
  alarm->state.acknowledged = true;
}

void board_a_alarm_copy_state(const board_a_alarm_t *alarm,
                              board_a_alarm_state_t *state)
{
  if ((alarm == NULL) || (state == NULL)) {
    return;
  }
  *state = alarm->state;
}

void board_a_alarm_force_unknown(board_a_alarm_t *alarm)
{
  if (alarm == NULL) {
    return;
  }
  alarm->history_count = 0U;
  alarm->history_next = 0U;
  alarm->has_last_sample = false;
  alarm->last_sample_id = 0U;
  alarm->last_sample_time_us = 0U;
  alarm->event_active = false;
  alarm->event_started_ms = 0U;
  alarm->pending_count = 0U;
  alarm->pending_level = BOARD_A_ALARM_UNKNOWN;
  alarm->pending_reason = BOARD_A_ALARM_REASON_NONE;
  alarm->pending_phase = BOARD_A_ALARM_PHASE_NONE;
  alarm->state.valid = false;
  alarm->state.sample_id = 0U;
  alarm->state.sample_time_ms = 0U;
  alarm->state.display_mask = 0U;
  alarm->state.comparison_mask = 0U;
  alarm->state.fault_mask = 0U;
  memset(alarm->state.temperature_x16, 0,
         sizeof(alarm->state.temperature_x16));
  memset(alarm->state.quality, 0, sizeof(alarm->state.quality));
  alarm->state.delta_valid = false;
  alarm->state.maximum_delta_x16 = 0;
  alarm->state.hottest_phase = BOARD_A_ALARM_PHASE_NONE;
  alarm->state.coldest_phase = BOARD_A_ALARM_PHASE_NONE;
  alarm->state.trigger_phase = BOARD_A_ALARM_PHASE_NONE;
  alarm->state.hottest_temperature_x16 = 0;
  alarm->state.maximum_rise_x16_per_min = 0;
  alarm->state.maximum_rise_phase = BOARD_A_ALARM_PHASE_NONE;
  alarm->state.rise_valid_mask = 0U;
  alarm->state.level = BOARD_A_ALARM_UNKNOWN;
  alarm->state.reason = BOARD_A_ALARM_REASON_NONE;
  alarm->state.latched = false;
  alarm->state.acknowledged = false;
  alarm->state.duration_sec = 0U;
}
