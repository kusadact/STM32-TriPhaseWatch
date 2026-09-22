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

static void test_unbound_entries_are_not_present(void)
{
  fake_bus_t bus = make_bus(3U);
  ds18b20_port_t port = make_port(&bus);
  board_a_sensor_manager_t manager;
  board_a_sensor_map_t map;
  board_a_sensor_snapshot_t snapshot;

  memset(&map, 0, sizeof(map));
  map.valid_mask = 0x01U;
  map.bindings[0].bound = true;
  memcpy(map.bindings[0].rom, bus.devices[0].rom, DS18B20_ROM_SIZE);
  map.bindings[0].rom_short = 0x1234U;
  board_a_sensor_manager_init(&manager, &port);
  CHECK(board_a_sensor_manager_set_map(&manager, &map));
  CHECK(!discover(&manager, &bus));
  CHECK(read_after_conversion(&manager, &bus, &snapshot));
  CHECK(snapshot.sensors[0].quality == BOARD_A_QUALITY_OK);
  CHECK(snapshot.sensors[1].quality == BOARD_A_QUALITY_NOT_PRESENT);
  CHECK(snapshot.sensors[2].quality == BOARD_A_QUALITY_NOT_PRESENT);
  CHECK(snapshot.valid_mask == 0x0001U);
}

int main(void)
{
  test_auto_discovery_and_three_temperatures();
  test_missing_stale_and_recovery();
  test_bound_map_survives_enumeration_order_change();
  test_unbound_entries_are_not_present();
  printf("DS18B20 manager host tests: %u checks, %u failures\n",
         g_checks, g_failures);
  return g_failures == 0U ? 0 : 1;
}
