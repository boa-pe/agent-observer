/*
 * Screen rendering. Each frame is drawn band by band (DISP_BAND_LINES rows at a time)
 * so no full framebuffer is required; every draw call clips to the current band.
 * All layout is derived from DISP_W / DISP_H so other panel sizes work unchanged.
 */
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "display.h"
#include "gfx.h"
#include "agents.h"
#include "wifi_mgr.h"

#define TWO_PI 6.2831853f
#define FRAME_MS 40

/* ---------- palette ---------- */
#define C_BG_TOP    RGB(0x0B, 0x10, 0x20)
#define C_BG_BOT    RGB(0x17, 0x1D, 0x33)
#define C_TEXT      RGB(0xE8, 0xEC, 0xF4)
#define C_MUTED     RGB(0x7A, 0x84, 0x96)
#define C_FACE      RGB(0x10, 0x15, 0x26)
#define C_SHADOW    RGB(0x05, 0x07, 0x10)
#define C_GRAY      RGB(0x6B, 0x72, 0x80)
#define C_WHITE     RGB(0xFF, 0xFF, 0xFF)
#define C_KEYBOARD  RGB(0x3A, 0x44, 0x5C)

static volatile ui_mode_t s_mode = UI_MODE_BOOT;
static volatile int s_boot_window_pct, s_boot_hold_pct;
static char s_msg_title[32], s_msg_body[96];
static agents_snapshot_t s_snap; /* refreshed once per frame */
static char s_ip[20];
static bool s_connected;

void ui_set_mode(ui_mode_t mode) { s_mode = mode; }
void ui_set_boot_progress(int window_pct, int hold_pct)
{
    s_boot_window_pct = window_pct;
    s_boot_hold_pct = hold_pct;
}
void ui_set_message(const char *title, const char *body)
{
    strlcpy(s_msg_title, title, sizeof(s_msg_title));
    strlcpy(s_msg_body, body, sizeof(s_msg_body));
    s_mode = UI_MODE_MESSAGE;
}

static color_t state_color(agent_state_t s)
{
    switch (s) {
    case AGENT_STATE_IDLE:    return RGB(0x34, 0xD0, 0x48); /* green */
    case AGENT_STATE_WORKING: return RGB(0xF0, 0x3A, 0x2E); /* red */
    case AGENT_STATE_WAITING: return RGB(0xFF, 0xD4, 0x00); /* yellow (blinking) */
    case AGENT_STATE_ERROR:   return RGB(0xFF, 0x3D, 0x9A);
    default:                  return C_MUTED;
    }
}

static const char *state_label(agent_state_t s)
{
    switch (s) {
    case AGENT_STATE_IDLE:    return "IDLE";
    case AGENT_STATE_WORKING: return "WORKING";
    case AGENT_STATE_WAITING: return "WAITING";
    case AGENT_STATE_ERROR:   return "ERROR";
    default:                  return "OFFLINE";
    }
}

static void kind_colors(agent_kind_t k, color_t *body, color_t *shade)
{
    switch (k) {
    case AGENT_KIND_CLAUDE:  *body = RGB(0xD9, 0x77, 0x57); *shade = RGB(0xA8, 0x50, 0x36); break;
    case AGENT_KIND_COPILOT: *body = RGB(0x7C, 0x6C, 0xF0); *shade = RGB(0x52, 0x44, 0xC0); break;
    default:                 *body = RGB(0x2B, 0xB3, 0xA3); *shade = RGB(0x1C, 0x80, 0x74); break;
    }
}

static const char *kind_label(agent_kind_t k)
{
    switch (k) {
    case AGENT_KIND_CLAUDE:  return "CLAUDE";
    case AGENT_KIND_COPILOT: return "COPILOT";
    default:                 return "AGENT";
    }
}

/* ---------- text helpers ---------- */

