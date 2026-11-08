// Hub-side wrapper around microlink: settings, start once Wi-Fi is up, watch state.
//
// Changes that need a running node stopped (turning it off, logging out) are applied
// by a restart, which the caller performs: microlink's own stop path waits a fixed
// 3 s while a task can sit 10 s in a socket timeout, then frees memory those tasks
// may still use. A restart costs Zigbee ~2 s; these changes are rare and deliberate.

#include <string.h>
#include <ctype.h>
#include "tailscale.h"
#include "microlink.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/tcpip.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "esp_netif_net_stack.h"
#include "hub_lwip_hooks.h"

static const char *TAG = "ts";

#define NVS_NS "ts"

static SemaphoreHandle_t s_mtx;     // settings + NVS writes (httpd task vs ts task)
static bool s_enabled = true;
static bool s_registered;
static char s_auth_key[128];        // read by microlink's coord task on each registration
static char s_hostname[33];

// Crash guard. A bug in Tailscale must not crash-loop the hub (each loop takes Zigbee down). Two consecutive crash resets
// while the node was running suspend it on the next boot. Kept in RTC memory,
// which survives a crash reset; cleared by a clean boot or 10 min of running.
#define CRASH_MAGIC     0x54534352u
#define CRASH_LIMIT     2
#define STABLE_MS       (10 * 60 * 1000)
static RTC_NOINIT_ATTR uint32_t s_rtc_magic, s_rtc_running, s_rtc_crashes;
static bool s_suspended;

static TaskHandle_t s_task;
static microlink_t *s_ml;           // set once by ts_task, never freed
static volatile int64_t s_connected_ms = -1;
static volatile bool s_ui_watching;  // set by the web server (WebSocket clients)

// ---- settings ----------------------------------------------------------------------

static void cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;  // first boot: defaults
    uint8_t v;
    if (nvs_get_u8(h, "on", &v) == ESP_OK) s_enabled = v != 0;
    if (nvs_get_u8(h, "reg", &v) == ESP_OK) s_registered = v != 0;
    size_t len = sizeof(s_auth_key);
    if (nvs_get_str(h, "key", s_auth_key, &len) != ESP_OK) s_auth_key[0] = '\0';
    nvs_close(h);
}

// Writes all three settings. Caller holds s_mtx.
static esp_err_t cfg_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "on", s_enabled);
    if (err == ESP_OK) err = nvs_set_u8(h, "reg", s_registered);
    if (err == ESP_OK) {
        err = s_auth_key[0] ? nvs_set_str(h, "key", s_auth_key) : nvs_erase_key(h, "key");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "saving settings failed: %s", esp_err_to_name(err));
    return err;
}


// ---- strong host for the tailnet IP (lwIP input hook, hub_lwip_hooks.h) --------------

static volatile uint32_t s_guard_ip_nbo;        // tailnet IP, network order; 0 = not armed
static struct netif *volatile s_guard_sta;      // lwIP netif of the Wi-Fi station
static volatile uint32_t s_guard_drops;

// Runs on the TCP/IP thread for every IPv4 packet, before lwIP's address checks.
int hub_ip4_input_hook(struct pbuf *p, struct netif *inp)
{
    uint32_t ip = s_guard_ip_nbo;
    if (!ip || inp != s_guard_sta || p->len < 20) return 0;
    const uint8_t *h = (const uint8_t *)p->payload;
    if ((h[0] >> 4) != 4) return 0;
    uint32_t dst;
    memcpy(&dst, h + 16, 4);
    if (dst != ip) return 0;
    s_guard_drops++;
    pbuf_free(p);
    return 1;
}

static void guard_arm(uint32_t ip_hbo)
{
    if (s_guard_ip_nbo == htonl(ip_hbo) && s_guard_sta) return;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    s_guard_sta = sta ? (struct netif *)esp_netif_get_netif_impl(sta) : NULL;
    s_guard_ip_nbo = htonl(ip_hbo);
    ESP_LOGI(TAG, "dropping packets for the tailnet IP that arrive on Wi-Fi");
}

uint32_t ts_tailnet_ip(void)
{
    return ntohl(s_guard_ip_nbo);
}

uint32_t ts_guard_drops(void)
{
    return s_guard_drops;
}

// ---- node task ---------------------------------------------------------------------

static bool sta_has_ip(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    return sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
}

static const char *state_str(microlink_state_t st)
{
    switch (st) {
    case ML_STATE_IDLE:         return "off";
    case ML_STATE_WIFI_WAIT:    return "waiting for Wi-Fi";
    case ML_STATE_CONNECTING:   return "connecting";
    case ML_STATE_REGISTERING:  return "registering";
    case ML_STATE_CONNECTED:    return "connected";
    case ML_STATE_RECONNECTING: return "reconnecting";
    case ML_STATE_ERROR:        return "error";
    default:                    return "unknown";
    }
}

