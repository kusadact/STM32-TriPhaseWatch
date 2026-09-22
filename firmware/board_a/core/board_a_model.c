#include "board_a_model.h"

#include <string.h>

#define BOARD_A_FIRMWARE_MAJOR 1U
#define BOARD_A_FIRMWARE_MINOR 0U
#define BOARD_A_FIRMWARE_PATCH 0U
#define BOARD_A_PROTOCOL_VERSION 3U

#define BOARD_A_DEFAULT_PERIOD_SEC 10U
#define BOARD_A_DEFAULT_CHANNEL_MASK 0x0001U
#define BOARD_A_DEFAULT_RECORD_COUNT 0U
#define BOARD_A_DS18B20_CONTRACT_REVISION 2U

static uint16_t word_low16(uint32_t value)
{
  return (uint16_t)(value & 0xFFFFU);
}

static uint16_t word_high16(uint32_t value)
{
  return (uint16_t)(value >> 16U);
}

static uint16_t channel_count_from_mask(uint16_t mask)
{
  uint16_t count = 0U;
  uint8_t channel;

  for (channel = 0U; channel < BOARD_A_MAX_CHANNELS; ++channel) {
    if ((mask & (uint16_t)(1U << channel)) != 0U) {
      count++;
    }
  }
  return count;
}

static uint32_t current_utc_seconds(const board_a_model_t *model,
                                    uint64_t now_us)
{
  uint64_t elapsed_us;
  uint64_t current;

  if (!model->time.time_valid) {
    return BOARD_A_TIME_INVALID_SECONDS;
  }

  elapsed_us = (now_us > model->time.utc_anchor_us) ?
      (now_us - model->time.utc_anchor_us) : 0U;
  current = (uint64_t)model->time.utc_anchor_seconds +
            (elapsed_us / 1000000ULL);
  if (current >= (uint64_t)BOARD_A_TIME_INVALID_SECONDS) {
    /*
     * Saturate at the highest legal UTC value; the reserved invalid sentinel
     * must keep meaning "no software UTC" on the wire.
     */
    current = (uint64_t)(BOARD_A_TIME_INVALID_SECONDS - 1U);
  }
  return (uint32_t)current;
}

/*
 * The target second begins exactly (target - anchor) whole seconds after the
 * anchor instant. The subtraction stays in 64 bits so the anchor's fractional
 * second is never truncated before the deadline is formed.
 */
static bool compute_target_deadline_us(const board_a_model_t *model,
                                       uint32_t target_seconds,
                                       uint64_t *deadline_us)
{
  uint64_t target = (uint64_t)target_seconds;
  uint64_t anchor_seconds = (uint64_t)model->time.utc_anchor_seconds;
  uint64_t delta_seconds;
  uint64_t delta_us;

  if (target <= anchor_seconds) {
    return false;
  }

  delta_seconds = target - anchor_seconds;
  delta_us = delta_seconds * 1000000ULL;
  if (delta_us > (UINT64_MAX - model->time.utc_anchor_us)) {
    return false;
  }

  *deadline_us = model->time.utc_anchor_us + delta_us;
  return true;
}

static bool config_is_valid(const board_a_config_t *config)
{
  if ((config->period_sec < BOARD_A_PERIOD_MIN_SEC) ||
      (config->period_sec > BOARD_A_PERIOD_MAX_SEC)) {
    return false;
  }
  if ((config->channel_mask < BOARD_A_CHANNEL_MASK_MIN) ||
      (config->channel_mask > BOARD_A_CHANNEL_MASK_MAX)) {
    return false;
  }
  return true;
}

static bool data_source_is_valid(uint16_t source)
{
  return (source == BOARD_A_DATA_SOURCE_TEST) ||
      (source == BOARD_A_DATA_SOURCE_REAL_DS18B20);
}

static void reset_sensor_snapshot(board_a_sensor_snapshot_t *snapshot)
{
  uint8_t index;

  memset(snapshot, 0, sizeof(*snapshot));
  for (index = 0U; index < BOARD_A_SENSOR_COUNT; ++index) {
    snapshot->sensors[index].sensor_id = index;
    snapshot->sensors[index].sensor_type = BOARD_A_SENSOR_TYPE_DS18B20;
    snapshot->sensors[index].temperature_x16 = 0;
    snapshot->sensors[index].quality = BOARD_A_QUALITY_NOT_PRESENT;
  }
}

static void generate_record(board_a_model_t *model,
                            board_a_sample_trigger_t trigger,
                            uint64_t planned_us, uint64_t now_us)
{
  board_a_record_format_record_t record;
  uint8_t channel;

  memset(&record, 0, sizeof(record));
  model->snapshot.valid = true;
  model->snapshot.sequence++;
  model->snapshot.sample_time_us = now_us;
  model->snapshot.source = model->data_source;
  model->snapshot.trigger = (uint16_t)trigger;
  model->snapshot.channel_count =
      channel_count_from_mask(model->active_config.config.channel_mask);

  if (model->data_source == BOARD_A_DATA_SOURCE_REAL_DS18B20) {
    model->snapshot.sensors = model->pending_sensor_snapshot;
    for (channel = 0U; channel < BOARD_A_MAX_CHANNELS; ++channel) {
      model->snapshot.channel_values[channel] = 0U;
      model->snapshot.channel_quality[channel] =
          BOARD_A_QUALITY_UNAVAILABLE;
    }
  } else {
    /*
     * TEST data is deterministic and intentionally not tied to a real
     * sensor. Counts wrap naturally at 16 bits; the sequence remains the
     * record identity.
     */
    reset_sensor_snapshot(&model->snapshot.sensors);
    model->snapshot.sensors.sample_id = model->snapshot.sequence;
    for (channel = 0U; channel < BOARD_A_MAX_CHANNELS; ++channel) {
      if ((model->active_config.config.channel_mask &
           (uint16_t)(1U << channel)) != 0U) {
        model->snapshot.channel_values[channel] =
            (uint16_t)((model->snapshot.sequence * 10U) + channel);
        model->snapshot.channel_quality[channel] =
            BOARD_A_QUALITY_TEST_VALID;
      } else {
        model->snapshot.channel_values[channel] = 0U;
        model->snapshot.channel_quality[channel] =
            BOARD_A_QUALITY_UNAVAILABLE;
      }
    }
  }

  record.session_id = model->session_id;
  record.sequence = model->snapshot.sequence;
  record.trigger = (uint16_t)trigger;
  record.planned_ms = planned_us / 1000U;
  record.actual_ms = now_us / 1000U;
  if (model->time.time_valid) {
    record.utc_valid = 1U;
    record.utc_seconds = current_utc_seconds(model, now_us);
  } else {
    record.utc_valid = 0U;
    record.utc_seconds = 0U;
  }
  record.config_version = model->active_config.version;
  record.period_sec = model->active_config.config.period_sec;
  record.channel_mask = model->active_config.config.channel_mask;
  record.sample_count = model->active_config.config.record_count;
  record.source = model->data_source;
  for (channel = 0U; channel < BOARD_A_MAX_CHANNELS; channel++) {
    record.values[channel] = model->snapshot.channel_values[channel];
    record.units[channel] =
        (model->data_source == BOARD_A_DATA_SOURCE_REAL_DS18B20) ?
        BOARD_A_UNIT_TEMPERATURE_X16 : BOARD_A_UNIT_COUNT;
    record.qualities[channel] = model->snapshot.channel_quality[channel];
  }
  if (model->data_source == BOARD_A_DATA_SOURCE_REAL_DS18B20) {
    record.ds18b20_valid_mask = model->snapshot.sensors.valid_mask;
    record.ds18b20_sample_id = model->snapshot.sensors.sample_id;
    for (channel = 0U; channel < BOARD_A_SENSOR_COUNT; ++channel) {
      record.ds18b20_temperature_x16[channel] =
          model->snapshot.sensors.sensors[channel].temperature_x16;
      record.ds18b20_quality[channel] =
          model->snapshot.sensors.sensors[channel].quality;
      record.ds18b20_error[channel] =
          model->snapshot.sensors.sensors[channel].error;
      record.ds18b20_rom_short[channel] =
          model->snapshot.sensors.sensors[channel].rom_short;
      record.ds18b20_sample_time_ms[channel] =
          model->snapshot.sensors.sensors[channel].sample_time_ms;
    }
  }

  if (!board_a_persistence_queue_push(&model->persistence, &record)) {
    model->stats.storage_dropped = model->persistence.storage.dropped;
  }
}

