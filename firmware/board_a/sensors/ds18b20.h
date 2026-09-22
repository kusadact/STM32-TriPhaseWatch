#ifndef BOARD_A_DS18B20_H
#define BOARD_A_DS18B20_H

#include <stdbool.h>
#include <stdint.h>

#define DS18B20_ROM_SIZE 8U
#define DS18B20_FAMILY_CODE 0x28U
#define DS18B20_CONVERSION_US 750000U

#define DS18B20_RESET_LOW_US 500U
#define DS18B20_PRESENCE_WAIT_US 70U
#define DS18B20_PRESENCE_TIMEOUT_US 500U
#define DS18B20_WRITE_ONE_LOW_US 2U
#define DS18B20_WRITE_ZERO_LOW_US 60U
#define DS18B20_READ_LOW_US 2U
#define DS18B20_READ_SAMPLE_US 12U
#define DS18B20_SLOT_US 62U

typedef enum {
  DS18B20_STATUS_OK = 0,
  DS18B20_STATUS_RESET_TIMEOUT = 1,
  DS18B20_STATUS_BUS_STUCK_LOW = 2,
  DS18B20_STATUS_ROM_CRC_ERROR = 3,
  DS18B20_STATUS_ROM_FAMILY_ERROR = 4,
  DS18B20_STATUS_SEARCH_ERROR = 5,
  DS18B20_STATUS_NO_MORE_DEVICES = 6,
  DS18B20_STATUS_SCRATCHPAD_CRC_ERROR = 7,
  DS18B20_STATUS_RANGE_ERROR = 8,
  DS18B20_STATUS_INVALID_ARGUMENT = 9,
  DS18B20_STATUS_NO_RESPONSE = 10
} ds18b20_status_t;

typedef struct {
  void *context;
  uint64_t (*now_us)(void *context);
  void (*delay_us)(void *context, uint32_t delay_us);
  void (*drive_low)(void *context);
  void (*release_bus)(void *context);
  bool (*read_level)(void *context);
} ds18b20_port_t;

typedef struct {
  uint8_t bytes[DS18B20_ROM_SIZE];
} ds18b20_rom_t;

typedef struct {
  const ds18b20_port_t *port;
  uint8_t last_discrepancy;
  bool last_device;
} ds18b20_t;

typedef struct {
  int16_t temperature_x16;
  uint8_t scratchpad[9];
} ds18b20_sample_t;

void ds18b20_init(ds18b20_t *device, const ds18b20_port_t *port);

ds18b20_status_t ds18b20_reset(ds18b20_t *device);

void ds18b20_search_start(ds18b20_t *device);

ds18b20_status_t ds18b20_search_next(
    ds18b20_t *device, ds18b20_rom_t *rom);

ds18b20_status_t ds18b20_match_rom(
    ds18b20_t *device, const ds18b20_rom_t *rom);

ds18b20_status_t ds18b20_skip_rom(ds18b20_t *device);

ds18b20_status_t ds18b20_start_conversion_all(ds18b20_t *device);

ds18b20_status_t ds18b20_read_scratchpad(
    ds18b20_t *device, const ds18b20_rom_t *rom, ds18b20_sample_t *sample);

bool ds18b20_rom_is_valid(const ds18b20_rom_t *rom);

uint8_t ds18b20_crc8(const uint8_t *data, uint8_t length);

const char *ds18b20_status_name(ds18b20_status_t status);

#endif /* BOARD_A_DS18B20_H */
