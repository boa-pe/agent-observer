#include "agents.h"
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "settings.h"

static const char *TAG = "agents";

#define BLOB_KEY     "agents"
#define BLOB_VERSION 1

typedef struct {
    uint8_t version;
    uint8_t count;
    agent_cfg_t cfg[AGENT_MAX];
} agents_blob_t;

static SemaphoreHandle_t s_lock;
static agent_t s_agents[AGENT_MAX];
static int s_count;
static int s_displayed = -1;
static int64_t s_manual_until_us;
static uint32_t s_version;

/*
 * Last known states survive reboots and power cuts (hooks are push-only, the device cannot ask
 * agents for their state). Saved separately from the configuration, keyed by id, and only
 * when a state actually changed - coalesced over STATE_SAVE_DELAY_US to limit flash wear.
 */
#define STATE_KEY           "astate"
#define STATE_VERSION       1
#define STATE_SAVE_DELAY_US (10 * 1000000LL)

typedef struct {
    char id[AGENT_ID_LEN];
    uint8_t state;
} saved_state_t;

typedef struct {
    uint8_t version;
    uint8_t count;
    saved_state_t s[AGENT_MAX];
} states_blob_t;

static bool s_states_dirty;
static int64_t s_states_dirty_since;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static const char *k_state_names[AGENT_STATE_COUNT] = {
    "offline", "idle", "working", "waiting", "error"
};
static const char *k_kind_names[AGENT_KIND_COUNT] = { "claude", "copilot", "other" };

const char *agent_state_name(agent_state_t st)
{
    return st < AGENT_STATE_COUNT ? k_state_names[st] : "unknown";
}

bool agent_state_from_name(const char *s, agent_state_t *out)
{
    if (!s) return false;
    for (int i = 0; i < AGENT_STATE_COUNT; i++) {
        if (strcasecmp(s, k_state_names[i]) == 0) { *out = i; return true; }
    }
    /* friendly aliases */
    if (!strcasecmp(s, "busy") || !strcasecmp(s, "running") || !strcasecmp(s, "thinking")) { *out = AGENT_STATE_WORKING; return true; }
    if (!strcasecmp(s, "idling") || !strcasecmp(s, "ready") || !strcasecmp(s, "done")) { *out = AGENT_STATE_IDLE; return true; }
    if (!strcasecmp(s, "input") || !strcasecmp(s, "attention") || !strcasecmp(s, "permission")) { *out = AGENT_STATE_WAITING; return true; }
    return false;
}

const char *agent_kind_name(agent_kind_t k)
{
    return k < AGENT_KIND_COUNT ? k_kind_names[k] : "other";
}

bool agent_kind_from_name(const char *s, agent_kind_t *out)
{
    if (!s) return false;
    for (int i = 0; i < AGENT_KIND_COUNT; i++) {
        if (strcasecmp(s, k_kind_names[i]) == 0) { *out = i; return true; }
    }
    return false;
}

bool agent_id_valid(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n == 0 || n >= AGENT_ID_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
    }
    return true;
}

static int find_locked(const char *id)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_agents[i].cfg.id, id) == 0) return i;
    }
    return -1;
}

static esp_err_t persist_locked(void)
{
    static agents_blob_t blob; /* static: keep it off the httpd stack */
    memset(&blob, 0, sizeof(blob));
    blob.version = BLOB_VERSION;
    blob.count = s_count;
    for (int i = 0; i < s_count; i++) blob.cfg[i] = s_agents[i].cfg;
    size_t len = offsetof(agents_blob_t, cfg) + s_count * sizeof(agent_cfg_t);
    esp_err_t err = settings_save_blob(BLOB_KEY, &blob, len);
    if (err != ESP_OK) ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    return err;
}

static void mark_states_dirty_locked(void)
{
    if (!s_states_dirty) s_states_dirty_since = esp_timer_get_time();
    s_states_dirty = true;
}

static size_t build_states_locked(states_blob_t *b)
{
    memset(b, 0, sizeof(*b));
    b->version = STATE_VERSION;
    b->count = s_count;
    for (int i = 0; i < s_count; i++) {
        strlcpy(b->s[i].id, s_agents[i].cfg.id, AGENT_ID_LEN);
        b->s[i].state = s_agents[i].state;
    }
    return offsetof(states_blob_t, s) + s_count * sizeof(saved_state_t);
}

