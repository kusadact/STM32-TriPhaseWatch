#include "at24c02_port.h"

#include "FreeRTOS.h"
#include "delay.h"
#include "task.h"

void at24c02_port_delay_ms(uint16_t milliseconds)
{
  delay_ms(milliseconds);
}

void at24c02_port_yield(void)
{
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
    taskYIELD();
  }
}
