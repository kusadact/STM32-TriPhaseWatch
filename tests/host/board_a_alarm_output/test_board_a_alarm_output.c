#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_a_alarm_output.h"

static bool fake_buzzer_on;
static bool fake_led0_on;
static bool fake_led1_on;
static unsigned int fake_buzzer_init_calls;
static unsigned int fake_led_init_calls;
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

void board_a_buzzer_port_init(void)
{
  fake_buzzer_init_calls++;
  fake_buzzer_on = false;
}

void board_a_buzzer_port_set(bool on)
{
  fake_buzzer_on = on;
}

void board_a_led_port_init(void)
{
  fake_led_init_calls++;
  fake_led0_on = false;
  fake_led1_on = false;
}

void board_a_led_port_set(bool led0_on, bool led1_on)
{
  fake_led0_on = led0_on;
  fake_led1_on = led1_on;
}

static void fake_ports_reset(void)
{
  fake_buzzer_on = false;
  fake_led0_on = false;
  fake_led1_on = false;
  fake_buzzer_init_calls = 0U;
  fake_led_init_calls = 0U;
}

static board_a_alarm_state_t state_for(board_a_alarm_level_t level)
{
  board_a_alarm_state_t state;

  memset(&state, 0, sizeof(state));
  state.valid = true;
  state.level = level;
  state.event_id = 1U;
  state.buzzer_enable = true;
  return state;
}

static void update(const board_a_alarm_state_t *state,
                   bool run_active,
                   uint32_t now_ms)
{
  board_a_alarm_output_update(state, run_active, now_ms);
}

static void check_output(bool buzzer_on, bool led0_on, bool led1_on)
{
  CHECK(fake_buzzer_on == buzzer_on);
  CHECK(fake_led0_on == led0_on);
  CHECK(fake_led1_on == led1_on);
  CHECK(board_a_alarm_output_buzzer_active() == buzzer_on);
}

static void test_init_and_inactive_levels(void)
{
  board_a_alarm_state_t state = state_for(BOARD_A_ALARM_NOTICE);

  fake_ports_reset();
  board_a_alarm_output_init();
  CHECK(fake_buzzer_init_calls == 1U);
  CHECK(fake_led_init_calls == 1U);
  check_output(false, false, false);

  state.level = BOARD_A_ALARM_NORMAL;
  update(&state, true, 10U);
  check_output(false, false, false);

  state.level = BOARD_A_ALARM_UNKNOWN;
  update(&state, true, 20U);
  check_output(false, false, false);

  state.level = BOARD_A_ALARM_CRITICAL;
  update(&state, false, 30U);
  check_output(false, false, false);

  update(NULL, true, 40U);
  check_output(false, false, false);
}

static void test_notice_single_beep(void)
{
  board_a_alarm_state_t state = state_for(BOARD_A_ALARM_NOTICE);

  board_a_alarm_output_init();
  update(&state, true, 1000U);
  check_output(true, false, false);
  update(&state, true, 1099U);
  check_output(true, false, false);
  update(&state, true, 1100U);
  check_output(false, false, false);
  update(&state, true, 9000U);
  check_output(false, false, false);

  state.level = BOARD_A_ALARM_NORMAL;
  update(&state, true, 10000U);
  check_output(false, false, false);
  state.level = BOARD_A_ALARM_NOTICE;
  state.event_id = 2U;
  update(&state, true, 20000U);
  check_output(true, false, false);
  update(&state, true, 20099U);
  check_output(true, false, false);
  update(&state, true, 20100U);
  check_output(false, false, false);
}

static void test_warning_timeline_and_ack(void)
{
  board_a_alarm_state_t state = state_for(BOARD_A_ALARM_WARNING);

  board_a_alarm_output_init();
  update(&state, true, 1000U);
  check_output(true, true, false);
  update(&state, true, 1149U);
  check_output(true, true, false);
  update(&state, true, 1150U);
  check_output(false, true, false);
  update(&state, true, 1249U);
  check_output(false, true, false);
  update(&state, true, 1250U);
  check_output(false, false, false);
  update(&state, true, 1299U);
  check_output(false, false, false);
  update(&state, true, 1300U);
  check_output(true, false, false);
  update(&state, true, 1449U);
  check_output(true, false, false);
  update(&state, true, 1450U);
  check_output(false, false, false);
  update(&state, true, 1499U);
  check_output(false, false, false);
  update(&state, true, 1500U);
  check_output(false, true, false);
  update(&state, true, 3999U);
  check_output(false, false, false);
  update(&state, true, 4000U);
  check_output(true, true, false);

  state.acknowledged = true;
  update(&state, true, 4050U);
  check_output(false, true, false);
  update(&state, true, 4250U);
  check_output(false, false, false);
  update(&state, true, 4300U);
  check_output(false, false, false);

  state.acknowledged = false;
  update(&state, true, 5000U);
  check_output(true, true, false);
}

