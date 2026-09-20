#include "config_store.h"

#include <string.h>

#define CONFIG_STORE_MAGIC_0 0x57U
#define CONFIG_STORE_MAGIC_1 0x43U
#define CONFIG_STORE_HEADER_SIZE_BYTES 10U
#define CONFIG_STORE_CRC_OFFSET_BYTES 122U

typedef struct {
  uint8_t format_version;
  uint32_t sequence;
  uint8_t payload_length;
  uint8_t payload[CONFIG_STORE_PAYLOAD_MAX_BYTES];
} config_store_record_t;

_Static_assert(CONFIG_STORE_PAYLOAD_MAX_BYTES +
                       CONFIG_STORE_HEADER_SIZE_BYTES + 2U <=
                   CONFIG_STORE_SLOT_SIZE_BYTES,
               "config store record must fit in one EEPROM slot");
_Static_assert(CONFIG_STORE_SLOT_SIZE_BYTES * CONFIG_STORE_SLOT_COUNT == 256U,
               "AT24C02 must contain exactly two config slots");

static uint16_t config_store_crc16(const uint8_t *data, uint16_t length)
{
  uint16_t crc = 0xFFFFU;
  uint16_t index = 0U;

  for (index = 0U; index < length; index++) {
    uint8_t bit = 0U;

    crc ^= (uint16_t)((uint16_t)data[index] << 8U);
    for (bit = 0U; bit < 8U; bit++) {
      if ((crc & 0x8000U) != 0U) {
        crc = (uint16_t)((uint16_t)(crc << 1U) ^ 0x1021U);
      } else {
        crc = (uint16_t)(crc << 1U);
      }
    }
  }

  return crc;
}

static uint16_t config_store_get_le16(const uint8_t *data)
{
  return (uint16_t)((uint16_t)data[0] |
                    ((uint16_t)data[1] << 8U));
}

static uint32_t config_store_get_le32(const uint8_t *data)
{
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
         ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static void config_store_put_le16(uint8_t *data, uint16_t value)
{
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8U);
}

static void config_store_put_le32(uint8_t *data, uint32_t value)
{
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8U);
  data[2] = (uint8_t)(value >> 16U);
  data[3] = (uint8_t)(value >> 24U);
}

static uint16_t config_store_slot_address(uint8_t slot)
{
  return (uint16_t)(slot * CONFIG_STORE_SLOT_SIZE_BYTES);
}

static uint8_t config_store_slot_bit(uint8_t slot)
{
  return (uint8_t)(1U << slot);
}

static uint8_t config_store_parse_slot(const uint8_t *slot,
                                       config_store_record_t *record)
{
  uint16_t payload_length;
  uint16_t stored_crc;
  uint16_t computed_crc;
  uint16_t index;

  if (slot[0] != CONFIG_STORE_MAGIC_0 ||
      slot[1] != CONFIG_STORE_MAGIC_1 ||
      slot[2] != CONFIG_STORE_FORMAT_VERSION || slot[3] != 0U) {
    return 0U;
  }

  payload_length = config_store_get_le16(&slot[8]);
  if (payload_length > CONFIG_STORE_PAYLOAD_MAX_BYTES) {
    return 0U;
  }

  stored_crc =
      config_store_get_le16(&slot[CONFIG_STORE_CRC_OFFSET_BYTES]);
  computed_crc = config_store_crc16(slot, CONFIG_STORE_CRC_OFFSET_BYTES);
  if (stored_crc != computed_crc) {
    return 0U;
  }

  for (index = (uint16_t)(CONFIG_STORE_HEADER_SIZE_BYTES + payload_length);
       index < CONFIG_STORE_CRC_OFFSET_BYTES; index++) {
    if (slot[index] != 0U) {
      return 0U;
    }
  }

  for (index = (uint16_t)(CONFIG_STORE_CRC_OFFSET_BYTES + 2U);
       index < CONFIG_STORE_SLOT_SIZE_BYTES; index++) {
    if (slot[index] != 0U) {
      return 0U;
    }
  }

  record->format_version = slot[2];
  record->sequence = config_store_get_le32(&slot[4]);
  record->payload_length = (uint8_t)payload_length;
  if (payload_length > 0U) {
    memcpy(record->payload, &slot[CONFIG_STORE_HEADER_SIZE_BYTES],
           payload_length);
  }

  return 1U;
}

static void config_store_serialize_slot(uint8_t *slot, uint32_t sequence,
                                        const uint8_t *payload,
                                        uint8_t payload_length)
{
  uint16_t crc;

  memset(slot, 0, CONFIG_STORE_SLOT_SIZE_BYTES);
  slot[0] = CONFIG_STORE_MAGIC_0;
  slot[1] = CONFIG_STORE_MAGIC_1;
  slot[2] = CONFIG_STORE_FORMAT_VERSION;
  slot[3] = 0U;
  config_store_put_le32(&slot[4], sequence);
  config_store_put_le16(&slot[8], payload_length);

  if (payload_length > 0U) {
    memcpy(&slot[CONFIG_STORE_HEADER_SIZE_BYTES], payload, payload_length);
  }

  crc = config_store_crc16(slot, CONFIG_STORE_CRC_OFFSET_BYTES);
  config_store_put_le16(&slot[CONFIG_STORE_CRC_OFFSET_BYTES], crc);
}