/* Copy s into out, truncating with ".." so it fits max_w at scale. */
static void fit_text(char *out, size_t out_len, const char *s, int max_w, int scale)
{
    strlcpy(out, s, out_len);
    if (gfx_text_width(out, scale) <= max_w) return;
    size_t n = strlen(out);
    while (n > 1) {
        n--;
        out[n] = 0;
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s..", out);
        if (gfx_text_width(tmp, scale) <= max_w) {
            strlcpy(out, tmp, out_len);
            return;
        }
    }
}

/* Word-wrapped, centred text. Returns y after the last line. */
static int text_wrapped(int cx, int y, int max_w, const char *s, int scale, color_t c)
{
    int max_chars = (max_w + scale) / (6 * scale);
    if (max_chars < 1) max_chars = 1;
    char line[64];
    while (*s) {
        while (*s == ' ') s++;
        int len = strlen(s);
        int take = len;
        const char *nl = strchr(s, '\n');
        if (nl && nl - s < take) take = nl - s;
        if (take > max_chars) {
            take = max_chars;
            for (int i = max_chars; i > 0; i--) {
                if (s[i] == ' ') { take = i; break; }
            }
        }
        if (take > (int)sizeof(line) - 1) take = sizeof(line) - 1;
        memcpy(line, s, take);
        line[take] = 0;
        gfx_text_centered(cx, y, line, scale, c);
        y += 9 * scale;
        s += take;
        if (*s == '\n') s++;
    }
    return y;
}

static void format_duration(char *buf, size_t len, int64_t us)
{
    int64_t s = us / 1000000;
    if (s < 60) snprintf(buf, len, "%ds", (int)s);
    else if (s < 3600) snprintf(buf, len, "%dm %02ds", (int)(s / 60), (int)(s % 60));
    else snprintf(buf, len, "%dh %02dm", (int)(s / 3600), (int)((s / 60) % 60));
}

/* ---------- the robot ---------- */

typedef struct {
    float k;   /* pixels per unit */
    int cx;    /* centre x */
    int top;   /* y of unit 0 */
} fig_t;

/* Figure is designed in a ~120 x 140 unit box, x centred on 0, y from 0 (top) to 140. */
#define FIG_UNITS_W 120.0f
#define FIG_UNITS_H 142.0f

static inline int FX(const fig_t *f, float u) { return f->cx + (int)lrintf(u * f->k); }
static inline int FY(const fig_t *f, float u) { return f->top + (int)lrintf(u * f->k); }
static inline int FS(const fig_t *f, float u) { int v = (int)lrintf(u * f->k); return v < 1 ? 1 : v; }

static void fig_rrect(const fig_t *f, float x, float y, float w, float h, float r, color_t c)
{
    int x0 = FX(f, x), y0 = FY(f, y);
    gfx_fill_round_rect(x0, y0, FX(f, x + w) - x0, FY(f, y + h) - y0, FS(f, r), c);
}

static void fig_line(const fig_t *f, float x0, float y0, float x1, float y1, float t, color_t c)
{
    gfx_thick_line(FX(f, x0), FY(f, y0), FX(f, x1), FY(f, y1), FS(f, t), c);
}

static void fig_circle(const fig_t *f, float x, float y, float r, color_t c)
{
    gfx_fill_circle(FX(f, x), FY(f, y), FS(f, r), c);
}

static void draw_bubble(const fig_t *f, float cx, float cy, float w, float h, float tail_x, float tail_y,
                        color_t c, const char *text, color_t text_c)
{
    gfx_fill_triangle(FX(f, cx - w * 0.25f), FY(f, cy + h * 0.3f), FX(f, cx - w * 0.05f), FY(f, cy + h * 0.45f),
                      FX(f, tail_x), FY(f, tail_y), c);
    fig_rrect(f, cx - w / 2, cy - h / 2, w, h, h * 0.4f, c);
    if (text) {
        int sc = (int)(f->k * h / 12.0f);
        if (sc < 1) sc = 1;
        gfx_text_centered(FX(f, cx) + sc / 2, FY(f, cy) - 7 * sc / 2, text, sc, text_c);
    }
}