// First successful registration: the node key now identifies the hub. The auth key
// is erased so a used (or reusable) key doesn't sit on the board.
static void on_registered(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (!s_registered) {
        s_registered = true;
        memset(s_auth_key, 0, sizeof(s_auth_key));
        cfg_save();
        ESP_LOGI(TAG, "registered; auth key erased from the hub");
    }
    xSemaphoreGive(s_mtx);
}

// Internal-RAM stack on purpose: microlink_init() writes the node keys to NVS on the
// first run, and on_registered() writes settings (flash writes need an internal
// stack). microlink's own tasks use PSRAM stacks.
static void ts_task(void *arg)
{
    while (!sta_has_ip()) vTaskDelay(pdMS_TO_TICKS(500));

    int64_t t0 = esp_timer_get_time() / 1000;
    microlink_config_t cfg = {
        .auth_key = s_auth_key,  // "" once registered: RegisterRequest then carries no Auth
        .device_name = s_hostname,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
    };
    microlink_t *ml = microlink_init(&cfg);
    if (!ml || microlink_start(ml) != ESP_OK) {
        ESP_LOGE(TAG, "start failed");
        s_task = NULL;
        vTaskDelete(NULL);
    }
    s_ml = ml;
    microlink_set_ui_watching(ml, s_ui_watching);
    s_rtc_running = 1;
    ESP_LOGI(TAG, "started as %s (%s)", s_hostname, s_registered ? "registered" : "registering with the auth key");

    microlink_state_t last = (microlink_state_t)-1;
    uint32_t last_ip = 0;
    char last_err[96] = "";
    for (;;) {
        microlink_state_t st = microlink_get_state(ml);
        uint32_t ip = microlink_get_vpn_ip(ml);
        const char *err = microlink_get_last_error(ml);
        // The tailnet IP comes only from an accepted registration (MapResponse):
        // record it right away, so a failure later on doesn't keep the key around.
        if (ip != 0 && !err[0] && !s_registered) on_registered();
        if (st == ML_STATE_CONNECTED && ip != 0) guard_arm(ip);
        if (st == ML_STATE_CONNECTED && ip != 0) {
            if (s_connected_ms < 0) {
                s_connected_ms = esp_timer_get_time() / 1000;
                char ipstr[16];
                microlink_ip_to_str(ip, ipstr);
                ESP_LOGI(TAG, "TS_READY ip=%s name=%s took_ms=%lld", ipstr,
                         microlink_get_self_name(ml), s_connected_ms - t0);
            }
        }
        if (st != last || ip != last_ip) {
            char ipstr[16];
            microlink_ip_to_str(ip, ipstr);
            ESP_LOGI(TAG, "state %s, ip %s", state_str(st), ip ? ipstr : "-");
            last = st;
            last_ip = ip;
        }
        if (s_rtc_crashes && s_connected_ms >= 0 &&
            esp_timer_get_time() / 1000 - s_connected_ms > STABLE_MS) {
            s_rtc_crashes = 0;
            ESP_LOGI(TAG, "stable for 10 min: crash count cleared");
        }
        if (strcmp(err, last_err) != 0) {
            if (err[0]) ESP_LOGW(TAG, "control server: %s", err);
            strlcpy(last_err, err, sizeof(last_err));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Caller holds s_mtx.
static void maybe_start(void)
{
    if (s_task || s_suspended || !s_enabled || (!s_registered && !s_auth_key[0])) return;
    if (xTaskCreate(ts_task, "ts", 4096, NULL, 2, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "no memory for the ts task");
    }
}

// ---- public ------------------------------------------------------------------------

// ---- diagnostics: who holds lwIP's core lock ----------------------------------------
// Every socket call in the system (the web server included) waits while another task
// holds lwIP's core lock. Sampled every 50 ms; a holder that keeps it > 200 ms is logged
// with its name, and again when it lets go.

extern sys_mutex_t lock_tcpip_core;

// lwIP has CONFIG_LWIP_MAX_SOCKETS (24) descriptors; when they run out, accept() fails
// and lwIP resets the new connection, and connects wait meanwhile. Count them by
// probing each descriptor; list them (type, peer) when nearly all are taken.
static void socket_census(int64_t now)
{
    static int last_n = -1, peak;
    static int64_t last_log;
    static bool listed;
    int n = 0;
    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; fd++) {
        int type;
        socklen_t len = sizeof(type);
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0) n++;
    }
    if (n > peak) peak = n;
    if (n != last_n && now - last_log > 5000000) {
        ESP_LOGI(TAG, "lwIP sockets in use: %d of %d (peak %d)", n, CONFIG_LWIP_MAX_SOCKETS, peak);
        last_n = n;
        last_log = now;
    }
    if (n >= CONFIG_LWIP_MAX_SOCKETS - 3 && !listed) {
        listed = true;
        for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; fd++) {
            int type;
            socklen_t len = sizeof(type);
            if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) != 0) continue;
            struct sockaddr_in local = {0}, peer = {0};
            socklen_t ll = sizeof(local), pl = sizeof(peer);
            getsockname(fd, (struct sockaddr *)&local, &ll);
            bool connected = getpeername(fd, (struct sockaddr *)&peer, &pl) == 0;
            char lip[16], pip[16];
            inet_ntoa_r(local.sin_addr, lip, sizeof(lip));
            inet_ntoa_r(peer.sin_addr, pip, sizeof(pip));
            ESP_LOGW(TAG, "  fd %d %s local %s:%u peer %s:%u", fd, type == SOCK_STREAM ? "tcp" : "udp",
                     lip, ntohs(local.sin_port), connected ? pip : "-", connected ? ntohs(peer.sin_port) : 0);
        }
    } else if (n < CONFIG_LWIP_MAX_SOCKETS - 6) {
        listed = false;
    }
}

