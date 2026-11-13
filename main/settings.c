#include "settings.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#define NS "ao"
static const char *TAG = "settings";

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static bool get_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t l = len;
    if (nvs_get_str(h, key, out, &l) != ESP_OK) {
        if (len) out[0] = 0;
        return false;
    }
    return true;
}

/* Pre-multi-network format: a single "ssid"/"pass" pair. Only read for migration. */
static bool settings_get_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    ssid[0] = 0;
    pass[0] = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = get_str(h, "ssid", ssid, ssid_len);
    get_str(h, "pass", pass, pass_len);
    nvs_close(h);
    return ok && ssid[0];
}

#define WIFI_KEY     "wifis"
#define WIFI_VERSION 1

typedef struct {
    uint8_t version;
    wifi_list_t list;
} wifi_blob_t;

bool settings_get_wifi_list(wifi_list_t *out)
{
    static wifi_blob_t b;
    size_t len = sizeof(b);
    memset(out, 0, sizeof(*out));
    out->preferred = -1;
    memset(&b, 0, sizeof(b));
    if (settings_load_blob(WIFI_KEY, &b, &len) == ESP_OK && b.version == WIFI_VERSION) {
        *out = b.list;
        if (out->count > WIFI_MAX_NETS) out->count = WIFI_MAX_NETS;
        if (out->preferred >= out->count) out->preferred = -1;
        for (int i = 0; i < out->count; i++) {
            out->net[i].ssid[sizeof(out->net[i].ssid) - 1] = 0;
            out->net[i].pass[sizeof(out->net[i].pass) - 1] = 0;
        }
        return out->count > 0;
    }
    /* migrate the single network stored by older firmware */
    if (settings_get_wifi(out->net[0].ssid, sizeof(out->net[0].ssid), out->net[0].pass, sizeof(out->net[0].pass))) {
        out->count = 1;
        ESP_LOGI(TAG, "migrated saved network '%s'", out->net[0].ssid);
    }
    return out->count > 0;
}

static esp_err_t save_wifi_list(const wifi_list_t *l)
{
    static wifi_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = WIFI_VERSION;
    b.list = *l;
    return settings_save_blob(WIFI_KEY, &b, sizeof(b));
}

static int wifi_find(const wifi_list_t *l, const char *ssid)
{
    for (int i = 0; i < l->count; i++) {
        if (strcmp(l->net[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

esp_err_t settings_wifi_upsert(const char *ssid, const char *pass, int preferred, wifi_list_t *out)
{
    static wifi_list_t l;
    settings_get_wifi_list(&l);
    int i = wifi_find(&l, ssid);
    if (i < 0) {
        if (l.count >= WIFI_MAX_NETS) return ESP_ERR_NO_MEM;
        i = l.count++;
        memset(&l.net[i], 0, sizeof(l.net[i]));
        strlcpy(l.net[i].ssid, ssid, sizeof(l.net[i].ssid));
    }
    if (pass) strlcpy(l.net[i].pass, pass, sizeof(l.net[i].pass));
    if (preferred == 1) l.preferred = i;
    else if (preferred == 0 && l.preferred == i) l.preferred = -1;
    esp_err_t err = save_wifi_list(&l);
    if (err == ESP_OK && out) *out = l;
    return err;
}

esp_err_t settings_wifi_remove(const char *ssid, wifi_list_t *out)
{
    static wifi_list_t l;
    settings_get_wifi_list(&l);
    int i = wifi_find(&l, ssid);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    memmove(&l.net[i], &l.net[i + 1], sizeof(wifi_net_t) * (l.count - i - 1));
    l.count--;
    memset(&l.net[l.count], 0, sizeof(wifi_net_t));
    if (l.preferred == i) l.preferred = -1;
    else if (l.preferred > i) l.preferred--;
    esp_err_t err = save_wifi_list(&l);
    if (err == ESP_OK && out) *out = l;
    return err;
}

bool settings_get_force_setup(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    nvs_get_u8(h, "force_ap", &v);
    nvs_close(h);
    return v != 0;
}

esp_err_t settings_set_force_setup(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "force_ap", on ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool settings_get_str(const char *key, char *out, size_t len)
{
    nvs_handle_t h;
    if (len) out[0] = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = get_str(h, key, out, len);
    nvs_close(h);
    return ok;
}

esp_err_t settings_set_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val ? val : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

uint32_t settings_get_u32(const char *key, uint32_t def)
{
    nvs_handle_t h;
    uint32_t v = def;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return def;
    if (nvs_get_u32(h, key, &v) != ESP_OK) v = def;
    nvs_close(h);
    return v;
}

esp_err_t settings_set_u32(const char *key, uint32_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_load_blob(const char *key, void *buf, size_t *len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(h, key, buf, len);
    nvs_close(h);
    return err;
}

esp_err_t settings_save_blob(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