static void draw_robot(const fig_t *f, agent_kind_t kind, agent_state_t st, uint32_t t)
{
    const float ph = t / 1000.0f;
    /* The figure is painted in the state color (green idle, red working, yellow blinking while
     * waiting); the agent kind stays recognizable by its antennas, face and the label above. */
    color_t acc = state_color(st);
    color_t body, shade;
    if (st == AGENT_STATE_OFFLINE) {
        kind_colors(kind, &body, &shade);
        body = gfx_mix(body, C_GRAY, 190);
        shade = gfx_mix(shade, C_GRAY, 190);
    } else {
        body = acc;
        if (st == AGENT_STATE_WAITING) {
            /* same 800 ms / 50 % blink and same clock as the status LED, so both flash together */
            bool on = (esp_timer_get_time() / 1000) % 800 < 400;
            if (!on) body = gfx_mix(acc, C_SHADOW, 170);
        }
        shade = gfx_mix(body, C_SHADOW, 90);
    }
    const color_t dark = gfx_mix(shade, C_SHADOW, 110);

    /* --- motion parameters --- */
    float lift = 0;   /* whole figure (hops) */
    float bob = 0;    /* upper body */
    float ox = 0;     /* horizontal shake */
    switch (st) {
    case AGENT_STATE_IDLE:    bob = 2.0f * sinf(ph * TWO_PI / 2.4f); break;
    case AGENT_STATE_WORKING: bob = -1.6f * fabsf(sinf(ph * TWO_PI / 0.5f)); break;
    case AGENT_STATE_WAITING: lift = -5.0f * fabsf(sinf(ph * TWO_PI / 1.6f)); break;
    case AGENT_STATE_ERROR:   ox = (t % 3000) < 700 ? 2.5f * sinf(ph * TWO_PI * 11) : 0; break;
    default:                  bob = 0.8f * sinf(ph * TWO_PI / 4.0f); break;
    }
    const float ub = lift + bob;          /* upper body offset */
    const float head_y = 34 + lift + bob * 1.4f;

    /* shadow (shrinks while hopping) */
    gfx_fill_ellipse(FX(f, 0), FY(f, 136), FS(f, 30 + lift * 1.2f), FS(f, 4), C_SHADOW);

    /* legs + feet */
    for (int side = -1; side <= 1; side += 2) {
        fig_rrect(f, ox + side * 12 - 5, 112 + ub, 10, 18 - bob, 3, shade);
        fig_rrect(f, ox + side * 12 - 9, 126 + lift, 18, 7, 3, dark);
    }

    /* neck + body */
    fig_rrect(f, ox - 7, head_y + 40, 14, 12, 2, shade);
    fig_rrect(f, ox - 26, 84 + ub, 52, 34, 10, body);
    fig_rrect(f, ox - 16, 91 + ub, 32, 20, 5, shade);
    int pulse;
    switch (st) {
    case AGENT_STATE_WORKING: pulse = (t % 300) < 150 ? 0 : 160; break;
    case AGENT_STATE_WAITING: pulse = (t % 800) < 400 ? 0 : 200; break;
    case AGENT_STATE_OFFLINE: pulse = 200; break;
    default: pulse = (int)(90 + 90 * sinf(ph * TWO_PI / 2.0f)); break;
    }
    fig_circle(f, ox, 101 + ub, 4.5f, gfx_mix(acc, shade, pulse));

    /* keyboard while working */
    float tapL = 0, tapR = 0;
    if (st == AGENT_STATE_WORKING) {
        fig_rrect(f, ox - 32, 114 + ub, 64, 8, 2, C_KEYBOARD);
        for (int i = 0; i < 7; i++) {
            bool lit = ((t / 120) + i * 3) % 7 == 0;
            fig_rrect(f, ox - 28 + i * 8, 116 + ub, 6, 3, 1, lit ? acc : gfx_mix(C_KEYBOARD, C_WHITE, 60));
        }
        tapL = sinf(ph * TWO_PI * 3.0f) > 0 ? -3 : 0;
        tapR = sinf(ph * TWO_PI * 3.0f + 1.8f) > 0 ? -3 : 0;
    }

    /* arms: angle in degrees, 0 = right, 90 = down */
    const float sy = 92 + ub;
    float aL = 100, aR = 80;
    switch (st) {
    case AGENT_STATE_IDLE: {
        float sw = 6 * sinf(ph * TWO_PI / 2.4f);
        aL = 102 + sw; aR = 78 - sw;
        break;
    }
    case AGENT_STATE_WAITING:
        aL = 104;
        aR = -45 + 28 * sinf(ph * TWO_PI / 0.7f); /* waving */
        break;
    case AGENT_STATE_ERROR: {
        float sw = 8 * sinf(ph * TWO_PI * 2);
        aL = -130 + sw; aR = -50 - sw;
        break;
    }
    case AGENT_STATE_OFFLINE:
        aL = 95; aR = 85;
        break;
    default:
        break;
    }
    const float arm_len = 22, arm_t = 7, hand_r = 5;
    for (int side = -1; side <= 1; side += 2) {
        float shx = ox + side * 25;
        float hx, hy;
        if (st == AGENT_STATE_WORKING) {
            hx = ox + side * 13;
            hy = 113 + ub + (side < 0 ? tapL : tapR);
        } else {
            float a = (side < 0 ? aL : aR) * (TWO_PI / 360.0f);
            hx = shx + arm_len * cosf(a);
            hy = sy + arm_len * sinf(a);
        }
        fig_line(f, shx, sy, hx, hy, arm_t, shade);
        fig_circle(f, hx, hy, hand_r, body);
    }

    /* antenna */
    color_t ant = acc;
    if (st == AGENT_STATE_WORKING) ant = (t % 400) < 200 ? acc : gfx_mix(acc, C_WHITE, 120);
    else if (st == AGENT_STATE_WAITING) ant = (t % 800) < 400 ? acc : gfx_mix(acc, shade, 200);
    else if (st == AGENT_STATE_OFFLINE) ant = C_GRAY;
    if (kind == AGENT_KIND_COPILOT) {
        for (int side = -1; side <= 1; side += 2) {
            fig_line(f, ox + side * 14, head_y + 2, ox + side * 21, head_y - 9, 3, shade);
            fig_circle(f, ox + side * 21, head_y - 10, 4, ant);
        }
    } else {
        fig_line(f, ox, head_y + 2, ox, head_y - 9, 3, shade);
        fig_circle(f, ox, head_y - 12, 5, ant);
    }

    /* head */
    fig_circle(f, ox - 33, head_y + 22, 4, shade);
    fig_circle(f, ox + 33, head_y + 22, 4, shade);
    fig_rrect(f, ox - 32, head_y, 64, 44, 13, body);

    float ex = 12, ey = head_y + 20;
    if (kind == AGENT_KIND_COPILOT) {
        ex = 13;
        fig_rrect(f, ox - 4, ey - 2, 8, 4, 1, shade);
        for (int side = -1; side <= 1; side += 2) {
            fig_circle(f, ox + side * ex, ey, 12.5f, shade);
            fig_circle(f, ox + side * ex, ey, 10, C_FACE);
        }
    } else {
        fig_rrect(f, ox - 26, head_y + 7, 52, 31, 8, C_FACE);
    }

    /* eyes */
    color_t eye = st == AGENT_STATE_OFFLINE ? C_GRAY : acc;
    float look = 0, ew = 9, eh = 12;
    switch (st) {
    case AGENT_STATE_IDLE:
        look = 2.0f * sinf(ph * TWO_PI / 5.0f);
        if (t % 3600 < 140) eh = 2;
        break;
    case AGENT_STATE_WORKING:
        look = 3.0f * sinf(ph * TWO_PI / 1.2f);
        ew = 10; eh = 6; ey += 2;
        break;
    case AGENT_STATE_WAITING:
        ew = 11; eh = 14;
        if (t % 2500 < 120) eh = 2;
        break;
    case AGENT_STATE_OFFLINE:
        ew = 10; eh = 2; ey += 2;
        break;
    default:
        break;
    }
    for (int side = -1; side <= 1; side += 2) {
        float x = ox + side * ex + look;
        if (st == AGENT_STATE_ERROR) {
            fig_line(f, x - 4, ey - 4, x + 4, ey + 4, 2.5f, eye);
            fig_line(f, x - 4, ey + 4, x + 4, ey - 4, 2.5f, eye);
            continue;
        }
        float r = (ew < eh ? ew : eh) / 2.5f;
        fig_rrect(f, x - ew / 2, ey - eh / 2, ew, eh, r, eye);
        if (eh > 6) fig_circle(f, x - ew / 5, ey - eh / 5, 1.6f, gfx_mix(eye, C_WHITE, 200));
    }

    /* mouth */
    color_t mc = kind == AGENT_KIND_COPILOT ? C_FACE : gfx_mix(acc, C_FACE, 60);
    if (st == AGENT_STATE_OFFLINE) mc = kind == AGENT_KIND_COPILOT ? C_FACE : C_GRAY;
    float my = kind == AGENT_KIND_COPILOT ? head_y + 37 : head_y + 31;
    switch (st) {
    case AGENT_STATE_IDLE:
        fig_line(f, ox - 5, my - 1, ox - 2, my + 1, 2, mc);
        fig_line(f, ox - 2, my + 1, ox + 2, my + 1, 2, mc);
        fig_line(f, ox + 2, my + 1, ox + 5, my - 1, 2, mc);
        break;
    case AGENT_STATE_WORKING: {
        float w = 3 + 1.5f * sinf(ph * TWO_PI * 2);
        fig_line(f, ox - w, my, ox + w, my, 2, mc);
        break;
    }
    case AGENT_STATE_WAITING:
        gfx_ring(FX(f, ox), FY(f, my), FS(f, 3.2f), FS(f, 1.6f), mc);
        break;
    case AGENT_STATE_ERROR:
        fig_line(f, ox - 6, my, ox - 3, my - 2, 1.6f, mc);
        fig_line(f, ox - 3, my - 2, ox, my, 1.6f, mc);
        fig_line(f, ox, my, ox + 3, my - 2, 1.6f, mc);
        fig_line(f, ox + 3, my - 2, ox + 6, my, 1.6f, mc);
        break;
    default:
        fig_line(f, ox - 3, my, ox + 3, my, 1.6f, mc);
        break;
    }

    /* effects above the head */
    switch (st) {
    case AGENT_STATE_WORKING: {
        /* thought bubble with a typing indicator */
        fig_circle(f, 24, 28 + bob, 2, C_TEXT);
        fig_circle(f, 29, 22 + bob, 3, C_TEXT);
        fig_rrect(f, 22, 0 + bob, 34, 17, 8, C_TEXT);
        for (int i = 0; i < 3; i++) {
            float up = sinf(ph * TWO_PI * 1.5f - i * 0.9f);
            if (up < 0) up = 0;
            fig_circle(f, 30 + i * 9, 8.5f + bob - 2.5f * up, 2.6f, gfx_mix(acc, C_FACE, (int)(120 - up * 120)));
        }
        break;
    }
    case AGENT_STATE_WAITING: {
        float b = 2 * sinf(ph * TWO_PI / 0.8f);
        draw_bubble(f, 40, 10 + b + lift, 30, 22, 26, 30 + lift, acc, (t % 2000) < 1000 ? "?" : "!", C_FACE);
        break;
    }
    case AGENT_STATE_ERROR:
        draw_bubble(f, 40, 10, 26, 20, 26, 30, acc, "!", C_FACE);
        break;
    case AGENT_STATE_OFFLINE:
        for (int i = 0; i < 3; i++) {
            float p = fmodf(ph / 3.0f + i / 3.0f, 1.0f); /* 0..1 rising */
            int sc = (int)(f->k * (1.0f + p * 1.2f));
            if (sc < 1) sc = 1;
            color_t zc = gfx_mix(C_MUTED, C_BG_TOP, (int)(p * 230));
            gfx_text(FX(f, 24 + p * 22), FY(f, 30 - p * 30), "z", sc, zc);
        }
        break;
    default:
        break;
    }
}

