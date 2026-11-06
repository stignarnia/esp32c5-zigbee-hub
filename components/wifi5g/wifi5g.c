#include "wifi5g.h"

#include <inttypes.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "wifi5g";

#define MIN_5G_CHANNEL        36
#define SEC_US                (1000LL * 1000)
#define MIN_US                (60 * SEC_US)
#define BACKOFF_MIN_US        (1 * SEC_US)
#define BACKOFF_MAX_US        (30 * SEC_US)
#define FAILOVER_US           (2 * MIN_US)   // per network, before trying the other one
#define DRIVER_RESTART_US     (5 * MIN_US)   // every 5 min of total downtime
#define DRIVER_RESTARTS_FIRST 3
#define RTC_MAGIC             0x57F15A5Eu

// Survive esp_restart() (not power loss). Validated by the magic.
RTC_NOINIT_ATTR static uint32_t s_rtc_magic;
RTC_NOINIT_ATTR static uint32_t s_rtc_wd_reboots;   // consecutive, current outage
RTC_NOINIT_ATTR static uint32_t s_rtc_last_was_wd;  // previous boot ended by our watchdog

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static esp_netif_t *s_netif;
static wifi5g_reboot_gate_t s_gate;
static wifi5g_net_t s_nets[WIFI5G_MAX_NETS];      // in use; changed only under s_lock
static wifi5g_net_t s_defaults[WIFI5G_MAX_NETS];  // from flash.sh (prov)
static int s_nnets;

// A switch requested by the UI, executed by the Wi-Fi task.
static struct {
    bool pending;             // request waiting for the task
    bool active;              // trial running
    bool got_ip;              // set by the IP handler while active
    bool reset_defaults;      // success erases the NVS override instead of writing it
    uint8_t target, prev_net;
    int prev_nnets;
    uint32_t trial_s;
    int64_t start_at_us;      // switch no earlier than this: the HTTP reply must get out first
    int64_t deadline_us;
    wifi5g_net_t req[WIFI5G_MAX_NETS], prev[WIFI5G_MAX_NETS];
} s_trial;
static uint8_t s_ch_hint[WIFI5G_MAX_NETS];  // last channel per network, from NVS
static nvs_handle_t s_nvs;

static struct {
    bool connected;
    uint8_t net;              // index into s_nets
    int64_t down_since_us;    // no connectivity at all since; 0 while connected
    int64_t net_since_us;     // current network failing since (or switched to at)
    int64_t reconnect_at_us;  // 0 = none pending
    int64_t backoff_us;
    uint32_t strikes;         // driver restarts in the current outage
    // After an auth timeout the driver blacklists the AP, then on retry scans every
    // channel (DFS ones passively) before falling back to it: ~8 s. Pinning the
    // retry to that BSSID skips the detour. Unpinned after the next disconnect from
    // a working link, so roaming between APs still works normally.
    bool want_pin, pinned;
    uint8_t pin_bssid[6];
    uint8_t pin_tries;
    // Early auth abort: after an abrupt reset the AP still holds our old (PMF)
    // association and ignores the first auth; the driver waits its full 4 s. The
    // AP is ready well before that (measured: accepted 1.8 s after auth start), so
    // abort once, early, and retry pinned. Healthy connects finish in ~1.3 s.
    int64_t attempt_us;       // when the current connect attempt started, 0 = none
    bool abort_done, abort_pending;  // once per outage; re-armed on GOT_IP
    wifi5g_stats_t stats;
} s;

#define AUTH_ABORT_US (2000LL * 1000)  // after connect(); auth starts ~0.9 s in

static const char *reason_str(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_AUTH_EXPIRE:          return "AUTH_EXPIRE";
    case WIFI_REASON_ASSOC_LEAVE:          return "ASSOC_LEAVE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_BEACON_TIMEOUT:       return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:          return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:            return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:           return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:    return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:      return "CONNECTION_FAIL";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:     return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default:                               return "other";
    }
}

static const char *net_name(uint8_t idx)
{
    return idx == 0 ? "primary" : "backup";
}

static int64_t reboot_threshold_us(uint32_t wd_reboots)
{
    if (wd_reboots == 0) return 20 * MIN_US;
    if (wd_reboots == 1) return 30 * MIN_US;
    return 60 * MIN_US;
}

