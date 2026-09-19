#include "board_b_io.h"

#include "bridge_config.h"
#include "stm32f4xx.h"

#if (BOARD_B_LINK_DATA_BITS != 8U) || (BOARD_B_LINK_PARITY_EVEN != 1U) || \
    (BOARD_B_LINK_STOP_BITS != 1U)
#error "Board B USART configuration currently implements 8E1 only"
#endif

board_b_rx_queue_t g_board_b_host_rx_queue;
board_b_rx_queue_t g_board_b_rs485_rx_queue;

volatile board_b_io_stats_t g_board_b_io_stats;

static void usart_irq_handler(USART_TypeDef *usart,
                              board_b_rx_queue_t *queue,
                              volatile uint32_t *uart_errors,
                              volatile uint32_t *overruns)
{
  uint16_t status = usart->SR;
  uint16_t received;
  const uint16_t error_flags = (uint16_t)(USART_SR_PE | USART_SR_FE |
                                          USART_SR_NE | USART_SR_ORE);

  if ((status & USART_SR_RXNE) != 0U)
  {
    received = (uint16_t)usart->DR;
    /*
     * Arrival time is sampled in the interrupt, not when the main loop drains
     * the queue: several bytes with real gaps can be dequeued in one batch.
     */
    uint32_t arrival_us = board_b_micros();

    if ((status & error_flags) != 0U)
    {
      (*uart_errors)++;
    }
    if (board_b_rx_queue_push(queue, (uint8_t)received, arrival_us) == 0)
    {
      (*overruns)++;
    }
    return;
  }

  if ((status & error_flags) != 0U)
  {
    (*uart_errors)++;
    (void)usart->DR;
  }
}

/*
 * Free-running 1 us counter for receive timestamps. The bridge only needs
 * differences between two receive events, so a 32-bit wrap at ~71.6 minutes is
 * harmless as long as consecutive bytes are closer than half a period.
 */
static void timer_init_microseconds(void)
{
  RCC_ClocksTypeDef clocks;
  TIM_TimeBaseInitTypeDef timer;
  uint32_t timer_clock;
  uint16_t prescaler;

  RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
  RCC_GetClocksFreq(&clocks);

  timer_clock = clocks.PCLK1_Frequency;
  if ((RCC->CFGR & RCC_CFGR_PPRE1) != RCC_CFGR_PPRE1_DIV1)
  {
    timer_clock *= 2U;
  }

  prescaler = (uint16_t)((timer_clock / 1000000U) - 1U);
  TIM_TimeBaseStructInit(&timer);
  timer.TIM_Prescaler = prescaler;
  timer.TIM_CounterMode = TIM_CounterMode_Up;
  timer.TIM_Period = 0xFFFFFFFFU;
  timer.TIM_ClockDivision = TIM_CKD_DIV1;
  timer.TIM_RepetitionCounter = 0U;
  TIM_TimeBaseInit(TIM2, &timer);
  TIM_SetCounter(TIM2, 0U);
  TIM_Cmd(TIM2, ENABLE);
}

uint32_t board_b_micros(void)
{
  return TIM2->CNT;
}

static void usart_send(USART_TypeDef *usart, const uint8_t *data, uint16_t length)
{
  uint16_t index;

  for (index = 0U; index < length; index++)
  {
    while (USART_GetFlagStatus(usart, USART_FLAG_TXE) == RESET)
    {
    }
    USART_SendData(usart, data[index]);
  }

  while (USART_GetFlagStatus(usart, USART_FLAG_TC) == RESET)
  {
  }
}

static void usart_init_8e1(USART_TypeDef *usart)
{
  USART_InitTypeDef usart_config;

  usart_config.USART_BaudRate = BOARD_B_LINK_BAUD_RATE;
  /*
   * STM32 standard peripheral library terminology: with parity enabled, M=1
   * selects eight data bits plus one parity bit, which is the required 8E1.
   */
  usart_config.USART_WordLength = USART_WordLength_9b;
  usart_config.USART_StopBits = USART_StopBits_1;
  usart_config.USART_Parity = USART_Parity_Even;
  usart_config.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
  usart_config.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;

  USART_Init(usart, &usart_config);
  USART_Cmd(usart, ENABLE);
  USART_ITConfig(usart, USART_IT_RXNE, ENABLE);
}

void board_b_io_init(void)
{
  GPIO_InitTypeDef gpio_config;
  volatile uint32_t discard;

  board_b_rx_queue_init(&g_board_b_host_rx_queue);
  board_b_rx_queue_init(&g_board_b_rs485_rx_queue);
  g_board_b_io_stats.host_rx_overruns = 0U;
  g_board_b_io_stats.rs485_rx_overruns = 0U;
  g_board_b_io_stats.host_uart_errors = 0U;
  g_board_b_io_stats.rs485_uart_errors = 0U;

  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOG, ENABLE);
  RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);

  GPIO_PinAFConfig(GPIOA, GPIO_PinSource2, GPIO_AF_USART2);
  GPIO_PinAFConfig(GPIOA, GPIO_PinSource3, GPIO_AF_USART2);
  GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF_USART1);
  GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_USART1);

  gpio_config.GPIO_Mode = GPIO_Mode_AF;
  gpio_config.GPIO_Speed = GPIO_Speed_50MHz;
  gpio_config.GPIO_OType = GPIO_OType_PP;
  gpio_config.GPIO_PuPd = GPIO_PuPd_UP;

  gpio_config.GPIO_Pin = GPIO_Pin_2 | GPIO_Pin_3;
  GPIO_Init(GPIOA, &gpio_config);

  gpio_config.GPIO_Pin = GPIO_Pin_9 | GPIO_Pin_10;
  GPIO_Init(GPIOA, &gpio_config);

  gpio_config.GPIO_Pin = GPIO_Pin_8;
  gpio_config.GPIO_Mode = GPIO_Mode_OUT;
  GPIO_Init(GPIOG, &gpio_config);
  GPIO_ResetBits(GPIOG, GPIO_Pin_8);

  usart_init_8e1(USART1);
  usart_init_8e1(USART2);

  /*
   * Clear stale receive/error state left by reset or a previous debug session.
   */
  discard = USART1->SR;
  discard = USART1->DR;
  discard = USART2->SR;
  discard = USART2->DR;
  (void)discard;

  NVIC_SetPriority(USART1_IRQn, 2U);
  NVIC_SetPriority(USART2_IRQn, 2U);
  NVIC_EnableIRQ(USART1_IRQn);
  NVIC_EnableIRQ(USART2_IRQn);

  timer_init_microseconds();
}

void board_b_host_send(const uint8_t *data, uint16_t length)
{
  usart_send(USART1, data, length);
}

void board_b_rs485_send(const uint8_t *data, uint16_t length)
{
  GPIO_SetBits(GPIOG, GPIO_Pin_8);
  usart_send(USART2, data, length);
  GPIO_ResetBits(GPIOG, GPIO_Pin_8);
}

void USART1_IRQHandler(void)
{
  usart_irq_handler(USART1, &g_board_b_host_rx_queue,
                    &g_board_b_io_stats.host_uart_errors,
                    &g_board_b_io_stats.host_rx_overruns);
}

void USART2_IRQHandler(void)
{
  usart_irq_handler(USART2, &g_board_b_rs485_rx_queue,
                    &g_board_b_io_stats.rs485_uart_errors,
                    &g_board_b_io_stats.rs485_rx_overruns);
}
