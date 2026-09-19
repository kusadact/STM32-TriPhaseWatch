#include "debug_uart.h"
#include "stm32f4xx.h"

void debug_uart_init(uint32_t baudrate)
{
  GPIO_InitTypeDef gpio;
  USART_InitTypeDef usart;

  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);

  GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF_USART1);
  GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_USART1);

  gpio.GPIO_Pin = GPIO_Pin_9 | GPIO_Pin_10;
  gpio.GPIO_Mode = GPIO_Mode_AF;
  gpio.GPIO_Speed = GPIO_Speed_50MHz;
  gpio.GPIO_OType = GPIO_OType_PP;
  gpio.GPIO_PuPd = GPIO_PuPd_UP;
  GPIO_Init(GPIOA, &gpio);

  usart.USART_BaudRate = baudrate;
  usart.USART_WordLength = USART_WordLength_8b;
  usart.USART_StopBits = USART_StopBits_1;
  usart.USART_Parity = USART_Parity_No;
  usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
  usart.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
  USART_Init(USART1, &usart);
  USART_Cmd(USART1, ENABLE);
}

void debug_uart_putc(char c)
{
  while ((USART1->SR & USART_FLAG_TXE) == 0U) {
  }
  USART1->DR = (uint16_t)(uint8_t)c;
}

void debug_uart_puts(const char *text)
{
  while (*text != '\0') {
    debug_uart_putc(*text++);
  }
}
