#include "hublog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hublog";

#define RING_SIZE   (32 * 1024)
#define HUBLOG_LINE    240
#define FLUSH_MS    10000

static char *s_ring;              // PSRAM
static size_t s_head, s_len;      // circular buffer: [s_head, s_head + s_len)
static uint32_t s_dropped;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig;
static bool s_mounted;
static char *s_flushbuf;          // PSRAM, RING_SIZE

// Tags whose INFO lines are worth keeping; everything at WARN/ERROR is kept.
static const char *const KEEP_INFO[] = {"wifi5g", "hubsys", "web.api", "web.auth", "web",
                                        "sample", "boot", "hublog", "prov", "zb"};

bool hublog_time_valid(void)
{
    return time(NULL) > 1735689600;  // after 2025-01-01: SNTP has synced
}

static bool keep_line(const char *s)
{
    // Lines look like "W (1234) tag: msg" after stripping colour codes.
    if (s[0] != 'W' && s[0] != 'E' && s[0] != 'I') return false;
    const char *p = strchr(s, ')');
    if (!p || p[1] != ' ') return false;
    p += 2;
    const char *colon = strchr(p, ':');
    if (!colon) return false;
    // The Wi-Fi driver prints some lines in pieces: the first call carries only the
    // prefix ("W (1571) wifi:"), the text follows without one. Those are its
    // capability dumps; a prefix with no text is noise either way.
    const char *rest = colon + 1;
    while (*rest == ' ') rest++;
    if (!*rest) return false;
    if (s[0] == 'W' || s[0] == 'E') return true;
    size_t n = colon - p;
    for (size_t i = 0; i < sizeof(KEEP_INFO) / sizeof(KEEP_INFO[0]); i++) {
        if (strlen(KEEP_INFO[i]) == n && strncmp(p, KEEP_INFO[i], n) == 0) return true;
    }
    return false;
}

static void ring_put(const char *s, size_t n)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_len + n > RING_SIZE) {
        s_dropped++;  // flash writer fell behind: drop the new line, keep order
    } else {
        size_t tail = (s_head + s_len) % RING_SIZE;
        size_t first = n < RING_SIZE - tail ? n : RING_SIZE - tail;
        memcpy(s_ring + tail, s, first);
        memcpy(s_ring, s + first, n - first);
        s_len += n;
    }
    taskEXIT_CRITICAL(&s_lock);
}

// Runs in whatever task logged: no flash access, no blocking, small stack use.
static int log_hook(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int r = s_orig ? s_orig(fmt, args) : vprintf(fmt, args);

    if (s_ring) {
        // One buffer, formatted and cleaned in place: this runs on the stack of
        // every task that logs, including the small system event task.
        char line[HUBLOG_LINE];
        size_t body;
        if (hublog_time_valid()) {
            time_t t = time(NULL);
            struct tm tm;
            gmtime_r(&t, &tm);
            body = strftime(line, sizeof(line), "%Y-%m-%dT%H:%M:%SZ ", &tm);
        } else {
            body = snprintf(line, sizeof(line), "+%llds ", esp_timer_get_time() / 1000000);
        }
        int n = vsnprintf(line + body, sizeof(line) - body - 1, fmt, copy);
        if (n > 0) {
            // Strip ANSI colour codes ("\033[0;33m" ... "\033[0m") and newlines.
            size_t o = body;
            for (size_t i = body; line[i]; i++) {
                if (line[i] == '\033') {
                    while (line[i] && line[i] != 'm') i++;
                    if (!line[i]) break;
                    continue;
                }
                if (line[i] == '\r' || line[i] == '\n') continue;
                line[o++] = line[i];
            }
            line[o] = '\0';
            if (o > body && keep_line(line + body)) {
                line[o++] = '\n';
                ring_put(line, o);
            }
        }
    }
    va_end(copy);
    return r;
}

static void rotate_if_needed(void)
{
    struct stat st;
    if (stat(HUBLOG_CUR, &st) == 0 && st.st_size >= HUBLOG_FILE_MAX) {
        remove(HUBLOG_OLD);
        rename(HUBLOG_CUR, HUBLOG_OLD);
    }
}

void hublog_flush(void)
{
    if (!s_mounted || !s_ring) return;
    taskENTER_CRITICAL(&s_lock);
    size_t n = s_len;
    size_t first = n < RING_SIZE - s_head ? n : RING_SIZE - s_head;
    memcpy(s_flushbuf, s_ring + s_head, first);
    memcpy(s_flushbuf + first, s_ring, n - first);
    s_head = (s_head + n) % RING_SIZE;
    s_len = 0;
    uint32_t dropped = s_dropped;
    s_dropped = 0;
    taskEXIT_CRITICAL(&s_lock);
    if (n == 0 && dropped == 0) return;

    rotate_if_needed();
    FILE *f = fopen(HUBLOG_CUR, "a");
    if (!f) return;
    if (dropped) fprintf(f, "[hublog: %u lines dropped, writer fell behind]\n", (unsigned)dropped);
    fwrite(s_flushbuf, 1, n, f);
    fclose(f);
}

static void flush_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FLUSH_MS));
        hublog_flush();
    }
}

static void on_shutdown(void)
{
    ESP_LOGI(TAG, "restarting: log flushed");
    hublog_flush();
}

esp_err_t hublog_init(void)
{
    s_ring = heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_flushbuf = heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring || !s_flushbuf) return ESP_ERR_NO_MEM;
    s_orig = esp_log_set_vprintf(log_hook);

    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .format_if_mount_failed = true,  // first boot: blank partition
        .dont_mount = false,
    };
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount /storage failed: %s (logging to serial only)", esp_err_to_name(err));
        return err;
    }
    mkdir(HUBLOG_DIR, 0755);
    s_mounted = true;
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    ESP_LOGI(TAG, "/storage mounted in %lld ms: %u KB used of %u KB", (esp_timer_get_time() - t0) / 1000,
             (unsigned)(used / 1024), (unsigned)(total / 1024));

    esp_register_shutdown_handler(on_shutdown);
    xTaskCreate(flush_task, "hublog", 3072, NULL, 2, NULL);
    return ESP_OK;
}

static void on_time_sync(struct timeval *tv)
{
    time_t t = tv->tv_sec;
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    ESP_LOGI(TAG, "time synced: %s (uptime %lld s)", buf, esp_timer_get_time() / 1000000);
}

void hublog_start_time_sync(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = on_time_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) ESP_LOGW(TAG, "sntp: %s", esp_err_to_name(err));
}
