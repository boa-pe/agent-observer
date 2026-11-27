#pragma once
/*
 * Wall clock: time from an NTP server (SNTP, re-synchronized hourly) shown in a selectable
 * time zone. In standby the display can show the date and time (dimmed) instead of switching off.
 *
 * Settings (NVS, defaults from Kconfig): NTP server host name, time zone as a POSIX TZ string plus
 * the zone name picked in the web UI (only for showing the choice again), 24-hour format, and
 * whether standby shows the clock.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#define CLOCK_SERVER_LEN  64
#define CLOCK_TZ_LEN      64
#define CLOCK_TZNAME_LEN  48

typedef struct {
    char server[CLOCK_SERVER_LEN];   /* NTP host name or IP */
    char tz[CLOCK_TZ_LEN];           /* POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3" */
    char tz_name[CLOCK_TZNAME_LEN];  /* e.g. "Europe/Berlin", may be empty */
    bool h24;                        /* 24-hour format (else 12-hour with AM/PM) */
    bool standby_clock;              /* standby shows date and time instead of turning the display off */
} clock_cfg_t;

/* Load the settings and apply the time zone. Call early in app_main. */
void clock_init(void);
/* Start SNTP (after Wi-Fi station start; it retries until the network is up). */
void clock_start(void);

void clock_get_config(clock_cfg_t *out);
/* Validate, store and apply (a changed server restarts SNTP). ESP_ERR_INVALID_ARG on bad values. */
esp_err_t clock_set_config(const clock_cfg_t *cfg);
bool clock_server_valid(const char *s);
bool clock_tz_valid(const char *s);

/* True once the system time is plausible (synchronized now or before a software restart). */
bool clock_time_valid(void);
/* Local time; false if the time is not known yet. usec (optional) receives the microseconds
 * within the current second. */
bool clock_localtime(struct tm *out, int *usec);
/* Seconds since the last successful synchronization, -1 = none since boot. */
int64_t clock_since_sync_s(void);

bool clock_standby_enabled(void);
bool clock_24h(void);