static void save_states(bool force)
{
    static states_blob_t blob, last;
    static size_t last_len;
    /* never block forever: this also runs from the shutdown handler */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    bool due = s_states_dirty && (force || esp_timer_get_time() - s_states_dirty_since >= STATE_SAVE_DELAY_US);
    size_t len = 0;
    if (due) {
        len = build_states_locked(&blob);
        s_states_dirty = false;
    }
    UNLOCK();
    if (!due || (len == last_len && memcmp(&blob, &last, len) == 0)) return; /* nothing new */
    esp_err_t err = settings_save_blob(STATE_KEY, &blob, len);
    if (err == ESP_OK) {
        last = blob;
        last_len = len;
    } else {
        ESP_LOGE(TAG, "saving states failed: %s", esp_err_to_name(err));
    }
}

void agents_flush_states(void) { save_states(true); }

static void restore_states(void)
{
    static states_blob_t b;
    size_t len = sizeof(b);
    memset(&b, 0, sizeof(b));
    if (settings_load_blob(STATE_KEY, &b, &len) != ESP_OK || b.version != STATE_VERSION) return;
    int64_t now = esp_timer_get_time();
    int n = b.count > AGENT_MAX ? AGENT_MAX : b.count;
    int restored = 0;
    for (int k = 0; k < n; k++) {
        b.s[k].id[AGENT_ID_LEN - 1] = 0;
        int i = find_locked(b.s[k].id);
        if (i < 0 || b.s[k].state >= AGENT_STATE_COUNT || b.s[k].state == AGENT_STATE_OFFLINE) continue;
        agent_t *a = &s_agents[i];
        a->state = b.s[k].state;
        a->since_us = now;
        a->last_event_us = now; /* lets the stale timeout expire it if the agent never reports */
        a->restored = true;
        strlcpy(a->detail, "restored", AGENT_DETAIL_LEN);
        if (a->state == AGENT_STATE_WAITING) s_displayed = i;
        restored++;
    }
    if (restored) ESP_LOGI(TAG, "restored %d agent state(s)", restored);
}

static void shutdown_handler(void) { save_states(true); }

esp_err_t agents_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    static agents_blob_t blob;
    size_t len = sizeof(blob);
    memset(&blob, 0, sizeof(blob));
    if (settings_load_blob(BLOB_KEY, &blob, &len) == ESP_OK && blob.version == BLOB_VERSION) {
        int n = blob.count > AGENT_MAX ? AGENT_MAX : blob.count;
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < n; i++) {
            memset(&s_agents[i], 0, sizeof(agent_t));
            s_agents[i].cfg = blob.cfg[i];
            s_agents[i].cfg.id[AGENT_ID_LEN - 1] = 0;
            s_agents[i].cfg.name[AGENT_NAME_LEN - 1] = 0;
            s_agents[i].state = AGENT_STATE_OFFLINE;
            s_agents[i].since_us = now;
        }
        s_count = n;
    }
    s_displayed = s_count ? 0 : -1;
    ESP_LOGI(TAG, "loaded %d agent(s)", s_count);
    restore_states();
    esp_register_shutdown_handler(shutdown_handler); /* OTA / setup / rollback reboots keep the latest state */
    return ESP_OK;
}

void agents_snapshot(agents_snapshot_t *out)
{
    LOCK();
    out->count = s_count;
    out->displayed = s_displayed;
    out->version = s_version;
    memcpy(out->agents, s_agents, sizeof(agent_t) * s_count);
    UNLOCK();
}

esp_err_t agents_add(const agent_cfg_t *cfg)
{
    if (!agent_id_valid(cfg->id) || !cfg->name[0]) return ESP_ERR_INVALID_ARG;
    LOCK();
    esp_err_t err = ESP_OK;
    if (find_locked(cfg->id) >= 0) {
        err = ESP_ERR_INVALID_STATE;
    } else if (s_count >= AGENT_MAX) {
        err = ESP_ERR_NO_MEM;
    } else {
        agent_t *a = &s_agents[s_count];
        memset(a, 0, sizeof(*a));
        a->cfg = *cfg;
        a->cfg.kind = cfg->kind < AGENT_KIND_COUNT ? cfg->kind : AGENT_KIND_OTHER;
        a->state = AGENT_STATE_OFFLINE;
        a->since_us = esp_timer_get_time();
        s_count++;
        if (s_displayed < 0) s_displayed = 0;
        s_version++;
        err = persist_locked();
    }
    UNLOCK();
    return err;
}

esp_err_t agents_update(const char *id, const char *name, int kind)
{
    LOCK();
    int i = find_locked(id);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (i >= 0) {
        if (name && name[0]) strlcpy(s_agents[i].cfg.name, name, AGENT_NAME_LEN);
        if (kind >= 0 && kind < AGENT_KIND_COUNT) s_agents[i].cfg.kind = kind;
        s_version++;
        err = persist_locked();
    }
    UNLOCK();
    return err;
}