static int32_t config_store_sequence_compare(uint32_t left, uint32_t right)
{
  uint32_t difference = left - right;

  if (difference == 0U) {
    return 0;
  }
  if (difference < 0x80000000U) {
    return 1;
  }
  return -1;
}

static uint8_t config_store_records_equal(const config_store_record_t *left,
                                          const config_store_record_t *right)
{
  if (left->format_version != right->format_version ||
      left->sequence != right->sequence ||
      left->payload_length != right->payload_length) {
    return 0U;
  }

  if (left->payload_length == 0U) {
    return 1U;
  }

  return memcmp(left->payload, right->payload, left->payload_length) == 0;
}

config_store_status_t
config_store_init(config_store_t *store, const config_store_backend_t *backend)
{
  if (store == NULL || backend == NULL || backend->read == NULL ||
      backend->write == NULL ||
      ((backend->lock == NULL) != (backend->unlock == NULL))) {
    return CONFIG_STORE_INVALID_ARGUMENT;
  }

  store->backend = *backend;
  store->sequence = 0U;
  store->active_slot = CONFIG_STORE_SLOT_NONE;
  store->initialized = 0U;
  return CONFIG_STORE_OK;
}

static config_store_status_t
config_store_load_impl(config_store_t *store, uint8_t *payload,
                       uint8_t payload_capacity,
                       config_store_metadata_t *metadata)
{
  config_store_record_t records[CONFIG_STORE_SLOT_COUNT];
  uint8_t slot_buffer[CONFIG_STORE_SLOT_SIZE_BYTES];
  uint8_t selected_slot = CONFIG_STORE_SLOT_NONE;
  uint8_t slot;
  uint8_t valid_slot_mask = 0U;
  uint8_t read_error_mask = 0U;
  uint8_t format_error_mask = 0U;
  uint8_t semantic_error_mask = 0U;
  config_store_status_t read_status = CONFIG_STORE_OK;

  if (store == NULL || store->backend.read == NULL ||
      store->backend.write == NULL || metadata == NULL ||
      (payload == NULL && payload_capacity > 0U)) {
    return CONFIG_STORE_INVALID_ARGUMENT;
  }

  memset(records, 0, sizeof(records));
  memset(metadata, 0, sizeof(*metadata));
  metadata->selected_slot = CONFIG_STORE_SLOT_NONE;

  for (slot = 0U; slot < CONFIG_STORE_SLOT_COUNT; slot++) {
    config_store_status_t result = store->backend.read(
        store->backend.context, config_store_slot_address(slot), slot_buffer,
        CONFIG_STORE_SLOT_SIZE_BYTES);

    if (result != CONFIG_STORE_OK) {
      read_error_mask |= config_store_slot_bit(slot);
      if (result == CONFIG_STORE_BUSY) {
        read_status = CONFIG_STORE_BUSY;
      } else if (read_status == CONFIG_STORE_OK) {
        read_status = result;
      }
      continue;
    }

    if (config_store_parse_slot(slot_buffer, &records[slot]) == 0U) {
      format_error_mask |= config_store_slot_bit(slot);
      continue;
    }

    /*
     * A slot is a candidate only after the physical format and the owning
     * component's business schema both validate. This preserves the old
     * generic-store behavior when no validator is installed.
     */
    if (store->backend.validate != NULL) {
      config_store_status_t validate_status =
          store->backend.validate(store->backend.context,
                                  records[slot].payload,
                                  records[slot].payload_length);
      if (validate_status != CONFIG_STORE_OK) {
        semantic_error_mask |= config_store_slot_bit(slot);
        continue;
      }
    }

    valid_slot_mask |= config_store_slot_bit(slot);
  }

  metadata->valid_slot_mask = valid_slot_mask;
  metadata->read_error_mask = read_error_mask;
  metadata->format_error_mask = format_error_mask;
  metadata->semantic_error_mask = semantic_error_mask;

  if ((valid_slot_mask & config_store_slot_bit(0U)) != 0U) {
    selected_slot = 0U;
  }
  if ((valid_slot_mask & config_store_slot_bit(1U)) != 0U) {
    if (selected_slot == CONFIG_STORE_SLOT_NONE) {
      selected_slot = 1U;
    } else {
      int32_t comparison = config_store_sequence_compare(
          records[1].sequence, records[0].sequence);

      if (comparison > 0) {
        selected_slot = 1U;
      } else if (comparison == 0 &&
                 config_store_records_equal(&records[0], &records[1]) == 0U) {
        store->initialized = 1U;
        store->active_slot = 0U;
        store->sequence = records[0].sequence;
        return CONFIG_STORE_CONFLICT;
      }
    }
  }

  if (selected_slot == CONFIG_STORE_SLOT_NONE) {
    store->initialized = 1U;
    store->active_slot = CONFIG_STORE_SLOT_NONE;
    store->sequence = 0U;
    return (read_status != CONFIG_STORE_OK) ? read_status
                                            : CONFIG_STORE_NO_VALID_RECORD;
  }

  metadata->selected_slot = selected_slot;
  metadata->sequence = records[selected_slot].sequence;
  metadata->payload_length = records[selected_slot].payload_length;
  metadata->format_version = records[selected_slot].format_version;

  if (records[selected_slot].payload_length > payload_capacity) {
    return CONFIG_STORE_INVALID_ARGUMENT;
  }

  if (records[selected_slot].payload_length > 0U) {
    memcpy(payload, records[selected_slot].payload,
           records[selected_slot].payload_length);
  }

  store->initialized = 1U;
  store->active_slot = selected_slot;
  store->sequence = records[selected_slot].sequence;
  return CONFIG_STORE_OK;
}

