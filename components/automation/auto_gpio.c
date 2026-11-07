// GPIO for scripts: pins enabled in Automations → GPIO, claimed by one script at a time,
// all driven low again when that script stops (fail-safe: a crashed script never
// leaves a buzzer on).
//
// Claims happen only while a script loads (its top level: on Save, enable, boot): any
// gpio call there claims the pin, and gpio.claim() claims pins used only in handlers.
// A handler can't claim a new pin, so a conflict with another script is reported when
// the script is saved, never at 3 am when the door opens. Reading needs no claim.
//
// Buzzer patterns are timed in C by an esp_timer, not by Lua: 30-40 ms steps stay
// exact whatever the scripts are doing, and the script returns at once.
//
// Allowed pins (Phase 1b, CLAUDE.md): on the header and not strapping (2, 7, 25, 27,
// 28), USB (13, 14), UART0 (11, 12), 32 kHz crystal (0, 1) or flash/PSRAM.
// Outputs use drive strength 3 (measured with the 12 mm buzzers: 3.0 V loaded).
#include <stdio.h>
#include <string.h>

#include "auto_internal.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lauxlib.h"

static const char *TAG = "auto";

static const uint8_t ALLOWED[] = {3, 4, 5, 6, 8, 9, 10, 15, 23, 24, 26};
#define NPINS          32
#define MAX_CFG        (sizeof(ALLOWED))
#define DEBOUNCE_US    (30 * 1000)
#define MAX_PATTERNS   4
#define PAT_PINS       4
#define PAT_STEPS      128
#define PULSE_MAX_MS   2000            // continuous on-time per pin in a beep/pattern
#define PAT_TOTAL_MAX_MS (10 * 60 * 1000)

typedef enum { M_NONE = 0, M_OUT, M_IN, M_IN_PU, M_IN_PD, M_GROUND } pmode_t;
static const char *const MODE_NAMES[] = {"", "out", "in", "in_pullup", "in_pulldown", "ground"};

typedef struct {
    char name[16];
    uint8_t pin;
    uint8_t mode;
} pincfg_t;

static pincfg_t s_cfg[MAX_CFG];
static int s_ncfg;
static SemaphoreHandle_t s_cfg_mtx;
static int8_t s_owner[NPINS];          // script index, -1 = free (automation task)
static bool s_watched[NPINS];          // ISR installed
static int8_t s_stable[NPINS];
static int64_t s_check_at[NPINS];

// ---- patterns ---------------------------------------------------------------------------------

typedef struct {
    bool active;
    int8_t owner;
    uint8_t npins;
    uint8_t pins[PAT_PINS];
    uint16_t nsteps, step;
    uint32_t rounds, round;
    uint8_t *mask;          // PSRAM, PAT_STEPS
    uint16_t *ms;
    esp_timer_handle_t tmr;
} pattern_t;

static pattern_t s_pat[MAX_PATTERNS];
static SemaphoreHandle_t s_pat_mtx;    // the esp_timer task and the automation task

static void pat_cb(void *arg)
{
    pattern_t *p = arg;
    xSemaphoreTake(s_pat_mtx, portMAX_DELAY);
    if (p->active) {
        if (p->step >= p->nsteps) {
            p->step = 0;
            p->round++;
        }
        if (p->round >= p->rounds) {
            for (int i = 0; i < p->npins; i++) gpio_set_level(p->pins[i], 0);
            p->active = false;
        } else {
            uint8_t m = p->mask[p->step];
            uint32_t ms = p->ms[p->step];
            p->step++;
            for (int i = 0; i < p->npins; i++) gpio_set_level(p->pins[i], (m >> i) & 1);
            esp_timer_start_once(p->tmr, (uint64_t)ms * 1000);
        }
    }
    xSemaphoreGive(s_pat_mtx);
}

// Under s_pat_mtx.
static void pat_stop_locked(pattern_t *p)
{
    if (!p->active) return;
    esp_timer_stop(p->tmr);
    for (int i = 0; i < p->npins; i++) gpio_set_level(p->pins[i], 0);
    p->active = false;
}