static void apply_net_ex(uint8_t idx, const uint8_t *bssid)
{
    wifi_config_t wc = {0};
    if (bssid) {
        wc.sta.bssid_set = true;
        memcpy(wc.sta.bssid, bssid, 6);
    }
    taskENTER_CRITICAL(&s_lock);
    strlcpy((char *)wc.sta.ssid, s_nets[idx].ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s_nets[idx].psk, sizeof(wc.sta.password));
    taskEXIT_CRITICAL(&s_lock);
    // Fast scan from the last known channel: connect on the first match. An
    // all-channel scan walks every 5 GHz channel (DFS ones passively) and cost
    // ~7 s per attempt here, after the AP had already been heard in 0.5 s.
    wc.sta.scan_method = WIFI_FAST_SCAN;
    wc.sta.channel = s_ch_hint[idx];
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    // A floor, not an exact match; WPA3 is still used when the AP offers it.
    // (WPA2_WPA3 here rejected plain-WPA3 APs in the reference repo.)
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.required = false;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
}

static void apply_net(uint8_t idx)
{
    apply_net_ex(idx, NULL);
}

static void try_connect(void)
{
    taskENTER_CRITICAL(&s_lock);
    s.attempt_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_lock);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "connect: %s", esp_err_to_name(err));
    }
}

// Radio up and locked to 5 GHz, then connect. The band can only be set after
// esp_wifi_start(), so nothing may connect on WIFI_EVENT_STA_START.
static void radio_up_and_connect(void)
{
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  // mains powered; lowest latency
    try_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    int64_t now = esp_timer_get_time();

    if (id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *e = data;
        taskENTER_CRITICAL(&s_lock);
        s.attempt_us = 0;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "associated: ch %u, bssid " MACSTR ", auth %d", e->channel,
                 MAC2STR(e->bssid), e->authmode);
        if (e->channel < MIN_5G_CHANNEL) {
            // Can't happen with 5G_ONLY; if it ever does, refuse the link.
            ESP_LOGE(TAG, "associated on 2.4 GHz channel %u, refusing", e->channel);
            esp_wifi_disconnect();
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = data;
        taskENTER_CRITICAL(&s_lock);
        s.attempt_us = 0;
        if (s.abort_pending) {
            // Our own early abort: retry right away, not counted as a failure.
            s.abort_pending = false;
            s.reconnect_at_us = now;
            static const uint8_t zero[6] = {0};
            if (memcmp(e->bssid, zero, 6) != 0) {
                s.want_pin = true;  // the abort blacklists the AP too
                memcpy(s.pin_bssid, e->bssid, 6);
            }
            taskEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "auth abort: retrying now (reason %u)", e->reason);
            if (s_task) xTaskNotifyGive(s_task);
            return;
        }
        bool was_connected = s.connected;
        s.connected = false;
        s.stats.connected = false;
        s.stats.channel = 0;
        s.stats.ip = 0;
        s.stats.disconnects++;
        s.stats.last_reason = e->reason;
        if (e->reason == WIFI_REASON_NO_AP_FOUND) s.stats.no_ap_found++;
        if (s.down_since_us == 0) s.down_since_us = now;
        if (was_connected) {
            s.net_since_us = now;
            s.want_pin = false;
            s.pin_tries = 0;
        } else if (e->reason == WIFI_REASON_AUTH_EXPIRE && s.pin_tries < 2) {
            s.want_pin = true;
            memcpy(s.pin_bssid, e->bssid, 6);
            s.pin_tries++;
        } else {
            s.want_pin = false;  // pinning didn't help: let the driver choose again
        }
        bool immediate = s.want_pin && s.pin_tries == 1;  // the AP was there 4 s ago: no backoff
        int64_t backoff = immediate ? 0 : s.backoff_us;
        s.reconnect_at_us = now + backoff;
        if (!immediate) {
            s.backoff_us = s.backoff_us * 2 > BACKOFF_MAX_US ? BACKOFF_MAX_US : s.backoff_us * 2;
        }
        uint8_t net = s.net;
        taskEXIT_CRITICAL(&s_lock);
        if (immediate && s_task) xTaskNotifyGive(s_task);

        ESP_LOGW(TAG, "%s (%s): reason %u %s, rssi %d, retry in %lld s",
                 was_connected ? "disconnected" : "connect failed", net_name(net), e->reason,
                 reason_str(e->reason), e->rssi, backoff / SEC_US);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id != IP_EVENT_STA_GOT_IP) return;
    const ip_event_got_ip_t *e = data;
    int64_t now = esp_timer_get_time();

    wifi_ap_record_t ap = {0};
    esp_wifi_sta_get_ap_info(&ap);

    taskENTER_CRITICAL(&s_lock);
    int64_t down_for = s.down_since_us ? now - s.down_since_us : 0;
    s.connected = true;
    s.down_since_us = 0;
    s.reconnect_at_us = 0;
    s.backoff_us = BACKOFF_MIN_US;
    s.strikes = 0;
    s.abort_done = false;
    s.stats.connected = true;
    s.stats.net = s.net;
    s.stats.channel = ap.primary;
    s.stats.rssi = ap.rssi;
    s.stats.ip = e->ip_info.ip.addr;
    s.stats.connects++;
    if (s.stats.first_ip_ms < 0) s.stats.first_ip_ms = now / 1000;
    s.stats.watchdog_reboots = 0;
    uint8_t net = s.net;
    if (s_trial.active) {
        s_trial.got_ip = true;
        s.reconnect_at_us = 0;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (s_trial.got_ip && s_task) xTaskNotifyGive(s_task);
    s_rtc_wd_reboots = 0;

    if (s_nvs && ap.primary >= MIN_5G_CHANNEL && ap.primary != s_ch_hint[net]) {
        s_ch_hint[net] = ap.primary;  // written only when it changes: no flash wear
        char key[4] = {'c', 'h', (char)('0' + net), '\0'};
        if (nvs_set_u8(s_nvs, key, ap.primary) == ESP_OK) nvs_commit(s_nvs);
    }

    bool dfs = ap.primary >= 52 && ap.primary <= 144;
    ESP_LOGI(TAG, "got ip " IPSTR " on '%s' (%s), ch %u%s, rssi %d, after %lld ms down",
             IP2STR(&e->ip_info.ip), (const char *)ap.ssid, net_name(net), ap.primary,
             dfs ? " (DFS)" : "", ap.rssi, down_for / 1000);
    // Machine-readable line; flash.sh waits for it.
    ESP_LOGI(TAG, "HUB_READY ip=" IPSTR " net=%s ch=%u rssi=%d boot_ms=%lld",
             IP2STR(&e->ip_info.ip), net_name(net), ap.primary, ap.rssi, now / 1000);
}

