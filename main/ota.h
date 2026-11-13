#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

/*
 * Firmware-over-the-air updates.
 *
 * The update source is a single URL, auto-detected as one of:
 *   - GitHub repository / releases page   https://github.com/<owner>/<repo>[/releases[/tag/<tag>]]
 *     (the latest release - or the given tag - is queried through the GitHub API and the
 *      matching .bin asset is downloaded)
 *   - JSON manifest (URL ends in .json)    {"version":"1.2.0","url":"https://.../app.bin"}
 *   - direct firmware binary (anything else, e.g. .../releases/latest/download/app.bin);
 *     the version is read from the image header before anything is written to flash.
 *
 * Dual-slot layout (ota_0 / ota_1): updates are written to the inactive slot, so a failed or
 * interrupted download never touches the running firmware. A new image boots in "pending verify"
 * state and is confirmed only after Wi-Fi has been up for CONFIRM_STABLE_S without a crash; any
 * reset before that makes the bootloader fall back to the previous slot. A version that failed
 * this way (or was rolled back manually) is remembered and not auto-installed again.
 */

#define OTA_URL_LEN   256
#define OTA_ASSET_LEN 48

typedef struct {
    char url[OTA_URL_LEN];
    char asset[OTA_ASSET_LEN];  /* optional: substring the release asset name must contain */
    bool auto_update;           /* periodically check and install updates automatically */
    uint16_t interval_h;        /* check interval in hours (1..168) */
} ota_config_t;

/* Load configuration. Call once at boot, after settings_init(). */
void ota_init(void);
/* Confirm the running image (cancels the pending rollback after an update). */
void ota_mark_valid(void);
/* Start the background checker (normal mode only). */
void ota_start(void);

void ota_get_config(ota_config_t *out);
esp_err_t ota_set_config(const ota_config_t *cfg);

/* Ask the background task to check now; with install=true an available update is installed. */
esp_err_t ota_request(bool install);

/* Switch back to the firmware in the other slot (marks the running one bad) and reboot.
 * ESP_ERR_NOT_FOUND if the other slot holds no usable image. */
esp_err_t ota_rollback(void);

/* {"config":{...},"status":{...}} */
cJSON *ota_to_json(void);
