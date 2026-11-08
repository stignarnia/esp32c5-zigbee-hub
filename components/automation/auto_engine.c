// Automation engine: one task runs every script's Lua state.
//
// Everything a script does is either instant (reads, gpio writes, starting a buzzer
// pattern, queuing an HTTP request) or a callback the task calls later (Zigbee
// changes, timers, cron, GPIO edges, HTTP results, network up/down). Nothing a script
// does can block Zigbee: Zigbee posts events into a queue without waiting, and drops
// (and counts) them if the queue is full.
//
// Safety:
//   - each script has its own lua_State with a PSRAM allocator capped at 256 KB
//   - every call into Lua is limited to 1 s of run time (instruction-count hook)
//   - an error, a timeout or running out of memory stops that script only: its
//     outputs go low, its buzzer patterns stop, its timers are dropped
//   - the sandbox has no io, no os.execute/exit/getenv/remove/rename, no require,
//     no load/loadfile/dofile (no bytecode, no file access), no string.dump
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "auto_internal.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hublog.h"
#include "lauxlib.h"
#include "lualib.h"
#include "nvs.h"
#include "wifi5g.h"

static const char *TAG = "auto";

#define RUN_LIMIT_US     (1000 * 1000)    // per call into Lua
#define HOOK_EVERY       1000             // instructions between run-time checks
#define QUEUE_LEN        32
#define MAX_TIMERS       64
#define MAX_TIMERS_EACH  16
#define MAX_CRON         32
#define MAX_HANDLERS     64               // per list (zb.on, gpio.on, net.on) per script
#define LOG_LINES        256
#define LOG_MSG          200
#define STATE_KEYS_MAX   64
#define STATE_STR_MAX    1024
#define GATE_HORIZON_US  (120LL * 1000000)   // reboot gate: nothing due within 2 min
#define MAX_BUZ_BLAME    5
// PSRAM. Lua limits nesting to LUAI_MAXCCALLS=100 C levels, but not their size: the
// heaviest level (a string.gsub callback: gsub's own frame is 880 B) costs ~1.15 KB.
// Worst case (gsub nested to the limit, uncaught, so the traceback runs at the deepest
// point): 116.4 KB.
#define TASK_STACK       (160 * 1024)

script_t *g_scripts;
SemaphoreHandle_t g_mtx;

static QueueHandle_t s_q;
static TaskHandle_t s_task;
static volatile bool s_running;          // Lua is executing right now
static int64_t s_run_start_us;
static bool s_killed;
static uint32_t s_zb_drops;
static bool s_online;
static const script_t *s_loading;        // script running its top level (start_p), or NULL

// The script that is executing, kept across a soft reset. If the hub panics or a
// watchdog fires while it runs, the next boot disables that script instead of running
// it again into a boot loop (Zigbee would go down with every round).
#define RUN_MAGIC 0x52554e31u
typedef struct {
    uint32_t magic;
    char name[AUTO_NAME_MAX + 1];
} rtc_run_t;
static RTC_NOINIT_ATTR rtc_run_t s_rtc_run;

// Boot-loop guard. Abnormal = the hub didn't mean to restart: a crash,
// a watchdog, a power glitch (a loose buzzer throws one), a brownout, a CPU lockup.
// Power-on, the RST button / serial reset and esp_restart() (OTA, planned reboots, the
// Wi-Fi watchdog) are normal.
static bool reset_abnormal(esp_reset_reason_t r)
{
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
           r == ESP_RST_BROWNOUT || r == ESP_RST_PWR_GLITCH || r == ESP_RST_CPU_LOCKUP;
}

static const char *reset_words(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    default: return "normal";
    }
}

// Did RTC memory survive the last reset? A canary written at every boot; reported in the
// log and the API, so the first real power-glitch reset tells whether the buzzer marker
// (RTC memory) can catch one. The safe-mode counter is in NVS and doesn't depend on it.
#define CANARY 0x43414e59u
static RTC_NOINIT_ATTR uint32_t s_rtc_canary[2];
static bool s_rtc_kept;

// Safe mode: NVS "autoboot": n = abnormal resets in a row that came < 60 s after boot,
// up = the boot before reached 60 s. Written by the main task at boot and by a small
// internal-stack task at 60 s (the engine task's stack is in PSRAM: no flash writes there).
#define BOOT_NS          "autoboot"
#define SAFE_AFTER       3
#define BOOT_OK_US       (60LL * 1000000)
static bool s_safe_mode;
static uint8_t s_boot_resets;
static const char *s_last_reset = "normal";

// ---- log ring (script log in the UI) --------------------------------------------------------

typedef struct {
    unsigned seq;
    int64_t t;                            // epoch s, or -uptime s before the clock is set
    char script[AUTO_NAME_MAX + 1];
    char level;
    char msg[LOG_MSG];
} logline_t;

static logline_t *s_log;
static unsigned s_log_seq;
static SemaphoreHandle_t s_log_mtx;

void auto_log(const char *script, char level, const char *fmt, ...)
{
    char msg[LOG_MSG];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    auto_mask_secrets(msg, sizeof(msg));
    if (level == 'E') ESP_LOGW("script", "%s: %s", script, msg);
    else ESP_LOGI("script", "%s: %s", script, msg);
    if (!s_log) return;
    xSemaphoreTake(s_log_mtx, portMAX_DELAY);
    logline_t *l = &s_log[s_log_seq % LOG_LINES];
    l->seq = ++s_log_seq;
    l->t = hublog_time_valid() ? (int64_t)time(NULL) : -(esp_timer_get_time() / 1000000);
    strlcpy(l->script, script, sizeof(l->script));
    l->level = level;
    strlcpy(l->msg, msg, sizeof(l->msg));
    xSemaphoreGive(s_log_mtx);
}

cJSON *automation_log_json(unsigned after, unsigned max, unsigned *last)
{
    cJSON *arr = cJSON_CreateArray();
    xSemaphoreTake(s_log_mtx, portMAX_DELAY);
    unsigned newest = s_log_seq;
    unsigned oldest = newest > LOG_LINES ? newest - LOG_LINES + 1 : 1;
    unsigned from = after + 1 > oldest ? after + 1 : oldest;
    if (newest >= from && newest - from + 1 > max) from = newest - max + 1;
    for (unsigned seq = from; seq <= newest; seq++) {
        const logline_t *l = &s_log[(seq - 1) % LOG_LINES];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "seq", l->seq);
        cJSON_AddNumberToObject(o, "t", (double)l->t);
        cJSON_AddStringToObject(o, "script", l->script);
        char lv[2] = {l->level, 0};
        cJSON_AddStringToObject(o, "level", lv);
        cJSON_AddStringToObject(o, "msg", l->msg);
        cJSON_AddItemToArray(arr, o);
    }
    xSemaphoreGive(s_log_mtx);
    *last = newest;
    return arr;
}

void auto_mask_secrets(char *msg, size_t cap)
{
    if (!g_mtx) return;
    const char *vals[AUTO_MAX_SECRETS];
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    int n = store_secret_values(vals, AUTO_MAX_SECRETS);
    for (int i = 0; i < n; i++) {
        size_t vl = strlen(vals[i]);
        if (vl < 4) continue;  // too short to mask without mangling ordinary text
        char *p;
        while ((p = strstr(msg, vals[i])) != NULL) {
            size_t tail = strlen(p + vl);
            memcpy(p, "***", 3);
            memmove(p + 3, p + vl, tail + 1);
        }
    }
    xSemaphoreGive(g_mtx);
    (void)cap;
}

// ---- events -----------------------------------------------------------------------------------

bool auto_post(const ev_t *ev)
{
    return s_q && xQueueSend(s_q, ev, 0) == pdTRUE;
}

bool auto_post_from_isr(const ev_t *ev, BaseType_t *woken)
{
    return s_q && xQueueSendFromISR(s_q, ev, woken) == pdTRUE;
}

// Zigbee task, registry locked: copy and go.
static void on_zb_event(const zb_event_t *e)
{
    ev_t ev = {.type = EV_ZB};
    ev.zb = *e;
    if (!auto_post(&ev)) s_zb_drops++;
}

// ---- scripts: identity, memory, run-time limit --------------------------------------------------

script_t *auto_script_of(lua_State *L)
{
    return *(script_t **)lua_getextraspace(L);
}

int auto_script_index(const script_t *s)
{
    return (int)(s - g_scripts);
}

static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    script_t *s = ud;
    if (!ptr) osize = 0;
    if (nsize == 0) {
        heap_caps_free(ptr);
        s->mem -= osize;
        return NULL;
    }
    if (nsize > osize && s->mem - osize + nsize > s->mem_cap) return NULL;  // over the cap
    void *p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return NULL;
    s->mem = s->mem - osize + nsize;
    if (s->mem > s->mem_peak) s->mem_peak = s->mem;
    return p;
}

static void hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (esp_timer_get_time() - s_run_start_us > RUN_LIMIT_US) {
        s_killed = true;  // even if the script catches this with pcall, it is stopped after
        luaL_error(L, "ran for more than %d ms without returning (endless loop?)", RUN_LIMIT_US / 1000);
    }
}

