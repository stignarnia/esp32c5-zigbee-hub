// Local automation engine: Lua 5.5 scripts stored on the board, run on one task.
//
// Never depends on Wi-Fi: scripts start right after the Zigbee registry is loaded,
// triggers and timers need no clock and no uplink. Network side effects (HTTP) are
// queued to their own task and come back as callbacks.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#define AUTO_NAME_MAX      24          // script name: [a-z0-9_-], 1..24 chars
#define AUTO_SCRIPT_MAX    (32 * 1024) // bytes of source
#define AUTO_MAX_SCRIPTS   16

// Call after zb_start() (the device registry is loaded) and before Wi-Fi.
esp_err_t automation_start(void);

// ---- scripts (any task except the automation task) ---------------------------------
// Snapshot for the UI: scripts (name, enabled, status, error, memory), tz, secrets
// (names only), GPIO config. Caller frees with cJSON_Delete.
cJSON *automation_json(void);

// Source of a script (malloc'd, NUL-terminated; caller frees). NULL if not found.
char *automation_source(const char *name);

// Syntax-checks and stores a script; (re)starts it if enabled. On a syntax error
// returns ESP_ERR_INVALID_ARG and fills `err` ("name:line: message").
esp_err_t automation_save(const char *name, const char *src, size_t len, bool enabled,
                          char *err, size_t err_cap);
esp_err_t automation_set_enabled(const char *name, bool enabled);
esp_err_t automation_restart(const char *name);  // after an error
esp_err_t automation_delete(const char *name);

// Boot-loop guard, safe mode: after 3 abnormal resets in a row, each less than 60 s after
// boot, the engine starts no scripts this boot (nothing is disabled or deleted). The UI
// shows it; this starts the enabled scripts and leaves safe mode.
bool automation_safe_mode(void);
esp_err_t automation_leave_safe_mode(void);

// Script log lines with seq > `after` (at most `max`), as a JSON array of
// {seq, t (epoch s or -uptime s), script, level, msg}. *last = newest seq.
cJSON *automation_log_json(unsigned after, unsigned max, unsigned *last);

// Brief list of the scripts (any task): name, the user's choice, running right now.
typedef struct {
    char name[AUTO_NAME_MAX + 1];
    bool enabled;
    bool running;
} auto_brief_t;
int automation_list(auto_brief_t *out, int max);

// ---- Home Assistant entities published by scripts (ha.sensor / ha.binary / ha.event) ---
// Definitions are kept (in /storage/auto/ha.json) until the script is deleted or calls
// ha.remove; values are not: they are unknown until the script publishes them after
// it starts, and become unknown again when it stops.
#define AUTO_HA_MAX          32   // all scripts together
#define AUTO_HA_PER_SCRIPT   8
#define AUTO_HA_NAME         32
#define AUTO_HA_EVENT_TYPES  4
#define AUTO_HA_TYPE_LEN     16

typedef enum { AUTO_HA_SENSOR = 1, AUTO_HA_BINARY, AUTO_HA_EVENT } auto_ha_type_t;

typedef struct {
    char script[AUTO_NAME_MAX + 1];
    char name[AUTO_HA_NAME + 1];
    uint8_t type;                 // auto_ha_type_t
    char unit[16];
    char device_class[32];
    char icon[48];                // "mdi:…", "" = HA's default
    int8_t decimals;              // sensors: digits shown
    uint8_t state_class;          // sensors: 0 none, 1 measurement, 2 total_increasing, 3 total
    uint8_t n_types;              // events: the event types used so far
    char types[AUTO_HA_EVENT_TYPES][AUTO_HA_TYPE_LEN];
    bool has_value;               // sensor/binary published since the script started
    float value;
    bool on;
} auto_ha_entity_t;

int automation_ha_entities(auto_ha_entity_t *out, int max);  // any task
uint32_t automation_ha_revision(void);  // changes with every definition or value change
// Called on the automation task for every ha.event(): copy and return, never block.
typedef void (*auto_ha_event_cb_t)(const char *script, const char *name, const char *type);
void automation_ha_set_event_cb(auto_ha_event_cb_t cb);

// ---- settings -----------------------------------------------------------------------
// Secrets: write-only values scripts read with secret(name). value NULL = delete.
esp_err_t automation_secret_set(const char *name, const char *value);
// Time zone for cron (POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3").
esp_err_t automation_set_tz(const char *tz);
// GPIO config: JSON array of {name, pin, mode}; mode "out" | "in" | "in_pullup" |
// "in_pulldown" | "ground". Validated against the allowed pins.
esp_err_t automation_set_gpio(const cJSON *pins, char *err, size_t err_cap);

// ---- reboot gate (Wi-Fi watchdog's last resort) ---------------------------------------
// NULL when a reboot is fine now (and script state + named timers were saved for
// it); otherwise the reason to wait.
const char *automation_reboot_blocker(void);
