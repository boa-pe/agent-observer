#include "ota.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "sdkconfig.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "ui.h"

static const char *TAG = "ota";

/* NVS keys (max 15 chars) */
#define K_URL   "ota_url"
#define K_ASSET "ota_asset"
#define K_AUTO  "ota_auto"
#define K_INT   "ota_int"
#define K_TAG   "ota_tag"   /* release id we last installed (or skipped as identical) */
#define K_IVER  "ota_iver"  /* image version that release contained */
#define K_BAD   "ota_bad"   /* image version that failed / was rolled back: never auto-installed again */

#define REQ_CHECK   (1u << 0)
#define REQ_INSTALL (1u << 1)
#define REQ_RESCHED (1u << 2)
#define REQ_ROLLBACK (1u << 3)

#define FIRST_CHECK_DELAY_S 60          /* after Wi-Fi comes up */
#define CONFIRM_STABLE_S    30          /* new image must run this long with Wi-Fi up before it is kept */
#define CONFIG_CHECK_DELAY_S 5          /* after the configuration was changed */
#define ERROR_RETRY_S       (30 * 60)
#define API_BODY_CAP        (40 * 1024) /* GitHub lists assets before the (long) release notes */
#define MANIFEST_CAP        2048
#define HTTP_RX_BUF         4096        /* GitHub asset redirects carry very long Location headers */
#define HTTP_TX_BUF         4096

typedef enum {
    OTA_IDLE, OTA_CHECKING, OTA_UP_TO_DATE, OTA_AVAILABLE, OTA_DOWNLOADING, OTA_REBOOTING, OTA_ERROR
} ota_state_t;
static const char *const STATE_NAMES[] = {
    "idle", "checking", "up_to_date", "available", "downloading", "rebooting", "error"
};

typedef struct {
    char version[64]; /* release tag / manifest version; "" = unknown until the image header is read */
    char url[768];    /* firmware binary */
} release_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static ota_config_t s_cfg;
static char s_bad[32];  /* see K_BAD */
static struct {
    ota_state_t state;
    int progress;
    char latest[64];
    char message[160];
    int64_t last_check_us; /* 0 = never */
    int64_t next_check_us; /* 0 = not scheduled */
} s_st;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static void set_status(ota_state_t st, int progress, const char *fmt, ...)
{
    char msg[sizeof(s_st.message)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    LOCK();
    s_st.state = st;
    s_st.progress = progress;
    strlcpy(s_st.message, msg, sizeof(s_st.message));
    UNLOCK();
    if (st == OTA_ERROR) ESP_LOGW(TAG, "%s", msg);
    else ESP_LOGI(TAG, "%s", msg);
}

static void set_latest(const char *v)
{
    LOCK();
    strlcpy(s_st.latest, v, sizeof(s_st.latest));
    UNLOCK();
}

static void schedule_in(int64_t seconds)
{
    LOCK();
    s_st.next_check_us = seconds < 0 ? 0 : esp_timer_get_time() + seconds * 1000000LL;
    UNLOCK();
}

/* ---------- string helpers ---------- */

static bool ci_contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return true;
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, n) == 0) return true;
    }
    return false;
}

/* Case-insensitive suffix test that ignores any ?query or #fragment. */
static bool url_has_ext(const char *url, const char *ext)
{
    size_t l = strcspn(url, "?#"), el = strlen(ext);
    return l >= el && strncasecmp(url + l - el, ext, el) == 0;
}

/* Does the asset name mention this chip as a whole word ("esp32" must not match "esp32c6")? */
static bool name_has_target(const char *name)
{
    const char *t = CONFIG_IDF_TARGET;
    size_t tl = strlen(t);
    for (const char *p = name; *p; p++) {
        if (strncasecmp(p, t, tl) != 0) continue;
        bool start_ok = p == name || !isalnum((unsigned char)p[-1]);
        bool end_ok = !isalnum((unsigned char)p[tl]);
        if (start_ok && end_ok) return true;
    }
    return false;
}

