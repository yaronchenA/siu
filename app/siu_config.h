/*
 * SIU settings changed by the CPM with CONFIG_SET / read with CONFIG_GET (cpm_siu_protocol.md §8.7).
 * Held in RAM for now; persisted to flash once cfg_flash exists (ARCHITECTURE.md §5).
 */
#ifndef SIU_CONFIG_H
#define SIU_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* CONFIG keys (§8.7) */
enum {
    CFG_KEY_LED_BRIGHTNESS = 0x01,   /* u8, 0..100 % */
};

void siu_config_init(void);

/* Returns false if the key is unknown or the value invalid. */
bool siu_config_set(uint8_t key, const uint8_t *val, uint8_t len);

/* Writes the value for key into out (up to cap bytes); returns its length, 0 if the key is unknown. */
uint8_t siu_config_get(uint8_t key, uint8_t *out, uint8_t cap);

uint8_t siu_config_led_brightness(void);

#endif /* SIU_CONFIG_H */
