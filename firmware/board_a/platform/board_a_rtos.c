#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "../alarm/board_a_alarm.h"
#include "../alarm/board_a_alarm_output.h"
#include "../sensors/sensor_manager.h"
#include "board_a_event_buffer.h"
#include "board_a_log_schedule.h"
#include "board_a_monotonic.h"
#include "board_a_persistence_tasks.h"
#include "board_a_rx_recovery.h"
#include "board_a_runtime.h"
#include "board_a_tx.h"
#include "config.h"
#include "debug_uart.h"
#include "delay.h"
#include "queue.h"
#include "semphr.h"
#include "soft_i2c.h"
#include "stm32f4xx.h"
#include "task.h"

#define BOARD_A_RS485_BAUD 9600U
#define BOARD_A_T35_US 4011U
#define BOARD_A_RX_QUEUE_CAPACITY 512U
#define BOARD_A_COMM_TASK_PRIORITY 3U
#define BOARD_A_ACQUISITION_TASK_PRIORITY 4U
#define BOARD_A_ALARM_TASK_PRIORITY 2U
#define BOARD_A_COMM_STACK_WORDS 1024U
#define BOARD_A_ACQUISITION_STACK_WORDS 512U
#define BOARD_A_ALARM_STACK_WORDS 256U
#define BOARD_A_CONFIG_STACK_WORDS 512U
#define BOARD_A_STORAGE_STACK_WORDS 1024U
#define BOARD_A_IDLE_STACK_WORDS 128U
#define BOARD_A_LOG_PERIOD_US 5000000ULL
#define BOARD_A_MAX_WAIT_MS 1000U
#define BOARD_A_ALARM_WAIT_MS 10U
#define BOARD_A_MONOTONIC_MAX_GAP_US 60000000U

#define BOARD_A_USART2_PREEMPTION_PRIORITY 5U

_Static_assert(configPRIO_BITS == 4U, "board A expects four NVIC priority bits");
_Static_assert(configTICK_RATE_HZ == 1000U, "board A expects a 1 kHz tick");
_Static_assert(BOARD_A_ACQUISITION_TASK_PRIORITY < configMAX_PRIORITIES,
               "DS18B20 acquisition priority must be valid");
_Static_assert(BOARD_A_USART2_PREEMPTION_PRIORITY ==
                   configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY,
               "USART2 priority must be at or below the RTOS syscall limit");

typedef struct {
  uint8_t byte;
  uint32_t timestamp_us;
  uint32_t flags;
} board_a_rx_event_t;

typedef struct {
  volatile uint32_t task_ready_mask;
  volatile uint32_t comm_stack_min_words;
  volatile uint32_t acquisition_stack_min_words;
  volatile uint32_t alarm_stack_min_words;
  volatile uint32_t config_stack_min_words;
  volatile uint32_t storage_stack_min_words;
  volatile uint32_t idle_stack_min_words;
  volatile uint32_t rx_queue_depth_max;
  volatile uint32_t rx_queue_drops;
  volatile uint32_t rx_continuity_errors;
  volatile uint32_t uart_error_flags;
  volatile uint32_t tx_timeouts;
  volatile uint32_t tx_completions;
  volatile uint32_t model_lock_max_us;
  volatile uint32_t schedule_late_max_us;
  volatile uint32_t schedule_start_late_max_us;
  volatile uint32_t schedule_start_count;
  volatile uint32_t monotonic_wrap_count;
  volatile uint32_t fault_flags;
  volatile uint32_t rx_queue_item_size;
} board_a_rtos_diag_t;

enum {
  BOARD_A_FAULT_ASSERT = 1U << 0,
  BOARD_A_FAULT_STACK_OVERFLOW = 1U << 1,
  BOARD_A_FAULT_TASK_CREATE = 1U << 2,
  BOARD_A_FAULT_SCHEDULER_RETURNED = 1U << 3,
  BOARD_A_FAULT_HARD_FAULT = 1U << 4,
  BOARD_A_FAULT_MEM_MANAGE = 1U << 5,
  BOARD_A_FAULT_BUS_FAULT = 1U << 6,
  BOARD_A_FAULT_USAGE_FAULT = 1U << 7,
  BOARD_A_FAULT_MONOTONIC_GAP = 1U << 8
};

volatile board_a_rtos_diag_t g_board_a_rtos_diag __attribute__((used));

static board_a_runtime_t g_runtime;
static board_a_monotonic_t g_monotonic;
static board_a_tx_t g_tx;
static board_a_rx_recovery_t g_rx_recovery;
static board_a_log_schedule_t g_log_schedule;
static board_a_sensor_manager_t g_sensor_manager;
static board_a_alarm_t g_alarm;
static board_a_alarm_config_t g_applied_alarm_config;
static board_a_event_buffer_t g_event_buffer;
static bool g_alarm_config_applied;
static uint32_t g_event_marker_command_id;

static QueueHandle_t g_rx_queue;
static SemaphoreHandle_t g_model_mutex;
static TaskHandle_t g_comm_task;
static TaskHandle_t g_acquisition_task;
static TaskHandle_t g_alarm_task;
static TaskHandle_t g_config_task;
static TaskHandle_t g_storage_task;

static StaticQueue_t g_rx_queue_buffer;
static uint32_t g_rx_queue_storage[
    (BOARD_A_RX_QUEUE_CAPACITY * sizeof(board_a_rx_event_t) +
     sizeof(uint32_t) - 1U) /
    sizeof(uint32_t)] __attribute__((aligned(8)));

