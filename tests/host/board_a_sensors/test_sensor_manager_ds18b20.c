#define main ds18b20_driver_test_main
#include "test_ds18b20.c"
#undef main

#include "sensor_manager.h"

static bool discover(
    board_a_sensor_manager_t *manager, fake_bus_t *bus)
{
  board_a_sensor_snapshot_t snapshot;

  if (bus->now_us < manager->next_step_us) {
    bus->now_us = manager->next_step_us;
  }
  return board_a_sensor_manager_step(manager, bus->now_us, &snapshot);
}

static bool read_after_conversion(
    board_a_sensor_manager_t *manager, fake_bus_t *bus,
    board_a_sensor_snapshot_t *snapshot)
{
  bus->now_us += DS18B20_CONVERSION_US;
  return board_a_sensor_manager_step(manager, bus->now_us, snapshot);
}

static bool binding_matches_device(
    const board_a_sensor_binding_t *binding,
    const fake_device_t *device)
{
  return memcmp(binding->rom, device->rom, DS18B20_ROM_SIZE) == 0;
}

static int device_index_for_binding(
    const fake_bus_t *bus, const board_a_sensor_binding_t *binding)
{
  uint8_t index;

  for (index = 0U; index < bus->device_count; ++index) {
    if (binding_matches_device(binding, &bus->devices[index])) {
      return (int)index;
    }
  }
  return -1;
}

static void set_bound_temperature(
    fake_bus_t *bus, const board_a_sensor_binding_t *binding,
    int16_t temperature_x16)
{
  int index = device_index_for_binding(bus, binding);

  CHECK(index >= 0);
  if (index >= 0) {
    make_scratchpad(&bus->devices[index], temperature_x16);
  }
}

static void set_bound_presence(
    fake_bus_t *bus, const board_a_sensor_binding_t *binding,
    bool present)
{
  int index = device_index_for_binding(bus, binding);

  CHECK(index >= 0);
  if (index >= 0) {
    bus->devices[index].present = present;
  }
}

static void test_auto_discovery_and_three_temperatures(void)
{
  fake_bus_t bus = make_bus(3U);
  ds18b20_port_t port = make_port(&bus);
  board_a_sensor_manager_t manager;
  board_a_sensor_snapshot_t snapshot;

  board_a_sensor_manager_init(&manager, &port);
  CHECK(!discover(&manager, &bus));
  CHECK(manager.map.valid_mask == 0x07U);
  CHECK(manager.map_dirty);
  CHECK(manager.conversion_pending);
  set_bound_temperature(&bus, &manager.map.bindings[0], 250);
  set_bound_temperature(&bus, &manager.map.bindings[1], -162);
  set_bound_temperature(&bus, &manager.map.bindings[2], 0);

  CHECK(read_after_conversion(&manager, &bus, &snapshot));
  CHECK(snapshot.sample_id == 1U);
  CHECK(snapshot.valid_mask == 0x07U);
  CHECK(snapshot.sensors[0].temperature_x16 == 250);
  CHECK(snapshot.sensors[1].temperature_x16 == -162);
  CHECK(snapshot.sensors[2].temperature_x16 == 0);
  CHECK(snapshot.sensors[0].quality == BOARD_A_QUALITY_OK);
  CHECK(snapshot.sensors[0].sensor_type == BOARD_A_SENSOR_TYPE_DS18B20);
  CHECK(board_a_sensor_manager_take_map_dirty(&manager));
  CHECK(!board_a_sensor_manager_take_map_dirty(&manager));
}

