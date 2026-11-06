// Device registry + interview.
//
// Threading: the table is written on the Zigbee task (signals, ZCL/ZDO callbacks,
// the 1 s tick) and read by the HTTP task for the UI, under s_mtx. File I/O never
// happens on the Zigbee task or under the Zigbee lock: a saver task writes the
// registry when it is dirty (CLAUDE.md lesson 11).
//
// Interview (sleepy end devices give us seconds after joining, not minutes):
//   on announce, at once and in parallel: Basic read (manufacturer, model, power
//   source, ZCL/app version, 0xFFFE — the Tuya "magic packet") + Active EP request
//   -> Simple Descriptor per endpoint
//   -> IAS Zone: write our IEEE to IAS_CIE_Address, unsolicited Enroll Response,
//      read zone type/status (answer Enroll Requests whenever they come)
//   -> bind + configure reporting for the clusters we understand
// Each step retries every 2 s while the device is likely awake; if it falls
// asleep, the missing steps resume the next time anything is heard from it.

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "zb.h"
#include "zb_internal.h"

static const char *TAG = "zb";

#define REG_DIR   "/storage/zb"
#define REG_PATH  REG_DIR "/devices.json"
#define REG_TMP   REG_DIR "/devices.tmp"

#define RETRY_US        (2000LL * 1000)

// Availability. Sleepy (battery) devices can't be asked: they're offline after 25 h
// of silence (zigbee2mqtt's default; our battery reporting is configured for at least
// one report every 6 h). Mains devices are pinged (Basic read) after 10 min of
// silence, 3 pings 30 s apart; no answer = offline.
#define SLEEPY_TIMEOUT_US  (25LL * 3600 * 1000000)
#define MAINS_IDLE_US      (10LL * 60 * 1000000)
#define PING_EVERY_US      (30LL * 1000000)
#define PINGS_MAX          3
#define MAX_TRIES       6           // ~12 s of fast retries, then wait for a check-in

enum {
    IV_BASIC   = 1 << 0,
    IV_EPS     = 1 << 1,
    IV_DESCS   = 1 << 2,
    IV_IAS     = 1 << 3,
    IV_REPORT  = 1 << 4,
    IV_ALL     = 0x1F,
};

typedef struct {
    uint8_t id;
    uint16_t profile, device;
    uint8_t n_in, n_out;
    uint16_t in[ZB_MAX_CLUSTERS], out[ZB_MAX_CLUSTERS];
} ep_t;

typedef struct {
    bool used;
    uint64_t ieee;
    uint16_t short_addr;
    uint8_t cap;                 // MAC capability (bit 2 mains, bit 3 rx-on-when-idle)
    char name[ZB_NAME_LEN];
    char manuf[33], model[33];
    uint8_t power_src;           // Basic 0x0007, 0xFF = unknown
    uint8_t n_ep;
    ep_t ep[ZB_MAX_EPS];
    uint8_t done;                // IV_* steps completed
    uint8_t desc_ok;             // per-ep bitmask of simple descriptors received
    uint8_t tries;
    int64_t next_try_us;         // 0 = not waiting
    int64_t joined_us;
    int64_t woke_us;             // when the current interview attempt started
    // live state (not persisted)
    int64_t last_seen_us;        // 0 = not heard since boot (availability counts from boot)
    bool offline;
    uint8_t pings;               // unanswered availability pings (mains devices)
    int64_t ping_at_us;
    int8_t rssi;
    uint16_t zone_type, zone_status;
    bool has_zone_status;
    int16_t temp;     bool has_temp;     // 0.01 °C
    uint16_t hum;     bool has_hum;      // 0.01 %
    uint8_t batt;     bool has_batt;     // 0.5 %
    uint8_t batt_v;   bool has_batt_v;   // 100 mV
    bool on;          bool has_on;
    uint8_t occ;      bool has_occ;
} zdev_t;

static zdev_t *s_dev;                    // PSRAM, ZB_MAX_DEVICES
static SemaphoreHandle_t s_mtx;
static volatile bool s_dirty;
static uint32_t s_rev;                  // bumped on every change the UI should see
static uint64_t s_my_ieee;

#define LOCK()   xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

// ---- helpers ------------------------------------------------------------------------

static zdev_t *by_short(uint16_t s)
{
    for (int i = 0; i < ZB_MAX_DEVICES; i++) {
        if (s_dev[i].used && s_dev[i].short_addr == s) return &s_dev[i];
    }
    return NULL;
}

static zdev_t *by_ieee(uint64_t ieee)
{
    for (int i = 0; i < ZB_MAX_DEVICES; i++) {
        if (s_dev[i].used && s_dev[i].ieee == ieee) return &s_dev[i];
    }
    return NULL;
}

static int dev_index(const zdev_t *d)
{
    return (int)(d - s_dev);
}

// Callback context for ZDO requests: the table slot plus the device's short
// address, packed in 32 bits (pointers are 32-bit on this chip — an IEEE address
// doesn't fit; truncating it made every endpoint response unmatchable).
static void *slot_ctx(const zdev_t *d)
{
    return (void *)(uintptr_t)(((uint32_t)dev_index(d) << 16) | d->short_addr);
}

