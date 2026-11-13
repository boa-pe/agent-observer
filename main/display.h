#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sdkconfig.h"

#define DISP_W CONFIG_AO_LCD_WIDTH
#define DISP_H CONFIG_AO_LCD_HEIGHT
#define DISP_BAND_LINES 32

esp_err_t display_init(void);
void display_backlight(bool on);
/* Backlight intensity in percent (5..100, clamped). persist = also store it in NVS. */
esp_err_t display_set_brightness(int percent, bool persist);
uint8_t display_get_brightness(void);
/* Inactivity dimming: minutes (0..60, 0 = never) without an agent event after which the
 * backlight drops to half the set level. Persisted in NVS. */
esp_err_t display_set_dim_timeout(int minutes);
uint8_t display_get_dim_timeout(void);
bool display_is_dimmed(void);
/* Call on every received agent event: restores full brightness and restarts the countdown. */
void display_activity(void);
/* Call periodically (the UI loop does); dims when the timeout has elapsed. */
void display_dim_tick(void);
/* Obtain a free band buffer (DISP_W * DISP_BAND_LINES pixels, byte-swapped RGB565).
 * Blocks while both buffers are still being transferred. */
uint16_t *display_acquire_band(void);
/* Send a band to the panel (asynchronous; the buffer is released when DMA is done). */
void display_flush_band(uint16_t *buf, int y0, int lines);
