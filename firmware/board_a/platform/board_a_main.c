#include <stdint.h>

#include "board_a_log_schedule.h"
#include "board_a_slave.h"
#include "config.h"
#include "debug_uart.h"
#include "stm32f4xx.h"

#define BOARD_A_RS485_BAUD 9600U

static board_a_slave_t g_slave;
static uint32_t g_monotonic_last_us;
static uint64_t g_monotonic_us;

static uint32_t timer_raw_us(void)
{
  return TIM2->CNT;
}

static uint64_t monotonic_now_us(void)
{
  uint32_t now = timer_raw_us();
  uint32_t elapsed = (uint32_t)(now - g_monotonic_last_us);

  g_monotonic_us += elapsed;
  g_monotonic_last_us = now;
  return g_monotonic_us;
}

static void debug_write_u32(uint32_t value)
{
  char buffer[11];
  uint8_t index = 0U;

  if (value == 0U) {
    debug_uart_putc('0');
    return;
  }

  while (value != 0U) {
    buffer[index] = (char)('0' + (value % 10U));
    value /= 10U;
    index++;
  }
  while (index != 0U) {
    index--;
    debug_uart_putc(buffer[index]);
  }
}

static void timer_init(void)
{
  RCC_ClocksTypeDef clocks;
  TIM_TimeBaseInitTypeDef timer;
  uint32_t timer_clock;
  uint16_t prescaler;

  RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
  RCC_GetClocksFreq(&clocks);

  timer_clock = clocks.PCLK1_Frequency;
  if ((RCC->CFGR & RCC_CFGR_PPRE1) != RCC_CFGR_PPRE1_DIV1) {
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

  g_monotonic_last_us = 0U;
  g_monotonic_us = 0U;
}

static void rs485_init(void)
{
  GPIO_InitTypeDef gpio;
  USART_InitTypeDef usart;
  NVIC_InitTypeDef nvic;

  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA |
                         RCC_AHB1Periph_GPIOG, ENABLE);
  RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);

  GPIO_PinAFConfig(GPIOA, GPIO_PinSource2, GPIO_AF_USART2);
  GPIO_PinAFConfig(GPIOA, GPIO_PinSource3, GPIO_AF_USART2);
  gpio.GPIO_Pin = GPIO_Pin_2 | GPIO_Pin_3;
  gpio.GPIO_Mode = GPIO_Mode_AF;
  gpio.GPIO_Speed = GPIO_Speed_50MHz;
  gpio.GPIO_OType = GPIO_OType_PP;
  gpio.GPIO_PuPd = GPIO_PuPd_UP;
  GPIO_Init(GPIOA, &gpio);

  gpio.GPIO_Pin = GPIO_Pin_8;
  gpio.GPIO_Mode = GPIO_Mode_OUT;
  gpio.GPIO_Speed = GPIO_Speed_50MHz;
  gpio.GPIO_OType = GPIO_OType_PP;
  gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;
  GPIO_Init(GPIOG, &gpio);
  GPIO_ResetBits(GPIOG, GPIO_Pin_8);

  usart.USART_BaudRate = BOARD_A_RS485_BAUD;
  /*
   * STM32F4 parity occupies the ninth transmitted bit. The 8 data bits plus
   * even parity therefore require WordLength_9b in the peripheral config.
   */
  usart.USART_WordLength = USART_WordLength_9b;
  usart.USART_StopBits = USART_StopBits_1;
  usart.USART_Parity = USART_Parity_Even;
  usart.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
  usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
  USART_Init(USART2, &usart);

  USART_ClearFlag(USART2, USART_FLAG_TC);
  USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);

  NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
  nvic.NVIC_IRQChannel = USART2_IRQn;
  nvic.NVIC_IRQChannelPreemptionPriority = 2U;
  nvic.NVIC_IRQChannelSubPriority = 0U;
  nvic.NVIC_IRQChannelCmd = ENABLE;
  NVIC_Init(&nvic);

  USART_Cmd(USART2, ENABLE);
}

static void rs485_send(const uint8_t *data, uint16_t length)
{
  uint16_t index;

  GPIO_SetBits(GPIOG, GPIO_Pin_8);
  USART_ClearFlag(USART2, USART_FLAG_TC);
  for (index = 0U; index < length; ++index) {
    while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET) {
    }
    /*
     * With M=1 and PCE=1, bit 8 of the data written to USART_DR is replaced
     * by hardware parity, so the lower 8 bits remain the Modbus byte.
     */
    USART_SendData(USART2, data[index]);
  }
  while (USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET) {
  }
  GPIO_ResetBits(GPIOG, GPIO_Pin_8);
}

void USART2_IRQHandler(void)
{
  uint8_t byte;

  if (USART_GetITStatus(USART2, USART_IT_RXNE) != RESET) {
    byte = (uint8_t)USART_ReceiveData(USART2);
    board_a_slave_push_byte(&g_slave, byte, timer_raw_us());
  }
}

int main(void)
{
  uint8_t response[MODBUS_RTU_MAX_ADU_SIZE];
  board_a_log_schedule_t log_schedule;
  uint64_t now_us;

  timer_init();
  board_a_slave_init(&g_slave, 1U);
  debug_uart_init(DEBUG_UART_BAUD);
  rs485_init();

  debug_uart_puts("\r\n[board-a] Modbus RTU slave\r\n");
  debug_uart_puts("[board-a] addr=1 uart=USART2 PA2/PA3 PG8 9600 8E1\r\n");
  debug_uart_puts("[board-a] debug=USART1 PA9/PA10 115200 8N1\r\n");

  board_a_log_schedule_init(&log_schedule, 5000000U, monotonic_now_us());

  while (1) {
    size_t response_length;

    now_us = monotonic_now_us();
    board_a_slave_tick(&g_slave, now_us);

    /*
     * Re-read the timer before polling so a byte that arrived while the loop
     * was running is compared against a timestamp at least as new as the
     * ISR's own timestamp.
     */
    now_us = monotonic_now_us();
    response_length = board_a_slave_poll(
        &g_slave, (uint32_t)now_us, response, sizeof(response));
    if (response_length != 0U) {
      rs485_send(response, (uint16_t)response_length);
    }

    if (board_a_log_schedule_due(&log_schedule, now_us)) {
      debug_uart_puts("[board-a] run=");
      debug_write_u32((uint32_t)g_slave.model.run_state);
      debug_uart_puts(" records=");
      debug_write_u32(g_slave.model.snapshot.sequence);
      debug_uart_puts(" rx=");
      debug_write_u32(g_slave.model.stats.rx_frames);
      debug_uart_puts(" crc=");
      debug_write_u32(g_slave.model.stats.crc_errors);
      debug_uart_puts("\r\n");
    }
  }
}