static void driver_restart(uint32_t strikes)
{
    ESP_LOGW(TAG, "watchdog: driver restart (#%" PRIu32 " this outage)", strikes + 1);
    esp_wifi_stop();
    radio_up_and_connect();
}

static void failover(int64_t now)
{
    taskENTER_CRITICAL(&s_lock);
    uint8_t from = s.net;
    uint8_t to = (uint8_t)((from + 1) % s_nnets);
    s.net = to;
    s.stats.net = to;
    s.net_since_us = now;
    s.reconnect_at_us = 0;
    s.backoff_us = BACKOFF_MIN_US;
    s.stats.failovers++;
    s.want_pin = s.pinned = false;
    s.pin_tries = 0;
    taskEXIT_CRITICAL(&s_lock);

    ESP_LOGW(TAG, "%s '%s' unreachable for %lld s, trying %s '%s'", net_name(from),
             s_nets[from].ssid, FAILOVER_US / SEC_US, net_name(to), s_nets[to].ssid);
    esp_wifi_disconnect();
    apply_net(to);
    try_connect();
}

// ---- networks saved from the UI (app NVS, namespace "wifi5g") -------------------

static const char *const NVS_SSID[WIFI5G_MAX_NETS] = {"ssid0", "ssid1"};
static const char *const NVS_PSK[WIFI5G_MAX_NETS] = {"psk0", "psk1"};

static bool nets_load(wifi5g_net_t out[WIFI5G_MAX_NETS])
{
    if (!s_nvs) return false;
    memset(out, 0, sizeof(wifi5g_net_t) * WIFI5G_MAX_NETS);
    for (int i = 0; i < WIFI5G_MAX_NETS; i++) {
        size_t len = sizeof(out[i].ssid);
        if (nvs_get_str(s_nvs, NVS_SSID[i], out[i].ssid, &len) != ESP_OK) out[i].ssid[0] = '\0';
        len = sizeof(out[i].psk);
        if (nvs_get_str(s_nvs, NVS_PSK[i], out[i].psk, &len) != ESP_OK) out[i].psk[0] = '\0';
    }
    return out[0].ssid[0] != '\0';
}

