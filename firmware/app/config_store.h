#ifndef BUSCOMM_CONFIG_STORE_H
#define BUSCOMM_CONFIG_STORE_H

#include <stdint.h>

/*
 * AT24C02 is divided into two independent 128-byte records.
 *
 * Persisted record layout:
 *   0..1   magic 0x57 0x43 ("WC")
 *   2      format version
 *   3      flags (must be zero)
 *   4..7   sequence, little-endian
 *   8..9   payload length, little-endian
 *   10..121 payload area, zero padded
 *   122..123 CRC-16/CCITT over bytes 0..121, little-endian
 *   124..127 reserved (must be zero)
 */
#define CONFIG_STORE_FORMAT_VERSION 1U
#define CONFIG_STORE_SLOT_COUNT 2U
#define CONFIG_STORE_SLOT_SIZE_BYTES 128U
#define CONFIG_STORE_PAYLOAD_MAX_BYTES 112U
#define CONFIG_STORE_SLOT_NONE 0xFFU

typedef enum {
  CONFIG_STORE_OK = 0,
  CONFIG_STORE_NO_VALID_RECORD,
  CONFIG_STORE_INVALID_ARGUMENT,
  CONFIG_STORE_INVALID_RECORD,
  CONFIG_STORE_IO_ERROR,
  CONFIG_STORE_CONFLICT,
  CONFIG_STORE_VERIFY_FAILED,
  CONFIG_STORE_NOT_READY,
  CONFIG_STORE_BUSY
} config_store_status_t;

typedef config_store_status_t (*config_store_read_fn)(
    void *context, uint16_t address, uint8_t *data, uint16_t length);
typedef config_store_status_t (*config_store_write_fn)(
    void *context, uint16_t address, const uint8_t *data, uint16_t length);
typedef config_store_status_t (*config_store_validate_fn)(
    void *context, const uint8_t *payload, uint8_t payload_length);
typedef config_store_status_t (*config_store_lock_fn)(void *context);
typedef void (*config_store_unlock_fn)(void *context);

typedef struct {
  config_store_read_fn read;
  config_store_write_fn write;
  config_store_validate_fn validate;
  config_store_lock_fn lock;
  config_store_unlock_fn unlock;
  void *context;
} config_store_backend_t;

typedef struct {
  uint8_t selected_slot;
  uint32_t sequence;
  uint8_t payload_length;
  uint8_t format_version;
  uint8_t valid_slot_mask;
  uint8_t read_error_mask;
  uint8_t format_error_mask;
} config_store_metadata_t;

typedef struct {
  config_store_backend_t backend;
  uint32_t sequence;
  uint8_t active_slot;
  uint8_t initialized;
} config_store_t;

/*
 * A config_store_t is not thread-safe. The owner must serialize load/save
 * calls, for example by running all EEPROM operations in one storage task.
 */
config_store_status_t
config_store_init(config_store_t *store, const config_store_backend_t *backend);

config_store_status_t
config_store_load(config_store_t *store, uint8_t *payload,
                  uint8_t payload_capacity,
                  config_store_metadata_t *metadata);

config_store_status_t config_store_save(config_store_t *store,
                                        const uint8_t *payload,
                                        uint8_t payload_length,
                                        config_store_metadata_t *metadata);

#endif /* BUSCOMM_CONFIG_STORE_H */