static int msgh(lua_State *L)
{
    const char *msg = lua_tostring(L, 1);
    if (!msg) msg = luaL_typename(L, 1);
    luaL_traceback(L, L, msg, 1);
    return 1;
}

static void stop_script(script_t *s, uint8_t status, const char *err);

// Every entry into Lua goes through here: `f` runs inside one protected call, so a
// memory error anywhere (even pushing a string) can't reach Lua's panic handler (which
// would abort the hub). Inside `f`, user code is called with auto_call(). False if the
// script failed (it has been stopped and its state closed).
bool auto_run(script_t *s, lua_CFunction f, void *ud)
{
    lua_State *L = s->L;
    lua_pushcfunction(L, msgh);
    lua_pushcfunction(L, f);
    lua_pushlightuserdata(L, ud);
    s_killed = false;
    s_run_start_us = esp_timer_get_time();
    int64_t t0 = s_run_start_us;
    strlcpy(s_rtc_run.name, s->name, sizeof(s_rtc_run.name));
    s_rtc_run.magic = RUN_MAGIC;
    s_running = true;
    int rc = lua_pcall(L, 1, 0, -3);
    s_running = false;
    s_rtc_run.magic = 0;
    s->busy_us += esp_timer_get_time() - t0;
    if (rc == LUA_OK && !s_killed) {
        lua_pop(L, 1);  // msgh
        return true;
    }
    char err[sizeof(s->err)];
    if (rc == LUA_ERRMEM) {
        snprintf(err, sizeof(err), "out of memory (limit %u KB)", (unsigned)(s->mem_cap / 1024));
    } else if (rc != LUA_OK) {
        strlcpy(err, lua_tostring(L, -1) ? lua_tostring(L, -1) : "error", sizeof(err));
    } else {
        snprintf(err, sizeof(err), "ran for more than %d ms (the error was caught by pcall; stopped anyway)",
                 RUN_LIMIT_US / 1000);
    }
    auto_log(s->name, 'E', "stopped: %s", err);
    stop_script(s, ST_ERROR, err);
    return false;
}

// Calls user code (function + nargs on the stack) from inside auto_run(), with a fresh
// run-time allowance. Errors propagate to auto_run().
void auto_call(lua_State *L, int nargs)
{
    auto_script_of(L)->calls++;
    s_run_start_us = esp_timer_get_time();
    lua_call(L, nargs, 0);
}

// ---- handler lists in the registry (zb.on, gpio.on, net.on) -------------------------------------

// Appends the table at the top of the stack to registry list `key` and pops it.
void auto_reg_append(lua_State *L, const char *key)
{
    if (lua_getfield(L, LUA_REGISTRYINDEX, key) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, key);
    }
    lua_Unsigned n = lua_rawlen(L, -1);
    if (n >= MAX_HANDLERS) luaL_error(L, "too many handlers (max %d)", MAX_HANDLERS);
    lua_insert(L, -2);
    lua_rawseti(L, -2, (lua_Integer)n + 1);
    lua_pop(L, 1);
}

// ---- timers and cron ---------------------------------------------------------------------------

typedef struct {
    bool used;
    uint8_t script;
    uint32_t id;
    int64_t due_us, every_us;     // every 0 = one-shot
    int ref;                      // function in the registry, LUA_NOREF = named handler
    char handler[33];             // global function name (survives a reboot)
} atimer_t;

typedef struct {
    bool used;
    uint8_t script;
    uint32_t id;
    uint64_t min;
    uint32_t hour, dom;
    uint16_t mon;
    uint8_t dow;
    bool dom_star, dow_star;
    int ref;
    char spec[48];
} acron_t;

static atimer_t *s_timers;       // PSRAM
static acron_t *s_cron;
static SemaphoreHandle_t s_tmtx; // timers/cron: the reboot gate reads them from another task
static uint32_t s_next_id = 1;
static int64_t s_cron_key = -1;  // last evaluated minute (epoch / 60)

static int timers_of(int script)
{
    int n = 0;
    for (int i = 0; i < MAX_TIMERS; i++) n += s_timers[i].used && s_timers[i].script == script;
    return n;
}

static int new_timer(lua_State *L, bool every)
{
    script_t *s = auto_script_of(L);
    int idx = auto_script_index(s);
    lua_Number sec = luaL_checknumber(L, 1);
    luaL_argcheck(L, sec >= 0.01 && sec <= 30 * 86400, 1, "seconds: 0.01 .. 30 days");
    luaL_argcheck(L, !every || sec >= 0.1, 1, "timer.every: at least 0.1 s");
    int t = lua_type(L, 2);
    luaL_argexpected(L, t == LUA_TFUNCTION || t == LUA_TSTRING, 2, "function or global function name");
    if (t == LUA_TSTRING) luaL_argcheck(L, lua_rawlen(L, 2) < 33, 2, "name too long");
    if (timers_of(idx) >= MAX_TIMERS_EACH) luaL_error(L, "too many timers (max %d per script)", MAX_TIMERS_EACH);
    xSemaphoreTake(s_tmtx, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS && slot < 0; i++) if (!s_timers[i].used) slot = i;
    xSemaphoreGive(s_tmtx);
    if (slot < 0) luaL_error(L, "too many timers on the hub (max %d)", MAX_TIMERS);
    int ref = LUA_NOREF;
    char handler[33] = "";
    if (t == LUA_TFUNCTION) {
        lua_pushvalue(L, 2);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        strlcpy(handler, lua_tostring(L, 2), sizeof(handler));
    }
    int64_t us = (int64_t)(sec * 1e6);
    xSemaphoreTake(s_tmtx, portMAX_DELAY);
    atimer_t *tm = &s_timers[slot];
    *tm = (atimer_t){.used = true, .script = (uint8_t)idx, .id = s_next_id++, .ref = ref,
                     .due_us = esp_timer_get_time() + us, .every_us = every ? us : 0};
    strlcpy(tm->handler, handler, sizeof(tm->handler));
    uint32_t id = tm->id;
    xSemaphoreGive(s_tmtx);
    lua_pushinteger(L, id);
    return 1;
}

static int l_timer_after(lua_State *L) { return new_timer(L, false); }
static int l_timer_every(lua_State *L) { return new_timer(L, true); }

static int l_timer_cancel(lua_State *L)
{
    script_t *s = auto_script_of(L);
    int idx = auto_script_index(s);
    lua_Integer id = luaL_checkinteger(L, 1);
    int ref = LUA_NOREF;
    bool found = false;
    xSemaphoreTake(s_tmtx, portMAX_DELAY);
    for (int i = 0; i < MAX_TIMERS && !found; i++) {
        atimer_t *t = &s_timers[i];
        if (t->used && t->script == idx && t->id == id) { ref = t->ref; t->used = false; found = true; }
    }
    for (int i = 0; i < MAX_CRON && !found; i++) {
        acron_t *c = &s_cron[i];
        if (c->used && c->script == idx && c->id == id) { ref = c->ref; c->used = false; found = true; }
    }
    xSemaphoreGive(s_tmtx);
    if (ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ref);
    lua_pushboolean(L, found);
    return 1;
}

// One cron field: "*", "*/n", "a", "a-b", "a-b/n", comma lists. Bits lo..hi.
static bool cron_field(const char *f, size_t len, int lo, int hi, uint64_t *mask, bool *star)
{
    *mask = 0;
    *star = len == 1 && f[0] == '*';
    const char *p = f, *end = f + len;
    while (p < end) {
        const char *comma = memchr(p, ',', end - p);
        const char *ie = comma ? comma : end;
        int a = lo, b = hi, step = 1;
        const char *q = p;
        if (*q == '*') {
            q++;
        } else {
            char *e;
            a = (int)strtol(q, &e, 10);
            if (e == q) return false;
            b = a;
            q = e;
            if (q < ie && *q == '-') {
                b = (int)strtol(q + 1, &e, 10);
                if (e == q + 1) return false;
                q = e;
            }
        }
        if (q < ie && *q == '/') {
            char *e;
            step = (int)strtol(q + 1, &e, 10);
            if (e == q + 1 || step < 1) return false;
            q = e;
            if (a == b && p[0] != '*') b = hi;  // "5/15" = from 5 every 15
        }
        if (q != ie || a < lo || b > hi || a > b) return false;
        for (int v = a; v <= b; v += step) *mask |= 1ULL << v;
        p = comma ? comma + 1 : end;
    }
    return *mask != 0;
}

