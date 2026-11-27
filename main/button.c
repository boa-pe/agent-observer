#include "button.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_log.h"

#define POLL_MS     10
#define DEBOUNCE_MS 30

#if CONFIG_AO_BUTTON_ACTIVE_LOW
#define PRESSED_LEVEL_INTR GPIO_INTR_LOW_LEVEL
#else
#define PRESSED_LEVEL_INTR GPIO_INTR_HIGH_LEVEL
#endif

static const char *TAG = "button";
static button_cb_t s_cb;
static volatile bool s_pressed;
static TaskHandle_t s_task;

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

/* Level interrupt on the pressed level: disarm (it would keep firing) and let the task poll. */
static void on_button_isr(void *arg)
{
    gpio_intr_disable(CONFIG_AO_BUTTON_GPIO);
    if (!s_task) return; /* gpio_isr_handler_add() arms it before the task exists; the task re-arms */
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void button_task(void *arg)
{
    bool stable = raw_pressed();
    bool last_raw = stable;
    int64_t changed_at = esp_timer_get_time();
    int64_t pressed_at = 0;
    bool armed = false; /* press seen going down by this task; a press already held at start is ignored */
    bool edit_fired = false, long_fired = false;

    for (;;) {
        if (!stable && !last_raw) {
            /* released and settled: no polling (lets the CPU sleep) until the button goes down */
            gpio_intr_enable(CONFIG_AO_BUTTON_GPIO);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        } else {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        }
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
                armed = true;
                edit_fired = long_fired = false;
            } else if (armed) {
                armed = false;
                s_cb(edit_fired || long_fired ? BUTTON_EVENT_HOLD_END : BUTTON_EVENT_SHORT);
            }
        }
        if (stable && armed) {
#if CONFIG_AO_EDIT_HOLD_MS > 0
            if (!edit_fired && now - pressed_at >= (int64_t)CONFIG_AO_EDIT_HOLD_MS * 1000) {
                edit_fired = true;
                s_cb(BUTTON_EVENT_EDIT);
            }
#endif
#if CONFIG_AO_LONG_PRESS_SETUP_MS > 0
            if (!long_fired && now - pressed_at >= (int64_t)CONFIG_AO_LONG_PRESS_SETUP_MS * 1000) {
                long_fired = true;
                s_cb(BUTTON_EVENT_LONG);
            }
#endif
        }
    }
}

void button_start_events(button_cb_t cb)
{
    s_cb = cb;
    /* the interrupt stays disarmed until the task enables it */
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_LOGE(TAG, "isr service: %s", esp_err_to_name(err));
    gpio_intr_disable(CONFIG_AO_BUTTON_GPIO);
    gpio_set_intr_type(CONFIG_AO_BUTTON_GPIO, PRESSED_LEVEL_INTR);
    /* a press also wakes the CPU from automatic light sleep (same level as the interrupt) */
    gpio_wakeup_enable(CONFIG_AO_BUTTON_GPIO, PRESSED_LEVEL_INTR);
    esp_sleep_enable_gpio_wakeup();
    gpio_isr_handler_add(CONFIG_AO_BUTTON_GPIO, on_button_isr, NULL);
    xTaskCreate(button_task, "button", 3072, NULL, 5, &s_task);
}
