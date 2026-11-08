// Local automation engine: Lua 5.5 scripts stored on the board, run on one task.
//
// Never depends on Wi-Fi: scripts start right after the Zigbee registry is loaded,
// triggers and timers need no clock and no uplink. Network side effects (HTTP) are
// queued to their own task and come back as callbacks.
#pragma once

#include <stdbool.h>
#include <stddef.h>
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
int automation_enabled_count(void);              // Home Assistant mode rule

// Boot-loop guard, safe mode: after 3 abnormal resets in a row, each less than 60 s after
// boot, the engine starts no scripts this boot (nothing is disabled or deleted). The UI
// shows it; this starts the enabled scripts and leaves safe mode.
bool automation_safe_mode(void);
esp_err_t automation_leave_safe_mode(void);

// Script log lines with seq > `after` (at most `max`), as a JSON array of
// {seq, t (epoch s or -uptime s), script, level, msg}. *last = newest seq.
cJSON *automation_log_json(unsigned after, unsigned max, unsigned *last);

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
