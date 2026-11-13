#include "button.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#define POLL_MS     10
#define DEBOUNCE_MS 30

static button_cb_t s_cb;
static volatile bool s_pressed;

static bool raw_pressed(void)
{
    int lvl = gpio_get_level(CONFIG_AO_BUTTON_GPIO);
#if CONFIG_AO_BUTTON_ACTIVE_LOW
    return lvl == 0;
#else
    return lvl != 0;
#endif
}

void button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_AO_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
#if CONFIG_AO_BUTTON_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#else
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    s_pressed = raw_pressed();
}

bool button_is_pressed(void)
{
    /* Before the event task runs, read the pin directly. */
    return s_cb ? s_pressed : raw_pressed();
}

static void button_task(void *arg)
{
    bool stable = raw_pressed();
    bool last_raw = stable;
    int64_t changed_at = esp_timer_get_time();
    int64_t pressed_at = stable ? changed_at : 0;
    bool long_fired = stable; /* ignore a press that was already held when we started */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        bool r = raw_pressed();
        int64_t now = esp_timer_get_time();
        if (r != last_raw) {
            last_raw = r;
            changed_at = now;
        }
        if (r != stable && now - changed_at >= DEBOUNCE_MS * 1000) {
            stable = r;
            s_pressed = stable;
            if (stable) {
                pressed_at = now;
                long_fired = false;
            } else if (!long_fired && pressed_at) {
                s_cb(BUTTON_EVENT_SHORT);
            }
        }
#if CONFIG_AO_LONG_PRESS_SETUP_MS > 0
        if (stable && !long_fired && now - pressed_at >= (int64_t)CONFIG_AO_LONG_PRESS_SETUP_MS * 1000) {
            long_fired = true;
            s_cb(BUTTON_EVENT_LONG);
        }
#endif
    }
}

void button_start_events(button_cb_t cb)
{
    s_cb = cb;
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);
}
