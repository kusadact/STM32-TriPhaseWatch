#include "FreeRTOS.h"
#include "delay.h"
#include "stm32f4xx.h"
#include "task.h"

/*
 * Board A owns TIM2 as its 1 MHz monotonic source. delay_init() intentionally
 * does not touch SysTick because the FreeRTOS port owns SysTick after startup.
 */
void delay_init(u8 sysclk)
{
  (void)sysclk;
}

void delay_us(u32 nus)
{
  uint32_t start;
  uint32_t elapsed;

  if (nus == 0U) {
    return;
  }

  start = TIM2->CNT;
  do {
    elapsed = (uint32_t)(TIM2->CNT - start);
  } while (elapsed < (uint32_t)nus);
}

void delay_ms(u16 nms)
{
  if (nms == 0U) {
    return;
  }

  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
    vTaskDelay(pdMS_TO_TICKS(nms));
    return;
  }

  while (nms-- != 0U) {
    delay_us(1000U);
  }
}
