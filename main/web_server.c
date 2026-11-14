#include "web_server.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "cJSON.h"
#include "agents.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "ui.h"
#include "ota.h"
#include "display.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[] asm("_binary_setup_html_end");

#define MAX_BODY 4096

/* ---------- helpers ---------- */

/* Read up to max-1 bytes of the body into a NUL-terminated heap buffer; drains any excess. */
static char *read_body(httpd_req_t *req, size_t max)
{
    size_t want = req->content_len < max - 1 ? req->content_len : max - 1;
    char *buf = malloc(want + 1);
    if (!buf) return NULL;
    size_t got = 0;
    while (got < want) {
        int r = httpd_req_recv(req, buf + got, want - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(buf); return NULL; }
        got += r;
    }
    buf[got] = 0;
    /* drain oversized payloads (e.g. big tool outputs in PostToolUse hooks) */
    size_t remaining = req->content_len - got;
    char sink[256];
    while (remaining > 0) {
        int r = httpd_req_recv(req, sink, remaining < sizeof(sink) ? remaining : sizeof(sink));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        remaining -= r;
    }
    return buf;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    return send_json(req, o);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

static esp_err_t err_to_resp(httpd_req_t *req, esp_err_t err)
{
    switch (err) {
    case ESP_OK:                return send_ok(req);
    case ESP_ERR_NOT_FOUND:     return send_error(req, "404 Not Found", "unknown agent id");
    case ESP_ERR_INVALID_ARG:   return send_error(req, "400 Bad Request", "invalid arguments");
    case ESP_ERR_INVALID_STATE: return send_error(req, "409 Conflict", "agent id already exists");
    case ESP_ERR_NO_MEM:        return send_error(req, "409 Conflict", "agent limit reached");
    case ESP_ERR_NOT_SUPPORTED: return send_error(req, "400 Bad Request", "unknown event/state");
    default:                    return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
}

/* Extract the path segment after `prefix`, stopping at '/' or '?'. */
static bool path_id(const char *uri, const char *prefix, char *out, size_t len)
{
    size_t pl = strlen(prefix);
    if (strncmp(uri, prefix, pl) != 0) return false;
    const char *p = uri + pl;
    size_t n = strcspn(p, "/?");
    if (n == 0 || n >= len) return false;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

static bool query_param(httpd_req_t *req, const char *key, char *out, size_t len)
{
    size_t ql = httpd_req_get_url_query_len(req);
    if (ql == 0 || ql > 256) return false;
    char q[257];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, out, len) != ESP_OK) return false;
    /* httpd returns the raw value: decode %XX escapes and '+' in place */
    char *w = out;
    for (const char *r = out; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
    return true;
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void schedule_reboot(void) { xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL); }

/* ---------- normal mode handlers ---------- */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static cJSON *agent_to_json(const agent_t *a, bool displayed, int64_t now)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", a->cfg.id);
    cJSON_AddStringToObject(o, "name", a->cfg.name);
    cJSON_AddStringToObject(o, "kind", agent_kind_name(a->cfg.kind));
    cJSON_AddStringToObject(o, "state", agent_state_name(a->state));
    cJSON_AddStringToObject(o, "detail", a->detail);
    cJSON_AddNumberToObject(o, "state_age_s", (double)((now - a->since_us) / 1000000));
    cJSON_AddNumberToObject(o, "last_event_age_s", a->last_event_us ? (double)((now - a->last_event_us) / 1000000) : -1);
    cJSON_AddBoolToObject(o, "displayed", displayed);
    cJSON_AddBoolToObject(o, "restored", a->restored);
    return o;
}

static esp_err_t h_agents_get(httpd_req_t *req)
{
    static agents_snapshot_t snap; /* large; keep it off the stack (httpd is single-threaded) */
    agents_snapshot(&snap);
    int64_t now = esp_timer_get_time();
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "agents");
    for (int i = 0; i < snap.count; i++) {
        cJSON_AddItemToArray(arr, agent_to_json(&snap.agents[i], i == snap.displayed, now));
    }
    cJSON_AddNumberToObject(root, "max", AGENT_MAX);
    cJSON_AddNumberToObject(root, "offline_timeout_min", agents_get_offline_timeout());
    return send_json(req, root);
}