// Stops any pattern that drives `pin` (-1 = every pattern of `owner`).
static void pat_stop_pin(int owner, int pin)
{
    xSemaphoreTake(s_pat_mtx, portMAX_DELAY);
    for (int k = 0; k < MAX_PATTERNS; k++) {
        pattern_t *p = &s_pat[k];
        if (!p->active || p->owner != owner) continue;
        bool hit = pin < 0;
        for (int i = 0; i < p->npins && !hit; i++) hit = p->pins[i] == pin;
        if (hit) pat_stop_locked(p);
    }
    xSemaphoreGive(s_pat_mtx);
}

bool agpio_busy(void)
{
    bool busy = false;
    for (int k = 0; k < MAX_PATTERNS; k++) busy |= s_pat[k].active;
    return busy;
}

// ---- tone (passive buzzers, LEDC) --------------------------------------------------------------

static int s_tone_pin = -1;
static int8_t s_tone_owner = -1;
static esp_timer_handle_t s_tone_tmr;

static void out_low(int pin)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT_OUTPUT};
    gpio_config(&io);  // also gives the pad back to the GPIO function (after LEDC)
    gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_3);
    gpio_set_level(pin, 0);
}

static void tone_stop(void)
{
    if (s_tone_pin < 0) return;
    esp_timer_stop(s_tone_tmr);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    out_low(s_tone_pin);
    s_tone_pin = -1;
    s_tone_owner = -1;
}

static void tone_off_cb(void *arg)
{
    // esp_timer task: LEDC stop + pad reset are safe here; the automation task only
    // starts tones, and a new tone_start stops this timer first.
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    int pin = s_tone_pin;
    if (pin >= 0) out_low(pin);
    s_tone_pin = -1;
    s_tone_owner = -1;
}

// ---- config -----------------------------------------------------------------------------------

static bool pin_allowed(int pin)
{
    for (size_t i = 0; i < sizeof(ALLOWED); i++) if (ALLOWED[i] == pin) return true;
    return false;
}

static void apply_pin(int pin, uint8_t mode)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << pin, .intr_type = GPIO_INTR_DISABLE};
    switch (mode) {
    case M_OUT:
    case M_GROUND:
        io.mode = GPIO_MODE_INPUT_OUTPUT;
        gpio_config(&io);
        gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_3);
        gpio_set_level(pin, 0);
        break;
    case M_IN_PU: io.pull_up_en = 1; /* fall through */
    case M_IN_PD: if (mode == M_IN_PD) io.pull_down_en = 1; /* fall through */
    case M_IN:
    default:  // M_NONE: plain input, no pulls
        io.mode = GPIO_MODE_INPUT;
        gpio_config(&io);
        break;
    }
}

