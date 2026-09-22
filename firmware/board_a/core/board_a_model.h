#ifndef BOARD_A_MODEL_H
#define BOARD_A_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#include "../sensors/sensor_manager.h"
#include "board_a_persistence.h"
#include "modbus_rtu.h"

#define BOARD_A_SLAVE_ADDRESS 1U
#define BOARD_A_MAX_CHANNELS 4U

#define BOARD_A_PERIOD_MIN_SEC 10U
#define BOARD_A_PERIOD_MAX_SEC 3600U
#define BOARD_A_CHANNEL_MASK_MIN 0x0001U
#define BOARD_A_CHANNEL_MASK_MAX 0x000FU
#define BOARD_A_SINGLE_DEDUP_CAPACITY 16U
#define BOARD_A_TIME_INVALID_SECONDS 0xFFFFFFFFU

enum {
  BOARD_A_HOLDING_CFG_PERIOD_SEC = 0x0000,
  BOARD_A_HOLDING_CFG_CHANNEL_MASK = 0x0001,
  BOARD_A_HOLDING_CFG_RECORD_COUNT = 0x0002,
  BOARD_A_HOLDING_PENDING_UTC_SECONDS_HI = 0x0020,
  BOARD_A_HOLDING_PENDING_UTC_SECONDS_LO = 0x0021,
  BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_HI = 0x0022,
  BOARD_A_HOLDING_PENDING_START_UTC_SECONDS_LO = 0x0023,
  BOARD_A_HOLDING_COMMAND = 0x0040,
  BOARD_A_HOLDING_COMMAND_ID_HI = 0x0041,
  BOARD_A_HOLDING_COMMAND_ID_LO = 0x0042
};

enum {
  BOARD_A_INPUT_DEVICE_TYPE = 0x0000,
  BOARD_A_INPUT_FIRMWARE_MAJOR = 0x0001,
  BOARD_A_INPUT_FIRMWARE_MINOR = 0x0002,
  BOARD_A_INPUT_FIRMWARE_PATCH = 0x0003,
  BOARD_A_INPUT_PROTOCOL_VERSION = 0x0004,
  BOARD_A_INPUT_RUN_STATE = 0x0005,
  BOARD_A_INPUT_ACTIVE_CONFIG_VALID = 0x0006,
  BOARD_A_INPUT_ACTIVE_PERIOD_SEC = 0x0007,
  BOARD_A_INPUT_ACTIVE_CHANNEL_MASK = 0x0008,
  BOARD_A_INPUT_ACTIVE_RECORD_COUNT = 0x0009,
  BOARD_A_INPUT_ACTIVE_CONFIG_VERSION_HI = 0x000A,
  BOARD_A_INPUT_ACTIVE_CONFIG_VERSION_LO = 0x000B,
  BOARD_A_INPUT_LAST_COMMAND = 0x000C,
  BOARD_A_INPUT_COMMAND_RESULT = 0x000D,
  BOARD_A_INPUT_LAST_COMMAND_ID_HI = 0x000E,
  BOARD_A_INPUT_LAST_COMMAND_ID_LO = 0x000F,
  BOARD_A_INPUT_DATA_SOURCE_TYPE = 0x0010,
  BOARD_A_INPUT_SESSION_ID_HI = 0x0011,
  BOARD_A_INPUT_SESSION_ID_LO = 0x0012,
  BOARD_A_INPUT_PERSISTENCE_STATUS = 0x0013,
  BOARD_A_INPUT_STORAGE_STATUS = 0x0014,
  BOARD_A_INPUT_RTOS_STATUS = 0x0015,
  BOARD_A_INPUT_TIME_STATUS = 0x0016,
  BOARD_A_INPUT_RECORDS_THIS_RUN_HI = 0x0017,
  BOARD_A_INPUT_RECORDS_THIS_RUN_LO = 0x0018,
  BOARD_A_INPUT_SCHEDULE_STATE = 0x0019,
  BOARD_A_INPUT_CURRENT_UTC_SECONDS_HI = 0x001A,
  BOARD_A_INPUT_CURRENT_UTC_SECONDS_LO = 0x001B,
  BOARD_A_INPUT_ARMED_START_UTC_SECONDS_HI = 0x001C,
  BOARD_A_INPUT_ARMED_START_UTC_SECONDS_LO = 0x001D
};