static void set_command_status(board_a_model_t *model,
                               uint16_t command,
                               uint16_t result,
                               uint32_t command_id)
{
  model->last_command = command;
  model->command_result = result;
  model->last_command_id = command_id;
}

static bool command_id_is_known(const uint32_t *ids, uint8_t count,
                                uint32_t id)
{
  uint8_t index;

  for (index = 0U; index < count; ++index) {
    if (ids[index] == id) {
      return true;
    }
  }
  return false;
}

static void remember_command_id(uint32_t *ids, uint8_t *count, uint8_t *next,
                                uint32_t id)
{
  ids[*next] = id;
  *next = (uint8_t)((*next + 1U) % BOARD_A_SINGLE_DEDUP_CAPACITY);
  if (*count < BOARD_A_SINGLE_DEDUP_CAPACITY) {
    (*count)++;
  }
}

static modbus_result_t execute_command(board_a_model_t *model,
                                       uint16_t command,
                                       uint32_t command_id,
                                       uint64_t now_us)
{
  uint32_t target_seconds;
  uint64_t deadline_us;

  switch (command) {
    case BOARD_A_COMMAND_APPLY_CONFIG:
      if (!config_is_valid(&model->pending_config) ||
          !board_a_alarm_validate_config(&model->pending_alarm_config)) {
        set_command_status(model, command, BOARD_A_COMMAND_RESULT_REJECTED,
                           command_id);
        model->stats.device_faults++;
        return MODBUS_RESULT_DEVICE_FAILURE;
      }
      model->active_config.config = model->pending_config;
      model->active_config.alarm_config = model->pending_alarm_config;
      model->active_config.valid = true;
      model->active_config.version++;
      set_command_status(model, command, BOARD_A_COMMAND_RESULT_ACCEPTED,
                         command_id);
      return MODBUS_RESULT_OK;

    case BOARD_A_COMMAND_SAVE_CONFIG:
      {
        board_a_persisted_config_t config;
        board_a_save_accept_result_t accept_result;
        uint8_t sensor;

        memset(&config, 0, sizeof(config));
        config.period_sec = model->active_config.config.period_sec;
        config.channel_mask = model->active_config.config.channel_mask;
        config.record_count = model->active_config.config.record_count;
        config.alarm = model->active_config.alarm_config;
        config.sensor_valid_mask = model->sensor_map.valid_mask;
        for (sensor = 0U; sensor < BOARD_A_SENSOR_COUNT; ++sensor) {
          memcpy(config.sensor_roms[sensor],
                 model->sensor_map.bindings[sensor].rom,
                 DS18B20_ROM_SIZE);
        }
        accept_result = board_a_persistence_accept_save(
            &model->persistence, &config, model->active_config.version,
            command_id);
        if (accept_result == BOARD_A_SAVE_ACCEPT_BUSY) {
          model->stats.persistence_errors++;
          set_command_status(model, command,
                             BOARD_A_COMMAND_RESULT_REJECTED, command_id);
          return MODBUS_RESULT_SLAVE_BUSY;
        }
        if (accept_result != BOARD_A_SAVE_ACCEPT_OK) {
          model->stats.persistence_errors++;
          set_command_status(model, command,
                             BOARD_A_COMMAND_RESULT_REJECTED, command_id);
          return MODBUS_RESULT_ILLEGAL_VALUE;
        }
        set_command_status(model, command, BOARD_A_COMMAND_RESULT_ACCEPTED,
                           command_id);
        return MODBUS_RESULT_OK;
      }

    case BOARD_A_COMMAND_START:
      if (model->persistence.storage.drain_state ==
          BOARD_A_DRAIN_PENDING) {
        model->stats.device_faults++;
        set_command_status(model, command, BOARD_A_COMMAND_RESULT_REJECTED,
                           command_id);
        return MODBUS_RESULT_SLAVE_BUSY;
      }
      if (!model->active_config.valid) {
        set_command_status(model, command, BOARD_A_COMMAND_RESULT_REJECTED,
                           command_id);
        model->stats.device_faults++;
        return MODBUS_RESULT_DEVICE_FAILURE;
      }
      /* An explicit start supersedes any waiting scheduled start. */
      model->time.schedule_armed = false;
      model->time.schedule_target_seconds = 0U;
      model->time.schedule_deadline_us = 0U;
      if (model->run_state != BOARD_A_RUN_RUNNING) {
        model->run_state = BOARD_A_RUN_RUNNING;
        model->records_this_run = 0U;
        model->next_sample_us = 0U;
        model->start_pending = true;
      }
      set_command_status(model, command, BOARD_A_COMMAND_RESULT_ACCEPTED,
                         command_id);
      return MODBUS_RESULT_OK;

    case BOARD_A_COMMAND_STOP:
      model->time.schedule_armed = false;
      model->time.schedule_target_seconds = 0U;
      model->time.schedule_deadline_us = 0U;
      model->run_state = BOARD_A_RUN_STOPPED;
      model->start_pending = false;
      model->next_sample_us = 0U;
      board_a_persistence_request_drain(&model->persistence);
      set_command_status(model, command, BOARD_A_COMMAND_RESULT_ACCEPTED,
                         command_id);
      return MODBUS_RESULT_OK;

    case BOARD_A_COMMAND_SINGLE:
      if (model->persistence.storage.drain_state ==
          BOARD_A_DRAIN_PENDING) {
        model->stats.device_faults++;
        set_command_status(model, command, BOARD_A_COMMAND_RESULT_REJECTED,
                           command_id);
        return MODBUS_RESULT_SLAVE_BUSY;
      }
      if (command_id_is_known(model->single_ids, model->single_id_count,
                              command_id)) {
        set_command_status(model, command, BOARD_A_COMMAND_RESULT_DUPLICATE,
                           command_id);
        return MODBUS_RESULT_OK;
      }
      generate_record(model, BOARD_A_SAMPLE_TRIGGER_SINGLE, now_us, now_us);
      remember_command_id(model->single_ids, &model->single_id_count,
                          &model->single_id_next, command_id);
      set_command_status(model, command, BOARD_A_COMMAND_RESULT_ACCEPTED,
                         command_id);
      return MODBUS_RESULT_OK;

    case BOARD_A_COMMAND_ACK_ALARM:
      if (command_id_is_known(model->alarm_ack_ids,
                              model->alarm_ack_id_count, command_id)) {
        set_command_status(model, command, BOARD_A_COMMAND_RESULT_DUPLICATE,
                           command_id);
        return MODBUS_RESULT_OK;
      }
      model->alarm_ack_requested = true;
      remember_command_id(model->alarm_ack_ids, &model->alarm_ack_id_count,
                          &model->alarm_ack_id_next, command_id);
      set_command_status(model, command, BOARD_A_COMMAND_RESULT_ACCEPTED,
                         command_id);
      return MODBUS_RESULT_OK;

    case BOARD_A_COMMAND_SET_TIME:
      if (model->time.schedule_armed) {
        set_command_status(model, command, BOARD_A_COMMAND_RESULT_REJECTED,
                           command_id);
        model->stats.device_faults++;
        return MODBUS_RESULT_DEVICE_FAILURE;
      }
      if (model->time.pending_utc_seconds >= BOARD_A_TIME_INVALID_SECONDS) {
        set_command_status(model, command, BOARD_A_COMMAND_RESULT_REJECTED,
                           command_id);
        return MODBUS_RESULT_ILLEGAL_VALUE;
      }
      model->time.time_valid = true;
      model->time.utc_anchor_seconds = model->time.pending_utc_seconds;
      model->time.utc_anchor_us = now_us;
      set_command_status(model, command, BOARD_A_COMMAND_RESULT_ACCEPTED,
                         command_id);
      return MODBUS_RESULT_OK;

    case BOARD_A_COMMAND_ARM_START:
      if (model->persistence.storage.drain_state ==
          BOARD_A_DRAIN_PENDING) {
        model->stats.device_faults++;
        set_command_status(model, command, BOARD_A_COMMAND_RESULT_REJECTED,
                           command_id);
        return MODBUS_RESULT_SLAVE_BUSY;
      }
      if (!model->time.time_valid ||
          (model->run_state == BOARD_A_RUN_RUNNING)) {
        set_command_status(model, command, BOARD_A_COMMAND_RESULT_REJECTED,
                           command_id);
        model->stats.device_faults++;
        return MODBUS_RESULT_DEVICE_FAILURE;
      }
      target_seconds = model->time.pending_start_utc_seconds;
      if ((target_seconds >= BOARD_A_TIME_INVALID_SECONDS) ||
          !compute_target_deadline_us(model, target_seconds, &deadline_us) ||
          (deadline_us <= now_us)) {
        set_command_status(model, command, BOARD_A_COMMAND_RESULT_REJECTED,
                           command_id);
        return MODBUS_RESULT_ILLEGAL_VALUE;
      }
      /*
       * A new accepted target atomically replaces the latched one; a rejected
       * target above leaves the previous schedule untouched.
       */
      model->time.schedule_armed = true;
      model->time.schedule_target_seconds = target_seconds;
      model->time.schedule_deadline_us = deadline_us;
      set_command_status(model, command, BOARD_A_COMMAND_RESULT_ACCEPTED,
                         command_id);
      return MODBUS_RESULT_OK;

    case BOARD_A_COMMAND_NONE:
    default:
      set_command_status(model, command, BOARD_A_COMMAND_RESULT_REJECTED,
                         command_id);
      return MODBUS_RESULT_ILLEGAL_VALUE;
  }
}