static config_store_status_t
config_store_save_impl(config_store_t *store, const uint8_t *payload,
                       uint8_t payload_length,
                       config_store_metadata_t *metadata)
{
  config_store_record_t expected;
  config_store_record_t readback;
  uint8_t slot_buffer[CONFIG_STORE_SLOT_SIZE_BYTES];
  uint8_t target_slot;
  uint32_t next_sequence;
  config_store_status_t result;

  if (store == NULL || store->backend.read == NULL ||
      store->backend.write == NULL || store->initialized == 0U ||
      metadata == NULL ||
      payload_length > CONFIG_STORE_PAYLOAD_MAX_BYTES ||
      (payload == NULL && payload_length > 0U)) {
    return CONFIG_STORE_INVALID_ARGUMENT;
  }

  if (store->backend.validate != NULL) {
    result = store->backend.validate(store->backend.context, payload,
                                     payload_length);
    if (result != CONFIG_STORE_OK) {
      return result;
    }
  }

  if (store->active_slot == 0U) {
    target_slot = 1U;
  } else if (store->active_slot == 1U) {
    target_slot = 0U;
  } else {
    target_slot = 0U;
  }

  next_sequence = store->sequence + 1U;
  memset(&expected, 0, sizeof(expected));
  expected.format_version = CONFIG_STORE_FORMAT_VERSION;
  expected.sequence = next_sequence;
  expected.payload_length = payload_length;
  if (payload_length > 0U) {
    memcpy(expected.payload, payload, payload_length);
  }

  config_store_serialize_slot(slot_buffer, next_sequence, payload,
                              payload_length);
  memset(metadata, 0, sizeof(*metadata));

  result = store->backend.write(store->backend.context,
                                config_store_slot_address(target_slot),
                                slot_buffer, CONFIG_STORE_SLOT_SIZE_BYTES);
  if (result != CONFIG_STORE_OK) {
    return result;
  }

  result = store->backend.read(store->backend.context,
                               config_store_slot_address(target_slot),
                               slot_buffer, CONFIG_STORE_SLOT_SIZE_BYTES);
  if (result != CONFIG_STORE_OK) {
    return result;
  }

  memset(&readback, 0, sizeof(readback));
  if (config_store_parse_slot(slot_buffer, &readback) == 0U ||
      config_store_records_equal(&expected, &readback) == 0U) {
    return CONFIG_STORE_VERIFY_FAILED;
  }

  store->active_slot = target_slot;
  store->sequence = next_sequence;
  metadata->selected_slot = target_slot;
  metadata->sequence = next_sequence;
  metadata->payload_length = payload_length;
  metadata->format_version = CONFIG_STORE_FORMAT_VERSION;
  metadata->valid_slot_mask = config_store_slot_bit(target_slot);
  return CONFIG_STORE_OK;
}

config_store_status_t
config_store_load(config_store_t *store, uint8_t *payload,
                  uint8_t payload_capacity,
                  config_store_metadata_t *metadata)
{
  config_store_status_t result;

  if (store == NULL) {
    return CONFIG_STORE_INVALID_ARGUMENT;
  }
  if (store->backend.lock != NULL) {
    result = store->backend.lock(store->backend.context);
    if (result != CONFIG_STORE_OK) {
      return result;
    }
  }

  result =
      config_store_load_impl(store, payload, payload_capacity, metadata);

  if (store->backend.unlock != NULL) {
    store->backend.unlock(store->backend.context);
  }
  return result;
}

config_store_status_t config_store_save(config_store_t *store,
                                        const uint8_t *payload,
                                        uint8_t payload_length,
                                        config_store_metadata_t *metadata)
{
  config_store_status_t result;

  if (store == NULL) {
    return CONFIG_STORE_INVALID_ARGUMENT;
  }
  if (store->backend.lock != NULL) {
    result = store->backend.lock(store->backend.context);
    if (result != CONFIG_STORE_OK) {
      return result;
    }
  }

  result =
      config_store_save_impl(store, payload, payload_length, metadata);

  if (store->backend.unlock != NULL) {
    store->backend.unlock(store->backend.context);
  }
  return result;
}