enum {
  BOARD_A_INPUT_SNAPSHOT_VALID = 0x0020,
  BOARD_A_INPUT_RECORD_SEQUENCE_HI = 0x0021,
  BOARD_A_INPUT_RECORD_SEQUENCE_LO = 0x0022,
  BOARD_A_INPUT_SOURCE_TYPE = 0x0023,
  BOARD_A_INPUT_CHANNEL_COUNT = 0x0024,
  BOARD_A_INPUT_CHANNEL_0 = 0x0025,
  BOARD_A_INPUT_CHANNEL_1 = 0x0026,
  BOARD_A_INPUT_CHANNEL_2 = 0x0027,
  BOARD_A_INPUT_CHANNEL_3 = 0x0028,
  BOARD_A_INPUT_CHANNEL_0_QUALITY = 0x0029,
  BOARD_A_INPUT_CHANNEL_1_QUALITY = 0x002A,
  BOARD_A_INPUT_CHANNEL_2_QUALITY = 0x002B,
  BOARD_A_INPUT_CHANNEL_3_QUALITY = 0x002C,
  BOARD_A_INPUT_LAST_SAMPLE_TRIGGER = 0x002D,
  BOARD_A_INPUT_UNIT_CODE = 0x002E
};

enum {
  BOARD_A_INPUT_RX_FRAMES_HI = 0x0060,
  BOARD_A_INPUT_RX_FRAMES_LO = 0x0061,
  BOARD_A_INPUT_CRC_ERRORS_HI = 0x0062,
  BOARD_A_INPUT_CRC_ERRORS_LO = 0x0063,
  BOARD_A_INPUT_ADDRESS_MISMATCH_HI = 0x0064,
  BOARD_A_INPUT_ADDRESS_MISMATCH_LO = 0x0065,
  BOARD_A_INPUT_OVERLONG_FRAMES_HI = 0x0066,
  BOARD_A_INPUT_OVERLONG_FRAMES_LO = 0x0067,
  BOARD_A_INPUT_MALFORMED_FRAMES_HI = 0x0068,
  BOARD_A_INPUT_MALFORMED_FRAMES_LO = 0x0069,
  BOARD_A_INPUT_TX_RESPONSES_HI = 0x006A,
  BOARD_A_INPUT_TX_RESPONSES_LO = 0x006B,
  BOARD_A_INPUT_BROADCAST_WRITES_HI = 0x006C,
  BOARD_A_INPUT_BROADCAST_WRITES_LO = 0x006D,
  BOARD_A_INPUT_BROADCAST_READS_HI = 0x006E,
  BOARD_A_INPUT_BROADCAST_READS_LO = 0x006F,
  BOARD_A_INPUT_EXCEPTION_RESPONSES_HI = 0x0070,
  BOARD_A_INPUT_EXCEPTION_RESPONSES_LO = 0x0071,
  BOARD_A_INPUT_SCHEDULER_MISSED_HI = 0x0072,
  BOARD_A_INPUT_SCHEDULER_MISSED_LO = 0x0073,
  BOARD_A_INPUT_STORAGE_DROPPED_HI = 0x0074,
  BOARD_A_INPUT_STORAGE_DROPPED_LO = 0x0075,
  BOARD_A_INPUT_PERSISTENCE_ERRORS_HI = 0x0076,
  BOARD_A_INPUT_PERSISTENCE_ERRORS_LO = 0x0077,
  BOARD_A_INPUT_DEVICE_FAULTS_HI = 0x0078,
  BOARD_A_INPUT_DEVICE_FAULTS_LO = 0x0079
};

