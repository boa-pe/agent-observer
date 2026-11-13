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
/* Obtain a free band buffer (DISP_W * DISP_BAND_LINES pixels, byte-swapped RGB565).
 * Blocks while both buffers are still being transferred. */
uint16_t *display_acquire_band(void);
/* Send a band to the panel (asynchronous; the buffer is released when DMA is done). */
void display_flush_band(uint16_t *buf, int y0, int lines);
