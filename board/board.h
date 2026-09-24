/*
 * Board interface — everything pin-, clock- and timer-specific lives behind this header.
 * One implementation per board: board_f0308disco.c today, our own SIU PCB later.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

/* LED PWM full scale (compare value for 100 % on). */
#define BOARD_LED_PWM_MAX 999u

/* Per-channel colour balance for the fitted LED(s), 0..256 (256 = no correction).
 * Set once per LED part number so white/amber/purple look right (siu_detailed_design.md §6.1.5). */
extern const uint16_t board_led_balance[3];

/* Puts outputs in their safe state, then sets up clocks (48 MHz), the 1 ms tick, and GPIO/timers. */
void board_init(void);

/* Milliseconds since board_init(); wraps after ~49 days — compare with subtraction. */
uint32_t board_millis(void);

/* Raw user-button level (bench board only — demo input). */
bool board_button_pressed(void);

/* LED PWM duty per channel, 0..BOARD_LED_PWM_MAX. */
void board_led_pwm_write(uint16_t r, uint16_t g, uint16_t b);

/* Link UART (to the CPM): peripheral clock of the USART, and its TX/RX/DE pins set up. */
#define BOARD_LINK_UART_CLOCK_HZ 48000000u
void board_link_uart_pins_init(void);

#endif /* BOARD_H */