enum {
  BOARD_A_INPUT_CONTRACT_REVISION = 0x0080,
  BOARD_A_INPUT_SAVE_STATE = 0x0081,
  BOARD_A_INPUT_SAVE_COMMAND_ID_HI = 0x0082,
  BOARD_A_INPUT_SAVE_COMMAND_ID_LO = 0x0083,
  BOARD_A_INPUT_SAVE_CONFIG_VERSION_HI = 0x0084,
  BOARD_A_INPUT_SAVE_CONFIG_VERSION_LO = 0x0085,
  BOARD_A_INPUT_SAVE_ERROR = 0x0086,
  BOARD_A_INPUT_CONFIG_LOAD_STATE = 0x0087,
  BOARD_A_INPUT_EXTENDED_STORAGE_STATE = 0x0088,
  BOARD_A_INPUT_EXTENDED_STORAGE_ERROR = 0x0089,
  BOARD_A_INPUT_STORAGE_QUEUED = 0x008A,
  BOARD_A_INPUT_STORAGE_HIGH_WATER = 0x008B,
  BOARD_A_INPUT_STORAGE_GENERATED_HI = 0x008C,
  BOARD_A_INPUT_STORAGE_GENERATED_LO = 0x008D,
  BOARD_A_INPUT_STORAGE_SYNCED_HI = 0x008E,
  BOARD_A_INPUT_STORAGE_SYNCED_LO = 0x008F,
  BOARD_A_INPUT_STORAGE_DROPPED_EXT_HI = 0x0090,
  BOARD_A_INPUT_STORAGE_DROPPED_EXT_LO = 0x0091,
  BOARD_A_INPUT_STORAGE_UNCERTAIN_HI = 0x0092,
  BOARD_A_INPUT_STORAGE_UNCERTAIN_LO = 0x0093,
  BOARD_A_INPUT_STORAGE_IN_FLIGHT = 0x0094,
  BOARD_A_INPUT_DRAIN_STATE = 0x0095,
  BOARD_A_INPUT_LAST_SYNCED_SEQ_HI = 0x0096,
  BOARD_A_INPUT_LAST_SYNCED_SEQ_LO = 0x0097,
  BOARD_A_INPUT_LAST_SYNCED_FILE_HI = 0x0098,
  BOARD_A_INPUT_LAST_SYNCED_FILE_LO = 0x0099,
  BOARD_A_INPUT_LAST_SYNCED_DATE_HI = 0x009A,
  BOARD_A_INPUT_LAST_SYNCED_DATE_LO = 0x009B,
  BOARD_A_INPUT_EXTENDED_ACTIVE_CONFIG_VERSION_HI = 0x009C,
  BOARD_A_INPUT_EXTENDED_ACTIVE_CONFIG_VERSION_LO = 0x009D,
  BOARD_A_INPUT_DRAIN_GENERATION_HI = 0x009E,
  BOARD_A_INPUT_DRAIN_GENERATION_LO = 0x009F,
  BOARD_A_INPUT_STORAGE_ERRORS_HI = 0x00A0,
  BOARD_A_INPUT_STORAGE_ERRORS_LO = 0x00A1,
  BOARD_A_INPUT_LOAD_SEQUENCE_HI = 0x00A2,
  BOARD_A_INPUT_LOAD_SEQUENCE_LO = 0x00A3,
  BOARD_A_INPUT_CAPTURED_PERIOD_HI = 0x00A4,
  BOARD_A_INPUT_CAPTURED_PERIOD_LO = 0x00A5,
  BOARD_A_INPUT_CAPTURED_MASK = 0x00A6,
  BOARD_A_INPUT_CAPTURED_COUNT = 0x00A7
};

/*
 * DS18B20 extension block. It reuses the P6 extension address range but
 * advances the contract revision so DHT11 clients cannot misinterpret it.
 */
enum {
  BOARD_A_INPUT_DS18B20_CONTRACT_REVISION = 0x00B0,
  BOARD_A_INPUT_DS18B20_SOURCE_TYPE = 0x00B1,
  BOARD_A_INPUT_DS18B20_VALID_MASK = 0x00B2,
  BOARD_A_INPUT_DS18B20_SAMPLE_ID_HI = 0x00B3,
  BOARD_A_INPUT_DS18B20_SAMPLE_ID_LO = 0x00B4,
  BOARD_A_INPUT_DS18B20_TEMPERATURE_0 = 0x00B5,
  BOARD_A_INPUT_DS18B20_TEMPERATURE_1 = 0x00B6,
  BOARD_A_INPUT_DS18B20_TEMPERATURE_2 = 0x00B7,
  BOARD_A_INPUT_DS18B20_QUALITY_0 = 0x00B8,
  BOARD_A_INPUT_DS18B20_QUALITY_1 = 0x00B9,
  BOARD_A_INPUT_DS18B20_QUALITY_2 = 0x00BA,
  BOARD_A_INPUT_DS18B20_ERROR_0 = 0x00BB,
  BOARD_A_INPUT_DS18B20_ERROR_1 = 0x00BC,
  BOARD_A_INPUT_DS18B20_ERROR_2 = 0x00BD,
  BOARD_A_INPUT_DS18B20_SAMPLE_TIME_0_HI = 0x00BE,
  BOARD_A_INPUT_DS18B20_SAMPLE_TIME_0_LO = 0x00BF,
  BOARD_A_INPUT_DS18B20_SAMPLE_TIME_1_HI = 0x00C0,
  BOARD_A_INPUT_DS18B20_SAMPLE_TIME_1_LO = 0x00C1,
  BOARD_A_INPUT_DS18B20_SAMPLE_TIME_2_HI = 0x00C2,
  BOARD_A_INPUT_DS18B20_SAMPLE_TIME_2_LO = 0x00C3,
  BOARD_A_INPUT_DS18B20_SENSOR_TYPE = 0x00C4,
  BOARD_A_INPUT_DS18B20_ROM_SHORT_0 = 0x00C5,
  BOARD_A_INPUT_DS18B20_ROM_SHORT_1 = 0x00C6,
  BOARD_A_INPUT_DS18B20_ROM_SHORT_2 = 0x00C7
};

