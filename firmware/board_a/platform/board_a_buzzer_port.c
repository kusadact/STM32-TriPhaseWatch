#include "board_a_buzzer_port.h"

#include "stm32f4xx.h"

void board_a_buzzer_port_init(void)
{
  GPIO_InitTypeDef gpio;

  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOF, ENABLE);

  GPIO_ResetBits(GPIOF, GPIO_Pin_8);
  gpio.GPIO_Pin = GPIO_Pin_8;
  gpio.GPIO_Mode = GPIO_Mode_OUT;
  gpio.GPIO_Speed = GPIO_Speed_100MHz;
  gpio.GPIO_OType = GPIO_OType_PP;
  gpio.GPIO_PuPd = GPIO_PuPd_UP;
  GPIO_Init(GPIOF, &gpio);

  GPIO_ResetBits(GPIOF, GPIO_Pin_8);
}

void board_a_buzzer_port_set(bool on)
{
  if (on) {
    GPIO_SetBits(GPIOF, GPIO_Pin_8);
  } else {
    GPIO_ResetBits(GPIOF, GPIO_Pin_8);
  }
}
