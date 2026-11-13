idf_component_register(
    SRCS "main.c" "settings.c" "agents.c" "button.c" "status_led.c"
         "display.c" "gfx.c" "font5x7.c" "ui.c"
         "wifi_mgr.c" "dns_server.c" "web_server.c" "ota.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html" "web/setup.html"
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server
                  esp_lcd esp_driver_gpio esp_driver_spi esp_driver_ledc
                  json lwip esp_timer esp_app_format
                  app_update esp_https_ota esp_http_client esp_partition mbedtls
)