typedef enum {
  BOARD_A_COMMAND_NONE = 0,
  BOARD_A_COMMAND_APPLY_CONFIG = 1,
  BOARD_A_COMMAND_SAVE_CONFIG = 2,
  BOARD_A_COMMAND_START = 3,
  BOARD_A_COMMAND_STOP = 4,
  BOARD_A_COMMAND_SINGLE = 5,
  BOARD_A_COMMAND_SET_TIME = 6,
  BOARD_A_COMMAND_ARM_START = 7
} board_a_command_t;

typedef enum {
  BOARD_A_COMMAND_RESULT_NONE = 0,
  BOARD_A_COMMAND_RESULT_ACCEPTED = 1,
  BOARD_A_COMMAND_RESULT_DUPLICATE = 2,
  BOARD_A_COMMAND_RESULT_UNSUPPORTED = 3,
  BOARD_A_COMMAND_RESULT_REJECTED = 4
} board_a_command_result_t;

typedef enum {
  BOARD_A_RUN_STOPPED = 0,
  BOARD_A_RUN_RUNNING = 1
} board_a_run_state_t;

typedef enum {
  BOARD_A_SAMPLE_TRIGGER_NONE = 0,
  BOARD_A_SAMPLE_TRIGGER_PERIODIC = 1,
  BOARD_A_SAMPLE_TRIGGER_SINGLE = 2
} board_a_sample_trigger_t;

enum {
  BOARD_A_DATA_SOURCE_TEST = 1,
  BOARD_A_DATA_SOURCE_REAL_DHT11 = 2,
  BOARD_A_DATA_SOURCE_REAL_DS18B20 = 3
};

enum {
  BOARD_A_STATUS_UNSUPPORTED = 0,
  BOARD_A_STATUS_SUPPORTED = 1
};

enum {
  BOARD_A_TIME_STATUS_UNCALIBRATED = 0,
  BOARD_A_TIME_STATUS_CALIBRATED = 1
};

enum {
  BOARD_A_SCHEDULE_STATE_NONE = 0,
  BOARD_A_SCHEDULE_STATE_WAITING = 1
};

enum {
  BOARD_A_UNIT_NONE = 0,
  BOARD_A_UNIT_COUNT = 1,
  BOARD_A_UNIT_TEMPERATURE_X16 = 2
};

typedef struct {
  uint16_t period_sec;
  uint16_t channel_mask;
  uint16_t record_count;
} board_a_config_t;

typedef struct {
  bool valid;
  uint32_t version;
  board_a_config_t config;
} board_a_active_config_t;

/*
 * Software UTC staging, calibration anchor, and one-shot scheduled start.
 * The anchor pairs the UTC second written by SET_TIME with the monotonic
 * microsecond instant of that command; scheduling derives target deadlines
 * from the pair without truncating to whole seconds.
 */
typedef struct {
  uint32_t pending_utc_seconds;
  uint32_t pending_start_utc_seconds;
  bool time_valid;
  uint32_t utc_anchor_seconds;
  uint64_t utc_anchor_us;
  bool schedule_armed;
  uint32_t schedule_target_seconds;
  uint64_t schedule_deadline_us;
  uint32_t schedule_start_late_us;
  uint32_t schedule_start_count;
} board_a_time_state_t;