static void test_missing_stale_and_recovery(void)
{
  fake_bus_t bus = make_bus(3U);
  ds18b20_port_t port = make_port(&bus);
  board_a_sensor_manager_t manager;
  board_a_sensor_snapshot_t snapshot;
  uint8_t scan;

  board_a_sensor_manager_init(&manager, &port);
  CHECK(!discover(&manager, &bus));
  set_bound_temperature(&bus, &manager.map.bindings[0], 250);
  set_bound_temperature(&bus, &manager.map.bindings[1], -162);
  set_bound_temperature(&bus, &manager.map.bindings[2], 0);
  CHECK(read_after_conversion(&manager, &bus, &snapshot));

  set_bound_presence(&bus, &manager.map.bindings[1], false);
  if (bus.now_us < manager.next_step_us) {
    bus.now_us = manager.next_step_us;
  }
  CHECK(!board_a_sensor_manager_step(&manager, bus.now_us, &snapshot));
  CHECK(read_after_conversion(&manager, &bus, &snapshot));
  CHECK(snapshot.sensors[1].quality == BOARD_A_QUALITY_STALE);
  CHECK(snapshot.sensors[1].has_value);
  CHECK(snapshot.sensors[1].temperature_x16 == -162);
  CHECK(snapshot.sensors[0].quality == BOARD_A_QUALITY_OK);
  CHECK(snapshot.sensors[2].quality == BOARD_A_QUALITY_OK);

  for (scan = 0U; scan < 2U; ++scan) {
    if (bus.now_us < manager.next_step_us) {
      bus.now_us = manager.next_step_us;
    }
    CHECK(!board_a_sensor_manager_step(&manager, bus.now_us, &snapshot));
    CHECK(read_after_conversion(&manager, &bus, &snapshot));
  }
  CHECK(snapshot.sensors[1].quality == BOARD_A_QUALITY_NOT_PRESENT);
  CHECK(!snapshot.sensors[1].has_value);
  CHECK((snapshot.valid_mask & 0x0002U) == 0U);

  set_bound_presence(&bus, &manager.map.bindings[1], true);
  if (bus.now_us < manager.next_step_us) {
    bus.now_us = manager.next_step_us;
  }
  CHECK(!board_a_sensor_manager_step(&manager, bus.now_us, &snapshot));
  CHECK(read_after_conversion(&manager, &bus, &snapshot));
  CHECK(snapshot.sensors[1].quality == BOARD_A_QUALITY_OK);
  CHECK(snapshot.sensors[1].temperature_x16 == -162);
  CHECK(snapshot.valid_mask == 0x0007U);
}

static void test_bound_map_survives_enumeration_order_change(void)
{
  fake_bus_t bus = make_bus(3U);
  ds18b20_port_t port = make_port(&bus);
  board_a_sensor_manager_t first_manager;
  board_a_sensor_manager_t second_manager;
  board_a_sensor_map_t map;
  board_a_sensor_snapshot_t snapshot;
  fake_device_t saved;

  board_a_sensor_manager_init(&first_manager, &port);
  CHECK(!discover(&first_manager, &bus));
  set_bound_temperature(&bus, &first_manager.map.bindings[0], 250);
  set_bound_temperature(&bus, &first_manager.map.bindings[1], -162);
  set_bound_temperature(&bus, &first_manager.map.bindings[2], 0);
  CHECK(read_after_conversion(&first_manager, &bus, &snapshot));
  CHECK(board_a_sensor_manager_copy_map(&first_manager, &map));

  saved = bus.devices[0];
  bus.devices[0] = bus.devices[2];
  bus.devices[2] = saved;

  board_a_sensor_manager_init(&second_manager, &port);
  CHECK(board_a_sensor_manager_set_map(&second_manager, &map));
  CHECK(!discover(&second_manager, &bus));
  CHECK(read_after_conversion(&second_manager, &bus, &snapshot));
  CHECK(snapshot.sensors[0].temperature_x16 == 250);
  CHECK(snapshot.sensors[1].temperature_x16 == -162);
  CHECK(snapshot.sensors[2].temperature_x16 == 0);
}

