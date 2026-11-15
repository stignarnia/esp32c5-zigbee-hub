// Entities scripts publish to Home Assistant: ha.sensor, ha.binary, ha.event, ha.remove.
//
// The first call with a name defines the entity; options given on a later call change
// its definition. Definitions are persisted (the HA entity and its history stay while
// the script is stopped or disabled), values are not. The Home Assistant API server
// (components/esphome_api) reads this table with automation_ha_entities(); the table
// works the same whether that integration is on or off.
//
// Threads: the Lua functions run on the automation task (PSRAM stack: no file I/O), the
// definitions file is written by the auto_io task (aha_flush), readers are any task.
// Everything in s_ent is under s_mtx.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "auto_internal.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lauxlib.h"

static const char *TAG = "auto_ha";

typedef struct {
    bool used;
    auto_ha_entity_t e;
} slot_t;

static slot_t *s_ent;                    // AUTO_HA_MAX, PSRAM
static SemaphoreHandle_t s_mtx;
static volatile uint32_t s_rev;
static volatile bool s_dirty;            // definitions changed: write the file
static auto_ha_event_cb_t s_event_cb;

static const char *const STATE_CLASSES[] = {"", "measurement", "total_increasing", "total"};

#define LOCK()   xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

static slot_t *find(const char *script, const char *name)
{
    for (int i = 0; i < AUTO_HA_MAX; i++) {
        if (s_ent[i].used && strcmp(s_ent[i].e.script, script) == 0 && strcmp(s_ent[i].e.name, name) == 0) {
            return &s_ent[i];
        }
    }
    return NULL;
}

static int count_of(const char *script)
{
    int n = 0;
    for (int i = 0; i < AUTO_HA_MAX; i++) n += s_ent[i].used && strcmp(s_ent[i].e.script, script) == 0;
    return n;
}

// ---- persistence ------------------------------------------------------------------------------

void aha_init(void)
{
    s_ent = heap_caps_calloc(AUTO_HA_MAX, sizeof(slot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mtx = xSemaphoreCreateMutex();
    if (!s_ent || !s_mtx) {
        ESP_LOGE(TAG, "out of memory");
        abort();
    }
    size_t len;
    char *txt = store_read_file(AUTO_HA_DEFS, 32 * 1024, &len);
    if (!txt) return;
    cJSON *arr = cJSON_Parse(txt);
    free(txt);
    int n = 0;
    const cJSON *j;
    cJSON_ArrayForEach(j, arr) {
        if (n >= AUTO_HA_MAX) break;
        const char *script = cJSON_GetStringValue(cJSON_GetObjectItem(j, "s"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(j, "n"));
        int type = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "t"));
        if (!script || !name || !store_valid_name(script) || type < AUTO_HA_SENSOR || type > AUTO_HA_EVENT) continue;
        auto_ha_entity_t *e = &s_ent[n].e;
        strlcpy(e->script, script, sizeof(e->script));
        strlcpy(e->name, name, sizeof(e->name));
        e->type = type;
        const char *v;
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(j, "u")))) strlcpy(e->unit, v, sizeof(e->unit));
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(j, "dc")))) strlcpy(e->device_class, v, sizeof(e->device_class));
        if ((v = cJSON_GetStringValue(cJSON_GetObjectItem(j, "i")))) strlcpy(e->icon, v, sizeof(e->icon));
        e->decimals = (int8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "d"));
        double sc = cJSON_GetNumberValue(cJSON_GetObjectItem(j, "sc"));
        e->state_class = sc >= 0 && sc <= 3 ? (uint8_t)sc : 0;
        const cJSON *t;
        cJSON_ArrayForEach(t, cJSON_GetObjectItem(j, "ev")) {
            if (cJSON_IsString(t) && e->n_types < AUTO_HA_EVENT_TYPES) {
                strlcpy(e->types[e->n_types++], t->valuestring, AUTO_HA_TYPE_LEN);
            }
        }
        s_ent[n++].used = true;
    }
    cJSON_Delete(arr);
    ESP_LOGI(TAG, "%d entities published by scripts", n);
}

