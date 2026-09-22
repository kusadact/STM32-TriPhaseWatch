#ifndef BOARD_A_ALARM_H
#define BOARD_A_ALARM_H

#include <stdbool.h>
#include <stdint.h>

#include "../sensors/sensor_manager.h"

#define BOARD_A_ALARM_PHASE_COUNT 3U
#define BOARD_A_ALARM_RISE_HISTORY_CAPACITY 8U

typedef enum {
  BOARD_A_ALARM_NORMAL = 0,
  BOARD_A_ALARM_NOTICE = 1,
  BOARD_A_ALARM_WARNING = 2,
  BOARD_A_ALARM_CRITICAL = 3,
  BOARD_A_ALARM_SENSOR_FAULT = 4,
  BOARD_A_ALARM_UNKNOWN = 5
} board_a_alarm_level_t;

typedef enum {
  BOARD_A_ALARM_REASON_NONE = 0,
  BOARD_A_ALARM_REASON_PHASE_TEMPERATURE_HIGH = 1,
  BOARD_A_ALARM_REASON_PHASE_DELTA_HIGH = 2,
  BOARD_A_ALARM_REASON_RISE_RATE_HIGH = 3,
  BOARD_A_ALARM_REASON_SENSOR_NOT_PRESENT = 4,
  BOARD_A_ALARM_REASON_SENSOR_TIMEOUT = 5,
  BOARD_A_ALARM_REASON_SENSOR_CRC_ERROR = 6,
  BOARD_A_ALARM_REASON_SENSOR_RANGE_ERROR = 7,
  BOARD_A_ALARM_REASON_INSUFFICIENT_VALID_PHASES = 8,
  BOARD_A_ALARM_REASON_CONFIG_INVALID = 9
} board_a_alarm_reason_t;

typedef enum {
  BOARD_A_ALARM_PHASE_NONE = 0,
  BOARD_A_ALARM_PHASE_A = 1,
  BOARD_A_ALARM_PHASE_B = 2,
  BOARD_A_ALARM_PHASE_C = 3
} board_a_alarm_phase_t;

typedef enum {
  BOARD_A_ALARM_EVENT_NONE = 0,
  BOARD_A_ALARM_EVENT_RAISED = 1,
  BOARD_A_ALARM_EVENT_UPDATED = 2,
  BOARD_A_ALARM_EVENT_RECOVERED = 3
} board_a_alarm_event_type_t;

typedef struct {
  int16_t phase_notice_x16;
  int16_t phase_warning_x16;
  int16_t phase_critical_x16;
  int16_t delta_notice_x16;
  int16_t delta_warning_x16;
  int16_t delta_critical_x16;
  int16_t rise_notice_x16_per_min;
  int16_t rise_warning_x16_per_min;
  int16_t rise_critical_x16_per_min;
  uint16_t assert_samples;
  uint16_t clear_samples;
  int16_t hysteresis_x16;
  uint16_t rise_window_samples;
  uint32_t rise_window_min_ms;
  uint8_t buzzer_enable;
} board_a_alarm_config_t;

typedef struct {
  bool valid;
  uint32_t sample_id;
  uint64_t sample_time_ms;
  uint16_t display_mask;
  uint16_t comparison_mask;
  uint16_t fault_mask;
  int16_t temperature_x16[BOARD_A_ALARM_PHASE_COUNT];
  uint16_t quality[BOARD_A_ALARM_PHASE_COUNT];

  bool delta_valid;
  int16_t maximum_delta_x16;
  board_a_alarm_phase_t hottest_phase;
  board_a_alarm_phase_t coldest_phase;
  board_a_alarm_phase_t trigger_phase;
  int16_t hottest_temperature_x16;

  int16_t maximum_rise_x16_per_min;
  board_a_alarm_phase_t maximum_rise_phase;
  uint16_t rise_valid_mask;

  board_a_alarm_level_t level;
  board_a_alarm_reason_t reason;
  bool latched;
  bool acknowledged;
  bool buzzer_enable;
  uint32_t event_id;
  uint32_t duration_sec;
  uint32_t notice_count;
  uint32_t warning_count;
  uint32_t critical_count;
  uint32_t sensor_fault_count;
} board_a_alarm_state_t;

typedef struct {
  bool event;
  board_a_alarm_event_type_t event_type;
  uint32_t event_id;
  uint64_t event_time_ms;
  board_a_alarm_state_t state;
} board_a_alarm_result_t;

typedef struct {
  uint32_t sample_id;
  uint64_t time_us;
  bool valid[BOARD_A_ALARM_PHASE_COUNT];
  int16_t temperature_x16[BOARD_A_ALARM_PHASE_COUNT];
} board_a_alarm_history_sample_t;

typedef struct {
  board_a_alarm_config_t config;
  board_a_alarm_state_t state;
  board_a_alarm_history_sample_t history[
      BOARD_A_ALARM_RISE_HISTORY_CAPACITY];
  uint8_t history_count;
  uint8_t history_next;
  bool has_last_sample;
  uint32_t last_sample_id;
  uint64_t last_sample_time_us;
  bool event_active;
  uint64_t event_started_ms;
  board_a_alarm_level_t pending_level;
  board_a_alarm_reason_t pending_reason;
  board_a_alarm_phase_t pending_phase;
  uint16_t pending_count;
} board_a_alarm_t;

void board_a_alarm_default_config(board_a_alarm_config_t *config);

bool board_a_alarm_validate_config(const board_a_alarm_config_t *config);

bool board_a_alarm_config_equal(const board_a_alarm_config_t *left,
                                const board_a_alarm_config_t *right);

void board_a_alarm_init(board_a_alarm_t *alarm);

bool board_a_alarm_set_config(board_a_alarm_t *alarm,
                              const board_a_alarm_config_t *config);

bool board_a_alarm_update(board_a_alarm_t *alarm,
                          const board_a_sensor_snapshot_t *snapshot,
                          board_a_alarm_result_t *result);

void board_a_alarm_ack(board_a_alarm_t *alarm);

void board_a_alarm_copy_state(const board_a_alarm_t *alarm,
                              board_a_alarm_state_t *state);

void board_a_alarm_force_unknown(board_a_alarm_t *alarm);

#endif /* BOARD_A_ALARM_H */