/* An application image, not one of the other build outputs often attached to releases. */
static bool asset_is_app_bin(const char *name)
{
    return url_has_ext(name, ".bin") && !ci_contains(name, "bootloader") && !ci_contains(name, "partition")
        && !ci_contains(name, "ota_data") && !ci_contains(name, "merged");
}

/* Strip a tag prefix so only the version remains: "v1.2.3", "release_1.2.3", "fw-1.2" -> "1.2.3" / "1.2".
 * Returns the first "<digits>.<digit>" run; strings without one are returned unchanged (minus a leading 'v'). */
static const char *ver_norm(const char *v)
{
    for (const char *p = v; *p; p++) {
        if (isdigit((unsigned char)*p) && (p == v || !isdigit((unsigned char)p[-1])) &&
            (p[strspn(p, "0123456789")] == '.') && isdigit((unsigned char)p[strspn(p, "0123456789") + 1]))
            return p;
    }
    return (*v == 'v' || *v == 'V') ? v + 1 : v;
}

/* >0 if a is newer than b, 0 if equal, <0 if older. Falls back to "different = newer" for non-semver. */
static int ver_cmp(const char *a, const char *b)
{
    int x[3] = { 0 }, y[3] = { 0 };
    a = ver_norm(a);
    b = ver_norm(b);
    if (sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]) >= 2 && sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]) >= 2) {
        for (int i = 0; i < 3; i++) {
            if (x[i] != y[i]) return x[i] > y[i] ? 1 : -1;
        }
        return 0;
    }
    return strcmp(a, b) == 0 ? 0 : 1;
}

/*
 * Minimal JSON string-field scanner over a (possibly truncated) document. Finds the next
 * "key": "value" at or after `from`, unescapes value into out. Returns the position after the
 * value, or NULL if not found. Values that do not fit are skipped.
 */
static const char *json_next_str(const char *from, const char *key, char *out, size_t len)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = from;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (isspace((unsigned char)*q)) q++;
        if (*q != ':') { p = q; continue; }
        q++;
        while (isspace((unsigned char)*q)) q++;
        if (*q != '"') { p = q; continue; } /* null / non-string */
        q++;
        size_t o = 0;
        bool overflow = false;
        while (*q && *q != '"') {
            char c = *q++;
            if (c == '\\' && *q) {
                c = *q++;
                if (c == 'u') { for (int i = 0; i < 4 && *q; i++) q++; c = '?'; }
                else if (c == 'n') c = '\n';
                else if (c == 't') c = '\t';
            }
            if (o + 1 < len) out[o++] = c;
            else overflow = true;
        }
        out[o] = 0;
        if (*q != '"') return NULL; /* truncated document */
        if (overflow) { p = q + 1; continue; }
        return q + 1;
    }
    return NULL;
}

/* github.com/<owner>/<repo>[/releases[/tag/<tag>]] -> GitHub API release URL */
static bool github_api_url(const char *url, char *api, size_t len)
{
    const char *p = url;
    if (!strncasecmp(p, "https://", 8)) p += 8;
    else if (!strncasecmp(p, "http://", 7)) p += 7;
    if (!strncasecmp(p, "www.", 4)) p += 4;
    if (strncasecmp(p, "github.com/", 11) != 0) return false;
    p += 11;
    char seg[5][64];
    int n = 0;
    while (*p && *p != '?' && *p != '#' && n < 5) {
        size_t l = strcspn(p, "/?#");
        if (l >= sizeof(seg[0])) return false;
        if (l) {
            memcpy(seg[n], p, l);
            seg[n][l] = 0;
            n++;
        }
        p += l;
        if (*p == '/') p++;
    }
    if (n < 2) return false;
    size_t rl = strlen(seg[1]);
    if (rl > 4 && !strcasecmp(seg[1] + rl - 4, ".git")) seg[1][rl - 4] = 0;
    if (n >= 5 && !strcmp(seg[2], "releases") && !strcmp(seg[3], "tag")) {
        snprintf(api, len, "https://api.github.com/repos/%s/%s/releases/tags/%s", seg[0], seg[1], seg[4]);
    } else {
        snprintf(api, len, "https://api.github.com/repos/%s/%s/releases/latest", seg[0], seg[1]);
    }
    return true;
}