static modbus_result_t read_holding_register(const board_a_model_t *model,
                                             uint16_t address,
                                             uint16_t *value)
{
  switch (address) {
    case BOARD_A_HOLDING_CFG_PERIOD_SEC:
      *value = model->pending_config.period_sec;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_CFG_CHANNEL_MASK:
      *value = model->pending_config.channel_mask;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_CFG_RECORD_COUNT:
      *value = model->pending_config.record_count;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_PENDING_UTC_SECONDS_HI:
      *value = word_high16(model->time.pending_utc_seconds);
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_PENDING_UTC_SECONDS_LO:
      *value = word_low16(model->time.pending_utc_seconds);
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_HI:
      *value = word_high16(model->time.pending_start_utc_seconds);
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_LO:
      *value = word_low16(model->time.pending_start_utc_seconds);
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_COMMAND:
      *value = model->command_register;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_COMMAND_ID_HI:
      *value = model->command_id_hi;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_COMMAND_ID_LO:
      *value = model->command_id_lo;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_ALARM_CONFIG_REVISION:
      *value = BOARD_A_ALARM_CONFIG_CONTRACT_REVISION;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_ALARM_PHASE_NOTICE:
      *value = (uint16_t)model->pending_alarm_config.phase_notice_x16;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_ALARM_PHASE_WARNING:
      *value = (uint16_t)model->pending_alarm_config.phase_warning_x16;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_ALARM_PHASE_CRITICAL:
      *value = (uint16_t)model->pending_alarm_config.phase_critical_x16;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_ALARM_DELTA_NOTICE:
      *value = (uint16_t)model->pending_alarm_config.delta_notice_x16;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_ALARM_DELTA_WARNING:
      *value = (uint16_t)model->pending_alarm_config.delta_warning_x16;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_ALARM_DELTA_CRITICAL:
      *value = (uint16_t)model->pending_alarm_config.delta_critical_x16;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_ALARM_RISE_NOTICE:
      *value =
          (uint16_t)model->pending_alarm_config.rise_notice_x16_per_min;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_ALARM_RISE_WARNING:
      *value =
          (uint16_t)model->pending_alarm_config.rise_warning_x16_per_min;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_ALARM_RISE_CRITICAL:
      *value =
          (uint16_t)model->pending_alarm_config.rise_critical_x16_per_min;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_ALARM_ASSERT_SAMPLES:
      *value = model->pending_alarm_config.assert_samples;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_ALARM_CLEAR_SAMPLES:
      *value = model->pending_alarm_config.clear_samples;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_ALARM_HYSTERESIS:
      *value = (uint16_t)model->pending_alarm_config.hysteresis_x16;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_ALARM_BUZZER_ENABLE:
      *value = model->pending_alarm_config.buzzer_enable;
      return MODBUS_RESULT_OK;
    case BOARD_A_HOLDING_ALARM_RESERVED_0:
    case BOARD_A_HOLDING_ALARM_RESERVED_1:
      *value = 0U;
      return MODBUS_RESULT_OK;
    default:
      return MODBUS_RESULT_ILLEGAL_ADDRESS;
  }
}

static uint16_t alarm_flags(const board_a_alarm_state_t *state,
                            bool buzzer_active)
{
  uint16_t flags = 0U;

  if (state->valid) {
    flags |= 0x0001U;
  }
  if (state->latched) {
    flags |= 0x0002U;
  }
  if (state->acknowledged) {
    flags |= 0x0004U;
  }
  if (buzzer_active) {
    flags |= 0x0008U;
  }
  return flags;
}

static modbus_result_t read_alarm_register(
    const board_a_alarm_state_t *state, bool buzzer_active,
    uint16_t address, uint16_t *value)
{
  switch (address) {
    case BOARD_A_INPUT_ALARM_CONTRACT_REVISION:
      *value = BOARD_A_ALARM_INPUT_CONTRACT_REVISION;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_LEVEL:
      *value = (uint16_t)state->level;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_REASON:
      *value = (uint16_t)state->reason;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_FLAGS:
      *value = alarm_flags(state, buzzer_active);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_TRIGGER_PHASE:
      *value = (uint16_t)state->trigger_phase;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_DELTA_VALID:
      *value = state->delta_valid ? 1U : 0U;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_MAXIMUM_DELTA_X16:
      *value = (uint16_t)state->maximum_delta_x16;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_HOTTEST_TEMPERATURE_X16:
      *value = (uint16_t)state->hottest_temperature_x16;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_HOTTEST_PHASE:
      *value = (uint16_t)state->hottest_phase;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_TEMPERATURE_A_X16:
    case BOARD_A_INPUT_ALARM_TEMPERATURE_B_X16:
    case BOARD_A_INPUT_ALARM_TEMPERATURE_C_X16:
      *value = (uint16_t)state->temperature_x16[
          address - BOARD_A_INPUT_ALARM_TEMPERATURE_A_X16];
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_QUALITY_A:
    case BOARD_A_INPUT_ALARM_QUALITY_B:
    case BOARD_A_INPUT_ALARM_QUALITY_C:
      *value = state->quality[address - BOARD_A_INPUT_ALARM_QUALITY_A];
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_EVENT_ID_HI:
      *value = word_high16(state->event_id);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_EVENT_ID_LO:
      *value = word_low16(state->event_id);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_SAMPLE_ID_HI:
      *value = word_high16(state->sample_id);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_SAMPLE_ID_LO:
      *value = word_low16(state->sample_id);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_DURATION_SEC_HI:
      *value = word_high16(state->duration_sec);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_DURATION_SEC_LO:
      *value = word_low16(state->duration_sec);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_NOTICE_COUNT_HI:
      *value = word_high16(state->notice_count);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_NOTICE_COUNT_LO:
      *value = word_low16(state->notice_count);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_WARNING_COUNT_HI:
      *value = word_high16(state->warning_count);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_WARNING_COUNT_LO:
      *value = word_low16(state->warning_count);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_CRITICAL_COUNT_HI:
      *value = word_high16(state->critical_count);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_CRITICAL_COUNT_LO:
      *value = word_low16(state->critical_count);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_SENSOR_FAULT_COUNT_HI:
      *value = word_high16(state->sensor_fault_count);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ALARM_SENSOR_FAULT_COUNT_LO:
      *value = word_low16(state->sensor_fault_count);
      return MODBUS_RESULT_OK;
    default:
      return MODBUS_RESULT_ILLEGAL_ADDRESS;
  }
}

