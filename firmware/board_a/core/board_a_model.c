#include "board_a_model.h"

#define BOARD_A_FIRMWARE_MAJOR 1U
#define BOARD_A_FIRMWARE_MINOR 0U
#define BOARD_A_FIRMWARE_PATCH 0U
#define BOARD_A_PROTOCOL_VERSION 2U

#define BOARD_A_DEFAULT_PERIOD_SEC 10U
#define BOARD_A_DEFAULT_CHANNEL_MASK 0x0001U
#define BOARD_A_DEFAULT_RECORD_COUNT 0U

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

static void generate_record(board_a_model_t *model,
                            board_a_sample_trigger_t trigger,
                            uint64_t now_us)
{
  uint8_t channel;

  model->snapshot.valid = true;
  model->snapshot.sequence++;
  model->snapshot.sample_time_us = now_us;
  model->snapshot.trigger = (uint16_t)trigger;
  model->snapshot.channel_count =
      channel_count_from_mask(model->active_config.config.channel_mask);

  /*
   * TEST data is deterministic and intentionally not tied to a real sensor.
   * Counts wrap naturally at 16 bits; the record sequence remains the identity.
   */
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

static void set_command_status(board_a_model_t *model,
                               uint16_t command,
                               uint16_t result,
                               uint32_t command_id)
{
  model->last_command = command;
  model->command_result = result;
  model->last_command_id = command_id;
}

static bool single_id_is_known(const board_a_model_t *model, uint32_t id)
{
  uint8_t index;

  for (index = 0U; index < model->single_id_count; ++index) {
    if (model->single_ids[index] == id) {
      return true;
    }
  }
  return false;
}

static void remember_single_id(board_a_model_t *model, uint32_t id)
{
  model->single_ids[model->single_id_next] = id;
  model->single_id_next =
      (uint8_t)((model->single_id_next + 1U) %
                BOARD_A_SINGLE_DEDUP_CAPACITY);
  if (model->single_id_count < BOARD_A_SINGLE_DEDUP_CAPACITY) {
    model->single_id_count++;
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
      if (!config_is_valid(&model->pending_config)) {
        set_command_status(model, command, BOARD_A_COMMAND_RESULT_REJECTED,
                           command_id);
        model->stats.device_faults++;
        return MODBUS_RESULT_DEVICE_FAILURE;
      }
      model->active_config.config = model->pending_config;
      model->active_config.valid = true;
      model->active_config.version++;
      set_command_status(model, command, BOARD_A_COMMAND_RESULT_ACCEPTED,
                         command_id);
      return MODBUS_RESULT_OK;

    case BOARD_A_COMMAND_SAVE_CONFIG:
      model->stats.persistence_errors++;
      set_command_status(model, command, BOARD_A_COMMAND_RESULT_UNSUPPORTED,
                         command_id);
      return MODBUS_RESULT_DEVICE_FAILURE;

    case BOARD_A_COMMAND_START:
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
      set_command_status(model, command, BOARD_A_COMMAND_RESULT_ACCEPTED,
                         command_id);
      return MODBUS_RESULT_OK;

    case BOARD_A_COMMAND_SINGLE:
      if (single_id_is_known(model, command_id)) {
        set_command_status(model, command, BOARD_A_COMMAND_RESULT_DUPLICATE,
                           command_id);
        return MODBUS_RESULT_OK;
      }
      generate_record(model, BOARD_A_SAMPLE_TRIGGER_SINGLE, 0U);
      remember_single_id(model, command_id);
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
    default:
      return MODBUS_RESULT_ILLEGAL_ADDRESS;
  }
}

static modbus_result_t read_input_register(const board_a_model_t *model,
                                           uint16_t address,
                                           uint16_t *value,
                                           uint64_t now_us)
{
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
    case BOARD_A_INPUT_SOURCE_TYPE:
      *value = BOARD_A_DATA_SOURCE_TEST;
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_SESSION_ID_HI:
      *value = word_high16(model->session_id);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_SESSION_ID_LO:
      *value = word_low16(model->session_id);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_PERSISTENCE_STATUS:
    case BOARD_A_INPUT_STORAGE_STATUS:
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
      *value = BOARD_A_UNIT_COUNT;
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
      *value = word_high16(model->stats.storage_dropped);
      return MODBUS_RESULT_OK;
    case BOARD_A_INPUT_STORAGE_DROPPED_LO:
      *value = word_low16(model->stats.storage_dropped);
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
    default:
      return MODBUS_RESULT_ILLEGAL_ADDRESS;
  }
}

