#include <stdio.h>
#include <string.h>

#include "config_store.h"
#include "board_a_record_format.h"

#define FAKE_MEMORY_SIZE 256U
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
      return 1;                                                                \
    }                                                                          \
  } while (0)

typedef struct {
  uint8_t memory[FAKE_MEMORY_SIZE];
  uint8_t read_error_mask;
  uint8_t read_busy_mask;
  uint8_t write_error_mask;
  uint8_t partial_write_mask;
  uint8_t corrupt_readback_mask;
  uint8_t partial_write_bytes;
  uint8_t validation_enabled;
  uint8_t rejected_first_byte;
  config_store_status_t next_lock_result;
  unsigned int write_count;
  unsigned int validate_count;
  unsigned int lock_count;
  unsigned int unlock_count;
} fake_backend_t;

static uint8_t slot_bit_from_address(uint16_t address)
{
  return (uint8_t)(1U << (address / CONFIG_STORE_SLOT_SIZE_BYTES));
}

static config_store_status_t fake_read(void *context, uint16_t address,
                                       uint8_t *data, uint16_t length)
{
  fake_backend_t *backend = context;
  uint8_t bit = slot_bit_from_address(address);

  if ((backend->read_busy_mask & bit) != 0U) {
    return CONFIG_STORE_BUSY;
  }
  if ((backend->read_error_mask & bit) != 0U ||
      address + length > FAKE_MEMORY_SIZE) {
    return CONFIG_STORE_IO_ERROR;
  }

  memcpy(data, &backend->memory[address], length);
  if ((backend->corrupt_readback_mask & bit) != 0U && length > 10U) {
    data[10] ^= 0x01U;
    backend->memory[address + 10U] ^= 0x01U;
    backend->corrupt_readback_mask &= (uint8_t)~bit;
  }
  return CONFIG_STORE_OK;
}

static config_store_status_t fake_write(void *context, uint16_t address,
                                        const uint8_t *data, uint16_t length)
{
  fake_backend_t *backend = context;
  uint8_t bit = slot_bit_from_address(address);

  backend->write_count++;
  if ((backend->write_error_mask & bit) != 0U ||
      address + length > FAKE_MEMORY_SIZE) {
    return CONFIG_STORE_IO_ERROR;
  }

  if ((backend->partial_write_mask & bit) != 0U) {
    uint16_t partial =
        (backend->partial_write_bytes < length) ? backend->partial_write_bytes
                                                : length;

    memcpy(&backend->memory[address], data, partial);
    return CONFIG_STORE_IO_ERROR;
  }

  memcpy(&backend->memory[address], data, length);
  return CONFIG_STORE_OK;
}

static config_store_status_t fake_validate(void *context,
                                           const uint8_t *payload,
                                           uint8_t payload_length)
{
  fake_backend_t *backend = context;

  backend->validate_count++;
  if (backend->validation_enabled != 0U && payload_length > 0U &&
      payload[0] == backend->rejected_first_byte) {
    return CONFIG_STORE_INVALID_RECORD;
  }
  return CONFIG_STORE_OK;
}

static config_store_status_t fake_lock(void *context)
{
  fake_backend_t *backend = context;
  config_store_status_t result;

  backend->lock_count++;
  result = backend->next_lock_result;
  backend->next_lock_result = CONFIG_STORE_OK;
  return result;
}

static void fake_unlock(void *context)
{
  fake_backend_t *backend = context;

  backend->unlock_count++;
}

static void fake_reset(fake_backend_t *backend)
{
  memset(backend, 0, sizeof(*backend));
  memset(backend->memory, 0xFF, sizeof(backend->memory));
}

static config_store_status_t init_store(fake_backend_t *backend,
                                        config_store_t *store)
{
  config_store_backend_t callbacks = {
      .read = fake_read,
      .write = fake_write,
      .validate = fake_validate,
      .lock = fake_lock,
      .unlock = fake_unlock,
      .context = backend,
  };

  return config_store_init(store, &callbacks);
}