typedef struct {
  bool valid;
  uint32_t sequence;
  uint64_t sample_time_us;
  uint16_t source;
  uint16_t channel_count;
  uint16_t channel_values[BOARD_A_MAX_CHANNELS];
  uint16_t channel_quality[BOARD_A_MAX_CHANNELS];
  uint16_t trigger;
  board_a_sensor_snapshot_t sensors;
} board_a_snapshot_t;

typedef struct {
  uint32_t rx_frames;
  uint32_t crc_errors;
  uint32_t address_mismatch;
  uint32_t overlong_frames;
  uint32_t malformed_frames;
  uint32_t tx_responses;
  uint32_t broadcast_writes;
  uint32_t broadcast_reads;
  uint32_t exception_responses;
  uint32_t scheduler_missed;
  uint32_t storage_dropped;
  uint32_t persistence_errors;
  uint32_t device_faults;
} board_a_model_stats_t;

typedef struct {
  board_a_config_t pending_config;
  board_a_active_config_t active_config;
  board_a_sensor_snapshot_t pending_sensor_snapshot;
  board_a_sensor_map_t sensor_map;
  board_a_snapshot_t snapshot;
  uint16_t data_source;
  board_a_run_state_t run_state;
  uint32_t session_id;
  uint16_t command_register;
  uint16_t command_id_hi;
  uint16_t command_id_lo;
  uint16_t last_command;
  uint16_t command_result;
  uint32_t last_command_id;
  /*
   * Bounded FIFO set of distinct single-command IDs for this MCU session.
   * A full window overwrites the oldest ID; duplicates do not refresh age.
   */
  uint32_t single_ids[BOARD_A_SINGLE_DEDUP_CAPACITY];
  uint8_t single_id_count;
  uint8_t single_id_next;
  uint32_t records_this_run;
  uint64_t next_sample_us;
  bool start_pending;
  board_a_time_state_t time;
  board_a_persistence_t persistence;
  board_a_model_stats_t stats;
} board_a_model_t;

void board_a_model_init(board_a_model_t *model, uint32_t session_id);

bool board_a_model_set_data_source(board_a_model_t *model,
                                   uint16_t source);

void board_a_model_publish_sensor_snapshot(
    board_a_model_t *model,
    const board_a_sensor_snapshot_t *snapshot);

bool board_a_model_publish_sensor_map(
    board_a_model_t *model, const board_a_sensor_map_t *map);

bool board_a_model_copy_sensor_map(
    const board_a_model_t *model, board_a_sensor_map_t *map);

modbus_result_t board_a_model_read_registers(void *context,
                                             modbus_register_space_t space,
                                             uint16_t address,
                                             uint16_t quantity,
                                             uint16_t *values,
                                             uint64_t now_us);

modbus_result_t board_a_model_write_registers(void *context,
                                              uint16_t address,
                                              const uint16_t *values,
                                              uint16_t quantity,
                                              uint64_t now_us);

void board_a_model_tick(board_a_model_t *model, uint64_t now_us);

void board_a_model_apply_loaded_config(
    board_a_model_t *model, const board_a_persisted_config_t *config,
    uint32_t sequence);

void board_a_model_note_config_load(
    board_a_model_t *model, board_a_config_load_state_t state,
    uint32_t sequence);

int board_a_model_claim_save(board_a_model_t *model,
                             board_a_save_request_t *request);

void board_a_model_complete_save(
    board_a_model_t *model, int success, board_a_save_error_t error,
    uint32_t raw_error);

int board_a_model_pop_record(
    board_a_model_t *model, board_a_record_format_record_t *record);

void board_a_model_requeue_record(
    board_a_model_t *model,
    const board_a_record_format_record_t *record);

void board_a_model_complete_record(
    board_a_model_t *model,
    const board_a_record_format_record_t *record,
    board_a_record_complete_result_t result);

void board_a_model_set_storage_state(
    board_a_model_t *model, board_a_storage_state_t state,
    board_a_storage_error_t error, uint32_t raw_error);

void board_a_model_note_storage_error(
    board_a_model_t *model, board_a_storage_error_t error,
    uint32_t raw_error);

void board_a_model_complete_drain(board_a_model_t *model,
                                  uint32_t generation, int success);

#endif /* BOARD_A_MODEL_H */