/* ---------- screens ---------- */

typedef struct {
    int hdr_h;
    fig_t fig;
    int info_x, info_y, info_w; /* centre column for state text */
} layout_t;

static int name_scale_for_width(int w) { return w >= 160 ? 3 : 2; }

static void compute_layout(layout_t *l, bool with_header)
{
    const bool landscape = DISP_W > DISP_H;
    l->hdr_h = with_header ? 14 + 7 * name_scale_for_width(DISP_W) + 8 : 0;
    const int big = DISP_W >= 160 ? 3 : 2;
    const int info_h = 7 * big + 6 + 12 + 12 + 14 + 12;
    int fx, fy, fw, fh;
    if (landscape) {
        fx = 0; fy = l->hdr_h; fw = DISP_W / 2; fh = DISP_H - l->hdr_h;
        l->info_x = DISP_W * 3 / 4;
        l->info_w = DISP_W / 2 - 8;
        l->info_y = l->hdr_h + (fh - info_h) / 2;
    } else {
        fx = 0; fy = l->hdr_h; fw = DISP_W; fh = DISP_H - l->hdr_h - info_h;
        l->info_x = DISP_W / 2;
        l->info_w = DISP_W - 8;
        l->info_y = fy + fh;
    }
    float kx = fw / FIG_UNITS_W, ky = (fh - 6) / FIG_UNITS_H;
    l->fig.k = kx < ky ? kx : ky;
    l->fig.cx = fx + fw / 2;
    l->fig.top = fy + (fh - (int)(FIG_UNITS_H * l->fig.k)) / 2 + 2;
}

