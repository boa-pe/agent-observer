/*
 * Agent Observer - shows the state of Claude Code / GitHub Copilot agents on a small LCD + RGB LED.
 *
 * Boot flow:
 *   1. Show splash. Holding the user button during the boot window enters Wi-Fi setup
 *      (on ESP32-C6/S3 the BOOT button is a strapping pin, so it must be pressed *after* power-on).
 *   2. Setup mode (button held, no credentials stored, or requested via web UI / long press):
 *      open access point + captive portal for entering SSID/password.
 *   3. Normal mode: connect to Wi-Fi, serve the agent management UI and hook API,
 *      animate the displayed agent; the button cycles through agents.
 *      Optionally checks a configured URL (e.g. a GitHub releases page) for new firmware (ota.c).
 *      When all agents have been offline for a while the display and LED switch off (power.c);
 *      an agent event or a button press switches them on again.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mdns.h"
#include "settings.h"
#include "agents.h"
#include "button.h"
#include "status_led.h"
#include "display.h"
#include "ui.h"
#include "wifi_mgr.h"
#include "dns_server.h"
#include "web_server.h"
#include "ota.h"
#include "power.h"

static const char *TAG = "main";

static bool s_edit_msg; /* the "editing enabled" message is on screen until the button is released */

static void on_button_normal(button_event_t ev)
{
    if (ev == BUTTON_EVENT_SHORT) {
        if (power_wake()) return; /* the press that switches the display on doesn't cycle */
        agents_cycle();
    } else if (ev == BUTTON_EVENT_EDIT) {
        power_wake();
        web_edit_enable();
        char body[96];
        snprintf(body, sizeof(body), "Web UI unlocked until logout or %d min idle. Keep holding for Wi-Fi setup.",
                 CONFIG_AO_EDIT_TIMEOUT_MIN);
        ui_set_message("Editing enabled", body);
        s_edit_msg = true;
    } else if (ev == BUTTON_EVENT_HOLD_END) {
        if (s_edit_msg) ui_set_mode(UI_MODE_NORMAL);
        s_edit_msg = false;
    } else if (ev == BUTTON_EVENT_LONG) {
        ESP_LOGI(TAG, "long press: entering setup mode");
        settings_set_force_setup(true);
        ui_set_message("Rebooting", "Starting Wi-Fi setup mode");
        vTaskDelay(pdMS_TO_TICKS(1200));
        esp_restart();
    }
}

static void on_button_setup(button_event_t ev)
{
    if (ev == BUTTON_EVENT_LONG) {
        /* leave setup without changes (only useful if credentials exist) */
        settings_set_force_setup(false);
        esp_restart();
    }
}

/* Returns true if the user held the button long enough during the boot window. */
static bool boot_window_wants_setup(void)
{
    const int64_t start = esp_timer_get_time();
    const int64_t window = (int64_t)CONFIG_AO_SETUP_WINDOW_MS * 1000;
    const int64_t hold_needed = (int64_t)CONFIG_AO_SETUP_HOLD_MS * 1000;
    int64_t held_since = 0;
    status_led_set(LED_PATTERN_BREATHE, 0, 80, 255);
    for (;;) {
        int64_t now = esp_timer_get_time();
        bool pressed = button_is_pressed();
        if (pressed && !held_since) held_since = now;
        if (!pressed) held_since = 0;
        int hold_pct = held_since ? (int)((now - held_since) * 100 / hold_needed) : 0;
        if (hold_pct >= 100) return true;
        /* while the button is held, extend the window so the hold can complete */
        if (now - start >= window && !held_since) return false;
        int win_pct = (int)((now - start) * 100 / window);
        ui_set_boot_progress(win_pct > 100 ? 100 : win_pct, hold_pct);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) return;
    char host[SETTINGS_HOSTNAME_LEN];
    settings_get_hostname(host, sizeof(host));
    mdns_hostname_set(host);
    mdns_instance_name_set(host); /* distinct service names when several monitors share a network */
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

}

static void update_led(const agents_snapshot_t *s)
{
    if (power_in_standby()) {
        status_led_set(LED_PATTERN_OFF, 0, 0, 0);
        return;
    }
    if (!wifi_mgr_connected()) {
        status_led_set(LED_PATTERN_BREATHE, 0, 60, 255); /* blue: no network */
        return;
    }
    if (s->count == 0) {
        status_led_set(LED_PATTERN_BREATHE, 0, 160, 160); /* cyan: nothing configured */
        return;
    }
#if CONFIG_AO_LED_MODE_DISPLAYED
    agent_state_t st = s->displayed >= 0 ? s->agents[s->displayed].state : AGENT_STATE_OFFLINE;
#else
    agent_state_t st = agents_aggregate_state(s);
#endif
    switch (st) {
    case AGENT_STATE_IDLE:    status_led_set(LED_PATTERN_SOLID, 0, 255, 0); break;
    case AGENT_STATE_WORKING: status_led_set(LED_PATTERN_SOLID, 255, 0, 0); break;
    case AGENT_STATE_WAITING: status_led_set(LED_PATTERN_BLINK, 255, 200, 0); break; /* yellow */
    case AGENT_STATE_ERROR:   status_led_set(LED_PATTERN_FAST_BLINK, 255, 0, 0); break;
    default:                  status_led_set(LED_PATTERN_OFF, 0, 0, 0); break;
    }
}

static void run_setup_mode(void)
{
    ESP_LOGI(TAG, "entering Wi-Fi setup mode");
    ui_set_mode(UI_MODE_SETUP_AP);
    status_led_set(LED_PATTERN_BREATHE, 180, 0, 255);
    button_start_events(on_button_setup);
    wifi_mgr_start_ap();
    dns_server_start();
    web_server_start_setup();
    ota_mark_valid(); /* setup portal is up: this firmware is usable */
    for (;;) vTaskDelay(portMAX_DELAY);
}

static void run_normal_mode(const wifi_list_t *nets)
{
    ui_set_mode(UI_MODE_NORMAL);
    button_start_events(on_button_normal);
    wifi_mgr_start_sta(nets);
    start_mdns();
    web_server_start_main();
    ota_start(); /* confirms the image once Wi-Fi is up, then checks for updates periodically */
    static agents_snapshot_t snap;
    for (;;) {
        agents_tick();
        agents_snapshot(&snap);
        power_tick(&snap);
        update_led(&snap);
        power_wait(100); /* 1 s in standby; agent events and the button cut it short */
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(settings_init());
    power_init();
    agents_init();
    ota_init();

    status_led_init();
    button_init();
    ESP_ERROR_CHECK(display_init());
    ui_set_mode(UI_MODE_BOOT);
    ui_start();

    static wifi_list_t nets;
    bool have_creds = settings_get_wifi_list(&nets);
    bool force = settings_get_force_setup();

    bool setup = boot_window_wants_setup() || force || !have_creds;
    if (setup) {
        if (force) settings_set_force_setup(false); /* one-shot: next reboot returns to normal */
        run_setup_mode();
    } else {
        run_normal_mode(&nets);
    }
}