/* ---------- HTTP ---------- */

static void http_cfg(esp_http_client_config_t *c, const char *url)
{
    memset(c, 0, sizeof(*c));
    c->url = url;
    c->crt_bundle_attach = esp_crt_bundle_attach;
    c->timeout_ms = 15000;
    c->buffer_size = HTTP_RX_BUF;
    c->buffer_size_tx = HTTP_TX_BUF;
    c->user_agent = "agent-observer-ota";
    c->keep_alive_enable = true;
    c->max_redirection_count = 5;
}

typedef struct {
    char *buf;
    size_t len, cap;
} body_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (e->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    int code = esp_http_client_get_status_code(e->client);
    if (code >= 300 && code < 400) return ESP_OK; /* body of a redirect */
    body_t *b = e->user_data;
    size_t n = e->data_len;
    if (b->len + n > b->cap - 1) n = b->cap - 1 - b->len;
    memcpy(b->buf + b->len, e->data, n);
    b->len += n;
    b->buf[b->len] = 0;
    return ESP_OK;
}

/* GET url into a heap buffer of at most cap bytes (truncated beyond that). Caller frees *out. */
static esp_err_t http_get(const char *url, size_t cap, bool github, char **out, char *err, size_t el)
{
    body_t b = { .buf = malloc(cap), .len = 0, .cap = cap };
    if (!b.buf) { snprintf(err, el, "out of memory"); return ESP_ERR_NO_MEM; }
    b.buf[0] = 0;
    esp_http_client_config_t c;
    http_cfg(&c, url);
    c.event_handler = http_evt;
    c.user_data = &b;
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) { free(b.buf); snprintf(err, el, "invalid URL"); return ESP_FAIL; }
    if (github) {
        esp_http_client_set_header(h, "Accept", "application/vnd.github+json");
        esp_http_client_set_header(h, "X-GitHub-Api-Version", "2022-11-28");
    }
    esp_err_t r = esp_http_client_perform(h);
    int code = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (r != ESP_OK) {
        snprintf(err, el, "request failed: %s", esp_err_to_name(r));
    } else if (code != 200) {
        if (github && code == 404) snprintf(err, el, "GitHub: repository or release not found (404)");
        else if (github && code == 403) snprintf(err, el, "GitHub: API rate limit hit (403), try later");
        else snprintf(err, el, "HTTP %d from server", code);
        r = ESP_FAIL;
    }
    if (r != ESP_OK) { free(b.buf); return r; }
    *out = b.buf;
    return ESP_OK;
}

/* ---------- source resolution ---------- */

static esp_err_t resolve_github(const char *api, const ota_config_t *cfg, release_t *rel, char *err, size_t el)
{
    char *body = NULL;
    esp_err_t r = http_get(api, API_BODY_CAP, true, &body, err, el);
    if (r != ESP_OK) return r;
    if (!json_next_str(body, "tag_name", rel->version, sizeof(rel->version))) {
        free(body);
        snprintf(err, el, "no release found");
        return ESP_FAIL;
    }
    static char url[sizeof(rel->url)]; /* only used from the OTA task */
    char first[sizeof(rel->url)] = "";
    int candidates = 0;
    bool picked = false;
    const char *p = body;
    while (!picked && (p = json_next_str(p, "browser_download_url", url, sizeof(url))) != NULL) {
        const char *name = strrchr(url, '/');
        name = name ? name + 1 : url;
        if (!asset_is_app_bin(name)) continue;
        if (cfg->asset[0]) {
            if (ci_contains(name, cfg->asset)) { strlcpy(rel->url, url, sizeof(rel->url)); picked = true; }
            continue;
        }
        if (name_has_target(name)) { strlcpy(rel->url, url, sizeof(rel->url)); picked = true; continue; }
        if (++candidates == 1) strlcpy(first, url, sizeof(first));
    }
    free(body);
    if (picked) return ESP_OK;
    if (!cfg->asset[0] && candidates == 1) {
        strlcpy(rel->url, first, sizeof(rel->url));
        return ESP_OK;
    }
    if (cfg->asset[0]) snprintf(err, el, "release %.32s has no .bin asset containing \"%.32s\"", rel->version, cfg->asset);
    else if (candidates == 0) snprintf(err, el, "release %.32s has no firmware .bin asset", rel->version);
    else snprintf(err, el, "release %.32s has several .bin assets; set the asset name filter", rel->version);
    return ESP_FAIL;
}