static void nets_save(const wifi5g_net_t nets[WIFI5G_MAX_NETS], bool erase)
{
    if (!s_nvs) return;
    for (int i = 0; i < WIFI5G_MAX_NETS; i++) {
        if (erase || nets[i].ssid[0] == '\0') {
            nvs_erase_key(s_nvs, NVS_SSID[i]);
            nvs_erase_key(s_nvs, NVS_PSK[i]);
        } else {
            nvs_set_str(s_nvs, NVS_SSID[i], nets[i].ssid);
            nvs_set_str(s_nvs, NVS_PSK[i], nets[i].psk);
        }
    }
    nvs_commit(s_nvs);
}

// ---- switching ---------------------------------------------------------------------

static void switch_to(uint8_t target, int64_t now)
{
    taskENTER_CRITICAL(&s_lock);
    s.net = target;
    s.stats.net = target;
    s.net_since_us = now;
    s.reconnect_at_us = 0;
    s.backoff_us = BACKOFF_MIN_US;
    s.want_pin = s.pinned = false;
    s.pin_tries = 0;
    taskEXIT_CRITICAL(&s_lock);
    esp_wifi_disconnect();
    apply_net(target);
    try_connect();
}

// Runs on the Wi-Fi task: starts a requested trial, then commits or reverts it.
static void trial_tick(int64_t now)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_trial.pending && now >= s_trial.start_at_us) {
        s_trial.pending = false;
        memcpy(s_trial.prev, s_nets, sizeof(s_nets));
        s_trial.prev_net = s.net;
        s_trial.prev_nnets = s_nnets;
        memcpy(s_nets, s_trial.req, sizeof(s_nets));
        s_nnets = s_nets[1].ssid[0] ? 2 : 1;
        s_trial.active = true;
        s_trial.got_ip = false;
        s_trial.deadline_us = now + (int64_t)s_trial.trial_s * SEC_US;
        s.stats.trial = WIFI5G_TRIAL_RUNNING;
        s.stats.trial_target = s_trial.target;
        uint8_t target = s_trial.target;
        uint32_t secs = s_trial.trial_s;
        char ssid[33];
        strlcpy(ssid, s_nets[target].ssid, sizeof(ssid));
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "trial: switching to %s '%s', %" PRIu32 " s to get an IP", net_name(target),
                 ssid, secs);
        switch_to(target, now);
        return;
    }
    if (s_trial.active && s_trial.got_ip) {
        s_trial.active = false;
        s.stats.trial = WIFI5G_TRIAL_OK;
        bool erase = s_trial.reset_defaults;
        s.stats.custom = !erase;
        wifi5g_net_t keep[WIFI5G_MAX_NETS];
        memcpy(keep, s_nets, sizeof(keep));
        taskEXIT_CRITICAL(&s_lock);
        nets_save(keep, erase);
        ESP_LOGI(TAG, "trial: OK, %s", erase ? "back to the flash.sh networks" : "networks saved");
        return;
    }
    if (s_trial.active && now >= s_trial.deadline_us) {
        s_trial.active = false;
        memcpy(s_nets, s_trial.prev, sizeof(s_nets));
        s_nnets = s_trial.prev_nnets;
        s.stats.trial = WIFI5G_TRIAL_REVERTED;
        uint8_t back = s_trial.prev_net;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "trial: no IP in time, reverting to %s", net_name(back));
        switch_to(back, now);
        return;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static void watchdog_reboot(int64_t down_for, uint32_t strikes)
{
    s_rtc_wd_reboots++;
    s_rtc_last_was_wd = 1;
    s_rtc_magic = RTC_MAGIC;
    ESP_LOGE(TAG, "watchdog: no Wi-Fi for %lld min after %" PRIu32 " driver restarts, rebooting "
             "(reboot #%" PRIu32 " this outage)", down_for / MIN_US, strikes, s_rtc_wd_reboots);
    vTaskDelay(pdMS_TO_TICKS(200));  // let the log drain
    esp_restart();
}

// Live link quality for the UI / samples. RSSI comes from the last beacon; the
// channel can change under us when the AP moves (DFS radar, auto-channel CSA).
static void refresh_link(void)
{
    int rssi = 0;
    wifi_ap_record_t ap = {0};
    bool have_rssi = esp_wifi_sta_get_rssi(&rssi) == ESP_OK && rssi != 0;
    bool have_ap = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    taskENTER_CRITICAL(&s_lock);
    if (s.connected) {
        if (have_rssi) s.stats.rssi = (int8_t)rssi;
        if (have_ap && ap.primary) s.stats.channel = ap.primary;
    }
    taskEXIT_CRITICAL(&s_lock);
}

