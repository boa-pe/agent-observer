#include "gfx.h"
#include <string.h>
#include <stdlib.h>
#include "font5x7.h"

static uint16_t *s_buf;
static int s_w, s_y0, s_y1; /* band rows [s_y0, s_y1) */

static inline uint16_t swap16(color_t c) { return (uint16_t)((c >> 8) | (c << 8)); }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

void gfx_begin_band(uint16_t *buf, int width, int y0, int lines)
{
    s_buf = buf;
    s_w = width;
    s_y0 = y0;
    s_y1 = y0 + lines;
}

int gfx_isqrt(int v)
{
    if (v <= 0) return 0;
    unsigned r = 0, bit = 1u << 30;
    unsigned n = (unsigned)v;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (int)r;
}

color_t gfx_mix(color_t a, color_t b, int t)
{
    if (t <= 0) return a;
    if (t >= 255) return b;
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + (br - ar) * t / 255;
    int g = ag + (bg - ag) * t / 255;
    int bl = ab + (bb - ab) * t / 255;
    return (color_t)((r << 11) | (g << 5) | bl);
}

static inline void span(int x0, int x1, int y, uint16_t sc)
{
    if (y < s_y0 || y >= s_y1) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= s_w) x1 = s_w - 1;
    if (x0 > x1) return;
    uint16_t *p = s_buf + (y - s_y0) * s_w + x0;
    for (int x = x0; x <= x1; x++) *p++ = sc;
}

void gfx_hline(int x0, int x1, int y, color_t c) { span(x0, x1, y, swap16(c)); }

void gfx_fill(color_t c)
{
    uint16_t sc = swap16(c);
    int n = s_w * (s_y1 - s_y0);
    for (int i = 0; i < n; i++) s_buf[i] = sc;
}

void gfx_fill_rect(int x, int y, int w, int h, color_t c)
{
    uint16_t sc = swap16(c);
    int ya = imax(y, s_y0), yb = imin(y + h, s_y1);
    for (int yy = ya; yy < yb; yy++) span(x, x + w - 1, yy, sc);
}

void gfx_rect(int x, int y, int w, int h, int t, color_t c)
{
    gfx_fill_rect(x, y, w, t, c);
    gfx_fill_rect(x, y + h - t, w, t, c);
    gfx_fill_rect(x, y, t, h, c);
    gfx_fill_rect(x + w - t, y, t, h, c);
}

void gfx_vgradient(int x, int y, int w, int h, color_t top, color_t bottom)
{
    int ya = imax(y, s_y0), yb = imin(y + h, s_y1);
    for (int yy = ya; yy < yb; yy++) {
        span(x, x + w - 1, yy, swap16(gfx_mix(top, bottom, (yy - y) * 255 / (h > 1 ? h - 1 : 1))));
    }
}

void gfx_fill_round_rect(int x, int y, int w, int h, int r, color_t c)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    uint16_t sc = swap16(c);
    int ya = imax(y, s_y0), yb = imin(y + h, s_y1);
    for (int yy = ya; yy < yb; yy++) {
        int inset = 0;
        int dy = 0;
        if (yy < y + r) dy = (y + r) - yy;
        else if (yy >= y + h - r) dy = yy - (y + h - r - 1);
        if (dy > 0) {
            /* pixel-centre distance for a smoother corner */
            int d2 = r * r * 4 - (2 * dy - 1) * (2 * dy - 1);
            inset = r - gfx_isqrt(d2) / 2;
        }
        span(x + inset, x + w - 1 - inset, yy, sc);
    }
}

void gfx_fill_ellipse(int cx, int cy, int rx, int ry, color_t c)
{
    if (rx <= 0 || ry <= 0) return;
    uint16_t sc = swap16(c);
    int ya = imax(cy - ry, s_y0), yb = imin(cy + ry, s_y1 - 1);
    for (int yy = ya; yy <= yb; yy++) {
        int dy = yy - cy;
        int dx = rx * gfx_isqrt((ry * ry - dy * dy) * 256 / (ry * ry)) / 16;
        span(cx - dx, cx + dx, yy, sc);
    }
}