static esp_err_t resolve_manifest(const char *url, release_t *rel, char *err, size_t el)
{
    char *body = NULL;
    esp_err_t r = http_get(url, MANIFEST_CAP, false, &body, err, el);
    if (r != ESP_OK) return r;
    cJSON *j = cJSON_Parse(body);
    free(body);
    const cJSON *v = cJSON_GetObjectItem(j, "version");
    const cJSON *u = cJSON_GetObjectItem(j, "url");
    if (!cJSON_IsString(v) || !cJSON_IsString(u)) {
        cJSON_Delete(j);
        snprintf(err, el, "manifest must be {\"version\":\"..\",\"url\":\"..\"}");
        return ESP_FAIL;
    }
    strlcpy(rel->version, v->valuestring, sizeof(rel->version));
    strlcpy(rel->url, u->valuestring, sizeof(rel->url));
    cJSON_Delete(j);
    return ESP_OK;
}

static esp_err_t resolve(const ota_config_t *cfg, release_t *rel, char *err, size_t el)
{
    char api[256];
    if (url_has_ext(cfg->url, ".bin")) {
        strlcpy(rel->url, cfg->url, sizeof(rel->url)); /* version comes from the image header */
        return ESP_OK;
    }
    if (github_api_url(cfg->url, api, sizeof(api))) return resolve_github(api, cfg, rel, err, el);
    if (url_has_ext(cfg->url, ".json")) return resolve_manifest(cfg->url, rel, err, el);
    strlcpy(rel->url, cfg->url, sizeof(rel->url));
    return ESP_OK;
}

/* ---------- check / install ---------- */

static bool is_bad(const char *version)
{
    return s_bad[0] && version[0] && strcmp(ver_norm(version), ver_norm(s_bad)) == 0;
}

static void set_bad(const char *version)
{
    strlcpy(s_bad, version, sizeof(s_bad));
    settings_set_str(K_BAD, s_bad);
}

