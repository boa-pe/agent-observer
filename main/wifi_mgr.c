#include "wifi_mgr.h"
#include "settings.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "cJSON.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

#define MAX_SCAN 20

static esp_netif_t *s_sta_netif;
static volatile bool s_connected;
static char s_ip[16] = "0.0.0.0";
static char s_ap_ssid[33];
static char s_sta_ssid[33];
static bool s_ap_mode;

/* station connection manager */
#define BIT_GOT_IP BIT0
#define BIT_DISC   BIT1
#define BIT_WAKE   BIT2
#define CONNECT_TIMEOUT_MS 15000
static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_nets_lock;
static wifi_list_t s_nets;

static SemaphoreHandle_t s_scan_lock;
static wifi_ap_record_t s_scan[MAX_SCAN];
static uint16_t s_scan_n;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (!s_ap_mode) {
            strcpy(s_ip, "0.0.0.0");
            wifi_event_sta_disconnected_t *d = data;
            ESP_LOGW(TAG, "'%s': disconnected (reason %d)", s_sta_ssid, d->reason);
            xEventGroupSetBits(s_ev, BIT_DISC);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "connected to '%s', IP %s", s_sta_ssid, s_ip);
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "client joined setup AP");
    }
}

static void common_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    s_scan_lock = xSemaphoreCreateMutex();
    s_nets_lock = xSemaphoreCreateMutex();
    s_ev = xEventGroupCreate();
}

/* One connection attempt; returns true once an IP address was obtained. */
static bool try_connect(const wifi_net_t *n)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, n->ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, n->pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = n->pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    strlcpy(s_sta_ssid, n->ssid, sizeof(s_sta_ssid));
    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_DISC);
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK || esp_wifi_connect() != ESP_OK) return false;
    ESP_LOGI(TAG, "trying '%s'", n->ssid);
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_DISC, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if (b & BIT_GOT_IP) return true;
    if (!(b & BIT_DISC)) {
        /* associated but no DHCP lease (or stuck): drop it before trying the next one */
        esp_wifi_disconnect();
        xEventGroupWaitBits(s_ev, BIT_DISC, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
    }
    return false;
}

/* Order saved networks: preferred (if in range), then by signal, then unseen ones (maybe hidden). */
static int candidate_order(const wifi_list_t *l, int *order)
{
    int rssi[WIFI_MAX_NETS];
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    for (int i = 0; i < l->count; i++) {
        rssi[i] = -1000;
        for (int k = 0; k < s_scan_n; k++) {
            if (strcmp((const char *)s_scan[k].ssid, l->net[i].ssid) == 0 && s_scan[k].rssi > rssi[i]) {
                rssi[i] = s_scan[k].rssi;
            }
        }
    }
    xSemaphoreGive(s_scan_lock);
    int n = 0;
    for (int i = 0; i < l->count; i++) order[n++] = i;
    /* insertion sort by (preferred && visible) desc, then rssi desc; stable keeps list order */
    for (int a = 1; a < n; a++) {
        int v = order[a], b = a - 1;
        int kv = (v == l->preferred && rssi[v] > -1000) ? 1000 : rssi[v];
        while (b >= 0) {
            int u = order[b];
            int ku = (u == l->preferred && rssi[u] > -1000) ? 1000 : rssi[u];
            if (ku >= kv) break;
            order[b + 1] = u;
            b--;
        }
        order[b + 1] = v;
    }
    return n;
}

static void sta_task(void *arg)
{
    static wifi_list_t nets;
    int backoff = 0;
    for (;;) {
        xSemaphoreTake(s_nets_lock, portMAX_DELAY);
        nets = s_nets;
        xSemaphoreGive(s_nets_lock);
        xEventGroupClearBits(s_ev, BIT_WAKE);
        if (nets.count == 0) {
            s_sta_ssid[0] = 0;
            ESP_LOGW(TAG, "no saved networks");
            xEventGroupWaitBits(s_ev, BIT_WAKE, pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }

        wifi_mgr_scan();
        int order[WIFI_MAX_NETS];
        int n = candidate_order(&nets, order);
        bool ok = false;
        for (int k = 0; k < n && !ok; k++) ok = try_connect(&nets.net[order[k]]);

        if (ok) {
            backoff = 0;
            xEventGroupWaitBits(s_ev, BIT_DISC, pdTRUE, pdFALSE, portMAX_DELAY); /* until the link drops */
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        /* nothing reachable: back off 1, 2, 4 ... 30 s (a list change retries immediately) */
        int delay_s = 1 << (backoff < 5 ? backoff : 5);
        if (delay_s > 30) delay_s = 30;
        backoff++;
        ESP_LOGW(TAG, "no saved network reachable, retry in %d s", delay_s);
        xEventGroupWaitBits(s_ev, BIT_WAKE, pdTRUE, pdFALSE, pdMS_TO_TICKS(delay_s * 1000));
    }
}

esp_err_t wifi_mgr_start_sta(const wifi_list_t *nets)
{
    common_init();
    s_nets = *nets;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    static char host[SETTINGS_HOSTNAME_LEN]; /* netif keeps the pointer */
    settings_get_hostname(host, sizeof(host));
    esp_netif_set_hostname(s_sta_netif, host);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Lower latency for incoming hook requests; costs a little power. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "%d saved network(s)", nets->count);
    xTaskCreate(sta_task, "wifi_sta", 4096, NULL, 5, NULL);
    return ESP_OK;
}

void wifi_mgr_set_networks(const wifi_list_t *nets)
{
    if (s_ap_mode || !s_nets_lock) return;
    xSemaphoreTake(s_nets_lock, portMAX_DELAY);
    s_nets = *nets;
    xSemaphoreGive(s_nets_lock);
    xEventGroupSetBits(s_ev, BIT_WAKE);
}

esp_err_t wifi_mgr_start_ap(void)
{
    s_ap_mode = true;
    common_init();
    esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X", CONFIG_AO_AP_SSID_PREFIX, mac[4], mac[5]);

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    if (strlen(CONFIG_AO_AP_PASSWORD) >= 8) {
        strlcpy((char *)ap.ap.password, CONFIG_AO_AP_PASSWORD, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    strcpy(s_ip, "192.168.4.1");
    ESP_LOGI(TAG, "setup AP '%s' started", s_ap_ssid);
    wifi_mgr_scan();
    return ESP_OK;
}

bool wifi_mgr_connected(void) { return s_connected; }

void wifi_mgr_get_ip(char *buf, size_t len) { strlcpy(buf, s_ip, len); }

int wifi_mgr_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

const char *wifi_mgr_ap_ssid(void) { return s_ap_ssid; }
const char *wifi_mgr_sta_ssid(void) { return s_sta_ssid; }

void wifi_mgr_scan(void)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed");
        return;
    }
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    uint16_t n = MAX_SCAN;
    if (esp_wifi_scan_get_ap_records(&n, s_scan) != ESP_OK) n = 0;
    s_scan_n = n;
    xSemaphoreGive(s_scan_lock);
    ESP_LOGI(TAG, "scan found %u networks", n);
}

char *wifi_mgr_scan_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    for (int i = 0; i < s_scan_n; i++) {
        const char *ssid = (const char *)s_scan[i].ssid;
        if (!ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp(ssid, (const char *)s_scan[j].ssid) == 0) { dup = true; break; }
        }
        if (dup) continue; /* records are sorted by RSSI; keep the strongest */
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", ssid);
        cJSON_AddNumberToObject(o, "rssi", s_scan[i].rssi);
        cJSON_AddBoolToObject(o, "secure", s_scan[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    xSemaphoreGive(s_scan_lock);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}
