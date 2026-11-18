/**
 * @file wireguard-platform-esp32.c
 * @brief ESP32 platform implementation for wireguard-lwip
 */

#include "wireguard-platform.h"
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sys.h"
#include <string.h>

/* ============================================================================
 * Time Functions
 * ========================================================================== */

uint32_t wireguard_sys_now() {
    // Use lwIP's built-in time function
    return sys_now();
}

/*
 * Handshake timestamp (TAI64N): strictly increasing across reboots (upstream microlink 484fcc4,
 * aec27f9). A peer remembers the greatest timestamp it accepted from our static key and silently
 * drops any initiation that isn't newer (WireGuard 5.1). This used uptime, so after a reboot every
 * initiation of ours was dropped until uptime passed the previous session's.
 *
 *   t = max(wall clock if set, base + uptime)
 *   base = (floor saved in NVS) + SAVE_INTERVAL + 1 at boot, so t exceeds anything sent before.
 *   The floor is re-saved once t passes it by SAVE_INTERVAL.
 * Hub port: NVS is read and written by ts_task (internal stack: wireguard_ts_init /
 * wireguard_ts_persist), never here: this runs on microlink's task, whose stack is in PSRAM, where
 * flash access isn't allowed. A new floor is written within ~1 s of being due; a reboot inside that
 * second could repeat a timestamp (the peer then drops that one initiation).
 */
#define WG_TS_NVS_NAMESPACE     "ml_wg"
#define WG_TS_NVS_KEY           "ts_floor"
#define WG_TS_SAVE_INTERVAL_S   600ULL
#define WG_TS_FIRST_BOOT_BASE   1735689600ULL   /* 2025-01-01T00:00:00Z */
#define WG_TS_MIN_WALLCLOCK     1735689600ULL   /* wall clock below this = not set */

static portMUX_TYPE s_ts_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_ts_base = WG_TS_FIRST_BOOT_BASE;  /* seconds; t = base + uptime */
static uint64_t s_ts_saved;      /* floor in NVS (0 = none yet) */
static uint64_t s_ts_want;       /* floor due to be written */
static uint64_t s_ts_last_s;
static uint32_t s_ts_last_ns;

void wireguard_ts_init(void) {
    nvs_handle_t h;
    uint64_t v = 0;
    if (nvs_open(WG_TS_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u64(h, WG_TS_NVS_KEY, &v) != ESP_OK) v = 0;
        nvs_close(h);
    }
    portENTER_CRITICAL(&s_ts_lock);
    s_ts_saved = v;
    s_ts_base = v ? v + WG_TS_SAVE_INTERVAL_S + 1 : WG_TS_FIRST_BOOT_BASE;
    portEXIT_CRITICAL(&s_ts_lock);
}

void wireguard_ts_persist(void) {
    static bool err_logged;
    portENTER_CRITICAL(&s_ts_lock);
    uint64_t v = s_ts_want, saved = s_ts_saved;
    portEXIT_CRITICAL(&s_ts_lock);
    if (v <= saved) return;
    nvs_handle_t h;
    esp_err_t err = nvs_open(WG_TS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u64(h, WG_TS_NVS_KEY, v);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_ts_lock);
        if (v > s_ts_saved) s_ts_saved = v;  /* only once it is really in flash (aec27f9) */
        portEXIT_CRITICAL(&s_ts_lock);
        err_logged = false;
    } else if (!err_logged) {
        err_logged = true;
        ESP_LOGW("wg", "Could not save the handshake timestamp floor (%s); after a reboot handshakes "
                 "may be dropped as replays until this succeeds", esp_err_to_name(err));
    }
}

void wireguard_tai64n_now(uint8_t *output) {
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint64_t up_s = now_us / 1000000ULL;
    uint32_t nanoseconds = (uint32_t)(now_us % 1000000ULL) * 1000U;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    const uint64_t wall = (tv.tv_sec > 0 && (uint64_t)tv.tv_sec >= WG_TS_MIN_WALLCLOCK) ? (uint64_t)tv.tv_sec : 0;

    uint64_t seconds;
    portENTER_CRITICAL(&s_ts_lock);
    seconds = s_ts_base + up_s;
    if (wall > seconds) {
        s_ts_base = wall - up_s;  /* adopt wall-clock time; base only ever moves forward */
        seconds = wall;
    }
    /* Strictly increasing within a boot, even for back-to-back calls */
    if (seconds < s_ts_last_s || (seconds == s_ts_last_s && nanoseconds <= s_ts_last_ns)) {
        seconds = s_ts_last_s;
        nanoseconds = s_ts_last_ns + 1;
        if (nanoseconds >= 1000000000U) {
            seconds++;
            nanoseconds = 0;
        }
    }
    s_ts_last_s = seconds;
    s_ts_last_ns = nanoseconds;
    if ((s_ts_saved == 0 || seconds >= s_ts_saved + WG_TS_SAVE_INTERVAL_S) && seconds > s_ts_want) {
        s_ts_want = seconds;  /* written by ts_task */
    }
    portEXIT_CRITICAL(&s_ts_lock);

    // TAI64 label: 2^62 + 10 + Unix seconds
    seconds += 0x400000000000000AULL;

    // Write in big-endian format
    output[0] = (seconds >> 56) & 0xFF;
    output[1] = (seconds >> 48) & 0xFF;
    output[2] = (seconds >> 40) & 0xFF;
    output[3] = (seconds >> 32) & 0xFF;
    output[4] = (seconds >> 24) & 0xFF;
    output[5] = (seconds >> 16) & 0xFF;
    output[6] = (seconds >> 8) & 0xFF;
    output[7] = seconds & 0xFF;

    output[8] = (nanoseconds >> 24) & 0xFF;
    output[9] = (nanoseconds >> 16) & 0xFF;
    output[10] = (nanoseconds >> 8) & 0xFF;
    output[11] = nanoseconds & 0xFF;
}

/* ============================================================================
 * Random Number Generation
 * ========================================================================== */

void wireguard_random_bytes(void *bytes, size_t size) {
    // Use ESP32 hardware RNG
    esp_fill_random(bytes, size);
}

/* ============================================================================
 * Load Management
 * ========================================================================== */

bool wireguard_is_under_load() {
    // For now, always return false (not under load)
    // Could be enhanced to check:
    // - Free heap memory
    // - CPU usage
    // - Number of active connections
    return false;
}