static bool running_pending_verify(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p && esp_ota_get_state_partition(p, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

static void remember_installed(const char *release_id, const char *image_version)
{
    settings_set_str(K_TAG, release_id);
    settings_set_str(K_IVER, image_version);
}

/* Returns true if the device is about to reboot into a new image. */
static bool fetch_image(const release_t *rel, bool install, bool manual, const esp_app_desc_t *run)
{
    esp_http_client_config_t hc;
    http_cfg(&hc, rel->url);
    esp_https_ota_config_t oc = { .http_config = &hc };
    esp_https_ota_handle_t h = NULL;

    esp_err_t e = esp_https_ota_begin(&oc, &h);
    if (e != ESP_OK) {
        set_status(OTA_ERROR, 0, "Download failed: %s", esp_err_to_name(e));
        return false;
    }
    esp_app_desc_t nd;
    if (esp_https_ota_get_img_desc(h, &nd) != ESP_OK) {
        esp_https_ota_abort(h);
        set_status(OTA_ERROR, 0, "Not a valid firmware image");
        return false;
    }
    if (strncmp(nd.project_name, run->project_name, sizeof(nd.project_name)) != 0) {
        esp_https_ota_abort(h);
        set_status(OTA_ERROR, 0, "Image is for project \"%.32s\", not \"%.32s\"", nd.project_name, run->project_name);
        return false;
    }
    const char *rel_id = rel->version[0] ? rel->version : nd.version;
    if (!rel->version[0]) set_latest(nd.version);

    bool same = strcmp(nd.version, run->version) == 0;
    if (same || (!rel->version[0] && ver_cmp(nd.version, run->version) < 0)) {
        esp_https_ota_abort(h);
        if (rel->version[0]) remember_installed(rel_id, run->version); /* don't re-download this release */
        if (same && rel->version[0] && strcmp(ver_norm(rel->version), ver_norm(nd.version)) != 0) {
            set_status(OTA_UP_TO_DATE, 0, "Release %s contains the running version %s", rel->version, nd.version);
        } else {
            set_status(OTA_UP_TO_DATE, 0, "Up to date (%s)", run->version);
        }
        return false;
    }
    if (!install || (!manual && is_bad(nd.version))) {
        esp_https_ota_abort(h);
        if (is_bad(nd.version)) {
            set_status(OTA_AVAILABLE, 0, "%s failed before and was rolled back; not installed automatically (use Update now to retry)", rel_id);
        } else {
            set_status(OTA_AVAILABLE, 0, "Update available: %s", rel_id);
        }
        return false;
    }

    ESP_LOGI(TAG, "installing %s (image %s) from %s", rel_id, nd.version, rel->url);
    set_status(OTA_DOWNLOADING, 0, "Downloading %s", rel_id);
    ui_set_message("Updating", "Downloading firmware...");
    int last_pct = -1;
    while ((e = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int total = esp_https_ota_get_image_size(h);
        int got = esp_https_ota_get_image_len_read(h);
        int pct = total > 0 ? (int)((int64_t)got * 100 / total) : 0;
        if (pct != last_pct) {
            last_pct = pct;
            set_status(OTA_DOWNLOADING, pct, "Downloading %s: %d%%", rel_id, pct);
            if (pct % 5 == 0) {
                char m[32];
                snprintf(m, sizeof(m), "Downloading %d%%", pct);
                ui_set_message("Updating", m);
            }
        }
    }
    if (e != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        set_status(OTA_ERROR, 0, "Download interrupted: %s", esp_err_to_name(e));
        ui_set_mode(UI_MODE_NORMAL);
        return false;
    }
    e = esp_https_ota_finish(h);
    if (e != ESP_OK) {
        if (e == ESP_ERR_OTA_VALIDATE_FAILED) set_status(OTA_ERROR, 0, "Image validation failed (wrong chip?)");
        else set_status(OTA_ERROR, 0, "Finishing update failed: %s", esp_err_to_name(e));
        ui_set_mode(UI_MODE_NORMAL);
        return false;
    }
    remember_installed(rel_id, nd.version);
    if (s_bad[0]) set_bad(""); /* explicit retry; a renewed failure is recorded again at boot */
    set_status(OTA_REBOOTING, 100, "Installed %s, rebooting", rel_id);
    char m[48];
    snprintf(m, sizeof(m), "Rebooting into %.32s", nd.version);
    ui_set_message("Updated!", m);
    return true;
}

static void do_check(bool install, bool manual)
{
    ota_config_t cfg;
    ota_get_config(&cfg);
    LOCK();
    s_st.last_check_us = esp_timer_get_time();
    UNLOCK();
    if (!cfg.url[0]) {
        set_status(OTA_IDLE, 0, "No update URL configured");
        return;
    }
    if (install && running_pending_verify()) {
        /* writing now would overwrite the only known-good fallback image */
        set_status(OTA_ERROR, 0, "Running firmware is still being verified; try again in a minute");
        return;
    }
    set_status(OTA_CHECKING, 0, "Checking for updates...");

    release_t *rel = calloc(1, sizeof(*rel));
    if (!rel) { set_status(OTA_ERROR, 0, "out of memory"); return; }
    char err[128] = "";
    if (resolve(&cfg, rel, err, sizeof(err)) != ESP_OK) {
        set_status(OTA_ERROR, 0, "%s", err);
        free(rel);
        return;
    }
    const esp_app_desc_t *run = esp_app_get_description();
    bool reboot = false;
    if (rel->version[0]) {
        set_latest(rel->version);
        char tag[sizeof(rel->version)], iver[32];
        settings_get_str(K_TAG, tag, sizeof(tag));
        settings_get_str(K_IVER, iver, sizeof(iver));
        bool already = !strcmp(tag, rel->version) && !strcmp(iver, run->version);
        if (already || ver_cmp(rel->version, run->version) <= 0) {
            set_status(OTA_UP_TO_DATE, 0, "Up to date (%s)", run->version);
        } else if (is_bad(rel->version) && (!install || !manual)) {
            set_status(OTA_AVAILABLE, 0, "%s failed before and was rolled back; not installed automatically (use Update now to retry)", rel->version);
        } else if (!install) {
            set_status(OTA_AVAILABLE, 0, "Update available: %s", rel->version);
        } else {
            reboot = fetch_image(rel, true, manual, run);
        }
    } else {
        /* direct binary: the version is only known from the image header */
        reboot = fetch_image(rel, install, manual, run);
    }
    free(rel);
    if (reboot) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
}

static void ota_task(void *arg)
{
    bool was_connected = false;
    int64_t conn_since = 0;
    bool confirmed = !running_pending_verify();
    for (;;) {
        uint32_t req = 0;
        xTaskNotifyWait(0, UINT32_MAX, &req, pdMS_TO_TICKS(5000));
        ota_config_t cfg;
        ota_get_config(&cfg);
        bool periodic = cfg.auto_update && cfg.url[0];

        if (req & REQ_ROLLBACK) {
            const esp_app_desc_t *run = esp_app_get_description();
            ESP_LOGW(TAG, "manual rollback requested, leaving %s", run->version);
            set_bad(run->version); /* don't let auto-update reinstall it */
            set_status(OTA_REBOOTING, 0, "Rolling back to the previous firmware");
            ui_set_message("Rollback", "Restoring previous firmware");
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_err_t e = esp_ota_mark_app_invalid_rollback_and_reboot(); /* only returns on failure */
            set_status(OTA_ERROR, 0, "Rollback failed: %s", esp_err_to_name(e));
            ui_set_mode(UI_MODE_NORMAL);
            continue;
        }

        bool conn = wifi_mgr_connected();
        int64_t now = esp_timer_get_time();
        if (!conn) conn_since = 0;
        else if (!conn_since) conn_since = now;
        if (!confirmed && conn_since && now - conn_since >= CONFIRM_STABLE_S * 1000000LL) {
            ota_mark_valid(); /* network up and no crash for a while: keep this image */
            confirmed = true;
        }
        if (conn && !was_connected) {
            LOCK();
            bool scheduled = s_st.next_check_us != 0;
            UNLOCK();
            if (periodic && !scheduled) schedule_in(FIRST_CHECK_DELAY_S);
        }
        was_connected = conn;

        if (req & REQ_RESCHED) schedule_in(periodic ? CONFIG_CHECK_DELAY_S : -1);

        LOCK();
        int64_t next = s_st.next_check_us;
        UNLOCK();
        bool due = periodic && next && esp_timer_get_time() >= next;
        if (!(req & (REQ_CHECK | REQ_INSTALL)) && !due) continue;
        if (!conn) {
            set_status(OTA_ERROR, 0, "Wi-Fi not connected");
            if (due) schedule_in(FIRST_CHECK_DELAY_S);
            continue;
        }
        do_check((req & REQ_INSTALL) || due, (req & (REQ_CHECK | REQ_INSTALL)) != 0);
        if (periodic) {
            LOCK();
            bool failed = s_st.state == OTA_ERROR;
            UNLOCK();
            int64_t interval = (int64_t)cfg.interval_h * 3600;
            schedule_in(failed && interval > ERROR_RETRY_S ? ERROR_RETRY_S : interval);
        } else {
            schedule_in(-1);
        }
    }
}

/* ---------- public API ---------- */

void ota_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!settings_get_str(K_URL, s_cfg.url, sizeof(s_cfg.url)))
        strlcpy(s_cfg.url, CONFIG_AO_OTA_URL, sizeof(s_cfg.url)); /* never configured */
    if (!settings_get_str(K_ASSET, s_cfg.asset, sizeof(s_cfg.asset)))
        strlcpy(s_cfg.asset, CONFIG_AO_OTA_ASSET, sizeof(s_cfg.asset)); /* never configured */
    s_cfg.auto_update = settings_get_u32(K_AUTO, 1) != 0;
    uint32_t iv = settings_get_u32(K_INT, 24);
    s_cfg.interval_h = iv < 1 ? 1 : iv > 168 ? 168 : iv;
    s_st.state = OTA_IDLE;
    strlcpy(s_st.message, s_cfg.url[0] ? "Waiting for first check" : "No update URL configured", sizeof(s_st.message));

    const esp_app_desc_t *run = esp_app_get_description();
    const esp_partition_t *p = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "running %s from partition %s", run->version, p ? p->label : "?");

    /* Did the bootloader fall back because an update failed to confirm itself? */
    settings_get_str(K_BAD, s_bad, sizeof(s_bad));
    const esp_partition_t *inv = esp_ota_get_last_invalid_partition();
    esp_app_desc_t d;
    if (inv && esp_ota_get_partition_description(inv, &d) == ESP_OK && strcmp(d.version, run->version) != 0
        && strcmp(d.version, s_bad) != 0) {
        ESP_LOGW(TAG, "firmware %s in %s failed and was rolled back", d.version, inv->label);
        set_bad(d.version);
        s_st.state = OTA_ERROR;
        snprintf(s_st.message, sizeof(s_st.message), "Update to %.32s failed to start; rolled back to %.32s",
                 d.version, run->version);
    }
}

