#include "status_led.h"
#include <math.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

#if CONFIG_AO_LED_WS2812
#include "led_strip.h"
static led_strip_handle_t s_strip;
#elif CONFIG_AO_LED_GPIO_RGB
#include "driver/ledc.h"
#endif

static volatile led_pattern_t s_pattern = LED_PATTERN_OFF;
static volatile uint32_t s_rgb;

static void hw_write(uint8_t r, uint8_t g, uint8_t b)
{
    r = (uint16_t)r * CONFIG_AO_LED_BRIGHTNESS / 255;
    g = (uint16_t)g * CONFIG_AO_LED_BRIGHTNESS / 255;
    b = (uint16_t)b * CONFIG_AO_LED_BRIGHTNESS / 255;
#if CONFIG_AO_LED_WS2812
#if CONFIG_AO_LED_WS2812_ORDER_RGB
    /* the driver sends G,R,B; swapping makes the wire order R,G,B */
    led_strip_set_pixel(s_strip, 0, g, r, b);
#else
    led_strip_set_pixel(s_strip, 0, r, g, b);
#endif
    led_strip_refresh(s_strip);
#elif CONFIG_AO_LED_GPIO_RGB
    const uint8_t v[3] = { r, g, b };
    for (int ch = 0; ch < 3; ch++) {
        uint32_t duty = v[ch];
#if CONFIG_AO_LED_ACTIVE_LOW
        duty = 255 - duty;
#endif
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
    }
#else
    (void)r; (void)g; (void)b;
#endif
}

#if !CONFIG_AO_LED_NONE
static void led_task(void *arg)
{
    uint32_t last = 0xFFFFFFFF;
    for (;;) {
        uint32_t rgb = s_rgb;
        uint8_t r = rgb >> 16, g = rgb >> 8, b = rgb;
        int64_t ms = esp_timer_get_time() / 1000;
        float k = 1.0f;
        switch (s_pattern) {
        case LED_PATTERN_OFF:        k = 0; break;
        case LED_PATTERN_SOLID:      k = 1; break;
        case LED_PATTERN_BLINK:      k = (ms % 800) < 400 ? 1 : 0; break;
        case LED_PATTERN_FAST_BLINK: k = (ms % 250) < 125 ? 1 : 0; break;
        case LED_PATTERN_BREATHE:    k = 0.1f + 0.9f * (0.5f - 0.5f * cosf((ms % 2000) * 2 * (float)M_PI / 2000)); break;
        }
        uint8_t rr = r * k, gg = g * k, bb = b * k;
        uint32_t out = (rr << 16) | (gg << 8) | bb;
        if (out != last) {
            hw_write(rr, gg, bb);
            last = out;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
#endif

void status_led_init(void)
{
#if CONFIG_AO_LED_WS2812
    led_strip_config_t cfg = {
        .strip_gpio_num = CONFIG_AO_LED_WS2812_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB, /* RGB-order parts are handled in hw_write() */
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &s_strip));
    led_strip_clear(s_strip);
#elif CONFIG_AO_LED_GPIO_RGB
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    const int pins[3] = { CONFIG_AO_LED_PIN_R, CONFIG_AO_LED_PIN_G, CONFIG_AO_LED_PIN_B };
    for (int ch = 0; ch < 3; ch++) {
        ledc_channel_config_t c = {
            .gpio_num = pins[ch],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = ch,
            .timer_sel = LEDC_TIMER_1,
            .duty = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&c));
    }
#endif
    hw_write(0, 0, 0);
#if !CONFIG_AO_LED_NONE
    xTaskCreate(led_task, "led", 2560, NULL, 3, NULL);
#endif
}

void status_led_set(led_pattern_t p, uint8_t r, uint8_t g, uint8_t b)
{
    s_rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    s_pattern = p;
}
