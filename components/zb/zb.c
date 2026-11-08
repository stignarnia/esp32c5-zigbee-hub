#include "zb.h"
#include "zb_internal.h"

#include <inttypes.h>
#include <string.h>

#include "esp_coexist.h"
#include "esp_coex_i154.h"
#include "nvs_flash.h"
#include "esp_ieee802154.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "zb";

// 2.4 GHz neighbours here sit on Wi-Fi channels 6, 8, 9, 11: Zigbee 15 and 25
// are clear of them. The energy scan at formation picks the quieter one.
#define ZB_CHANNEL_MASK    ((1u << 15) | (1u << 25))
#define ZB_STORAGE_PART    "zb_storage"
#define ZB_MAX_CHILDREN    32

static volatile bool s_up;
static volatile int64_t s_permit_until_us;
static esp_ieee802154_coex_config_t s_coex_default;
static int64_t s_start_us;

// Coex priority on this board:
//  - txrx HIGH permanently: a coordinator is always receiving, so Wi-Fi never got
//    the RF to scan (NO_AP_FOUND after 15 s, every attempt).
//  - driver defaults (txrx LOW, txrx_at MIDDLE) from the start: network formation
//    never completed (energy/active scans starved), even with Wi-Fi in modem sleep.
// So: HIGH while forming/restoring, defaults once the network is up. Formation
// happens once per device life; a restore takes ~1 s and is done before Wi-Fi starts.
static void net_refresh(void);

static void coex_settle(const char *what)
{
    esp_ieee802154_set_coex_config(s_coex_default);
    // set_coex_config() only stores the values: the driver applies a PTI when an
    // operation starts (enable_rx, transmit, ED). A coordinator sits in one long
    // receive, so HIGH would stay in force until its first transmit 10-14 s later, and
    // Wi-Fi can't transmit at all meanwhile. Apply it now; the radio is receiving.
    esp_coex_ieee802154_txrx_pti_set(s_coex_default.txrx);
    ezb_extaddr_t me;
    ezb_get_extended_address(&me);
    zb_dev_set_my_ieee(me.u64);
    net_refresh();
    s_up = true;
    ESP_LOGI(TAG, "%s in %lld ms; coex back to defaults (txrx %d, txrx_at %d)", what,
             (esp_timer_get_time() - s_start_us) / 1000, s_coex_default.txrx, s_coex_default.txrx_at);
}

static void log_network(const char *what)
{
    ezb_extpanid_t ext;
    ezb_nwk_get_extended_panid(&ext);
    ESP_LOGI(TAG, "%s: PAN 0x%04x, ext PAN 0x%016llx, channel %d, short 0x%04x", what,
             ezb_nwk_get_panid(), (unsigned long long)ext.u64, ezb_nwk_get_current_channel(),
             ezb_nwk_get_short_address());
}

static void retry_commissioning(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    ezb_bdb_start_top_level_commissioning((ezb_bdb_comm_mode_mask_t)(uintptr_t)arg);
}