/* POST /api/agents/settings   body: {"offline_timeout_min":0..240}  (0 = never mark offline) */
static esp_err_t h_agents_settings(httpd_req_t *req)
{
    char *body = read_body(req, 128);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON");
    const cJSON *t = cJSON_GetObjectItem(j, "offline_timeout_min");
    bool ok = cJSON_IsNumber(t) && t->valuedouble >= 0 && t->valuedouble <= 240;
    int minutes = ok ? t->valueint : 0;
    cJSON_Delete(j);
    if (!ok) return send_error(req, "400 Bad Request", "offline_timeout_min must be 0..240");
    esp_err_t e = agents_set_offline_timeout(minutes);
    if (e != ESP_OK) return send_error(req, "500 Internal Server Error", esp_err_to_name(e));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "offline_timeout_min", agents_get_offline_timeout());
    return send_json(req, o);
}

static esp_err_t h_agents_post(httpd_req_t *req)
{
    char *body = read_body(req, 1024);
    if (!body) return send_error(req, "400 Bad Request", "no body");
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON");
    agent_cfg_t cfg = { 0 };
    const cJSON *id = cJSON_GetObjectItem(j, "id");
    const cJSON *name = cJSON_GetObjectItem(j, "name");
    const cJSON *kind = cJSON_GetObjectItem(j, "kind");
    agent_kind_t k = AGENT_KIND_CLAUDE;
    if (cJSON_IsString(id)) strlcpy(cfg.id, id->valuestring, sizeof(cfg.id));
    if (cJSON_IsString(name)) strlcpy(cfg.name, name->valuestring, sizeof(cfg.name));
    if (cJSON_IsString(kind)) agent_kind_from_name(kind->valuestring, &k);
    cfg.kind = k;
    bool id_too_long = cJSON_IsString(id) && strlen(id->valuestring) >= AGENT_ID_LEN;
    cJSON_Delete(j);
    if (id_too_long || !agent_id_valid(cfg.id))
        return send_error(req, "400 Bad Request", "id must be 1-15 chars: letters, digits, - or _");
    if (!cfg.name[0]) return send_error(req, "400 Bad Request", "name required");
    return err_to_resp(req, agents_add(&cfg));
}

/* PUT /api/agents/<id>   body: {"name":"..","kind":"..","move":-1|1} */
static esp_err_t h_agents_put(httpd_req_t *req)
{
    char id[AGENT_ID_LEN];
    if (!path_id(req->uri, "/api/agents/", id, sizeof(id))) return send_error(req, "400 Bad Request", "bad id");
    char *body = read_body(req, 1024);
    if (!body) return send_error(req, "400 Bad Request", "no body");
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON");
    const cJSON *name = cJSON_GetObjectItem(j, "name");
    const cJSON *kind = cJSON_GetObjectItem(j, "kind");
    const cJSON *move = cJSON_GetObjectItem(j, "move");
    esp_err_t err = ESP_OK;
    if (cJSON_IsNumber(move)) {
        err = agents_move(id, move->valueint < 0 ? -1 : 1);
    } else {
        agent_kind_t k;
        int ki = (cJSON_IsString(kind) && agent_kind_from_name(kind->valuestring, &k)) ? (int)k : -1;
        err = agents_update(id, cJSON_IsString(name) ? name->valuestring : NULL, ki);
    }
    cJSON_Delete(j);
    return err_to_resp(req, err);
}

static esp_err_t h_agents_delete(httpd_req_t *req)
{
    char id[AGENT_ID_LEN];
    if (!path_id(req->uri, "/api/agents/", id, sizeof(id))) return send_error(req, "400 Bad Request", "bad id");
    return err_to_resp(req, agents_remove(id));
}

