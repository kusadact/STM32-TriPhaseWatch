#include "board_a_alarm_output.h"

#include <stddef.h>

#include "../platform/board_a_buzzer_port.h"
#include "../platform/board_a_led_port.h"

#define BOARD_A_OUTPUT_NOTICE_MS 100U
#define BOARD_A_OUTPUT_WARNING_BEEP_MS 150U
#define BOARD_A_OUTPUT_WARNING_GAP_MS 150U
#define BOARD_A_OUTPUT_WARNING_PERIOD_MS 3000U
#define BOARD_A_OUTPUT_WARNING_LED_PERIOD_MS 500U
#define BOARD_A_OUTPUT_WARNING_LED_ON_MS 250U
#define BOARD_A_OUTPUT_CRITICAL_BEEP_MS 500U
#define BOARD_A_OUTPUT_CRITICAL_GAP_MS 250U
#define BOARD_A_OUTPUT_CRITICAL_PERIOD_MS \
    (BOARD_A_OUTPUT_CRITICAL_BEEP_MS + BOARD_A_OUTPUT_CRITICAL_GAP_MS)
#define BOARD_A_OUTPUT_CRITICAL_LED_PERIOD_MS 250U
#define BOARD_A_OUTPUT_CRITICAL_LED_ON_MS 125U
#define BOARD_A_OUTPUT_FAULT_BEEP_MS 100U
#define BOARD_A_OUTPUT_FAULT_GAP_MS 100U
#define BOARD_A_OUTPUT_FAULT_PERIOD_MS 5000U
#define BOARD_A_OUTPUT_FAULT_LED_PERIOD_MS 1000U
#define BOARD_A_OUTPUT_FAULT_LED_ON_MS 500U

typedef struct {
  board_a_alarm_level_t level;
  uint32_t pattern_started_ms;
  uint32_t event_id;
  bool acknowledged;
} board_a_alarm_output_state_t;

static board_a_alarm_output_state_t g_output = {
    BOARD_A_ALARM_UNKNOWN,
    0U,
    0U,
    false
};
static bool g_buzzer_on;

static bool level_is_actuated(board_a_alarm_level_t level)
{
  return (level == BOARD_A_ALARM_NOTICE) ||
      (level == BOARD_A_ALARM_WARNING) ||
      (level == BOARD_A_ALARM_CRITICAL) ||
      (level == BOARD_A_ALARM_SENSOR_FAULT);
}

static void set_outputs(bool buzzer_on, bool led0_on, bool led1_on)
{
  g_buzzer_on = buzzer_on;
  board_a_buzzer_port_set(buzzer_on);
  board_a_led_port_set(led0_on, led1_on);
}

static void output_off(void)
{
  g_output.level = BOARD_A_ALARM_UNKNOWN;
  g_output.pattern_started_ms = 0U;
  g_output.event_id = 0U;
  g_output.acknowledged = false;
  set_outputs(false, false, false);
}

static bool warning_buzzer_on(uint32_t elapsed_ms)
{
  uint32_t phase = elapsed_ms % BOARD_A_OUTPUT_WARNING_PERIOD_MS;
  uint32_t second_beep_start =
      BOARD_A_OUTPUT_WARNING_BEEP_MS + BOARD_A_OUTPUT_WARNING_GAP_MS;

  return (phase < BOARD_A_OUTPUT_WARNING_BEEP_MS) ||
      ((phase >= second_beep_start) &&
       (phase < (second_beep_start + BOARD_A_OUTPUT_WARNING_BEEP_MS)));
}

static bool warning_led_on(uint32_t elapsed_ms)
{
  return (elapsed_ms % BOARD_A_OUTPUT_WARNING_LED_PERIOD_MS) <
      BOARD_A_OUTPUT_WARNING_LED_ON_MS;
}

static bool critical_buzzer_on(uint32_t elapsed_ms)
{
  return (elapsed_ms % BOARD_A_OUTPUT_CRITICAL_PERIOD_MS) <
      BOARD_A_OUTPUT_CRITICAL_BEEP_MS;
}

static bool critical_led_on(uint32_t elapsed_ms)
{
  return (elapsed_ms % BOARD_A_OUTPUT_CRITICAL_LED_PERIOD_MS) <
      BOARD_A_OUTPUT_CRITICAL_LED_ON_MS;
}

static bool sensor_fault_buzzer_on(uint32_t elapsed_ms)
{
  uint32_t phase = elapsed_ms % BOARD_A_OUTPUT_FAULT_PERIOD_MS;
  uint32_t second_beep_start =
      BOARD_A_OUTPUT_FAULT_BEEP_MS + BOARD_A_OUTPUT_FAULT_GAP_MS;

  return (phase < BOARD_A_OUTPUT_FAULT_BEEP_MS) ||
      ((phase >= second_beep_start) &&
       (phase < (second_beep_start + BOARD_A_OUTPUT_FAULT_BEEP_MS)));
}

static bool sensor_fault_led_on(uint32_t elapsed_ms)
{
  return (elapsed_ms % BOARD_A_OUTPUT_FAULT_LED_PERIOD_MS) <
      BOARD_A_OUTPUT_FAULT_LED_ON_MS;
}

void board_a_alarm_output_init(void)
{
  board_a_buzzer_port_init();
  board_a_led_port_init();
  output_off();
}

void board_a_alarm_output_update(const board_a_alarm_state_t *state,
                                 bool run_active,
                                 uint32_t now_ms)
{
  uint32_t elapsed_ms;
  bool buzzer_on = false;
  bool led0_on = false;
  bool led1_on = false;

  if ((state == NULL) || !run_active || !level_is_actuated(state->level)) {
    output_off();
    return;
  }

  if ((g_output.level != state->level) ||
      (g_output.event_id != state->event_id) ||
      (g_output.acknowledged && !state->acknowledged)) {
    g_output.pattern_started_ms = now_ms;
  }
  g_output.level = state->level;
  g_output.event_id = state->event_id;
  g_output.acknowledged = state->acknowledged;

  elapsed_ms = now_ms - g_output.pattern_started_ms;
  switch (state->level) {
    case BOARD_A_ALARM_NOTICE:
      buzzer_on = elapsed_ms < BOARD_A_OUTPUT_NOTICE_MS;
      break;
    case BOARD_A_ALARM_WARNING:
      buzzer_on = warning_buzzer_on(elapsed_ms);
      led0_on = warning_led_on(elapsed_ms);
      break;
    case BOARD_A_ALARM_CRITICAL:
      buzzer_on = critical_buzzer_on(elapsed_ms);
      led0_on = critical_led_on(elapsed_ms);
      led1_on = led0_on;
      break;
    case BOARD_A_ALARM_SENSOR_FAULT:
      buzzer_on = sensor_fault_buzzer_on(elapsed_ms);
      led1_on = sensor_fault_led_on(elapsed_ms);
      break;
    default:
      break;
  }

  if (!state->buzzer_enable || state->acknowledged) {
    buzzer_on = false;
  }
  set_outputs(buzzer_on, led0_on, led1_on);
}

void board_a_alarm_output_force_off(void)
{
  output_off();
}

bool board_a_alarm_output_buzzer_active(void)
{
  return g_buzzer_on;
}