void ota_mark_valid(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (p && esp_ota_get_state_partition(p, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "new firmware confirmed, rollback cancelled");
        }
    }
}

void ota_start(void)
{
    if (s_task) return;
    xTaskCreate(ota_task, "ota", 8192, NULL, 4, &s_task);
}

void ota_get_config(ota_config_t *out)
{
    LOCK();
    *out = s_cfg;
    UNLOCK();
}

esp_err_t ota_set_config(const ota_config_t *cfg)
{
    ota_config_t c = *cfg;
    if (c.interval_h < 1) c.interval_h = 1;
    if (c.interval_h > 168) c.interval_h = 168;
    esp_err_t e = settings_set_str(K_URL, c.url);
    if (e == ESP_OK) e = settings_set_str(K_ASSET, c.asset);
    if (e == ESP_OK) e = settings_set_u32(K_AUTO, c.auto_update);
    if (e == ESP_OK) e = settings_set_u32(K_INT, c.interval_h);
    if (e != ESP_OK) return e;
    LOCK();
    bool url_changed = strcmp(s_cfg.url, c.url) != 0;
    s_cfg = c;
    if (url_changed) {
        s_st.latest[0] = 0;
        s_st.state = OTA_IDLE;
        strlcpy(s_st.message, c.url[0] ? "Waiting for first check" : "No update URL configured", sizeof(s_st.message));
    }
    UNLOCK();
    if (s_task) xTaskNotify(s_task, REQ_RESCHED, eSetBits);
    return ESP_OK;
}