static modbus_result_t read_ds18b20_register(const board_a_model_t *model,
                                             uint16_t address,
                                             uint16_t *value)
{
  const board_a_sensor_snapshot_t *snapshot =
      &model->pending_sensor_snapshot;
  uint16_t offset;
  uint8_t sensor;

  if ((address < BOARD_A_INPUT_DS18B20_CONTRACT_REVISION) ||
      (address > BOARD_A_INPUT_DS18B20_ROM_SHORT_2)) {
    return MODBUS_RESULT_ILLEGAL_ADDRESS;
  }

  switch (address) {
    case BOARD_A_INPUT_DS18B20_CONTRACT_REVISION:
      *value = BOARD_A_DS18B20_CONTRACT_REVISION;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_DS18B20_SOURCE_TYPE:
      *value = ((model->data_source ==
                 BOARD_A_DATA_SOURCE_REAL_DS18B20) &&
                (snapshot->sample_id != 0U)) ?
          BOARD_A_DATA_SOURCE_REAL_DS18B20 : 0U;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_DS18B20_VALID_MASK:
      *value = snapshot->valid_mask;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_DS18B20_SAMPLE_ID_HI:
      *value = word_high16(snapshot->sample_id);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_DS18B20_SAMPLE_ID_LO:
      *value = word_low16(snapshot->sample_id);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_DS18B20_SENSOR_TYPE:
      *value = BOARD_A_SENSOR_TYPE_DS18B20;
      return MODBUS_RESULT_OK;
    default:
      break;
  }

  if ((address >= BOARD_A_INPUT_DS18B20_TEMPERATURE_0) &&
      (address <= BOARD_A_INPUT_DS18B20_TEMPERATURE_2)) {
    sensor = (uint8_t)(address - BOARD_A_INPUT_DS18B20_TEMPERATURE_0);
    *value = (uint16_t)snapshot->sensors[sensor].temperature_x16;
    return MODBUS_RESULT_OK;
  }
  if ((address >= BOARD_A_INPUT_DS18B20_QUALITY_0) &&
      (address <= BOARD_A_INPUT_DS18B20_QUALITY_2)) {
    sensor = (uint8_t)(address - BOARD_A_INPUT_DS18B20_QUALITY_0);
    *value = snapshot->sensors[sensor].quality;
    return MODBUS_RESULT_OK;
  }
  if ((address >= BOARD_A_INPUT_DS18B20_ERROR_0) &&
      (address <= BOARD_A_INPUT_DS18B20_ERROR_2)) {
    sensor = (uint8_t)(address - BOARD_A_INPUT_DS18B20_ERROR_0);
    *value = snapshot->sensors[sensor].error;
    return MODBUS_RESULT_OK;
  }
  if ((address >= BOARD_A_INPUT_DS18B20_SAMPLE_TIME_0_HI) &&
      (address <= BOARD_A_INPUT_DS18B20_SAMPLE_TIME_2_LO)) {
    offset = (uint16_t)(address - BOARD_A_INPUT_DS18B20_SAMPLE_TIME_0_HI);
    sensor = (uint8_t)(offset / 2U);
    *value = ((offset % 2U) == 0U) ?
        word_high16(snapshot->sensors[sensor].sample_time_ms) :
        word_low16(snapshot->sensors[sensor].sample_time_ms);
    return MODBUS_RESULT_OK;
  }
  if ((address >= BOARD_A_INPUT_DS18B20_ROM_SHORT_0) &&
      (address <= BOARD_A_INPUT_DS18B20_ROM_SHORT_2)) {
    sensor = (uint8_t)(address - BOARD_A_INPUT_DS18B20_ROM_SHORT_0);
    *value = snapshot->sensors[sensor].rom_short;
    return MODBUS_RESULT_OK;
  }

  return MODBUS_RESULT_ILLEGAL_ADDRESS;
}