static esp_err_t parse_cfg(const cJSON *pins, pincfg_t *out, int *n, char *err, size_t cap)
{
    *n = 0;
    if (!cJSON_IsArray(pins)) {
        strlcpy(err, "pins: array expected", cap);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *e;
    cJSON_ArrayForEach(e, pins) {
        if (*n >= (int)MAX_CFG) {
            strlcpy(err, "too many pins", cap);
            return ESP_ERR_INVALID_ARG;
        }
        const cJSON *jp = cJSON_GetObjectItem(e, "pin");
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(e, "name"));
        const char *mode = cJSON_GetStringValue(cJSON_GetObjectItem(e, "mode"));
        int pin = cJSON_IsNumber(jp) ? jp->valueint : -1;
        if (!pin_allowed(pin)) {
            snprintf(err, cap, "GPIO %d is not usable (allowed: 3 4 5 6 8 9 10 15 23 24 26)", pin);
            return ESP_ERR_INVALID_ARG;
        }
        pincfg_t c = {.pin = (uint8_t)pin};
        for (int m = M_OUT; m <= M_GROUND && mode; m++) if (strcmp(mode, MODE_NAMES[m]) == 0) c.mode = (uint8_t)m;
        if (!c.mode) {
            snprintf(err, cap, "GPIO %d: mode must be out, in, in_pullup, in_pulldown or ground", pin);
            return ESP_ERR_INVALID_ARG;
        }
        if (name && name[0]) {
            size_t len = strlen(name);
            bool ok = len < sizeof(c.name);
            for (size_t i = 0; i < len && ok; i++) {
                char ch = name[i];
                ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
            }
            if (!ok || (name[0] >= '0' && name[0] <= '9')) {
                snprintf(err, cap, "GPIO %d: name must be 1-15 of a-z 0-9 _ and not start with a digit", pin);
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(c.name, name, sizeof(c.name));
        }
        for (int i = 0; i < *n; i++) {
            if (out[i].pin == c.pin || (c.name[0] && strcmp(out[i].name, c.name) == 0)) {
                snprintf(err, cap, "GPIO %d / '%s' listed twice", pin, c.name);
                return ESP_ERR_INVALID_ARG;
            }
        }
        out[(*n)++] = c;
    }
    return ESP_OK;
}

static void apply_cfg(const pincfg_t *cfg, int n)
{
    // Pins dropped from the config go back to plain inputs.
    for (int i = 0; i < s_ncfg; i++) {
        bool kept = false;
        for (int k = 0; k < n && !kept; k++) kept = cfg[k].pin == s_cfg[i].pin;
        if (!kept) apply_pin(s_cfg[i].pin, M_NONE);
    }
    for (int k = 0; k < n; k++) {
        bool same = false;
        for (int i = 0; i < s_ncfg && !same; i++) same = s_cfg[i].pin == cfg[k].pin && s_cfg[i].mode == cfg[k].mode;
        if (!same) apply_pin(cfg[k].pin, cfg[k].mode);
    }
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    memcpy(s_cfg, cfg, sizeof(pincfg_t) * n);
    s_ncfg = n;
    xSemaphoreGive(s_cfg_mtx);
}

static cJSON *cfg_to_json(const pincfg_t *cfg, int n)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", cfg[i].name);
        cJSON_AddNumberToObject(o, "pin", cfg[i].pin);
        cJSON_AddStringToObject(o, "mode", MODE_NAMES[cfg[i].mode]);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

void agpio_init(void)
{
    s_cfg_mtx = xSemaphoreCreateMutex();
    s_pat_mtx = xSemaphoreCreateMutex();
    for (int i = 0; i < NPINS; i++) {
        s_owner[i] = -1;
        s_stable[i] = -1;
    }
    for (int k = 0; k < MAX_PATTERNS; k++) {
        s_pat[k].mask = heap_caps_calloc(PAT_STEPS, 1, MALLOC_CAP_SPIRAM);
        s_pat[k].ms = heap_caps_calloc(PAT_STEPS, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        esp_timer_create_args_t a = {.callback = pat_cb, .arg = &s_pat[k], .name = "gpio_pat"};
        esp_timer_create(&a, &s_pat[k].tmr);
    }
    esp_timer_create_args_t ta = {.callback = tone_off_cb, .name = "gpio_tone"};
    esp_timer_create(&ta, &s_tone_tmr);
    esp_err_t r = gpio_install_isr_service(0);
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "gpio isr service: %s", esp_err_to_name(r));

    FILE *f = fopen(AUTO_GPIO_CFG, "r");
    if (!f) return;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *j = cJSON_Parse(buf);
    pincfg_t cfg[MAX_CFG];
    int ncfg;
    char err[96];
    if (parse_cfg(j, cfg, &ncfg, err, sizeof(err)) == ESP_OK) {
        apply_cfg(cfg, ncfg);
        ESP_LOGI(TAG, "gpio: %d pin(s) configured", ncfg);
    } else {
        ESP_LOGW(TAG, "gpio config ignored: %s", err);
    }
    cJSON_Delete(j);
}

esp_err_t agpio_set_config(const cJSON *pins, char *err, size_t cap)
{
    pincfg_t cfg[MAX_CFG];
    int n;
    esp_err_t r = parse_cfg(pins, cfg, &n, err, cap);
    if (r != ESP_OK) return r;
    // A pin a running script holds can't change under it.
    for (int i = 0; i < s_ncfg; i++) {
        int pin = s_cfg[i].pin;
        if (s_owner[pin] < 0) continue;
        bool same = false;
        for (int k = 0; k < n && !same; k++) same = cfg[k].pin == pin && cfg[k].mode == s_cfg[i].mode;
        if (!same) {
            snprintf(err, cap, "GPIO %d is in use by script '%s': stop it first", pin, g_scripts[s_owner[pin]].name);
            return ESP_ERR_INVALID_STATE;
        }
    }
    cJSON *j = cfg_to_json(cfg, n);
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) return ESP_ERR_NO_MEM;
    FILE *f = fopen(AUTO_GPIO_CFG, "w");
    bool ok = f && fwrite(txt, 1, strlen(txt), f) == strlen(txt);
    if (f) ok &= fclose(f) == 0;
    cJSON_free(txt);
    if (!ok) {
        strlcpy(err, "could not write to flash", cap);
        return ESP_FAIL;
    }
    apply_cfg(cfg, n);
    ESP_LOGI(TAG, "gpio: %d pin(s) configured", n);
    return ESP_OK;
}