static int l_cron(lua_State *L)
{
    script_t *s = auto_script_of(L);
    int idx = auto_script_index(s);
    size_t len;
    const char *spec = luaL_checklstring(L, 1, &len);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    luaL_argcheck(L, len < sizeof(((acron_t *)0)->spec), 1, "too long");
    const char *f[5];
    size_t fl[5];
    int n = 0;
    for (const char *p = spec; *p && n < 6;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *st = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (n < 5) { f[n] = st; fl[n] = p - st; }
        n++;
    }
    if (n != 5) luaL_argerror(L, 1, "need 5 fields: minute hour day month weekday");
    acron_t c = {.used = true, .script = (uint8_t)idx};
    uint64_t m;
    bool star;
    static const int lo[5] = {0, 0, 1, 1, 0}, hi[5] = {59, 23, 31, 12, 7};
    static const char *names[5] = {"minute", "hour", "day of month", "month", "weekday"};
    for (int i = 0; i < 5; i++) {
        if (!cron_field(f[i], fl[i], lo[i], hi[i], &m, &star)) luaL_error(L, "cron '%s': bad %s field", spec, names[i]);
        switch (i) {
        case 0: c.min = m; break;
        case 1: c.hour = (uint32_t)m; break;
        case 2: c.dom = (uint32_t)m; c.dom_star = star; break;
        case 3: c.mon = (uint16_t)m; break;
        case 4: c.dow = (uint8_t)((m | (m >> 7)) & 0x7F); c.dow_star = star; break;  // 7 = Sunday = 0
        }
    }
    strlcpy(c.spec, spec, sizeof(c.spec));
    lua_pushvalue(L, 2);
    c.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    xSemaphoreTake(s_tmtx, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_CRON && slot < 0; i++) if (!s_cron[i].used) slot = i;
    if (slot >= 0) {
        c.id = s_next_id++;
        s_cron[slot] = c;
    }
    xSemaphoreGive(s_tmtx);
    if (slot < 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, c.ref);
        luaL_error(L, "too many cron jobs on the hub (max %d)", MAX_CRON);
    }
    lua_pushinteger(L, c.id);
    return 1;
}

static bool cron_match(const acron_t *c, const struct tm *t)
{
    if (!(c->min >> t->tm_min & 1) || !(c->hour >> t->tm_hour & 1) || !(c->mon >> (t->tm_mon + 1) & 1)) return false;
    bool dom = c->dom >> t->tm_mday & 1, dow = c->dow >> t->tm_wday & 1;
    if (c->dom_star && c->dow_star) return true;
    if (c->dom_star) return dow;
    if (c->dow_star) return dom;
    return dom || dow;  // both restricted: either matches (classic cron)
}

// Drop everything a script registered (it stopped). Refs die with the state.
static void timers_drop(int idx)
{
    xSemaphoreTake(s_tmtx, portMAX_DELAY);
    for (int i = 0; i < MAX_TIMERS; i++) if (s_timers[i].used && s_timers[i].script == idx) s_timers[i].used = false;
    for (int i = 0; i < MAX_CRON; i++) if (s_cron[i].used && s_cron[i].script == idx) s_cron[i].used = false;
    xSemaphoreGive(s_tmtx);
}

static int timer_p(lua_State *L)
{
    const atimer_t *t = lua_touserdata(L, 1);
    if (t->ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, t->ref);
        if (!t->every_us) luaL_unref(L, LUA_REGISTRYINDEX, t->ref);  // one-shot: done with it
    } else if (lua_getglobal(L, t->handler) != LUA_TFUNCTION) {
        auto_log(auto_script_of(L)->name, 'E', "timer: '%s' is not a global function; timer dropped", t->handler);
        return 0;
    }
    auto_call(L, 0);
    return 0;
}

static int64_t run_timers(int64_t now)
{
    int64_t next = INT64_MAX;
    for (int i = 0; i < MAX_TIMERS; i++) {
        xSemaphoreTake(s_tmtx, portMAX_DELAY);
        atimer_t t = s_timers[i];
        bool fire = t.used && t.due_us <= now;
        if (fire) {
            if (t.every_us) {
                s_timers[i].due_us += t.every_us;
                if (s_timers[i].due_us <= now) s_timers[i].due_us = now + t.every_us;  // fell behind: no burst
            } else {
                s_timers[i].used = false;
            }
        }
        xSemaphoreGive(s_tmtx);
        if (fire && g_scripts[t.script].status == ST_RUNNING) auto_run(&g_scripts[t.script], timer_p, &t);
    }
    xSemaphoreTake(s_tmtx, portMAX_DELAY);
    for (int i = 0; i < MAX_TIMERS; i++) if (s_timers[i].used && s_timers[i].due_us < next) next = s_timers[i].due_us;
    xSemaphoreGive(s_tmtx);
    return next;
}

static int cron_p(lua_State *L)
{
    const acron_t *c = lua_touserdata(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref);
    auto_call(L, 0);
    return 0;
}

static void run_cron(void)
{
    if (!hublog_time_valid()) return;  // "clock unset": never fire at a wrong time
    time_t now = time(NULL);
    int64_t key = now / 60;
    if (key == s_cron_key) return;
    s_cron_key = key;
    struct tm tm;
    localtime_r(&now, &tm);
    for (int i = 0; i < MAX_CRON; i++) {
        xSemaphoreTake(s_tmtx, portMAX_DELAY);
        acron_t c = s_cron[i];
        xSemaphoreGive(s_tmtx);
        if (!c.used || !cron_match(&c, &tm)) continue;
        if (g_scripts[c.script].status == ST_RUNNING) auto_run(&g_scripts[c.script], cron_p, &c);
    }
}

// ---- timers that survive a reboot (named handlers only) ------------------------------------------

#define RTC_MAGIC   0x4155544Fu   // "AUTO"
#define RTC_TIMERS  16

typedef struct {
    char script[AUTO_NAME_MAX + 1];
    char handler[33];
    uint32_t remain_ms, every_ms;
} rtc_timer_t;

typedef struct {
    uint32_t magic;
    uint32_t n;
    rtc_timer_t t[RTC_TIMERS];
    uint32_t sum;
} rtc_timers_t;

static RTC_NOINIT_ATTR rtc_timers_t s_rtc;
static rtc_timers_t *s_restore;       // copy taken at boot (PSRAM)