static modbus_result_t read_input_register(
    const board_a_model_t *model, const board_a_alarm_state_t *alarm,
    uint16_t address, uint16_t *value, uint64_t now_us)
{
  if ((address >= BOARD_A_INPUT_ALARM_CONTRACT_REVISION) &&
      (address <= BOARD_A_INPUT_ALARM_SENSOR_FAULT_COUNT_LO)) {
    return read_alarm_register(alarm, model->alarm_buzzer_active,
                               address, value);
  }

  switch (address) {
    case BOARD_A_INPUT_DEVICE_TYPE:
      *value = 0x0001U;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_FIRMWARE_MAJOR:
      *value = BOARD_A_FIRMWARE_MAJOR;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_FIRMWARE_MINOR:
      *value = BOARD_A_FIRMWARE_MINOR;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_FIRMWARE_PATCH:
      *value = BOARD_A_FIRMWARE_PATCH;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_PROTOCOL_VERSION:
      *value = BOARD_A_PROTOCOL_VERSION;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_RUN_STATE:
      *value = (uint16_t)model->run_state;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ACTIVE_CONFIG_VALID:
      *value = model->active_config.valid ? 1U : 0U;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ACTIVE_PERIOD_SEC:
      *value = model->active_config.config.period_sec;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ACTIVE_CHANNEL_MASK:
      *value = model->active_config.config.channel_mask;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ACTIVE_RECORD_COUNT:
      *value = model->active_config.config.record_count;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ACTIVE_CONFIG_VERSION_HI:
      *value = word_high16(model->active_config.version);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ACTIVE_CONFIG_VERSION_LO:
      *value = word_low16(model->active_config.version);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_LAST_COMMAND:
      *value = model->last_command;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_COMMAND_RESULT:
      *value = model->command_result;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_LAST_COMMAND_ID_HI:
      *value = word_high16(model->last_command_id);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_LAST_COMMAND_ID_LO:
      *value = word_low16(model->last_command_id);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_DATA_SOURCE_TYPE:
      *value = model->data_source;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_SOURCE_TYPE:
      *value = model->snapshot.valid ?
          model->snapshot.source : model->data_source;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_SESSION_ID_HI:
      *value = word_high16(model->session_id);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_SESSION_ID_LO:
      *value = word_low16(model->session_id);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_PERSISTENCE_STATUS:
      *value = (uint16_t)model->persistence.save.state;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_STATUS:
      *value = (uint16_t)model->persistence.storage_state;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_RTOS_STATUS:
      *value = BOARD_A_STATUS_UNSUPPORTED;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_TIME_STATUS:
      *value = model->time.time_valid ?
          BOARD_A_TIME_STATUS_CALIBRATED : BOARD_A_TIME_STATUS_UNCALIBRATED;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_SCHEDULE_STATE:
      *value = model->time.schedule_armed ?
          BOARD_A_SCHEDULE_STATE_WAITING : BOARD_A_SCHEDULE_STATE_NONE;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_CURRENT_UTC_SECONDS_HI:
      *value = word_high16(current_utc_seconds(model, now_us));
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_CURRENT_UTC_SECONDS_LO:
      *value = word_low16(current_utc_seconds(model, now_us));
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI:
      *value = word_high16(model->time.schedule_armed ?
                           model->time.schedule_target_seconds :
                           BOARD_A_TIME_INVALID_SECONDS);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ARMED_START_UTC_SECONDS_LO:
      *value = word_low16(model->time.schedule_armed ?
                          model->time.schedule_target_seconds :
                          BOARD_A_TIME_INVALID_SECONDS);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_RECORDS_THIS_RUN_HI:
      *value = word_high16(model->records_this_run);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_RECORDS_THIS_RUN_LO:
      *value = word_low16(model->records_this_run);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_SNAPSHOT_VALID:
      *value = model->snapshot.valid ? 1U : 0U;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_RECORD_SEQUENCE_HI:
      *value = word_high16(model->snapshot.sequence);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_RECORD_SEQUENCE_LO:
      *value = word_low16(model->snapshot.sequence);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_CHANNEL_COUNT:
      *value = model->snapshot.channel_count;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_CHANNEL_0:
    case BOARD_A_INPUT_CHANNEL_1:
    case BOARD_A_INPUT_CHANNEL_2:
    case BOARD_A_INPUT_CHANNEL_3:
      *value = model->snapshot.channel_values[
          address - BOARD_A_INPUT_CHANNEL_0];
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_CHANNEL_0_QUALITY:
    case BOARD_A_INPUT_CHANNEL_1_QUALITY:
    case BOARD_A_INPUT_CHANNEL_2_QUALITY:
    case BOARD_A_INPUT_CHANNEL_3_QUALITY:
      *value = model->snapshot.channel_quality[
          address - BOARD_A_INPUT_CHANNEL_0_QUALITY];
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_LAST_SAMPLE_TRIGGER:
      *value = model->snapshot.trigger;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_UNIT_CODE:
      *value = (model->snapshot.valid &&
                (model->snapshot.source ==
                 BOARD_A_DATA_SOURCE_REAL_DS18B20)) ?
          BOARD_A_UNIT_TEMPERATURE_X16 : BOARD_A_UNIT_COUNT;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_RX_FRAMES_HI:
      *value = word_high16(model->stats.rx_frames);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_RX_FRAMES_LO:
      *value = word_low16(model->stats.rx_frames);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_CRC_ERRORS_HI:
      *value = word_high16(model->stats.crc_errors);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_CRC_ERRORS_LO:
      *value = word_low16(model->stats.crc_errors);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ADDRESS_MISMATCH_HI:
      *value = word_high16(model->stats.address_mismatch);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_ADDRESS_MISMATCH_LO:
      *value = word_low16(model->stats.address_mismatch);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_OVERLONG_FRAMES_HI:
      *value = word_high16(model->stats.overlong_frames);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_OVERLONG_FRAMES_LO:
      *value = word_low16(model->stats.overlong_frames);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_MALFORMED_FRAMES_HI:
      *value = word_high16(model->stats.malformed_frames);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_MALFORMED_FRAMES_LO:
      *value = word_low16(model->stats.malformed_frames);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_TX_RESPONSES_HI:
      *value = word_high16(model->stats.tx_responses);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_TX_RESPONSES_LO:
      *value = word_low16(model->stats.tx_responses);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_BROADCAST_WRITES_HI:
      *value = word_high16(model->stats.broadcast_writes);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_BROADCAST_WRITES_LO:
      *value = word_low16(model->stats.broadcast_writes);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_BROADCAST_READS_HI:
      *value = word_high16(model->stats.broadcast_reads);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_BROADCAST_READS_LO:
      *value = word_low16(model->stats.broadcast_reads);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_EXCEPTION_RESPONSES_HI:
      *value = word_high16(model->stats.exception_responses);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_EXCEPTION_RESPONSES_LO:
      *value = word_low16(model->stats.exception_responses);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_SCHEDULER_MISSED_HI:
      *value = word_high16(model->stats.scheduler_missed);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_SCHEDULER_MISSED_LO:
      *value = word_low16(model->stats.scheduler_missed);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_DROPPED_HI:
      *value = word_high16(model->persistence.storage.dropped);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_DROPPED_LO:
      *value = word_low16(model->persistence.storage.dropped);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_PERSISTENCE_ERRORS_HI:
      *value = word_high16(model->stats.persistence_errors);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_PERSISTENCE_ERRORS_LO:
      *value = word_low16(model->stats.persistence_errors);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_DEVICE_FAULTS_HI:
      *value = word_high16(model->stats.device_faults);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_DEVICE_FAULTS_LO:
      *value = word_low16(model->stats.device_faults);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_CONTRACT_REVISION:
      *value = BOARD_A_PERSISTENCE_CONTRACT_REVISION;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_SAVE_STATE:
      *value = (uint16_t)model->persistence.save.state;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_SAVE_COMMAND_ID_HI:
      *value = (uint16_t)((model->persistence.save.state ==
                           BOARD_A_SAVE_IDLE) ?
                          0U :
                          word_high16(
                              model->persistence.save.request.command_id));
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_SAVE_COMMAND_ID_LO:
      *value = (uint16_t)((model->persistence.save.state ==
                           BOARD_A_SAVE_IDLE) ?
                          0U :
                          word_low16(
                              model->persistence.save.request.command_id));
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_SAVE_CONFIG_VERSION_HI:
      *value = (uint16_t)((model->persistence.save.state ==
                           BOARD_A_SAVE_IDLE) ?
                          0U :
                          word_high16(
                              model->persistence.save.request.config_version));
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_SAVE_CONFIG_VERSION_LO:
      *value = (uint16_t)((model->persistence.save.state ==
                           BOARD_A_SAVE_IDLE) ?
                          0U :
                          word_low16(
                              model->persistence.save.request.config_version));
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_SAVE_ERROR:
      *value = (uint16_t)model->persistence.save.error;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_CONFIG_LOAD_STATE:
      *value = (uint16_t)model->persistence.config_load_state;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_EXTENDED_STORAGE_STATE:
      *value = (uint16_t)model->persistence.storage_state;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_EXTENDED_STORAGE_ERROR:
      *value = (uint16_t)model->persistence.storage_error;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_QUEUED:
      *value = model->persistence.storage.count;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_HIGH_WATER:
      *value = model->persistence.storage.high_water;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_GENERATED_HI:
      *value = word_high16(model->persistence.storage.generated);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_GENERATED_LO:
      *value = word_low16(model->persistence.storage.generated);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_SYNCED_HI:
      *value = word_high16(model->persistence.storage.synced);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_SYNCED_LO:
      *value = word_low16(model->persistence.storage.synced);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_DROPPED_EXT_HI:
      *value = word_high16(model->persistence.storage.dropped);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_DROPPED_EXT_LO:
      *value = word_low16(model->persistence.storage.dropped);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_UNCERTAIN_HI:
      *value = word_high16(model->persistence.storage.uncertain);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_UNCERTAIN_LO:
      *value = word_low16(model->persistence.storage.uncertain);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_IN_FLIGHT:
      *value = model->persistence.storage.in_flight;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_DRAIN_STATE:
      *value = (uint16_t)model->persistence.storage.drain_state;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_LAST_SYNCED_SEQ_HI:
      *value = word_high16(model->persistence.storage.last_synced_valid ?
                           model->persistence.storage.last_synced_seq : 0U);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_LAST_SYNCED_SEQ_LO:
      *value = word_low16(model->persistence.storage.last_synced_valid ?
                          model->persistence.storage.last_synced_seq : 0U);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_LAST_SYNCED_FILE_HI:
      *value = word_high16(model->persistence.storage.last_synced_valid ?
                           model->persistence.storage.last_synced_file : 0U);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_LAST_SYNCED_FILE_LO:
      *value = word_low16(model->persistence.storage.last_synced_valid ?
                          model->persistence.storage.last_synced_file : 0U);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_LAST_SYNCED_DATE_HI:
      *value = word_high16(model->persistence.storage.last_synced_valid ?
                           model->persistence.storage.last_synced_date : 0U);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_LAST_SYNCED_DATE_LO:
      *value = word_low16(model->persistence.storage.last_synced_valid ?
                          model->persistence.storage.last_synced_date : 0U);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_EXTENDED_ACTIVE_CONFIG_VERSION_HI:
      *value = word_high16(model->active_config.version);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_EXTENDED_ACTIVE_CONFIG_VERSION_LO:
      *value = word_low16(model->active_config.version);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_DRAIN_GENERATION_HI:
      *value = word_high16(model->persistence.storage.drain_generation);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_DRAIN_GENERATION_LO:
      *value = word_low16(model->persistence.storage.drain_generation);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_ERRORS_HI:
      *value = word_high16(model->persistence.storage.storage_errors);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_ERRORS_LO:
      *value = word_low16(model->persistence.storage.storage_errors);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_LOAD_SEQUENCE_HI:
      *value = word_high16(model->persistence.load_sequence);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_LOAD_SEQUENCE_LO:
      *value = word_low16(model->persistence.load_sequence);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_CAPTURED_PERIOD_HI:
      *value = (uint16_t)((model->persistence.save.state ==
                           BOARD_A_SAVE_IDLE) ?
                          0U :
                          word_high16(
                              model->persistence.save.request.config.period_sec));
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_CAPTURED_PERIOD_LO:
      *value = (uint16_t)((model->persistence.save.state ==
                           BOARD_A_SAVE_IDLE) ?
                          0U :
                          word_low16(
                              model->persistence.save.request.config.period_sec));
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_CAPTURED_MASK:
      *value = (model->persistence.save.state == BOARD_A_SAVE_IDLE) ?
          0U : model->persistence.save.request.config.channel_mask;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_CAPTURED_COUNT:
      *value = (model->persistence.save.state == BOARD_A_SAVE_IDLE) ?
          0U : model->persistence.save.request.config.record_count;
      return MODBUS_RESULT_OK;
    case 0x00A8:
    case 0x00A9:
    case 0x00AA:
    case 0x00AB:
    case 0x00AC:
    case 0x00AD:
    case 0x00AE:
    case 0x00AF:
      *value = 0U;
      return MODBUS_RESULT_OK;
    default:
      return read_ds18b20_register(model, address, value);
  }
}

