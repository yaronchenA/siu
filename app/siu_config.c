#include "siu_config.h"

static uint8_t s_led_brightness;
static uint8_t s_log_enable;

void siu_config_init(void)
{
    s_led_brightness = 100u;
    s_log_enable = 0u;          /* off by default (cpm_siu_protocol.md §8.8) */
}

bool siu_config_set(uint8_t key, const uint8_t *val, uint8_t len)
{
    switch (key) {
    case CFG_KEY_LED_BRIGHTNESS:
        if (len < 1u || val[0] > 100u) {
            return false;
        }
        s_led_brightness = val[0];
        return true;
    case CFG_KEY_LOG_ENABLE:
        if (len < 1u || val[0] > 1u) {
            return false;
        }
        s_log_enable = val[0];
        return true;
    default:
        return false;
    }
}

uint8_t siu_config_get(uint8_t key, uint8_t *out, uint8_t cap)
{
    switch (key) {
    case CFG_KEY_LED_BRIGHTNESS:
        if (cap < 1u) {
            return 0;
        }
        out[0] = s_led_brightness;
        return 1;
    case CFG_KEY_LOG_ENABLE:
        if (cap < 1u) {
            return 0;
        }
        out[0] = s_log_enable;
        return 1;
    default:
        return 0;
    }
}

uint8_t siu_config_led_brightness(void)
{
    return s_led_brightness;
}

bool siu_config_log_enabled(void)
{
    return s_log_enable != 0u;
}
