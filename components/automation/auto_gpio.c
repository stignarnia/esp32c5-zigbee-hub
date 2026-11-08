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
// Allowed pins: "GPIO map of this board" in CLAUDE.md (ESP32-C5 datasheet v1.5 + the
// Waveshare ESP32-C5-WIFI6-KIT-NXRX schematic). Never: 11/12 (UART0 to the
// CH343), 13/14 (native USB), 15 (SPICS1, the PSRAM chip select: driving it low locks the
// CPU up), 16-22 (flash/PSRAM bus, flash supply), 28 (BOOT). 0/1 only because this board
// leaves the 32 kHz crystal out. 7 (JTAG-source strap) is ignored with JTAG_SEL_ENABLE = 0,
// 25/3 (SDIO edge straps) only matter in SDIO-slave mode, 2 has no boot role, 26 only
// matters with BOOT held.
// Outputs use drive strength 3 (the 12 mm buzzers load a pin to 3.0 V).
//
// The board's RGB LED (WS2812B) has its data input on GPIO27, which is also allowed as a
// buzzer's ground. C owns the pin: Lua only sees led.set/off/claim. A frame (~80 us) is sent
// only while the outputs that may be wired to GPIO27 are low, and those are switched to
// inputs for the frame, so no current can flow through a buzzer whatever the data line does.
#include <stdio.h>
#include <string.h>

#include "auto_internal.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lauxlib.h"

static const char *TAG = "auto";

static const uint8_t ALLOWED[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 23, 24, 25, 26};
// Usable only as a buzzer's ground (held low after boot, never driven high, never read):
// 27 is a strapping pin and the board's RGB LED data line. On this chip its reset level only
// selects UART boot printing, which the eFuse UART_PRINT_CONTROL = 0 ignores; with BOOT
// held, the board's 10 k pull-up keeps it high (normal download mode).
static const uint8_t GROUND_ONLY[] = {27};
#define LED_PIN        27              // WS2812B data in (board), also a ground-only pin
#define NPINS          32
#define MAX_CFG        (sizeof(ALLOWED) + sizeof(GROUND_ONLY))
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
    char gfor[16];          // ground pins: the output this pin is the ground for ("" = not said)
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
static int8_t s_tone_owner = -1;

// Boot-loop guard, buzzer marker: which scripts' buzzers are playing, kept across a soft
// reset. If the hub resets abnormally while one plays (a loose buzzer can throw a power
// glitch), the next boot disables that script instead of playing into the same reset again.
#define BUZ_MAGIC      0x42555a31u
#define BUZ_MAX        (MAX_PATTERNS + 1)
typedef struct {
    uint32_t magic;
    uint8_t n;
    char names[BUZ_MAX][AUTO_NAME_MAX + 1];
} rtc_buz_t;
static RTC_NOINIT_ATTR rtc_buz_t s_rtc_buz;
static portMUX_TYPE s_buz_lock = portMUX_INITIALIZER_UNLOCKED;

// Recomputes the marker from the playing patterns and tone. Called after every change
// (automation task and esp_timer task); reads the flags without s_pat_mtx on purpose (the
// callers may hold it), the last call after a change leaves it right.
static void marker_update(void)
{
    int8_t own[BUZ_MAX];
    int n = 0;
    for (int k = 0; k < MAX_PATTERNS; k++) {
        if (!s_pat[k].active || s_pat[k].owner < 0) continue;
        bool dup = false;
        for (int i = 0; i < n; i++) dup |= own[i] == s_pat[k].owner;
        if (!dup) own[n++] = s_pat[k].owner;
    }
    if (s_tone_owner >= 0) {
        bool dup = false;
        for (int i = 0; i < n; i++) dup |= own[i] == s_tone_owner;
        if (!dup && n < BUZ_MAX) own[n++] = s_tone_owner;
    }
    portENTER_CRITICAL_SAFE(&s_buz_lock);
    s_rtc_buz.n = (uint8_t)n;
    for (int i = 0; i < n; i++) strlcpy(s_rtc_buz.names[i], g_scripts[own[i]].name, sizeof(s_rtc_buz.names[i]));
    s_rtc_buz.magic = BUZ_MAGIC;
    portEXIT_CRITICAL_SAFE(&s_buz_lock);
}