void board_a_model_init(board_a_model_t *model, uint32_t session_id)
{
  uint8_t channel;

  if (model == NULL) {
    return;
  }

  model->pending_config.period_sec = BOARD_A_DEFAULT_PERIOD_SEC;
  model->pending_config.channel_mask = BOARD_A_DEFAULT_CHANNEL_MASK;
  model->pending_config.record_count = BOARD_A_DEFAULT_RECORD_COUNT;
  board_a_alarm_default_config(&model->pending_alarm_config);
  model->active_config.valid = true;
  model->active_config.version = 0U;
  model->active_config.config = model->pending_config;
  model->active_config.alarm_config = model->pending_alarm_config;
  model->snapshot.valid = false;
  model->snapshot.sequence = 0U;
  model->snapshot.sample_time_us = 0U;
  model->snapshot.source = BOARD_A_DATA_SOURCE_TEST;
  model->snapshot.channel_count = 1U;
  model->snapshot.trigger = BOARD_A_SAMPLE_TRIGGER_NONE;
  for (channel = 0U; channel < BOARD_A_MAX_CHANNELS; ++channel) {
    model->snapshot.channel_values[channel] = 0U;
    model->snapshot.channel_quality[channel] = BOARD_A_QUALITY_UNAVAILABLE;
  }
  reset_sensor_snapshot(&model->snapshot.sensors);
  reset_sensor_snapshot(&model->pending_sensor_snapshot);
  memset(&model->sensor_map, 0, sizeof(model->sensor_map));
  memset(&model->alarm_state, 0, sizeof(model->alarm_state));
  model->alarm_state.level = BOARD_A_ALARM_UNKNOWN;
  model->alarm_state.reason = BOARD_A_ALARM_REASON_NONE;
  model->alarm_state.trigger_phase = BOARD_A_ALARM_PHASE_NONE;
  model->alarm_state.hottest_phase = BOARD_A_ALARM_PHASE_NONE;
  model->alarm_state.coldest_phase = BOARD_A_ALARM_PHASE_NONE;
  model->alarm_state.maximum_rise_phase = BOARD_A_ALARM_PHASE_NONE;
  model->alarm_event = false;
  model->alarm_event_type = BOARD_A_ALARM_EVENT_NONE;
  model->alarm_event_id = 0U;
  model->alarm_event_time_ms = 0U;
  model->alarm_buzzer_active = false;
  model->data_source = BOARD_A_DATA_SOURCE_TEST;
  model->run_state = BOARD_A_RUN_STOPPED;
  model->session_id = session_id;
  model->command_register = BOARD_A_COMMAND_NONE;
  model->command_id_hi = 0U;
  model->command_id_lo = 0U;
  model->last_command = BOARD_A_COMMAND_NONE;
  model->command_result = BOARD_A_COMMAND_RESULT_NONE;
  model->last_command_id = 0U;
  model->single_id_count = 0U;
  model->single_id_next = 0U;
  model->alarm_ack_id_count = 0U;
  model->alarm_ack_id_next = 0U;
  model->alarm_ack_requested = false;
  model->records_this_run = 0U;
  model->next_sample_us = 0U;
  model->start_pending = false;
  model->time.pending_utc_seconds = 0U;
  model->time.pending_start_utc_seconds = 0U;
  model->time.time_valid = false;
  model->time.utc_anchor_seconds = 0U;
  model->time.utc_anchor_us = 0U;
  model->time.schedule_armed = false;
  model->time.schedule_target_seconds = 0U;
  model->time.schedule_deadline_us = 0U;
  model->time.schedule_start_late_us = 0U;
  model->time.schedule_start_count = 0U;
  model->stats.rx_frames = 0U;
  model->stats.crc_errors = 0U;
  model->stats.address_mismatch = 0U;
  model->stats.overlong_frames = 0U;
  model->stats.malformed_frames = 0U;
  model->stats.tx_responses = 0U;
  model->stats.broadcast_writes = 0U;
  model->stats.broadcast_reads = 0U;
  model->stats.exception_responses = 0U;
  model->stats.scheduler_missed = 0U;
  model->stats.storage_dropped = 0U;
  model->stats.persistence_errors = 0U;
  model->stats.device_faults = 0U;
  board_a_persistence_init(&model->persistence);
}

bool board_a_model_set_data_source(board_a_model_t *model, uint16_t source)
{
  if ((model == NULL) || !data_source_is_valid(source)) {
    return false;
  }
  model->data_source = source;
  return true;
}

void board_a_model_publish_sensor_snapshot(
    board_a_model_t *model,
    const board_a_sensor_snapshot_t *snapshot)
{
  if ((model == NULL) || (snapshot == NULL)) {
    return;
  }
  model->pending_sensor_snapshot = *snapshot;
}

bool board_a_model_publish_sensor_map(
    board_a_model_t *model, const board_a_sensor_map_t *map)
{
  uint8_t index;

  if ((model == NULL) || (map == NULL) ||
      ((map->valid_mask & (uint8_t)~0x07U) != 0U)) {
    return false;
  }
  model->sensor_map = *map;
  for (index = 0U; index < BOARD_A_SENSOR_COUNT; ++index) {
    model->sensor_map.bindings[index].bound =
        (map->valid_mask & (uint8_t)(1U << index)) != 0U;
  }
  return true;
}

bool board_a_model_copy_sensor_map(
    const board_a_model_t *model, board_a_sensor_map_t *map)
{
  if ((model == NULL) || (map == NULL)) {
    return false;
  }
  *map = model->sensor_map;
  return true;
}

void board_a_model_publish_alarm_state(
    board_a_model_t *model, const board_a_alarm_state_t *state)
{
  if ((model == NULL) || (state == NULL)) {
    return;
  }
  model->alarm_state = *state;
}

bool board_a_model_copy_alarm_state(
    const board_a_model_t *model, board_a_alarm_state_t *state)
{
  if ((model == NULL) || (state == NULL)) {
    return false;
  }
  *state = model->alarm_state;
  return true;
}

void board_a_model_publish_alarm_result(
    board_a_model_t *model, const board_a_alarm_result_t *result)
{
  if ((model == NULL) || (result == NULL)) {
    return;
  }
  model->alarm_state = result->state;
  model->alarm_event = result->event;
  model->alarm_event_type = result->event_type;
  model->alarm_event_id = result->event_id;
  model->alarm_event_time_ms = result->event_time_ms;
}

bool board_a_model_copy_alarm_event(
    const board_a_model_t *model, board_a_alarm_result_t *result)
{
  if ((model == NULL) || (result == NULL)) {
    return false;
  }
  memset(result, 0, sizeof(*result));
  result->event = model->alarm_event;
  result->event_type = model->alarm_event_type;
  result->event_id = model->alarm_event_id;
  result->event_time_ms = model->alarm_event_time_ms;
  result->state = model->alarm_state;
  return true;
}

bool board_a_model_copy_alarm_config(
    const board_a_model_t *model, board_a_alarm_config_t *config)
{
  if ((model == NULL) || (config == NULL)) {
    return false;
  }
  *config = model->active_config.alarm_config;
  return true;
}

void board_a_model_set_alarm_buzzer_active(
    board_a_model_t *model, bool active)
{
  if (model != NULL) {
    model->alarm_buzzer_active = active;
  }
}

bool board_a_model_take_alarm_ack_request(board_a_model_t *model)
{
  bool requested;

  if (model == NULL) {
    return false;
  }
  requested = model->alarm_ack_requested;
  model->alarm_ack_requested = false;
  return requested;
}