static uint16_t test_crc16(const uint8_t *data, uint16_t length)
{
  uint16_t crc = 0xFFFFU;
  uint16_t index;

  for (index = 0U; index < length; index++) {
    uint8_t bit;

    crc ^= (uint16_t)((uint16_t)data[index] << 8U);
    for (bit = 0U; bit < 8U; bit++) {
      crc = ((crc & 0x8000U) != 0U)
                ? (uint16_t)((uint16_t)(crc << 1U) ^ 0x1021U)
                : (uint16_t)(crc << 1U);
    }
  }

  return crc;
}

static void store_le16(uint8_t *data, uint16_t value)
{
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8U);
}

static void store_le32(uint8_t *data, uint32_t value)
{
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8U);
  data[2] = (uint8_t)(value >> 16U);
  data[3] = (uint8_t)(value >> 24U);
}

static void manufacture_slot(fake_backend_t *backend, uint8_t slot,
                             uint8_t version, uint32_t sequence,
                             const uint8_t *payload, uint8_t payload_length)
{
  uint8_t *record = &backend->memory[slot * CONFIG_STORE_SLOT_SIZE_BYTES];
  uint16_t crc;

  memset(record, 0, CONFIG_STORE_SLOT_SIZE_BYTES);
  record[0] = 0x57U;
  record[1] = 0x43U;
  record[2] = version;
  record[3] = 0U;
  store_le32(&record[4], sequence);
  store_le16(&record[8], payload_length);
  if (payload_length > 0U) {
    memcpy(&record[10], payload, payload_length);
  }
  crc = test_crc16(record, 122U);
  store_le16(&record[122], crc);
}

static int test_no_valid_record(void)
{
  fake_backend_t backend;
  config_store_t store;
  config_store_metadata_t metadata;
  uint8_t payload[CONFIG_STORE_PAYLOAD_MAX_BYTES];

  fake_reset(&backend);
  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_NO_VALID_RECORD);
  CHECK(metadata.selected_slot == CONFIG_STORE_SLOT_NONE);
  CHECK(metadata.valid_slot_mask == 0U);
  CHECK(metadata.read_error_mask == 0U);
  return 0;
}