int agpio_take_marker(char names[][AUTO_NAME_MAX + 1], int max)
{
    int n = 0;
    if (s_rtc_buz.magic == BUZ_MAGIC && s_rtc_buz.n <= BUZ_MAX) {
        for (int i = 0; i < s_rtc_buz.n && n < max; i++) {
            s_rtc_buz.names[i][AUTO_NAME_MAX] = '\0';
            strlcpy(names[n++], s_rtc_buz.names[i], AUTO_NAME_MAX + 1);
        }
    }
    s_rtc_buz.n = 0;
    s_rtc_buz.magic = BUZ_MAGIC;
    return n;
}

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
            marker_update();
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
    marker_update();
    xSemaphoreGive(s_pat_mtx);
}

// RGB LED state (see the RGB LED section)
static int8_t s_led_owner = -1;        // script index, -1 = nobody
static uint8_t s_led_want[3];          // r, g, b requested
static bool s_led_pending;             // s_led_want not shown yet
static uint8_t s_led_shown[3];
static bool s_led_shown_valid;         // false until the first frame after boot
static void led_try(void);
static void led_request(uint8_t r, uint8_t g, uint8_t b);

bool agpio_busy(void)
{
    bool busy = false;
    for (int k = 0; k < MAX_PATTERNS; k++) busy |= s_pat[k].active;
    return busy;
}

// ---- tone (passive buzzers, LEDC) --------------------------------------------------------------

static int s_tone_pin = -1;
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
    marker_update();
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
    marker_update();
}

// ---- config -----------------------------------------------------------------------------------

static bool pin_allowed(int pin, uint8_t mode)
{
    for (size_t i = 0; i < sizeof(ALLOWED); i++) if (ALLOWED[i] == pin) return true;
    for (size_t i = 0; i < sizeof(GROUND_ONLY); i++) if (GROUND_ONLY[i] == pin) return mode == M_GROUND;
    return false;
}