modbus_result_t board_a_model_read_registers(void *context,
                                             modbus_register_space_t space,
                                             uint16_t address,
                                             uint16_t quantity,
                                             uint16_t *values,
                                             uint64_t now_us)
{
  board_a_model_t *model = (board_a_model_t *)context;
  board_a_alarm_state_t alarm_snapshot;
  uint16_t index;
  modbus_result_t result;

  if ((model == NULL) || (values == NULL) || (quantity == 0U)) {
    return MODBUS_RESULT_ILLEGAL_VALUE;
  }

  alarm_snapshot = model->alarm_state;
  for (index = 0U; index < quantity; ++index) {
    if (space == MODBUS_REGISTER_HOLDING) {
      result = read_holding_register(model,
                                     (uint16_t)(address + index),
                                     &values[index]);
    } else if (space == MODBUS_REGISTER_INPUT) {
      result = read_input_register(model, &alarm_snapshot,
                                   (uint16_t)(address + index),
                                   &values[index], now_us);
    } else {
      return MODBUS_RESULT_ILLEGAL_ADDRESS;
    }

    if (result != MODBUS_RESULT_OK) {
      return result;
    }
  }

  return MODBUS_RESULT_OK;
}

modbus_result_t board_a_model_write_registers(void *context,
                                              uint16_t address,
                                              const uint16_t *values,
                                              uint16_t quantity,
                                              uint64_t now_us)
{
  board_a_model_t *model = (board_a_model_t *)context;
  board_a_config_t candidate_config;
  board_a_alarm_config_t candidate_alarm_config;
  uint16_t command_register;
  uint16_t command_id_hi;
  uint16_t command_id_lo;
  uint32_t command_id;
  uint16_t index;
  bool writes_command;

  if ((model == NULL) || (values == NULL) || (quantity == 0U)) {
    return MODBUS_RESULT_ILLEGAL_VALUE;
  }

  if (address <= BOARD_A_HOLDING_CFG_RECORD_COUNT) {
    if ((uint32_t)address + quantity >
        (uint32_t)BOARD_A_HOLDING_CFG_RECORD_COUNT + 1U) {
      return MODBUS_RESULT_ILLEGAL_ADDRESS;
    }
    candidate_config = model->pending_config;
    for (index = 0U; index < quantity; ++index) {
      switch (address + index) {
        case BOARD_A_HOLDING_CFG_PERIOD_SEC:
          candidate_config.period_sec = values[index];
          break;
        case BOARD_A_HOLDING_CFG_CHANNEL_MASK:
          candidate_config.channel_mask = values[index];
          break;
        case BOARD_A_HOLDING_CFG_RECORD_COUNT:
          candidate_config.record_count = values[index];
          break;
        default:
          return MODBUS_RESULT_ILLEGAL_ADDRESS;
      }
    }
    if (!config_is_valid(&candidate_config)) {
      return MODBUS_RESULT_ILLEGAL_VALUE;
    }
    model->pending_config = candidate_config;
    return MODBUS_RESULT_OK;
  }

  if ((address >= BOARD_A_HOLDING_ALARM_CONFIG_REVISION) &&
      (address <= BOARD_A_HOLDING_ALARM_RESERVED_1)) {
    if ((address < BOARD_A_HOLDING_ALARM_PHASE_NOTICE) ||
        (address > BOARD_A_HOLDING_ALARM_BUZZER_ENABLE) ||
        ((uint32_t)address + quantity >
         (uint32_t)BOARD_A_HOLDING_ALARM_BUZZER_ENABLE + 1U)) {
      return MODBUS_RESULT_ILLEGAL_ADDRESS;
    }

    candidate_alarm_config = model->pending_alarm_config;
    for (index = 0U; index < quantity; ++index) {
      switch (address + index) {
        case BOARD_A_HOLDING_ALARM_PHASE_NOTICE:
          candidate_alarm_config.phase_notice_x16 =
              (int16_t)values[index];
          break;
        case BOARD_A_HOLDING_ALARM_PHASE_WARNING:
          candidate_alarm_config.phase_warning_x16 =
              (int16_t)values[index];
          break;
        case BOARD_A_HOLDING_ALARM_PHASE_CRITICAL:
          candidate_alarm_config.phase_critical_x16 =
              (int16_t)values[index];
          break;
        case BOARD_A_HOLDING_ALARM_DELTA_NOTICE:
          candidate_alarm_config.delta_notice_x16 =
              (int16_t)values[index];
          break;
        case BOARD_A_HOLDING_ALARM_DELTA_WARNING:
          candidate_alarm_config.delta_warning_x16 =
              (int16_t)values[index];
          break;
        case BOARD_A_HOLDING_ALARM_DELTA_CRITICAL:
          candidate_alarm_config.delta_critical_x16 =
              (int16_t)values[index];
          break;
        case BOARD_A_HOLDING_ALARM_RISE_NOTICE:
          candidate_alarm_config.rise_notice_x16_per_min =
              (int16_t)values[index];
          break;
        case BOARD_A_HOLDING_ALARM_RISE_WARNING:
          candidate_alarm_config.rise_warning_x16_per_min =
              (int16_t)values[index];
          break;
        case BOARD_A_HOLDING_ALARM_RISE_CRITICAL:
          candidate_alarm_config.rise_critical_x16_per_min =
              (int16_t)values[index];
          break;
        case BOARD_A_HOLDING_ALARM_ASSERT_SAMPLES:
          candidate_alarm_config.assert_samples = values[index];
          break;
        case BOARD_A_HOLDING_ALARM_CLEAR_SAMPLES:
          candidate_alarm_config.clear_samples = values[index];
          break;
        case BOARD_A_HOLDING_ALARM_HYSTERESIS:
          candidate_alarm_config.hysteresis_x16 = (int16_t)values[index];
          break;
        case BOARD_A_HOLDING_ALARM_BUZZER_ENABLE:
          if (values[index] > 1U) {
            return MODBUS_RESULT_ILLEGAL_VALUE;
          }
          candidate_alarm_config.buzzer_enable = (uint8_t)values[index];
          break;
        default:
          return MODBUS_RESULT_ILLEGAL_ADDRESS;
      }
    }
    if (!board_a_alarm_validate_config(&candidate_alarm_config)) {
      return MODBUS_RESULT_ILLEGAL_VALUE;
    }
    model->pending_alarm_config = candidate_alarm_config;
    return MODBUS_RESULT_OK;
  }

  if ((address >= BOARD_A_HOLDING_PENDING_UTC_SECONDS_HI) &&
      (address <= BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_LO)) {
    uint32_t pending_utc = model->time.pending_utc_seconds;
    uint32_t pending_start = model->time.pending_start_utc_seconds;

    if ((uint32_t)address + quantity >
        (uint32_t)BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_LO + 1U) {
      return MODBUS_RESULT_ILLEGAL_ADDRESS;
    }
    for (index = 0U; index < quantity; ++index) {
      switch (address + index) {
        case BOARD_A_HOLDING_PENDING_UTC_SECONDS_HI:
          pending_utc = (pending_utc & 0x0000FFFFU) |
                        ((uint32_t)values[index] << 16U);
          break;
        case BOARD_A_HOLDING_PENDING_UTC_SECONDS_LO:
          pending_utc = (pending_utc & 0xFFFF0000U) |
                        (uint32_t)values[index];
          break;
        case BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_HI:
          pending_start = (pending_start & 0x0000FFFFU) |
                          ((uint32_t)values[index] << 16U);
          break;
        case BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_LO:
          pending_start = (pending_start & 0xFFFF0000U) |
                          (uint32_t)values[index];
          break;
        default:
          return MODBUS_RESULT_ILLEGAL_ADDRESS;
      }
    }
    if ((pending_utc == BOARD_A_TIME_INVALID_SECONDS) ||
        (pending_start == BOARD_A_TIME_INVALID_SECONDS)) {
      return MODBUS_RESULT_ILLEGAL_VALUE;
    }
    model->time.pending_utc_seconds = pending_utc;
    model->time.pending_start_utc_seconds = pending_start;
    return MODBUS_RESULT_OK;
  }

  if ((address < BOARD_A_HOLDING_COMMAND) ||
      (address >
       (uint16_t)(BOARD_A_HOLDING_COMMAND_ID_LO + 1U))) {
    return MODBUS_RESULT_ILLEGAL_ADDRESS;
  }
  if ((uint32_t)address + quantity >
      (uint32_t)BOARD_A_HOLDING_COMMAND_ID_LO + 1U) {
    return MODBUS_RESULT_ILLEGAL_ADDRESS;
  }

  command_register = model->command_register;
  command_id_hi = model->command_id_hi;
  command_id_lo = model->command_id_lo;
  writes_command = false;
  for (index = 0U; index < quantity; ++index) {
    switch (address + index) {
      case BOARD_A_HOLDING_COMMAND:
        command_register = values[index];
        writes_command = true;
        break;
      case BOARD_A_HOLDING_COMMAND_ID_HI:
        command_id_hi = values[index];
        break;
      case BOARD_A_HOLDING_COMMAND_ID_LO:
        command_id_lo = values[index];
        break;
      default:
        return MODBUS_RESULT_ILLEGAL_ADDRESS;
    }
  }

  if (writes_command &&
      ((command_register < BOARD_A_COMMAND_APPLY_CONFIG) ||
       (command_register > BOARD_A_COMMAND_ACK_ALARM))) {
    return MODBUS_RESULT_ILLEGAL_VALUE;
  }

  command_id = ((uint32_t)command_id_hi << 16U) | (uint32_t)command_id_lo;
  model->command_register = command_register;
  model->command_id_hi = command_id_hi;
  model->command_id_lo = command_id_lo;

  if (writes_command) {
    return execute_command(model, command_register, command_id, now_us);
  }
  return MODBUS_RESULT_OK;
}