esp_err_t ota_rollback(void)
{
    if (!s_task) return ESP_ERR_INVALID_STATE;
    if (!esp_ota_check_rollback_is_possible()) return ESP_ERR_NOT_FOUND;
    LOCK();
    bool busy = s_st.state == OTA_CHECKING || s_st.state == OTA_DOWNLOADING || s_st.state == OTA_REBOOTING;
    UNLOCK();
    if (busy) return ESP_ERR_INVALID_STATE;
    xTaskNotify(s_task, REQ_ROLLBACK, eSetBits);
    return ESP_OK;
}

static const char *img_state_name(const esp_partition_t *p)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(p, &st) != ESP_OK) return "valid"; /* flashed over USB, no OTA history */
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "verifying";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "failed";
    default:                         return "valid";
    }
}

esp_err_t ota_request(bool install)
{
    if (!s_task) return ESP_ERR_INVALID_STATE;
    LOCK();
    bool busy = s_st.state == OTA_CHECKING || s_st.state == OTA_DOWNLOADING || s_st.state == OTA_REBOOTING;
    bool have_url = s_cfg.url[0] != 0;
    UNLOCK();
    if (busy) return ESP_ERR_INVALID_STATE;
    if (!have_url) return ESP_ERR_INVALID_ARG;
    xTaskNotify(s_task, install ? REQ_INSTALL : REQ_CHECK, eSetBits);
    return ESP_OK;
}

