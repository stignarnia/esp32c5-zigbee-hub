// esp32c5-zigbee-hub — boot sequence: hardware/build report, provisioning, Zigbee
// (before Wi-Fi), automations, 5 GHz Wi-Fi + mDNS, web UI, Tailscale.

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "automation.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "mdns.h"
#include "motion.h"
#include "nvs_flash.h"
#include "prov.h"
#include "tailscale.h"
#include "hublog.h"
#include "hubsys.h"
#include "web.h"
#include "wifi5g.h"
#include "zb.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hub";

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software (esp_restart)";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    case ESP_RST_EFUSE:     return "eFuse error";
    case ESP_RST_PWR_GLITCH:return "power glitch";
    case ESP_RST_CPU_LOCKUP:return "CPU lockup";
    default:                return "unknown";
    }
}

static void report_firmware(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "firmware %s %s, IDF %s, built %s %s",
             app->project_name, app->version, app->idf_ver, app->date, app->time);
    ESP_LOGI(TAG, "running from %s @ 0x%06" PRIx32, running->label, running->address);
    ESP_LOGI(TAG, "reset reason: %s", reset_reason_str(esp_reset_reason()));
}

static void report_hardware(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip: ESP32-C5 rev v%d.%d, %d core(s)",
             chip.revision / 100, chip.revision % 100, chip.cores);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "flash: %" PRIu32 " MB", flash_size / (1024 * 1024));
    } else {
        ESP_LOGE(TAG, "flash: size query failed");
    }

    if (esp_psram_is_initialized()) {
        ESP_LOGI(TAG, "psram: %u KB initialised", (unsigned)(esp_psram_get_size() / 1024));
    } else {
        ESP_LOGE(TAG, "psram: NOT initialised");
    }
}

static void report_partitions(void)
{
    ESP_LOGI(TAG, "partitions:");
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                     ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        ESP_LOGI(TAG, "  %-11s type %d/0x%02x  0x%06" PRIx32 "  %5" PRIu32 " KB",
                 p->label, p->type, p->subtype, p->address, p->size / 1024);
    }
    esp_partition_iterator_release(it);
}