static zdev_t *from_ctx(void *ctx)
{
    uint32_t v = (uint32_t)(uintptr_t)ctx;
    int idx = v >> 16;
    if (idx >= ZB_MAX_DEVICES || !s_dev[idx].used || s_dev[idx].short_addr != (v & 0xFFFF)) return NULL;
    return &s_dev[idx];
}

static bool has_server(const zdev_t *d, uint16_t cluster, uint8_t *ep_out)
{
    for (int e = 0; e < d->n_ep; e++) {
        for (int c = 0; c < d->ep[e].n_in; c++) {
            if (d->ep[e].in[c] == cluster) {
                if (ep_out) *ep_out = d->ep[e].id;
                return true;
            }
        }
    }
    return false;
}

static uint8_t first_ep(const zdev_t *d)
{
    return d->n_ep ? d->ep[0].id : 1;
}

static void changed(bool persist)
{
    s_rev++;
    if (persist) s_dirty = true;
}

void zb_dev_notify(void)
{
    s_rev++;
}

static ezb_zcl_cmd_ctrl_t ctrl_to(const zdev_t *d, uint8_t ep, uint16_t cluster)
{
    ezb_zcl_cmd_ctrl_t c = {0};
    c.dst_addr.addr_mode = EZB_ADDR_MODE_SHORT;
    c.dst_addr.u.short_addr = d->short_addr;
    c.dst_ep = ep;
    c.src_ep = ZB_HUB_EP;
    c.cluster_id = cluster;
    return c;
}

static const char *zone_kind(uint16_t t)
{
    switch (t) {
    case 0x000D: return "motion";
    case 0x0015: return "contact";
    case 0x0028: return "fire";
    case 0x002A: return "water";
    case 0x002B: return "co";
    case 0x002D: return "vibration";
    case 0x010F: return "remote";
    case 0x0115: return "keyfob";
    case 0x021D: return "keypad";
    case 0x0225: return "warning";
    default:     return "zone";
    }
}

// Finished = every step done, and an IAS device's zone type known (it says what the
// device is: contact, motion, water...).
static bool complete(const zdev_t *d)
{
    return d->done == IV_ALL && (d->zone_type || !has_server(d, EZB_ZCL_CLUSTER_ID_IAS_ZONE, NULL));
}

// ---- interview: requests --------------------------------------------------------------

static void basic_read(zdev_t *d)
{
    // One request, all at once: Tuya devices want this right after joining or they
    // stay silent. Unsupported attributes just come back with an error status.
    static uint16_t attrs[] = {0x0004, 0x0000, 0x0001, 0x0005, 0x0007, 0xFFFE};
    ezb_zcl_read_attr_cmd_t req = {
        .cmd_ctrl = ctrl_to(d, first_ep(d), EZB_ZCL_CLUSTER_ID_BASIC),
        .payload = {.attr_number = sizeof(attrs) / sizeof(attrs[0]), .attr_field = attrs},
    };
    ezb_err_t e = ezb_zcl_read_attr_cmd_req(&req);
    if (e != EZB_ERR_NONE) ESP_LOGW(TAG, "0x%04x: basic read: 0x%x", d->short_addr, e);
}

static void active_ep_cb(const ezb_zdo_active_ep_req_result_t *r, void *ctx);
static void simple_desc_cb(const ezb_zdo_simple_desc_req_result_t *r, void *ctx);

static void active_ep_req(zdev_t *d)
{
    ezb_zdo_active_ep_req_t req = {
        .dst_nwk_addr = d->short_addr,
        .field = {.nwk_addr_of_interest = d->short_addr},
        .cb = active_ep_cb,
        .user_ctx = slot_ctx(d),
    };
    ezb_zdo_active_ep_req(&req);
}

static void simple_desc_req(zdev_t *d, uint8_t ep)
{
    ezb_zdo_simple_desc_req_t req = {
        .dst_nwk_addr = d->short_addr,
        .field = {.nwk_addr_of_interest = d->short_addr, .endpoint = ep},
        .cb = simple_desc_cb,
        .user_ctx = slot_ctx(d),
    };
    ezb_zdo_simple_desc_req(&req);
}

static void ias_enroll_rsp(zdev_t *d, uint8_t ep)
{
    ezb_zcl_ias_zone_enroll_rsp_cmd_t rsp = {
        .cmd_ctrl = {
            .dst_addr = {.addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = d->short_addr},
            .dst_ep = ep,
            .src_ep = ZB_HUB_EP,
        },
        .payload = {.enroll_rsp_code = 0x00 /* success */, .zone_id = (uint8_t)dev_index(d)},
    };
    ezb_zcl_ias_zone_enroll_cmd_resp(&rsp);
}