_Static_assert(sizeof(board_a_rx_event_t) == 12U,
               "board A RX event layout changed");
_Static_assert(sizeof(g_rx_queue_storage) >=
                   (BOARD_A_RX_QUEUE_CAPACITY * sizeof(board_a_rx_event_t)),
               "board A RX queue storage is undersized");
static StaticSemaphore_t g_model_mutex_buffer;
static StaticTask_t g_comm_task_buffer;
static StaticTask_t g_acquisition_task_buffer;
static StaticTask_t g_alarm_task_buffer;
static StaticTask_t g_config_task_buffer;
static StaticTask_t g_storage_task_buffer;
static StaticTask_t g_idle_task_buffer;
static StackType_t g_comm_stack[BOARD_A_COMM_STACK_WORDS]
    __attribute__((aligned(8)));
static StackType_t g_acquisition_stack[BOARD_A_ACQUISITION_STACK_WORDS]
    __attribute__((aligned(8)));
static StackType_t g_alarm_stack[BOARD_A_ALARM_STACK_WORDS]
    __attribute__((aligned(8)));
static StackType_t g_config_stack[BOARD_A_CONFIG_STACK_WORDS]
    __attribute__((aligned(8)));
static StackType_t g_storage_stack[BOARD_A_STORAGE_STACK_WORDS]
    __attribute__((aligned(8)));
static StackType_t g_idle_stack[BOARD_A_IDLE_STACK_WORDS]
    __attribute__((aligned(8)));

static uint8_t g_response[MODBUS_RTU_MAX_ADU_SIZE];
static volatile uint32_t g_rx_fault_epoch;
static volatile uint32_t g_rx_fault_us;
static uint32_t g_rx_fault_epoch_seen;
static uint32_t g_model_lock_started_us;

void vPortSVCHandler(void);
void xPortPendSVHandler(void);
void xPortSysTickHandler(void);

void SVC_Handler(void)
{
  vPortSVCHandler();
}

void PendSV_Handler(void)
{
  xPortPendSVHandler();
}

void SysTick_Handler(void)
{
  xPortSysTickHandler();
}

static void board_a_rtos_fatal(uint32_t fault)
{
  g_board_a_rtos_diag.fault_flags |= fault;
  board_a_alarm_output_force_off();
  __disable_irq();
  for (;;) {
  }
}

void board_a_rtos_assert_failed(const char *file, int line)
{
  (void)file;
  (void)line;
  board_a_rtos_fatal(BOARD_A_FAULT_ASSERT);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
  (void)task;
  (void)task_name;
  board_a_rtos_fatal(BOARD_A_FAULT_STACK_OVERFLOW);
}

void vApplicationIdleHook(void)
{
  g_board_a_rtos_diag.idle_stack_min_words =
      uxTaskGetStackHighWaterMark(NULL);
}

void vApplicationGetIdleTaskMemory(StaticTask_t **task_buffer,
                                   StackType_t **stack_buffer,
                                   uint32_t *stack_size)
{
  *task_buffer = &g_idle_task_buffer;
  *stack_buffer = g_idle_stack;
  *stack_size = BOARD_A_IDLE_STACK_WORDS;
}

void HardFault_Handler(void)
{
  board_a_rtos_fatal(BOARD_A_FAULT_HARD_FAULT);
}

void MemManage_Handler(void)
{
  board_a_rtos_fatal(BOARD_A_FAULT_MEM_MANAGE);
}

void BusFault_Handler(void)
{
  board_a_rtos_fatal(BOARD_A_FAULT_BUS_FAULT);
}

void UsageFault_Handler(void)
{
  board_a_rtos_fatal(BOARD_A_FAULT_USAGE_FAULT);
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

  board_a_monotonic_init(&g_monotonic, TIM2->CNT);
}

static uint64_t board_a_rtos_now_us(void *context)
{
  uint32_t raw_us;
  uint32_t previous_raw_us;
  uint64_t now_us;

  (void)context;
  taskENTER_CRITICAL();
  raw_us = TIM2->CNT;
  previous_raw_us = g_monotonic.last_raw_us;
  if (g_monotonic.initialized &&
      ((uint32_t)(raw_us - previous_raw_us) >
       BOARD_A_MONOTONIC_MAX_GAP_US)) {
    g_board_a_rtos_diag.fault_flags |= BOARD_A_FAULT_MONOTONIC_GAP;
  }
  now_us = board_a_monotonic_update(&g_monotonic, raw_us);
  if (g_monotonic.initialized && (raw_us < previous_raw_us)) {
    g_board_a_rtos_diag.monotonic_wrap_count++;
  }
  taskEXIT_CRITICAL();
  return now_us;
}

uint32_t board_a_rtos_now_ms(void)
{
  (void)board_a_rtos_now_us(NULL);
  return board_a_monotonic_ms(&g_monotonic);
}

static size_t event_drain_to_persistence(void)
{
  return board_a_runtime_drain_event_records(&g_runtime, &g_event_buffer);
}

static void event_finish_stop_flush(uint64_t now_us)
{
  board_a_event_buffer_status_t event_status;

  (void)board_a_event_buffer_force_close(&g_event_buffer, now_us / 1000ULL);
  (void)event_drain_to_persistence();
  board_a_event_buffer_status(&g_event_buffer, &event_status);
  if ((event_status.queued_records == 0U) && !event_status.event_open) {
    (void)board_a_runtime_complete_event_stop_flush(&g_runtime);
    if (g_storage_task != NULL) {
      xTaskNotifyGive(g_storage_task);
    }
  }
}