static uint32_t rtc_sum(const rtc_timers_t *r)
{
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)r;
    for (size_t i = 0; i < offsetof(rtc_timers_t, sum); i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void rtc_save(void)
{
    if (!s_timers || xSemaphoreTake(s_tmtx, pdMS_TO_TICKS(200)) != pdTRUE) return;
    int64_t now = esp_timer_get_time();
    uint32_t n = 0, lost = 0;
    for (int i = 0; i < MAX_TIMERS; i++) {
        const atimer_t *t = &s_timers[i];
        if (!t->used) continue;
        if (t->ref != LUA_NOREF || n >= RTC_TIMERS) { lost++; continue; }
        rtc_timer_t *r = &s_rtc.t[n++];
        strlcpy(r->script, g_scripts[t->script].name, sizeof(r->script));
        strlcpy(r->handler, t->handler, sizeof(r->handler));
        r->remain_ms = t->due_us > now ? (uint32_t)((t->due_us - now) / 1000) : 0;
        r->every_ms = (uint32_t)(t->every_us / 1000);
    }
    xSemaphoreGive(s_tmtx);
    s_rtc.n = n;
    s_rtc.magic = RTC_MAGIC;
    s_rtc.sum = rtc_sum(&s_rtc);
    if (n || lost) {
        ESP_LOGI(TAG, "reboot: %lu named timer(s) kept for after the restart, %lu with function callbacks lost",
                 (unsigned long)n, (unsigned long)lost);
    }
}

static void rtc_take(void)
{
    if (s_rtc.magic == RTC_MAGIC && s_rtc.n <= RTC_TIMERS && s_rtc.sum == rtc_sum(&s_rtc)) {
        s_restore = heap_caps_malloc(sizeof(*s_restore), MALLOC_CAP_SPIRAM);
        if (s_restore) *s_restore = s_rtc;
    }
    s_rtc.magic = 0;
}

static void rtc_restore_for(script_t *s)
{
    if (!s_restore) return;
    int idx = auto_script_index(s), n = 0;
    int64_t now = esp_timer_get_time();
    for (uint32_t i = 0; i < s_restore->n; i++) {
        rtc_timer_t *r = &s_restore->t[i];
        if (!r->script[0] || strcmp(r->script, s->name) != 0) continue;
        xSemaphoreTake(s_tmtx, portMAX_DELAY);
        for (int k = 0; k < MAX_TIMERS; k++) {
            if (s_timers[k].used) continue;
            s_timers[k] = (atimer_t){.used = true, .script = (uint8_t)idx, .id = s_next_id++, .ref = LUA_NOREF,
                                     .due_us = now + (int64_t)r->remain_ms * 1000,
                                     .every_us = (int64_t)r->every_ms * 1000};
            strlcpy(s_timers[k].handler, r->handler, sizeof(s_timers[k].handler));
            n++;
            break;
        }
        xSemaphoreGive(s_tmtx);
        r->script[0] = '\0';
    }
    if (n) auto_log(s->name, 'I', "%d timer(s) restored after the restart", n);
}

static void on_shutdown(void)
{
    rtc_save();
    store_state_flush();
}

const char *automation_reboot_blocker(void)
{
    if (!s_timers) return NULL;
    if (s_running) return "a script is running";
    if (agpio_busy()) return "a buzzer pattern is playing";
    int64_t now = esp_timer_get_time();
    const char *why = NULL;
    xSemaphoreTake(s_tmtx, portMAX_DELAY);
    for (int i = 0; i < MAX_TIMERS && !why; i++) {
        if (s_timers[i].used && s_timers[i].due_us - now < GATE_HORIZON_US) why = "a script timer is due within 2 min";
    }
    if (!why && hublog_time_valid()) {
        time_t t = time(NULL);
        for (int ahead = 0; ahead <= 2 && !why; ahead++) {
            time_t at = t + ahead * 60;
            struct tm tm;
            localtime_r(&at, &tm);
            for (int i = 0; i < MAX_CRON && !why; i++) {
                if (s_cron[i].used && cron_match(&s_cron[i], &tm)) why = "a cron job is due within 2 min";
            }
        }
    }
    xSemaphoreGive(s_tmtx);
    if (why) return why;
    store_state_flush();
    rtc_save();
    return NULL;
}

// ---- cJSON values kept safe across Lua errors ----------------------------------------------------
// A Lua error (out of memory while building a table) longjmps out of C: anything
// malloc'd outside Lua would leak. Such pointers live in a userdata box whose __gc frees them.

typedef struct {
    cJSON *json;
    char *str;
} box_t;

static void box_free(box_t *b)
{
    if (b->json) cJSON_Delete(b->json);
    if (b->str) cJSON_free(b->str);
    b->json = NULL;
    b->str = NULL;
}

static int box_gc(lua_State *L)
{
    box_free(lua_touserdata(L, 1));
    return 0;
}

static box_t *box_new(lua_State *L)
{
    box_t *b = lua_newuserdatauv(L, sizeof(box_t), 0);
    *b = (box_t){0};
    if (luaL_newmetatable(L, "hub.box")) {
        lua_pushcfunction(L, box_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    return b;
}

static void push_json(lua_State *L, const cJSON *j, int depth)
{
    if (depth > 32) luaL_error(L, "json: nested too deeply");
    luaL_checkstack(L, 4, "json");
    if (cJSON_IsBool(j)) lua_pushboolean(L, cJSON_IsTrue(j));
    else if (cJSON_IsNumber(j)) {
        double v = j->valuedouble;
        if (v == floor(v) && fabs(v) < 9007199254740992.0) lua_pushinteger(L, (lua_Integer)v);
        else lua_pushnumber(L, v);
    } else if (cJSON_IsString(j)) lua_pushstring(L, j->valuestring);
    else if (cJSON_IsArray(j)) {
        lua_createtable(L, cJSON_GetArraySize(j), 0);
        int i = 1;
        for (const cJSON *c = j->child; c; c = c->next) {
            push_json(L, c, depth + 1);
            lua_rawseti(L, -2, i++);
        }
    } else if (cJSON_IsObject(j)) {
        lua_newtable(L);
        for (const cJSON *c = j->child; c; c = c->next) {
            if (cJSON_IsNull(c)) continue;
            push_json(L, c, depth + 1);
            lua_setfield(L, -2, c->string);
        }
    } else lua_pushnil(L);
}

// Pass 1: validate (may raise errors, allocates nothing outside Lua).
static void json_check(lua_State *L, int idx, int depth)
{
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL: case LUA_TBOOLEAN: case LUA_TNUMBER: case LUA_TSTRING: return;
    case LUA_TTABLE:
        if (depth > 16) luaL_error(L, "json: nested too deeply (or a cycle)");
        luaL_checkstack(L, 4, "json");
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            int kt = lua_type(L, -2);
            if (kt != LUA_TSTRING && kt != LUA_TNUMBER) luaL_error(L, "json: keys must be strings or numbers");
            json_check(L, -1, depth + 1);
            lua_pop(L, 1);
        }
        return;
    default:
        luaL_error(L, "json: can't encode a %s", luaL_typename(L, idx));
    }
}

// Pass 2: build (no Lua errors possible; cJSON allocation failures give NULL).
static cJSON *json_build(lua_State *L, int idx)
{
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN: return cJSON_CreateBool(lua_toboolean(L, idx));
    case LUA_TNUMBER: return cJSON_CreateNumber(lua_tonumber(L, idx));
    case LUA_TSTRING: return cJSON_CreateString(lua_tostring(L, idx));
    case LUA_TTABLE: {
        lua_Unsigned n = lua_rawlen(L, idx);
        size_t count = 0;
        lua_pushnil(L);
        while (lua_next(L, idx)) { count++; lua_pop(L, 1); }
        bool array = n > 0 && count == n;
        cJSON *o = array ? cJSON_CreateArray() : cJSON_CreateObject();
        if (!o) return NULL;
        if (array) {
            for (lua_Unsigned i = 1; i <= n; i++) {
                lua_rawgeti(L, idx, (lua_Integer)i);
                cJSON *c = json_build(L, -1);
                lua_pop(L, 1);
                if (c) cJSON_AddItemToArray(o, c);
            }
        } else {
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                char key[32];
                const char *k;
                if (lua_type(L, -2) == LUA_TNUMBER) {
                    snprintf(key, sizeof(key), LUA_NUMBER_FMT, (LUAI_UACNUMBER)lua_tonumber(L, -2));
                    if (lua_isinteger(L, -2)) snprintf(key, sizeof(key), LUA_INTEGER_FMT, (LUAI_UACINT)lua_tointeger(L, -2));
                    k = key;
                } else {
                    k = lua_tostring(L, -2);
                }
                cJSON *c = json_build(L, -1);
                if (c) cJSON_AddItemToObject(o, k, c);
                lua_pop(L, 1);
            }
        }
        return o;
    }
    default: return cJSON_CreateNull();
    }
}

static int l_json_encode(lua_State *L)
{
    luaL_checkany(L, 1);
    lua_settop(L, 1);
    json_check(L, 1, 0);
    box_t *b = box_new(L);
    b->json = json_build(L, 1);
    if (b->json) b->str = cJSON_PrintUnformatted(b->json);
    if (!b->str) return luaL_error(L, "json: out of memory");
    lua_pushstring(L, b->str);
    box_free(b);  // now, not at the next collection
    return 1;
}

static int l_json_decode(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    box_t *b = box_new(L);
    b->json = cJSON_ParseWithLength(s, len);
    if (!b->json) {
        lua_pushnil(L);
        lua_pushstring(L, "json: parse error");
        return 2;
    }
    push_json(L, b->json, 0);
    cJSON_Delete(b->json);
    b->json = NULL;
    return 1;
}

// ---- libraries: log, zb, state, secret, net, os subset -------------------------------------------

static int l_log(lua_State *L)
{
    script_t *s = auto_script_of(L);
    int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&b, ' ');  // before pushing the value: the buffer must be on top
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    auto_log(s->name, 'I', "%s", lua_tostring(L, -1));
    return 0;
}

static void push_value(lua_State *L, bool is_bool, double v)
{
    if (is_bool) lua_pushboolean(L, v != 0);
    else lua_pushnumber(L, v);
}

static int l_zb_on(lua_State *L)
{
    const char *dev = luaL_checkstring(L, 1);
    const char *attr = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_createtable(L, 0, 3);
    lua_pushstring(L, dev);
    lua_setfield(L, -2, "dev");
    lua_pushstring(L, attr);
    lua_setfield(L, -2, "attr");
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, "fn");
    auto_reg_append(L, "hub.zb");
    return 0;
}

static int l_zb_get(lua_State *L)
{
    const char *dev = luaL_checkstring(L, 1);
    const char *attr = luaL_checkstring(L, 2);
    double v;
    bool is_bool;
    esp_err_t err = zb_get_attr(dev, attr, &v, &is_bool);
    if (err == ESP_OK) {
        push_value(L, is_bool, v);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, err == ESP_ERR_NOT_FOUND ? "no such device" : "no value yet");
    return 2;
}

static int l_zb_devices(lua_State *L)
{
    box_t *b = box_new(L);
    b->json = zb_devices_json();
    lua_newtable(L);
    int i = 1;
    for (const cJSON *d = b->json ? b->json->child : NULL; d; d = d->next) {
        lua_newtable(L);
        const cJSON *v;
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(d, "name"));
        const char *model = cJSON_GetStringValue(cJSON_GetObjectItem(d, "model"));
        lua_pushstring(L, name && name[0] ? name : model ? model : "");
        lua_setfield(L, -2, "name");
        static const char *const strs[] = {"ieee", "model", "manuf", "kind"};
        for (size_t k = 0; k < sizeof(strs) / sizeof(strs[0]); k++) {
            if ((v = cJSON_GetObjectItem(d, strs[k])) && cJSON_IsString(v)) {
                lua_pushstring(L, v->valuestring);
                lua_setfield(L, -2, strs[k]);
            }
        }
        if ((v = cJSON_GetObjectItem(d, "available"))) {
            lua_pushboolean(L, cJSON_IsTrue(v));
            lua_setfield(L, -2, "available");
        }
        lua_rawseti(L, -2, i++);
    }
    box_free(b);  // frees the snapshot now; the box itself is collected later
    return 1;
}

