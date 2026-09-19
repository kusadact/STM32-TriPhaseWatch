#include "at24c02_port.h"

#include "delay.h"

void at24c02_port_delay_ms(uint16_t milliseconds)
{
  delay_ms(milliseconds);
}

void at24c02_port_yield(void)
{
  /* A FreeRTOS port can replace this with taskYIELD(). */
}