static void ias_setup(zdev_t *d, uint8_t ep)
{
    // 1. Tell the zone where to send its alarms (us). Without this a contact sensor
    //    joins fine and then never reports.
    static ezb_extaddr_t cie;
    cie.u64 = s_my_ieee;
    ezb_zcl_attribute_t a = {
        .id = EZB_ZCL_ATTR_IAS_ZONE_IAS_CIE_ADDRESS_ID,
        .data = {.type = EZB_ZCL_ATTR_TYPE_EUI64, .size = 8, .value = &cie},
    };
    ezb_zcl_write_attr_cmd_t w = {
        .cmd_ctrl = ctrl_to(d, ep, EZB_ZCL_CLUSTER_ID_IAS_ZONE),
        .payload = {.attr_number = 1, .attr_field = &a},
    };
    ezb_zcl_write_attr_cmd_req(&w);
    // 2. Unsolicited enroll response: some zones never send an Enroll Request.
    ias_enroll_rsp(d, ep);
    // 3. Zone type (what it is) and status (current state).
    static uint16_t attrs[] = {EZB_ZCL_ATTR_IAS_ZONE_ZONE_TYPE_ID, EZB_ZCL_ATTR_IAS_ZONE_ZONE_STATUS_ID,
                               EZB_ZCL_ATTR_IAS_ZONE_ZONE_STATE_ID};
    ezb_zcl_read_attr_cmd_t r = {
        .cmd_ctrl = ctrl_to(d, ep, EZB_ZCL_CLUSTER_ID_IAS_ZONE),
        .payload = {.attr_number = 3, .attr_field = attrs},
    };
    ezb_zcl_read_attr_cmd_req(&r);
}

typedef struct {
    uint16_t cluster, attr;
    uint8_t type;
    uint16_t min_s, max_s;
    uint32_t change;
} report_spec_t;

static const report_spec_t REPORTS[] = {
    {EZB_ZCL_CLUSTER_ID_POWER_CONFIG, 0x0021, EZB_ZCL_ATTR_TYPE_UINT8, 3600, 21600, 2},     // battery %
    {EZB_ZCL_CLUSTER_ID_POWER_CONFIG, 0x0020, EZB_ZCL_ATTR_TYPE_UINT8, 3600, 21600, 1},     // battery V
    {EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT, 0x0000, EZB_ZCL_ATTR_TYPE_INT16, 10, 600, 20},     // 0.2 °C
    {EZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT, 0x0000, EZB_ZCL_ATTR_TYPE_UINT16, 10, 600, 100},
    {EZB_ZCL_CLUSTER_ID_ON_OFF, 0x0000, EZB_ZCL_ATTR_TYPE_BOOL, 0, 300, 0},
    {EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING, 0x0000, EZB_ZCL_ATTR_TYPE_MAP8, 0, 300, 0},
};

static void bind_and_report(zdev_t *d)
{
    uint16_t last_cluster = 0;
    for (size_t i = 0; i < sizeof(REPORTS) / sizeof(REPORTS[0]); i++) {
        const report_spec_t *s = &REPORTS[i];
        uint8_t ep;
        if (!has_server(d, s->cluster, &ep)) continue;
        if (s->cluster != last_cluster) {
            ezb_zdo_bind_req_t b = {
                .dst_nwk_addr = d->short_addr,
                .field = {
                    .src_ep = ep,
                    .cluster_id = s->cluster,
                    .dst_addr_mode = EZB_ADDR_MODE_EXT,
                    .dst_ep = ZB_HUB_EP,
                },
            };
            b.field.src_addr.u64 = d->ieee;
            b.field.dst_addr.extended_addr.u64 = s_my_ieee;
            ezb_zdo_bind_req(&b);
            last_cluster = s->cluster;
        }
        ezb_zcl_config_report_record_t rec = {
            .direction = EZB_ZCL_REPORTING_SEND,
            .attr_id = s->attr,
        };
        rec.client.attr_type = s->type;
        rec.client.min_interval = s->min_s;
        rec.client.max_interval = s->max_s;
        rec.client.reportable_change.u32 = s->change;
        ezb_zcl_config_report_cmd_t c = {
            .cmd_ctrl = ctrl_to(d, ep, s->cluster),
            .payload = {.record_number = 1, .record_field = &rec},
        };
        ezb_zcl_config_report_cmd_req(&c);
    }
    // An initial read, so the UI has values before the first report.
    uint8_t ep;
    if (has_server(d, EZB_ZCL_CLUSTER_ID_POWER_CONFIG, &ep)) {
        static uint16_t attrs[] = {0x0020, 0x0021};
        ezb_zcl_read_attr_cmd_t r = {.cmd_ctrl = ctrl_to(d, ep, EZB_ZCL_CLUSTER_ID_POWER_CONFIG),
                                     .payload = {.attr_number = 2, .attr_field = attrs}};
        ezb_zcl_read_attr_cmd_req(&r);
    }
}

// Sends whatever the interview still needs. Called on announce, on every retry tick,
// and when a device that fell asleep mid-interview is heard from again.
static void interview_step(zdev_t *d)
{
    int64_t now = esp_timer_get_time();
    if (complete(d)) {
        d->next_try_us = 0;
        return;
    }
    if (!(d->done & IV_BASIC)) basic_read(d);
    if (!(d->done & IV_EPS)) {
        active_ep_req(d);
    } else if (!(d->done & IV_DESCS)) {
        for (int e = 0; e < d->n_ep; e++) {
            if (!(d->desc_ok & (1 << e))) simple_desc_req(d, d->ep[e].id);
        }
    } else {
        uint8_t ep;
        bool ias = has_server(d, EZB_ZCL_CLUSTER_ID_IAS_ZONE, &ep);
        if (!ias) {
            d->done |= IV_IAS;
        } else if (!(d->done & IV_IAS) || !d->zone_type) {
            // Also when notifications already arrive (the device enrolled itself):
            // the zone type says what it is (contact, motion, water...).
            ias_setup(d, ep);
        }
        if (!(d->done & IV_REPORT)) {
            bind_and_report(d);
            d->done |= IV_REPORT;  // fire-and-forget: reports arriving prove it worked
        }
    }
    d->tries++;
    d->next_try_us = now + RETRY_US;
    changed(true);
}

