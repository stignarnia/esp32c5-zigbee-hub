// Shared between the parts of the automation engine.
//
// Tasks:
//   auto     — runs every Lua state (PSRAM stack: never touches flash or NVS itself)
//   auto_io  — writes script `state` files (internal stack)
//   auto_http— HTTP requests for scripts, one at a time (PSRAM stack)
// Script files, metadata, secrets and GPIO config are written by the HTTP server task
// (the API calls), boot loading happens on the main task.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "automation.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lua.h"
#include "zb.h"

#define AUTO_DIR        "/storage/auto"
#define AUTO_STATE_DIR  AUTO_DIR "/state"
#define AUTO_META       AUTO_DIR "/scripts.json"
#define AUTO_GPIO_CFG   AUTO_DIR "/gpio.json"
#define AUTO_SCRIPT_DIR "/storage/scripts"

#define AUTO_MEM_CAP    (256 * 1024)   // Lua heap per script (PSRAM)
#define AUTO_MAX_SECRETS 16
#define AUTO_SECRET_NAME 15            // NVS key limit
#define AUTO_SECRET_MAX 512

typedef enum { ST_STOPPED = 0, ST_RUNNING, ST_ERROR } script_status_t;

typedef struct {
    bool used;
    char name[AUTO_NAME_MAX + 1];
    bool enabled;                 // the user's choice (persisted)
    uint8_t status;               // script_status_t (live)
    char err[256];                // last error, "" if none
    lua_State *L;                 // only touched on the automation task
    size_t mem, mem_peak, mem_cap;
    uint32_t gen;                 // bumped on every stop: stale callbacks are dropped
    int64_t started_us;
    uint32_t calls;               // callbacks run
    int64_t busy_us;              // time spent running Lua
    int64_t http_win_us;          // rate limit window start
    uint8_t http_n;               // requests in the window
} script_t;

extern script_t *g_scripts;          // AUTO_MAX_SCRIPTS, PSRAM
extern SemaphoreHandle_t g_mtx;      // script table metadata, secrets

// ---- events into the automation task -----------------------------------------------------
typedef struct auto_cmd auto_cmd_t;

typedef enum { EV_ZB = 1, EV_GPIO, EV_HTTP, EV_CMD } ev_type_t;

typedef struct {
    uint8_t type;
    union {
        zb_event_t zb;
        struct { uint8_t pin; } gpio;
        struct {
            uint8_t script;
            uint32_t gen;
            int ref;              // on_done in that script's registry, LUA_NOREF = none
            int status;           // HTTP status, 0 = network error / gave up
            char *body;           // malloc'd (PSRAM), may be NULL; freed by the receiver
            size_t len;
            char err[64];
        } http;
        auto_cmd_t *cmd;
    };
} ev_t;

bool auto_post(const ev_t *ev);                    // never blocks; false = queue full
bool auto_post_from_isr(const ev_t *ev, BaseType_t *woken);

// ---- engine helpers for the libraries (automation task only) ------------------------------
script_t *auto_script_of(lua_State *L);
// True while `L`'s script runs its top level (when it is saved, enabled or the hub
// boots). GPIO pins can only be claimed then, so conflicts show up at Save.
bool auto_loading(lua_State *L);
int auto_script_index(const script_t *s);
void auto_log(const char *script, char level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
// Appends the table at the top of the stack to registry list `key` (zb.on, gpio.on,
// net.on) and pops it. Raises a Lua error when the list is full.
void auto_reg_append(lua_State *L, const char *key);
// Runs `f(lightuserdata ud)` in the script's state inside one protected call with the
// run-time limit. False if the script failed (it was stopped, its state closed).
bool auto_run(script_t *s, lua_CFunction f, void *ud);
// From inside auto_run(): call user code (function + nargs pushed) with a fresh 1 s allowance.
void auto_call(lua_State *L, int nargs);
// GPIO edge for the script that owns `pin` (called by auto_gpio.c on the automation task).
void auto_dispatch_gpio(int script, int pin, int level);

// ---- secrets (auto_engine.c) ------------------------------------------------------------
void auto_mask_secrets(char *msg, size_t cap);

// ---- store (auto_store.c) ------------------------------------------------------------------
void store_init_dirs(void);
bool store_valid_name(const char *name);
esp_err_t store_load_meta(void);                 // fills g_scripts (names, enabled)
esp_err_t store_write_meta(void);
char *store_read_script(const char *name, size_t *len);
esp_err_t store_write_script(const char *name, const char *src, size_t len);
void store_delete_script(const char *name);
void store_secrets_load(void);
esp_err_t store_secret_set(const char *name, const char *value);
const char *store_secret_get(const char *name);  // under g_mtx; NULL if none
cJSON *store_secret_names(void);
int store_secret_values(const char **vals, int max);  // under g_mtx, for masking
const char *store_tz(void);
esp_err_t store_set_tz(const char *tz);
// script state (state.get/put)
void store_state_load(int idx);
cJSON *store_state_lock(int idx);                // returns the object, takes the lock
void store_state_unlock(int idx, bool dirty);
void store_state_forget(int idx, bool delete_file);
void store_state_flush(void);                    // write every dirty state now
void store_io_start(void);

// ---- GPIO (auto_gpio.c) ----------------------------------------------------------------
void agpio_init(void);                           // main task: load + apply config
esp_err_t agpio_set_config(const cJSON *pins, char *err, size_t cap);
cJSON *agpio_json(void);
void agpio_open_lib(lua_State *L);
void agpio_release(int script);                  // fail-safe: outputs low, patterns off
void agpio_on_isr_event(uint8_t pin, int64_t now_us);
int64_t agpio_tick(int64_t now_us);              // debounce; returns next check (0 = none)
bool agpio_busy(void);                           // a pattern is playing
// Boot-loop guard: names of the scripts whose buzzer was playing when the previous boot
// ended (RTC memory); clears the marker. Call once at boot, before scripts start.
int agpio_take_marker(char names[][AUTO_NAME_MAX + 1], int max);
cJSON *agpio_led_json(void);                     // RGB LED: owner, rgb shown, pending

// ---- HTTP (auto_http.c) --------------------------------------------------------------------
void ahttp_start(void);
void ahttp_open_lib(lua_State *L);
