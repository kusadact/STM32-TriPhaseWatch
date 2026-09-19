#include "fake_at24c02_port.h"

#include "at24c02_port.h"

static uint32_t delay_ms_total;
static uint32_t yield_count;

void fake_at24c02_port_reset(void)
{
  delay_ms_total = 0U;
  yield_count = 0U;
}

uint32_t fake_at24c02_port_delay_ms_total(void)
{
  return delay_ms_total;
}

uint32_t fake_at24c02_port_yield_count(void)
{
  return yield_count;
}

void at24c02_port_delay_ms(uint16_t milliseconds)
{
  delay_ms_total += milliseconds;
}

void at24c02_port_yield(void)
{
  yield_count++;
}