void aha_flush(void)
{
    if (!s_dirty) return;
    s_dirty = false;
    cJSON *arr = cJSON_CreateArray();
    LOCK();
    for (int i = 0; i < AUTO_HA_MAX; i++) {
        if (!s_ent[i].used) continue;
        const auto_ha_entity_t *e = &s_ent[i].e;
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "s", e->script);
        cJSON_AddStringToObject(j, "n", e->name);
        cJSON_AddNumberToObject(j, "t", e->type);
        if (e->unit[0]) cJSON_AddStringToObject(j, "u", e->unit);
        if (e->device_class[0]) cJSON_AddStringToObject(j, "dc", e->device_class);
        if (e->icon[0]) cJSON_AddStringToObject(j, "i", e->icon);
        if (e->decimals) cJSON_AddNumberToObject(j, "d", e->decimals);
        if (e->state_class) cJSON_AddNumberToObject(j, "sc", e->state_class);
        if (e->n_types) {
            cJSON *ev = cJSON_AddArrayToObject(j, "ev");
            for (int t = 0; t < e->n_types; t++) cJSON_AddItemToArray(ev, cJSON_CreateString(e->types[t]));
        }
        cJSON_AddItemToArray(arr, j);
    }
    UNLOCK();
    char *txt = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!txt || store_write_file(AUTO_HA_DEFS, txt, strlen(txt)) != ESP_OK) s_dirty = true;  // next round
    cJSON_free(txt);
}

// ---- for the API server -------------------------------------------------------------------------

int automation_ha_entities(auto_ha_entity_t *out, int max)
{
    if (!s_ent) return 0;
    int n = 0;
    LOCK();
    for (int i = 0; i < AUTO_HA_MAX && n < max; i++) {
        if (s_ent[i].used) out[n++] = s_ent[i].e;
    }
    UNLOCK();
    return n;
}

uint32_t automation_ha_revision(void)
{
    return s_rev;
}

void automation_ha_set_event_cb(auto_ha_event_cb_t cb)
{
    s_event_cb = cb;
}

void aha_script_stopped(const char *script)
{
    LOCK();
    for (int i = 0; i < AUTO_HA_MAX; i++) {
        if (s_ent[i].used && strcmp(s_ent[i].e.script, script) == 0) s_ent[i].e.has_value = false;
    }
    s_rev++;
    UNLOCK();
}

void aha_script_deleted(const char *script)
{
    LOCK();
    for (int i = 0; i < AUTO_HA_MAX; i++) {
        if (s_ent[i].used && strcmp(s_ent[i].e.script, script) == 0) {
            s_ent[i].used = false;
            s_dirty = true;
        }
    }
    s_rev++;
    UNLOCK();
}

// ---- Lua ----------------------------------------------------------------------------------------

static const char *check_name(lua_State *L, int arg)
{
    size_t n;
    const char *name = luaL_checklstring(L, arg, &n);
    luaL_argcheck(L, n > 0 && n <= AUTO_HA_NAME, arg, "name: 1..32 characters");
    for (size_t i = 0; i < n; i++) {
        luaL_argcheck(L, (unsigned char)name[i] >= 0x20 && name[i] != 0x7f, arg, "name: no control characters");
    }
    return name;
}

// Optional string option `key` of the table at `opts` into dst; true if it was given.
static bool opt_str(lua_State *L, int opts, const char *key, char *dst, size_t cap)
{
    if (lua_type(L, opts) != LUA_TTABLE) return false;
    int t = lua_getfield(L, opts, key);
    bool given = t != LUA_TNIL;
    if (given) {
        size_t n;
        const char *v = lua_tolstring(L, -1, &n);
        if (t != LUA_TSTRING || n >= cap) luaL_error(L, "option %s: a string of at most %d characters", key, (int)cap - 1);
        memcpy(dst, v, n + 1);
    }
    lua_pop(L, 1);
    return given;
}