static void interview_done_check(zdev_t *d)
{
    if (complete(d) && d->next_try_us) {  // logged once: on the transition
        d->next_try_us = 0;
        ESP_LOGI(TAG, "0x%04x %s %s: interview complete (%lld ms since it last woke up)", d->short_addr,
                 d->manuf, d->model, (esp_timer_get_time() - d->woke_us) / 1000);
    }
}

// Heard from: resume a stalled interview right away (the device is awake now).
static void heard(zdev_t *d, int8_t rssi)
{
    d->last_seen_us = esp_timer_get_time();
    if (rssi) d->rssi = rssi;
    d->pings = 0;
    if (d->offline) {
        d->offline = false;
        ESP_LOGI(TAG, "0x%04x %s: online again", d->short_addr, d->name[0] ? d->name : d->model);
        changed(false);
    }
    if (!complete(d) && d->next_try_us == 0) {
        ESP_LOGI(TAG, "0x%04x awake again: resuming interview", d->short_addr);
        d->woke_us = esp_timer_get_time();
        d->tries = 0;
        interview_step(d);
    }
}

// ---- interview: responses --------------------------------------------------------------

static void active_ep_cb(const ezb_zdo_active_ep_req_result_t *r, void *ctx)
{
    LOCK();
    zdev_t *d = from_ctx(ctx);
    if (d && (r->error != EZB_ERR_NONE || !r->rsp)) ESP_LOGW(TAG, "0x%04x: active EP request: error 0x%x", d->short_addr, r->error);
    if (d && r->error == EZB_ERR_NONE && r->rsp && r->rsp->status == EZB_ZDP_STATUS_SUCCESS && !(d->done & IV_EPS)) {
        d->n_ep = r->rsp->active_ep_count < ZB_MAX_EPS ? r->rsp->active_ep_count : ZB_MAX_EPS;
        for (int i = 0; i < d->n_ep; i++) d->ep[i].id = r->rsp->active_ep_list[i];
        d->done |= IV_EPS;
        d->desc_ok = 0;
        heard(d, 0);
        ESP_LOGI(TAG, "0x%04x: %u endpoint(s)", d->short_addr, d->n_ep);
        for (int e = 0; e < d->n_ep; e++) simple_desc_req(d, d->ep[e].id);
        changed(true);
    }
    UNLOCK();
}

static void simple_desc_cb(const ezb_zdo_simple_desc_req_result_t *r, void *ctx)
{
    LOCK();
    zdev_t *d = from_ctx(ctx);
    if (d && (r->error != EZB_ERR_NONE || !r->rsp)) ESP_LOGW(TAG, "0x%04x: simple desc request: error 0x%x", d->short_addr, r->error);
    if (d && r->error == EZB_ERR_NONE && r->rsp && r->rsp->status == EZB_ZDP_STATUS_SUCCESS) {
        const ezb_af_simple_desc_t *s = &r->rsp->desc;
        for (int e = 0; e < d->n_ep; e++) {
            if (d->ep[e].id != s->ep_id) continue;
            ep_t *ep = &d->ep[e];
            ep->profile = s->app_profile_id;
            ep->device = s->app_device_id;
            ep->n_in = s->app_input_cluster_count < ZB_MAX_CLUSTERS ? s->app_input_cluster_count : ZB_MAX_CLUSTERS;
            ep->n_out = s->app_output_cluster_count < ZB_MAX_CLUSTERS ? s->app_output_cluster_count : ZB_MAX_CLUSTERS;
            for (int c = 0; c < ep->n_in; c++) ep->in[c] = s->app_cluster_list[c];
            for (int c = 0; c < ep->n_out; c++) ep->out[c] = s->app_cluster_list[s->app_input_cluster_count + c];
            d->desc_ok |= 1 << e;
            ESP_LOGI(TAG, "0x%04x: ep %u profile 0x%04x device 0x%04x, %u in / %u out clusters", d->short_addr,
                     ep->id, ep->profile, ep->device, ep->n_in, ep->n_out);
        }
        heard(d, 0);
        if (d->desc_ok == (1 << d->n_ep) - 1 && !(d->done & IV_DESCS)) {
            d->done |= IV_DESCS;
            d->tries = 0;
            interview_step(d);  // straight on to IAS + reporting while it's awake
            interview_done_check(d);
        }
        changed(true);
    }
    UNLOCK();
}

static void zcl_string(char *out, size_t cap, const void *v)
{
    const uint8_t *p = v;
    size_t n = p[0] == 0xFF ? 0 : p[0];
    if (n >= cap) n = cap - 1;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = (char)p[1 + i];
        if (c >= 0x20 && c < 0x7F) out[o++] = c;  // printable ASCII only (UI + JSON safety)
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = '\0';
}

