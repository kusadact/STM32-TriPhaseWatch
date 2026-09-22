#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ds18b20.h"

#define FAKE_MAX_DEVICES 3U
#define FAKE_COMMAND_BITS 8U
#define FAKE_MATCH_BITS 64U
#define FAKE_SCRATCHPAD_BITS 72U

typedef enum {
  FAKE_PHASE_COMMAND = 0,
  FAKE_PHASE_MATCH_ROM,
  FAKE_PHASE_SEARCH_ROM,
  FAKE_PHASE_READ_ROM,
  FAKE_PHASE_READ_SCRATCHPAD
} fake_phase_t;

typedef struct {
  uint8_t rom[DS18B20_ROM_SIZE];
  uint8_t scratchpad[9];
  bool present;
} fake_device_t;

typedef struct {
  uint64_t now_us;
  bool host_low;
  uint64_t low_started_us;
  uint64_t presence_start_us;
  uint64_t presence_until_us;
  bool stuck_low;
  fake_device_t devices[FAKE_MAX_DEVICES];
  uint8_t device_count;
  uint8_t selected_mask;
  uint8_t active_mask;
  fake_phase_t phase;
  uint8_t command_bits;
  uint8_t command_value;
  uint8_t command;
  uint8_t match_bits;
  uint8_t match_rom[DS18B20_ROM_SIZE];
  uint8_t search_bit;
  bool search_complement;
  bool search_expect_direction;
  uint8_t read_byte_index;
  uint8_t read_bit_index;
  uint8_t read_bytes[9];
} fake_bus_t;

static unsigned int g_checks;
static unsigned int g_failures;

static void check_true(int condition, const char *expression, int line)
{
  g_checks++;
  if (!condition) {
    g_failures++;
    printf("FAIL line %d: %s\n", line, expression);
  }
}

#define CHECK(condition) check_true((condition) != 0, #condition, __LINE__)

static void make_rom(fake_device_t *device, uint8_t serial)
{
  uint8_t index;

  memset(device->rom, 0, sizeof(device->rom));
  device->rom[0] = DS18B20_FAMILY_CODE;
  for (index = 1U; index < 7U; ++index) {
    device->rom[index] = (uint8_t)(serial + index);
  }
  device->rom[7] = ds18b20_crc8(device->rom, 7U);
}

static void make_scratchpad(fake_device_t *device, int16_t temperature_x16)
{
  uint16_t raw = (uint16_t)temperature_x16;

  memset(device->scratchpad, 0, sizeof(device->scratchpad));
  device->scratchpad[0] = (uint8_t)raw;
  device->scratchpad[1] = (uint8_t)(raw >> 8U);
  device->scratchpad[2] = 0x4BU;
  device->scratchpad[3] = 0x46U;
  device->scratchpad[4] = 0x1FU;
  device->scratchpad[8] = ds18b20_crc8(device->scratchpad, 8U);
}

static fake_bus_t make_bus(uint8_t device_count)
{
  fake_bus_t bus;
  uint8_t index;

  memset(&bus, 0, sizeof(bus));
  bus.device_count = device_count;
  bus.phase = FAKE_PHASE_COMMAND;
  for (index = 0U; index < device_count; ++index) {
    bus.devices[index].present = true;
    make_rom(&bus.devices[index], (uint8_t)(index + 1U));
    make_scratchpad(&bus.devices[index], (int16_t)(index * 16));
  }
  bus.active_mask = (uint8_t)((1U << device_count) - 1U);
  return bus;
}

static uint64_t fake_now_us(void *context)
{
  fake_bus_t *bus = (fake_bus_t *)context;
  return bus->now_us;
}

static void fake_delay_us(void *context, uint32_t delay_us)
{
  fake_bus_t *bus = (fake_bus_t *)context;
  bus->now_us += delay_us;
}

static void fake_drive_low(void *context)
{
  fake_bus_t *bus = (fake_bus_t *)context;

  if (!bus->host_low) {
    bus->host_low = true;
    bus->low_started_us = bus->now_us;
  }
}