static int test_save_load_and_alternation(void)
{
  fake_backend_t backend;
  config_store_t writer;
  config_store_t reader;
  config_store_metadata_t metadata;
  uint8_t first[] = {1U, 2U, 3U, 4U, 5U};
  uint8_t second[] = {9U, 8U, 7U, 6U};
  uint8_t payload[CONFIG_STORE_PAYLOAD_MAX_BYTES];
  uint8_t length = 0U;

  fake_reset(&backend);
  CHECK(init_store(&backend, &writer) == CONFIG_STORE_OK);
  CHECK(config_store_load(&writer, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_NO_VALID_RECORD);
  CHECK(config_store_save(&writer, first, sizeof(first), &metadata) ==
        CONFIG_STORE_OK);
  CHECK(metadata.selected_slot == 0U);
  CHECK(metadata.sequence == 1U);

  CHECK(config_store_save(&writer, second, sizeof(second), &metadata) ==
        CONFIG_STORE_OK);
  CHECK(metadata.selected_slot == 1U);
  CHECK(metadata.sequence == 2U);
  CHECK(metadata.valid_slot_mask == (1U << 1U));
  CHECK(backend.write_count == 2U);

  CHECK(init_store(&backend, &reader) == CONFIG_STORE_OK);
  CHECK(config_store_load(&reader, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_OK);
  CHECK(metadata.selected_slot == 1U);
  CHECK(metadata.sequence == 2U);
  CHECK(metadata.payload_length == sizeof(second));
  CHECK(memcmp(payload, second, sizeof(second)) == 0);

  backend.memory[CONFIG_STORE_SLOT_SIZE_BYTES + 20U] ^= 0x01U;
  CHECK(config_store_load(&reader, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_OK);
  CHECK(metadata.selected_slot == 0U);
  CHECK(metadata.sequence == 1U);
  CHECK(metadata.payload_length == sizeof(first));
  CHECK(memcmp(payload, first, sizeof(first)) == 0);

  length = metadata.payload_length;
  CHECK(length == sizeof(first));
  return 0;
}

static int test_validation_rejects_before_write(void)
{
  fake_backend_t backend;
  config_store_t store;
  config_store_metadata_t metadata;
  uint8_t invalid[] = {0xEEU, 0x01U};
  uint8_t valid[] = {0x01U, 0x02U};
  uint8_t payload[CONFIG_STORE_PAYLOAD_MAX_BYTES];

  fake_reset(&backend);
  backend.validation_enabled = 1U;
  backend.rejected_first_byte = 0xEEU;
  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_NO_VALID_RECORD);

  CHECK(config_store_save(&store, invalid, sizeof(invalid), &metadata) ==
        CONFIG_STORE_INVALID_RECORD);
  CHECK(backend.write_count == 0U);
  CHECK(backend.validate_count == 1U);

  CHECK(config_store_save(&store, valid, sizeof(valid), &metadata) ==
        CONFIG_STORE_OK);
  CHECK(backend.write_count == 1U);
  CHECK(backend.validate_count == 2U);
  return 0;
}

static int test_validation_selects_only_valid_business_slot(void)
{
  fake_backend_t backend;
  config_store_t store;
  config_store_metadata_t metadata;
  uint8_t valid[] = {0x01U, 0x02U, 0x03U};
  uint8_t invalid[] = {0xEEU, 0x01U};
  uint8_t payload[CONFIG_STORE_PAYLOAD_MAX_BYTES];

  fake_reset(&backend);
  backend.validation_enabled = 1U;
  backend.rejected_first_byte = 0xEEU;
  manufacture_slot(&backend, 0U, CONFIG_STORE_FORMAT_VERSION, 1U, valid,
                   sizeof(valid));
  manufacture_slot(&backend, 1U, CONFIG_STORE_FORMAT_VERSION, 2U, invalid,
                   sizeof(invalid));

  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_OK);
  CHECK(metadata.selected_slot == 0U);
  CHECK(metadata.sequence == 1U);
  CHECK(metadata.payload_length == sizeof(valid));
  CHECK(memcmp(payload, valid, sizeof(valid)) == 0);
  CHECK(metadata.valid_slot_mask == (1U << 0U));
  CHECK((metadata.semantic_error_mask & (1U << 1U)) != 0U);

  manufacture_slot(&backend, 0U, CONFIG_STORE_FORMAT_VERSION, 3U, invalid,
                   sizeof(invalid));
  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_NO_VALID_RECORD);
  CHECK(metadata.selected_slot == CONFIG_STORE_SLOT_NONE);
  CHECK(metadata.valid_slot_mask == 0U);
  CHECK(metadata.semantic_error_mask ==
        (uint8_t)((1U << 0U) | (1U << 1U)));
  return 0;
}

static int test_business_payload_torn_write_preserves_old_slot(void)
{
  fake_backend_t backend;
  config_store_t store;
  config_store_metadata_t metadata;
  board_a_persisted_config_t first = {10U, 0x0001U, 0U, 0U, {{0U}}};
  board_a_persisted_config_t second = {30U, 0x000FU, 5U, 0U, {{0U}}};
  board_a_persisted_config_t decoded;
  uint8_t first_payload[BOARD_A_CONFIG_PAYLOAD_SIZE];
  uint8_t second_payload[BOARD_A_CONFIG_PAYLOAD_SIZE];
  uint8_t payload[CONFIG_STORE_PAYLOAD_MAX_BYTES];

  fake_reset(&backend);
  backend.validation_enabled = 1U;
  backend.rejected_first_byte = 0xEEU;
  CHECK(board_a_config_payload_encode(&first, first_payload,
                                      sizeof(first_payload)));
  CHECK(board_a_config_payload_encode(&second, second_payload,
                                      sizeof(second_payload)));

  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_NO_VALID_RECORD);
  CHECK(config_store_save(&store, first_payload, sizeof(first_payload),
                          &metadata) == CONFIG_STORE_OK);
  backend.partial_write_mask = 1U << 1U;
  backend.partial_write_bytes = 20U;
  CHECK(config_store_save(&store, second_payload, sizeof(second_payload),
                          &metadata) == CONFIG_STORE_IO_ERROR);

  backend.partial_write_mask = 0U;
  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_OK);
  CHECK(metadata.selected_slot == 0U);
  CHECK(metadata.sequence == 1U);
  CHECK(board_a_config_payload_decode(payload, metadata.payload_length,
                                      &decoded));
  CHECK(decoded.period_sec == first.period_sec);
  CHECK(decoded.channel_mask == first.channel_mask);
  CHECK(decoded.record_count == first.record_count);
  return 0;
}

