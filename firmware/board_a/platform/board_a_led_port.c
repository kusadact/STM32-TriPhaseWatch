#include "board_a_led_port.h"

#include "stm32f4xx.h"

void board_a_led_port_init(void)
{
  GPIO_InitTypeDef gpio;

  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOF, ENABLE);

  GPIO_SetBits(GPIOF, GPIO_Pin_9 | GPIO_Pin_10);
  gpio.GPIO_Pin = GPIO_Pin_9 | GPIO_Pin_10;
  gpio.GPIO_Mode = GPIO_Mode_OUT;
  gpio.GPIO_Speed = GPIO_Speed_100MHz;
  gpio.GPIO_OType = GPIO_OType_PP;
  gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;
  GPIO_Init(GPIOF, &gpio);

  GPIO_SetBits(GPIOF, GPIO_Pin_9 | GPIO_Pin_10);
}

void board_a_led_port_set(bool led0_on, bool led1_on)
{
  if (led0_on) {
    GPIO_ResetBits(GPIOF, GPIO_Pin_9);
  } else {
    GPIO_SetBits(GPIOF, GPIO_Pin_9);
  }

  if (led1_on) {
    GPIO_ResetBits(GPIOF, GPIO_Pin_10);
  } else {
    GPIO_SetBits(GPIOF, GPIO_Pin_10);
  }
}