void board_a_model_init(board_a_model_t *model, uint32_t session_id)
{
  uint8_t channel;

  model->pending_config.period_sec = BOARD_A_DEFAULT_PERIOD_SEC;
  model->pending_config.channel_mask = BOARD_A_DEFAULT_CHANNEL_MASK;
  model->pending_config.record_count = BOARD_A_DEFAULT_RECORD_COUNT;
  model->active_config.valid = true;
  model->active_config.version = 0U;
  model->active_config.config = model->pending_config;
  model->snapshot.valid = false;
  model->snapshot.sequence = 0U;
  model->snapshot.sample_time_us = 0U;
  model->snapshot.channel_count = 1U;
  model->snapshot.trigger = BOARD_A_SAMPLE_TRIGGER_NONE;
  for (channel = 0U; channel < BOARD_A_MAX_CHANNELS; ++channel) {
    model->snapshot.channel_values[channel] = 0U;
    model->snapshot.channel_quality[channel] = BOARD_A_QUALITY_UNAVAILABLE;
  }
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
}

modbus_result_t board_a_model_read_registers(void *context,
                                             modbus_register_space_t space,
                                             uint16_t address,
                                             uint16_t quantity,
                                             uint16_t *values,
                                             uint64_t now_us)
{
  board_a_model_t *model = (board_a_model_t *)context;
  uint16_t index;
  modbus_result_t result;

  if ((model == NULL) || (values == NULL) || (quantity == 0U)) {
    return MODBUS_RESULT_ILLEGAL_VALUE;
  }

  for (index = 0U; index < quantity; ++index) {
    if (space == MODBUS_REGISTER_HOLDING) {
      result = read_holding_register(model,
                                     (uint16_t)(address + index),
                                     &values[index]);
    } else if (space == MODBUS_REGISTER_INPUT) {
      result = read_input_register(model,
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
       (command_register > BOARD_A_COMMAND_ARM_START))) {
    return MODBUS_RESULT_ILLEGAL_VALUE;
  }

  command_id = ((uint32_t)command_id_hi << 16U) | (uint32_t)command_id_lo;
  if (writes_command &&
      (command_register == BOARD_A_COMMAND_SAVE_CONFIG)) {
    model->stats.persistence_errors++;
    set_command_status(model, command_register,
                       BOARD_A_COMMAND_RESULT_UNSUPPORTED, command_id);
    return MODBUS_RESULT_DEVICE_FAILURE;
  }

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
    generate_record(model, BOARD_A_SAMPLE_TRIGGER_PERIODIC, now_us);
    model->records_this_run++;
    model->start_pending = false;
    model->next_sample_us = now_us + period_us;
  } else if ((model->next_sample_us != 0U) &&
             (now_us >= model->next_sample_us)) {
    missed = (now_us - model->next_sample_us) / period_us;
    model->stats.scheduler_missed += (uint32_t)missed;
    generate_record(model, BOARD_A_SAMPLE_TRIGGER_PERIODIC, now_us);
    model->records_this_run++;
    model->next_sample_us = now_us + period_us;
  }

  if ((model->active_config.config.record_count != 0U) &&
      (model->records_this_run >=
       model->active_config.config.record_count)) {
    model->run_state = BOARD_A_RUN_STOPPED;
    model->start_pending = false;
    model->next_sample_us = 0U;
  }
}