cJSON *agpio_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    for (int i = 0; i < s_ncfg; i++) {
        const pincfg_t *c = &s_cfg[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", c->name);
        cJSON_AddNumberToObject(o, "pin", c->pin);
        cJSON_AddStringToObject(o, "mode", MODE_NAMES[c->mode]);
        cJSON_AddNumberToObject(o, "level", gpio_get_level(c->pin));
        int owner = s_owner[c->pin];
        cJSON_AddStringToObject(o, "owner", owner >= 0 ? g_scripts[owner].name : "");
        bool playing = false;
        for (int k = 0; k < MAX_PATTERNS; k++) {
            for (int p = 0; p < s_pat[k].npins && s_pat[k].active; p++) playing |= s_pat[k].pins[p] == c->pin;
        }
        cJSON_AddBoolToObject(o, "playing", playing || s_tone_pin == c->pin);
        cJSON_AddItemToArray(arr, o);
    }
    xSemaphoreGive(s_cfg_mtx);
    return arr;
}

// ---- inputs ------------------------------------------------------------------------------------

static void gpio_isr(void *arg)
{
    int pin = (int)(intptr_t)arg;
    gpio_intr_disable(pin);  // one event per debounce window, whatever the pin does
    ev_t ev = {.type = EV_GPIO, .gpio.pin = (uint8_t)pin};
    BaseType_t woken = pdFALSE;
    auto_post_from_isr(&ev, &woken);
    if (woken) portYIELD_FROM_ISR();
}

void agpio_on_isr_event(uint8_t pin, int64_t now_us)
{
    if (pin < NPINS && s_watched[pin]) s_check_at[pin] = now_us + DEBOUNCE_US;
}

int64_t agpio_tick(int64_t now_us)
{
    int64_t next = 0;
    for (int pin = 0; pin < NPINS; pin++) {
        if (!s_watched[pin] || !s_check_at[pin]) continue;
        if (now_us < s_check_at[pin]) {
            if (!next || s_check_at[pin] < next) next = s_check_at[pin];
            continue;
        }
        s_check_at[pin] = 0;
        int level = gpio_get_level(pin);
        gpio_intr_enable(pin);
        if (level != s_stable[pin]) {
            s_stable[pin] = (int8_t)level;
            auto_dispatch_gpio(s_owner[pin], pin, level);
        }
    }
    return next;
}

// ---- Lua API ----------------------------------------------------------------------------------

// Pin argument (number or configured name) → pin number, checked against the config.
static int resolve(lua_State *L, int arg, const char *want, uint8_t *mode_out)
{
    int pin = -1;
    uint8_t mode = M_NONE;
    const char *name = NULL;
    if (lua_type(L, arg) == LUA_TNUMBER) pin = (int)luaL_checkinteger(L, arg);
    else name = luaL_checkstring(L, arg);
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    for (int i = 0; i < s_ncfg; i++) {
        if ((name && strcmp(s_cfg[i].name, name) == 0) || (!name && s_cfg[i].pin == pin)) {
            pin = s_cfg[i].pin;
            mode = s_cfg[i].mode;
        }
    }
    xSemaphoreGive(s_cfg_mtx);
    if (mode == M_NONE) {
        if (name) return luaL_error(L, "gpio: no pin named '%s' (Automations -> GPIO)", name);
        return luaL_error(L, "gpio: GPIO %d is not enabled in Automations -> GPIO", pin);
    }
    if (mode == M_GROUND) return luaL_error(L, "gpio: GPIO %d is a ground pin", pin);
    if (want && strcmp(want, "out") == 0 && mode != M_OUT) return luaL_error(L, "gpio: GPIO %d is not an output", pin);
    if (want && strcmp(want, "in") == 0 && mode == M_OUT) return luaL_error(L, "gpio: GPIO %d is not an input", pin);
    if (mode_out) *mode_out = mode;
    return pin;
}

// "buzzer1 (GPIO 5)" or "GPIO 5"
static const char *pin_label(int pin, char *buf, size_t cap)
{
    snprintf(buf, cap, "GPIO %d", pin);
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    for (int i = 0; i < s_ncfg; i++) {
        if (s_cfg[i].pin == pin && s_cfg[i].name[0]) snprintf(buf, cap, "%s (GPIO %d)", s_cfg[i].name, pin);
    }
    xSemaphoreGive(s_cfg_mtx);
    return buf;
}

