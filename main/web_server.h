#pragma once
#include "esp_err.h"

/* Normal operation: agent management UI + REST/hook API. */
esp_err_t web_server_start_main(void);
/* Provisioning: Wi-Fi setup page + captive portal redirects. */
esp_err_t web_server_start_setup(void);
