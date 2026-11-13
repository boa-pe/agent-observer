#pragma once
#include <stdint.h>

typedef enum {
    LED_PATTERN_OFF,
    LED_PATTERN_SOLID,      /* steady color */
    LED_PATTERN_BLINK,      /* 50% on/off, ~1.25 Hz */
    LED_PATTERN_FAST_BLINK, /* ~4 Hz */
    LED_PATTERN_BREATHE,    /* smooth fade */
} led_pattern_t;

void status_led_init(void);
void status_led_set(led_pattern_t p, uint8_t r, uint8_t g, uint8_t b);