/* POST /api/hook/<id>[?event=Name]   body: raw hook JSON from Claude Code / Copilot (optional) */
static esp_err_t h_hook(httpd_req_t *req)
{
    char id[AGENT_ID_LEN], ev[40] = "";
    if (!path_id(req->uri, "/api/hook/", id, sizeof(id))) return send_error(req, "400 Bad Request", "bad id");
    query_param(req, "event", ev, sizeof(ev));
    char *body = req->content_len ? read_body(req, MAX_BODY) : NULL;
    esp_err_t err = agents_handle_hook(id, ev, body ? body : "");
    free(body);
    if (err != ESP_OK) return err_to_resp(req, err);
    /* Empty 2xx on success: hook runners treat a response body as hook output (Claude Code validates the
       JSON of HTTP hooks and feeds command-hook stdout of SessionStart/UserPromptSubmit to the model). */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

/* POST /api/state/<id>   body {"state":"working","detail":"..."}  or  ?state=working */
static esp_err_t h_state(httpd_req_t *req)
{
    char id[AGENT_ID_LEN], st_s[20] = "", detail[AGENT_DETAIL_LEN] = "";
    if (!path_id(req->uri, "/api/state/", id, sizeof(id))) return send_error(req, "400 Bad Request", "bad id");
    query_param(req, "state", st_s, sizeof(st_s));
    query_param(req, "detail", detail, sizeof(detail));
    if (req->content_len) {
        char *body = read_body(req, 1024);
        cJSON *j = body ? cJSON_Parse(body) : NULL;
        free(body);
        if (j) {
            const cJSON *s = cJSON_GetObjectItem(j, "state");
            const cJSON *d = cJSON_GetObjectItem(j, "detail");
            if (cJSON_IsString(s)) strlcpy(st_s, s->valuestring, sizeof(st_s));
            if (cJSON_IsString(d)) strlcpy(detail, d->valuestring, sizeof(detail));
            cJSON_Delete(j);
        }
    }
    agent_state_t st;
    if (!agent_state_from_name(st_s, &st)) return send_error(req, "400 Bad Request", "unknown state");
    return err_to_resp(req, agents_set_state(id, st, detail));
}

static esp_err_t h_status(httpd_req_t *req)
{
    char ip[20];
    wifi_mgr_get_ip(ip, sizeof(ip));
    cJSON *o = cJSON_CreateObject();
    char host[SETTINGS_HOSTNAME_LEN];
    settings_get_hostname(host, sizeof(host));
    cJSON_AddStringToObject(o, "hostname", host);
    cJSON_AddNumberToObject(o, "hook_port", settings_get_hook_port());
    cJSON_AddStringToObject(o, "ip", ip);
    cJSON_AddStringToObject(o, "ssid", wifi_mgr_sta_ssid());
    cJSON_AddNumberToObject(o, "rssi", wifi_mgr_rssi());
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "free_heap", heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    cJSON_AddStringToObject(o, "version", esp_app_get_description()->version);
    return send_json(req, o);
}

static esp_err_t h_enter_setup(httpd_req_t *req)
{
    settings_set_force_setup(true);
    send_ok(req);
    ui_set_message("Rebooting", "Starting Wi-Fi setup mode");
    schedule_reboot();
    return ESP_OK;
}

/* ---------- network identity ---------- */

/* POST /api/network   body: {"hostname":"agent-observer-2","hook_port":8080}
 * Stores both and restarts the device, which then answers as <hostname>.local and accepts agent
 * status on port 80 and on hook_port. */
static esp_err_t h_network_set(httpd_req_t *req)
{
    char *body = read_body(req, 256);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON");
    const cJSON *hn = cJSON_GetObjectItem(j, "hostname");
    const cJSON *pt = cJSON_GetObjectItem(j, "hook_port");
    char host[SETTINGS_HOSTNAME_LEN] = "";
    if (cJSON_IsString(hn) && strlen(hn->valuestring) < sizeof(host)) strlcpy(host, hn->valuestring, sizeof(host));
    bool port_ok = cJSON_IsNumber(pt) && pt->valuedouble >= 1 && pt->valuedouble <= 65535 &&
                   pt->valuedouble == (double)pt->valueint;
    uint16_t port = port_ok ? (uint16_t)pt->valueint : 0;
    cJSON_Delete(j);
    if (!settings_hostname_valid(host))
        return send_error(req, "400 Bad Request", "hostname: 1-31 chars, lowercase letters, digits and '-' (not first or last)");
    if (!port_ok) return send_error(req, "400 Bad Request", "hook_port must be 1..65535");
    char cur[SETTINGS_HOSTNAME_LEN];
    settings_get_hostname(cur, sizeof(cur));
    if (!strcmp(cur, host) && settings_get_hook_port() == port) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "restart", false);
        return send_json(req, o);
    }
    esp_err_t e = settings_set_network(host, port);
    if (e != ESP_OK) return send_error(req, "500 Internal Server Error", esp_err_to_name(e));
    ESP_LOGI(TAG, "network identity saved: %s.local, hook port %u; restarting", host, port);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "restart", true);
    send_json(req, o);
    ui_set_message("Rebooting", "Applying new hostname and port");
    schedule_reboot();
    return ESP_OK;
}

