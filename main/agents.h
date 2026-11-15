#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#define AGENT_ID_LEN     16
#define AGENT_NAME_LEN   32
#define AGENT_DETAIL_LEN 40
#define AGENT_MAX        CONFIG_AO_MAX_AGENTS

typedef enum {
    AGENT_KIND_CLAUDE = 0,
    AGENT_KIND_COPILOT,
    AGENT_KIND_OTHER,
    AGENT_KIND_COUNT
} agent_kind_t;

typedef enum {
    AGENT_STATE_OFFLINE = 0,
    AGENT_STATE_IDLE,
    AGENT_STATE_WORKING,
    AGENT_STATE_WAITING,
    AGENT_STATE_ERROR,
    AGENT_STATE_COUNT
} agent_state_t;

typedef struct {
    char id[AGENT_ID_LEN];      /* url-safe slug used by hooks */
    char name[AGENT_NAME_LEN];  /* display name */
    uint8_t kind;               /* agent_kind_t */
} agent_cfg_t;

typedef struct {
    agent_cfg_t cfg;
    agent_state_t state;
    int64_t since_us;           /* when the current state began */
    int64_t last_event_us;      /* last event of any kind (0 = never) */
    char detail[AGENT_DETAIL_LEN];
    bool restored;              /* state was restored after a reboot; no live event since */
} agent_t;

typedef struct {
    int count;
    int displayed;              /* index into agents[], -1 when empty */
    uint32_t version;           /* bumps on any change */
    agent_t agents[AGENT_MAX];
} agents_snapshot_t;

esp_err_t agents_init(void);

void agents_snapshot(agents_snapshot_t *out);

/* Configuration (persisted). */
esp_err_t agents_add(const agent_cfg_t *cfg);
esp_err_t agents_update(const char *id, const char *name, int kind /* -1 = keep */);
esp_err_t agents_remove(const char *id);
esp_err_t agents_move(const char *id, int delta);

/* Runtime state. */
esp_err_t agents_set_state(const char *id, agent_state_t st, const char *detail);
/* Map a Claude Code / Copilot hook event to a state. event may be NULL (then taken from body). */
esp_err_t agents_handle_hook(const char *id, const char *event, const char *body);

/* Diagnostics: the most recent hook events as received (newest first), whether or not they
 * matched an agent or changed its state. */
#define AGENT_EVENT_LOG 32
typedef struct {
    int64_t t_us;               /* esp_timer time of arrival */
    char id[AGENT_ID_LEN];
    char event[24];
    char tool[24];
    char session[9];            /* first 8 chars of session_id / sessionId, if present */
    int8_t state;               /* resulting agent_state_t, -1 = unknown event / unknown agent */
    int8_t prev;                /* agent state before the event, -1 = unknown agent */
} agent_event_t;
int agents_get_events(agent_event_t *out, int max);

/* Minutes (0..240, 0 = never) without any event after which an agent is marked offline.
 * Persisted in NVS; the default comes from CONFIG_AO_STALE_TIMEOUT_MIN. */
esp_err_t agents_set_offline_timeout(int minutes);
uint8_t agents_get_offline_timeout(void);

/* Button: show next agent. */
void agents_cycle(void);
/* Periodic housekeeping (stale timeout, auto-focus on waiting agents, saving states). */
void agents_tick(void);
/* Save pending state changes now (also runs automatically from esp_restart()). */
void agents_flush_states(void);

const char *agent_state_name(agent_state_t st);
bool agent_state_from_name(const char *s, agent_state_t *out);
const char *agent_kind_name(agent_kind_t k);
bool agent_kind_from_name(const char *s, agent_kind_t *out);
bool agent_id_valid(const char *id);

/* Most urgent state across all agents (waiting > error > working > idle > offline). */
agent_state_t agents_aggregate_state(const agents_snapshot_t *s);