static void apply_attr(zdev_t *d, uint16_t cluster, uint16_t attr, uint8_t type, const void *v)
{
    if (!v) return;
    switch (cluster) {
    case EZB_ZCL_CLUSTER_ID_BASIC:
        if (attr == 0x0004 && type == EZB_ZCL_ATTR_TYPE_STRING) zcl_string(d->manuf, sizeof(d->manuf), v);
        else if (attr == 0x0005 && type == EZB_ZCL_ATTR_TYPE_STRING) zcl_string(d->model, sizeof(d->model), v);
        else if (attr == 0x0007) d->power_src = *(const uint8_t *)v;
        break;
    case EZB_ZCL_CLUSTER_ID_POWER_CONFIG:
        if (attr == 0x0021) { d->batt = *(const uint8_t *)v; d->has_batt = true; }
        else if (attr == 0x0020) { d->batt_v = *(const uint8_t *)v; d->has_batt_v = true; }
        break;
    case EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT:
        if (attr == 0x0000) { memcpy(&d->temp, v, 2); d->has_temp = d->temp != (int16_t)0x8000; }
        break;
    case EZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT:
        if (attr == 0x0000) { memcpy(&d->hum, v, 2); d->has_hum = d->hum != 0xFFFF; }
        break;
    case EZB_ZCL_CLUSTER_ID_ON_OFF:
        if (attr == 0x0000) { d->on = *(const uint8_t *)v; d->has_on = true; }
        break;
    case EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING:
        if (attr == 0x0000) { d->occ = *(const uint8_t *)v; d->has_occ = true; }
        break;
    case EZB_ZCL_CLUSTER_ID_IAS_ZONE:
        if (attr == EZB_ZCL_ATTR_IAS_ZONE_ZONE_TYPE_ID) memcpy(&d->zone_type, v, 2);
        else if (attr == EZB_ZCL_ATTR_IAS_ZONE_ZONE_STATUS_ID) {
            memcpy(&d->zone_status, v, 2);
            d->has_zone_status = true;
        } else if (attr == EZB_ZCL_ATTR_IAS_ZONE_ZONE_STATE_ID && *(const uint8_t *)v == 1) {
            d->done |= IV_IAS;  // enrolled
        }
        break;
    default:
        break;
    }
}

static zdev_t *from_header(const ezb_zcl_cmd_hdr_t *h)
{
    if (!h || h->src_addr.addr_mode != EZB_ADDR_MODE_SHORT) return NULL;
    return by_short(h->src_addr.u.short_addr);
}

void zb_dev_on_zcl(ezb_zcl_core_action_callback_id_t id, void *message)
{
    LOCK();
    switch (id) {
    case EZB_ZCL_CORE_READ_ATTR_RSP_CB_ID: {
        ezb_zcl_cmd_read_attr_rsp_message_t *m = message;
        zdev_t *d = from_header(m->in.header);
        if (!d) break;
        heard(d, m->in.header->rssi);
        uint16_t cl = m->info.cluster_id;
        for (ezb_zcl_read_attr_rsp_variable_t *v = m->in.variables; v; v = v->next) {
            if (v->status == EZB_ZCL_STATUS_SUCCESS) apply_attr(d, cl, v->attr_id, v->attr_type, v->attr_value);
        }
        if (cl == EZB_ZCL_CLUSTER_ID_BASIC && !(d->done & IV_BASIC)) {
            d->done |= IV_BASIC;
            ESP_LOGI(TAG, "0x%04x: '%s' '%s', power source %u", d->short_addr, d->manuf, d->model, d->power_src);
        }
        if (cl == EZB_ZCL_CLUSTER_ID_IAS_ZONE && d->zone_type) {
            ESP_LOGI(TAG, "0x%04x: IAS zone type 0x%04x (%s), status 0x%04x", d->short_addr, d->zone_type,
                     zone_kind(d->zone_type), d->zone_status);
        }
        interview_done_check(d);
        changed(true);
        m->out.result = EZB_ZCL_STATUS_SUCCESS;
        break;
    }
    case EZB_ZCL_CORE_REPORT_ATTR_CB_ID: {
        ezb_zcl_cmd_report_attr_message_t *m = message;
        zdev_t *d = from_header(m->in.header);
        if (!d) break;
        heard(d, m->in.header->rssi);
        for (ezb_zcl_report_attr_variable_t *v = m->in.variables; v; v = v->next) {
            apply_attr(d, m->info.cluster_id, v->attr_id, v->attr_type, v->attr_value);
        }
        changed(false);
        m->out.result = EZB_ZCL_STATUS_SUCCESS;
        break;
    }
    case EZB_ZCL_CORE_WRITE_ATTR_RSP_CB_ID: {
        ezb_zcl_cmd_write_attr_rsp_message_t *m = message;
        zdev_t *d = from_header(m->in.header);
        if (!d) break;
        heard(d, m->in.header->rssi);
        if (m->info.cluster_id == EZB_ZCL_CLUSTER_ID_IAS_ZONE) {
            bool ok = m->in.variables && m->in.variables->status == EZB_ZCL_STATUS_SUCCESS;
            ESP_LOGI(TAG, "0x%04x: IAS CIE address write %s", d->short_addr, ok ? "ok" : "failed");
        }
        m->out.result = EZB_ZCL_STATUS_SUCCESS;
        break;
    }
    case EZB_ZCL_CORE_CONFIG_REPORT_RSP_CB_ID: {
        ezb_zcl_cmd_config_report_rsp_message_t *m = message;
        zdev_t *d = from_header(m->in.header);
        if (!d) break;
        heard(d, m->in.header->rssi);
        bool ok = m->in.variables && m->in.variables->status == EZB_ZCL_STATUS_SUCCESS;
        ESP_LOGI(TAG, "0x%04x: reporting on cluster 0x%04x %s", d->short_addr, m->info.cluster_id,
                 ok ? "configured" : "refused");
        m->out.result = EZB_ZCL_STATUS_SUCCESS;
        break;
    }
    case EZB_ZCL_CORE_IAS_ZONE_ENROLL_CB_ID: {
        ezb_zcl_ias_zone_enroll_req_message_t *m = message;
        zdev_t *d = from_header(m->in.header);
        if (!d) break;
        heard(d, m->in.header->rssi);
        d->zone_type = m->in.payload.zone_type;
        ias_enroll_rsp(d, m->in.header->src_ep);
        d->done |= IV_IAS;
        ESP_LOGI(TAG, "0x%04x: zone enroll request (type 0x%04x, %s): enrolled as zone %d", d->short_addr,
                 d->zone_type, zone_kind(d->zone_type), dev_index(d));
        interview_done_check(d);
        changed(true);
        m->out.result = EZB_ZCL_STATUS_SUCCESS;
        break;
    }
    case EZB_ZCL_CORE_IAS_ZONE_STATUS_CHANGE_NOTIF_CB_ID: {
        ezb_zcl_ias_zone_status_change_notif_message_t *m = message;
        zdev_t *d = from_header(m->in.header);
        if (!d) break;
        heard(d, m->in.header->rssi);
        d->zone_status = m->in.payload.zone_status;
        d->has_zone_status = true;
        d->done |= IV_IAS;  // notifications reaching us = enrolled
        ESP_LOGI(TAG, "0x%04x %s: %s 0x%04x%s%s", d->short_addr, d->name[0] ? d->name : d->model,
                 zone_kind(d->zone_type), d->zone_status, (d->zone_status & 1) ? " ALARM" : "",
                 (d->zone_status & 4) ? " tamper" : "");
        interview_done_check(d);
        changed(false);
        m->out.result = EZB_ZCL_STATUS_SUCCESS;
        break;
    }
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID:
        break;
    default:
        ESP_LOGD(TAG, "zcl action 0x%04" PRIx32, (uint32_t)id);
        break;
    }
    UNLOCK();
}