static void test_partial_map_fills_free_slots_without_renumbering(void)
{
  fake_bus_t bus = make_bus(3U);
  ds18b20_port_t port = make_port(&bus);
  board_a_sensor_manager_t manager;
  board_a_sensor_map_t map;
  board_a_sensor_snapshot_t snapshot;
  uint8_t saved_rom[DS18B20_ROM_SIZE];

  memset(&map, 0, sizeof(map));
  map.valid_mask = 0x01U;
  map.bindings[0].bound = true;
  memcpy(map.bindings[0].rom, bus.devices[0].rom, DS18B20_ROM_SIZE);
  map.bindings[0].rom_short = 0x1234U;
  memcpy(saved_rom, bus.devices[0].rom, DS18B20_ROM_SIZE);

  board_a_sensor_manager_init(&manager, &port);
  CHECK(board_a_sensor_manager_set_map(&manager, &map));
  CHECK(!manager.discovery_complete);
  CHECK(!discover(&manager, &bus));
  CHECK(manager.map.valid_mask == 0x07U);
  CHECK(manager.discovery_complete);
  CHECK(manager.map_dirty);
  CHECK(memcmp(manager.map.bindings[0].rom, saved_rom,
               DS18B20_ROM_SIZE) == 0);
  CHECK(manager.map.bindings[0].rom_short == 0x1234U);
  CHECK(device_index_for_binding(&bus, &manager.map.bindings[1]) >= 0);
  CHECK(device_index_for_binding(&bus, &manager.map.bindings[2]) >= 0);
  CHECK(device_index_for_binding(&bus, &manager.map.bindings[1]) !=
        device_index_for_binding(&bus, &manager.map.bindings[2]));
  CHECK(read_after_conversion(&manager, &bus, &snapshot));
  CHECK(snapshot.valid_mask == 0x0007U);
  CHECK(snapshot.sensors[0].quality == BOARD_A_QUALITY_OK);
  CHECK(snapshot.sensors[1].quality == BOARD_A_QUALITY_OK);
  CHECK(snapshot.sensors[2].quality == BOARD_A_QUALITY_OK);
}

static void test_full_map_is_not_rescanned(void)
{
  fake_bus_t bus = make_bus(3U);
  ds18b20_port_t port = make_port(&bus);
  board_a_sensor_manager_t manager;
  board_a_sensor_snapshot_t snapshot;
  uint8_t saved_rom[DS18B20_ROM_SIZE];
  uint8_t slot;
  bool found = false;

  board_a_sensor_manager_init(&manager, &port);
  CHECK(!discover(&manager, &bus));
  CHECK(read_after_conversion(&manager, &bus, &snapshot));
  CHECK(manager.map.valid_mask == 0x07U);
  CHECK(manager.discovery_complete);

  /* The map binds in ROM search order, so locate the slot of device 1 first. */
  for (slot = 0U; slot < BOARD_A_SENSOR_COUNT; ++slot) {
    if (binding_matches_device(&manager.map.bindings[slot], &bus.devices[1])) {
      found = true;
      break;
    }
  }
  CHECK(found);
  memcpy(saved_rom, manager.map.bindings[slot].rom, DS18B20_ROM_SIZE);

  /* A different ROM appears in place of device 1: the slot must not follow it. */
  make_rom(&bus.devices[1], 9U);
  if (bus.now_us < manager.next_step_us) {
    bus.now_us = manager.next_step_us;
  }
  CHECK(!board_a_sensor_manager_step(&manager, bus.now_us, &snapshot));
  CHECK(read_after_conversion(&manager, &bus, &snapshot));
  CHECK(manager.map.valid_mask == 0x07U);
  CHECK(memcmp(manager.map.bindings[slot].rom, saved_rom,
               DS18B20_ROM_SIZE) == 0);
  CHECK(snapshot.sensors[slot].quality == BOARD_A_QUALITY_STALE);
  CHECK(snapshot.sensors[slot].error == BOARD_A_SENSOR_ERROR_RESET_TIMEOUT);
}

