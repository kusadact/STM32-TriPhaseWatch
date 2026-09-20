/*
 * P5 host integration adapter.
 *
 * The Modbus server, model, persistence accounting, record formatter, and
 * storage state machine are the real P4 candidate sources. Only the serial
 * framing and the EEPROM/SD I/O endpoints are substitutes in this executable.
 * This is synthetic host evidence, not board or card evidence.
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board_a_runtime.h"
#include "board_a_storage_engine.h"
#include "config_store.h"
#include "modbus_crc.h"
#include "modbus_rtu.h"

#define ADAPTER_EXCHANGE 0x01U
#define ADAPTER_TICK 0x02U
#define ADAPTER_RESET 0x03U
#define ADAPTER_CRC 0x04U

#define P5_EEPROM_SIZE 256U
#define P5_DEFAULT_TIME_STEP_US 10000000ULL

static board_a_runtime_t g_runtime;
static board_a_storage_engine_t g_storage_engine;
static uint64_t g_now_us;
static uint64_t g_time_step_us = P5_DEFAULT_TIME_STEP_US;
static uint32_t g_session_id = 1U;
static char g_storage_root[PATH_MAX];
static char g_config_image_path[PATH_MAX];
static uint8_t g_eeprom[P5_EEPROM_SIZE];
static config_store_t g_config_store;
static bool g_storage_probed;
static int g_save_delay_remaining;
static bool g_save_active;
static board_a_save_request_t g_save_request;

static config_store_status_t config_mem_read(
    void *context, uint16_t address, uint8_t *data, uint16_t length)
{
  (void)context;
  if ((data == NULL) || ((uint32_t)address + length > P5_EEPROM_SIZE)) {
    return CONFIG_STORE_INVALID_ARGUMENT;
  }
  memcpy(data, &g_eeprom[address], length);
  return CONFIG_STORE_OK;
}

static config_store_status_t config_mem_write(
    void *context, uint16_t address, const uint8_t *data, uint16_t length)
{
  (void)context;
  if ((data == NULL) || ((uint32_t)address + length > P5_EEPROM_SIZE)) {
    return CONFIG_STORE_INVALID_ARGUMENT;
  }
  memcpy(&g_eeprom[address], data, length);
  return CONFIG_STORE_OK;
}

static config_store_status_t config_mem_validate(
    void *context, const uint8_t *payload, uint8_t payload_length)
{
  (void)context;
  return board_a_config_payload_validate(payload, payload_length)
             ? CONFIG_STORE_OK
             : CONFIG_STORE_INVALID_RECORD;
}

static config_store_status_t config_mem_lock(void *context)
{
  (void)context;
  return CONFIG_STORE_OK;
}

static void config_mem_unlock(void *context)
{
  (void)context;
}

static const config_store_backend_t CONFIG_MEMORY_BACKEND = {
  .read = config_mem_read,
  .write = config_mem_write,
  .validate = config_mem_validate,
  .lock = config_mem_lock,
  .unlock = config_mem_unlock,
  .context = NULL,
};

static bool runtime_lock(void *context)
{
  (void)context;
  return true;
}

static void runtime_unlock(void *context)
{
  (void)context;
}

static uint64_t runtime_now_us(void *context)
{
  (void)context;
  return g_now_us;
}

static const board_a_runtime_ops_t RUNTIME_OPS = {
  .lock = runtime_lock,
  .unlock = runtime_unlock,
  .now_us = runtime_now_us,
};

static int ensure_directory(const char *path)
{
  char buffer[PATH_MAX];
  char *cursor;
  size_t length = strlen(path);

  if (length == 0U || length >= sizeof(buffer)) {
    return 0;
  }
  memcpy(buffer, path, length + 1U);
  for (cursor = buffer + 1; *cursor != '\0'; cursor++) {
    if (*cursor == '/') {
      *cursor = '\0';
      if (mkdir(buffer, 0777) != 0 && errno != EEXIST) {
        return 0;
      }
      *cursor = '/';
    }
  }
  if (mkdir(buffer, 0777) != 0 && errno != EEXIST) {
    return 0;
  }
  return 1;
}

static int parent_directories(const char *path)
{
  char buffer[PATH_MAX];
  char *slash;
  size_t length = strlen(path);

  if (length == 0U || length >= sizeof(buffer)) {
    return 0;
  }
  memcpy(buffer, path, length + 1U);
  slash = strrchr(buffer, '/');
  if (slash == NULL) {
    return 1;
  }
  *slash = '\0';
  return ensure_directory(buffer);
}

static int record_path(
    uint32_t file_date, uint32_t file_id, char *path, size_t capacity)
{
  char relative[BOARD_A_RECORD_PATH_MAX_BYTES];
  const char *suffix;
  int written;

  if (!board_a_record_format_make_path(
          relative, sizeof(relative), file_date, file_id)) {
    return 0;
  }
  suffix = (strncmp(relative, "0:/", 3U) == 0) ? relative + 3U : relative;
  written = snprintf(
      path, capacity, "%s/%s", g_storage_root, suffix);
  return written > 0 && (size_t)written < capacity;
}

static board_a_storage_io_result_t storage_probe(
    void *context, uint32_t deadline_ms, uint32_t *raw_error)
{
  (void)context;
  (void)deadline_ms;
  if (!ensure_directory(g_storage_root)) {
    *raw_error = (uint32_t)errno;
    return BOARD_A_STORAGE_IO_NOT_READY;
  }
  *raw_error = 0U;
  return BOARD_A_STORAGE_IO_OK;
}

static board_a_storage_io_result_t storage_open_new(
    void *context, uint32_t file_date, uint32_t file_id,
    uint32_t deadline_ms, void **handle, uint32_t *raw_error)
{
  char path[PATH_MAX];
  FILE *stream;

  (void)context;
  (void)deadline_ms;
  if (!record_path(file_date, file_id, path, sizeof(path)) ||
      !parent_directories(path)) {
    *raw_error = (uint32_t)errno;
    return BOARD_A_STORAGE_IO_OTHER;
  }
  stream = fopen(path, "wbx");
  if (stream == NULL) {
    *raw_error = (uint32_t)errno;
    return (errno == EEXIST) ? BOARD_A_STORAGE_IO_EXISTS
                             : BOARD_A_STORAGE_IO_NOT_READY;
  }
  *handle = stream;
  *raw_error = 0U;
  return BOARD_A_STORAGE_IO_OK;
}

static board_a_storage_io_result_t storage_write(
    void *context, void *handle, const uint8_t *data, uint16_t length,
    uint32_t deadline_ms, uint16_t *written, uint32_t *raw_error)
{
  size_t count;

  (void)context;
  (void)deadline_ms;
  count = fwrite(data, 1U, length, (FILE *)handle);
  *written = (uint16_t)count;
  *raw_error = (count == length) ? 0U : (uint32_t)errno;
  return (count == length) ? BOARD_A_STORAGE_IO_OK
                           : BOARD_A_STORAGE_IO_WRITE;
}

static board_a_storage_io_result_t storage_sync(
    void *context, void *handle, uint32_t deadline_ms, uint32_t *raw_error)
{
  (void)context;
  (void)deadline_ms;
  if (fflush((FILE *)handle) != 0) {
    *raw_error = (uint32_t)errno;
    return BOARD_A_STORAGE_IO_SYNC;
  }
  if (fsync(fileno((FILE *)handle)) != 0) {
    *raw_error = (uint32_t)errno;
    return BOARD_A_STORAGE_IO_SYNC;
  }
  *raw_error = 0U;
  return BOARD_A_STORAGE_IO_OK;
}

static board_a_storage_io_result_t storage_close(
    void *context, void *handle, uint32_t deadline_ms, uint32_t *raw_error)
{
  (void)context;
  (void)deadline_ms;
  if (fclose((FILE *)handle) != 0) {
    *raw_error = (uint32_t)errno;
    return BOARD_A_STORAGE_IO_CLOSE;
  }
  *raw_error = 0U;
  return BOARD_A_STORAGE_IO_OK;
}

static uint32_t storage_now_ms(void *context)
{
  (void)context;
  return (uint32_t)(g_now_us / 1000U);
}

static void storage_yield(void *context)
{
  (void)context;
}

static const board_a_storage_io_ops_t STORAGE_OPS = {
  .probe = storage_probe,
  .open_new = storage_open_new,
  .write = storage_write,
  .sync = storage_sync,
  .close = storage_close,
  .now_ms = storage_now_ms,
  .yield = storage_yield,
};

static int load_config_image(void)
{
  FILE *stream;
  size_t count;

  memset(g_eeprom, 0xFF, sizeof(g_eeprom));
  if (g_config_image_path[0] == '\0') {
    return 1;
  }
  stream = fopen(g_config_image_path, "rb");
  if (stream == NULL) {
    return errno == ENOENT;
  }
  count = fread(g_eeprom, 1U, sizeof(g_eeprom), stream);
  if (fclose(stream) != 0) {
    return 0;
  }
  return count == sizeof(g_eeprom);
}

static int persist_config_image(void)
{
  FILE *stream;
  size_t count;

  if (g_config_image_path[0] == '\0') {
    return 0;
  }
  stream = fopen(g_config_image_path, "wb");
  if (stream == NULL) {
    return 0;
  }
  count = fwrite(g_eeprom, 1U, sizeof(g_eeprom), stream);
  if (fflush(stream) != 0 || fclose(stream) != 0) {
    return 0;
  }
  return count == sizeof(g_eeprom);
}

static board_a_save_error_t map_save_error(config_store_status_t status)
{
  switch (status) {
    case CONFIG_STORE_OK:
      return BOARD_A_SAVE_ERROR_NONE;
    case CONFIG_STORE_NO_VALID_RECORD:
      return BOARD_A_SAVE_ERROR_NO_VALID_RECORD;
    case CONFIG_STORE_INVALID_ARGUMENT:
    case CONFIG_STORE_INVALID_RECORD:
    case CONFIG_STORE_VERIFY_FAILED:
      return BOARD_A_SAVE_ERROR_INVALID_DATA;
    case CONFIG_STORE_CONFLICT:
      return BOARD_A_SAVE_ERROR_CONFLICT;
    case CONFIG_STORE_NOT_READY:
      return BOARD_A_SAVE_ERROR_NOT_READY;
    case CONFIG_STORE_BUSY:
      return BOARD_A_SAVE_ERROR_BUSY;
    case CONFIG_STORE_IO_ERROR:
    default:
      return BOARD_A_SAVE_ERROR_IO;
  }
}

static void load_business_config(void)
{
  uint8_t payload[CONFIG_STORE_PAYLOAD_MAX_BYTES];
  config_store_metadata_t metadata;
  board_a_persisted_config_t config;
  config_store_status_t result =
      config_store_load(&g_config_store, payload, sizeof(payload), &metadata);

  if (result == CONFIG_STORE_OK &&
      board_a_config_payload_decode(payload, metadata.payload_length, &config)) {
    board_a_runtime_apply_loaded_config(&g_runtime, &config, metadata.sequence);
    return;
  }
  if (result == CONFIG_STORE_NO_VALID_RECORD) {
    board_a_runtime_note_config_load(
        &g_runtime, BOARD_A_CONFIG_LOAD_DEFAULT_NO_RECORD, 0U);
  } else {
    board_a_runtime_note_config_load(
        &g_runtime, BOARD_A_CONFIG_LOAD_DEFAULT_ERROR, 0U);
  }
}

static void pump_save(void)
{
  if (!g_save_active) {
    if (!board_a_runtime_claim_save(&g_runtime, &g_save_request)) {
      return;
    }
    g_save_active = true;
    if (g_save_delay_remaining > 0) {
      g_save_delay_remaining--;
      return;
    }
  } else if (g_save_delay_remaining > 0) {
    g_save_delay_remaining--;
    return;
  }

  uint8_t payload[BOARD_A_CONFIG_PAYLOAD_SIZE];
  config_store_metadata_t metadata;
  board_a_save_error_t error = BOARD_A_SAVE_ERROR_NONE;
  config_store_status_t result = CONFIG_STORE_OK;
  int success = 0;

  if (!board_a_config_payload_encode(
          &g_save_request.config, payload, sizeof(payload))) {
    error = BOARD_A_SAVE_ERROR_INVALID_DATA;
    result = CONFIG_STORE_INVALID_RECORD;
  } else {
    result = config_store_save(
        &g_config_store, payload, sizeof(payload), &metadata);
    if (result == CONFIG_STORE_OK && !persist_config_image()) {
      result = CONFIG_STORE_IO_ERROR;
    }
    success = result == CONFIG_STORE_OK;
    error = map_save_error(result);
  }
  board_a_runtime_complete_save(
      &g_runtime, success, error, (uint32_t)result);
  g_save_active = false;
}

static void pump_storage(void)
{
  board_a_persistence_status_t status;
  board_a_record_format_record_t record;
  board_a_storage_record_result_t result;
  uint32_t raw_error = 0U;
  uint32_t deadline_ms = storage_now_ms(NULL) +
                         BOARD_A_STORAGE_OPERATION_TIMEOUT_MS;

  if (!g_storage_probed) {
    if (board_a_storage_engine_probe(
            &g_storage_engine, &STORAGE_OPS, deadline_ms)) {
      g_storage_probed = true;
      board_a_runtime_set_storage_state(
          &g_runtime, BOARD_A_STORAGE_READY, BOARD_A_STORAGE_ERROR_NONE, 0U);
    } else {
      board_a_runtime_set_storage_state(
          &g_runtime, BOARD_A_STORAGE_UNAVAILABLE,
          g_storage_engine.last_error, g_storage_engine.raw_error);
      return;
    }
  }

  while (board_a_runtime_pop_record(&g_runtime, &record)) {
    result = board_a_storage_engine_process(
        &g_storage_engine, &STORAGE_OPS, &record, deadline_ms);
    if (result == BOARD_A_STORAGE_RECORD_SYNCED) {
      board_a_runtime_complete_record(
          &g_runtime, &record, BOARD_A_RECORD_COMPLETE_SYNCED);
    } else if (result == BOARD_A_STORAGE_RECORD_UNCERTAIN) {
      board_a_runtime_complete_record(
          &g_runtime, &record, BOARD_A_RECORD_COMPLETE_UNCERTAIN);
      board_a_runtime_set_storage_state(
          &g_runtime, BOARD_A_STORAGE_IO_ERROR,
          g_storage_engine.last_error, g_storage_engine.raw_error);
    } else {
      board_a_runtime_requeue_record(&g_runtime, &record);
      board_a_runtime_set_storage_state(
          &g_runtime, BOARD_A_STORAGE_UNAVAILABLE,
          g_storage_engine.last_error, g_storage_engine.raw_error);
      break;
    }
  }

  if (!board_a_runtime_persistence_status(&g_runtime, &status)) {
    return;
  }
  if (status.drain_state == BOARD_A_DRAIN_PENDING &&
      status.queued == 0U && status.in_flight == 0U) {
    uint32_t drain_deadline_ms = storage_now_ms(NULL) +
                                 BOARD_A_STORAGE_DRAIN_TIMEOUT_MS;
    int success = board_a_storage_engine_drain(
        &g_storage_engine, &STORAGE_OPS, drain_deadline_ms);
    if (!success) {
      raw_error = g_storage_engine.raw_error;
      board_a_runtime_note_storage_error(
          &g_runtime, g_storage_engine.last_error, raw_error);
    }
    board_a_runtime_complete_drain(
        &g_runtime, status.drain_generation, success);
  }
}

static void after_activity(void)
{
  g_now_us += g_time_step_us;
  board_a_runtime_tick(&g_runtime, g_now_us);
  pump_save();
  pump_storage();
}

static void initialize_runtime(void)
{
  board_a_runtime_init(&g_runtime, g_session_id, &RUNTIME_OPS, NULL);
  board_a_storage_engine_init(&g_storage_engine);
  g_storage_probed = false;
  g_save_active = false;
  if (!load_config_image()) {
    board_a_runtime_note_config_load(
        &g_runtime, BOARD_A_CONFIG_LOAD_DEFAULT_ERROR, 0U);
  } else {
    config_store_status_t init =
        config_store_init(&g_config_store, &CONFIG_MEMORY_BACKEND);
    if (init != CONFIG_STORE_OK) {
      board_a_runtime_note_config_load(
          &g_runtime, BOARD_A_CONFIG_LOAD_DEFAULT_ERROR, 0U);
    } else {
      load_business_config();
    }
  }
  pump_storage();
}

static int read_exact(uint8_t *buffer, size_t length)
{
  size_t offset = 0U;

  while (offset < length) {
    size_t count = fread(buffer + offset, 1U, length - offset, stdin);
    if (count == 0U) {
      return feof(stdin) ? 0 : -1;
    }
    offset += count;
  }
  return 1;
}

static int write_exact(const uint8_t *buffer, size_t length)
{
  size_t offset = 0U;

  while (offset < length) {
    size_t count = fwrite(buffer + offset, 1U, length - offset, stdout);
    if (count == 0U) {
      return -1;
    }
    offset += count;
  }
  return fflush(stdout) == 0 ? 1 : -1;
}

static uint16_t read_u16_be(const uint8_t *bytes)
{
  return (uint16_t)(((uint16_t)bytes[0] << 8U) | (uint16_t)bytes[1]);
}

static uint32_t read_u32_be(const uint8_t *bytes)
{
  return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
         ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
}

static uint64_t read_u64_be(const uint8_t *bytes)
{
  uint64_t value = 0U;
  size_t index;

  for (index = 0U; index < 8U; ++index) {
    value = (value << 8U) | (uint64_t)bytes[index];
  }
  return value;
}

static void write_u16_be(uint8_t *bytes, uint16_t value)
{
  bytes[0] = (uint8_t)(value >> 8U);
  bytes[1] = (uint8_t)value;
}

static int send_exchange_response(const uint8_t *response, uint16_t length)
{
  uint8_t header[3];

  header[0] = 0U;
  write_u16_be(&header[1], length);
  if (write_exact(header, sizeof(header)) < 0) {
    return -1;
  }
  if ((length != 0U) && write_exact(response, length) < 0) {
    return -1;
  }
  return 0;
}

static int handle_exchange(void)
{
  uint8_t length_bytes[2];
  uint8_t request[MODBUS_RTU_MAX_ADU_SIZE];
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  uint16_t request_length;
  uint16_t index;
  uint32_t poll_us;
  size_t response_length;

  if (read_exact(length_bytes, sizeof(length_bytes)) != 1) {
    return -1;
  }
  request_length = read_u16_be(length_bytes);
  if (request_length < 4U || request_length > MODBUS_RTU_MAX_ADU_SIZE) {
    fprintf(stderr, "invalid ADU length %u\n", (unsigned int)request_length);
    return -1;
  }
  if (read_exact(request, request_length) != 1) {
    return -1;
  }
  for (index = 0U; index < request_length; ++index) {
    uint32_t timestamp_us =
        (uint32_t)(g_now_us + ((uint64_t)index * 100U));

    board_a_runtime_push_byte(&g_runtime, request[index], timestamp_us);
  }
  g_now_us += ((uint64_t)request_length * 100U) + 5000U;
  poll_us = (uint32_t)g_now_us;
  response_length = board_a_runtime_poll(
      &g_runtime, poll_us, response, sizeof(response));
  if (send_exchange_response(response, (uint16_t)response_length) < 0) {
    return -1;
  }
  after_activity();
  return 0;
}

static int handle_tick(void)
{
  uint8_t now_bytes[8];
  uint8_t acknowledgement = 0U;

  if (read_exact(now_bytes, sizeof(now_bytes)) != 1) {
    return -1;
  }
  g_now_us = read_u64_be(now_bytes);
  board_a_runtime_tick(&g_runtime, g_now_us);
  pump_save();
  pump_storage();
  return write_exact(&acknowledgement, 1U) < 0 ? -1 : 0;
}

static int handle_reset(void)
{
  uint8_t session_bytes[4];
  uint8_t acknowledgement = 0U;

  if (read_exact(session_bytes, sizeof(session_bytes)) != 1) {
    return -1;
  }
  g_now_us = 0U;
  g_session_id = read_u32_be(session_bytes);
  initialize_runtime();
  return write_exact(&acknowledgement, 1U) < 0 ? -1 : 0;
}

static int handle_crc(void)
{
  uint8_t length_bytes[2];
  uint8_t data[MODBUS_RTU_MAX_ADU_SIZE];
  uint8_t result[2];
  uint16_t length;

  if (read_exact(length_bytes, sizeof(length_bytes)) != 1) {
    return -1;
  }
  length = read_u16_be(length_bytes);
  if (length == 0U || length > MODBUS_RTU_MAX_ADU_SIZE) {
    return -1;
  }
  if (read_exact(data, length) != 1) {
    return -1;
  }
  write_u16_be(result, modbus_crc16(data, length));
  return write_exact(result, sizeof(result)) < 0 ? -1 : 0;
}

static void read_environment(void)
{
  const char *step = getenv("P5_TIME_STEP_US");
  const char *delay = getenv("P5_SAVE_DELAY_CALLS");
  char *end = NULL;

  if (step != NULL && *step != '\0') {
    unsigned long long parsed = strtoull(step, &end, 10);
    if (end != step && *end == '\0' && parsed > 0U) {
      g_time_step_us = (uint64_t)parsed;
    }
  }
  if (delay != NULL && *delay != '\0') {
    long parsed = strtol(delay, &end, 10);
    if (end != delay && *end == '\0' && parsed >= 0) {
      g_save_delay_remaining = (int)parsed;
    }
  }
}

int main(int argc, char **argv)
{
  uint8_t command;
  const char *environment_root;
  const char *environment_config;

  if (setvbuf(stdout, NULL, _IONBF, 0U) != 0) {
    return EXIT_FAILURE;
  }
  environment_root = getenv("P5_STORAGE_ROOT");
  environment_config = getenv("P5_CONFIG_IMAGE");
  if (argc >= 2) {
    snprintf(g_storage_root, sizeof(g_storage_root), "%s", argv[1]);
  } else if (environment_root != NULL) {
    snprintf(g_storage_root, sizeof(g_storage_root), "%s", environment_root);
  } else {
    snprintf(g_storage_root, sizeof(g_storage_root), "./p5-storage");
  }
  if (argc >= 3) {
    snprintf(g_config_image_path, sizeof(g_config_image_path), "%s", argv[2]);
  } else if (environment_config != NULL) {
    snprintf(
        g_config_image_path, sizeof(g_config_image_path), "%s", environment_config);
  }
  read_environment();
  if (!ensure_directory(g_storage_root)) {
    fprintf(stderr, "cannot create storage root %s\n", g_storage_root);
    return EXIT_FAILURE;
  }
  g_now_us = 0U;
  initialize_runtime();

  while (read_exact(&command, 1U) == 1) {
    int result;

    switch (command) {
      case 0x00U:
        return EXIT_SUCCESS;
      case ADAPTER_EXCHANGE:
        result = handle_exchange();
        break;
      case ADAPTER_TICK:
        result = handle_tick();
        break;
      case ADAPTER_RESET:
        result = handle_reset();
        break;
      case ADAPTER_CRC:
        result = handle_crc();
        break;
      default:
        fprintf(stderr, "unknown adapter command 0x%02X\n",
                (unsigned int)command);
        return EXIT_FAILURE;
    }
    if (result != 0) {
      return EXIT_FAILURE;
    }
  }
  return ferror(stdin) ? EXIT_FAILURE : EXIT_SUCCESS;
}
