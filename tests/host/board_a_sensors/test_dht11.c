#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dht11.h"
#include "sensor_manager.h"

typedef enum {
  FAKE_DATA = 0,
  FAKE_RESPONSE_TIMEOUT,
  FAKE_BIT_TIMEOUT
} fake_mode_t;

typedef struct {
  fake_mode_t mode;
  uint8_t bytes[5];
} fake_sensor_t;

typedef struct {
  uint64_t now_us;
  uint64_t release_us;
  uint8_t active_sensor;
  bool active;
  unsigned int begin_calls;
  unsigned int end_calls;
  uint8_t drive_order[32];
  unsigned int drive_count;
  fake_sensor_t sensors[BOARD_A_SENSOR_COUNT];
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

static bool fake_bit(const fake_sensor_t *sensor, uint8_t bit_index)
{
  uint8_t byte_index = (uint8_t)(bit_index / 8U);
  uint8_t mask = (uint8_t)(0x80U >> (bit_index % 8U));

  return (sensor->bytes[byte_index] & mask) != 0U;
}

static bool fake_level_at(const fake_bus_t *bus, uint64_t now_us)
{
  const fake_sensor_t *sensor;
  uint64_t elapsed;
  uint8_t bit_index;

  if (!bus->active || (bus->active_sensor >= BOARD_A_SENSOR_COUNT)) {
    return true;
  }
  sensor = &bus->sensors[bus->active_sensor];
  if (sensor->mode == FAKE_RESPONSE_TIMEOUT) {
    return true;
  }
  if (now_us < bus->release_us) {
    return false;
  }

  elapsed = now_us - bus->release_us;
  if (elapsed < 20U) {
    return true;
  }
  if (elapsed < 100U) {
    return false;
  }
  if (elapsed < 180U) {
    return true;
  }
  if (sensor->mode == FAKE_BIT_TIMEOUT) {
    return false;
  }

  elapsed -= 180U;
  for (bit_index = 0U; bit_index < 40U; ++bit_index) {
    uint64_t low_us = 50U;
    uint64_t high_us = fake_bit(sensor, bit_index) ? 70U : 28U;

    if (elapsed < low_us) {
      return false;
    }
    elapsed -= low_us;
    if (elapsed < high_us) {
      return true;
    }
    elapsed -= high_us;
  }
  return true;
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

static void fake_drive_low(void *context, uint8_t sensor_id)
{
  fake_bus_t *bus = (fake_bus_t *)context;

  if (bus->drive_count <
      (sizeof(bus->drive_order) / sizeof(bus->drive_order[0]))) {
    bus->drive_order[bus->drive_count] = sensor_id;
  }
  bus->drive_count++;
  bus->active_sensor = sensor_id;
  bus->active = true;
  bus->release_us = 0U;
}

static void fake_release_input(void *context, uint8_t sensor_id)
{
  fake_bus_t *bus = (fake_bus_t *)context;

  (void)sensor_id;
  bus->active = true;
  bus->release_us = bus->now_us;
}

static bool fake_read_level(void *context, uint8_t sensor_id)
{
  fake_bus_t *bus = (fake_bus_t *)context;
  bool level = fake_level_at(bus, bus->now_us);

  (void)sensor_id;
  bus->now_us++;
  return level;
}

static void fake_begin_read(void *context, uint8_t sensor_id)
{
  fake_bus_t *bus = (fake_bus_t *)context;

  (void)sensor_id;
  bus->begin_calls++;
}

static void fake_end_read(void *context, uint8_t sensor_id)
{
  fake_bus_t *bus = (fake_bus_t *)context;

  (void)sensor_id;
  bus->end_calls++;
}

static dht11_port_t make_fake_port(fake_bus_t *bus)
{
  dht11_port_t port;

  port.context = bus;
  port.now_us = fake_now_us;
  port.delay_us = fake_delay_us;
  port.drive_low = fake_drive_low;
  port.release_input = fake_release_input;
  port.read_level = fake_read_level;
  port.begin_read = fake_begin_read;
  port.end_read = fake_end_read;
  return port;
}

static uint8_t checksum(const uint8_t bytes[5])
{
  return (uint8_t)(bytes[0] + bytes[1] + bytes[2] + bytes[3]);
}

static void set_frame(fake_bus_t *bus, uint8_t sensor_id,
                      uint8_t humidity, uint8_t temperature)
{
  fake_sensor_t *sensor = &bus->sensors[sensor_id];

  sensor->mode = FAKE_DATA;
  sensor->bytes[0] = humidity;
  sensor->bytes[1] = 0U;
  sensor->bytes[2] = temperature;
  sensor->bytes[3] = 0U;
  sensor->bytes[4] = checksum(sensor->bytes);
}

static void test_driver_valid_and_interval(void)
{
  fake_bus_t bus;
  dht11_port_t port;
  dht11_t device;
  dht11_sample_t sample;
  dht11_status_t status;
  unsigned int drive_count;

  memset(&bus, 0, sizeof(bus));
  port = make_fake_port(&bus);
  bus.sensors[0].mode = FAKE_DATA;
  set_frame(&bus, 0U, 45U, 23U);
  dht11_init(&device, &port, 0U);

  status = dht11_read(&device, &sample);
  if (status != DHT11_STATUS_OK) {
    printf("valid frame status=%s bytes=%u,%u,%u,%u,%u now=%llu\n",
           dht11_status_name(status),
           (unsigned int)bus.sensors[0].bytes[0],
           (unsigned int)bus.sensors[0].bytes[1],
           (unsigned int)bus.sensors[0].bytes[2],
           (unsigned int)bus.sensors[0].bytes[3],
           (unsigned int)bus.sensors[0].bytes[4],
           (unsigned long long)bus.now_us);
  }
  CHECK(status == DHT11_STATUS_OK);
  CHECK(sample.humidity_x10 == 450U);
  CHECK(sample.temperature_x10 == 230U);
  CHECK(bus.begin_calls == 1U);
  CHECK(bus.end_calls == 1U);

  drive_count = bus.drive_count;
  bus.now_us = device.last_start_us + DHT11_MIN_INTERVAL_US - 1U;
  CHECK(dht11_read(&device, &sample) == DHT11_STATUS_TOO_SOON);
  CHECK(bus.drive_count == drive_count);

  bus.now_us = device.last_start_us + DHT11_MIN_INTERVAL_US;
  CHECK(dht11_read(&device, &sample) == DHT11_STATUS_OK);
  CHECK(bus.drive_count == drive_count + 1U);
}

static void test_driver_timeouts_and_errors(void)
{
  fake_bus_t bus;
  dht11_port_t port;
  dht11_t device;
  dht11_sample_t sample;

  memset(&bus, 0, sizeof(bus));
  port = make_fake_port(&bus);
  bus.sensors[0].mode = FAKE_RESPONSE_TIMEOUT;
  dht11_init(&device, &port, 0U);
  CHECK(dht11_read(&device, &sample) ==
        DHT11_STATUS_TIMEOUT_RESPONSE);
  CHECK(bus.end_calls == 1U);

  memset(&bus, 0, sizeof(bus));
  port = make_fake_port(&bus);
  bus.sensors[0].mode = FAKE_BIT_TIMEOUT;
  dht11_init(&device, &port, 0U);
  CHECK(dht11_read(&device, &sample) == DHT11_STATUS_TIMEOUT_BIT);

  memset(&bus, 0, sizeof(bus));
  port = make_fake_port(&bus);
  set_frame(&bus, 0U, 45U, 23U);
  bus.sensors[0].bytes[0] ^= 2U;
  dht11_init(&device, &port, 0U);
  CHECK(dht11_read(&device, &sample) ==
        DHT11_STATUS_CHECKSUM_ERROR);

  memset(&bus, 0, sizeof(bus));
  port = make_fake_port(&bus);
  set_frame(&bus, 0U, 10U, 23U);
  dht11_init(&device, &port, 0U);
  CHECK(dht11_read(&device, &sample) == DHT11_STATUS_RANGE_ERROR);

  memset(&bus, 0, sizeof(bus));
  port = make_fake_port(&bus);
  set_frame(&bus, 0U, 20U, 0U);
  dht11_init(&device, &port, 0U);
  CHECK(dht11_read(&device, &sample) == DHT11_STATUS_OK);
  CHECK(sample.humidity_x10 == 200U);
  CHECK(sample.temperature_x10 == 0U);

  memset(&bus, 0, sizeof(bus));
  port = make_fake_port(&bus);
  set_frame(&bus, 0U, 90U, 50U);
  dht11_init(&device, &port, 0U);
  CHECK(dht11_read(&device, &sample) == DHT11_STATUS_OK);
  CHECK(sample.humidity_x10 == 900U);
  CHECK(sample.temperature_x10 == 500U);

  memset(&bus, 0, sizeof(bus));
  port = make_fake_port(&bus);
  set_frame(&bus, 0U, 45U, 24U);
  dht11_init(&device, &port, 0U);
  CHECK(dht11_read(&device, &sample) == DHT11_STATUS_OK);
  CHECK(sample.humidity_x10 == 450U);
  CHECK(sample.temperature_x10 == 240U);

  memset(&bus, 0, sizeof(bus));
  port = make_fake_port(&bus);
  bus.sensors[0].mode = FAKE_DATA;
  bus.sensors[0].bytes[0] = 45U;
  bus.sensors[0].bytes[1] = 0U;
  bus.sensors[0].bytes[2] = 23U;
  bus.sensors[0].bytes[3] = 5U;
  bus.sensors[0].bytes[4] = 73U;
  dht11_init(&device, &port, 0U);
  CHECK(dht11_read(&device, &sample) == DHT11_STATUS_OK);
  CHECK(sample.temperature_x10 == 235U);
}

static void test_manager_serial_scan_and_isolated_failure(void)
{
  fake_bus_t bus;
  dht11_port_t port;
  board_a_sensor_manager_t manager;
  board_a_sensor_snapshot_t snapshot;
  uint64_t next_scan_us;

  memset(&bus, 0, sizeof(bus));
  port = make_fake_port(&bus);
  set_frame(&bus, 0U, 45U, 23U);
  set_frame(&bus, 1U, 55U, 24U);
  set_frame(&bus, 2U, 65U, 25U);
  board_a_sensor_manager_init(&manager, &port);

  CHECK(board_a_sensor_manager_scan(&manager, bus.now_us, &snapshot));
  CHECK(snapshot.valid_mask == 0x0007U);
  CHECK(snapshot.sample_id == 1U);
  CHECK(snapshot.sensors[0].quality == BOARD_A_QUALITY_OK);
  CHECK(snapshot.sensors[1].quality == BOARD_A_QUALITY_OK);
  CHECK(snapshot.sensors[2].quality == BOARD_A_QUALITY_OK);
  CHECK(snapshot.sensors[0].temperature_x10 == 230U);
  CHECK(snapshot.sensors[1].temperature_x10 == 240U);
  CHECK(snapshot.sensors[2].temperature_x10 == 250U);
  CHECK(bus.drive_count == 3U);
  CHECK(bus.drive_order[0] == 0U);
  CHECK(bus.drive_order[1] == 1U);
  CHECK(bus.drive_order[2] == 2U);

  next_scan_us = board_a_sensor_manager_next_scan_us(&manager);
  CHECK(next_scan_us == BOARD_A_SENSOR_SCAN_PERIOD_US);
  bus.now_us = next_scan_us - 1U;
  CHECK(!board_a_sensor_manager_scan(&manager, bus.now_us, &snapshot));
  CHECK(bus.drive_count == 3U);
  bus.now_us = next_scan_us;
  CHECK(board_a_sensor_manager_scan(&manager, bus.now_us, &snapshot));
  CHECK(snapshot.sample_id == 2U);
  CHECK(bus.drive_count == 6U);
}

static void test_manager_checksum_and_missing_do_not_stop_others(void)
{
  fake_bus_t bus;
  dht11_port_t port;
  board_a_sensor_manager_t manager;
  board_a_sensor_snapshot_t snapshot;

  memset(&bus, 0, sizeof(bus));
  port = make_fake_port(&bus);
  set_frame(&bus, 0U, 41U, 21U);
  set_frame(&bus, 1U, 42U, 22U);
  bus.sensors[1].bytes[4] ^= 0x80U;
  set_frame(&bus, 2U, 43U, 23U);
  board_a_sensor_manager_init(&manager, &port);
  CHECK(board_a_sensor_manager_scan(&manager, bus.now_us, &snapshot));
  CHECK(snapshot.valid_mask == 0x0005U);
  CHECK(snapshot.sensors[1].quality ==
        BOARD_A_QUALITY_CHECKSUM_ERROR);
  CHECK(snapshot.sensors[1].error == BOARD_A_SENSOR_ERROR_CHECKSUM);
  CHECK(snapshot.sensors[2].quality == BOARD_A_QUALITY_OK);

  memset(&bus, 0, sizeof(bus));
  port = make_fake_port(&bus);
  set_frame(&bus, 0U, 41U, 21U);
  bus.sensors[1].mode = FAKE_RESPONSE_TIMEOUT;
  set_frame(&bus, 2U, 43U, 23U);
  board_a_sensor_manager_init(&manager, &port);
  CHECK(board_a_sensor_manager_scan(&manager, bus.now_us, &snapshot));
  CHECK(snapshot.valid_mask == 0x0005U);
  CHECK(snapshot.sensors[1].quality == BOARD_A_QUALITY_TIMEOUT);
  CHECK(snapshot.sensors[1].error ==
        BOARD_A_SENSOR_ERROR_TIMEOUT_RESPONSE);
  CHECK(snapshot.sensors[0].quality == BOARD_A_QUALITY_OK);
  CHECK(snapshot.sensors[2].quality == BOARD_A_QUALITY_OK);
}

static void test_manager_stale_then_not_present(void)
{
  fake_bus_t bus;
  dht11_port_t port;
  board_a_sensor_manager_t manager;
  board_a_sensor_snapshot_t snapshot;
  uint8_t scan;

  memset(&bus, 0, sizeof(bus));
  port = make_fake_port(&bus);
  set_frame(&bus, 0U, 35U, 20U);
  set_frame(&bus, 1U, 45U, 21U);
  set_frame(&bus, 2U, 55U, 22U);
  board_a_sensor_manager_init(&manager, &port);
  CHECK(board_a_sensor_manager_scan(&manager, bus.now_us, &snapshot));

  bus.sensors[1].bytes[0] ^= 2U;
  bus.now_us += BOARD_A_SENSOR_SCAN_PERIOD_US;
  CHECK(board_a_sensor_manager_scan(&manager, bus.now_us, &snapshot));
  CHECK(snapshot.sensors[1].quality == BOARD_A_QUALITY_STALE);
  CHECK(snapshot.sensors[1].error == BOARD_A_SENSOR_ERROR_CHECKSUM);
  CHECK(snapshot.sensors[1].temperature_x10 == 210U);
  CHECK(snapshot.sensors[1].humidity_x10 == 450U);
  CHECK((snapshot.valid_mask & 0x0002U) != 0U);

  for (scan = 0U; scan < 2U; ++scan) {
    bus.now_us += BOARD_A_SENSOR_SCAN_PERIOD_US;
    CHECK(board_a_sensor_manager_scan(&manager, bus.now_us, &snapshot));
  }
  CHECK(snapshot.sensors[1].quality == BOARD_A_QUALITY_NOT_PRESENT);
  CHECK(!snapshot.sensors[1].has_value);
  CHECK(snapshot.sensors[1].temperature_x10 == 0U);
  CHECK(snapshot.sensors[1].humidity_x10 == 0U);
  CHECK((snapshot.valid_mask & 0x0002U) == 0U);
  CHECK(snapshot.sensors[0].quality == BOARD_A_QUALITY_OK);
  CHECK(snapshot.sensors[2].quality == BOARD_A_QUALITY_OK);
}

static void test_manager_missing_stays_not_present_across_counter_boundary(void)
{
  fake_bus_t bus;
  dht11_port_t port;
  board_a_sensor_manager_t manager;
  board_a_sensor_snapshot_t snapshot;
  uint16_t scan;

  memset(&bus, 0, sizeof(bus));
  port = make_fake_port(&bus);
  set_frame(&bus, 0U, 40U, 20U);
  bus.sensors[1].mode = FAKE_RESPONSE_TIMEOUT;
  set_frame(&bus, 2U, 60U, 30U);
  board_a_sensor_manager_init(&manager, &port);

  for (scan = 0U; scan < 300U; ++scan) {
    CHECK(board_a_sensor_manager_scan(&manager, bus.now_us, &snapshot));
    if (scan >= 2U) {
      CHECK(snapshot.sensors[1].quality ==
            BOARD_A_QUALITY_NOT_PRESENT);
      CHECK(!snapshot.sensors[1].has_value);
      CHECK((snapshot.valid_mask & 0x0002U) == 0U);
    }
    bus.now_us += BOARD_A_SENSOR_SCAN_PERIOD_US;
  }
}

int main(void)
{
  test_driver_valid_and_interval();
  test_driver_timeouts_and_errors();
  test_manager_serial_scan_and_isolated_failure();
  test_manager_checksum_and_missing_do_not_stop_others();
  test_manager_stale_then_not_present();
  test_manager_missing_stays_not_present_across_counter_boundary();

  printf("DHT11 sensor host tests: %u checks, %u failures\n",
         g_checks, g_failures);
  return g_failures == 0U ? 0 : 1;
}