// ---- signals ------------------------------------------------------------------------------

void zb_dev_on_announce(uint16_t short_addr, uint64_t ieee, uint8_t capability)
{
    LOCK();
    zdev_t *d = by_ieee(ieee);
    bool fresh = false;
    if (!d) {
        for (int i = 0; i < ZB_MAX_DEVICES && !d; i++) {
            if (!s_dev[i].used) d = &s_dev[i];
        }
        if (!d) {
            UNLOCK();
            ESP_LOGE(TAG, "device table full (%d): ignoring 0x%016llx", ZB_MAX_DEVICES, (unsigned long long)ieee);
            return;
        }
        memset(d, 0, sizeof(*d));
        d->used = true;
        d->ieee = ieee;
        d->power_src = 0xFF;
        d->joined_us = esp_timer_get_time();
        fresh = true;
    }
    d->short_addr = short_addr;
    d->cap = capability;
    d->last_seen_us = esp_timer_get_time();  // not heard(): the interview starts below
    d->pings = 0;
    d->offline = false;
    ESP_LOGI(TAG, "%s 0x%016llx short 0x%04x cap 0x%02x%s", fresh ? "NEW device" : "rejoined",
             (unsigned long long)ieee, short_addr, capability, (capability & 0x08) ? "" : " (sleepy)");
    if (!complete(d)) {
        d->tries = 0;
        d->woke_us = esp_timer_get_time();
        interview_step(d);  // immediately: a sleepy device is awake only now
    }
    changed(true);
    UNLOCK();
}

void zb_dev_on_leave(uint64_t ieee)
{
    LOCK();
    zdev_t *d = by_ieee(ieee);
    if (d) {
        ESP_LOGW(TAG, "0x%04x %s left the network", d->short_addr, d->name);
        memset(d, 0, sizeof(*d));
        changed(true);
    }
    UNLOCK();
}

static bool is_mains(const zdev_t *d)
{
    return d->cap & 0x0C;  // MAC capability: mains powered (0x04) or receiver always on (0x08)
}

static void availability(zdev_t *d, int64_t now)
{
    int64_t idle = now - d->last_seen_us;  // last_seen 0 = counts from boot
    if (d->offline) return;
    if (!is_mains(d)) {
        if (idle > SLEEPY_TIMEOUT_US) {
            d->offline = true;
            ESP_LOGW(TAG, "0x%04x %s: offline (silent for %lld h)", d->short_addr, d->name[0] ? d->name : d->model,
                     idle / 3600000000LL);
            changed(false);
        }
        return;
    }
    if (idle < MAINS_IDLE_US || now - d->ping_at_us < PING_EVERY_US) return;
    if (d->pings >= PINGS_MAX) {
        d->offline = true;
        ESP_LOGW(TAG, "0x%04x %s: offline (no answer to %d pings)", d->short_addr, d->name[0] ? d->name : d->model,
                 PINGS_MAX);
        changed(false);
        return;
    }
    static uint16_t attr = 0x0000;  // ZCL version: every device has it
    ezb_zcl_read_attr_cmd_t r = {.cmd_ctrl = ctrl_to(d, first_ep(d), EZB_ZCL_CLUSTER_ID_BASIC),
                                 .payload = {.attr_number = 1, .attr_field = &attr}};
    ezb_zcl_read_attr_cmd_req(&r);
    d->pings++;
    d->ping_at_us = now;
}

