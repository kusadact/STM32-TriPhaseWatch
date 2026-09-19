#include <stdio.h>

#include "config.h"
#include "debug_uart.h"
#include "delay.h"
#include "led.h"
#include "stm32f4xx.h"

int main(void)
{
  uint32_t tick = 0U;

  delay_init(168U);
  debug_uart_init(DEBUG_UART_BAUD);
  LED_Init();

  printf("\r\n");
  printf("[water-monitor] blank firmware, build %s %s\r\n",
         __DATE__, __TIME__);
  printf("[water-monitor] SYSCLK=%lu Hz, HSE=%lu Hz\r\n",
         (unsigned long)SystemCoreClock, (unsigned long)HSE_VALUE);
  printf("[water-monitor] P0 baseline: RS485 bridge and Modbus not implemented\r\n");

  while (1) {
    LED0 = 0U; /* DS0 red LED on (active low). */
    printf("[water-monitor] alive %lu\r\n", (unsigned long)tick);
    delay_ms(500U);
    LED0 = 1U;
    delay_ms(500U);
    tick++;
  }
}