static void lock_watch_task(void *arg)
{
    TaskHandle_t prev = NULL;
    int64_t since = 0;
    bool reported = false;
    int64_t last_wake = esp_timer_get_time();
    for (int tick = 0;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        // Scheduling lateness: this task has priority 15; waking > 300 ms late means
        // the CPU was held (flash erase with cache off, a long critical section, ISRs).
        int64_t woke = esp_timer_get_time();
        if (woke - last_wake > 350000) {
            ESP_LOGW(TAG, "CPU stall: watch task woke %lld ms late", (long long)((woke - last_wake) / 1000 - 50));
        }
        last_wake = woke;
        if (tick % 20 == 0) socket_census(esp_timer_get_time());
        TaskHandle_t h = xSemaphoreGetMutexHolder(lock_tcpip_core);
        int64_t now = esp_timer_get_time();
        if (h != prev) {
            if (reported && prev) {
                ESP_LOGW(TAG, "lwIP core lock: %s let go after ~%lld ms", pcTaskGetName(prev),
                         (long long)((now - since) / 1000));
            }
            prev = h;
            since = now;
            reported = false;
        } else if (h && !reported && now - since > 200000) {
            ESP_LOGW(TAG, "lwIP core lock held by %s for > 200 ms", pcTaskGetName(h));
            reported = true;
        }
    }
}

esp_err_t ts_init(const char *hostname)
{
    if (s_mtx) return ESP_ERR_INVALID_STATE;
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return ESP_ERR_NO_MEM;
    strlcpy(s_hostname, hostname && hostname[0] ? hostname : "zigbee-hub", sizeof(s_hostname));
    cfg_load();
    // PSRAM stack: it never writes flash (diagnostics only)
    xTaskCreateWithCaps(lock_watch_task, "lockwatch", 4096, NULL, 15, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    esp_reset_reason_t rr = esp_reset_reason();
    bool crashed = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
                   rr == ESP_RST_WDT || rr == ESP_RST_CPU_LOCKUP;
    if (s_rtc_magic != CRASH_MAGIC) {
        s_rtc_magic = CRASH_MAGIC;
        s_rtc_running = 0;
        s_rtc_crashes = 0;
    }
    if (!crashed) s_rtc_crashes = 0;
    else if (s_rtc_running) s_rtc_crashes++;
    s_rtc_running = 0;
    if (s_rtc_crashes >= CRASH_LIMIT) {
        s_suspended = true;
        ESP_LOGE(TAG, "suspended: the hub crashed %u times in a row while Tailscale was running "
                 "(turn it on again in Settings to retry)", (unsigned)s_rtc_crashes);
    }
    ESP_LOGI(TAG, "%s, %s", s_enabled ? "enabled" : "off",
             s_registered ? "registered" : s_auth_key[0] ? "auth key stored" : "no auth key");
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    maybe_start();
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}

esp_err_t ts_set_auth_key(const char *key)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    size_t n = key ? strlen(key) : 0;
    if (n < 16 || n >= sizeof(s_auth_key) || strncmp(key, "tskey-", 6) != 0) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)key[i]) && key[i] != '-' && key[i] != '_') return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_registered) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        // A running node that hasn't registered yet picks the new key up on its next
        // registration attempt (it retries with backoff).
        strlcpy(s_auth_key, key, sizeof(s_auth_key));
        err = cfg_save();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "auth key stored");
            maybe_start();
        }
    }
    xSemaphoreGive(s_mtx);
    return err;
}

