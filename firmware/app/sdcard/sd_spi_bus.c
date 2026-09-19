#include "sd_spi_bus.h"

#include "delay.h"
#include "stm32f4xx.h"

/*
 * Software SPI on the on-board SD socket lines (see the local wiring notes):
 *   PC11 / SD DAT3 -> CS
 *   PD2  / SD CMD  -> MOSI
 *   PC8  / SD DAT0 -> MISO
 *   PC12 / SD CLK  -> SCK
 * PC9/PC10 (DAT1/DAT2) stay pulled-up inputs and are unused in SPI mode.
 *
 * SPI mode 0 (CPOL=0, CPHA=0):
 *   MOSI changes while SCK is low, the card samples it on the rising edge,
 *   MISO is sampled right after the rising edge.
 *
 * Slow mode inserts SD_BUS_SLOW_DELAY_US on both half periods so card
 * initialization stays below the 400 kHz SD limit; fast mode runs at GPIO
 * toggle speed. Both rates are design values, not oscilloscope measurements.
 */

#define SD_BUS_SLOW_DELAY_US 2u

#define SD_CS_PIN   GPIO_Pin_11
#define SD_SCK_PIN  GPIO_Pin_12
#define SD_MISO_PIN GPIO_Pin_8
#define SD_MOSI_PIN GPIO_Pin_2

static uint8_t sd_bus_slow = 1u;

static void pin_high(GPIO_TypeDef *port, uint16_t pin)
{
  GPIO_SetBits(port, pin);
}

static void pin_low(GPIO_TypeDef *port, uint16_t pin)
{
  GPIO_ResetBits(port, pin);
}

static uint8_t pin_is_high(GPIO_TypeDef *port, uint16_t pin)
{
  return ((port->IDR & pin) != 0u) ? 1u : 0u;
}

void sd_bus_init(void)
{
  GPIO_InitTypeDef gpio;

  RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC | RCC_AHB1Periph_GPIOD, ENABLE);

  gpio.GPIO_Speed = GPIO_Speed_100MHz;
  gpio.GPIO_OType = GPIO_OType_PP;
  gpio.GPIO_Mode = GPIO_Mode_OUT;
  gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;
  gpio.GPIO_Pin = SD_CS_PIN | SD_SCK_PIN;
  GPIO_Init(GPIOC, &gpio);
  gpio.GPIO_Pin = SD_MOSI_PIN;
  GPIO_Init(GPIOD, &gpio);

  gpio.GPIO_Mode = GPIO_Mode_IN;
  gpio.GPIO_PuPd = GPIO_PuPd_UP;
  gpio.GPIO_Pin = SD_MISO_PIN | GPIO_Pin_9 | GPIO_Pin_10;
  GPIO_Init(GPIOC, &gpio);

  pin_low(GPIOC, SD_SCK_PIN);
  pin_high(GPIOC, SD_CS_PIN);
  pin_high(GPIOD, SD_MOSI_PIN);
  sd_bus_slow = 1u;
}

void sd_bus_set_slow(uint8_t slow)
{
  sd_bus_slow = (slow != 0u) ? 1u : 0u;
}

void sd_bus_cs(uint8_t level)
{
  if (level != 0u) {
    pin_high(GPIOC, SD_CS_PIN);
  } else {
    pin_low(GPIOC, SD_CS_PIN);
  }
}

uint8_t sd_bus_xfer(uint8_t tx)
{
  uint8_t rx = 0u;
  uint8_t bit;

  for (bit = 0u; bit < 8u; bit++) {
    if ((tx & 0x80u) != 0u) {
      pin_high(GPIOD, SD_MOSI_PIN);
    } else {
      pin_low(GPIOD, SD_MOSI_PIN);
    }
    tx = (uint8_t)(tx << 1);

    if (sd_bus_slow != 0u) {
      sd_bus_delay_us(SD_BUS_SLOW_DELAY_US);
    }
    pin_high(GPIOC, SD_SCK_PIN);

    rx = (uint8_t)(rx << 1);
    if (pin_is_high(GPIOC, SD_MISO_PIN) != 0u) {
      rx |= 0x01u;
    }

    if (sd_bus_slow != 0u) {
      sd_bus_delay_us(SD_BUS_SLOW_DELAY_US);
    }
    pin_low(GPIOC, SD_SCK_PIN);
  }

  return rx;
}

void sd_bus_delay_us(uint32_t us)
{
  delay_us(us);
}

void sd_bus_delay_ms(uint32_t ms)
{
  while (ms-- != 0u) {
    delay_ms(1u);
  }
}