static void sync_event_marker(uint64_t now_us)
{
  board_a_event_buffer_status_t event_status;
  bool marker_open;
  uint32_t marker_id;
  uint64_t marker_start_us;
  uint32_t command_id;

  if (!board_a_runtime_copy_event_marker(
          &g_runtime, &marker_open, &marker_id, &marker_start_us)) {
    return;
  }
  board_a_event_buffer_status(&g_event_buffer, &event_status);
  if (event_status.event_open != marker_open) {
    if (event_status.event_open) {
      board_a_runtime_set_event_marker(
          &g_runtime, true, event_status.event_id,
          (event_status.event_start_us != 0U) ?
              event_status.event_start_us : now_us);
    } else {
      board_a_runtime_set_event_marker(&g_runtime, false, 0U, 0U);
    }
  }
  if (board_a_runtime_event_marker_dirty(&g_runtime)) {
    command_id = ++g_event_marker_command_id;
    if (command_id == 0U) {
      command_id = ++g_event_marker_command_id;
    }
    if (board_a_runtime_request_event_marker_save(
            &g_runtime, command_id) && (g_config_task != NULL)) {
      xTaskNotifyGive(g_config_task);
    }
  }
}

static void recover_incomplete_event(void)
{
  if (board_a_runtime_recover_incomplete_event(&g_runtime)) {
    sync_event_marker(board_a_rtos_now_us(NULL));
  }
}

static uint64_t sensor_port_now_us(void *context)
{
  (void)context;
  return board_a_rtos_now_us(NULL);
}

static void sensor_port_delay_us(void *context, uint32_t delay_us)
{
  uint64_t started_us;

  (void)context;
  started_us = board_a_rtos_now_us(NULL);
  while ((board_a_rtos_now_us(NULL) - started_us) < delay_us) {
    /* 1-Wire timing is microsecond-scale; keep this bounded and short. */
  }
}

static void sensor_port_drive_low(void *context)
{
  GPIO_InitTypeDef gpio;

  (void)context;
  gpio.GPIO_Pin = GPIO_Pin_9;
  gpio.GPIO_Mode = GPIO_Mode_OUT;
  gpio.GPIO_Speed = GPIO_Speed_50MHz;
  gpio.GPIO_OType = GPIO_OType_PP;
  gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;
  GPIO_Init(GPIOG, &gpio);
  GPIO_ResetBits(GPIOG, GPIO_Pin_9);
}

static void sensor_port_release_bus(void *context)
{
  GPIO_InitTypeDef gpio;

  (void)context;
  gpio.GPIO_Pin = GPIO_Pin_9;
  gpio.GPIO_Mode = GPIO_Mode_IN;
  gpio.GPIO_Speed = GPIO_Speed_50MHz;
  gpio.GPIO_OType = GPIO_OType_PP;
  gpio.GPIO_PuPd = GPIO_PuPd_UP;
  GPIO_Init(GPIOG, &gpio);
}

static bool sensor_port_read_level(void *context)
{
  (void)context;
  return GPIO_ReadInputDataBit(GPIOG, GPIO_Pin_9) != Bit_RESET;
}

static const ds18b20_port_t g_sensor_port = {
  NULL,
  sensor_port_now_us,
  sensor_port_delay_us,
  sensor_port_drive_low,
  sensor_port_release_bus,
  sensor_port_read_level
};

static void sensor_gpio_init(void)
{
  GPIO_InitTypeDef gpio;

  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOG, ENABLE);
  gpio.GPIO_Mode = GPIO_Mode_IN;
  gpio.GPIO_Speed = GPIO_Speed_50MHz;
  gpio.GPIO_OType = GPIO_OType_PP;
  gpio.GPIO_PuPd = GPIO_PuPd_UP;
  gpio.GPIO_Pin = GPIO_Pin_9;
  GPIO_Init(GPIOG, &gpio);
}

void board_a_rtos_note_config_stack(uint32_t min_words)
{
  g_board_a_rtos_diag.config_stack_min_words = min_words;
}

void board_a_rtos_note_storage_stack(uint32_t min_words)
{
  g_board_a_rtos_diag.storage_stack_min_words = min_words;
}