static bool on_signal(const ezb_app_signal_t *sig)
{
    ezb_app_signal_type_t type = ezb_app_signal_get_type(sig);
    const void *params = ezb_app_signal_get_params(sig);

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t st = *(const ezb_bdb_comm_status_t *)params;
        if (st != EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "%s failed (0x%02x), retrying", ezb_app_signal_to_string(type), st);
            esp_zigbee_task_queue_post(retry_commissioning, (void *)(uintptr_t)EZB_BDB_MODE_INITIALIZATION);
        } else if (ezb_bdb_is_factory_new()) {
            ESP_LOGI(TAG, "factory new: forming a network");
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
        } else {
            log_network("network restored");
            coex_settle("restored");
        }
        break;
    }
    case EZB_BDB_SIGNAL_FORMATION: {
        ezb_bdb_comm_status_t st = *(const ezb_bdb_comm_status_t *)params;
        if (st == EZB_BDB_STATUS_SUCCESS) {
            log_network("network formed");
            coex_settle("formed");
        } else {
            ESP_LOGW(TAG, "formation failed (0x%02x), retrying", st);
            esp_zigbee_task_queue_post(retry_commissioning, (void *)(uintptr_t)EZB_BDB_MODE_NETWORK_FORMATION);
        }
        break;
    }
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        uint8_t secs = *(const uint8_t *)params;
        s_permit_until_us = secs ? esp_timer_get_time() + (int64_t)secs * 1000000 : 0;
        ESP_LOGI(TAG, "permit join: %s (%u s)", secs ? "open" : "closed", secs);
        zb_dev_notify();
        break;
    }
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const ezb_zdo_signal_device_annce_params_t *p = params;
        zb_dev_on_announce(p->short_addr, p->device_addr.u64, p->capability);
        break;
    }
    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        const ezb_zdo_signal_leave_indication_params_t *p = params;
        if (p->leave_type == EZB_ZDO_LEAVE_TYPE_RESET) zb_dev_on_leave(p->device_addr.u64);
        break;
    }
    case EZB_ZDO_SIGNAL_DEVICE_UNAVAILABLE: {
        const ezb_zdo_signal_device_unavailable_params_t *p = params;
        ESP_LOGW(TAG, "0x%04x: stack reports the device unavailable (delivery failed)", p->short_addr);
        break;
    }
    case EZB_ZDO_SIGNAL_DEVICE_UPDATE:
    case EZB_ZDO_SIGNAL_DEVICE_AUTHORIZED:
        break;  // join bookkeeping; the announce that follows is what matters
    default:
        ESP_LOGI(TAG, "signal %s (0x%02x)", ezb_app_signal_to_string(type), (unsigned)type);
        break;
    }
    return true;
}

static esp_err_t register_hub_endpoint(void)
{
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();
    ezb_zha_custom_gateway_config_t cfg = EZB_ZHA_CUSTOM_GATEWAY_CONFIG();
    ezb_af_ep_desc_t ep = ezb_zha_create_custom_gateway(ZB_HUB_EP, &cfg);
    ezb_zcl_cluster_desc_t basic = ezb_af_endpoint_get_cluster_desc(ep, EZB_ZCL_CLUSTER_ID_BASIC,
                                                                    EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)"\x0a" "zigbee-hub");
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)"\x07" "esp32c5");
    // Client-side clusters: what the hub reads, configures and receives reports or
    // IAS notifications from.
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_power_config_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_on_off_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_level_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_color_control_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_temperature_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_rel_humidity_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_pressure_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_illuminance_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_occupancy_sensing_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_ias_zone_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_electrical_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    ezb_af_endpoint_add_cluster_desc(ep, ezb_zcl_metering_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
    esp_err_t err = esp_zigbee_err_to_esp(ezb_af_device_add_endpoint_desc(dev, ep));
    if (err == ESP_OK) ezb_zcl_core_action_handler_register(zb_dev_on_zcl);
    if (err == ESP_OK) err = esp_zigbee_err_to_esp(ezb_af_device_desc_register(dev));
    return err;
}

// zb_storage is raw flash until the first boot after a USB flash: NVS needs it
// formatted. Erasing is only done when NVS reports it unusable (blank/garbage or an
// incompatible layout) — at 512 KB it never fills in normal use.
static esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init_partition(ZB_STORAGE_PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "%s: %s, formatting (a new network will be formed)", ZB_STORAGE_PART,
                 esp_err_to_name(err));
        err = nvs_flash_erase_partition(ZB_STORAGE_PART);
        if (err == ESP_OK) err = nvs_flash_init_partition(ZB_STORAGE_PART);
    }
    return err;
}

#define ZB_CHECK(x)                                                              \
    do {                                                                         \
        esp_err_t err_ = (x);                                                    \
        if (err_ != ESP_OK) {                                                    \
            ESP_LOGE(TAG, "%s: %s (Zigbee stays off)", #x, esp_err_to_name(err_)); \
            goto fail;                                                           \
        }                                                                        \
    } while (0)

static void zb_task(void *arg)
{
    ZB_CHECK(storage_init());
    esp_zigbee_config_t cfg = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR,
            .install_code_policy = false,
            .zczr_config = {.max_children = ZB_MAX_CHILDREN},
        },
        .platform_config = {
            .storage_partition_name = ZB_STORAGE_PART,
            .radio_config = {.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE},
        },
    };
    ZB_CHECK(esp_zigbee_init(&cfg));

    // Coex: HIGH until the network is up, driver defaults after (see coex_settle).
    s_coex_default = esp_ieee802154_get_coex_config();
    esp_ieee802154_coex_config_t boost = s_coex_default;
    boost.txrx = IEEE802154_HIGH;
    boost.txrx_at = IEEE802154_HIGH;
    esp_ieee802154_set_coex_config(boost);
    s_start_us = esp_timer_get_time();

    ezb_aps_secur_enable_distributed_security(false);  // centralised: we are the trust centre
    ZB_CHECK(esp_zigbee_err_to_esp(ezb_bdb_set_primary_channel_set(ZB_CHANNEL_MASK)));
    ZB_CHECK(esp_zigbee_err_to_esp(ezb_bdb_set_secondary_channel_set(ZB_CHANNEL_MASK)));
    ZB_CHECK(esp_zigbee_err_to_esp(ezb_app_signal_add_handler(on_signal)));
    ZB_CHECK(register_hub_endpoint());
    ZB_CHECK(esp_zigbee_start(false));
    esp_zigbee_launch_mainloop();  // never returns while the stack runs
    ESP_LOGE(TAG, "stack main loop exited");