static void draw_background(void)
{
    gfx_vgradient(0, 0, DISP_W, DISP_H, C_BG_TOP, C_BG_BOT);
}

static void draw_footer_net(int y)
{
    char buf[40];
    if (s_connected) snprintf(buf, sizeof(buf), "%s", s_ip);
    else snprintf(buf, sizeof(buf), "NO WI-FI");
    gfx_text_centered(DISP_W / 2, y, buf, 1, s_connected ? C_MUTED : state_color(AGENT_STATE_WORKING));
}

static void draw_normal(uint32_t t)
{
    layout_t l;
    const agents_snapshot_t *s = &s_snap;
    draw_background();

    if (s->count == 0 || s->displayed < 0) {
        compute_layout(&l, false);
        l.fig.top -= 20;
        draw_robot(&l.fig, AGENT_KIND_OTHER, s_connected ? AGENT_STATE_IDLE : AGENT_STATE_OFFLINE, t);
        int y = l.info_y - 10;
        if (s_connected) {
            y = text_wrapped(l.info_x, y, l.info_w, "No agents yet", DISP_W >= 160 ? 2 : 1, C_TEXT);
            y = text_wrapped(l.info_x, y + 4, l.info_w, "Add them at", 1, C_MUTED);
            char url[48];
            snprintf(url, sizeof(url), "http://%s", s_ip);
            text_wrapped(l.info_x, y + 2, l.info_w, url, 1, state_color(AGENT_STATE_WAITING));
        } else {
            y = text_wrapped(l.info_x, y, l.info_w, "Connecting...", DISP_W >= 160 ? 2 : 1, C_TEXT);
            text_wrapped(l.info_x, y + 4, l.info_w, wifi_mgr_sta_ssid(), 1, C_MUTED);
        }
        return;
    }

    const agent_t *a = &s->agents[s->displayed];
    compute_layout(&l, true);
    const color_t acc = state_color(a->state);

    /* header: kind + index, name */
    color_t body, shade;
    kind_colors(a->cfg.kind, &body, &shade);
    gfx_text(4, 4, kind_label(a->cfg.kind), 1, body);
    char idx[24];
    snprintf(idx, sizeof(idx), "%d/%d", s->displayed + 1, s->count);
    gfx_text(DISP_W - 4 - gfx_text_width(idx, 1), 4, idx, 1, C_MUTED);
    int ns = gfx_fit_scale(a->cfg.name, DISP_W - 8, name_scale_for_width(DISP_W));
    char name[AGENT_NAME_LEN + 4];
    fit_text(name, sizeof(name), a->cfg.name, DISP_W - 8, ns);
    gfx_text_centered(DISP_W / 2, 14 + (7 * (name_scale_for_width(DISP_W) - ns)) / 2, name, ns, C_TEXT);
    gfx_fill_rect(8, l.hdr_h - 4, DISP_W - 16, 1, gfx_mix(C_BG_TOP, acc, 140));

    /* figure */
    draw_robot(&l.fig, a->cfg.kind, a->state, t);

    /* state text */
    const int big = DISP_W >= 160 ? 3 : 2;
    int y = l.info_y;
    color_t lc = acc;
    if (a->state == AGENT_STATE_WAITING && (t % 1000) >= 700) lc = gfx_mix(acc, C_BG_BOT, 150);
    gfx_text_centered(l.info_x, y, state_label(a->state), big, lc);
    y += 7 * big + 6;
    char line[48];
    if (a->detail[0]) {
        fit_text(line, sizeof(line), a->detail, l.info_w, 1);
        gfx_text_centered(l.info_x, y, line, 1, C_TEXT);
    }
    y += 12;
    if (a->last_event_us) {
        char d[20];
        format_duration(d, sizeof(d), esp_timer_get_time() - a->since_us);
        snprintf(line, sizeof(line), "for %s", d);
    } else {
        snprintf(line, sizeof(line), "no events yet");
    }
    gfx_text_centered(l.info_x, y, line, 1, C_MUTED);
    y += 12;

    /* agent dots */
    const int r = 4, gap = 14;
    int x0 = l.info_x - (s->count - 1) * gap / 2;
    for (int i = 0; i < s->count; i++) {
        color_t c = state_color(s->agents[i].state);
        if (i == s->displayed) gfx_ring(x0 + i * gap, y + r + 1, r + 2, r, C_TEXT);
        gfx_fill_circle(x0 + i * gap, y + r + 1, i == s->displayed ? r - 1 : r - 1, c);
    }
    y += 14;
    draw_footer_net(DISP_H - 10 < y ? y : DISP_H - 10);
}

