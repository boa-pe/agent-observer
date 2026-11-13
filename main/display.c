#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
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

void display_backlight(bool on)
{
#if CONFIG_AO_LCD_PIN_BL >= 0
#if CONFIG_AO_LCD_BL_ACTIVE_HIGH
    gpio_set_level(CONFIG_AO_LCD_PIN_BL, on ? 1 : 0);
#else
    gpio_set_level(CONFIG_AO_LCD_PIN_BL, on ? 0 : 1);
#endif
#else
    (void)on;
#endif
}

esp_err_t display_init(void)
{
#if CONFIG_AO_LCD_PIN_BL >= 0
    gpio_config_t bl = { .pin_bit_mask = 1ULL << CONFIG_AO_LCD_PIN_BL, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&bl);
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