fail:
    vTaskDelete(NULL);
}

// Network info for the UI, refreshed on the Zigbee task (no lock needed there) and
// copied by the HTTP task under a short critical section. Taking the Zigbee lock on
// the HTTP task (up to 200 ms per snapshot) would block the web server behind the
// stack whenever it is busy, e.g. during an interview.
typedef struct {
    uint64_t ieee;
    uint16_t short_addr;
    uint8_t type, relationship, depth, lqi;
    int8_t rssi;
    bool rx_on;
} nbr_t;
static struct {
    bool valid;
    uint16_t pan;
    uint64_t ext_pan;
    uint8_t channel;
    int n;
    nbr_t nbr[ZB_MAX_DEVICES];
} s_net;
static portMUX_TYPE s_net_lock = portMUX_INITIALIZER_UNLOCKED;

static void net_refresh(void)  // Zigbee task only
{
    static nbr_t tmp[ZB_MAX_DEVICES];
    int n = 0;
    ezb_nwk_info_iterator_t it = EZB_NWK_INFO_ITERATOR_INIT;
    ezb_nwk_neighbor_info_t nb;
    while (n < ZB_MAX_DEVICES && ezb_nwk_get_next_neighbor(&it, &nb) == EZB_ERR_NONE) {
        tmp[n++] = (nbr_t){nb.ieee_addr.u64, nb.short_addr, nb.device_type, nb.relationship,
                           nb.depth, nb.lqi, nb.rssi, nb.rx_on_when_idle};
    }
    ezb_extpanid_t ext;
    ezb_nwk_get_extended_panid(&ext);
    uint16_t pan = ezb_nwk_get_panid();
    uint8_t ch = ezb_nwk_get_current_channel();
    taskENTER_CRITICAL(&s_net_lock);
    s_net.pan = pan;
    s_net.ext_pan = ext.u64;
    s_net.channel = ch;
    s_net.n = n;
    memcpy(s_net.nbr, tmp, n * sizeof(nbr_t));
    s_net.valid = true;
    taskEXIT_CRITICAL(&s_net_lock);
}

static void tick_on_zb_task(void *arg)
{
    net_refresh();
    zb_dev_tick(arg);
}

static void tick_cb(TimerHandle_t t)
{
    if (s_up) esp_zigbee_task_queue_post(tick_on_zb_task, NULL);
}

