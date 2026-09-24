/*
 * Board interface — everything pin-, clock- and timer-specific lives behind this header.
 * One implementation per board: board_f0308disco.c today, our own SIU PCB later.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

/* Hardware identity reported in HW_INFO (cpm_siu_protocol.md §8.2). The bench board carries the
 * JCTC EVE0C-16S7 socket: 16 A, 3-phase, Type 2 socket (siu_detailed_design.md §8). */
#define BOARD_HW_MODEL           0xB001u   /* bench prototype */
#define BOARD_HW_REV             0u
#define BOARD_CONNECTOR_RATING_A 16u
#define BOARD_PHASES             3u
#define BOARD_CONNECTOR_TYPE     0u        /* 0 = Type 2 socket */

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

/* STM32 96-bit factory unique ID. */
void board_uid(uint8_t out[12]);

/* Why the chip last reset, as a protocol RESET_* value. Read once, at start-up. */
uint8_t board_reset_reason(void);

/* Random 32-bit value from ADC noise, mixed with the UID — for BOOT_ID. Not cryptographic. */
uint32_t board_random32(void);

/* Masks interrupts; for data shared with the link handler (PendSV). Keep sections short. */
uint32_t board_critical_enter(void);
void board_critical_exit(uint32_t state);

/* Requests the link handler to run (pends PendSV). Safe to call from an ISR. */
void board_link_task_pend(void);

#endif /* BOARD_H */
