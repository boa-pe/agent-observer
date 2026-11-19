#include "agents.h"
#include "display.h"
#include "power.h"
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

/* Agent states are runtime-only: every agent starts offline after a reboot. Older firmware
 * saved them under this key; it is erased at boot to free the NVS space. */
#define LEGACY_STATE_KEY "astate"

/* Ring buffer of recent hook events (diagnostics, RAM only). Guarded by s_lock. */
static agent_event_t s_events[AGENT_EVENT_LOG];
static int s_event_head;   /* next slot to write */
static int s_event_count;

/* Offline timeout: an agent with no event for this many minutes is marked offline (0 = never).
 * Runtime setting stored in NVS; the Kconfig value is the default. */
#define OFFLINE_KEY     "off_min"
#define OFFLINE_MAX_MIN 240
static uint8_t s_offline_min;

static uint8_t clamp_offline(int minutes)
{
    return minutes < 0 ? 0 : minutes > OFFLINE_MAX_MIN ? OFFLINE_MAX_MIN : (uint8_t)minutes;
}

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static const char *k_state_names[AGENT_STATE_COUNT] = {
    "offline", "idle", "working", "waiting", "error"
};
static const char *k_kind_names[AGENT_KIND_COUNT] = { "claude", "copilot", "other", "grok" };

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

/* The displayed agent i just went offline: switch to the most urgent other agent that is online
 * (waiting > error > working > idle), searching from i onwards so ties go to the next one in list
 * order. Keeps the selection when all other agents are offline. */
static void focus_busy_locked(int i)
{
    if (i != s_displayed) return;
    static const int prio[AGENT_STATE_COUNT] = {
        [AGENT_STATE_OFFLINE] = 0, [AGENT_STATE_IDLE] = 1, [AGENT_STATE_WORKING] = 2,
        [AGENT_STATE_ERROR] = 3, [AGENT_STATE_WAITING] = 4,
    };
    int best = -1;
    for (int k = 1; k < s_count; k++) {
        int j = (i + k) % s_count;
        if (prio[s_agents[j].state] > (best < 0 ? 0 : prio[s_agents[best].state])) best = j;
    }
    if (best < 0) return;
    ESP_LOGI(TAG, "%s went offline, showing %s", s_agents[i].cfg.id, s_agents[best].cfg.id);
    s_displayed = best;
    s_manual_until_us = 0;
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

esp_err_t agents_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_offline_min = clamp_offline((int)settings_get_u32(OFFLINE_KEY, CONFIG_AO_STALE_TIMEOUT_MIN));
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
    settings_erase(LEGACY_STATE_KEY);
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
        } else if (st != AGENT_STATE_OFFLINE && power_in_standby() &&
                   (s_displayed < 0 || s_agents[s_displayed].state == AGENT_STATE_OFFLINE)) {
            /* This event wakes the display: show the agent that caused it. The display check keeps
             * the first one if several agents report before the standby flag is cleared. */
            s_displayed = i;
            s_manual_until_us = 0;
        } else if (st == AGENT_STATE_OFFLINE) {
            focus_busy_locked(i);
        }
    }
    a->last_event_us = now;
    if (detail) strlcpy(a->detail, detail, AGENT_DETAIL_LEN);
    s_version++;
    display_activity();
    power_kick(); /* leave standby right away if the agent came online */
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

static void log_event(const char *id, const char *ev, const char *tool, const char *sid, int state)
{
    LOCK();
    agent_event_t *e = &s_events[s_event_head];
    memset(e, 0, sizeof(*e));
    e->t_us = esp_timer_get_time();
    strlcpy(e->id, id, sizeof(e->id));
    strlcpy(e->event, ev, sizeof(e->event));
    strlcpy(e->tool, tool, sizeof(e->tool));
    strlcpy(e->session, sid, sizeof(e->session));
    int i = find_locked(id);
    e->prev = i >= 0 ? (int8_t)s_agents[i].state : -1;
    /* state -2: event leaves the state unchanged */
    e->state = i < 0 ? -1 : state == -2 ? e->prev : (int8_t)state;
    s_event_head = (s_event_head + 1) % AGENT_EVENT_LOG;
    if (s_event_count < AGENT_EVENT_LOG) s_event_count++;
    UNLOCK();
}

int agents_get_events(agent_event_t *out, int max)
{
    LOCK();
    int n = s_event_count < max ? s_event_count : max;
    for (int k = 0; k < n; k++)
        out[k] = s_events[(s_event_head - 1 - k + AGENT_EVENT_LOG) % AGENT_EVENT_LOG];
    UNLOCK();
    return n;
}