void zb_dev_tick(void *arg)
{
    int64_t now = esp_timer_get_time();
    LOCK();
    for (int i = 0; i < ZB_MAX_DEVICES; i++) {
        zdev_t *d = &s_dev[i];
        if (d->used) availability(d, now);
        if (!d->used || complete(d) || !d->next_try_us || now < d->next_try_us) continue;
        if (d->tries >= MAX_TRIES) {
            d->next_try_us = 0;  // asleep: resume on its next check-in (heard())
            ESP_LOGW(TAG, "0x%04x: no answer after %d tries (asleep?); interview resumes when it's heard from",
                     d->short_addr, d->tries);
            changed(false);
            continue;
        }
        interview_step(d);
    }
    UNLOCK();
}

// ---- persistence --------------------------------------------------------------------------

static void ieee_hex(uint64_t ieee, char out[17])
{
    snprintf(out, 17, "%016llx", (unsigned long long)ieee);
}

static cJSON *dev_to_json(const zdev_t *d, bool live)
{
    cJSON *o = cJSON_CreateObject();
    char hex[17];
    ieee_hex(d->ieee, hex);
    cJSON_AddStringToObject(o, "ieee", hex);
    cJSON_AddNumberToObject(o, "short", d->short_addr);
    cJSON_AddNumberToObject(o, "cap", d->cap);
    cJSON_AddStringToObject(o, "name", d->name);
    cJSON_AddStringToObject(o, "manuf", d->manuf);
    cJSON_AddStringToObject(o, "model", d->model);
    cJSON_AddNumberToObject(o, "power", d->power_src);
    cJSON_AddNumberToObject(o, "done", d->done);
    cJSON_AddNumberToObject(o, "zone_type", d->zone_type);
    cJSON *eps = cJSON_AddArrayToObject(o, "eps");
    for (int e = 0; e < d->n_ep; e++) {
        const ep_t *ep = &d->ep[e];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "id", ep->id);
        cJSON_AddNumberToObject(j, "profile", ep->profile);
        cJSON_AddNumberToObject(j, "device", ep->device);
        cJSON *in = cJSON_AddArrayToObject(j, "in");
        for (int c = 0; c < ep->n_in; c++) cJSON_AddItemToArray(in, cJSON_CreateNumber(ep->in[c]));
        cJSON *out = cJSON_AddArrayToObject(j, "out");
        for (int c = 0; c < ep->n_out; c++) cJSON_AddItemToArray(out, cJSON_CreateNumber(ep->out[c]));
        cJSON_AddItemToArray(eps, j);
    }
    if (live) {
        int64_t now = esp_timer_get_time();
        const char *st = complete(d) ? "ready" : d->next_try_us ? "interviewing" : "waiting";
        cJSON_AddStringToObject(o, "status", st);
        cJSON_AddBoolToObject(o, "available", !d->offline);
        cJSON_AddBoolToObject(o, "sleepy", !(d->cap & 0x08));
        cJSON_AddNumberToObject(o, "seen_s", d->last_seen_us ? (double)((now - d->last_seen_us) / 1000000) : -1);
        cJSON_AddNumberToObject(o, "timeout_s", is_mains(d) ? (double)(MAINS_IDLE_US / 1000000) : (double)(SLEEPY_TIMEOUT_US / 1000000));
        if (d->rssi) cJSON_AddNumberToObject(o, "rssi", d->rssi);
        if (d->zone_type || d->has_zone_status) cJSON_AddStringToObject(o, "kind", zone_kind(d->zone_type));
        if (d->has_zone_status) {
            cJSON_AddNumberToObject(o, "zone_status", d->zone_status);
            cJSON_AddBoolToObject(o, "alarm", d->zone_status & 1);
            cJSON_AddBoolToObject(o, "tamper", d->zone_status & 4);
            cJSON_AddBoolToObject(o, "battery_low", d->zone_status & 8);
        }
        if (d->has_batt) cJSON_AddNumberToObject(o, "battery", d->batt / 2.0);
        if (d->has_batt_v) cJSON_AddNumberToObject(o, "battery_v", d->batt_v / 10.0);
        if (d->has_temp) cJSON_AddNumberToObject(o, "temperature", d->temp / 100.0);
        if (d->has_hum) cJSON_AddNumberToObject(o, "humidity", d->hum / 100.0);
        if (d->has_on) cJSON_AddBoolToObject(o, "on", d->on);
        if (d->has_occ) cJSON_AddBoolToObject(o, "occupied", d->occ & 1);
    }
    return o;
}

static void save_now(void)
{
    LOCK();
    s_dirty = false;
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < ZB_MAX_DEVICES; i++) {
        if (s_dev[i].used) cJSON_AddItemToArray(arr, dev_to_json(&s_dev[i], false));
    }
    UNLOCK();
    char *txt = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    bool ok = txt != NULL;
    if (ok) {
        mkdir(REG_DIR, 0755);
        FILE *f = fopen(REG_TMP, "w");
        ok = f && fputs(txt, f) >= 0;
        if (f) ok = (fclose(f) == 0) && ok;
        if (ok) ok = rename(REG_TMP, REG_PATH) == 0;
        cJSON_free(txt);
    }
    if (!ok) {
        // Still unsaved: the saver task tries again on its next pass (2 s).
        ESP_LOGE(TAG, "saving the device registry failed, retrying");
        LOCK();
        s_dirty = true;
        UNLOCK();
    }
}