static void fake_reset(void *bus_context)
{
  fake_bus_t *bus = (fake_bus_t *)bus_context;
  uint8_t index;
  uint8_t present_mask = 0U;

  for (index = 0U; index < bus->device_count; ++index) {
    if (bus->devices[index].present) {
      present_mask |= (uint8_t)(1U << index);
    }
  }
  bus->phase = FAKE_PHASE_COMMAND;
  bus->command_bits = 0U;
  bus->command_value = 0U;
  bus->command = 0U;
  bus->match_bits = 0U;
  bus->search_bit = 0U;
  bus->search_complement = false;
  bus->search_expect_direction = false;
  bus->read_byte_index = 0U;
  bus->read_bit_index = 0U;
  bus->selected_mask = 0U;
  bus->active_mask = present_mask;
  if (bus->stuck_low) {
    bus->presence_start_us = 0U;
    bus->presence_until_us = UINT64_MAX;
  } else if (present_mask != 0U) {
    bus->presence_start_us = bus->now_us + 30U;
    bus->presence_until_us = bus->now_us + 90U;
  } else {
    bus->presence_start_us = 0U;
    bus->presence_until_us = 0U;
  }
}

static void fake_process_command(fake_bus_t *bus)
{
  uint8_t index;

  bus->command_bits = 0U;
  bus->command_value = 0U;
  switch (bus->command) {
    case 0x55U:
      bus->phase = FAKE_PHASE_MATCH_ROM;
      bus->match_bits = 0U;
      memset(bus->match_rom, 0, sizeof(bus->match_rom));
      break;
    case 0xCCU:
      bus->selected_mask = 0U;
      for (index = 0U; index < bus->device_count; ++index) {
        if (bus->devices[index].present) {
          bus->selected_mask |= (uint8_t)(1U << index);
        }
      }
      bus->phase = FAKE_PHASE_COMMAND;
      break;
    case 0xF0U:
      bus->phase = FAKE_PHASE_SEARCH_ROM;
      bus->active_mask = 0U;
      for (index = 0U; index < bus->device_count; ++index) {
        if (bus->devices[index].present) {
          bus->active_mask |= (uint8_t)(1U << index);
        }
      }
      bus->search_bit = 0U;
      bus->search_complement = false;
      bus->search_expect_direction = false;
      break;
    case 0xBEU:
      bus->phase = FAKE_PHASE_READ_SCRATCHPAD;
      bus->read_byte_index = 0U;
      bus->read_bit_index = 0U;
      memset(bus->read_bytes, 0, sizeof(bus->read_bytes));
      for (index = 0U; index < bus->device_count; ++index) {
        if (bus->devices[index].present &&
            (bus->selected_mask & (uint8_t)(1U << index)) != 0U) {
          memcpy(bus->read_bytes, bus->devices[index].scratchpad,
                 sizeof(bus->read_bytes));
          break;
        }
      }
      break;
    case 0x33U:
      bus->phase = FAKE_PHASE_READ_ROM;
      bus->read_byte_index = 0U;
      bus->read_bit_index = 0U;
      memset(bus->read_bytes, 0, sizeof(bus->read_bytes));
      for (index = 0U; index < bus->device_count; ++index) {
        if (bus->devices[index].present &&
            (bus->selected_mask & (uint8_t)(1U << index)) != 0U) {
          memcpy(bus->read_bytes, bus->devices[index].rom,
                 DS18B20_ROM_SIZE);
          break;
        }
      }
      break;
    case 0x44U:
    default:
      bus->phase = FAKE_PHASE_COMMAND;
      break;
  }
}