static int test_lock_rejects_concurrent_operation(void)
{
  fake_backend_t backend;
  config_store_t store;
  config_store_metadata_t metadata;
  uint8_t payload[CONFIG_STORE_PAYLOAD_MAX_BYTES];

  fake_reset(&backend);
  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  backend.next_lock_result = CONFIG_STORE_BUSY;
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_BUSY);
  CHECK(backend.lock_count == 1U);
  CHECK(backend.unlock_count == 0U);

  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_NO_VALID_RECORD);
  CHECK(backend.lock_count == 2U);
  CHECK(backend.unlock_count == 1U);
  return 0;
}

static int test_torn_write_preserves_previous_slot(void)
{
  fake_backend_t backend;
  config_store_t store;
  config_store_metadata_t metadata;
  uint8_t first[] = {0x10U, 0x20U, 0x30U};
  uint8_t second[] = {0x40U, 0x50U, 0x60U};
  uint8_t payload[CONFIG_STORE_PAYLOAD_MAX_BYTES];

  fake_reset(&backend);
  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_NO_VALID_RECORD);
  CHECK(config_store_save(&store, first, sizeof(first), &metadata) ==
        CONFIG_STORE_OK);

  backend.partial_write_mask = 1U << 1U;
  backend.partial_write_bytes = 20U;
  CHECK(config_store_save(&store, second, sizeof(second), &metadata) ==
        CONFIG_STORE_IO_ERROR);

  backend.partial_write_mask = 0U;
  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_OK);
  CHECK(metadata.selected_slot == 0U);
  CHECK(metadata.payload_length == sizeof(first));
  CHECK(memcmp(payload, first, sizeof(first)) == 0);
  return 0;
}

static int test_verify_failure_keeps_previous_slot(void)
{
  fake_backend_t backend;
  config_store_t store;
  config_store_metadata_t metadata;
  uint8_t first[] = {0x11U, 0x22U, 0x33U};
  uint8_t second[] = {0x44U, 0x55U, 0x66U};
  uint8_t payload[CONFIG_STORE_PAYLOAD_MAX_BYTES];

  fake_reset(&backend);
  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_NO_VALID_RECORD);
  CHECK(config_store_save(&store, first, sizeof(first), &metadata) ==
        CONFIG_STORE_OK);

  backend.corrupt_readback_mask = 1U << 1U;
  CHECK(config_store_save(&store, second, sizeof(second), &metadata) ==
        CONFIG_STORE_VERIFY_FAILED);

  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_OK);
  CHECK(metadata.selected_slot == 0U);
  CHECK(metadata.payload_length == sizeof(first));
  CHECK(memcmp(payload, first, sizeof(first)) == 0);
  return 0;
}

static int test_sequence_wrap_and_conflict(void)
{
  fake_backend_t backend;
  config_store_t store;
  config_store_metadata_t metadata;
  uint8_t old_payload[] = {0xAAU};
  uint8_t new_payload[] = {0xBBU};
  uint8_t different_payload[] = {0xCCU};
  uint8_t payload[CONFIG_STORE_PAYLOAD_MAX_BYTES];

  fake_reset(&backend);
  manufacture_slot(&backend, 0U, CONFIG_STORE_FORMAT_VERSION, 0xFFFFFFFFU,
                   old_payload, sizeof(old_payload));
  manufacture_slot(&backend, 1U, CONFIG_STORE_FORMAT_VERSION, 0U, new_payload,
                   sizeof(new_payload));
  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_OK);
  CHECK(metadata.selected_slot == 1U);
  CHECK(metadata.sequence == 0U);
  CHECK(payload[0] == new_payload[0]);

  manufacture_slot(&backend, 0U, CONFIG_STORE_FORMAT_VERSION, 7U,
                   old_payload, sizeof(old_payload));
  manufacture_slot(&backend, 1U, CONFIG_STORE_FORMAT_VERSION, 7U,
                   different_payload, sizeof(different_payload));
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_CONFLICT);
  CHECK(config_store_save(&store, new_payload, sizeof(new_payload), &metadata) ==
        CONFIG_STORE_OK);
  CHECK(metadata.sequence == 8U);
  CHECK(metadata.selected_slot == 1U);

  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_OK);
  CHECK(metadata.selected_slot == 1U);
  CHECK(metadata.sequence == 8U);
  CHECK(payload[0] == new_payload[0]);
  return 0;
}