static int l_state_get(lua_State *L)
{
    script_t *s = auto_script_of(L);
    const char *key = luaL_checkstring(L, 1);
    int idx = auto_script_index(s);
    cJSON *o = store_state_lock(idx);
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    int type = 0;
    double num = 0;
    char *str = NULL;
    if (cJSON_IsBool(v)) { type = 1; num = cJSON_IsTrue(v); }
    else if (cJSON_IsNumber(v)) { type = 2; num = v->valuedouble; }
    else if (cJSON_IsString(v)) { type = 3; str = strdup(v->valuestring); }
    store_state_unlock(idx, false);
    if (type == 1) lua_pushboolean(L, num != 0);
    else if (type == 2) {
        if (num == floor(num) && fabs(num) < 9007199254740992.0) lua_pushinteger(L, (lua_Integer)num);
        else lua_pushnumber(L, num);
    } else if (type == 3) {
        lua_pushstring(L, str ? str : "");  // (a memory error here would leak `str`: tiny, rare)
        free(str);
    } else lua_pushnil(L);
    return 1;
}

static int l_state_put(lua_State *L)
{
    script_t *s = auto_script_of(L);
    size_t klen;
    const char *key = luaL_checklstring(L, 1, &klen);
    luaL_argcheck(L, klen > 0 && klen <= 64, 1, "key: 1..64 characters");
    int t = lua_type(L, 2);
    luaL_argexpected(L, t == LUA_TNIL || t == LUA_TNONE || t == LUA_TBOOLEAN || t == LUA_TNUMBER || t == LUA_TSTRING,
                     2, "string, number, boolean or nil");
    if (t == LUA_TSTRING) luaL_argcheck(L, lua_rawlen(L, 2) <= STATE_STR_MAX, 2, "string longer than 1024 bytes");
    if (t == LUA_TNUMBER) luaL_argcheck(L, isfinite(lua_tonumber(L, 2)), 2, "not a finite number");
    int idx = auto_script_index(s);
    cJSON *o = store_state_lock(idx);
    bool full = false;
    if (o) {
        cJSON *v = NULL;
        if (t == LUA_TBOOLEAN) v = cJSON_CreateBool(lua_toboolean(L, 2));
        else if (t == LUA_TNUMBER) v = cJSON_CreateNumber(lua_tonumber(L, 2));
        else if (t == LUA_TSTRING) v = cJSON_CreateString(lua_tostring(L, 2));
        if (cJSON_GetObjectItemCaseSensitive(o, key)) {
            if (v) cJSON_ReplaceItemInObjectCaseSensitive(o, key, v);
            else cJSON_DeleteItemFromObjectCaseSensitive(o, key);
        } else if (v) {
            if (cJSON_GetArraySize(o) >= STATE_KEYS_MAX) { full = true; cJSON_Delete(v); }
            else cJSON_AddItemToObject(o, key, v);
        }
    }
    store_state_unlock(idx, o && !full);
    if (!o) return luaL_error(L, "state: not available");
    if (full) return luaL_error(L, "state: too many keys (max %d)", STATE_KEYS_MAX);
    return 0;
}

static int l_secret(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    char val[AUTO_SECRET_MAX + 1] = "";
    bool found = false;
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    const char *v = store_secret_get(name);
    if (v) {
        strlcpy(val, v, sizeof(val));
        found = true;
    }
    xSemaphoreGive(g_mtx);
    if (!found) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, val);
    memset(val, 0, sizeof(val));
    return 1;
}

static int l_net_online(lua_State *L)
{
    lua_pushboolean(L, wifi5g_is_connected());
    return 1;
}

static int l_net_on(lua_State *L)
{
    const char *evt = luaL_checkstring(L, 1);
    luaL_argcheck(L, strcmp(evt, "up") == 0 || strcmp(evt, "down") == 0, 1, "\"up\" or \"down\"");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_createtable(L, 0, 2);
    lua_pushstring(L, evt);
    lua_setfield(L, -2, "evt");
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "fn");
    auto_reg_append(L, "hub.net");
    return 0;
}

static void open_libs(lua_State *L)
{
    static const struct { const char *name; lua_CFunction open; } std[] = {
        {LUA_GNAME, luaopen_base},          {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},   {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},    {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_OSLIBNAME, luaopen_os},
    };
    for (size_t i = 0; i < sizeof(std) / sizeof(std[0]); i++) {
        luaL_requiref(L, std[i].name, std[i].open, 1);
        lua_pop(L, 1);
    }
    // Sandbox: no files, no code from strings/bytecode, no process control.
    static const char *const drop_g[] = {"dofile", "loadfile", "load", "require"};
    for (size_t i = 0; i < sizeof(drop_g) / sizeof(drop_g[0]); i++) {
        lua_pushnil(L);
        lua_setglobal(L, drop_g[i]);
    }
    lua_getglobal(L, "os");
    static const char *const drop_os[] = {"execute", "exit", "getenv", "remove", "rename", "tmpname", "setlocale"};
    for (size_t i = 0; i < sizeof(drop_os) / sizeof(drop_os[0]); i++) {
        lua_pushnil(L);
        lua_setfield(L, -2, drop_os[i]);
    }
    lua_pop(L, 1);
    lua_getglobal(L, "string");
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);

    lua_register(L, "log", l_log);
    lua_register(L, "print", l_log);
    lua_register(L, "secret", l_secret);
    lua_register(L, "cron", l_cron);

    static const luaL_Reg zb[] = {{"on", l_zb_on}, {"get", l_zb_get}, {"devices", l_zb_devices}, {NULL, NULL}};
    luaL_newlib(L, zb);
    lua_setglobal(L, "zb");
    static const luaL_Reg tm[] = {{"after", l_timer_after}, {"every", l_timer_every}, {"cancel", l_timer_cancel},
                                  {NULL, NULL}};
    luaL_newlib(L, tm);
    lua_setglobal(L, "timer");
    static const luaL_Reg st[] = {{"get", l_state_get}, {"put", l_state_put}, {NULL, NULL}};
    luaL_newlib(L, st);
    lua_setglobal(L, "state");
    static const luaL_Reg js[] = {{"encode", l_json_encode}, {"decode", l_json_decode}, {NULL, NULL}};
    luaL_newlib(L, js);
    lua_setglobal(L, "json");
    static const luaL_Reg net[] = {{"online", l_net_online}, {"on", l_net_on}, {NULL, NULL}};
    luaL_newlib(L, net);
    lua_setglobal(L, "net");
    agpio_open_lib(L);
    ahttp_open_lib(L);
}

// ---- script lifecycle (automation task) ----------------------------------------------------------

static void stop_script(script_t *s, uint8_t status, const char *err)
{
    int idx = auto_script_index(s);
    agpio_release(idx);
    timers_drop(idx);
    if (s->L) lua_close(s->L);
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    s->L = NULL;
    s->gen++;
    s->status = status;
    strlcpy(s->err, err ? err : "", sizeof(s->err));
    xSemaphoreGive(g_mtx);
}

typedef struct {
    const char *src;
    size_t len;
    const char *chunk;
} load_args_t;

// Protected: libraries, compile, then the script's top level.
static int start_p(lua_State *L)
{
    const load_args_t *a = lua_touserdata(L, 1);
    open_libs(L);
    if (luaL_loadbufferx(L, a->src, a->len, a->chunk, "t") != LUA_OK) return lua_error(L);
    auto_call(L, 0);
    return 0;
}

static void start_script(script_t *s, char *src, size_t len)
{
    if (s->L) stop_script(s, ST_STOPPED, NULL);
    s->mem = 0;
    s->mem_peak = 0;
    s->mem_cap = AUTO_MEM_CAP;
    s->calls = 0;
    s->busy_us = 0;
    lua_State *L = lua_newstate(l_alloc, s, esp_random());
    if (!L) {
        free(src);
        auto_log(s->name, 'E', "not started: out of memory");
        stop_script(s, ST_ERROR, "out of memory creating the script");
        return;
    }
    *(script_t **)lua_getextraspace(L) = s;
    lua_sethook(L, hook, LUA_MASKCOUNT, HOOK_EVERY);
    char chunk[AUTO_NAME_MAX + 2];
    snprintf(chunk, sizeof(chunk), "=%s", s->name);
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    s->L = L;
    s->status = ST_RUNNING;
    s->err[0] = '\0';
    s->started_us = esp_timer_get_time();
    xSemaphoreGive(g_mtx);
    load_args_t a = {.src = src, .len = len, .chunk = chunk};
    s_loading = s;
    bool ok = auto_run(s, start_p, &a);
    s_loading = NULL;
    free(src);
    if (!ok) return;
    lua_gc(L, LUA_GCCOLLECT);
    auto_log(s->name, 'I', "started (%u KB of Lua memory)", (unsigned)((s->mem + 1023) / 1024));
    rtc_restore_for(s);
}

// ---- commands from the web API --------------------------------------------------------------------

typedef enum { CMD_CHECK, CMD_START, CMD_STOP } cmd_kind_t;

struct auto_cmd {
    uint8_t kind;
    int idx;
    char name[AUTO_NAME_MAX + 1];
    char *src;                 // owned by the command (freed with it, or taken by START)
    size_t len;
    esp_err_t result;
    char err[200];
    SemaphoreHandle_t done;
    int refs;                  // sender + engine; the last one frees
};