esp_err_t agents_handle_hook(const char *id, const char *event, const char *tool_q, const char *ntype_q,
                             const char *body)
{
    char ev[40] = "", tool[48] = "", ntype[40] = "", msg[AGENT_DETAIL_LEN] = "";
    if (event && event[0]) strlcpy(ev, event, sizeof(ev));
    else if (!json_find_str(body, "hook_event_name", ev, sizeof(ev)))
        json_find_str(body, "hookEventName", ev, sizeof(ev)); /* Grok */
    if (tool_q && tool_q[0]) strlcpy(tool, tool_q, sizeof(tool));
    else if (!json_find_str(body, "tool_name", tool, sizeof(tool)))
        json_find_str(body, "toolName", tool, sizeof(tool));
    if (ntype_q && ntype_q[0]) strlcpy(ntype, ntype_q, sizeof(ntype));
    else if (!json_find_str(body, "notification_type", ntype, sizeof(ntype)))
        json_find_str(body, "notificationType", ntype, sizeof(ntype));
    json_find_str(body, "message", msg, sizeof(msg)); /* only sent by hooks that forward the payload */
    char sid[40] = "";
    if (!json_find_str(body, "session_id", sid, sizeof(sid)))
        json_find_str(body, "sessionId", sid, sizeof(sid));

    if (!ev[0]) {
        log_event(id, "(none)", tool, sid, -1);
        return ESP_ERR_INVALID_ARG;
    }

    agent_state_t st = AGENT_STATE_WORKING;
    bool keep = false;   /* event only proves the agent is alive; state unchanged */
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
               !strcasecmp(ev, "PreCompact") || !strcasecmp(ev, "PostCompact")) {
        st = AGENT_STATE_WORKING;
    } else if (!strcasecmp(ev, "PermissionDenied")) { /* Grok: the turn goes on without the tool */
        st = AGENT_STATE_WORKING;
        snprintf(dbuf, sizeof(dbuf), "denied %s", tool[0] ? tool : "tool");
        detail = dbuf;
    } else if (!strcasecmp(ev, "SubagentStart") || !strcasecmp(ev, "SubagentStop")) {
        /* Never change the state: during a turn the main agent is already working (and the
         * subagent's own tool hooks keep it so), while Claude Code also runs short background
         * agents after a turn has ended - their SubagentStop arrived after Stop and left the
         * observer stuck in "working". */
        keep = true;
    } else if (!strcasecmp(ev, "PermissionRequest")) {
        st = AGENT_STATE_WAITING;
        snprintf(dbuf, sizeof(dbuf), "allow %s?", tool[0] ? tool : "tool");
        detail = dbuf;
    } else if (!strcasecmp(ev, "Notification")) {
        if (!strcasecmp(ntype, "idle_prompt") || strstr(msg, "waiting for your input")) {
            st = AGENT_STATE_IDLE;
        } else if (!strcasecmp(ntype, "auth_success")) {
            keep = true; /* informational, nothing to do for the user */
        } else {
            st = AGENT_STATE_WAITING;
            if (msg[0]) detail = msg;
            else if (!strcasecmp(ntype, "permission_prompt")) detail = "needs permission";
            else if (!strcasecmp(ntype, "elicitation_dialog")) detail = "input needed";
            else detail = "needs attention";
        }
    } else if (!strcasecmp(ev, "Stop") || !strcasecmp(ev, "agentStop")) { /* agentStop: Copilot CLI */
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
        log_event(id, ev, tool, sid, -1);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGD(TAG, "%s: hook %s%s%s", id, ev, tool[0] ? " " : "", tool);
    log_event(id, ev, tool, sid, keep ? -2 : (int)st);
    LOCK();
    int i = find_locked(id);
    if (i >= 0 && keep) {
        s_agents[i].last_event_us = esp_timer_get_time();
        s_version++;
    } else if (i >= 0) {
        /* Keep the waiting state when a tool runs only after the user answered;
         * PreToolUse for the question tool itself already put us in WAITING. */
        set_state_locked(i, st, detail ? detail : "");
    }
    UNLOCK();
    return i >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

uint8_t agents_get_offline_timeout(void)
{
    return s_offline_min;
}

esp_err_t agents_set_offline_timeout(int minutes)
{
    s_offline_min = clamp_offline(minutes);
    if (settings_get_u32(OFFLINE_KEY, CONFIG_AO_STALE_TIMEOUT_MIN) == s_offline_min) return ESP_OK;
    esp_err_t err = settings_set_u32(OFFLINE_KEY, s_offline_min);
    if (err == ESP_OK) ESP_LOGI(TAG, "offline timeout %u min saved", s_offline_min);
    return err;
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
    const int64_t stale = (int64_t)s_offline_min * 60 * 1000000;
    for (int i = 0; stale > 0 && i < s_count; i++) {
        agent_t *a = &s_agents[i];
        if (a->state != AGENT_STATE_OFFLINE && a->last_event_us && now - a->last_event_us > stale) {
            ESP_LOGI(TAG, "%s: stale -> offline", a->cfg.id);
            a->state = AGENT_STATE_OFFLINE;
            a->since_us = now;
            strlcpy(a->detail, "no events", AGENT_DETAIL_LEN);
            focus_busy_locked(i);
            s_version++;
        }
    }
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
