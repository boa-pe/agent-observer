#pragma once
#include <stdint.h>

typedef uint16_t color_t; /* RGB565, native order */

#define RGB(r, g, b) ((color_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* All drawing goes into the current band; coordinates are full-screen. */
void gfx_begin_band(uint16_t *buf, int width, int y0, int lines);

color_t gfx_mix(color_t a, color_t b, int t /* 0..255: 0 = a, 255 = b */);

void gfx_fill(color_t c);
void gfx_hline(int x0, int x1, int y, color_t c);
void gfx_fill_rect(int x, int y, int w, int h, color_t c);
void gfx_rect(int x, int y, int w, int h, int t, color_t c);
void gfx_vgradient(int x, int y, int w, int h, color_t top, color_t bottom);
void gfx_fill_round_rect(int x, int y, int w, int h, int r, color_t c);
void gfx_fill_circle(int cx, int cy, int r, color_t c);
void gfx_fill_ellipse(int cx, int cy, int rx, int ry, color_t c);
void gfx_ring(int cx, int cy, int r_out, int r_in, color_t c);
void gfx_thick_line(int x0, int y0, int x1, int y1, int thick, color_t c);
void gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, color_t c);

int gfx_text_width(const char *s, int scale);
int gfx_text(int x, int y, const char *s, int scale, color_t c);
void gfx_text_centered(int cx, int y, const char *s, int scale, color_t c);
/* Largest scale (<= max_scale) at which s fits into max_w. */
int gfx_fit_scale(const char *s, int max_w, int max_scale);

int gfx_isqrt(int v);