static void report_heap(void)
{
    ESP_LOGI(TAG, "heap internal: %u KB free (min %u KB, largest block %u KB)",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    ESP_LOGI(TAG, "heap psram:    %u KB free (min %u KB)",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void report_wifi(void)
{
    wifi5g_stats_t w;
    wifi5g_get_stats(&w);
    if (w.connected) {
        ESP_LOGI(TAG, "wifi: up on %s, ch %u, rssi %d, first ip at %lld ms, connects %" PRIu32
                 ", disconnects %" PRIu32 " (no_ap_found %" PRIu32 "), failovers %" PRIu32
                 ", driver restarts %" PRIu32,
                 w.net == 0 ? "primary" : "backup", w.channel, w.rssi, w.first_ip_ms, w.connects,
                 w.disconnects, w.no_ap_found, w.failovers, w.driver_restarts);
    } else {
        ESP_LOGW(TAG, "wifi: down, last reason %u, disconnects %" PRIu32 ", driver restarts %" PRIu32,
                 w.last_reason, w.disconnects, w.driver_restarts);
    }
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Only the app-settings partition; prov and zb_storage are separate.
        ESP_LOGW(TAG, "nvs: %s, erasing app settings", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void start_mdns(const char *hostname)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns: init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(hostname);
    mdns_instance_name_set("Zigbee Hub");
    mdns_service_add("Zigbee Hub", "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mdns: %s.local", hostname);
}

// Tailnet-only access closed or re-opened the LAN side (web/access.c).
static char s_mdns_host[33];
static void on_lan_access(bool lan_open)
{
    if (lan_open) {
        start_mdns(s_mdns_host);
    } else {
        mdns_free();
        ESP_LOGI(TAG, "mdns: stopped (tailnet-only access)");
    }
}

// A freshly updated image runs on trial: the bootloader rolls it back if it
// reboots (crash) or isn't healthy within the deadline. Health = the Zigbee network
// is up and the web server runs. Wi-Fi is deliberately not required: an update
// installed while the router is down must not roll back (Zigbee comes first).
#define OTA_HEALTH_DEADLINE_S 120

static void ota_health_task(void *arg)
{
    for (int s = 0; s < OTA_HEALTH_DEADLINE_S; s++) {
        if (zb_is_up() && web_is_running()) {
            vTaskDelay(pdMS_TO_TICKS(5000));  // and still alive a moment later
            if (zb_is_up()) {
                hubsys_ota_confirm();
                vTaskDelete(NULL);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "new firmware not healthy after %d s: rebooting to roll back", OTA_HEALTH_DEADLINE_S);
    esp_restart();  // still pending-verify: the bootloader picks the previous slot
}

// One line every 5 min into the on-board log (tag "sample"): enough to see signal,
// memory and Wi-Fi recovery over a night without flooding the flash.
static void log_sample(void)
{
    wifi5g_stats_t w;
    wifi5g_get_stats(&w);
    ESP_LOGI("sample", "up=%llds wifi=%s ch=%u rssi=%d disc=%" PRIu32 " nap=%" PRIu32
             " fo=%" PRIu32 " drv=%" PRIu32 " heap=%uK min=%uK psram=%uK",
             esp_timer_get_time() / 1000000, w.connected ? "up" : "DOWN", w.channel, w.rssi,
             w.disconnects, w.no_ap_found, w.failovers, w.driver_restarts,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void log_boot_summary(void)
{
    hubsys_ota_result_t r;
    hubsys_ota_last_result(&r);
    ESP_LOGI("boot", "build %s, reset: %s%s%s, slot %s%s, last update: %s",
             hubsys_build_id(), reset_reason_str(esp_reset_reason()),
             hubsys_last_planned_reason()[0] ? " / " : "", hubsys_last_planned_reason(),
             esp_ota_get_running_partition()->label, hubsys_ota_pending_verify() ? " (on trial)" : "",
             r.kind);
}

// Wi-Fi watchdog's last resort (a reboot after a long outage) waits for a safe moment:
// nothing pairing, no device mid-interview, no firmware upload being written, no script
// running, no script timer or cron job due within 2 min, no buzzer pattern playing.
// Asked every second while the reboot is due; the registry, script state and named
// script timers are saved before saying yes.
static bool reboot_gate(void)
{
    static const char *last;
    const char *why = zb_permit_left() > 0 ? "pairing window open"
                    : zb_interviewing()    ? "a device is being interviewed"
                    : hubsys_ota_in_progress() ? "firmware upload in progress"
                    : automation_reboot_blocker();
    if (why) {
        if (why != last) ESP_LOGW(TAG, "watchdog reboot deferred: %s", why);
        last = why;
        return false;
    }
    zb_save_now();
    return true;
}

void app_main(void)
{
    hublog_init();  // first: capture everything from here on into the on-board log

    report_firmware();
    report_hardware();
    report_partitions();
    report_heap();

    init_nvs();
    hubsys_ota_boot_check();
    hubsys_crash_report();
    log_boot_summary();

    // Zigbee first, before Wi-Fi: it must never wait for the uplink.
    if (zb_start() != ESP_OK) ESP_LOGE(TAG, "zigbee failed to start");
    // Scripts next (the device registry is loaded): they don't wait for Wi-Fi either.
    if (automation_start() != ESP_OK) ESP_LOGE(TAG, "automation engine failed to start");
    // Wi-Fi motion sensing ("moving" / "not moving"); senses once Wi-Fi is up.
    if (motion_init() != ESP_OK) ESP_LOGE(TAG, "motion sensing failed to start");

    static prov_t prov;  // static: holds secrets, keep it off the small main stack
    bool have_prov = prov_load(&prov) == ESP_OK;
    if (have_prov && prov.has_wifi) {
        static wifi5g_net_t nets[WIFI5G_MAX_NETS];
        strlcpy(nets[0].ssid, prov.ssid, sizeof(nets[0].ssid));
        strlcpy(nets[0].psk, prov.psk, sizeof(nets[0].psk));
        strlcpy(nets[1].ssid, prov.ssid2, sizeof(nets[1].ssid));
        strlcpy(nets[1].psk, prov.psk2, sizeof(nets[1].psk));
        ESP_ERROR_CHECK(wifi5g_start(nets, prov.hostname, prov.country));
        wifi5g_set_reboot_gate(reboot_gate);
        memset(nets, 0, sizeof(nets));
        hublog_start_time_sync();

        strlcpy(s_mdns_host, prov.hostname, sizeof(s_mdns_host));
        web_config_t wc = {.hostname = prov.hostname, .admin_hash = prov.admin_hash,
                           .lan_reset = prov.lan_reset, .lan_cb = on_lan_access};
        if (web_start(&wc) != ESP_OK) ESP_LOGE(TAG, "web UI failed to start");
        // In tailnet-only mode nothing is announced on the LAN.
        if (web_lan_open()) start_mdns(s_mdns_host);
        else ESP_LOGI(TAG, "mdns: off (tailnet-only access)");

        // Tailscale last: it waits for an IP on its own task and nothing depends on it.
        // Its settings (auth key, on/off) come from the web UI, not from prov.
        if (ts_init(prov.hostname) != ESP_OK) ESP_LOGE(TAG, "tailscale failed to start");
    } else {
        ESP_LOGW(TAG, "no Wi-Fi credentials; running offline (provision with ./flash.sh)");
    }

    if (hubsys_ota_pending_verify()) {
        xTaskCreate(ota_health_task, "ota_health", 3072, NULL, 3, NULL);
    }

    for (int tick = 1;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        report_heap();
        report_wifi();
        if (tick % 10 == 0) log_sample();  // every 5 min
    }
}