// Finds or creates the entity (script, name) of `type`. Under s_mtx on return (the caller
// unlocks); raises a Lua error (unlocked) if it can't.
static auto_ha_entity_t *entity_for(lua_State *L, const char *name, uint8_t type, bool *created)
{
    static const char *const KIND[] = {"", "a sensor", "a binary sensor", "an event"};
    const char *script = auto_script_of(L)->name;
    LOCK();
    slot_t *sl = find(script, name);
    *created = false;
    if (sl && sl->e.type != type) {
        uint8_t was = sl->e.type;
        UNLOCK();
        luaL_error(L, "ha: \"%s\" is already %s (ha.remove it first)", name, KIND[was]);
    }
    if (!sl) {
        const char *why = count_of(script) >= AUTO_HA_PER_SCRIPT ? "this script has 8 already" : NULL;
        for (int i = 0; i < AUTO_HA_MAX && !sl && !why; i++) {
            if (!s_ent[i].used) sl = &s_ent[i];
        }
        if (!sl) {
            UNLOCK();
            luaL_error(L, "ha: too many entities (%s)", why ? why : "32 for all scripts together");
        }
        memset(sl, 0, sizeof(*sl));
        sl->used = true;
        strlcpy(sl->e.script, script, sizeof(sl->e.script));
        strlcpy(sl->e.name, name, sizeof(sl->e.name));
        sl->e.type = type;
        *created = true;
    }
    return &sl->e;
}

// Options common to all types. Validated into a scratch copy first (opt_str can raise an
// error, which must not happen while s_mtx is held).
typedef struct {
    char unit[16], device_class[32], icon[48], state_class[20];
    bool has_unit, has_dc, has_icon, has_sc, has_dec;
    int decimals;
} opts_t;

static void read_opts(lua_State *L, int arg, opts_t *o, bool sensor)
{
    memset(o, 0, sizeof(*o));
    int t = lua_type(L, arg);
    luaL_argexpected(L, t == LUA_TNONE || t == LUA_TNIL || t == LUA_TTABLE, arg, "options table");
    if (t != LUA_TTABLE) return;
    o->has_dc = opt_str(L, arg, "device_class", o->device_class, sizeof(o->device_class));
    o->has_icon = opt_str(L, arg, "icon", o->icon, sizeof(o->icon));
    if (!sensor) return;
    o->has_unit = opt_str(L, arg, "unit", o->unit, sizeof(o->unit));
    o->has_sc = opt_str(L, arg, "state_class", o->state_class, sizeof(o->state_class));
    if (o->has_sc) {
        bool ok = false;
        for (int i = 0; i < 4; i++) ok |= strcmp(o->state_class, STATE_CLASSES[i]) == 0;
        if (!ok) luaL_error(L, "option state_class: \"measurement\", \"total_increasing\", \"total\" or \"\"");
    }
    if (lua_getfield(L, arg, "decimals") != LUA_TNIL) {
        lua_Integer d = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : -1;
        if (d < 0 || d > 6) luaL_error(L, "option decimals: an integer 0..6");
        o->decimals = (int)d;
        o->has_dec = true;
    }
    lua_pop(L, 1);
}

// Applies the options to the definition; true if it changed.
static bool apply_opts(auto_ha_entity_t *e, const opts_t *o)
{
    bool ch = false;
    if (o->has_dc && strcmp(e->device_class, o->device_class)) { strlcpy(e->device_class, o->device_class, sizeof(e->device_class)); ch = true; }
    if (o->has_icon && strcmp(e->icon, o->icon)) { strlcpy(e->icon, o->icon, sizeof(e->icon)); ch = true; }
    if (o->has_unit && strcmp(e->unit, o->unit)) { strlcpy(e->unit, o->unit, sizeof(e->unit)); ch = true; }
    if (o->has_dec && e->decimals != o->decimals) { e->decimals = (int8_t)o->decimals; ch = true; }
    if (o->has_sc) {
        uint8_t sc = 0;
        for (uint8_t i = 0; i < 4; i++) if (strcmp(o->state_class, STATE_CLASSES[i]) == 0) sc = i;
        if (e->state_class != sc) { e->state_class = sc; ch = true; }
    }
    return ch;
}