static esp_err_t h_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------- display ---------- */

static cJSON *display_to_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "brightness", display_get_brightness());
    cJSON_AddBoolToObject(o, "adjustable", CONFIG_AO_LCD_PIN_BL >= 0);
    cJSON_AddNumberToObject(o, "dim_brightness", display_get_dim_level());
    cJSON_AddNumberToObject(o, "dim_timeout_min", display_get_dim_timeout());
    cJSON_AddBoolToObject(o, "dimmed", display_is_dimmed());
    cJSON_AddNumberToObject(o, "rotation", display_get_rotation());
    return o;
}

static esp_err_t h_display_get(httpd_req_t *req) { return send_json(req, display_to_json()); }

/* POST /api/display   body: {"brightness":5..100, "dim_brightness":5..100, "preview":false,
 *                            "dim_timeout_min":0..60, "rotation":0|180}
 * Fields are optional. preview=true applies brightness / dim_brightness without storing it (live
 * slider feedback; dim_brightness is then shown on the panel). dim_brightness: level used while
 * dimmed (capped at brightness). dim_timeout_min: minutes without an agent event before the
 * backlight drops to dim_brightness (0 = never); always stored. rotation: degrees, stored and applied at the next frame. */
static esp_err_t h_display_set(httpd_req_t *req)
{
    char *body = read_body(req, 256);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON");
    const cJSON *b = cJSON_GetObjectItem(j, "brightness");
    const cJSON *pv = cJSON_GetObjectItem(j, "preview");
    const cJSON *db = cJSON_GetObjectItem(j, "dim_brightness");
    const cJSON *dt = cJSON_GetObjectItem(j, "dim_timeout_min");
    const cJSON *rot = cJSON_GetObjectItem(j, "rotation");
    const char *err = NULL;
    if (b && (!cJSON_IsNumber(b) || b->valueint < 5 || b->valueint > 100)) err = "brightness must be 5..100";
    if (db && (!cJSON_IsNumber(db) || db->valueint < 5 || db->valueint > 100)) err = "dim_brightness must be 5..100";
    if (dt && (!cJSON_IsNumber(dt) || dt->valueint < 0 || dt->valueint > 60)) err = "dim_timeout_min must be 0..60";
    if (rot && (!cJSON_IsNumber(rot) || (rot->valueint != 0 && rot->valueint != 180))) err = "rotation must be 0 or 180";
    if (!b && !db && !dt && !rot && !err) err = "nothing to set";
    if (err) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", err);
    }
    bool persist = !(cJSON_IsBool(pv) && cJSON_IsTrue(pv));
    int pct = b ? b->valueint : -1, dim_pct = db ? db->valueint : -1, minutes = dt ? dt->valueint : -1, degrees = rot ? rot->valueint : -1;
    cJSON_Delete(j);
    esp_err_t e = ESP_OK;
    if (pct >= 0) e = display_set_brightness(pct, persist);
    if (e == ESP_OK && dim_pct >= 0) e = display_set_dim_level(dim_pct, persist);
    if (e == ESP_OK && minutes >= 0) e = display_set_dim_timeout(minutes);
    if (e == ESP_OK && degrees >= 0) e = display_set_rotation(degrees);
    if (e != ESP_OK) return send_error(req, "500 Internal Server Error", esp_err_to_name(e));
    return send_json(req, display_to_json());
}

