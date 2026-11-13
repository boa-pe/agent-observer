#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t settings_init(void);

/* Saved Wi-Fi networks (passwords never leave the device). */
#define WIFI_MAX_NETS 5

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_net_t;

typedef struct {
    uint8_t count;
    int8_t preferred;           /* index tried first when in range, -1 = none */
    wifi_net_t net[WIFI_MAX_NETS];
} wifi_list_t;

/* Loads the list (migrating the old single-network setting). Returns true if count > 0. */
bool settings_get_wifi_list(wifi_list_t *out);
/* Add a network or update an existing one with the same SSID.
 * pass == NULL keeps the stored password of an existing entry; preferred: 1 set, 0 clear, -1 keep.
 * ESP_ERR_NO_MEM when the list is full. *out (optional) receives the new list. */
esp_err_t settings_wifi_upsert(const char *ssid, const char *pass, int preferred, wifi_list_t *out);
/* ESP_ERR_NOT_FOUND if the SSID is not saved. */
esp_err_t settings_wifi_remove(const char *ssid, wifi_list_t *out);

bool settings_get_force_setup(void);
esp_err_t settings_set_force_setup(bool on);

/* Generic typed values (missing key -> default / empty string, returns false). */
bool settings_get_str(const char *key, char *out, size_t len);
esp_err_t settings_set_str(const char *key, const char *val);
uint32_t settings_get_u32(const char *key, uint32_t def);
esp_err_t settings_set_u32(const char *key, uint32_t val);

/* Raw blob storage for the agent list. */
esp_err_t settings_load_blob(const char *key, void *buf, size_t *len);
esp_err_t settings_save_blob(const char *key, const void *buf, size_t len);