// One task owns every recovery action, ticking once a second. Nothing here is
// reset by the events it reacts to: downtime is measured from the first
// disconnect, not the latest one (the reference repo's watchdog never fired
// because each failed retry re-armed it).
static void wifi_task(void *arg)
{
    for (;;) {
        taskENTER_CRITICAL(&s_lock);
        bool attempting = s.attempt_us != 0 || s_trial.pending;
        taskEXIT_CRITICAL(&s_lock);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(attempting ? 100 : 1000));
        int64_t now = esp_timer_get_time();

        trial_tick(now);

        taskENTER_CRITICAL(&s_lock);
        bool do_abort = !s.connected && s.attempt_us && !s.abort_done && !s.pinned &&
                        s_ch_hint[s.net] && now - s.attempt_us >= AUTH_ABORT_US;
        if (do_abort) {
            s.abort_done = true;
            s.abort_pending = true;
        }
        taskEXIT_CRITICAL(&s_lock);
        if (do_abort) {
            ESP_LOGW(TAG, "auth abort: no association %lld ms after connect, aborting once",
                     AUTH_ABORT_US / 1000);
            esp_wifi_disconnect();
            continue;
        }

        taskENTER_CRITICAL(&s_lock);
        bool connected = s.connected;
        bool reconnect = s.reconnect_at_us && now >= s.reconnect_at_us;
        if (reconnect) s.reconnect_at_us = 0;
        int64_t down_for = s.down_since_us ? now - s.down_since_us : 0;
        int64_t net_down_for = now - s.net_since_us;
        uint32_t strikes = s.strikes;
        taskEXIT_CRITICAL(&s_lock);

        if (connected) {
            refresh_link();
            continue;
        }

        if (down_for >= (int64_t)(strikes + 1) * DRIVER_RESTART_US) {
            driver_restart(strikes);
            taskENTER_CRITICAL(&s_lock);
            s.strikes++;
            s.stats.driver_restarts++;
            taskEXIT_CRITICAL(&s_lock);
            continue;
        }

        if (strikes >= DRIVER_RESTARTS_FIRST && down_for >= reboot_threshold_us(s_rtc_wd_reboots)) {
            if (s_gate == NULL || s_gate()) {
                watchdog_reboot(down_for, strikes);
            }
            continue;  // not safe yet: checked again next tick
        }

        if (s_nnets > 1 && net_down_for >= FAILOVER_US && !s_trial.active) {
            failover(now);
            continue;
        }

        if (reconnect) {
            taskENTER_CRITICAL(&s_lock);
            bool want = s.want_pin, changed = s.want_pin != s.pinned;
            uint8_t net = s.net, bssid[6];
            memcpy(bssid, s.pin_bssid, 6);
            s.pinned = want;
            taskEXIT_CRITICAL(&s_lock);
            if (changed) {
                if (want) ESP_LOGI(TAG, "retrying pinned to " MACSTR " (skip the blacklist detour)", MAC2STR(bssid));
                apply_net_ex(net, want ? bssid : NULL);
            }
            try_connect();
        }
    }
}

// Validates the RTC counters (garbage after power loss) and reports a watchdog reboot.
static void rtc_state_init(void)
{
    if (s_rtc_magic != RTC_MAGIC || esp_reset_reason() != ESP_RST_SW) {
        s_rtc_magic = RTC_MAGIC;
        s_rtc_wd_reboots = 0;
        s_rtc_last_was_wd = 0;
        return;
    }
    if (s_rtc_last_was_wd) {
        ESP_LOGW(TAG, "previous boot ended by the Wi-Fi watchdog (%" PRIu32 " in this outage)",
                 s_rtc_wd_reboots);
        s_rtc_last_was_wd = 0;
    }
}