esp_err_t zb_start(void)
{
    esp_err_t e = zb_dev_init();
    if (e != ESP_OK) return e;
    TimerHandle_t t = xTimerCreate("zb_tick", pdMS_TO_TICKS(1000), pdTRUE, NULL, tick_cb);
    if (!t || xTimerStart(t, 0) != pdPASS) return ESP_ERR_NO_MEM;

    // Before the first Wi-Fi association: without it the station barely associates.
    esp_err_t err = esp_coex_wifi_i154_enable();
    if (err != ESP_OK) ESP_LOGW(TAG, "coex enable: %s", esp_err_to_name(err));
    if (xTaskCreate(zb_task, "zigbee", 8192, NULL, 6, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool zb_is_up(void)
{
    return s_up;
}

// ---- API for other tasks ---------------------------------------------------------------

esp_err_t zb_permit_join(uint8_t seconds)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    ezb_err_t e = seconds ? ezb_bdb_open_network(seconds) : ezb_bdb_close_network();
    esp_zigbee_lock_release();
    if (e == EZB_ERR_NONE) {
        s_permit_until_us = seconds ? esp_timer_get_time() + (int64_t)seconds * 1000000 : 0;
        zb_dev_notify();
    }
    ESP_LOGI(TAG, "permit join %u s requested: 0x%x", seconds, e);
    return esp_zigbee_err_to_esp(e);
}

uint32_t zb_permit_left(void)
{
    int64_t left = s_permit_until_us - esp_timer_get_time();
    return left > 0 ? (uint32_t)((left + 999999) / 1000000) : 0;
}

cJSON *zb_network_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "up", s_up);
    cJSON_AddNumberToObject(o, "permit_s", zb_permit_left());
    static typeof(s_net) snap;  // HTTP task only (handlers and WebSocket pushes)
    taskENTER_CRITICAL(&s_net_lock);
    snap = s_net;
    taskEXIT_CRITICAL(&s_net_lock);
    if (s_up && snap.valid) {
        char buf[20];
        snprintf(buf, sizeof(buf), "0x%04x", snap.pan);
        cJSON_AddStringToObject(o, "pan", buf);
        snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)snap.ext_pan);
        cJSON_AddStringToObject(o, "ext_pan", buf);
        cJSON_AddNumberToObject(o, "channel", snap.channel);
        // Network map: the coordinator's neighbour table (every directly joined device).
        cJSON *nbrs = cJSON_AddArrayToObject(o, "neighbors");
        for (int i = 0; i < snap.n; i++) {
            const nbr_t *n = &snap.nbr[i];
            cJSON *j = cJSON_CreateObject();
            snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)n->ieee);
            cJSON_AddStringToObject(j, "ieee", buf);
            cJSON_AddNumberToObject(j, "short", n->short_addr);
            cJSON_AddNumberToObject(j, "type", n->type);  // 0 coordinator, 1 router, 2 end device
            cJSON_AddNumberToObject(j, "relationship", n->relationship);
            cJSON_AddNumberToObject(j, "depth", n->depth);
            cJSON_AddNumberToObject(j, "lqi", n->lqi);
            cJSON_AddNumberToObject(j, "rssi", n->rssi);
            cJSON_AddBoolToObject(j, "rx_on", n->rx_on);
            cJSON_AddItemToArray(nbrs, j);
        }
    }
    return o;
}

static void remove_on_zb_task(void *arg)
{
    uint64_t *p = arg;
    uint64_t ieee = p[0];
    uint16_t short_addr = (uint16_t)p[1];
    free(p);
    ezb_zdo_nwk_mgmt_leave_req_t req = {
        .dst_nwk_addr = short_addr,
        .field = {.rejoin = false},
    };
    req.field.device_addr.u64 = ieee;
    ezb_err_t e = ezb_zdo_nwk_mgmt_leave_req(&req);
    ESP_LOGI(TAG, "0x%04x: leave request sent (0x%x); forgotten", short_addr, e);
    zb_dev_forget(ieee);  // sleepy devices may not hear it: forget them anyway
}

esp_err_t zb_remove(const char *ieee_hex)
{
    uint16_t short_addr;
    uint64_t ieee;
    if (!zb_dev_lookup_short(ieee_hex, &short_addr, &ieee)) return ESP_ERR_NOT_FOUND;
    uint64_t *p = malloc(2 * sizeof(uint64_t));
    if (!p) return ESP_ERR_NO_MEM;
    p[0] = ieee;
    p[1] = short_addr;
    return esp_zigbee_task_queue_post(remove_on_zb_task, p);
}

static void reinterview_on_zb_task(void *arg)
{
    uint64_t ieee = *(uint64_t *)arg;
    free(arg);
    zb_dev_reinterview(ieee);
}

esp_err_t zb_reinterview(const char *ieee_hex)
{
    uint16_t short_addr;
    uint64_t ieee;
    if (!zb_dev_lookup_short(ieee_hex, &short_addr, &ieee)) return ESP_ERR_NOT_FOUND;
    uint64_t *p = malloc(sizeof(uint64_t));  // a pointer is 32 bits here: the IEEE doesn't fit
    if (!p) return ESP_ERR_NO_MEM;
    *p = ieee;
    esp_err_t err = esp_zigbee_task_queue_post(reinterview_on_zb_task, p);
    if (err != ESP_OK) free(p);
    return err;
}