static portMUX_TYPE s_cmd_lock = portMUX_INITIALIZER_UNLOCKED;

static void cmd_release(auto_cmd_t *c)
{
    taskENTER_CRITICAL(&s_cmd_lock);
    int r = --c->refs;
    taskEXIT_CRITICAL(&s_cmd_lock);
    if (r) return;
    free(c->src);
    vSemaphoreDelete(c->done);
    free(c);
}

static void check_syntax(auto_cmd_t *c)
{
    static script_t tmp;  // automation task only
    memset(&tmp, 0, sizeof(tmp));
    tmp.mem_cap = AUTO_MEM_CAP;
    lua_State *L = lua_newstate(l_alloc, &tmp, 0);
    if (!L) {
        c->result = ESP_ERR_NO_MEM;
        strlcpy(c->err, "out of memory", sizeof(c->err));
        return;
    }
    char chunk[AUTO_NAME_MAX + 2];
    snprintf(chunk, sizeof(chunk), "=%s", c->name);
    if (luaL_loadbufferx(L, c->src, c->len, chunk, "t") != LUA_OK) {
        c->result = ESP_ERR_INVALID_ARG;
        strlcpy(c->err, lua_tostring(L, -1) ? lua_tostring(L, -1) : "syntax error", sizeof(c->err));
    } else {
        c->result = ESP_OK;
    }
    lua_close(L);
}

static void handle_cmd(auto_cmd_t *c)
{
    script_t *s = c->idx >= 0 ? &g_scripts[c->idx] : NULL;
    c->result = ESP_OK;
    switch (c->kind) {
    case CMD_CHECK:
        check_syntax(c);
        break;
    case CMD_START: {
        char *src = c->src;
        c->src = NULL;  // start_script frees it
        start_script(s, src, c->len);
        if (s->status != ST_RUNNING) {
            c->result = ESP_FAIL;
            strlcpy(c->err, s->err, sizeof(c->err));
        }
        break;
    }
    case CMD_STOP:
        if (s->L) {
            stop_script(s, ST_STOPPED, NULL);
            auto_log(s->name, 'I', "stopped");
        } else {
            xSemaphoreTake(g_mtx, portMAX_DELAY);
            s->status = ST_STOPPED;
            s->err[0] = '\0';
            xSemaphoreGive(g_mtx);
        }
        break;
    }
    xSemaphoreGive(c->done);
    cmd_release(c);
}

// Sends a command; waits up to `wait_ms` for the result (0 = don't wait).
static esp_err_t run_cmd(uint8_t kind, int idx, const char *name, char *src, size_t len, uint32_t wait_ms,
                         char *err, size_t err_cap)
{
    auto_cmd_t *c = calloc(1, sizeof(*c));
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!c || !done) {
        free(c);
        if (done) vSemaphoreDelete(done);
        free(src);
        return ESP_ERR_NO_MEM;
    }
    *c = (auto_cmd_t){.kind = kind, .idx = idx, .src = src, .len = len, .done = done, .refs = 2};
    strlcpy(c->name, name, sizeof(c->name));
    ev_t ev = {.type = EV_CMD, .cmd = c};
    if (!s_q || xQueueSend(s_q, &ev, pdMS_TO_TICKS(1000)) != pdTRUE) {
        c->refs = 1;
        cmd_release(c);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t res = ESP_OK;
    if (wait_ms) {
        if (xSemaphoreTake(done, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
            res = c->result;
            if (err && err_cap) strlcpy(err, c->err, err_cap);
        } else {
            res = ESP_ERR_TIMEOUT;
            if (err && err_cap) strlcpy(err, "the automation engine is busy", err_cap);
        }
    }
    cmd_release(c);
    return res;
}

// ---- dispatch -------------------------------------------------------------------------------------

typedef struct {
    const zb_event_t *e;
} zb_args_t;

static int zb_p(lua_State *L)
{
    const zb_event_t *e = ((zb_args_t *)lua_touserdata(L, 1))->e;
    if (lua_getfield(L, LUA_REGISTRYINDEX, "hub.zb") != LUA_TTABLE) return 0;
    int list = lua_gettop(L);
    lua_Unsigned n = lua_rawlen(L, list);
    for (lua_Unsigned k = 1; k <= n; k++) {
        lua_rawgeti(L, list, (lua_Integer)k);
        lua_getfield(L, -1, "dev");
        lua_getfield(L, -2, "attr");
        const char *dev = lua_tostring(L, -2), *attr = lua_tostring(L, -1);
        bool match = dev && attr &&
                     (strcmp(dev, "*") == 0 || strcasecmp(dev, e->device) == 0 || strcasecmp(dev, e->ieee) == 0) &&
                     (strcmp(attr, "*") == 0 || strcmp(attr, e->attr) == 0 || (e->alias && strcmp(attr, e->alias) == 0));
        lua_pop(L, 2);
        if (match) {
            lua_getfield(L, -1, "fn");
            push_value(L, e->is_bool, e->value);
            lua_pushstring(L, e->device);
            lua_pushstring(L, e->attr);
            auto_call(L, 3);
        }
        lua_pop(L, 1);
    }
    return 0;
}

static void dispatch_zb(const zb_event_t *e)
{
    zb_args_t a = {.e = e};
    for (int i = 0; i < AUTO_MAX_SCRIPTS; i++) {
        if (g_scripts[i].used && g_scripts[i].status == ST_RUNNING) auto_run(&g_scripts[i], zb_p, &a);
    }
}

typedef struct {
    int pin, level;
} gpio_args_t;

static int gpio_p(lua_State *L)
{
    const gpio_args_t *g = lua_touserdata(L, 1);
    if (lua_getfield(L, LUA_REGISTRYINDEX, "hub.gpio") != LUA_TTABLE) return 0;
    int list = lua_gettop(L);
    lua_Unsigned n = lua_rawlen(L, list);
    for (lua_Unsigned k = 1; k <= n; k++) {
        lua_rawgeti(L, list, (lua_Integer)k);
        lua_getfield(L, -1, "pin");
        lua_getfield(L, -2, "edge");
        int pin = (int)lua_tointeger(L, -2);
        const char *edge = lua_tostring(L, -1);
        bool match = pin == g->pin && edge &&
                     (strcmp(edge, "both") == 0 || strcmp(edge, g->level ? "rising" : "falling") == 0);
        lua_pop(L, 2);
        if (match) {
            lua_getfield(L, -1, "fn");
            lua_pushinteger(L, g->level);
            lua_pushinteger(L, g->pin);
            auto_call(L, 2);
        }
        lua_pop(L, 1);
    }
    return 0;
}

void auto_dispatch_gpio(int script, int pin, int level)
{
    if (script < 0 || script >= AUTO_MAX_SCRIPTS || g_scripts[script].status != ST_RUNNING) return;
    gpio_args_t a = {.pin = pin, .level = level};
    auto_run(&g_scripts[script], gpio_p, &a);
}

static int net_p(lua_State *L)
{
    const char *evt = lua_touserdata(L, 1);
    if (lua_getfield(L, LUA_REGISTRYINDEX, "hub.net") != LUA_TTABLE) return 0;
    int list = lua_gettop(L);
    lua_Unsigned n = lua_rawlen(L, list);
    for (lua_Unsigned k = 1; k <= n; k++) {
        lua_rawgeti(L, list, (lua_Integer)k);
        lua_getfield(L, -1, "evt");
        bool match = lua_tostring(L, -1) && strcmp(lua_tostring(L, -1), evt) == 0;
        lua_pop(L, 1);
        if (match) {
            lua_getfield(L, -1, "fn");
            auto_call(L, 0);
        }
        lua_pop(L, 1);
    }
    return 0;
}

static void check_net(void)
{
    bool up = wifi5g_is_connected();
    if (up == s_online) return;
    s_online = up;
    for (int i = 0; i < AUTO_MAX_SCRIPTS; i++) {
        if (g_scripts[i].used && g_scripts[i].status == ST_RUNNING) auto_run(&g_scripts[i], net_p, (void *)(up ? "up" : "down"));
    }
}

typedef struct {
    ev_t *ev;
} http_args_t;

static int http_p(lua_State *L)
{
    ev_t *ev = ((http_args_t *)lua_touserdata(L, 1))->ev;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ev->http.ref);
    luaL_unref(L, LUA_REGISTRYINDEX, ev->http.ref);
    lua_pushinteger(L, ev->http.status);
    if (ev->http.body) lua_pushlstring(L, ev->http.body, ev->http.len);
    else lua_pushstring(L, ev->http.err);
    free(ev->http.body);  // copied into Lua (or a memory error: freed by the caller then)
    ev->http.body = NULL;
    auto_call(L, 2);
    return 0;
}

static void handle_http(ev_t *ev)
{
    script_t *s = ev->http.script < AUTO_MAX_SCRIPTS ? &g_scripts[ev->http.script] : NULL;
    if (s && s->status == ST_RUNNING && s->gen == ev->http.gen && ev->http.ref != LUA_NOREF) {
        http_args_t a = {.ev = ev};
        auto_run(s, http_p, &a);
    }
    free(ev->http.body);  // stale (script restarted) or not consumed
}