esp_err_t wifi5g_start(const wifi5g_net_t defaults[WIFI5G_MAX_NETS], const char *hostname,
                       const char *country)
{
    if (defaults[0].ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    memcpy(s_defaults, defaults, sizeof(s_defaults));

    rtc_state_init();
    memset(&s, 0, sizeof(s));
    int64_t now = esp_timer_get_time();
    s.backoff_us = BACKOFF_MIN_US;
    s.down_since_us = now;  // "down" until the first IP
    s.net_since_us = now;
    s.stats.first_ip_ms = -1;
    s.stats.watchdog_reboots = s_rtc_wd_reboots;

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, hostname);  // DHCP announces it to the router

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  // creds come from us each boot
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (country && country[0]) {
        err = esp_wifi_set_country_code(country, true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "country '%s' rejected: %s", country, esp_err_to_name(err));
        }
    }

    // App NVS (initialised by the caller): channel hints and UI-saved networks.
    if (nvs_open("wifi5g", NVS_READWRITE, &s_nvs) == ESP_OK) {
        nvs_get_u8(s_nvs, "ch0", &s_ch_hint[0]);
        nvs_get_u8(s_nvs, "ch1", &s_ch_hint[1]);
    } else {
        s_nvs = 0;
    }
    wifi5g_net_t saved[WIFI5G_MAX_NETS];
    if (nets_load(saved)) {
        memcpy(s_nets, saved, sizeof(s_nets));
        s.stats.custom = true;
        ESP_LOGI(TAG, "using networks saved from the UI (flash.sh defaults kept for reset)");
    } else {
        memcpy(s_nets, s_defaults, sizeof(s_nets));
    }
    s_nnets = s_nets[1].ssid[0] ? 2 : 1;

    apply_net(0);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL));

    if (xTaskCreate(wifi_task, "wifi5g", 4096, NULL, 5, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    if (s_nnets > 1) {
        ESP_LOGI(TAG, "connecting to '%s', backup '%s', 5 GHz only", s_nets[0].ssid, s_nets[1].ssid);
    } else {
        ESP_LOGI(TAG, "connecting to '%s', 5 GHz only (no backup network)", s_nets[0].ssid);
    }
    radio_up_and_connect();
    return ESP_OK;
}

void wifi5g_set_reboot_gate(wifi5g_reboot_gate_t gate)
{
    s_gate = gate;
}

void wifi5g_get_stats(wifi5g_stats_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s.stats;
    strlcpy(out->ssid, s_nets[s.net].ssid, sizeof(out->ssid));
    taskEXIT_CRITICAL(&s_lock);
}

void wifi5g_get_nets(wifi5g_net_t out[WIFI5G_MAX_NETS])
{
    taskENTER_CRITICAL(&s_lock);
    memcpy(out, s_nets, sizeof(s_nets));
    taskEXIT_CRITICAL(&s_lock);
}

static bool net_valid(const wifi5g_net_t *n)
{
    size_t ssid_len = strnlen(n->ssid, sizeof(n->ssid));
    size_t psk_len = strnlen(n->psk, sizeof(n->psk));
    if (ssid_len == 0 || ssid_len > 32) return false;
    return psk_len >= 8 && psk_len <= 64;  // WPA2/WPA3-Personal; open networks refused
}

static esp_err_t request_trial(const wifi5g_net_t nets[WIFI5G_MAX_NETS], uint8_t target,
                               uint32_t trial_s, bool reset_defaults)
{
    if (target >= WIFI5G_MAX_NETS || !net_valid(&nets[target])) return ESP_ERR_INVALID_ARG;
    if (nets[0].ssid[0] && !net_valid(&nets[0])) return ESP_ERR_INVALID_ARG;
    if (nets[1].ssid[0] && !net_valid(&nets[1])) return ESP_ERR_INVALID_ARG;
    if (trial_s < 20) trial_s = 20;

    taskENTER_CRITICAL(&s_lock);
    if (s_trial.pending || s_trial.active) {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(s_trial.req, nets, sizeof(s_trial.req));
    s_trial.target = target;
    s_trial.trial_s = trial_s;
    s_trial.reset_defaults = reset_defaults;
    // Measured: switching 30 ms after replying lost the reply in flight; the
    // client got it only when the board came back (60 s later on a failed trial).
    s_trial.start_at_us = esp_timer_get_time() + 800 * 1000;
    s_trial.pending = true;
    // Visible immediately: a status read right after the request must never show
    // the previous trial's result (the UI would report it as this one's).
    s.stats.trial = WIFI5G_TRIAL_RUNNING;
    s.stats.trial_target = target;
    taskEXIT_CRITICAL(&s_lock);
    if (s_task) xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t wifi5g_apply(const wifi5g_net_t nets[WIFI5G_MAX_NETS], uint8_t target, uint32_t trial_s)
{
    return request_trial(nets, target, trial_s, false);
}

esp_err_t wifi5g_reset_to_defaults(uint32_t trial_s)
{
    return request_trial(s_defaults, 0, trial_s, true);
}

void wifi5g_prepare_reboot(void)
{
    if (wifi5g_is_connected()) {
        esp_wifi_disconnect();  // deauth: the AP forgets us, next boot logs in at once
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

bool wifi5g_is_connected(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool c = s.connected;
    taskEXIT_CRITICAL(&s_lock);
    return c;
}
