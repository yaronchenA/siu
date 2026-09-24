/*
 * RGB status LED driver: turns a colour into PWM duty, applying the board's colour balance,
 * the configured brightness, and a gamma curve so fades and mixed colours look even.
 */
#ifndef RGB_LED_H
#define RGB_LED_H

#include <stdint.h>

#include "rgb.h"

/* 0..100 % — from CONFIG_SET key 0x01 (cpm_siu_protocol.md §8.7). Default 100. */
void rgb_led_set_brightness(uint8_t percent);

void rgb_led_show(rgb_t c);

#endif /* RGB_LED_H */
