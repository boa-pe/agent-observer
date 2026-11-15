#pragma once
/*
 * Power management.
 *
 * Normal operation (CONFIG_AO_POWER_SAVE): dynamic CPU frequency scaling and automatic light
 * sleep between animation frames / Wi-Fi beacons; peripherals that must keep running while the
 * CPU sleeps (backlight PWM, discrete LED PWM) use POWER_LEDC_* below.
 *
 * Standby: when every agent has been offline for the standby timeout, the display and the LED
 * are switched off and only Wi-Fi (hooks, web UI) keeps running. Any agent leaving the offline
 * state, a button press or a display change from the web UI ends it.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_idf_version.h"
#include "sdkconfig.h"
#include "agents.h"

/* LEDC outputs that stay alive in light sleep need a clock that keeps running (RC_FAST) and
 * the per-channel sleep mode added in ESP-IDF 5.4. Without it, light sleep is blocked while
 * the backlight is on. */
#if CONFIG_AO_POWER_SAVE && CONFIG_PM_ENABLE && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
#define POWER_LEDC_KEEP_ALIVE 1
#define POWER_LEDC_CLK        LEDC_USE_RC_FAST_CLK
#else
#define POWER_LEDC_KEEP_ALIVE 0
#define POWER_LEDC_CLK        LEDC_AUTO_CLK
#endif

/* Configure DFS / automatic light sleep and load the standby timeout. Call early in app_main. */
void power_init(void);

/* Standby state machine; call periodically from one task (the main loop) with a fresh snapshot. */
void power_tick(const agents_snapshot_t *snap);
/* Sleep until the next power_tick is due: a short period normally, 1 s in standby;
 * returns early on power_kick(). Call from the task that runs power_tick. */
void power_wait(int active_ms);
/* Re-evaluate soon (e.g. an agent event arrived). Safe from any task. */
void power_kick(void);

/* True while the display is supposed to be off. */
bool power_in_standby(void);
/* User interaction (button, web UI display setting): leave standby and stay awake for at
 * least a minute even if all agents are still offline. Returns true if it was in standby. */
bool power_wake(void);

/* Minutes (0..240, 0 = never) all agents must be offline before standby. Persisted in NVS. */
esp_err_t power_set_standby_timeout(int minutes);
uint8_t power_get_standby_timeout(void);