static void claim(lua_State *L, int pin)
{
    int me = auto_script_index(auto_script_of(L));
    if (s_owner[pin] == me) return;
    char label[40];
    if (!auto_loading(L)) {
        luaL_error(L, "gpio: %s is not claimed by this script: add gpio.claim(...) at the top of the script",
                   pin_label(pin, label, sizeof(label)));
    }
    if (s_owner[pin] >= 0) {
        luaL_error(L, "gpio: %s is used by script '%s'", pin_label(pin, label, sizeof(label)),
                   g_scripts[s_owner[pin]].name);
    }
    s_owner[pin] = (int8_t)me;
}

// gpio.claim(pin, ...) or gpio.claim({pins}) — top level only.
static int l_claim(lua_State *L)
{
    if (!auto_loading(L)) return luaL_error(L, "gpio.claim: only at the top of the script (when it loads)");
    bool table = lua_istable(L, 1);
    int n = table ? (int)lua_rawlen(L, 1) : lua_gettop(L);
    luaL_argcheck(L, n >= 1, 1, "at least one pin");
    for (int i = 1; i <= n; i++) {
        if (table) lua_rawgeti(L, 1, i);
        else lua_pushvalue(L, i);
        claim(L, resolve(L, -1, NULL, NULL));
        lua_pop(L, 1);
    }
    return 0;
}

static int l_write(lua_State *L)
{
    int pin = resolve(L, 1, "out", NULL);
    int v = lua_isboolean(L, 2) ? lua_toboolean(L, 2) : (int)luaL_checkinteger(L, 2) != 0;
    claim(L, pin);
    pat_stop_pin(s_owner[pin], pin);
    if (s_tone_pin == pin) tone_stop();
    gpio_set_level(pin, v);
    return 0;
}

static int l_read(lua_State *L)
{
    int pin = resolve(L, 1, NULL, NULL);
    lua_pushinteger(L, gpio_get_level(pin));
    return 1;
}

static int l_on(lua_State *L)
{
    int pin = resolve(L, 1, "in", NULL);
    const char *edge = luaL_checkstring(L, 2);
    luaL_argcheck(L, strcmp(edge, "rising") == 0 || strcmp(edge, "falling") == 0 || strcmp(edge, "both") == 0, 2,
                  "\"rising\", \"falling\" or \"both\"");
    luaL_checktype(L, 3, LUA_TFUNCTION);
    claim(L, pin);
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, pin);
    lua_setfield(L, -2, "pin");
    lua_pushstring(L, edge);
    lua_setfield(L, -2, "edge");
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, "fn");
    auto_reg_append(L, "hub.gpio");
    if (!s_watched[pin]) {
        s_stable[pin] = (int8_t)gpio_get_level(pin);
        s_check_at[pin] = 0;
        gpio_set_intr_type(pin, GPIO_INTR_ANYEDGE);
        if (gpio_isr_handler_add(pin, gpio_isr, (void *)(intptr_t)pin) != ESP_OK) {
            return luaL_error(L, "gpio: can't watch GPIO %d", pin);
        }
        gpio_intr_enable(pin);
        s_watched[pin] = true;
    }
    return 0;
}

// Starts a pattern (steps already validated) on pins owned by the calling script.
static void pat_start(lua_State *L, const uint8_t *pins, int npins, const uint8_t *mask, const uint16_t *ms,
                      int nsteps, uint32_t rounds)
{
    int me = auto_script_index(auto_script_of(L));
    for (int i = 0; i < npins; i++) {
        pat_stop_pin(me, pins[i]);
        if (s_tone_pin == pins[i]) tone_stop();
    }
    xSemaphoreTake(s_pat_mtx, portMAX_DELAY);
    pattern_t *p = NULL;
    for (int k = 0; k < MAX_PATTERNS && !p; k++) if (!s_pat[k].active) p = &s_pat[k];
    if (p) {
        p->owner = (int8_t)me;
        p->npins = (uint8_t)npins;
        memcpy(p->pins, pins, npins);
        memcpy(p->mask, mask, nsteps);
        memcpy(p->ms, ms, nsteps * sizeof(uint16_t));
        p->nsteps = (uint16_t)nsteps;
        p->step = 0;
        p->rounds = rounds;
        p->round = 0;
        p->active = true;
        esp_timer_stop(p->tmr);
        esp_timer_start_once(p->tmr, 1);  // first step now (from the timer task)
    }
    xSemaphoreGive(s_pat_mtx);
    if (!p) luaL_error(L, "gpio: %d patterns already playing", MAX_PATTERNS);
}

