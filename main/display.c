#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "settings.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "display";

#define LCD_HOST SPI2_HOST

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_free;   /* counts free band buffers */
static uint16_t *s_buf[2];
static int s_next;

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_free, &woken);
    return woken == pdTRUE;
}

/* Backlight: PWM via LEDC so the brightness can be adjusted at runtime. The level is a
 * percentage (BL_MIN..100) persisted in NVS; the duty follows a square curve so the slider
 * feels roughly linear to the eye. Timer 0 / channel 3 keep clear of the RGB status LED. */
#define BL_KEY      "bl"
#define BL_DEFAULT  100
#define BL_MIN      5
#define BL_TIMER    LEDC_TIMER_0
#define BL_CHANNEL  LEDC_CHANNEL_3
#define BL_RES      LEDC_TIMER_12_BIT
#define BL_MAX_DUTY ((1u << 12) - 1)

#define DIM_KEY     "bl_dim"
#define DIM_DEFAULT 0
#define DIM_MAX_MIN 60

static uint8_t s_brightness = BL_DEFAULT;
static bool s_bl_on;
/* Inactivity dimming: after s_dim_timeout_min minutes without an agent event the
 * backlight runs at half the configured level until the next event. 0 = disabled. */
static uint8_t s_dim_timeout_min = DIM_DEFAULT;
static bool s_dimmed;
static int64_t s_last_activity_us;

static uint8_t clamp_brightness(int pct)
{
    return pct < BL_MIN ? BL_MIN : pct > 100 ? 100 : (uint8_t)pct;
}

static void bl_apply(void)
{
#if CONFIG_AO_LCD_PIN_BL >= 0
    uint32_t duty = 0;
    if (s_bl_on) {
        uint32_t p = s_dimmed ? (s_brightness + 1) / 2 : s_brightness;
        duty = (BL_MAX_DUTY * p * p + 5000) / 10000;
        if (duty == 0) duty = 1;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
#endif
}

void display_backlight(bool on)
{
    s_bl_on = on;
    bl_apply();
}

uint8_t display_get_brightness(void)
{
    return s_brightness;
}

esp_err_t display_set_brightness(int percent, bool persist)
{
    s_brightness = clamp_brightness(percent);
    display_activity();   /* show the new level undimmed */
    if (!persist) return ESP_OK;
    if (settings_get_u32(BL_KEY, BL_DEFAULT) == s_brightness) return ESP_OK;
    esp_err_t err = settings_set_u32(BL_KEY, s_brightness);
    if (err == ESP_OK) ESP_LOGI(TAG, "brightness %u%% saved", s_brightness);
    return err;
}

uint8_t display_get_dim_timeout(void)
{
    return s_dim_timeout_min;
}

bool display_is_dimmed(void)
{
    return s_dimmed;
}

static uint8_t clamp_dim_timeout(int minutes)
{
    return minutes < 0 ? 0 : minutes > DIM_MAX_MIN ? DIM_MAX_MIN : (uint8_t)minutes;
}

esp_err_t display_set_dim_timeout(int minutes)
{
    s_dim_timeout_min = clamp_dim_timeout(minutes);
    display_activity();   /* restart the countdown from now */
    if (settings_get_u32(DIM_KEY, DIM_DEFAULT) == s_dim_timeout_min) return ESP_OK;
    esp_err_t err = settings_set_u32(DIM_KEY, s_dim_timeout_min);
    if (err == ESP_OK) ESP_LOGI(TAG, "dim timeout %u min saved", s_dim_timeout_min);
    return err;
}

void display_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (s_dimmed) {
        s_dimmed = false;
        bl_apply();
    }
}

void display_dim_tick(bool hold)
{
    if (hold) {
        /* something needs the user: stay at full level and restart the countdown, so dimming
         * can only begin dim_timeout_min after the last waiting agent was answered */
        display_activity();
        return;
    }
    if (s_dimmed || s_dim_timeout_min == 0) return;
    if (esp_timer_get_time() - s_last_activity_us >= (int64_t)s_dim_timeout_min * 60 * 1000000) {
        s_dimmed = true;
        ESP_LOGI(TAG, "no agent events for %u min, dimming backlight", s_dim_timeout_min);
        bl_apply();
    }
}

esp_err_t display_init(void)
{
    s_last_activity_us = esp_timer_get_time();
    s_dim_timeout_min = clamp_dim_timeout((int)settings_get_u32(DIM_KEY, DIM_DEFAULT));
    s_brightness = clamp_brightness((int)settings_get_u32(BL_KEY, BL_DEFAULT));
#if CONFIG_AO_LCD_PIN_BL >= 0
    ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_RES,
        .timer_num = BL_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&bl_timer), TAG, "backlight timer");
    ledc_channel_config_t bl_ch = {
        .gpio_num = CONFIG_AO_LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER,
        .duty = 0,
#if !CONFIG_AO_LCD_BL_ACTIVE_HIGH
        .flags.output_invert = 1,
#endif
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&bl_ch), TAG, "backlight channel");
    display_backlight(false);
#endif

    const size_t band_bytes = DISP_W * DISP_BAND_LINES * sizeof(uint16_t);
    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_AO_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = CONFIG_AO_LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = band_bytes,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    s_free = xSemaphoreCreateCounting(2, 2);
    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_malloc(band_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_buf[i]) return ESP_ERR_NO_MEM;
    }

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = CONFIG_AO_LCD_PIN_DC,
        .cs_gpio_num = CONFIG_AO_LCD_PIN_CS,
        .pclk_hz = CONFIG_AO_LCD_PCLK_MHZ * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_color_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = CONFIG_AO_LCD_PIN_RST,
#if CONFIG_AO_LCD_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
    /* To port to another controller (ILI9341, GC9A01, ...) add its esp_lcd driver
     * component and create the panel here; everything else is panel-agnostic. */
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pcfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
#if CONFIG_AO_LCD_INVERT_COLOR
    esp_lcd_panel_invert_color(s_panel, true);
#endif
#if CONFIG_AO_LCD_SWAP_XY
    esp_lcd_panel_swap_xy(s_panel, true);
#endif
#if CONFIG_AO_LCD_MIRROR_X || CONFIG_AO_LCD_MIRROR_Y
    esp_lcd_panel_mirror(s_panel, IS_ENABLED(CONFIG_AO_LCD_MIRROR_X), IS_ENABLED(CONFIG_AO_LCD_MIRROR_Y));
#endif
    esp_lcd_panel_set_gap(s_panel, CONFIG_AO_LCD_X_GAP, CONFIG_AO_LCD_Y_GAP);
    esp_lcd_panel_disp_on_off(s_panel, true);
    ESP_LOGI(TAG, "ST7789 %dx%d ready", DISP_W, DISP_H);
    return ESP_OK;
}

uint16_t *display_acquire_band(void)
{
    xSemaphoreTake(s_free, portMAX_DELAY);
    uint16_t *b = s_buf[s_next];
    s_next ^= 1;
    return b;
}

void display_flush_band(uint16_t *buf, int y0, int lines)
{
    esp_lcd_panel_draw_bitmap(s_panel, 0, y0, DISP_W, y0 + lines, buf);
}