static void test_missing_bound_rom_is_not_replaced(void)
{
  fake_bus_t bus = make_bus(2U);
  ds18b20_port_t port = make_port(&bus);
  board_a_sensor_manager_t manager;
  board_a_sensor_map_t map;
  board_a_sensor_snapshot_t snapshot;
  uint8_t saved_rom[DS18B20_ROM_SIZE];

  memset(&map, 0, sizeof(map));
  map.valid_mask = 0x01U;
  map.bindings[0].bound = true;
  memcpy(map.bindings[0].rom, bus.devices[0].rom, DS18B20_ROM_SIZE);
  memcpy(saved_rom, bus.devices[0].rom, DS18B20_ROM_SIZE);
  bus.devices[0].present = false;

  board_a_sensor_manager_init(&manager, &port);
  CHECK(board_a_sensor_manager_set_map(&manager, &map));
  CHECK(!discover(&manager, &bus));
  CHECK(manager.map.valid_mask == 0x03U);
  CHECK(memcmp(manager.map.bindings[0].rom, saved_rom,
               DS18B20_ROM_SIZE) == 0);
  CHECK(memcmp(manager.map.bindings[1].rom, bus.devices[1].rom,
               DS18B20_ROM_SIZE) == 0);
  CHECK(read_after_conversion(&manager, &bus, &snapshot));
  CHECK(snapshot.sensors[0].quality == BOARD_A_QUALITY_NOT_PRESENT);
  CHECK(snapshot.sensors[1].quality == BOARD_A_QUALITY_OK);
  CHECK(snapshot.valid_mask == 0x0002U);
}

static void test_scan_stops_once_all_slots_are_bound(void)
{
  fake_bus_t bus = make_bus(3U);
  ds18b20_port_t port = make_port(&bus);
  board_a_sensor_manager_t manager;
  board_a_sensor_map_t map;
  board_a_sensor_snapshot_t snapshot;

  memset(&map, 0, sizeof(map));
  map.valid_mask = 0x03U;
  map.bindings[0].bound = true;
  memcpy(map.bindings[0].rom, bus.devices[0].rom, DS18B20_ROM_SIZE);
  map.bindings[1].bound = true;
  memcpy(map.bindings[1].rom, bus.devices[1].rom, DS18B20_ROM_SIZE);

  board_a_sensor_manager_init(&manager, &port);
  CHECK(board_a_sensor_manager_set_map(&manager, &map));
  CHECK(!discover(&manager, &bus));
  CHECK(manager.map.valid_mask == 0x07U);
  /*
   * Enumerating three devices needs three search passes plus a terminating
   * pass. The scan must stop the moment the last slot is filled, so it may not
   * walk the whole tree (that is what kept the acquisition task busy forever
   * on the bench when the bus did not terminate the search by itself).
   */
  CHECK(bus.search_passes <= 3U);
  CHECK(read_after_conversion(&manager, &bus, &snapshot));
  CHECK(snapshot.valid_mask == 0x0007U);
}

static void test_slow_bus_is_bounded_by_time_budget(void)
{
  fake_bus_t bus = make_bus(1U);
  ds18b20_port_t port = make_port(&bus);
  board_a_sensor_manager_t manager;

  /*
   * Model a bus where every level read costs an extra millisecond: one search
   * pass already blows the 50 ms discovery budget, so the scan must stop after
   * that pass instead of walking the tree again.
   */
  bus.slow_read_us = 1000U;

  board_a_sensor_manager_init(&manager, &port);
  CHECK(!discover(&manager, &bus));
  CHECK(bus.search_passes == 1U);
  CHECK(manager.map.valid_mask == 0x01U);
  CHECK(!manager.discovery_complete);
  CHECK(manager.next_discovery_us > bus.now_us);
}

int main(void)
{
  test_auto_discovery_and_three_temperatures();
  test_missing_stale_and_recovery();
  test_bound_map_survives_enumeration_order_change();
  test_partial_map_fills_free_slots_without_renumbering();
  test_full_map_is_not_rescanned();
  test_missing_bound_rom_is_not_replaced();
  test_scan_stops_once_all_slots_are_bound();
  test_slow_bus_is_bounded_by_time_budget();
  printf("DS18B20 manager host tests: %u checks, %u failures\n",
         g_checks, g_failures);
  return g_failures == 0U ? 0 : 1;
}
