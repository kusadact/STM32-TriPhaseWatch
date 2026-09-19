#include "delay.h"
#include "stm32f4xx.h"

/*
 * SysTick-based blocking delay.
 *
 * SysTick runs from the processor clock (168 MHz) as a free-running 24-bit
 * down counter with interrupts disabled. delay_us() accounts for counter
 * wrap-around, so the same code works on real hardware and in Renode.
 *
 * Renode's STM32F4 model does not implement the DWT cycle counter, which is
 * why this deliberately avoids DWT.
 */

void delay_init(u8 SYSCLK)
{
  (void)SYSCLK;

  SysTick->CTRL = 0U;
  SysTick->LOAD = 0x00FFFFFFU;
  SysTick->VAL = 0U;
  SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
}

void delay_us(u32 nus)
{
  uint32_t load = SysTick->LOAD;
  uint32_t last = SysTick->VAL;
  uint32_t elapsed = 0U;
  uint32_t ticks = nus * (SystemCoreClock / 1000000U);

  while (elapsed < ticks) {
    uint32_t now = SysTick->VAL;

    if (now <= last) {
      elapsed += last - now;
    } else {
      elapsed += last + (load - now);
    }
    last = now;
  }
}

void delay_ms(u16 nms)
{
  while (nms-- != 0U) {
    delay_us(1000U);
  }
}