static bool model_lock(void *context)
{
  (void)context;
  if (xSemaphoreTake(g_model_mutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  g_model_lock_started_us = TIM2->CNT;
  return true;
}

static void model_unlock(void *context)
{
  uint32_t elapsed_us = (uint32_t)(TIM2->CNT - g_model_lock_started_us);

  (void)context;
  if (elapsed_us > g_board_a_rtos_diag.model_lock_max_us) {
    g_board_a_rtos_diag.model_lock_max_us = elapsed_us;
  }
  xSemaphoreGive(g_model_mutex);
}

static const board_a_runtime_ops_t g_runtime_ops = {
  model_lock,
  model_unlock,
  board_a_rtos_now_us
};

/*
 * Board A uses TIM2 for microsecond delays, not SysTick. The production
 * soft-I2C init runs before the scheduler, so report readiness from the
 * actual delay backend instead of the legacy SysTick check.
 */
bool soft_i2c_platform_ready(void)
{
  return ((RCC->APB1ENR & RCC_APB1ENR_TIM2EN) != 0U) &&
         ((TIM2->CR1 & TIM_CR1_CEN) != 0U);
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

  USART_ITConfig(USART2, USART_IT_RXNE, DISABLE);
  USART_ITConfig(USART2, USART_IT_ERR, DISABLE);
  USART_ITConfig(USART2, USART_IT_PE, DISABLE);
  USART_ITConfig(USART2, USART_IT_TXE, DISABLE);
  USART_ITConfig(USART2, USART_IT_TC, DISABLE);
  USART_ClearFlag(USART2, USART_FLAG_TC);

  /*
   * All four implemented priority bits are preemption bits. This gives a
   * single, explicit mapping for the FreeRTOS BASEPRI threshold and keeps
   * USART2 at or below configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY.
   */
  NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
  nvic.NVIC_IRQChannel = USART2_IRQn;
  nvic.NVIC_IRQChannelPreemptionPriority =
      BOARD_A_USART2_PREEMPTION_PRIORITY;
  nvic.NVIC_IRQChannelSubPriority = 0U;
  nvic.NVIC_IRQChannelCmd = ENABLE;
  NVIC_Init(&nvic);

  USART_Cmd(USART2, ENABLE);
}

static void rs485_enable_rx_interrupt(void)
{
  USART_ClearFlag(USART2, USART_FLAG_ORE | USART_FLAG_FE |
                           USART_FLAG_NE | USART_FLAG_PE);
  USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
  USART_ITConfig(USART2, USART_IT_ERR, ENABLE);
  USART_ITConfig(USART2, USART_IT_PE, ENABLE);
}

static void record_rx_fault(uint32_t timestamp_us, bool uart_error)
{
  g_rx_fault_us = timestamp_us;
  g_rx_fault_epoch++;
  if (uart_error) {
    g_board_a_rtos_diag.rx_continuity_errors++;
  }
}

static void rx_isr(void)
{
  uint32_t sr = USART2->SR;
  uint32_t timestamp_us = TIM2->CNT;
  uint32_t error_flags = sr & (USART_FLAG_ORE | USART_FLAG_FE |
                               USART_FLAG_NE | USART_FLAG_PE);
  bool received = (sr & USART_FLAG_RXNE) != 0U;
  uint16_t data = 0U;
  BaseType_t higher_priority_task_woken = pdFALSE;

  if (received || (error_flags != 0U)) {
    data = (uint16_t)USART2->DR;
  }

  if (error_flags != 0U) {
    g_board_a_rtos_diag.uart_error_flags |= error_flags;
    record_rx_fault(timestamp_us, true);
  }

  if (received) {
    board_a_rx_event_t event;

    event.byte = (uint8_t)data;
    event.timestamp_us = timestamp_us;
    event.flags = error_flags;
    if (xQueueSendFromISR(g_rx_queue, &event,
                          &higher_priority_task_woken) != pdPASS) {
      g_board_a_rtos_diag.rx_queue_drops++;
      record_rx_fault(timestamp_us, false);
    }
  }

  if ((sr & USART_SR_TXE) != 0U &&
      (USART2->CR1 & USART_CR1_TXEIE) != 0U) {
    uint8_t byte = 0U;

    if (board_a_tx_on_txe(&g_tx, &byte)) {
      USART2->DR = (uint16_t)byte;
    }
    if (g_tx.state == BOARD_A_TX_WAIT_TC) {
      USART2->CR1 &= (uint16_t)~USART_CR1_TXEIE;
      USART2->CR1 |= USART_CR1_TCIE;
    } else if (g_tx.state != BOARD_A_TX_SENDING) {
      USART2->CR1 &= (uint16_t)~USART_CR1_TXEIE;
    }
  }

  if ((sr & USART_SR_TC) != 0U &&
      (USART2->CR1 & USART_CR1_TCIE) != 0U) {
    USART2->CR1 &= (uint16_t)~USART_CR1_TCIE;
    if (board_a_tx_on_tc(&g_tx)) {
      GPIO_ResetBits(GPIOG, GPIO_Pin_8);
      g_board_a_rtos_diag.tx_completions++;
      if (g_comm_task != NULL) {
        vTaskNotifyGiveFromISR(g_comm_task, &higher_priority_task_woken);
      }
    }
  }

  portYIELD_FROM_ISR(higher_priority_task_woken);
}

void USART2_IRQHandler(void)
{
  rx_isr();
}

static void tx_abort_hardware(void)
{
  taskENTER_CRITICAL();
  USART2->CR1 &= (uint16_t)~(USART_CR1_TXEIE | USART_CR1_TCIE);
  board_a_tx_abort(&g_tx);
  GPIO_ResetBits(GPIOG, GPIO_Pin_8);
  taskEXIT_CRITICAL();
}

static bool tx_send(const uint8_t *data, uint16_t length)
{
  uint32_t wire_time_us;
  uint32_t budget_us;
  uint32_t start_us;
  uint32_t notification_value;
  uint32_t status;
  uint8_t first_byte;

  if ((data == NULL) || (length == 0U)) {
    return false;
  }

  wire_time_us = ((uint32_t)length * 11U * 1000000U) / BOARD_A_RS485_BAUD;
  budget_us = wire_time_us + 100000U;

  (void)ulTaskNotifyTake(pdTRUE, 0U);
  taskENTER_CRITICAL();
  USART2->CR1 &= (uint16_t)~(USART_CR1_TXEIE | USART_CR1_TCIE);
  status = USART2->SR;
  if (((status & USART_SR_TXE) == 0U) ||
      ((status & (USART_FLAG_ORE | USART_FLAG_FE |
                  USART_FLAG_NE | USART_FLAG_PE)) != 0U)) {
    GPIO_ResetBits(GPIOG, GPIO_Pin_8);
    taskEXIT_CRITICAL();
    return false;
  }
  start_us = TIM2->CNT;
  if (!board_a_tx_start(&g_tx, data, length, start_us, budget_us)) {
    taskEXIT_CRITICAL();
    return false;
  }
  GPIO_SetBits(GPIOG, GPIO_Pin_8);
  /*
   * TC is cleared by reading SR and writing DR. Prime the first byte here,
   * which both starts this frame and clears any TC left by the previous one.
   */
  if (!board_a_tx_on_txe(&g_tx, &first_byte)) {
    board_a_tx_abort(&g_tx);
    GPIO_ResetBits(GPIOG, GPIO_Pin_8);
    taskEXIT_CRITICAL();
    return false;
  }
  USART2->DR = (uint16_t)first_byte;
  if (g_tx.state == BOARD_A_TX_WAIT_TC) {
    USART2->CR1 |= USART_CR1_TCIE;
  } else {
    USART2->CR1 |= USART_CR1_TXEIE;
  }
  taskEXIT_CRITICAL();

  for (;;) {
    uint32_t elapsed_us;
    uint32_t remaining_us;
    TickType_t wait_ticks;

    taskENTER_CRITICAL();
    if (board_a_tx_take_completion(&g_tx)) {
      taskEXIT_CRITICAL();
      return true;
    }
    if (board_a_tx_poll_timeout(&g_tx, TIM2->CNT)) {
      USART2->CR1 &= (uint16_t)~(USART_CR1_TXEIE | USART_CR1_TCIE);
      GPIO_ResetBits(GPIOG, GPIO_Pin_8);
      (void)board_a_tx_take_timeout(&g_tx);
      g_board_a_rtos_diag.tx_timeouts++;
      taskEXIT_CRITICAL();
      return false;
    }
    if (!board_a_tx_is_active(&g_tx)) {
      taskEXIT_CRITICAL();
      tx_abort_hardware();
      return false;
    }

    elapsed_us = (uint32_t)(TIM2->CNT - start_us);
    remaining_us = (elapsed_us < budget_us) ? (budget_us - elapsed_us) : 0U;
    taskEXIT_CRITICAL();

    wait_ticks = pdMS_TO_TICKS((remaining_us + 999U) / 1000U);
    if (wait_ticks == 0U) {
      wait_ticks = 1U;
    }
    (void)xTaskNotifyWait(0U, 0xFFFFFFFFUL, &notification_value,
                          wait_ticks);
  }
}

static void service_rx_fault(void)
{
  uint32_t epoch;
  uint32_t fault_us;

  taskENTER_CRITICAL();
  epoch = g_rx_fault_epoch;
  fault_us = g_rx_fault_us;
  taskEXIT_CRITICAL();

  if (epoch != g_rx_fault_epoch_seen) {
    g_rx_fault_epoch_seen = epoch;
    board_a_rx_recovery_begin(&g_rx_recovery, fault_us);
    modbus_rtu_rx_discard(&g_runtime.slave.receiver);
  }
}

static bool rx_queue_empty_and_sample(uint32_t *now_us, uint32_t *fault_epoch)
{
  bool empty;

  taskENTER_CRITICAL();
  empty = uxQueueMessagesWaiting(g_rx_queue) == 0U;
  if (empty) {
    *fault_epoch = g_rx_fault_epoch;
    *now_us = (uint32_t)board_a_rtos_now_us(NULL);
  }
  taskEXIT_CRITICAL();
  return empty;
}

static void process_rx_event(const board_a_rx_event_t *event)
{
  if (event->flags != 0U) {
    if (!board_a_rx_recovery_is_discarding(&g_rx_recovery)) {
      board_a_rx_recovery_begin(&g_rx_recovery, event->timestamp_us);
      modbus_rtu_rx_discard(&g_runtime.slave.receiver);
    }
    return;
  }

  if (!board_a_rx_recovery_accept(&g_rx_recovery, event->timestamp_us)) {
    return;
  }

  board_a_runtime_push_byte(&g_runtime, event->byte, event->timestamp_us);
}

static void debug_status(uint64_t now_us)
{
  board_a_runtime_status_t status;
  board_a_persistence_status_t persistence;

  if (!board_a_log_schedule_due(&g_log_schedule, now_us)) {
    return;
  }
  if (!board_a_runtime_copy_status(&g_runtime, &status)) {
    return;
  }
  if (!board_a_runtime_persistence_status(&g_runtime, &persistence)) {
    return;
  }

  debug_uart_puts("[board-a] run=");
  debug_write_u32(status.run_state);
  debug_uart_puts(" records=");
  debug_write_u32(status.sequence);
  debug_uart_puts(" rx=");
  debug_write_u32(status.stats.rx_frames);
  debug_uart_puts(" crc=");
  debug_write_u32(status.stats.crc_errors);
  debug_uart_puts(" tx=");
  debug_write_u32(status.stats.tx_responses);
  debug_uart_puts(" qdrop=");
  debug_write_u32(g_board_a_rtos_diag.rx_queue_drops);
  debug_uart_puts(" uerr=");
  debug_write_u32(g_board_a_rtos_diag.uart_error_flags);
  debug_uart_puts(" txto=");
  debug_write_u32(g_board_a_rtos_diag.tx_timeouts);
  debug_uart_puts(" gen=");
  debug_write_u32(persistence.generated);
  debug_uart_puts(" sync=");
  debug_write_u32(persistence.synced);
  debug_uart_puts(" drop=");
  debug_write_u32(persistence.dropped);
  debug_uart_puts(" edrop=");
  debug_write_u32(persistence.event_dropped);
  debug_uart_puts(" q=");
  debug_write_u32(persistence.queued);
  debug_uart_puts(" st=");
  debug_write_u32(persistence.storage_state);
  debug_uart_puts(" stk=");
  debug_write_u32(g_board_a_rtos_diag.comm_stack_min_words);
  debug_uart_puts("/");
  debug_write_u32(g_board_a_rtos_diag.acquisition_stack_min_words);
  debug_uart_puts("/");
  debug_write_u32(g_board_a_rtos_diag.config_stack_min_words);
  debug_uart_puts("/");
  debug_write_u32(g_board_a_rtos_diag.storage_stack_min_words);
  debug_uart_puts("\r\n");
}

static void comm_task(void *argument)
{
  board_a_rx_event_t event;
  size_t response_length;
  uint32_t now_us;
  uint32_t poll_epoch;
  bool scheduling_state_changed;

  (void)argument;
  taskENTER_CRITICAL();
  g_board_a_rtos_diag.task_ready_mask |= 1U << 0;
  taskEXIT_CRITICAL();
  rs485_enable_rx_interrupt();

  for (;;) {
    UBaseType_t queue_depth;

    service_rx_fault();
    queue_depth = uxQueueMessagesWaiting(g_rx_queue);
    if (queue_depth > g_board_a_rtos_diag.rx_queue_depth_max) {
      g_board_a_rtos_diag.rx_queue_depth_max = queue_depth;
    }
    if (xQueueReceive(g_rx_queue, &event, pdMS_TO_TICKS(5)) == pdPASS) {
      do {
        process_rx_event(&event);
        service_rx_fault();
      } while (xQueueReceive(g_rx_queue, &event, 0U) == pdPASS);
      if (g_config_task != NULL) {
        xTaskNotifyGive(g_config_task);
      }
      continue;
    }

    if (!rx_queue_empty_and_sample(&now_us, &poll_epoch)) {
      continue;
    }
    if ((g_rx_fault_epoch != g_rx_fault_epoch_seen) ||
        (g_rx_fault_epoch != poll_epoch)) {
      service_rx_fault();
      continue;
    }
    if (!board_a_rx_recovery_can_poll(&g_rx_recovery, now_us)) {
      continue;
    }

    response_length = board_a_runtime_poll_observe(
        &g_runtime, now_us, g_response, sizeof(g_response),
        &scheduling_state_changed);
    if (scheduling_state_changed) {
      xTaskNotifyGive(g_acquisition_task);
      if (g_storage_task != NULL) {
        xTaskNotifyGive(g_storage_task);
      }
      if (g_alarm_task != NULL) {
        xTaskNotifyGive(g_alarm_task);
      }
    }
    if (response_length != 0U) {
      (void)tx_send(g_response, (uint16_t)response_length);
      if (g_storage_task != NULL) {
        xTaskNotifyGive(g_storage_task);
      }
    }
    debug_status(board_a_rtos_now_us(NULL));
    g_board_a_rtos_diag.comm_stack_min_words =
        uxTaskGetStackHighWaterMark(NULL);
  }
}

/*
 * The acquisition task waits for the earliest pending deadline: the next
 * periodic sample while running, or the latched scheduled-start deadline while
 * armed. The wait is capped at one second so the TIM2 monotonic service keeps
 * extending even when no deadline is near.
 */
static uint32_t acquisition_wait_ms(const board_a_runtime_status_t *status,
                                    uint64_t now_us,
                                    uint64_t sensor_scan_us)
{
  uint64_t earliest_us = 0U;
  uint64_t remaining_us;
  bool have_deadline = false;

  if (status->run_state == BOARD_A_RUN_RUNNING) {
    if (status->start_pending || (status->next_sample_us == 0U)) {
      return 0U;
    }
    earliest_us = status->next_sample_us;
    have_deadline = true;
  }

  if (status->schedule_armed &&
      (!have_deadline || (status->schedule_deadline_us < earliest_us))) {
    earliest_us = status->schedule_deadline_us;
    have_deadline = true;
  }
  if ((sensor_scan_us != 0U) &&
      (!have_deadline || (sensor_scan_us < earliest_us))) {
    earliest_us = sensor_scan_us;
    have_deadline = true;
  }

  if (!have_deadline) {
    return BOARD_A_MAX_WAIT_MS;
  }
  if (earliest_us <= now_us) {
    return 0U;
  }

  remaining_us = earliest_us - now_us;
  if (remaining_us > (uint64_t)BOARD_A_MAX_WAIT_MS * 1000ULL) {
    return BOARD_A_MAX_WAIT_MS;
  }
  return (uint32_t)((remaining_us + 999ULL) / 1000ULL);
}

static void sensor_scan_if_due(uint64_t now_us)
{
  board_a_alarm_result_t alarm_result;
  board_a_runtime_status_t status;
  board_a_sensor_snapshot_t snapshot;
  board_a_sensor_map_t runtime_map;
  board_a_sensor_map_t manager_map;

  if (!board_a_runtime_copy_status(&g_runtime, &status) ||
      (status.data_source != BOARD_A_DATA_SOURCE_REAL_DS18B20)) {
    return;
  }
  if (board_a_runtime_copy_sensor_map(&g_runtime, &runtime_map) &&
      (runtime_map.valid_mask != 0U) &&
      board_a_sensor_manager_copy_map(&g_sensor_manager, &manager_map) &&
      (manager_map.valid_mask == 0U)) {
    (void)board_a_sensor_manager_set_map(&g_sensor_manager, &runtime_map);
  }
  if (board_a_sensor_manager_step(&g_sensor_manager, now_us, &snapshot)) {
    board_a_runtime_publish_sensor_snapshot(&g_runtime, &snapshot);
    (void)board_a_event_buffer_push_snapshot(&g_event_buffer, &snapshot);
    if (board_a_alarm_update(&g_alarm, &snapshot, &alarm_result)) {
      board_a_runtime_publish_alarm_result(&g_runtime, &alarm_result);
      if (status.run_state == BOARD_A_RUN_RUNNING) {
        (void)board_a_event_buffer_note_alarm_result(&g_event_buffer,
                                                     &alarm_result);
      }
      if (g_alarm_task != NULL) {
        xTaskNotifyGive(g_alarm_task);
      }
    }
  }
  if (board_a_sensor_manager_take_map_dirty(&g_sensor_manager) &&
      board_a_sensor_manager_copy_map(&g_sensor_manager, &manager_map)) {
    (void)board_a_runtime_publish_sensor_map(&g_runtime, &manager_map);
    if (!board_a_runtime_request_sensor_map_save(
            &g_runtime, (uint32_t)(now_us ^ (now_us >> 32U)))) {
      board_a_sensor_manager_mark_map_dirty(&g_sensor_manager);
    } else if (g_config_task != NULL) {
      xTaskNotifyGive(g_config_task);
    }
  }
}

static void apply_alarm_config_if_changed(void)
{
  board_a_alarm_config_t config;

  if (!board_a_runtime_copy_alarm_config(&g_runtime, &config)) {
    return;
  }
  if (g_alarm_config_applied &&
      board_a_alarm_config_equal(&g_applied_alarm_config, &config)) {
    return;
  }
  if (board_a_alarm_set_config(&g_alarm, &config)) {
    g_applied_alarm_config = config;
    g_alarm_config_applied = true;
  }
}

static void process_alarm_ack_request(void)
{
  board_a_alarm_state_t state;

  if (!board_a_runtime_take_alarm_ack_request(&g_runtime)) {
    return;
  }
  board_a_alarm_ack(&g_alarm);
  board_a_alarm_copy_state(&g_alarm, &state);
  board_a_runtime_publish_alarm_state(&g_runtime, &state);
  if (g_alarm_task != NULL) {
    xTaskNotifyGive(g_alarm_task);
  }
}

static void acquisition_task(void *argument)
{
  board_a_runtime_status_t status;
  uint32_t wait_ms;
  uint32_t notification_value;

  (void)argument;
  taskENTER_CRITICAL();
  g_board_a_rtos_diag.task_ready_mask |= 1U << 1;
  taskEXIT_CRITICAL();

  for (;;) {
    uint64_t now_us = board_a_rtos_now_us(NULL);
    board_a_runtime_status_t before_tick;
    board_a_runtime_status_t current;

    if (board_a_runtime_copy_status(&g_runtime, &current) &&
        current.event_stop_flush_pending) {
      event_finish_stop_flush(now_us);
    }
    process_alarm_ack_request();
    apply_alarm_config_if_changed();
    /*
     * A due sensor scan runs before the scheduler tick so a new record uses
     * the latest complete DS18B20 snapshot. The record period remains anchored
     * to the model's planned deadline; this bounded scan only affects the
     * actual completion time.
     */
    sensor_scan_if_due(now_us);
    now_us = board_a_rtos_now_us(NULL);
    board_a_event_buffer_step(&g_event_buffer, now_us / 1000ULL);
    (void)event_drain_to_persistence();
    sync_event_marker(now_us);

    if (board_a_runtime_copy_status(&g_runtime, &before_tick) &&
        (before_tick.run_state == BOARD_A_RUN_RUNNING) &&
        !before_tick.start_pending &&
        (before_tick.next_sample_us != 0U) &&
        (now_us >= before_tick.next_sample_us)) {
      uint32_t late_us =
          (uint32_t)(now_us - before_tick.next_sample_us);

      if (late_us > g_board_a_rtos_diag.schedule_late_max_us) {
        g_board_a_rtos_diag.schedule_late_max_us = late_us;
      }
    }

    board_a_runtime_tick(&g_runtime, now_us);
    if (board_a_runtime_copy_status(&g_runtime, &status)) {
      if (status.event_stop_flush_pending) {
        event_finish_stop_flush(now_us);
      }
    }
    if (g_storage_task != NULL) {
      xTaskNotifyGive(g_storage_task);
    }
    if (!board_a_runtime_copy_status(&g_runtime, &status)) {
      wait_ms = BOARD_A_MAX_WAIT_MS;
    } else {
      uint64_t sensor_scan_us =
          (status.data_source == BOARD_A_DATA_SOURCE_REAL_DS18B20) ?
          board_a_sensor_manager_next_step_us(&g_sensor_manager) : 0U;

      if (status.schedule_start_late_us >
          g_board_a_rtos_diag.schedule_start_late_max_us) {
        g_board_a_rtos_diag.schedule_start_late_max_us =
            status.schedule_start_late_us;
      }
      g_board_a_rtos_diag.schedule_start_count =
          status.schedule_start_count;
      wait_ms = acquisition_wait_ms(&status, now_us, sensor_scan_us);
    }

    (void)xTaskNotifyWait(0U, 0xFFFFFFFFUL, &notification_value,
                          pdMS_TO_TICKS(wait_ms));
    (void)notification_value;
    taskENTER_CRITICAL();
    g_board_a_rtos_diag.acquisition_stack_min_words =
        uxTaskGetStackHighWaterMark(NULL);
    taskEXIT_CRITICAL();
  }
}

static void alarm_task(void *argument)
{
  board_a_alarm_state_t alarm_state;
  board_a_runtime_status_t status;
  uint32_t notification_value;
  bool last_buzzer_active = false;

  (void)argument;
  taskENTER_CRITICAL();
  g_board_a_rtos_diag.task_ready_mask |= 1U << 2;
  taskEXIT_CRITICAL();

  for (;;) {
    uint32_t now_ms = board_a_rtos_now_ms();
    bool alarm_active = false;

    if (board_a_runtime_copy_status(&g_runtime, &status) &&
        (status.data_source == BOARD_A_DATA_SOURCE_REAL_DS18B20)) {
      alarm_active = true;
    }
    if (board_a_runtime_copy_alarm_state(&g_runtime, &alarm_state)) {
      board_a_alarm_output_update(
          &alarm_state, alarm_active && alarm_state.valid, now_ms);
    } else {
      board_a_alarm_output_force_off();
    }
    if (board_a_alarm_output_buzzer_active() != last_buzzer_active) {
      last_buzzer_active = board_a_alarm_output_buzzer_active();
      board_a_runtime_publish_alarm_buzzer_active(
          &g_runtime, last_buzzer_active);
    }
    g_board_a_rtos_diag.alarm_stack_min_words =
        uxTaskGetStackHighWaterMark(NULL);
    (void)xTaskNotifyWait(0U, 0xFFFFFFFFUL, &notification_value,
                          pdMS_TO_TICKS(BOARD_A_ALARM_WAIT_MS));
    (void)notification_value;
  }
}

static void create_runtime_objects(void)
{
  g_model_mutex = xSemaphoreCreateMutexStatic(&g_model_mutex_buffer);
  g_rx_queue = xQueueCreateStatic(BOARD_A_RX_QUEUE_CAPACITY,
                                  sizeof(board_a_rx_event_t),
                                  (uint8_t *)g_rx_queue_storage,
                                  &g_rx_queue_buffer);
  if ((g_model_mutex == NULL) || (g_rx_queue == NULL)) {
    board_a_rtos_fatal(BOARD_A_FAULT_TASK_CREATE);
  }

  board_a_runtime_init(&g_runtime, 1U, &g_runtime_ops, NULL);
  (void)board_a_runtime_set_data_source(
      &g_runtime, BOARD_A_DATA_SOURCE_REAL_DS18B20);

  g_comm_task = xTaskCreateStatic(
      comm_task, "comm", BOARD_A_COMM_STACK_WORDS, NULL,
      BOARD_A_COMM_TASK_PRIORITY, g_comm_stack, &g_comm_task_buffer);
  g_acquisition_task = xTaskCreateStatic(
      acquisition_task, "acq", BOARD_A_ACQUISITION_STACK_WORDS, NULL,
      BOARD_A_ACQUISITION_TASK_PRIORITY, g_acquisition_stack,
      &g_acquisition_task_buffer);
  g_alarm_task = xTaskCreateStatic(
      alarm_task, "alarm", BOARD_A_ALARM_STACK_WORDS, NULL,
      BOARD_A_ALARM_TASK_PRIORITY, g_alarm_stack, &g_alarm_task_buffer);
  g_config_task = xTaskCreateStatic(
      board_a_config_task, "config", BOARD_A_CONFIG_STACK_WORDS, &g_runtime,
      1U, g_config_stack, &g_config_task_buffer);
  g_storage_task = xTaskCreateStatic(
      board_a_storage_task, "storage", BOARD_A_STORAGE_STACK_WORDS,
      &g_runtime, 1U, g_storage_stack, &g_storage_task_buffer);
  if ((g_comm_task == NULL) || (g_acquisition_task == NULL) ||
      (g_alarm_task == NULL) ||
      (g_config_task == NULL) || (g_storage_task == NULL)) {
    board_a_rtos_fatal(BOARD_A_FAULT_TASK_CREATE);
  }

  g_board_a_rtos_diag.rx_queue_item_size = sizeof(board_a_rx_event_t);
}

int board_a_rtos_run(void)
{
  timer_init();
  debug_uart_init(DEBUG_UART_BAUD);
  debug_uart_puts("\r\n[board-a] Modbus RTU slave / FreeRTOS\r\n");
  debug_uart_puts("[board-a] addr=1 uart=USART2 PA2/PA3 PG8 9600 8E1\r\n");
  debug_uart_puts("[board-a] debug=USART1 PA9/PA10 115200 8N1\r\n");
  debug_uart_puts("[board-a] ds18b20=PG9 shared 1-Wire, 1s scan\r\n");

  board_a_tx_init(&g_tx);
  board_a_rx_recovery_init(&g_rx_recovery, BOARD_A_T35_US);
  board_a_log_schedule_init(&g_log_schedule, BOARD_A_LOG_PERIOD_US, 0U);
  delay_init(168U);
  sensor_gpio_init();
  board_a_sensor_manager_init(&g_sensor_manager, &g_sensor_port);
  board_a_alarm_init(&g_alarm);
  board_a_event_buffer_init(&g_event_buffer);
  board_a_alarm_output_init();
  create_runtime_objects();
  rs485_init();
  board_a_persistence_startup(&g_runtime);
  recover_incomplete_event();

  debug_uart_puts("[board-a] scheduler starting\r\n");
  vTaskStartScheduler();

  board_a_rtos_fatal(BOARD_A_FAULT_SCHEDULER_RETURNED);
  return 0;
}