// Longest continuous on-time of each pin, rounds wrapping (on at the end + on at the start).
static void check_pulses(lua_State *L, const uint8_t *mask, const uint16_t *ms, int nsteps, uint32_t rounds, int npins)
{
    for (int b = 0; b < npins; b++) {
        uint32_t run = 0, longest = 0;
        int passes = rounds > 1 ? 2 : 1;
        for (int pass = 0; pass < passes; pass++) {
            for (int i = 0; i < nsteps; i++) {
                if ((mask[i] >> b) & 1) {
                    run += ms[i];
                    if (run > longest) longest = run;
                } else {
                    run = 0;
                }
            }
        }
        if (longest > PULSE_MAX_MS) {
            luaL_error(L, "gpio: pin %d would stay on for %u ms in a row (max %d)", b + 1, (unsigned)longest, PULSE_MAX_MS);
        }
    }
}

// gpio.beep(pin, ms) or gpio.beep(pin, {on=100, off=100, times=3})
static int l_beep(lua_State *L)
{
    int pin = resolve(L, 1, "out", NULL);
    lua_Integer on = 0, off = 0, times = 1;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "on");
        lua_getfield(L, 2, "off");
        lua_getfield(L, 2, "times");
        on = luaL_optinteger(L, -3, 100);
        off = luaL_optinteger(L, -2, 100);
        times = luaL_optinteger(L, -1, 1);
        lua_pop(L, 3);
    } else {
        on = luaL_checkinteger(L, 2);
    }
    luaL_argcheck(L, on >= 1 && on <= PULSE_MAX_MS, 2, "on: 1..2000 ms");
    luaL_argcheck(L, off >= 0 && off <= 60000, 2, "off: 0..60000 ms");
    luaL_argcheck(L, times >= 1 && times <= 1000, 2, "times: 1..1000");
    claim(L, pin);
    uint8_t mask[2] = {1, 0};
    uint16_t ms[2] = {(uint16_t)on, (uint16_t)(off ? off : 1)};
    int nsteps = off ? 2 : 1;
    uint8_t pins[1] = {(uint8_t)pin};
    pat_start(L, pins, 1, mask, ms, nsteps, (uint32_t)times);
    return 0;
}

// gpio.pattern(pins, steps, {rounds=N}): steps = {{mask, ms}, ...}, bit i of mask = pins[i+1]
static int l_pattern(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    int npins = (int)lua_rawlen(L, 1);
    luaL_argcheck(L, npins >= 1 && npins <= PAT_PINS, 1, "1 to 4 pins");
    uint8_t pins[PAT_PINS];
    for (int i = 0; i < npins; i++) {
        lua_rawgeti(L, 1, i + 1);
        pins[i] = (uint8_t)resolve(L, -1, "out", NULL);
        lua_pop(L, 1);
        for (int k = 0; k < i; k++) if (pins[k] == pins[i]) luaL_argerror(L, 1, "a pin is listed twice");
    }
    int nsteps = (int)lua_rawlen(L, 2);
    luaL_argcheck(L, nsteps >= 1 && nsteps <= PAT_STEPS, 2, "1 to 128 steps");
    uint8_t mask[PAT_STEPS];
    uint16_t ms[PAT_STEPS];
    uint64_t total = 0;
    for (int i = 0; i < nsteps; i++) {
        lua_rawgeti(L, 2, i + 1);
        luaL_argcheck(L, lua_istable(L, -1), 2, "each step is {mask, ms}");
        lua_rawgeti(L, -1, 1);
        lua_rawgeti(L, -2, 2);
        lua_Integer m = luaL_checkinteger(L, -2), t = luaL_checkinteger(L, -1);
        lua_pop(L, 3);
        if (m < 0 || m >= (1 << npins)) luaL_error(L, "gpio.pattern: step %d: mask 0..%d", i + 1, (1 << npins) - 1);
        if (t < 1 || t > 60000) luaL_error(L, "gpio.pattern: step %d: 1..60000 ms", i + 1);
        mask[i] = (uint8_t)m;
        ms[i] = (uint16_t)t;
        total += (uint64_t)t;
    }
    lua_Integer rounds = 1;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "rounds");
        rounds = luaL_optinteger(L, -1, 1);
        lua_pop(L, 1);
    }
    luaL_argcheck(L, rounds >= 1 && rounds <= 10000, 3, "rounds: 1..10000");
    if (total * (uint64_t)rounds > PAT_TOTAL_MAX_MS) luaL_error(L, "gpio.pattern: longer than 10 minutes");
    check_pulses(L, mask, ms, nsteps, (uint32_t)rounds, npins);
    for (int i = 0; i < npins; i++) claim(L, pins[i]);
    pat_start(L, pins, npins, mask, ms, nsteps, (uint32_t)rounds);
    return 0;
}