static void apply_pin(int pin, uint8_t mode)
{
    // The LED's data line idles low (a WS2812 needs a low line between frames; the board's
    // 10 k pull-up would hold it high), so unconfigured it is held low like a ground.
    if (pin == LED_PIN && mode == M_NONE) mode = M_GROUND;
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
        pincfg_t c = {.pin = (uint8_t)pin};
        for (int m = M_OUT; m <= M_GROUND && mode; m++) if (strcmp(mode, MODE_NAMES[m]) == 0) c.mode = (uint8_t)m;
        if (!c.mode) {
            snprintf(err, cap, "GPIO %d: mode must be out, in, in_pullup, in_pulldown or ground", pin);
            return ESP_ERR_INVALID_ARG;
        }
        if (!pin_allowed(pin, c.mode)) {
            snprintf(err, cap, "GPIO %d is not usable (allowed: 0-10, 23-26; 27 as ground only)", pin);
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
        const char *gfor = cJSON_GetStringValue(cJSON_GetObjectItem(e, "for"));
        if (gfor && gfor[0]) {
            if (c.mode != M_GROUND) {
                snprintf(err, cap, "GPIO %d: \"ground for\" is only for ground pins", pin);
                return ESP_ERR_INVALID_ARG;
            }
            if (strlen(gfor) >= sizeof(c.gfor)) {
                snprintf(err, cap, "GPIO %d: \"ground for\" names an output (1-15 characters)", pin);
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(c.gfor, gfor, sizeof(c.gfor));
        }
        for (int i = 0; i < *n; i++) {
            if (out[i].pin == c.pin || (c.name[0] && strcmp(out[i].name, c.name) == 0)) {
                snprintf(err, cap, "GPIO %d / '%s' listed twice", pin, c.name);
                return ESP_ERR_INVALID_ARG;
            }
        }
        out[(*n)++] = c;
    }
    // A ground's "for" must name an output in the same config.
    for (int i = 0; i < *n; i++) {
        if (!out[i].gfor[0]) continue;
        bool found = false;
        for (int k = 0; k < *n && !found; k++) found = out[k].mode == M_OUT && strcmp(out[k].name, out[i].gfor) == 0;
        if (!found) {
            snprintf(err, cap, "GPIO %d: ground for '%s', but no output has that name", out[i].pin, out[i].gfor);
            return ESP_ERR_INVALID_ARG;
        }
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
        if (cfg[i].gfor[0]) cJSON_AddStringToObject(o, "for", cfg[i].gfor);
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
    if (!f) {
        apply_pin(LED_PIN, M_NONE);
        led_request(0, 0, 0);    // the LED keeps its colour through a reset: off at every boot
        return;
    }
    // Runs on the main task (3.5 KB of stack): the file and the parsed table go on the heap.
    const size_t cap = 2048;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pincfg_t *cfg = heap_caps_calloc(MAX_CFG, sizeof(pincfg_t), MALLOC_CAP_SPIRAM);
    if (!buf || !cfg) {
        fclose(f);
        free(buf);
        free(cfg);
        ESP_LOGE(TAG, "gpio config: out of memory");
        apply_pin(LED_PIN, M_NONE);
        led_request(0, 0, 0);
        return;
    }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    int ncfg;
    char err[96];
    if (parse_cfg(j, cfg, &ncfg, err, sizeof(err)) == ESP_OK) {
        apply_cfg(cfg, ncfg);
        ESP_LOGI(TAG, "gpio: %d pin(s) configured", ncfg);
    } else {
        ESP_LOGW(TAG, "gpio config ignored: %s", err);
    }
    cJSON_Delete(j);
    free(cfg);
    bool led_cfg = false;
    for (int i = 0; i < s_ncfg; i++) led_cfg |= s_cfg[i].pin == LED_PIN;
    if (!led_cfg) apply_pin(LED_PIN, M_NONE);
    led_request(0, 0, 0);        // the LED keeps its colour through a reset: off at every boot
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
        cJSON_AddStringToObject(o, "for", c->gfor);
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
    if (s_led_pending) {
        led_try();
        if (s_led_pending) next = now_us + 20 * 1000;   // an output that shares GPIO27 is busy
    }
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

// ---- RGB LED (WS2812B on GPIO27) -------------------------------------------------------------
// Everything here runs on the automation task (led.* calls, agpio_tick, agpio_release) or
// on the main task at boot, before the engine runs: never two at once.


// One RGB frame at 800 kHz via RMT, then the pin goes back to a GPIO held low. Frames are
// rare (a script's led.set), so the channel lives only for the frame.
static esp_err_t led_frame(const uint8_t rgb[3])
{
    rmt_channel_handle_t ch = NULL;
    rmt_encoder_handle_t enc = NULL;
    rmt_tx_channel_config_t cc = {.gpio_num = LED_PIN, .clk_src = RMT_CLK_SRC_DEFAULT,
                                  .resolution_hz = 10000000, .mem_block_symbols = 48,
                                  .trans_queue_depth = 1};
    rmt_bytes_encoder_config_t ec = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},   // 0.3 / 0.9 us
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},   // 0.9 / 0.3 us
        .flags.msb_first = 1};
    esp_err_t r = rmt_new_tx_channel(&cc, &ch);
    if (r == ESP_OK) r = rmt_new_bytes_encoder(&ec, &enc);
    if (r == ESP_OK) r = rmt_enable(ch);
    if (r == ESP_OK) {
        // This board's LED (WS2812B-0807) takes R, G, B, not the usual WS2812B G, R, B.
        // Values go out as given, no white balance (equal values look a little bluish).
        rmt_transmit_config_t tc = {.loop_count = 0};
        r = rmt_transmit(ch, enc, rgb, 3, &tc);
        if (r == ESP_OK) r = rmt_tx_wait_all_done(ch, 100);
        rmt_disable(ch);
    }
    if (ch) rmt_del_channel(ch);
    if (enc) rmt_del_encoder(enc);
    apply_pin(LED_PIN, M_GROUND);       // back to a GPIO, low (> 50 us low latches the colour)
    return r;
}

// The outputs that may be wired to GPIO27 as their ground: the output its "ground for"
// names, or, when it names none (or 27 isn't configured), every configured output.
static int led_outputs(uint8_t *pins, int max)
{
    int n = 0;
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    const char *gfor = "";
    for (int i = 0; i < s_ncfg; i++) if (s_cfg[i].pin == LED_PIN && s_cfg[i].mode == M_GROUND) gfor = s_cfg[i].gfor;
    for (int i = 0; i < s_ncfg && n < max; i++) {
        if (s_cfg[i].mode != M_OUT) continue;
        if (gfor[0] && strcmp(s_cfg[i].name, gfor) != 0) continue;
        pins[n++] = s_cfg[i].pin;
    }
    xSemaphoreGive(s_cfg_mtx);
    return n;
}