static void draw_boot(uint32_t t)
{
    draw_background();
    layout_t l;
    compute_layout(&l, false);
    const int big = DISP_W >= 160 ? 2 : 1;
    l.fig.k *= 0.75f;
    l.fig.top = 30;
    draw_robot(&l.fig, AGENT_KIND_CLAUDE, AGENT_STATE_IDLE, t);
    gfx_text_centered(DISP_W / 2, 8, "AGENT OBSERVER", 1, C_TEXT);

    int y = l.fig.top + (int)(FIG_UNITS_H * l.fig.k) + 8;
    int hold = s_boot_hold_pct;
    if (hold > 0) {
        y = text_wrapped(DISP_W / 2, y, DISP_W - 8, "Keep holding for Wi-Fi setup", big, state_color(AGENT_STATE_WAITING));
    } else {
        y = text_wrapped(DISP_W / 2, y, DISP_W - 8, "Hold button now for Wi-Fi setup", big, C_TEXT);
    }
    y += 6;
    const int bw = DISP_W - 24, bh = 8;
    gfx_fill_round_rect(12, y, bw, bh, 4, gfx_mix(C_BG_BOT, C_WHITE, 30));
    int pct = hold > 0 ? hold : 100 - s_boot_window_pct;
    color_t bc = hold > 0 ? state_color(AGENT_STATE_WAITING) : state_color(AGENT_STATE_IDLE);
    if (pct > 0) gfx_fill_round_rect(12, y, bw * pct / 100 < bh ? bh : bw * pct / 100, bh, 4, bc);
}