// ha.sensor(name, value [, {unit=, device_class=, decimals=, state_class=, icon=}])
// value: a number, or nil for unknown.
static int l_sensor(lua_State *L)
{
    const char *name = check_name(L, 1);
    int t = lua_type(L, 2);
    luaL_argexpected(L, t == LUA_TNUMBER || t == LUA_TNIL, 2, "number or nil");
    double v = t == LUA_TNUMBER ? lua_tonumber(L, 2) : 0;
    if (t == LUA_TNUMBER) luaL_argcheck(L, isfinite(v), 2, "not a finite number");
    opts_t o;
    read_opts(L, 3, &o, true);
    bool created;
    auto_ha_entity_t *e = entity_for(L, name, AUTO_HA_SENSOR, &created);
    if (apply_opts(e, &o) || created) s_dirty = true;
    e->has_value = t == LUA_TNUMBER;
    e->value = (float)v;
    s_rev++;
    UNLOCK();
    return 0;
}

// ha.binary(name, on [, {device_class=, icon=}]); on: true/false, or nil for unknown.
static int l_binary(lua_State *L)
{
    const char *name = check_name(L, 1);
    int t = lua_type(L, 2);
    luaL_argexpected(L, t == LUA_TBOOLEAN || t == LUA_TNIL, 2, "boolean or nil");
    opts_t o;
    read_opts(L, 3, &o, false);
    bool created;
    auto_ha_entity_t *e = entity_for(L, name, AUTO_HA_BINARY, &created);
    if (apply_opts(e, &o) || created) s_dirty = true;
    e->has_value = t == LUA_TBOOLEAN;
    e->on = lua_toboolean(L, 2);
    s_rev++;
    UNLOCK();
    return 0;
}

// ha.event(name [, type="fired" [, {device_class=, icon=}]]). Home Assistant has to know
// every event type in advance: a type not used before is added (up to 4 per entity),
// which makes Home Assistant reload the hub's entities once.
static int l_event(lua_State *L)
{
    const char *name = check_name(L, 1);
    size_t tl;
    const char *type = luaL_optlstring(L, 2, "fired", &tl);
    luaL_argcheck(L, tl > 0 && tl < AUTO_HA_TYPE_LEN, 2, "type: 1..15 characters");
    for (size_t i = 0; i < tl; i++) {
        char c = type[i];
        luaL_argcheck(L, (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_', 2, "type: a-z 0-9 _");
    }
    opts_t o;
    read_opts(L, 3, &o, false);
    bool created;
    auto_ha_entity_t *e = entity_for(L, name, AUTO_HA_EVENT, &created);
    bool known = false;
    for (int i = 0; i < e->n_types; i++) known |= strcmp(e->types[i], type) == 0;
    if (!known && e->n_types >= AUTO_HA_EVENT_TYPES) {
        UNLOCK();
        return luaL_error(L, "ha.event: \"%s\" has 4 event types already", name);
    }
    if (!known) strlcpy(e->types[e->n_types++], type, AUTO_HA_TYPE_LEN);
    if (apply_opts(e, &o) || created || !known) s_dirty = true;
    s_rev++;
    UNLOCK();
    if (s_event_cb) s_event_cb(auto_script_of(L)->name, name, type);
    return 0;
}

// ha.remove(name): the entity disappears from Home Assistant (with its history).
static int l_remove(lua_State *L)
{
    const char *name = check_name(L, 1);
    LOCK();
    slot_t *sl = find(auto_script_of(L)->name, name);
    if (sl) {
        sl->used = false;
        s_dirty = true;
        s_rev++;
    }
    UNLOCK();
    lua_pushboolean(L, sl != NULL);
    return 1;
}

void aha_open_lib(lua_State *L)
{
    static const luaL_Reg ha[] = {{"sensor", l_sensor}, {"binary", l_binary}, {"event", l_event},
                                  {"remove", l_remove}, {NULL, NULL}};
    luaL_newlib(L, ha);
    lua_setglobal(L, "ha");
}