static bool pin_playing(int pin)
{
    if (s_tone_pin == pin) return true;
    for (int k = 0; k < MAX_PATTERNS; k++) {
        for (int i = 0; i < s_pat[k].npins && s_pat[k].active; i++) if (s_pat[k].pins[i] == pin) return true;
    }
    return false;
}

// Shows s_led_want if every output that may share GPIO27 is low and idle; otherwise it
// stays pending and agpio_tick tries again. Those outputs are inputs for the ~80 us frame.
static void led_try(void)
{
    if (!s_led_pending) return;
    uint8_t outs[MAX_CFG];
    int n = led_outputs(outs, MAX_CFG);
    for (int i = 0; i < n; i++) {
        if (pin_playing(outs[i]) || gpio_get_level(outs[i])) return;   // wait
    }
    for (int i = 0; i < n; i++) gpio_set_direction(outs[i], GPIO_MODE_INPUT);
    esp_err_t r = led_frame(s_led_want);
    for (int i = 0; i < n; i++) gpio_set_direction(outs[i], GPIO_MODE_INPUT_OUTPUT);  // output register still 0
    s_led_pending = false;
    if (r == ESP_OK) {
        memcpy(s_led_shown, s_led_want, 3);
        s_led_shown_valid = true;
    } else {
        ESP_LOGW(TAG, "RGB LED frame failed: %s", esp_err_to_name(r));
    }
}

static void led_request(uint8_t r, uint8_t g, uint8_t b)
{
    s_led_want[0] = r;
    s_led_want[1] = g;
    s_led_want[2] = b;
    s_led_pending = true;
    led_try();
}

static void led_claim(lua_State *L)
{
    int me = auto_script_index(auto_script_of(L));
    if (s_led_owner == me) return;
    if (!auto_loading(L)) luaL_error(L, "led: not claimed by this script: add led.claim() at the top of the script");
    if (s_led_owner >= 0) luaL_error(L, "led: used by script '%s'", g_scripts[s_led_owner].name);
    s_led_owner = (int8_t)me;
}

static int l_led_set(lua_State *L)
{
    lua_Integer r = luaL_checkinteger(L, 1), g = luaL_checkinteger(L, 2), b = luaL_checkinteger(L, 3);
    luaL_argcheck(L, r >= 0 && r <= 255, 1, "0..255");
    luaL_argcheck(L, g >= 0 && g <= 255, 2, "0..255");
    luaL_argcheck(L, b >= 0 && b <= 255, 3, "0..255");
    led_claim(L);
    led_request((uint8_t)r, (uint8_t)g, (uint8_t)b);
    return 0;
}

static int l_led_off(lua_State *L)
{
    led_claim(L);
    led_request(0, 0, 0);
    return 0;
}

static int l_led_claim(lua_State *L)
{
    if (!auto_loading(L)) return luaL_error(L, "led.claim: only at the top of the script (when it loads)");
    led_claim(L);
    return 0;
}

cJSON *agpio_led_json(void)
{
    cJSON *o = cJSON_CreateObject();
    int owner = s_led_owner;
    cJSON_AddStringToObject(o, "owner", owner >= 0 ? g_scripts[owner].name : "");
    cJSON *c = cJSON_AddArrayToObject(o, "rgb");
    for (int i = 0; i < 3; i++) cJSON_AddItemToArray(c, cJSON_CreateNumber(s_led_shown_valid ? s_led_shown[i] : 0));
    cJSON_AddBoolToObject(o, "pending", s_led_pending);
    return o;
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
        marker_update();
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
    marker_update();
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
    static const luaL_Reg led[] = {{"set", l_led_set}, {"off", l_led_off}, {"claim", l_led_claim}, {NULL, NULL}};
    luaL_newlib(L, led);
    lua_setglobal(L, "led");
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
    if (s_led_owner == script) {
        s_led_owner = -1;
        led_request(0, 0, 0);    // waits for the outputs sharing GPIO27 if they're busy
    }
}