cJSON *ota_to_json(void)
{
    const esp_app_desc_t *run = esp_app_get_description();
    const esp_partition_t *p = esp_ota_get_running_partition();
    int64_t now = esp_timer_get_time();
    cJSON *root = cJSON_CreateObject();
    cJSON *c = cJSON_AddObjectToObject(root, "config");
    cJSON *s = cJSON_AddObjectToObject(root, "status");
    LOCK();
    cJSON_AddStringToObject(c, "url", s_cfg.url);
    cJSON_AddStringToObject(c, "asset", s_cfg.asset);
    cJSON_AddBoolToObject(c, "auto_update", s_cfg.auto_update);
    cJSON_AddNumberToObject(c, "interval_h", s_cfg.interval_h);
    cJSON_AddStringToObject(s, "state", STATE_NAMES[s_st.state]);
    cJSON_AddNumberToObject(s, "progress", s_st.progress);
    cJSON_AddStringToObject(s, "message", s_st.message);
    cJSON_AddStringToObject(s, "latest", s_st.latest);
    cJSON_AddNumberToObject(s, "last_check_s", s_st.last_check_us ? (double)((now - s_st.last_check_us) / 1000000) : -1);
    cJSON_AddNumberToObject(s, "next_check_s",
                            s_st.next_check_us ? (double)((s_st.next_check_us > now ? s_st.next_check_us - now : 0) / 1000000) : -1);
    UNLOCK();
    cJSON_AddStringToObject(s, "current", run->version);
    cJSON_AddStringToObject(s, "project", run->project_name);
    cJSON_AddStringToObject(s, "target", CONFIG_IDF_TARGET);
    cJSON_AddStringToObject(s, "partition", p ? p->label : "?");
    cJSON_AddStringToObject(s, "bad", s_bad);
    cJSON_AddBoolToObject(s, "rollback_possible", esp_ota_check_rollback_is_possible());
    cJSON *slots = cJSON_AddArrayToObject(s, "slots");
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    for (esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
         it; it = esp_partition_next(it)) {
        const esp_partition_t *sp = esp_partition_get(it);
        esp_app_desc_t d;
        bool has = esp_ota_get_partition_description(sp, &d) == ESP_OK;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "label", sp->label);
        cJSON_AddStringToObject(o, "version", has ? d.version : "");
        cJSON_AddStringToObject(o, "state", has ? img_state_name(sp) : "empty");
        cJSON_AddBoolToObject(o, "running", p && sp->address == p->address);
        cJSON_AddBoolToObject(o, "boot", boot && sp->address == boot->address);
        cJSON_AddItemToArray(slots, o);
    }
    return root;
}