/* ---------- firmware update ---------- */

static esp_err_t h_ota_get(httpd_req_t *req) { return send_json(req, ota_to_json()); }

/* POST /api/ota   body: {"url":"..","asset":"..","auto_update":true,"interval_h":24} */
static esp_err_t h_ota_config(httpd_req_t *req)
{
    char *body = read_body(req, 1024);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON");
    ota_config_t c;
    ota_get_config(&c);
    const cJSON *url = cJSON_GetObjectItem(j, "url");
    const cJSON *asset = cJSON_GetObjectItem(j, "asset");
    const cJSON *au = cJSON_GetObjectItem(j, "auto_update");
    const cJSON *iv = cJSON_GetObjectItem(j, "interval_h");
    const char *err = NULL;
    if (cJSON_IsString(url)) {
        const char *u = url->valuestring;
        while (*u == ' ') u++;
        if (strlen(u) >= sizeof(c.url)) err = "URL too long";
#if !CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP
        else if (*u && strncmp(u, "https://", 8) != 0) err = "URL must start with https://";
#else
        else if (*u && strncmp(u, "https://", 8) != 0 && strncmp(u, "http://", 7) != 0) err = "URL must start with http(s)://";
#endif
        else strlcpy(c.url, u, sizeof(c.url));
    }
    if (cJSON_IsString(asset)) {
        if (strlen(asset->valuestring) >= sizeof(c.asset)) err = "asset filter too long";
        else strlcpy(c.asset, asset->valuestring, sizeof(c.asset));
    }
    if (cJSON_IsBool(au)) c.auto_update = cJSON_IsTrue(au);
    if (cJSON_IsNumber(iv)) c.interval_h = iv->valueint < 1 ? 1 : iv->valueint > 168 ? 168 : iv->valueint;
    cJSON_Delete(j);
    if (err) return send_error(req, "400 Bad Request", err);
    esp_err_t e = ota_set_config(&c);
    if (e != ESP_OK) return send_error(req, "500 Internal Server Error", esp_err_to_name(e));
    return send_json(req, ota_to_json());
}

static esp_err_t ota_req_resp(httpd_req_t *req, bool install)
{
    esp_err_t e = ota_request(install);
    if (e == ESP_ERR_INVALID_ARG) return send_error(req, "400 Bad Request", "set an update URL first");
    if (e == ESP_ERR_INVALID_STATE) return send_error(req, "409 Conflict", "an update check is already running");
    return send_ok(req);
}

static esp_err_t h_ota_check(httpd_req_t *req) { return ota_req_resp(req, false); }
static esp_err_t h_ota_update(httpd_req_t *req) { return ota_req_resp(req, true); }

static esp_err_t h_ota_rollback(httpd_req_t *req)
{
    esp_err_t e = ota_rollback();
    if (e == ESP_ERR_NOT_FOUND) return send_error(req, "409 Conflict", "no usable firmware in the other slot");
    if (e == ESP_ERR_INVALID_STATE) return send_error(req, "409 Conflict", "an update is in progress");
    return send_ok(req);
}

/* ---------- setup mode handlers ---------- */

static esp_err_t h_setup_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, setup_html_start, setup_html_end - setup_html_start);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    char v[4];
    if (query_param(req, "refresh", v, sizeof(v))) wifi_mgr_scan();
    char *s = wifi_mgr_scan_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, s ? s : "[]");
    free(s);
    return ESP_OK;
}

/* ---------- saved Wi-Fi networks (both modes) ---------- */

static cJSON *wifi_list_json(void)
{
    static wifi_list_t l;
    settings_get_wifi_list(&l);
    const char *cur = wifi_mgr_connected() ? wifi_mgr_sta_ssid() : "";
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < l.count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", l.net[i].ssid);
        cJSON_AddBoolToObject(o, "secure", l.net[i].pass[0] != 0);
        cJSON_AddBoolToObject(o, "preferred", l.preferred == i);
        cJSON_AddBoolToObject(o, "connected", strcmp(cur, l.net[i].ssid) == 0);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(root, "max", WIFI_MAX_NETS);
    return root;
}