// ---- the task -------------------------------------------------------------------------------------

static void auto_task(void *arg)
{
    ESP_LOGI(TAG, "engine running");
    s_online = wifi5g_is_connected();
    int64_t next_timer = INT64_MAX, next_gpio = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        int64_t wake = now + 1000000;  // network state and cron: once a second
        if (next_timer < wake) wake = next_timer;
        if (next_gpio && next_gpio < wake) wake = next_gpio;
        int64_t wait_ms = (wake - now + 999) / 1000;
        if (wait_ms < 0) wait_ms = 0;
        ev_t ev;
        if (xQueueReceive(s_q, &ev, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
            switch (ev.type) {
            case EV_ZB: dispatch_zb(&ev.zb); break;
            case EV_GPIO: agpio_on_isr_event(ev.gpio.pin, esp_timer_get_time()); break;
            case EV_HTTP: handle_http(&ev); break;
            case EV_CMD: handle_cmd(ev.cmd); break;
            }
        }
        now = esp_timer_get_time();
        next_timer = run_timers(now);
        next_gpio = agpio_tick(esp_timer_get_time());
        check_net();
        run_cron();
    }
}

// ---- public API (HTTP server task, main task) --------------------------------------------------------

bool auto_loading(lua_State *L)
{
    return s_loading && s_loading == auto_script_of(L);
}

static int find_script(const char *name)
{
    for (int i = 0; i < AUTO_MAX_SCRIPTS; i++) {
        if (g_scripts[i].used && strcmp(g_scripts[i].name, name) == 0) return i;
    }
    return -1;
}

static const char *status_str(uint8_t st)
{
    return st == ST_RUNNING ? "running" : st == ST_ERROR ? "error" : "stopped";
}

cJSON *automation_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "scripts");
    int timers[AUTO_MAX_SCRIPTS] = {0}, crons[AUTO_MAX_SCRIPTS] = {0};
    xSemaphoreTake(s_tmtx, portMAX_DELAY);
    for (int i = 0; i < MAX_TIMERS; i++) if (s_timers[i].used) timers[s_timers[i].script]++;
    for (int i = 0; i < MAX_CRON; i++) if (s_cron[i].used) crons[s_cron[i].script]++;
    xSemaphoreGive(s_tmtx);
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    for (int i = 0; i < AUTO_MAX_SCRIPTS; i++) {
        const script_t *s = &g_scripts[i];
        if (!s->used) continue;
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", s->name);
        cJSON_AddBoolToObject(j, "enabled", s->enabled);
        cJSON_AddStringToObject(j, "status", status_str(s->status));
        cJSON_AddStringToObject(j, "error", s->err);
        cJSON_AddNumberToObject(j, "mem", (double)s->mem);
        cJSON_AddNumberToObject(j, "mem_peak", (double)s->mem_peak);
        cJSON_AddNumberToObject(j, "mem_cap", (double)s->mem_cap);
        cJSON_AddNumberToObject(j, "calls", s->calls);
        cJSON_AddNumberToObject(j, "busy_ms", (double)(s->busy_us / 1000));
        cJSON_AddNumberToObject(j, "timers", timers[i]);
        cJSON_AddNumberToObject(j, "cron", crons[i]);
        cJSON_AddItemToArray(arr, j);
    }
    cJSON_AddItemToObject(o, "secrets", store_secret_names());
    xSemaphoreGive(g_mtx);
    cJSON_AddStringToObject(o, "tz", store_tz());
    cJSON_AddBoolToObject(o, "clock_set", hublog_time_valid());
    if (hublog_time_valid()) {
        time_t t = time(NULL);
        struct tm tm;
        char buf[32];
        localtime_r(&t, &tm);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M %Z", &tm);
        cJSON_AddStringToObject(o, "local_time", buf);
    }
    cJSON_AddNumberToObject(o, "zb_events_dropped", s_zb_drops);
    // Lowest free stack of the engine task since boot (bytes): the C-stack margin left by
    // the deepest script so far (parser nesting, pattern matching, nested calls).
    cJSON_AddNumberToObject(o, "stack_free_min", s_task ? (double)uxTaskGetStackHighWaterMark(s_task) : -1);
    cJSON_AddItemToObject(o, "gpio", agpio_json());
    cJSON_AddItemToObject(o, "led", agpio_led_json());
    cJSON *b = cJSON_AddObjectToObject(o, "boot");
    cJSON_AddBoolToObject(b, "safe_mode", s_safe_mode);
    cJSON_AddNumberToObject(b, "abnormal_resets", s_boot_resets);
    cJSON_AddStringToObject(b, "last_reset", s_last_reset);
    cJSON_AddBoolToObject(b, "rtc_kept", s_rtc_kept);
    return o;
}

char *automation_source(const char *name)
{
    if (!store_valid_name(name)) return NULL;
    size_t len;
    return store_read_script(name, &len);
}

int automation_enabled_count(void)
{
    int n = 0;
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    for (int i = 0; i < AUTO_MAX_SCRIPTS; i++) n += g_scripts[i].used && g_scripts[i].enabled;
    xSemaphoreGive(g_mtx);
    return n;
}

static char *copy_src(const char *src, size_t len)
{
    char *c = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c) {
        memcpy(c, src, len);
        c[len] = '\0';
    }
    return c;
}

esp_err_t automation_save(const char *name, const char *src, size_t len, bool enabled, char *err, size_t err_cap)
{
    if (err_cap) err[0] = '\0';
    if (!store_valid_name(name)) {
        strlcpy(err, "name: 1-24 characters, a-z 0-9 _ -", err_cap);
        return ESP_ERR_INVALID_ARG;
    }
    if (len > AUTO_SCRIPT_MAX) {
        snprintf(err, err_cap, "script longer than %d KB", AUTO_SCRIPT_MAX / 1024);
        return ESP_ERR_INVALID_SIZE;
    }
    char *copy = copy_src(src, len);
    if (!copy) return ESP_ERR_NO_MEM;
    esp_err_t r = run_cmd(CMD_CHECK, -1, name, copy, len, 10000, err, err_cap);
    if (r != ESP_OK) return r;
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    int idx = find_script(name);
    if (idx < 0) {
        for (int i = 0; i < AUTO_MAX_SCRIPTS && idx < 0; i++) {
            if (!g_scripts[i].used) {
                idx = i;
                memset(&g_scripts[i], 0, sizeof(g_scripts[i]));
                g_scripts[i].used = true;
                strlcpy(g_scripts[i].name, name, sizeof(g_scripts[i].name));
            }
        }
    }
    if (idx >= 0) g_scripts[idx].enabled = enabled;
    xSemaphoreGive(g_mtx);
    if (idx < 0) {
        snprintf(err, err_cap, "too many scripts (max %d)", AUTO_MAX_SCRIPTS);
        return ESP_ERR_NO_MEM;
    }
    r = store_write_script(name, src, len);
    if (r == ESP_OK) r = store_write_meta();
    if (r != ESP_OK) {
        strlcpy(err, "could not write to flash", err_cap);
        return r;
    }
    store_state_load(idx);
    if (!enabled) return run_cmd(CMD_STOP, idx, name, NULL, 0, 10000, err, err_cap);
    copy = copy_src(src, len);
    if (!copy) return ESP_ERR_NO_MEM;
    r = run_cmd(CMD_START, idx, name, copy, len, 10000, err, err_cap);
    // Saved even if it failed to start (e.g. a GPIO conflict): ESP_OK with `err` set,
    // so the editor can show why at once; it is also the script's status.
    return r == ESP_FAIL ? ESP_OK : r;
}

esp_err_t automation_set_enabled(const char *name, bool enabled)
{
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    int idx = find_script(name);
    if (idx >= 0) g_scripts[idx].enabled = enabled;
    xSemaphoreGive(g_mtx);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    esp_err_t r = store_write_meta();
    if (r != ESP_OK) return r;
    if (!enabled) return run_cmd(CMD_STOP, idx, name, NULL, 0, 10000, NULL, 0);
    size_t len;
    char *src = store_read_script(name, &len);
    if (!src) return ESP_ERR_NOT_FOUND;
    r = run_cmd(CMD_START, idx, name, src, len, 10000, NULL, 0);
    return r == ESP_FAIL ? ESP_OK : r;
}

esp_err_t automation_restart(const char *name)
{
    return automation_set_enabled(name, true);
}

esp_err_t automation_delete(const char *name)
{
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    int idx = find_script(name);
    xSemaphoreGive(g_mtx);
    if (idx < 0) return ESP_ERR_NOT_FOUND;
    esp_err_t r = run_cmd(CMD_STOP, idx, name, NULL, 0, 10000, NULL, 0);
    if (r != ESP_OK) return r;
    store_delete_script(name);
    store_state_forget(idx, true);
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    g_scripts[idx].used = false;
    xSemaphoreGive(g_mtx);
    auto_log(name, 'I', "deleted");
    return store_write_meta();
}

esp_err_t automation_secret_set(const char *name, const char *value)
{
    return store_secret_set(name, value);
}