esp_err_t agents_remove(const char *id)
{
    LOCK();
    int i = find_locked(id);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (i >= 0) {
        memmove(&s_agents[i], &s_agents[i + 1], sizeof(agent_t) * (s_count - i - 1));
        s_count--;
        if (s_count == 0) s_displayed = -1;
        else if (s_displayed > i || s_displayed >= s_count) s_displayed = (s_displayed - 1 + s_count) % s_count;
        s_version++;
        err = persist_locked();
    }
    UNLOCK();
    return err;
}

esp_err_t agents_move(const char *id, int delta)
{
    LOCK();
    int i = find_locked(id);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (i >= 0) {
        int j = i + delta;
        if (j < 0 || j >= s_count) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            agent_t tmp = s_agents[i];
            s_agents[i] = s_agents[j];
            s_agents[j] = tmp;
            if (s_displayed == i) s_displayed = j;
            else if (s_displayed == j) s_displayed = i;
            s_version++;
            err = persist_locked();
        }
    }
    UNLOCK();
    return err;
}

static void set_state_locked(int i, agent_state_t st, const char *detail)
{
    agent_t *a = &s_agents[i];
    int64_t now = esp_timer_get_time();
    if (a->state != st) {
        ESP_LOGI(TAG, "%s: %s -> %s", a->cfg.id, agent_state_name(a->state), agent_state_name(st));
        a->state = st;
        a->since_us = now;
        if (st == AGENT_STATE_WAITING) {
            /* An agent needing attention always grabs the display. */
            s_displayed = i;
            s_manual_until_us = 0;
        }
        mark_states_dirty_locked();
    }
    a->restored = false;
    a->last_event_us = now;
    if (detail) strlcpy(a->detail, detail, AGENT_DETAIL_LEN);
    s_version++;
}

esp_err_t agents_set_state(const char *id, agent_state_t st, const char *detail)
{
    LOCK();
    int i = find_locked(id);
    if (i >= 0) set_state_locked(i, st, detail);
    UNLOCK();
    return i >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* Minimal JSON string-field extractor: finds "key": "value" anywhere in body.
 * Good enough for hook payloads; avoids parsing potentially huge tool outputs. */
static bool json_find_str(const char *body, const char *key, char *out, size_t out_len)
{
    if (!body || !out_len) return false;
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = body;
    while ((p = strstr(p, pat)) != NULL) {
        p += strlen(pat);
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p != '"') continue;
        p++;
        size_t n = 0;
        while (*p && *p != '"' && n + 1 < out_len) {
            if (*p == '\\' && p[1]) p++;
            out[n++] = *p++;
        }
        out[n] = 0;
        return true;
    }
    return false;
}

static bool is_question_tool(const char *tool)
{
    static const char *tools[] = {
        "AskUserQuestion", "ExitPlanMode", "ask_user", "askQuestions", "ask_followup_question",
    };
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        if (strcasecmp(tool, tools[i]) == 0) return true;
    }
    return false;
}