static esp_err_t h_wifi_list(httpd_req_t *req) { return send_json(req, wifi_list_json()); }

/* Validate and store {"ssid":"..","password":"..","preferred":bool}. Returns NULL on success. */
static const char *wifi_upsert_from_body(httpd_req_t *req, char *ssid_out, size_t ssid_len)
{
    char *body = read_body(req, 512);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!j) return "invalid JSON";
    const cJSON *ssid = cJSON_GetObjectItem(j, "ssid");
    const cJSON *pass = cJSON_GetObjectItem(j, "password");
    const cJSON *pref = cJSON_GetObjectItem(j, "preferred");
    const char *err = NULL;
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0] || strlen(ssid->valuestring) > 32) {
        err = "SSID required (max 32 chars)";
    } else if (cJSON_IsString(pass) && (strlen(pass->valuestring) > 63 ||
                                        (pass->valuestring[0] && strlen(pass->valuestring) < 8))) {
        err = "password must be 8-63 chars (or empty for open networks)";
    } else {
        /* password omitted -> keep the stored one (lets the UI toggle "preferred" only) */
        esp_err_t e = settings_wifi_upsert(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : NULL,
                                           cJSON_IsBool(pref) ? cJSON_IsTrue(pref) : -1, NULL);
        if (e == ESP_ERR_NO_MEM) err = "network list is full - delete one first";
        else if (e != ESP_OK) err = esp_err_to_name(e);
        else strlcpy(ssid_out, ssid->valuestring, ssid_len);
    }
    cJSON_Delete(j);
    return err;
}

static void wifi_apply(void)
{
    static wifi_list_t l;
    settings_get_wifi_list(&l);
    wifi_mgr_set_networks(&l);
}

/* POST /api/wifi/networks   (normal mode: takes effect without reboot) */
static esp_err_t h_wifi_add(httpd_req_t *req)
{
    char ssid[33];
    const char *err = wifi_upsert_from_body(req, ssid, sizeof(ssid));
    if (err) return send_error(req, "400 Bad Request", err);
    ESP_LOGI(TAG, "saved Wi-Fi network '%s'", ssid);
    wifi_apply();
    return send_json(req, wifi_list_json());
}

/* POST /api/wifi/networks/delete   body {"ssid":".."} */
static esp_err_t h_wifi_delete(httpd_req_t *req)
{
    char *body = read_body(req, 256);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    const cJSON *ssid = cJSON_GetObjectItem(j, "ssid");
    esp_err_t e = cJSON_IsString(ssid) ? settings_wifi_remove(ssid->valuestring, NULL) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j);
    if (e == ESP_ERR_NOT_FOUND) return send_error(req, "404 Not Found", "network not saved");
    if (e != ESP_OK) return send_error(req, "400 Bad Request", "ssid required");
    wifi_apply();
    return send_json(req, wifi_list_json());
}

/* Setup portal: POST /api/wifi   save a network and reboot to connect */
static esp_err_t h_wifi_save(httpd_req_t *req)
{
    char ssid[33];
    const char *err = wifi_upsert_from_body(req, ssid, sizeof(ssid));
    if (err) return send_error(req, "400 Bad Request", err);
    char msg[64];
    snprintf(msg, sizeof(msg), "Connecting to %s", ssid);
    settings_set_force_setup(false);
    send_ok(req);
    ESP_LOGI(TAG, "Wi-Fi network '%s' saved, rebooting", ssid);
    ui_set_message("Saved!", msg);
    schedule_reboot();
    return ESP_OK;
}

/* Setup portal: POST /api/restart   leave setup with the saved networks */
static esp_err_t h_restart(httpd_req_t *req)
{
    static wifi_list_t l;
    if (!settings_get_wifi_list(&l)) return send_error(req, "409 Conflict", "no saved networks");
    settings_set_force_setup(false);
    send_ok(req);
    ui_set_message("Restarting", "Connecting to saved Wi-Fi");
    schedule_reboot();
    return ESP_OK;
}