esp_err_t automation_set_tz(const char *tz)
{
    return store_set_tz(tz);
}

esp_err_t automation_set_gpio(const cJSON *pins, char *err, size_t err_cap)
{
    return agpio_set_config(pins, err, err_cap);
}

static void *psram_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

// Boot: was a script running when the previous boot ended in a crash or a watchdog
// reset? Then disable it (persisted) and say why. A power glitch or brownout isn't the
// script's fault (a loose buzzer can cause one), so those don't count.
static void disable_for_reset(int idx, const char *why)
{
    script_t *s = &g_scripts[idx];
    if (!s->enabled) return;
    s->enabled = false;
    s->status = ST_ERROR;
    snprintf(s->err, sizeof(s->err),
             "the hub reset (%s) while this script's buzzer was playing; the script was disabled so it can't "
             "happen again. Check the buzzer's contacts, then enable it again", why);
    ESP_LOGE(TAG, "the hub reset (%s) while script '%s' had a buzzer playing: script disabled", why, s->name);
    auto_log(s->name, 'E', "disabled: the hub reset (%s) while its buzzer was playing", why);
    if (store_write_meta() != ESP_OK) ESP_LOGE(TAG, "could not save the disabled state");
}

// Boot-loop guard, buzzer marker: scripts whose buzzer was playing when an abnormal reset
// ended the previous boot are disabled (crash guard below only sees Lua running; a pattern
// plays in C after the script returned).
static void blame_buzzer(esp_reset_reason_t r)
{
    char names[MAX_BUZ_BLAME][AUTO_NAME_MAX + 1];
    int n = agpio_take_marker(names, MAX_BUZ_BLAME);
    if (!reset_abnormal(r)) return;
    for (int i = 0; i < n; i++) {
        int idx = find_script(names[i]);
        if (idx >= 0) disable_for_reset(idx, reset_words(r));
    }
}

static void blame_previous_crash(void)
{
    if (s_rtc_run.magic != RUN_MAGIC) return;
    s_rtc_run.magic = 0;
    s_rtc_run.name[AUTO_NAME_MAX] = '\0';
    esp_reset_reason_t r = esp_reset_reason();
    const char *what = r == ESP_RST_PANIC ? "crashed"
                     : (r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) ? "hung (watchdog reset)"
                     : NULL;
    if (!what) return;
    int idx = find_script(s_rtc_run.name);
    if (idx < 0) return;
    script_t *s = &g_scripts[idx];
    s->enabled = false;
    s->status = ST_ERROR;
    snprintf(s->err, sizeof(s->err),
             "the hub %s while this script was running and restarted; the script was disabled so it can't "
             "happen again. Enable it again once it's fixed", what);
    ESP_LOGE(TAG, "the hub %s while script '%s' was running: script disabled", what, s->name);
    auto_log(s->name, 'E', "disabled: the hub %s while it was running", what);
    if (store_write_meta() != ESP_OK) ESP_LOGE(TAG, "could not save the disabled state");
}

static void boot_ok_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(BOOT_OK_US / 1000));
    nvs_handle_t h;
    if (nvs_open(BOOT_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "n", 0);
        nvs_set_u8(h, "up", 1);
        nvs_commit(h);
        nvs_close(h);
    }
    s_boot_resets = 0;
    vTaskDelete(NULL);
}

// Counts abnormal resets that came less than 60 s after boot (main task, at boot).
static void boot_count(esp_reset_reason_t r)
{
    s_rtc_kept = s_rtc_canary[0] == CANARY && s_rtc_canary[1] == ~CANARY;
    s_rtc_canary[0] = CANARY;
    s_rtc_canary[1] = ~CANARY;
    s_last_reset = reset_words(r);
    uint8_t n = 0, up = 1;
    nvs_handle_t h;
    if (nvs_open(BOOT_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u8(h, "n", &n);
        nvs_get_u8(h, "up", &up);
        n = (reset_abnormal(r) && !up) ? (uint8_t)(n < 250 ? n + 1 : n) : 0;
        nvs_set_u8(h, "n", n);
        nvs_set_u8(h, "up", 0);
        nvs_commit(h);
        nvs_close(h);
    }
    s_boot_resets = n;
    s_safe_mode = n >= SAFE_AFTER;
    ESP_LOGI(TAG, "reset: %s; RTC memory %s; abnormal resets in a row: %u%s", s_last_reset,
             s_rtc_kept ? "kept" : "not kept", n, s_safe_mode ? " -> SAFE MODE, scripts not started" : "");
    if (xTaskCreate(boot_ok_task, "boot_ok", 3072, NULL, 1, NULL) != pdPASS) ESP_LOGW(TAG, "boot_ok task not created");
}

static int start_enabled(void)
{
    int n = 0;
    for (int i = 0; i < AUTO_MAX_SCRIPTS; i++) {
        script_t *s = &g_scripts[i];
        if (!s->used || !s->enabled || s->L) continue;
        size_t len;
        char *src = store_read_script(s->name, &len);
        if (!src) {
            auto_log(s->name, 'E', "not started: its file is missing");
            continue;
        }
        if (run_cmd(CMD_START, i, s->name, src, len, 0, NULL, 0) == ESP_OK) n++;
    }
    return n;
}

bool automation_safe_mode(void)
{
    return s_safe_mode;
}

esp_err_t automation_leave_safe_mode(void)
{
    if (!s_safe_mode) return ESP_OK;
    s_safe_mode = false;
    int n = start_enabled();
    ESP_LOGI(TAG, "safe mode left: %d script(s) starting", n);
    auto_log("hub", 'I', "safe mode left by the user: %d script(s) starting", n);
    return ESP_OK;
}

#ifdef HUB_TEST_HOOKS
// Test builds only: /api/debug/crash {"boots": N} makes each of the next N boots abort 5 s
// in (safe-mode test). Main task: NVS is fine here.
static void test_crash_cb(void *arg) { abort(); }
static void test_boot_crash(void)
{
    nvs_handle_t h;
    uint8_t n = 0;
    if (nvs_open("hubtest", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_get_u8(h, "crashn", &n);
    if (n) {
        nvs_set_u8(h, "crashn", (uint8_t)(n - 1));
        nvs_commit(h);
        static esp_timer_handle_t t;
        esp_timer_create_args_t a = {.callback = test_crash_cb, .name = "testboot"};
        esp_timer_create(&a, &t);
        esp_timer_start_once(t, 5000000);
        ESP_LOGW(TAG, "TEST: this boot aborts in 5 s (%u more after it)", n - 1);
    }
    nvs_close(h);
}
#endif

esp_err_t automation_start(void)
{
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    g_scripts = heap_caps_calloc(AUTO_MAX_SCRIPTS, sizeof(script_t), MALLOC_CAP_SPIRAM);
    s_timers = heap_caps_calloc(MAX_TIMERS, sizeof(atimer_t), MALLOC_CAP_SPIRAM);
    s_cron = heap_caps_calloc(MAX_CRON, sizeof(acron_t), MALLOC_CAP_SPIRAM);
    s_log = heap_caps_calloc(LOG_LINES, sizeof(logline_t), MALLOC_CAP_SPIRAM);
    g_mtx = xSemaphoreCreateMutex();
    s_tmtx = xSemaphoreCreateMutex();
    s_log_mtx = xSemaphoreCreateMutex();
    s_q = xQueueCreateWithCaps(QUEUE_LEN, sizeof(ev_t), MALLOC_CAP_SPIRAM);
    if (!g_scripts || !s_timers || !s_cron || !s_log || !g_mtx || !s_tmtx || !s_log_mtx || !s_q) {
        ESP_LOGE(TAG, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    rtc_take();
    store_init_dirs();
    store_secrets_load();
    setenv("TZ", store_tz(), 1);
    tzset();
    store_load_meta();
    esp_reset_reason_t rr = esp_reset_reason();
    boot_count(rr);
    blame_buzzer(rr);
    blame_previous_crash();
    for (int i = 0; i < AUTO_MAX_SCRIPTS; i++) if (g_scripts[i].used) store_state_load(i);
    agpio_init();
    zb_set_event_cb(on_zb_event);

    if (xTaskCreateWithCaps(auto_task, "auto", TASK_STACK, NULL, 3, &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "engine task not created");
        return ESP_ERR_NO_MEM;
    }
    ahttp_start();
    store_io_start();
    esp_register_shutdown_handler(on_shutdown);

    if (s_safe_mode) {
        ESP_LOGW(TAG, "safe mode: %u abnormal resets in a row soon after starting; scripts not started", s_boot_resets);
        auto_log("hub", 'W', "safe mode: the hub reset %u times in a row soon after starting (last: %s); scripts were "
                 "not started", s_boot_resets, s_last_reset);
#ifdef HUB_TEST_HOOKS
        test_boot_crash();
#endif
        return ESP_OK;
    }
    int n = start_enabled();
    ESP_LOGI(TAG, "%d script(s) starting%s", n, s_restore ? " (timers from before the restart pending)" : "");
#ifdef HUB_TEST_HOOKS
    test_boot_crash();
#endif
    return ESP_OK;
}
