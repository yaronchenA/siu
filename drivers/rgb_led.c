#include "rgb_led.h"

#include "board.h"

static uint8_t s_brightness = 100u;

void rgb_led_set_brightness(uint8_t percent)
{
    s_brightness = percent > 100u ? 100u : percent;
}

static uint16_t channel_duty(uint8_t value, uint16_t balance)
{
    uint32_t v = ((uint32_t)value * balance) >> 8;   /* colour balance, 0..255 */
    v = (v * s_brightness) / 100u;                  /* brightness */
    v = (v * v) / 255u;                             /* gamma ~2: perceptually even steps */
    return (uint16_t)((v * BOARD_LED_PWM_MAX) / 255u);
}

void rgb_led_show(rgb_t c)
{
    board_led_pwm_write(channel_duty(c.r, board_led_balance[0]),
                        channel_duty(c.g, board_led_balance[1]),
                        channel_duty(c.b, board_led_balance[2]));
}