static esp_err_t h_captive_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------- start ---------- */

static httpd_handle_t start_server(int max_handlers, uint16_t port, uint16_t ctrl_port, int max_socks)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = max_handlers;
    cfg.server_port = port;
    cfg.ctrl_port = ctrl_port; /* each running httpd instance needs its own control port */
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = max_socks; /* LWIP_MAX_SOCKETS is 16; each server also holds 2 internal sockets */
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    httpd_handle_t h = NULL;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start on port %u failed", port);
        return NULL;
    }
    return h;
}

#define REG(h, u, m, fn) do { \
        httpd_uri_t _d = { .uri = (u), .method = (m), .handler = (fn) }; \
        httpd_register_uri_handler((h), &_d); \
    } while (0)

esp_err_t web_server_start_main(void)
{
    httpd_handle_t h = start_server(28, 80, 32768, 7);
    if (!h) return ESP_FAIL;
    REG(h, "/", HTTP_GET, h_index);
    REG(h, "/api/agents", HTTP_GET, h_agents_get);
    REG(h, "/api/agents", HTTP_POST, h_agents_post);
    REG(h, "/api/agents/settings", HTTP_POST, h_agents_settings);
    REG(h, "/api/agents/*", HTTP_PUT, h_agents_put);
    REG(h, "/api/agents/*", HTTP_DELETE, h_agents_delete);
    REG(h, "/api/hook/*", HTTP_POST, h_hook);
    REG(h, "/api/state/*", HTTP_POST, h_state);
    REG(h, "/api/status", HTTP_GET, h_status);
    REG(h, "/api/setup", HTTP_POST, h_enter_setup);
    REG(h, "/api/network", HTTP_POST, h_network_set);
    REG(h, "/api/wifi/networks", HTTP_GET, h_wifi_list);
    REG(h, "/api/wifi/networks", HTTP_POST, h_wifi_add);
    REG(h, "/api/wifi/networks/delete", HTTP_POST, h_wifi_delete);
    REG(h, "/api/display", HTTP_GET, h_display_get);
    REG(h, "/api/display", HTTP_POST, h_display_set);
    REG(h, "/api/ota", HTTP_GET, h_ota_get);
    REG(h, "/api/ota", HTTP_POST, h_ota_config);
    REG(h, "/api/ota/check", HTTP_POST, h_ota_check);
    REG(h, "/api/ota/update", HTTP_POST, h_ota_update);
    REG(h, "/api/ota/rollback", HTTP_POST, h_ota_rollback);
    REG(h, "/*", HTTP_OPTIONS, h_options);
    ESP_LOGI(TAG, "web UI ready");

    /* Agent status on a custom port: a small second server with only the hook-facing routes. */
    uint16_t hook_port = settings_get_hook_port();
    if (hook_port != 80) {
        /* only handlers without shared static buffers: this server runs in its own task */
        httpd_handle_t hk = start_server(4, hook_port, 32769, 3);
        if (hk) {
            REG(hk, "/api/hook/*", HTTP_POST, h_hook);
            REG(hk, "/api/state/*", HTTP_POST, h_state);
            REG(hk, "/*", HTTP_OPTIONS, h_options);
            ESP_LOGI(TAG, "agent status also accepted on port %u", hook_port);
        }
    }
    return ESP_OK;
}

esp_err_t web_server_start_setup(void)
{
    httpd_handle_t h = start_server(10, 80, 32768, 7);
    if (!h) return ESP_FAIL;
    REG(h, "/", HTTP_GET, h_setup_index);
    REG(h, "/api/scan", HTTP_GET, h_scan);
    REG(h, "/api/wifi", HTTP_POST, h_wifi_save);
    REG(h, "/api/wifi/networks", HTTP_GET, h_wifi_list);
    REG(h, "/api/wifi/networks/delete", HTTP_POST, h_wifi_delete);
    REG(h, "/api/restart", HTTP_POST, h_restart);
    REG(h, "/*", HTTP_GET, h_captive_redirect); /* captive portal: everything else -> setup page */
    ESP_LOGI(TAG, "setup portal ready");
    return ESP_OK;
}