static void draw_setup(uint32_t t)
{
    draw_background();
    layout_t l;
    compute_layout(&l, false);
    l.fig.k *= 0.6f;
    l.fig.top = 26;
    draw_robot(&l.fig, AGENT_KIND_COPILOT, AGENT_STATE_WAITING, t);
    gfx_text_centered(DISP_W / 2, 6, "WI-FI SETUP", DISP_W >= 160 ? 2 : 1, state_color(AGENT_STATE_WAITING));

    int y = l.fig.top + (int)(FIG_UNITS_H * l.fig.k) + 6;
    const int w = DISP_W - 8;
    y = text_wrapped(DISP_W / 2, y, w, "1. Join network", 1, C_MUTED);
    y = text_wrapped(DISP_W / 2, y + 2, w, wifi_mgr_ap_ssid(), DISP_W >= 160 ? 2 : 1, C_TEXT);
    if (strlen(CONFIG_AO_AP_PASSWORD) >= 8) {
        char pw[80];
        snprintf(pw, sizeof(pw), "password: %s", CONFIG_AO_AP_PASSWORD);
        y = text_wrapped(DISP_W / 2, y + 2, w, pw, 1, C_MUTED);
    }
    y = text_wrapped(DISP_W / 2, y + 8, w, "2. Open", 1, C_MUTED);
    text_wrapped(DISP_W / 2, y + 2, w, "192.168.4.1", DISP_W >= 160 ? 2 : 1, state_color(AGENT_STATE_IDLE));
}

