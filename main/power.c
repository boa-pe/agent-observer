#include "power.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_clk_tree.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "power";

#define STANDBY_KEY     "standby"
#define STANDBY_MAX_MIN 240
#define WAKE_GRACE_US   (60LL * 1000000)   /* after a manual wake, stay on at least this long */
#define STANDBY_TICK_MS 1000

static uint8_t s_timeout_min = CONFIG_AO_STANDBY_TIMEOUT_MIN;
static volatile bool s_standby;
static volatile bool s_wake_req;
static int64_t s_all_off_since;   /* 0 = some agent is not offline */
static int64_t s_hold_until;      /* no standby before this time (manual wake) */
static TaskHandle_t s_tick_task;

static uint8_t clamp_timeout(int minutes)
{
    return minutes < 0 ? 0 : minutes > STANDBY_MAX_MIN ? STANDBY_MAX_MIN : (uint8_t)minutes;
}

void power_init(void)
{
    s_timeout_min = clamp_timeout((int)settings_get_u32(STANDBY_KEY, CONFIG_AO_STANDBY_TIMEOUT_MIN));
#if CONFIG_AO_POWER_SAVE && CONFIG_PM_ENABLE
    uint32_t xtal_hz = 40 * 1000 * 1000;
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_XTAL, ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT, &xtal_hz);
    esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = (int)(xtal_hz / 1000000),
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        .light_sleep_enable = true,
#endif
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "power management: %d..%d MHz, light sleep %s", pm.min_freq_mhz, pm.max_freq_mhz,
                 pm.light_sleep_enable ? "on" : "off");
    } else {
        ESP_LOGW(TAG, "power management not enabled: %s", esp_err_to_name(err));
    }
#endif
    ESP_LOGI(TAG, "standby after %u min with all agents offline%s", s_timeout_min,
             s_timeout_min ? "" : " (disabled)");
}

void power_tick(const agents_snapshot_t *s)
{
    if (!s_tick_task) s_tick_task = xTaskGetCurrentTaskHandle();
    const int64_t now = esp_timer_get_time();

    if (s_wake_req) {
        s_wake_req = false;
        s_hold_until = now + WAKE_GRACE_US;
    }
    bool all_off = true;
    for (int i = 0; i < s->count && all_off; i++) all_off = s->agents[i].state == AGENT_STATE_OFFLINE;
    if (!all_off) s_all_off_since = 0;
    else if (!s_all_off_since) s_all_off_since = now;

    const bool want = s_timeout_min > 0 && all_off && now >= s_hold_until &&
                      now - s_all_off_since >= (int64_t)s_timeout_min * 60 * 1000000;
    if (want == s_standby) return;
    s_standby = want;
    if (want) {
        ESP_LOGI(TAG, "all agents offline for %u min: display off, standby", s_timeout_min);
    } else {
        ESP_LOGI(TAG, "leaving standby (%s)", all_off ? "user" : "agent active");
    }
    ui_wake(); /* the UI task switches the panel off / on */
}

void power_wait(int active_ms)
{
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_standby ? STANDBY_TICK_MS : active_ms));
}

void power_kick(void)
{
    TaskHandle_t t = s_tick_task;
    if (t) xTaskNotifyGive(t);
}

bool power_in_standby(void)
{
    return s_standby;
}

bool power_wake(void)
{
    bool was = s_standby;
    s_wake_req = true;
    power_kick();
    return was;
}

uint8_t power_get_standby_timeout(void)
{
    return s_timeout_min;
}

esp_err_t power_set_standby_timeout(int minutes)
{
    s_timeout_min = clamp_timeout(minutes);
    power_kick();
    if (settings_get_u32(STANDBY_KEY, CONFIG_AO_STANDBY_TIMEOUT_MIN) == s_timeout_min) return ESP_OK;
    esp_err_t err = settings_set_u32(STANDBY_KEY, s_timeout_min);
    if (err == ESP_OK) ESP_LOGI(TAG, "standby timeout %u min saved", s_timeout_min);
    return err;
}