// gpio.stop() — every pattern/tone of this script; gpio.stop(pin) or gpio.stop({pins})
static int l_stop(lua_State *L)
{
    int me = auto_script_index(auto_script_of(L));
    if (lua_isnoneornil(L, 1)) {
        pat_stop_pin(me, -1);
        if (s_tone_owner == me) tone_stop();
        return 0;
    }
    int n = lua_istable(L, 1) ? (int)lua_rawlen(L, 1) : 1;
    for (int i = 0; i < n; i++) {
        int pin;
        if (lua_istable(L, 1)) {
            lua_rawgeti(L, 1, i + 1);
            pin = resolve(L, -1, "out", NULL);
            lua_pop(L, 1);
        } else {
            pin = resolve(L, 1, "out", NULL);
        }
        pat_stop_pin(me, pin);
        if (s_tone_pin == pin && s_tone_owner == me) tone_stop();
    }
    return 0;
}

// gpio.tone(pin, hz, ms) — passive buzzers (square wave from LEDC); one tone at a time.
static int l_tone(lua_State *L)
{
    int pin = resolve(L, 1, "out", NULL);
    lua_Integer hz = luaL_checkinteger(L, 2), ms = luaL_checkinteger(L, 3);
    luaL_argcheck(L, hz >= 20 && hz <= 20000, 2, "20..20000 Hz");
    luaL_argcheck(L, ms >= 1 && ms <= PULSE_MAX_MS, 3, "1..2000 ms");
    claim(L, pin);
    int me = s_owner[pin];
    if (s_tone_pin >= 0 && s_tone_owner != me) return luaL_error(L, "gpio.tone: another script is playing a tone");
    pat_stop_pin(me, pin);
    tone_stop();
    ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                             .timer_num = LEDC_TIMER_0, .freq_hz = (uint32_t)hz, .clk_cfg = LEDC_AUTO_CLK};
    ledc_channel_config_t c = {.gpio_num = pin, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
                               .timer_sel = LEDC_TIMER_0, .duty = 512, .hpoint = 0};
    if (ledc_timer_config(&t) != ESP_OK || ledc_channel_config(&c) != ESP_OK) {
        out_low(pin);
        return luaL_error(L, "gpio.tone: %d Hz not possible", (int)hz);
    }
    s_tone_pin = pin;
    s_tone_owner = (int8_t)me;
    esp_timer_start_once(s_tone_tmr, (uint64_t)ms * 1000);
    return 0;
}

void agpio_open_lib(lua_State *L)
{
    static const luaL_Reg fns[] = {
        {"write", l_write}, {"read", l_read},       {"on", l_on},     {"beep", l_beep},
        {"pattern", l_pattern}, {"stop", l_stop},   {"tone", l_tone}, {"claim", l_claim},
        {NULL, NULL},
    };
    luaL_newlib(L, fns);
    lua_setglobal(L, "gpio");
}

// Fail-safe: the script stopped (disabled, error, timeout, out of memory, deleted).
void agpio_release(int script)
{
    pat_stop_pin(script, -1);
    if (s_tone_owner == script) tone_stop();
    for (int pin = 0; pin < NPINS; pin++) {
        if (s_owner[pin] != script) continue;
        if (s_watched[pin]) {
            gpio_isr_handler_remove(pin);
            gpio_intr_disable(pin);
            gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
            s_watched[pin] = false;
            s_check_at[pin] = 0;
        } else {
            gpio_set_level(pin, 0);
        }
        s_owner[pin] = -1;
    }
}