static void draw_message(uint32_t t)
{
    draw_background();
    int y = DISP_H / 3;
    y = text_wrapped(DISP_W / 2, y, DISP_W - 8, s_msg_title, DISP_W >= 160 ? 2 : 1, C_TEXT);
    text_wrapped(DISP_W / 2, y + 8, DISP_W - 8, s_msg_body, 1, C_MUTED);
}

static void ui_task(void *arg)
{
    const int64_t t0 = esp_timer_get_time();
    TickType_t last = xTaskGetTickCount();
    bool first = true;
    for (;;) {
        uint32_t t = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        agents_snapshot(&s_snap);
        s_connected = wifi_mgr_connected();
        wifi_mgr_get_ip(s_ip, sizeof(s_ip));
        ui_mode_t mode = s_mode;

        for (int y0 = 0; y0 < DISP_H; y0 += DISP_BAND_LINES) {
            int lines = DISP_H - y0 < DISP_BAND_LINES ? DISP_H - y0 : DISP_BAND_LINES;
            uint16_t *buf = display_acquire_band();
            gfx_begin_band(buf, DISP_W, y0, lines);
            switch (mode) {
            case UI_MODE_BOOT:     draw_boot(t); break;
            case UI_MODE_SETUP_AP: draw_setup(t); break;
            case UI_MODE_MESSAGE:  draw_message(t); break;
            default:               draw_normal(t); break;
            }
            display_flush_band(buf, y0, lines);
        }
        if (first) {
            /* avoid showing panel garbage at power-up */
            vTaskDelay(pdMS_TO_TICKS(30));
            display_backlight(true);
            first = false;
        }
        /* keep the frame rate, but always yield a little so lower-priority tasks run */
        TickType_t spent = xTaskGetTickCount() - last;
        TickType_t frame = pdMS_TO_TICKS(FRAME_MS), min_sleep = pdMS_TO_TICKS(5);
        vTaskDelay(spent + min_sleep < frame ? frame - spent : min_sleep);
        last = xTaskGetTickCount();
    }
}

void ui_start(void)
{
    xTaskCreate(ui_task, "ui", 6144, NULL, 4, NULL);
}