static void saver_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (s_dirty) save_now();
    }
}

static void load(void)
{
    FILE *f = fopen(REG_PATH, "r");
    if (!f) return;
    struct stat st;
    if (stat(REG_PATH, &st) != 0 || st.st_size <= 0 || st.st_size > 256 * 1024) {
        fclose(f);
        return;
    }
    char *txt = heap_caps_malloc(st.st_size + 1, MALLOC_CAP_SPIRAM);
    size_t n = txt ? fread(txt, 1, st.st_size, f) : 0;
    fclose(f);
    if (!txt) return;
    txt[n] = '\0';
    cJSON *arr = cJSON_Parse(txt);
    free(txt);
    int count = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        if (count >= ZB_MAX_DEVICES) break;
        zdev_t *d = &s_dev[count];
        const cJSON *j = cJSON_GetObjectItem(o, "ieee");
        if (!cJSON_IsString(j)) continue;
        d->ieee = strtoull(j->valuestring, NULL, 16);
        d->used = d->ieee != 0;
        if (!d->used) continue;
#define NUM(k) (cJSON_IsNumber(cJSON_GetObjectItem(o, k)) ? cJSON_GetObjectItem(o, k)->valueint : 0)
#define STR(k, dst)                                                    \
        do {                                                           \
            const cJSON *s_ = cJSON_GetObjectItem(o, k);               \
            if (cJSON_IsString(s_)) strlcpy(dst, s_->valuestring, sizeof(dst)); \
        } while (0)
        d->short_addr = NUM("short");
        d->cap = NUM("cap");
        d->power_src = NUM("power");
        d->done = NUM("done");
        d->zone_type = NUM("zone_type");
        STR("name", d->name);
        STR("manuf", d->manuf);
        STR("model", d->model);
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(o, "eps")) {
            if (d->n_ep >= ZB_MAX_EPS) break;
            ep_t *ep = &d->ep[d->n_ep++];
            const cJSON *v;
            v = cJSON_GetObjectItem(e, "id");      ep->id = cJSON_IsNumber(v) ? v->valueint : 0;
            v = cJSON_GetObjectItem(e, "profile"); ep->profile = cJSON_IsNumber(v) ? v->valueint : 0;
            v = cJSON_GetObjectItem(e, "device");  ep->device = cJSON_IsNumber(v) ? v->valueint : 0;
            cJSON_ArrayForEach(v, cJSON_GetObjectItem(e, "in")) {
                if (ep->n_in < ZB_MAX_CLUSTERS) ep->in[ep->n_in++] = v->valueint;
            }
            cJSON_ArrayForEach(v, cJSON_GetObjectItem(e, "out")) {
                if (ep->n_out < ZB_MAX_CLUSTERS) ep->out[ep->n_out++] = v->valueint;
            }
        }
        d->desc_ok = (1 << d->n_ep) - 1;
        count++;
    }
    cJSON_Delete(arr);
    ESP_LOGI(TAG, "registry: %d device(s) loaded", count);
}

esp_err_t zb_dev_init(void)
{
    s_dev = heap_caps_calloc(ZB_MAX_DEVICES, sizeof(zdev_t), MALLOC_CAP_SPIRAM);
    s_mtx = xSemaphoreCreateMutex();
    if (!s_dev || !s_mtx) return ESP_ERR_NO_MEM;
    load();
    if (xTaskCreate(saver_task, "zb_save", 4096, NULL, 2, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void zb_dev_set_my_ieee(uint64_t ieee)
{
    s_my_ieee = ieee;
}

// ---- public API (any task) ------------------------------------------------------------------

uint32_t zb_revision(void)
{
    return s_rev;
}

cJSON *zb_devices_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    LOCK();
    for (int i = 0; i < ZB_MAX_DEVICES; i++) {
        if (s_dev[i].used) cJSON_AddItemToArray(arr, dev_to_json(&s_dev[i], true));
    }
    UNLOCK();
    return arr;
}

esp_err_t zb_rename(const char *ieee_hex, const char *name)
{
    uint64_t ieee = strtoull(ieee_hex, NULL, 16);
    LOCK();
    zdev_t *d = by_ieee(ieee);
    if (d) {
        size_t o = 0;
        for (size_t i = 0; name[i] && o < sizeof(d->name) - 1; i++) {
            if ((unsigned char)name[i] >= 0x20) d->name[o++] = name[i];
        }
        d->name[o] = '\0';
        changed(true);
    }
    UNLOCK();
    return d ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool zb_dev_lookup_short(const char *ieee_hex, uint16_t *short_addr, uint64_t *ieee_out)
{
    uint64_t ieee = strtoull(ieee_hex, NULL, 16);
    LOCK();
    zdev_t *d = by_ieee(ieee);
    if (d) {
        *short_addr = d->short_addr;
        *ieee_out = ieee;
    }
    UNLOCK();
    return d != NULL;
}

void zb_dev_forget(uint64_t ieee)
{
    zb_dev_on_leave(ieee);
}
