#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "settings.h"

/* Station mode: connect to the best saved network in range and keep reconnecting
 * (preferred network first, then strongest signal; networks not seen in the scan,
 * e.g. hidden ones, are tried last). */
esp_err_t wifi_mgr_start_sta(const wifi_list_t *nets);
/* Replace the saved-network list at runtime (no-op in setup mode). If disconnected, retries now. */
void wifi_mgr_set_networks(const wifi_list_t *nets);
/* Provisioning mode: open access point (APSTA so we can scan). */
esp_err_t wifi_mgr_start_ap(void);

bool wifi_mgr_connected(void);
void wifi_mgr_get_ip(char *buf, size_t len);
int wifi_mgr_rssi(void);
const char *wifi_mgr_ap_ssid(void);
/* SSID currently connected to (or being tried). */
const char *wifi_mgr_sta_ssid(void);

/* Scan for networks (blocking, ~2-3 s) and cache results. */
void wifi_mgr_scan(void);
/* Cached scan results as a JSON array: [{"ssid":"..","rssi":-50,"secure":true},...] (caller frees). */
char *wifi_mgr_scan_json(void);
