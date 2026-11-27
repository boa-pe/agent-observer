#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Web UI edit mode. The web UI starts read-only: every configuration change is refused (403) until
 * a button hold calls web_edit_enable(). Editing then stays allowed until web_edit_disable() (logout)
 * or until CONFIG_AO_EDIT_TIMEOUT_MIN minutes pass without web UI activity. Agent status (hooks)
 * is accepted in both modes. */
void web_edit_enable(void);
void web_edit_disable(void);
bool web_edit_active(void);

/* Normal operation: agent management UI + REST/hook API. */
esp_err_t web_server_start_main(void);
/* Provisioning: Wi-Fi setup page + captive portal redirects. */
esp_err_t web_server_start_setup(void);
