#include "clock.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "settings.h"
#include "sdkconfig.h"

static const char *TAG = "clock";

#define K_SERVER  "ntp_server"
#define K_TZ      "tz"
#define K_TZNAME  "tz_name"
#define K_H24     "clock_24h"
#define K_STANDBY "sb_clock"

#define VALID_EPOCH 1704067200   /* 2024-01-01: anything earlier means "never synchronized" */

static SemaphoreHandle_t s_lock;     /* config + TZ environment (tzset / localtime_r) */
static clock_cfg_t s_cfg;
static char s_sntp_server[CLOCK_SERVER_LEN]; /* SNTP keeps the pointer: only changed while stopped */
static bool s_started;
static volatile int64_t s_last_sync_us; /* 0 = none since boot */

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

bool clock_server_valid(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n >= CLOCK_SERVER_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_' || c == ':')) return false;
    }
    return true;
}

/* POSIX TZ: std name (letters or <+05>), offset, optional DST rules; no spaces. */
bool clock_tz_valid(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n < 2 || n >= CLOCK_TZ_LEN) return false;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '<')) return false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] <= ' ' || s[i] > '~') return false;
    }
    return true;
}

static bool tzname_valid(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n >= CLOCK_TZNAME_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < ' ' || s[i] > '~') return false;
    }
    return true;
}

static void apply_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

void clock_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    clock_cfg_t c = {0};
    if (!settings_get_str(K_SERVER, c.server, sizeof(c.server)) || !clock_server_valid(c.server))
        strlcpy(c.server, CONFIG_AO_NTP_SERVER, sizeof(c.server));
    if (!settings_get_str(K_TZ, c.tz, sizeof(c.tz)) || !clock_tz_valid(c.tz)) {
        strlcpy(c.tz, CONFIG_AO_TIMEZONE, sizeof(c.tz));
        strlcpy(c.tz_name, CONFIG_AO_TIMEZONE_NAME, sizeof(c.tz_name));
    } else {
        settings_get_str(K_TZNAME, c.tz_name, sizeof(c.tz_name));
    }
    c.h24 = settings_get_u32(K_H24, 1) != 0;
#if CONFIG_AO_STANDBY_CLOCK
    c.standby_clock = settings_get_u32(K_STANDBY, 1) != 0;
#else
    c.standby_clock = settings_get_u32(K_STANDBY, 0) != 0;
#endif
    s_cfg = c;
    apply_tz(c.tz);
    ESP_LOGI(TAG, "NTP %s, TZ %s (%s), standby %s", c.server, c.tz, c.tz_name[0] ? c.tz_name : "custom",
             c.standby_clock ? "shows the clock" : "turns the display off");
}

static void on_sync(struct timeval *tv)
{
    s_last_sync_us = esp_timer_get_time();
    ESP_LOGI(TAG, "time synchronized: %lld", (long long)tv->tv_sec);
}

/* caller holds the lock */
static void sntp_start_locked(void)
{
    strlcpy(s_sntp_server, s_cfg.server, sizeof(s_sntp_server));
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_sntp_server);
    cfg.sync_cb = on_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP start failed: %s", esp_err_to_name(err));
        return;
    }
    s_started = true;
    ESP_LOGI(TAG, "SNTP started with %s", s_sntp_server);
}

void clock_start(void)
{
    lock();
    if (!s_started) sntp_start_locked();
    unlock();
}

void clock_get_config(clock_cfg_t *out)
{
    lock();
    *out = s_cfg;
    unlock();
}

esp_err_t clock_set_config(const clock_cfg_t *in)
{
    if (!clock_server_valid(in->server) || !clock_tz_valid(in->tz) || !tzname_valid(in->tz_name))
        return ESP_ERR_INVALID_ARG;
    lock();
    const bool server_changed = strcmp(in->server, s_cfg.server) != 0;
    const bool tz_changed = strcmp(in->tz, s_cfg.tz) != 0;
    esp_err_t err = ESP_OK;
    if (server_changed) err = settings_set_str(K_SERVER, in->server);
    if (err == ESP_OK && tz_changed) err = settings_set_str(K_TZ, in->tz);
    if (err == ESP_OK && strcmp(in->tz_name, s_cfg.tz_name) != 0) err = settings_set_str(K_TZNAME, in->tz_name);
    if (err == ESP_OK && in->h24 != s_cfg.h24) err = settings_set_u32(K_H24, in->h24);
    if (err == ESP_OK && in->standby_clock != s_cfg.standby_clock) err = settings_set_u32(K_STANDBY, in->standby_clock);
    if (err == ESP_OK) {
        s_cfg = *in;
        if (tz_changed) apply_tz(s_cfg.tz);
        if (server_changed && s_started) {
            esp_netif_sntp_deinit();
            s_started = false;
            sntp_start_locked();
        }
        ESP_LOGI(TAG, "saved: NTP %s, TZ %s, %s, standby %s", s_cfg.server, s_cfg.tz, s_cfg.h24 ? "24h" : "12h",
                 s_cfg.standby_clock ? "clock" : "off");
    }
    unlock();
    return err;
}

bool clock_time_valid(void)
{
    return time(NULL) >= VALID_EPOCH;
}

bool clock_localtime(struct tm *out, int *usec)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    const time_t now = tv.tv_sec;
    if (usec) *usec = (int)tv.tv_usec;
    if (now < VALID_EPOCH) return false;
    lock();
    localtime_r(&now, out);
    unlock();
    return true;
}

int64_t clock_since_sync_s(void)
{
    int64_t t = s_last_sync_us;
    return t ? (esp_timer_get_time() - t) / 1000000 : -1;
}

bool clock_standby_enabled(void)
{
    return s_cfg.standby_clock;
}

bool clock_24h(void)
{
    return s_cfg.h24;
}
