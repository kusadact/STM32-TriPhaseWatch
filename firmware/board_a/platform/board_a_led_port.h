#ifndef BOARD_A_LED_PORT_H
#define BOARD_A_LED_PORT_H

#include <stdbool.h>

void board_a_led_port_init(void);
void board_a_led_port_set(bool led0_on, bool led1_on);

#endif /* BOARD_A_LED_PORT_H */
