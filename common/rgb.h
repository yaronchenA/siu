/* RGB colour value shared by app/led_ctrl (decides the colour) and drivers/rgb_led (shows it). */
#ifndef RGB_H
#define RGB_H

#include <stdint.h>

typedef struct {
    uint8_t r, g, b;
} rgb_t;

#endif /* RGB_H */