static void fake_write_bit(fake_bus_t *bus, bool value)
{
  if (bus->phase == FAKE_PHASE_MATCH_ROM) {
    uint8_t byte_index = (uint8_t)(bus->match_bits / 8U);
    uint8_t bit_index = (uint8_t)(bus->match_bits % 8U);
    if (value) {
      bus->match_rom[byte_index] |= (uint8_t)(1U << bit_index);
    }
    bus->match_bits++;
    if (bus->match_bits >= FAKE_MATCH_BITS) {
      uint8_t index;
      bus->selected_mask = 0U;
      for (index = 0U; index < bus->device_count; ++index) {
        if (bus->devices[index].present &&
            memcmp(bus->match_rom, bus->devices[index].rom,
                   DS18B20_ROM_SIZE) == 0) {
          bus->selected_mask = (uint8_t)(1U << index);
          break;
        }
      }
      bus->phase = FAKE_PHASE_COMMAND;
      bus->match_bits = 0U;
    }
    return;
  }

  if (bus->phase == FAKE_PHASE_SEARCH_ROM &&
      bus->search_expect_direction) {
    uint8_t index;
    uint8_t mask = 0U;

    for (index = 0U; index < bus->device_count; ++index) {
      if (!bus->devices[index].present ||
          (bus->active_mask & (uint8_t)(1U << index)) == 0U) {
        continue;
      }
      if (((bus->devices[index].rom[bus->search_bit / 8U] >>
            (bus->search_bit % 8U)) & 1U) == (value ? 1U : 0U)) {
        mask |= (uint8_t)(1U << index);
      }
    }
    bus->active_mask = mask;
    bus->search_bit++;
    bus->search_complement = false;
    bus->search_expect_direction = false;
    return;
  }

  bus->command_value |=
      (uint8_t)((value ? 1U : 0U) << bus->command_bits);
  bus->command_bits++;
  if (bus->command_bits >= FAKE_COMMAND_BITS) {
    bus->command = bus->command_value;
    fake_process_command(bus);
  }
}

static void fake_release_bus(void *context)
{
  fake_bus_t *bus = (fake_bus_t *)context;
  uint64_t low_us;

  if (!bus->host_low) {
    return;
  }
  low_us = bus->now_us - bus->low_started_us;
  bus->host_low = false;
  if (low_us >= 480U) {
    fake_reset(bus);
    return;
  }
  if ((bus->phase == FAKE_PHASE_SEARCH_ROM) &&
      !bus->search_expect_direction) {
    return;
  }
  if ((bus->phase == FAKE_PHASE_READ_ROM) ||
      (bus->phase == FAKE_PHASE_READ_SCRATCHPAD)) {
    return;
  }
  fake_write_bit(bus, low_us < 15U);
}

static bool fake_read_level(void *context)
{
  fake_bus_t *bus = (fake_bus_t *)context;
  uint8_t value = 1U;
  uint8_t index;

  if (bus->stuck_low) {
    return false;
  }
  if ((bus->now_us >= bus->presence_start_us) &&
      (bus->now_us < bus->presence_until_us)) {
    return false;
  }
  if (bus->phase == FAKE_PHASE_SEARCH_ROM) {
    for (index = 0U; index < bus->device_count; ++index) {
      if (bus->devices[index].present &&
          (bus->active_mask & (uint8_t)(1U << index)) != 0U) {
        uint8_t bit = (uint8_t)((bus->devices[index].rom[bus->search_bit / 8U] >>
                                 (bus->search_bit % 8U)) & 1U);
        if (bus->search_complement) {
          bit = (uint8_t)(bit ^ 1U);
        }
        value &= bit;
      }
    }
    bus->search_complement = !bus->search_complement;
    if (!bus->search_complement) {
      bus->search_expect_direction = true;
    }
    return value != 0U;
  }
  if ((bus->phase == FAKE_PHASE_READ_ROM) ||
      (bus->phase == FAKE_PHASE_READ_SCRATCHPAD)) {
    uint8_t byte_index = bus->read_byte_index;
    if (byte_index < sizeof(bus->read_bytes)) {
      value = (uint8_t)((bus->read_bytes[byte_index] >>
                         bus->read_bit_index) & 1U);
    }
    bus->read_bit_index++;
    if (bus->read_bit_index >= 8U) {
      bus->read_bit_index = 0U;
      bus->read_byte_index++;
      if ((bus->phase == FAKE_PHASE_READ_ROM) &&
          (bus->read_byte_index >= DS18B20_ROM_SIZE)) {
        bus->phase = FAKE_PHASE_COMMAND;
      } else if ((bus->phase == FAKE_PHASE_READ_SCRATCHPAD) &&
                 (bus->read_byte_index >= 9U)) {
        bus->phase = FAKE_PHASE_COMMAND;
      }
    }
    return value != 0U;
  }
  return true;
}

static ds18b20_port_t make_port(fake_bus_t *bus)
{
  ds18b20_port_t port;

  port.context = bus;
  port.now_us = fake_now_us;
  port.delay_us = fake_delay_us;
  port.drive_low = fake_drive_low;
  port.release_bus = fake_release_bus;
  port.read_level = fake_read_level;
  return port;
}