void board_a_model_tick(board_a_model_t *model, uint64_t now_us)
{
  uint64_t period_us;
  uint64_t missed;

  if (model == NULL) {
    return;
  }

  if (model->time.schedule_armed &&
      (now_us >= model->time.schedule_deadline_us)) {
    uint64_t late_us = now_us - model->time.schedule_deadline_us;

    if (late_us > (uint64_t)UINT32_MAX) {
      late_us = (uint64_t)UINT32_MAX;
    }
    if ((uint32_t)late_us > model->time.schedule_start_late_us) {
      model->time.schedule_start_late_us = (uint32_t)late_us;
    }
    model->time.schedule_start_count++;
    /*
     * The wake that crosses the target starts the run exactly once with the
     * active configuration at that instant. Clearing the schedule first keeps
     * later wakes from replaying missed periods.
     */
    model->time.schedule_armed = false;
    model->time.schedule_target_seconds = 0U;
    model->time.schedule_deadline_us = 0U;
    if (model->active_config.valid) {
      model->run_state = BOARD_A_RUN_RUNNING;
      model->records_this_run = 0U;
      model->next_sample_us = 0U;
      model->start_pending = true;
    }
  }

  if (model->run_state != BOARD_A_RUN_RUNNING) {
    return;
  }

  period_us = (uint64_t)model->active_config.config.period_sec * 1000000ULL;
  if (period_us == 0U) {
    return;
  }

  if (model->start_pending) {
    generate_record(model, BOARD_A_SAMPLE_TRIGGER_PERIODIC, now_us, now_us);
    model->records_this_run++;
    model->start_pending = false;
    model->next_sample_us = now_us + period_us;
  } else if ((model->next_sample_us != 0U) &&
             (now_us >= model->next_sample_us)) {
    missed = (now_us - model->next_sample_us) / period_us;
    model->stats.scheduler_missed += (uint32_t)missed;
    generate_record(model, BOARD_A_SAMPLE_TRIGGER_PERIODIC,
                    model->next_sample_us, now_us);
    model->records_this_run++;
    model->next_sample_us = now_us + period_us;
  }

  if ((model->active_config.config.record_count != 0U) &&
      (model->records_this_run >=
       model->active_config.config.record_count)) {
    model->run_state = BOARD_A_RUN_STOPPED;
    model->start_pending = false;
    model->next_sample_us = 0U;
    board_a_persistence_request_drain(&model->persistence);
  }
}

void board_a_model_apply_loaded_config(
    board_a_model_t *model, const board_a_persisted_config_t *config,
    uint32_t sequence)
{
  if ((model == NULL) || (config == NULL)) {
    return;
  }
  if ((config->period_sec < BOARD_A_PERIOD_MIN_SEC) ||
      (config->period_sec > BOARD_A_PERIOD_MAX_SEC) ||
      (config->channel_mask < BOARD_A_CHANNEL_MASK_MIN) ||
      (config->channel_mask > BOARD_A_CHANNEL_MASK_MAX) ||
      !board_a_alarm_validate_config(&config->alarm)) {
    board_a_model_note_config_load(
        model, BOARD_A_CONFIG_LOAD_DEFAULT_ERROR, 0U);
    return;
  }
  model->pending_config.period_sec = config->period_sec;
  model->pending_config.channel_mask = config->channel_mask;
  model->pending_config.record_count = config->record_count;
  model->pending_alarm_config = config->alarm;
  model->active_config.config = model->pending_config;
  model->active_config.alarm_config = model->pending_alarm_config;
  model->active_config.valid = true;
  memset(&model->sensor_map, 0, sizeof(model->sensor_map));
  model->sensor_map.valid_mask = config->sensor_valid_mask;
  {
    uint8_t index;
    for (index = 0U; index < BOARD_A_SENSOR_COUNT; ++index) {
      memcpy(model->sensor_map.bindings[index].rom,
             config->sensor_roms[index], DS18B20_ROM_SIZE);
      model->sensor_map.bindings[index].bound =
          (config->sensor_valid_mask & (uint8_t)(1U << index)) != 0U;
      model->sensor_map.bindings[index].rom_short =
          (uint16_t)((uint16_t)config->sensor_roms[index][2] << 8U) |
          (uint16_t)config->sensor_roms[index][1];
    }
  }
  board_a_persistence_note_load(&model->persistence,
                                BOARD_A_CONFIG_LOAD_SUCCESS, sequence);
}

void board_a_model_note_config_load(
    board_a_model_t *model, board_a_config_load_state_t state,
    uint32_t sequence)
{
  if (model == NULL) {
    return;
  }
  if (state == BOARD_A_CONFIG_LOAD_DEFAULT_ERROR) {
    model->stats.persistence_errors++;
  }
  board_a_persistence_note_load(&model->persistence, state, sequence);
}

int board_a_model_claim_save(board_a_model_t *model,
                             board_a_save_request_t *request)
{
  return (model == NULL) ? 0 :
      board_a_persistence_claim_save(&model->persistence, request);
}

void board_a_model_complete_save(
    board_a_model_t *model, int success, board_a_save_error_t error,
    uint32_t raw_error)
{
  if (model == NULL) {
    return;
  }
  if (success == 0) {
    model->stats.persistence_errors++;
  }
  board_a_persistence_complete_save(&model->persistence, success, error,
                                    raw_error);
}

int board_a_model_pop_record(
    board_a_model_t *model, board_a_record_format_record_t *record)
{
  return (model == NULL) ? 0 :
      board_a_persistence_queue_pop(&model->persistence, record);
}

void board_a_model_requeue_record(
    board_a_model_t *model,
    const board_a_record_format_record_t *record)
{
  if (model == NULL) {
    return;
  }
  board_a_persistence_queue_requeue(&model->persistence, record);
}

void board_a_model_complete_record(
    board_a_model_t *model,
    const board_a_record_format_record_t *record,
    board_a_record_complete_result_t result)
{
  if (model == NULL) {
    return;
  }
  board_a_persistence_complete_record(&model->persistence, record, result);
}

void board_a_model_set_storage_state(
    board_a_model_t *model, board_a_storage_state_t state,
    board_a_storage_error_t error, uint32_t raw_error)
{
  if (model == NULL) {
    return;
  }
  board_a_persistence_set_storage_state(&model->persistence, state, error,
                                        raw_error);
}

void board_a_model_note_storage_error(
    board_a_model_t *model, board_a_storage_error_t error,
    uint32_t raw_error)
{
  if (model == NULL) {
    return;
  }
  board_a_persistence_note_storage_error(&model->persistence, error,
                                         raw_error);
}

void board_a_model_complete_drain(board_a_model_t *model,
                                  uint32_t generation, int success)
{
  if (model == NULL) {
    return;
  }
  board_a_persistence_complete_drain(&model->persistence, generation,
                                     success);
}
