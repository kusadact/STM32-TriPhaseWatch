#ifndef BOARD_A_DHT11_H
#define BOARD_A_DHT11_H

#include <stdbool.h>
#include <stdint.h>

#define DHT11_MIN_INTERVAL_US 1000000ULL
#define DHT11_START_LOW_US 18000U
#define DHT11_TIMEOUT_US 1000U
#define DHT11_BIT_TIMEOUT_US 200U

typedef enum {
  DHT11_STATUS_OK = 0,
  DHT11_STATUS_TIMEOUT_RESPONSE = 1,
  DHT11_STATUS_TIMEOUT_BIT = 2,
  DHT11_STATUS_CHECKSUM_ERROR = 3,
  DHT11_STATUS_RANGE_ERROR = 4,
  DHT11_STATUS_TOO_SOON = 5,
  DHT11_STATUS_INVALID_ARGUMENT = 6
} dht11_status_t;

/*
 * The port owns the electrical state and monotonic clock. Bit timing calls
 * begin_read/end_read around the response and data phase only, allowing the
 * caller to keep the 18 ms start pulse schedulable.
 */
typedef struct {
  void *context;
  uint64_t (*now_us)(void *context);
  void (*delay_us)(void *context, uint32_t delay_us);
  void (*drive_low)(void *context, uint8_t sensor_id);
  void (*release_input)(void *context, uint8_t sensor_id);
  bool (*read_level)(void *context, uint8_t sensor_id);
  void (*begin_read)(void *context, uint8_t sensor_id);
  void (*end_read)(void *context, uint8_t sensor_id);
} dht11_port_t;

typedef struct {
  const dht11_port_t *port;
  uint8_t sensor_id;
  uint64_t last_start_us;
  bool has_started;
} dht11_t;

typedef struct {
  uint16_t temperature_x10;
  uint16_t humidity_x10;
  uint32_t sample_time_ms;
} dht11_sample_t;

void dht11_init(dht11_t *device, const dht11_port_t *port,
                uint8_t sensor_id);

dht11_status_t dht11_read(dht11_t *device, dht11_sample_t *sample);

const char *dht11_status_name(dht11_status_t status);

#endif /* BOARD_A_DHT11_H */