static void test_crc_and_rom_validation(void)
{
  uint8_t known[8] = {0x28U, 0xFFU, 0x64U, 0x1EU,
                      0x5BU, 0x16U, 0x03U, 0x75U};
  ds18b20_rom_t rom;

  CHECK(ds18b20_crc8(known, 7U) == 0x75U);
  memcpy(rom.bytes, known, sizeof(known));
  CHECK(ds18b20_rom_is_valid(&rom));
  rom.bytes[7] ^= 1U;
  CHECK(!ds18b20_rom_is_valid(&rom));
  rom.bytes[7] ^= 1U;
  rom.bytes[0] = 0x10U;
  CHECK(!ds18b20_rom_is_valid(&rom));
}

static void test_search_three_devices(void)
{
  fake_bus_t bus = make_bus(3U);
  ds18b20_port_t port = make_port(&bus);
  ds18b20_t device;
  ds18b20_rom_t found[3];
  uint8_t count = 0U;
  uint8_t index;

  ds18b20_init(&device, &port);
  ds18b20_search_start(&device);
  while (count < 3U) {
    CHECK(ds18b20_search_next(&device, &found[count]) ==
          DS18B20_STATUS_OK);
    CHECK(ds18b20_rom_is_valid(&found[count]));
    count++;
  }
  CHECK(ds18b20_search_next(&device, &found[0]) ==
        DS18B20_STATUS_NO_MORE_DEVICES);

  for (index = 0U; index < 3U; ++index) {
    uint8_t matches = 0U;
    uint8_t other;
    for (other = 0U; other < 3U; ++other) {
      if (memcmp(found[index].bytes, bus.devices[other].rom,
                 DS18B20_ROM_SIZE) == 0) {
        matches++;
      }
    }
    CHECK(matches == 1U);
  }
}

static void test_scratchpad_read_and_errors(void)
{
  fake_bus_t bus = make_bus(2U);
  ds18b20_port_t port = make_port(&bus);
  ds18b20_t device;
  ds18b20_rom_t rom;
  ds18b20_sample_t sample;

  make_scratchpad(&bus.devices[0], 401);
  make_scratchpad(&bus.devices[1], -162);
  memcpy(rom.bytes, bus.devices[0].rom, sizeof(rom.bytes));
  ds18b20_init(&device, &port);

  CHECK(ds18b20_start_conversion_all(&device) == DS18B20_STATUS_OK);
  CHECK(ds18b20_read_scratchpad(&device, &rom, &sample) ==
        DS18B20_STATUS_OK);
  CHECK(sample.temperature_x16 == 401);

  memcpy(rom.bytes, bus.devices[1].rom, sizeof(rom.bytes));
  CHECK(ds18b20_read_scratchpad(&device, &rom, &sample) ==
        DS18B20_STATUS_OK);
  CHECK(sample.temperature_x16 == -162);

  bus.devices[1].scratchpad[8] ^= 1U;
  CHECK(ds18b20_read_scratchpad(&device, &rom, &sample) ==
        DS18B20_STATUS_SCRATCHPAD_CRC_ERROR);

  make_scratchpad(&bus.devices[1], 3000);
  CHECK(ds18b20_read_scratchpad(&device, &rom, &sample) ==
        DS18B20_STATUS_RANGE_ERROR);
}

static void test_reset_failures(void)
{
  fake_bus_t empty = make_bus(0U);
  ds18b20_port_t port = make_port(&empty);
  ds18b20_t device;

  ds18b20_init(&device, &port);
  CHECK(ds18b20_reset(&device) == DS18B20_STATUS_RESET_TIMEOUT);

  empty.stuck_low = true;
  CHECK(ds18b20_reset(&device) == DS18B20_STATUS_BUS_STUCK_LOW);
}

int main(void)
{
  test_crc_and_rom_validation();
  test_search_three_devices();
  test_scratchpad_read_and_errors();
  test_reset_failures();
  printf("DS18B20 host tests: %u checks, %u failures\n",
         g_checks, g_failures);
  return g_failures == 0U ? 0 : 1;
}