void gfx_fill_circle(int cx, int cy, int r, color_t c)
{
    if (r <= 0) return;
    uint16_t sc = swap16(c);
    int ya = imax(cy - r, s_y0), yb = imin(cy + r, s_y1 - 1);
    for (int yy = ya; yy <= yb; yy++) {
        int dy = yy - cy;
        int dx = gfx_isqrt(r * r - dy * dy);
        span(cx - dx, cx + dx, yy, sc);
    }
}

void gfx_ring(int cx, int cy, int r_out, int r_in, color_t c)
{
    uint16_t sc = swap16(c);
    int ya = imax(cy - r_out, s_y0), yb = imin(cy + r_out, s_y1 - 1);
    for (int yy = ya; yy <= yb; yy++) {
        int dy = yy - cy;
        int dxo = gfx_isqrt(r_out * r_out - dy * dy);
        if (abs(dy) < r_in) {
            int dxi = gfx_isqrt(r_in * r_in - dy * dy);
            span(cx - dxo, cx - dxi, yy, sc);
            span(cx + dxi, cx + dxo, yy, sc);
        } else {
            span(cx - dxo, cx + dxo, yy, sc);
        }
    }
}

void gfx_thick_line(int x0, int y0, int x1, int y1, int thick, color_t c)
{
    int r = thick / 2;
    if (imax(y0, y1) + r < s_y0 || imin(y0, y1) - r >= s_y1) return;
    int dx = x1 - x0, dy = y1 - y0;
    int steps = imax(abs(dx), abs(dy));
    if (steps == 0) steps = 1;
    int stride = imax(1, r / 2);
    for (int i = 0; i <= steps; i += stride) {
        int x = x0 + dx * i / steps;
        int y = y0 + dy * i / steps;
        if (r > 0) gfx_fill_circle(x, y, r, c);
        else span(x, x, y, swap16(c));
    }
    if (r > 0) gfx_fill_circle(x1, y1, r, c);
}

void gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, color_t c)
{
    int t;
#define SWP(a, b) (t = a, a = b, b = t)
    if (y0 > y1) { SWP(y0, y1); SWP(x0, x1); }
    if (y1 > y2) { SWP(y1, y2); SWP(x1, x2); }
    if (y0 > y1) { SWP(y0, y1); SWP(x0, x1); }
#undef SWP
    if (y2 < s_y0 || y0 >= s_y1 || y0 == y2) return;
    uint16_t sc = swap16(c);
    int ya = imax(y0, s_y0), yb = imin(y2, s_y1 - 1);
    for (int y = ya; y <= yb; y++) {
        int xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        int xb;
        if (y < y1) xb = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
        else xb = (y2 == y1) ? x2 : x1 + (x2 - x1) * (y - y1) / (y2 - y1);
        if (xa > xb) { int q = xa; xa = xb; xb = q; }
        span(xa, xb, y, sc);
    }
}

int gfx_text_width(const char *s, int scale)
{
    int n = strlen(s);
    return n ? (n * 6 - 1) * scale : 0;
}

int gfx_text(int x, int y, const char *s, int scale, color_t c)
{
    if (y + 7 * scale < s_y0 || y >= s_y1) return gfx_text_width(s, scale);
    int x_start = x;
    uint16_t sc = swap16(c);
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t *g = font5x7[ch - 32];
        for (int col = 0; col < 5; col++) {
            uint8_t bits = g[col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1 << row)) {
                    int px = x + col * scale, py = y + row * scale;
                    for (int dy = 0; dy < scale; dy++) span(px, px + scale - 1, py + dy, sc);
                }
            }
        }
        x += 6 * scale;
    }
    return x - x_start - scale;
}

void gfx_text_centered(int cx, int y, const char *s, int scale, color_t c)
{
    gfx_text(cx - gfx_text_width(s, scale) / 2, y, s, scale, c);
}

int gfx_fit_scale(const char *s, int max_w, int max_scale)
{
    for (int sc = max_scale; sc > 1; sc--) {
        if (gfx_text_width(s, sc) <= max_w) return sc;
    }
    return 1;
}