esp_err_t agents_handle_hook(const char *id, const char *event, const char *body)
{
    char ev[40] = "", tool[48] = "", ntype[40] = "", msg[AGENT_DETAIL_LEN] = "";
    if (event && event[0]) strlcpy(ev, event, sizeof(ev));
    else json_find_str(body, "hook_event_name", ev, sizeof(ev));
    if (!json_find_str(body, "tool_name", tool, sizeof(tool)))
        json_find_str(body, "toolName", tool, sizeof(tool));
    json_find_str(body, "notification_type", ntype, sizeof(ntype));
    json_find_str(body, "message", msg, sizeof(msg));

    if (!ev[0]) return ESP_ERR_INVALID_ARG;

    agent_state_t st;
    const char *detail = NULL;
    char dbuf[AGENT_DETAIL_LEN];

    /* Event names: Claude Code / VS Code use PascalCase, Copilot CLI uses camelCase. */
    if (!strcasecmp(ev, "SessionStart")) {
        st = AGENT_STATE_IDLE; detail = "session started";
    } else if (!strcasecmp(ev, "SessionEnd")) {
        st = AGENT_STATE_OFFLINE; detail = "session ended";
    } else if (!strcasecmp(ev, "UserPromptSubmit") || !strcasecmp(ev, "UserPromptSubmitted")) {
        st = AGENT_STATE_WORKING; detail = "thinking";
    } else if (!strcasecmp(ev, "PreToolUse")) {
        if (tool[0] && is_question_tool(tool)) {
            st = AGENT_STATE_WAITING; detail = "question for you";
        } else {
            st = AGENT_STATE_WORKING;
            if (tool[0]) { snprintf(dbuf, sizeof(dbuf), "%s", tool); detail = dbuf; }
        }
    } else if (!strcasecmp(ev, "PostToolUse") || !strcasecmp(ev, "PostToolUseFailure") ||
               !strcasecmp(ev, "SubagentStart") || !strcasecmp(ev, "SubagentStop") ||
               !strcasecmp(ev, "PreCompact")) {
        st = AGENT_STATE_WORKING;
    } else if (!strcasecmp(ev, "PermissionRequest")) {
        st = AGENT_STATE_WAITING;
        snprintf(dbuf, sizeof(dbuf), "allow %s?", tool[0] ? tool : "tool");
        detail = dbuf;
    } else if (!strcasecmp(ev, "Notification")) {
        if (!strcasecmp(ntype, "idle_prompt") || strstr(msg, "waiting for your input")) {
            st = AGENT_STATE_IDLE;
        } else {
            st = AGENT_STATE_WAITING;
            detail = msg[0] ? msg : "needs attention";
        }
    } else if (!strcasecmp(ev, "Stop")) {
#if CONFIG_AO_STOP_IS_WAITING
        st = AGENT_STATE_WAITING; detail = "turn finished";
#else
        st = AGENT_STATE_IDLE; detail = "done";
#endif
    } else if (!strcasecmp(ev, "ErrorOccurred") || !strcasecmp(ev, "StopFailure")) {
        st = AGENT_STATE_ERROR; detail = "error";
    } else if (agent_state_from_name(ev, &st)) {
        /* allow ?event=working etc. */
    } else {
        ESP_LOGW(TAG, "unknown hook event '%s'", ev);
        return ESP_ERR_NOT_SUPPORTED;
    }

    LOCK();
    int i = find_locked(id);
    if (i >= 0) {
        /* Keep the waiting state when a tool runs only after the user answered;
         * PreToolUse for the question tool itself already put us in WAITING. */
        set_state_locked(i, st, detail ? detail : "");
    }
    UNLOCK();
    return i >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void agents_cycle(void)
{
    LOCK();
    if (s_count > 0) {
        s_displayed = (s_displayed + 1) % s_count;
        s_manual_until_us = esp_timer_get_time() + (int64_t)CONFIG_AO_MANUAL_OVERRIDE_S * 1000000;
        s_version++;
    }
    UNLOCK();
}

void agents_tick(void)
{
    int64_t now = esp_timer_get_time();
    LOCK();
#if CONFIG_AO_STALE_TIMEOUT_MIN > 0
    const int64_t stale = (int64_t)CONFIG_AO_STALE_TIMEOUT_MIN * 60 * 1000000;
    for (int i = 0; i < s_count; i++) {
        agent_t *a = &s_agents[i];
        if (a->state != AGENT_STATE_OFFLINE && a->last_event_us && now - a->last_event_us > stale) {
            ESP_LOGI(TAG, "%s: stale -> offline", a->cfg.id);
            a->state = AGENT_STATE_OFFLINE;
            a->since_us = now;
            strlcpy(a->detail, "no events", AGENT_DETAIL_LEN);
            a->restored = false;
            mark_states_dirty_locked();
            s_version++;
        }
    }
#endif
    /* Auto-focus: if anything waits for the user, show it (unless the user just pressed the button). */
    if (s_count > 0 && now >= s_manual_until_us &&
        (s_displayed < 0 || s_agents[s_displayed].state != AGENT_STATE_WAITING)) {
        int start = s_displayed < 0 ? 0 : s_displayed;
        for (int k = 1; k <= s_count; k++) {
            int i = (start + k) % s_count;
            if (s_agents[i].state == AGENT_STATE_WAITING) {
                s_displayed = i;
                s_version++;
                break;
            }
        }
    }
    UNLOCK();
    save_states(false);
}

agent_state_t agents_aggregate_state(const agents_snapshot_t *s)
{
    static const int prio[AGENT_STATE_COUNT] = {
        [AGENT_STATE_OFFLINE] = 0, [AGENT_STATE_IDLE] = 1, [AGENT_STATE_WORKING] = 2,
        [AGENT_STATE_ERROR] = 3, [AGENT_STATE_WAITING] = 4,
    };
    agent_state_t best = AGENT_STATE_OFFLINE;
    for (int i = 0; i < s->count; i++) {
        if (prio[s->agents[i].state] > prio[best]) best = s->agents[i].state;
    }
    return best;
}