esp_err_t ts_set_enabled(bool on, bool *reboot_needed)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    *reboot_needed = false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_enabled = on;
    if (on && s_suspended) {  // explicit retry after a crash suspension
        s_suspended = false;
        s_rtc_crashes = 0;
    }
    esp_err_t err = cfg_save();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s", on ? "enabled" : "disabled");
        if (on) maybe_start();
        else *reboot_needed = s_task != NULL;
    }
    xSemaphoreGive(s_mtx);
    return err;
}

esp_err_t ts_logout(bool *reboot_needed)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    microlink_factory_reset();  // node, machine and DISCO keys
    s_registered = false;
    memset(s_auth_key, 0, sizeof(s_auth_key));
    esp_err_t err = cfg_save();
    *reboot_needed = s_task != NULL;
    ESP_LOGW(TAG, "logged out: node key erased");
    xSemaphoreGive(s_mtx);
    return err;
}

void ts_get_status(ts_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = s_enabled;
    out->suspended = s_suspended;
    out->registered = s_registered;
    out->key_pending = s_auth_key[0] != '\0' && !s_registered;
    out->connected_ms = s_connected_ms;
    microlink_t *ml = s_ml;
    if (!ml) {
        if (s_suspended) {
            out->state = "suspended";
            strlcpy(out->error, "the hub crashed twice in a row while Tailscale was running", sizeof(out->error));
            return;
        }
        out->state = !s_enabled ? "off"
                   : (s_registered || s_auth_key[0]) ? "waiting for Wi-Fi"
                   : "needs an auth key";
        return;
    }
    out->state = state_str(microlink_get_state(ml));
    strlcpy(out->error, microlink_get_last_error(ml), sizeof(out->error));
    strlcpy(out->name, microlink_get_self_name(ml), sizeof(out->name));
    out->ip = microlink_get_vpn_ip(ml);
    out->key_expiry = microlink_get_key_expiry(ml);
    out->key_expired = microlink_key_expired(ml);
    if (out->key_expired && !out->error[0]) {
        strlcpy(out->error, "node key expired: log out and add a new auth key", sizeof(out->error));
    }
    out->peer_count = microlink_get_peer_count(ml);
    strlcpy(out->home_derp, microlink_get_home_derp(ml), sizeof(out->home_derp));
}

int ts_get_peers(ts_peer_t *out, int max)
{
    microlink_t *ml = s_ml;
    if (!ml) return 0;
    int n = 0;
    int count = microlink_get_peer_count(ml);
    for (int i = 0; i < count && n < max; i++) {
        microlink_peer_info_t p = {0};
        if (microlink_get_peer_info(ml, i, &p) != ESP_OK) continue;
        memset(&out[n], 0, sizeof(out[n]));
        strlcpy(out[n].name, p.hostname, sizeof(out[n].name));
        out[n].ip = p.vpn_ip;
        out[n].online = p.online_state;
        out[n].direct = p.direct_path;
        strlcpy(out[n].derp, p.derp, sizeof(out[n].derp));
        n++;
    }
    return n;
}

// ---- diagnostics -------------------------------------------------------------------

static const char *const s_ml_tags[] = {"ml_wg_mgr", "ml_derp", "ml_stun", "ml_coord", "ml_net_io"};
static esp_timer_handle_t s_debug_timer;

static void debug_logs_set(esp_log_level_t lvl)
{
    for (size_t i = 0; i < sizeof(s_ml_tags) / sizeof(s_ml_tags[0]); i++) esp_log_level_set(s_ml_tags[i], lvl);
}

static void debug_off_cb(void *arg)
{
    debug_logs_set(ESP_LOG_INFO);
    ESP_LOGI(TAG, "Tailscale debug logs off");
}

// Per-packet lines cost ~7 ms each on the 115200-baud console, inside the tasks that
// move tunnel packets: hence a timer, never left on.
void ts_debug_logs(int seconds)
{
    if (!s_debug_timer) {
        const esp_timer_create_args_t a = {.callback = debug_off_cb, .name = "ts_debug"};
        if (esp_timer_create(&a, &s_debug_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_debug_timer);
    debug_logs_set(ESP_LOG_DEBUG);
    esp_timer_start_once(s_debug_timer, (uint64_t)seconds * 1000000);
    ESP_LOGI(TAG, "Tailscale debug logs on for %d s", seconds);
}

void ts_set_ui_watching(bool watching)
{
    s_ui_watching = watching;
    if (s_ml) microlink_set_ui_watching(s_ml, watching);
}