static void test_critical_timeline_and_ack(void)
{
  board_a_alarm_state_t state = state_for(BOARD_A_ALARM_CRITICAL);

  board_a_alarm_output_init();
  update(&state, true, 0U);
  check_output(true, true, true);
  update(&state, true, 124U);
  check_output(true, true, true);
  update(&state, true, 125U);
  check_output(true, false, false);
  update(&state, true, 249U);
  check_output(true, false, false);
  update(&state, true, 250U);
  check_output(true, true, true);
  update(&state, true, 499U);
  check_output(true, false, false);
  update(&state, true, 500U);
  check_output(false, true, true);
  update(&state, true, 749U);
  check_output(false, false, false);
  update(&state, true, 750U);
  check_output(true, true, true);
  update(&state, true, 1500U);
  check_output(true, true, true);

  state.acknowledged = true;
  update(&state, true, 1510U);
  check_output(false, true, true);
  update(&state, true, 1625U);
  check_output(false, false, false);
  update(&state, true, 1750U);
  check_output(false, true, true);

  state.acknowledged = false;
  state.event_id = 2U;
  update(&state, true, 2000U);
  check_output(true, true, true);
}

static void test_sensor_fault_timeline(void)
{
  board_a_alarm_state_t state = state_for(BOARD_A_ALARM_SENSOR_FAULT);

  board_a_alarm_output_init();
  update(&state, true, 0U);
  check_output(true, false, true);
  update(&state, true, 99U);
  check_output(true, false, true);
  update(&state, true, 100U);
  check_output(false, false, true);
  update(&state, true, 199U);
  check_output(false, false, true);
  update(&state, true, 200U);
  check_output(true, false, true);
  update(&state, true, 299U);
  check_output(true, false, true);
  update(&state, true, 300U);
  check_output(false, false, true);
  update(&state, true, 499U);
  check_output(false, false, true);
  update(&state, true, 500U);
  check_output(false, false, false);
  update(&state, true, 999U);
  check_output(false, false, false);
  update(&state, true, 1000U);
  check_output(false, false, true);
  update(&state, true, 4999U);
  check_output(false, false, false);
  update(&state, true, 5000U);
  check_output(true, false, true);
}

static void test_state_switch_clears_old_mode(void)
{
  board_a_alarm_state_t state = state_for(BOARD_A_ALARM_WARNING);

  board_a_alarm_output_init();
  update(&state, true, 0U);
  check_output(true, true, false);
  update(&state, true, 300U);
  check_output(true, false, false);

  state.level = BOARD_A_ALARM_NORMAL;
  update(&state, true, 350U);
  check_output(false, false, false);

  state.level = BOARD_A_ALARM_CRITICAL;
  state.event_id = 2U;
  update(&state, true, 400U);
  check_output(true, true, true);

  state.level = BOARD_A_ALARM_UNKNOWN;
  update(&state, true, 450U);
  check_output(false, false, false);

  state.level = BOARD_A_ALARM_NOTICE;
  state.event_id = 3U;
  update(&state, true, 500U);
  check_output(true, false, false);

  state.level = BOARD_A_ALARM_SENSOR_FAULT;
  state.event_id = 4U;
  update(&state, true, 600U);
  check_output(true, false, true);

  state.level = BOARD_A_ALARM_NORMAL;
  update(&state, true, 650U);
  check_output(false, false, false);
}

static void test_buzzer_enable_only_mutes_sound(void)
{
  board_a_alarm_state_t state = state_for(BOARD_A_ALARM_CRITICAL);

  state.buzzer_enable = false;
  board_a_alarm_output_init();
  update(&state, true, 0U);
  check_output(false, true, true);
  update(&state, true, 125U);
  check_output(false, false, false);
  update(&state, true, 250U);
  check_output(false, true, true);

  state.buzzer_enable = true;
  update(&state, true, 300U);
  check_output(true, true, true);
}

static void test_force_off_stop_and_restart(void)
{
  board_a_alarm_state_t state = state_for(BOARD_A_ALARM_CRITICAL);

  board_a_alarm_output_init();
  update(&state, true, 0U);
  check_output(true, true, true);

  board_a_alarm_output_force_off();
  check_output(false, false, false);

  update(&state, true, 10U);
  check_output(true, true, true);
  update(&state, false, 20U);
  check_output(false, false, false);
  update(&state, true, 30U);
  check_output(true, true, true);

  board_a_alarm_output_init();
  check_output(false, false, false);
  update(&state, true, 40U);
  check_output(true, true, true);
}

static void test_tick_wrap(void)
{
  board_a_alarm_state_t state = state_for(BOARD_A_ALARM_NOTICE);
  uint32_t start_ms = 0xFFFFFFF0U;

  board_a_alarm_output_init();
  update(&state, true, start_ms);
  check_output(true, false, false);
  update(&state, true, start_ms + 99U);
  check_output(true, false, false);
  update(&state, true, start_ms + 100U);
  check_output(false, false, false);

  state.level = BOARD_A_ALARM_WARNING;
  state.event_id = 2U;
  start_ms = 0xFFFFFF00U;
  update(&state, true, start_ms);
  check_output(true, true, false);
  update(&state, true, start_ms + 149U);
  check_output(true, true, false);
  update(&state, true, start_ms + 150U);
  check_output(false, true, false);
  update(&state, true, start_ms + 300U);
  check_output(true, false, false);
  update(&state, true, start_ms + 3000U);
  check_output(true, true, false);
}

int main(void)
{
  test_init_and_inactive_levels();
  test_notice_single_beep();
  test_warning_timeline_and_ack();
  test_critical_timeline_and_ack();
  test_sensor_fault_timeline();
  test_state_switch_clears_old_mode();
  test_buzzer_enable_only_mutes_sound();
  test_force_off_stop_and_restart();
  test_tick_wrap();

  printf("board_a_alarm_output host tests: %u checks, %u failures\n",
         checks, failures);
  return (failures == 0U) ? 0 : 1;
}