static int test_read_error_visibility(void)
{
  fake_backend_t backend;
  config_store_t store;
  config_store_metadata_t metadata;
  uint8_t payload[CONFIG_STORE_PAYLOAD_MAX_BYTES];
  uint8_t record[] = {0x21U, 0x22U, 0x23U};

  fake_reset(&backend);
  manufacture_slot(&backend, 0U, CONFIG_STORE_FORMAT_VERSION, 1U, record,
                   sizeof(record));
  backend.read_error_mask = 1U << 1U;

  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_OK);
  CHECK(metadata.selected_slot == 0U);
  CHECK((metadata.read_error_mask & (1U << 1U)) != 0U);
  CHECK(memcmp(payload, record, sizeof(record)) == 0);

  backend.read_error_mask = (uint8_t)((1U << 0U) | (1U << 1U));
  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_IO_ERROR);

  backend.read_error_mask = 0U;
  backend.read_busy_mask = (uint8_t)((1U << 0U) | (1U << 1U));
  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_BUSY);
  return 0;
}

static int test_argument_and_capacity_failures(void)
{
  fake_backend_t backend;
  config_store_t store;
  config_store_metadata_t metadata;
  uint8_t payload[CONFIG_STORE_PAYLOAD_MAX_BYTES];
  uint8_t record[20];
  config_store_backend_t invalid_backend = {
      .read = fake_read,
      .write = fake_write,
      .lock = fake_lock,
      .unlock = NULL,
      .context = &backend,
  };

  fake_reset(&backend);
  memset(record, 0x5AU, sizeof(record));
  CHECK(config_store_init(&store, &invalid_backend) ==
        CONFIG_STORE_INVALID_ARGUMENT);
  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_save(&store, record, sizeof(record), &metadata) ==
        CONFIG_STORE_INVALID_ARGUMENT);
  CHECK(config_store_load(&store, payload, sizeof(payload), &metadata) ==
        CONFIG_STORE_NO_VALID_RECORD);
  CHECK(config_store_save(&store, record, CONFIG_STORE_PAYLOAD_MAX_BYTES + 1U,
                          &metadata) == CONFIG_STORE_INVALID_ARGUMENT);

  manufacture_slot(&backend, 0U, CONFIG_STORE_FORMAT_VERSION, 1U, record,
                   sizeof(record));
  CHECK(init_store(&backend, &store) == CONFIG_STORE_OK);
  CHECK(config_store_load(&store, payload, 8U, &metadata) ==
        CONFIG_STORE_INVALID_ARGUMENT);
  return 0;
}

int main(void)
{
  static const uint8_t crc_vector[] = "123456789";

  CHECK(test_crc16(crc_vector, 9U) == 0x29B1U);
  CHECK(test_no_valid_record() == 0);
  CHECK(test_validation_rejects_before_write() == 0);
  CHECK(test_validation_selects_only_valid_business_slot() == 0);
  CHECK(test_business_payload_torn_write_preserves_old_slot() == 0);
  CHECK(test_lock_rejects_concurrent_operation() == 0);
  CHECK(test_save_load_and_alternation() == 0);
  CHECK(test_torn_write_preserves_previous_slot() == 0);
  CHECK(test_verify_failure_keeps_previous_slot() == 0);
  CHECK(test_sequence_wrap_and_conflict() == 0);
  CHECK(test_read_error_visibility() == 0);
  CHECK(test_argument_and_capacity_failures() == 0);
  puts("PASS test_config_store");
  return 0;
}
