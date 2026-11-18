// The entity set Home Assistant sees, and each entity's state.
//
// Home Assistant identifies an ESPHome entity by "<mac>/<device_id>/<type>/<name>"
// (aioesphomeapi build_unique_id, version 3), so its history survives as long as the
// sub-device id and the entity name don't change:
//   - a Zigbee device's id is a hash of its IEEE address, its entities have fixed names
//     ("Contact", "Battery"); renaming the device in our UI renames only the HA device
//   - a script's id is a hash of the script name; its entities are "Enabled" and the
//     names the script publishes
//   - the hub's own entities are on device 0 with fixed names; its Wi-Fi motion sensor is a
//     device of its own ("Motion sensor", one unnamed entity: HA shows the device name)
// An IAS sensor's main entity is listed only once its zone type is known, so a contact
// sensor never shows up first as a generic "Alarm" that then disappears.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "ha_internal.h"
#include "hublog.h"
#include "motion.h"
#include "wifi5g.h"

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t fnv_str(uint32_t h, const char *s)
{
    return fnv(h, s, strlen(s) + 1);
}

static uint32_t dev_id(const char *prefix, const char *ref)
{
    uint32_t h = fnv_str(fnv_str(2166136261u, prefix), ref);
    return h ? h : 1;  // 0 is the hub
}

static ha_ent_t *add(ha_model_t *m, uint8_t type, uint8_t val, uint32_t device_id, const char *name)
{
    if (m->n_ent >= HA_MAX_ENT) return NULL;
    ha_ent_t *e = &m->ent[m->n_ent++];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->val = val;
    e->device_id = device_id;
    strlcpy(e->name, name, sizeof(e->name));
    return e;
}

static void set(ha_ent_t *e, const char *device_class, const char *unit, int decimals, uint8_t state_class,
                uint8_t category, bool disabled)
{
    if (!e) return;
    if (device_class) strlcpy(e->device_class, device_class, sizeof(e->device_class));
    if (unit) strlcpy(e->unit, unit, sizeof(e->unit));
    e->decimals = (int8_t)decimals;
    e->state_class = state_class;
    e->category = category;
    e->disabled = disabled;
}

#define DIAG 2
#define MEAS 1

// IAS zone type -> the name and HA device class of the sensor's main entity.
static void zone_entity(uint16_t zone_type, const char **name, const char **dc)
{
    switch (zone_type) {
    case 0x000D: *name = "Motion";          *dc = "motion";          break;
    case 0x0015: *name = "Contact";         *dc = "opening";         break;
    case 0x0028: *name = "Smoke";           *dc = "smoke";           break;
    case 0x002A: *name = "Water leak";      *dc = "moisture";        break;
    case 0x002B: *name = "Carbon monoxide"; *dc = "carbon_monoxide"; break;
    case 0x002D: *name = "Vibration";       *dc = "vibration";       break;
    default:     *name = "Alarm";           *dc = "safety";          break;
    }
}

// Stable keys of a Zigbee device's own entities: what the per-device hide list holds
static const char *zb_key(uint8_t val)
{
    switch (val) {
    case V_ZB_ZONE: return "zone";           case V_ZB_TAMPER: return "tamper";
    case V_ZB_BATT_LOW: return "battery_low"; case V_ZB_BATT: return "battery";
    case V_ZB_BATT_V: return "battery_v";    case V_ZB_TEMP: return "temperature";
    case V_ZB_HUM: return "humidity";        case V_ZB_OCC: return "occupancy";
    case V_ZB_ON: return "power";            case V_ZB_POWER_ON: return "power_on";
    case V_ZB_WATTS: return "power_draw";    case V_ZB_VOLTS: return "voltage";
    case V_ZB_AMPS: return "current";        case V_ZB_KWH: return "energy";
    case V_ZB_ONLINE: return "online";       case V_ZB_RSSI: return "signal";
    case V_ZB_SEEN: return "last_seen";      case V_ZB_FW: return "firmware";
    default: return "";
    }
}

static bool key_hidden(const char *list, const char *key)
{
    size_t n = strlen(key);
    for (const char *p = list; n && *p;) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && strncmp(p, key, n) == 0) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}

// A Zigbee device's entities on sub-device `id`. for_ha: leave out what the user hid, add the
// user's own entities (zb_custom_t); otherwise only the hub's own, all of them.
static void zb_entities(ha_model_t *m, const zb_dev_info_t *d, uint32_t id, bool for_ha)
{
    ha_ent_t *e;
    int first = m->n_ent;
    if (d->ias && d->zone_type) {
        const char *name, *dc;
        zone_entity(d->zone_type, &name, &dc);
        set(add(m, ET_BINARY, V_ZB_ZONE, id, name), dc, NULL, 0, 0, 0, false);
    }
    if (d->ias) {
        set(add(m, ET_BINARY, V_ZB_TAMPER, id, "Tamper"), "tamper", NULL, 0, 0, DIAG, false);
        set(add(m, ET_BINARY, V_ZB_BATT_LOW, id, "Battery low"), "battery", NULL, 0, 0, DIAG, false);
    }
    if (d->power_cfg) {
        set(add(m, ET_SENSOR, V_ZB_BATT, id, "Battery"), "battery", "%", 0, MEAS, DIAG, false);
        set(add(m, ET_SENSOR, V_ZB_BATT_V, id, "Battery voltage"), "voltage", "V", 1, MEAS, DIAG, true);
    }
    if (d->temp_meas) set(add(m, ET_SENSOR, V_ZB_TEMP, id, "Temperature"), "temperature", "°C", 1, MEAS, 0, false);
    if (d->hum_meas) set(add(m, ET_SENSOR, V_ZB_HUM, id, "Humidity"), "humidity", "%", 0, MEAS, 0, false);
    if (d->occ_sensing) set(add(m, ET_BINARY, V_ZB_OCC, id, "Occupancy"), "occupancy", NULL, 0, 0, 0, false);
    // A switch: Home Assistant turns it on/off through zb_set_on (ha_server.c)
    if (d->on_off) set(add(m, ET_SWITCH, V_ZB_ON, id, "Power"), "outlet", NULL, 0, 0, 0, false);
    // What it does when power comes back; Home Assistant sets it through zb_set_power_on
    if (d->on_off) {
        e = add(m, ET_SELECT, V_ZB_POWER_ON, id, "Power on behavior");
        set(e, NULL, NULL, 0, 0, 1, false);
        if (e) strlcpy(e->icon, "mdi:power-settings", sizeof(e->icon));
    }
    if (d->elec_meas) {
        set(add(m, ET_SENSOR, V_ZB_WATTS, id, "Power draw"), "power", "W", 1, MEAS, 0, false);
        set(add(m, ET_SENSOR, V_ZB_VOLTS, id, "Voltage"), "voltage", "V", 1, MEAS, 0, false);
        set(add(m, ET_SENSOR, V_ZB_AMPS, id, "Current"), "current", "A", 3, MEAS, 0, false);
    }
    if (d->metering) set(add(m, ET_SENSOR, V_ZB_KWH, id, "Energy"), "energy", "kWh", 3, 2, 0, false);
    set(add(m, ET_BINARY, V_ZB_ONLINE, id, "Online"), "connectivity", NULL, 0, 0, DIAG, false);
    set(add(m, ET_SENSOR, V_ZB_RSSI, id, "Signal"), "signal_strength", "dBm", 0, MEAS, DIAG, true);
    set(add(m, ET_TEXT, V_ZB_SEEN, id, "Last seen"), "timestamp", NULL, 0, 0, DIAG, false);
    // Installed vs offered OTA file version (zb_ota_offer); Install runs zb_ota_install.
    // Listed once the installed version is known (a version "0" would read as up to date)
    if (d->ota && d->has_fw_ver) set(add(m, ET_UPDATE, V_ZB_FW, id, "Firmware"), "firmware", NULL, 0, 0, 1, false);
    if (for_ha) {
        int w = first;
        for (int k = first; k < m->n_ent; k++) {
            if (!key_hidden(d->ha_hide, zb_key(m->ent[k].val))) m->ent[w++] = m->ent[k];
        }
        m->n_ent = w;
        // The user's own: value read by the hub (zb_dev.c), a switch writes through zb_custom_write
        for (int c = 0; c < d->n_custom; c++) {
            const zb_custom_t *u = &d->custom[c];
            uint8_t type = u->kind == ZB_CUSTOM_SWITCH ? ET_SWITCH : u->kind == ZB_CUSTOM_BINARY ? ET_BINARY : ET_SENSOR;
            e = add(m, type, V_ZB_CUSTOM, id, u->name);
            if (!e) break;
            int dec = u->scale >= 1000 ? 3 : u->scale >= 100 ? 2 : u->scale >= 10 ? 1 : 0;
            // HA's state classes: 1 measurement, 2 total_increasing (as ZB_STATE_CLASS_*)
            set(e, u->device_class[0] ? u->device_class : NULL, u->unit, dec, type == ET_SENSOR ? u->state_class : 0, 0,
                false);
            strlcpy(e->ref2, u->name, sizeof(e->ref2));
        }
    }
    for (int k = first; k < m->n_ent; k++) strlcpy(m->ent[k].ref, d->ieee, sizeof(m->ent[k].ref));
}

cJSON *ha_device_builtin_json(const zb_dev_info_t *d)
{
    cJSON *arr = cJSON_CreateArray();
    ha_model_t *m = heap_caps_calloc(1, sizeof(*m), MALLOC_CAP_SPIRAM);
    if (!m) return arr;
    zb_entities(m, d, 1, false);
    static const char *const KIND[] = {[ET_BINARY] = "binary", [ET_SENSOR] = "sensor", [ET_TEXT] = "text",
                                       [ET_SWITCH] = "switch", [ET_BUTTON] = "button", [ET_EVENT] = "event",
                                       [ET_UPDATE] = "update", [ET_SELECT] = "select"};
    for (int k = 0; k < m->n_ent; k++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "key", zb_key(m->ent[k].val));
        cJSON_AddStringToObject(o, "name", m->ent[k].name);
        cJSON_AddStringToObject(o, "kind", m->ent[k].type <= ET_SELECT ? KIND[m->ent[k].type] : "");
        if (m->ent[k].unit[0]) cJSON_AddStringToObject(o, "unit", m->ent[k].unit);
        cJSON_AddItemToArray(arr, o);
    }
    free(m);
    return arr;
}

void ha_model_build(ha_model_t *m, const ha_inputs_t *in)
{
    m->n_ent = 0;
    m->n_dev = 0;
    ha_ent_t *e;

    // The hub itself (device 0)
    e = add(m, ET_BUTTON, V_HUB_PERMIT, 0, "Permit join");
    if (e) strlcpy(e->icon, "mdi:zigbee", sizeof(e->icon));
    add(m, ET_BINARY, V_HUB_PAIRING, 0, "Pairing");
    set(add(m, ET_SENSOR, V_HUB_DEVICES, 0, "Zigbee devices"), NULL, NULL, 0, MEAS, DIAG, false);
    set(add(m, ET_SENSOR, V_HUB_CHANNEL, 0, "Zigbee channel"), NULL, NULL, 0, 0, DIAG, false);
    set(add(m, ET_SENSOR, V_HUB_RSSI, 0, "Wi-Fi signal"), "signal_strength", "dBm", 0, MEAS, DIAG, false);
    set(add(m, ET_SENSOR, V_HUB_UPTIME, 0, "Uptime"), "duration", "s", 0, 2, DIAG, false);

    // The hub's Wi-Fi motion sensor
    uint32_t motion_id = dev_id("hub:", "motion");
    m->dev[m->n_dev++] = (ha_dev_t){.id = motion_id, .name = "Motion sensor"};
    set(add(m, ET_BINARY, V_HUB_MOTION, motion_id, ""), "motion", NULL, 0, 0, 0, false);

    // Zigbee devices: one sub-device each
    for (int i = 0; i < in->n_zb && m->n_dev < HA_MAX_DEV; i++) {
        const zb_dev_info_t *d = &in->zb[i];
        uint32_t id = dev_id("zb:", d->ieee);
        ha_dev_t *dv = &m->dev[m->n_dev++];
        dv->id = id;
        strlcpy(dv->name, d->name[0] ? d->name : d->model[0] ? d->model : d->ieee, sizeof(dv->name));
        zb_entities(m, d, id, true);
    }

    // Scripts: one sub-device each, with its switch and what it publishes
    for (int i = 0; i < in->n_scripts && m->n_dev < HA_MAX_DEV; i++) {
        const auto_brief_t *sc = &in->scripts[i];
        uint32_t id = dev_id("script:", sc->name);
        ha_dev_t *dv = &m->dev[m->n_dev++];
        dv->id = id;
        snprintf(dv->name, sizeof(dv->name), "Script %s", sc->name);
        e = add(m, ET_SWITCH, V_SCRIPT_ENABLED, id, "Enabled");
        if (e) {
            strlcpy(e->ref, sc->name, sizeof(e->ref));
            strlcpy(e->icon, "mdi:script-text-outline", sizeof(e->icon));
            e->category = 1;  // config
        }
        for (int k = 0; k < in->n_sent; k++) {
            const auto_ha_entity_t *p = &in->sent[k];
            if (strcmp(p->script, sc->name) != 0) continue;
            uint8_t type = p->type == AUTO_HA_SENSOR ? ET_SENSOR : p->type == AUTO_HA_BINARY ? ET_BINARY : ET_EVENT;
            e = add(m, type, V_SCRIPT_VALUE, id, p->name);
            if (!e) break;
            strlcpy(e->ref, sc->name, sizeof(e->ref));
            strlcpy(e->ref2, p->name, sizeof(e->ref2));
            strlcpy(e->device_class, p->device_class, sizeof(e->device_class));
            strlcpy(e->unit, p->unit, sizeof(e->unit));
            strlcpy(e->icon, p->icon, sizeof(e->icon));
            e->decimals = p->decimals;
            e->state_class = p->state_class;
            e->n_types = p->n_types;
            memcpy(e->types, p->types, sizeof(e->types));
        }
    }

    // Keys: a hash of what identifies the entity in Home Assistant; collisions resolved
    // in a fixed order so the same set always gets the same keys.
    for (int i = 0; i < m->n_ent; i++) {
        ha_ent_t *x = &m->ent[i];
        uint32_t k = fnv_str(fnv(fnv(2166136261u, &x->type, 1), &x->device_id, 4), x->name);
        for (bool clash = true; clash;) {
            clash = k == 0;
            for (int j = 0; j < i && !clash; j++) clash = m->ent[j].key == k;
            if (clash) k++;
        }
        x->key = k;
    }

    uint32_t sig = 2166136261u;
    for (int i = 0; i < m->n_ent; i++) {
        const ha_ent_t *x = &m->ent[i];
        sig = fnv(sig, &x->key, 4);
        sig = fnv(sig, &x->category, 1);
        sig = fnv(sig, &x->disabled, 1);
        sig = fnv(sig, &x->decimals, 1);
        sig = fnv(sig, &x->state_class, 1);
        sig = fnv_str(sig, x->device_class);
        sig = fnv_str(sig, x->unit);
        sig = fnv_str(sig, x->icon);
        for (int t = 0; t < x->n_types; t++) sig = fnv_str(sig, x->types[t]);
    }
    for (int i = 0; i < m->n_dev; i++) {
        sig = fnv(sig, &m->dev[i].id, 4);
        sig = fnv_str(sig, m->dev[i].name);
    }
    m->sig = sig;
}

bool ha_model_is_slow(const ha_ent_t *e)
{
    return e->val == V_HUB_RSSI || e->val == V_HUB_UPTIME || e->val == V_ZB_RSSI;
}

static const zb_dev_info_t *zb_of(const ha_inputs_t *in, const char *ieee)
{
    for (int i = 0; i < in->n_zb; i++) {
        if (strcmp(in->zb[i].ieee, ieee) == 0) return &in->zb[i];
    }
    return NULL;
}

static void zb_state(const ha_ent_t *e, const zb_dev_info_t *d, const ha_state_t *prev, ha_state_t *st)
{
    st->missing = true;
    if (!d) return;
    // An offline device's values are old: unknown in Home Assistant, except whether
    // it is online and when it was last heard
    if (!d->available && e->val != V_ZB_ONLINE && e->val != V_ZB_SEEN) return;
    switch (e->val) {
    case V_ZB_ZONE:     st->missing = !d->has_zone; st->b = d->zone_status & 1; break;
    case V_ZB_TAMPER:   st->missing = !d->has_zone; st->b = d->zone_status & 4; break;
    case V_ZB_BATT_LOW: st->missing = !d->has_zone; st->b = d->zone_status & 8; break;
    case V_ZB_BATT:     st->missing = !d->has_batt; st->f = d->batt; break;
    case V_ZB_BATT_V:   st->missing = !d->has_batt_v; st->f = d->batt_v; break;
    case V_ZB_TEMP:     st->missing = !d->has_temp; st->f = d->temp; break;
    case V_ZB_HUM:      st->missing = !d->has_hum; st->f = d->hum; break;
    case V_ZB_OCC:      st->missing = !d->has_occ; st->b = d->occ; break;
    case V_ZB_ON:       st->missing = !d->has_on; st->b = d->on; break;
    case V_ZB_WATTS:    st->missing = !d->has_power; st->f = d->power; break;
    case V_ZB_VOLTS:    st->missing = !d->has_volt; st->f = d->volt; break;
    case V_ZB_AMPS:     st->missing = !d->has_curr; st->f = d->curr; break;
    case V_ZB_KWH:      st->missing = !d->has_energy; st->f = d->energy; break;
    case V_ZB_CUSTOM:
        for (int i = 0; i < d->n_custom; i++) {
            if (strcmp(d->custom[i].name, e->ref2) != 0) continue;
            st->missing = !d->custom[i].has_value;
            bool zero = (d->custom[i].flags & ZB_CUSTOM_ZERO_WHEN_OFF) && d->has_on && !d->on;
            st->f = zero ? 0 : (float)d->custom[i].value;
            st->b = !zero && d->custom[i].value != 0;
        }
        break;
    case V_ZB_POWER_ON: {
        static const char *const OPT[] = HA_POWER_ON_OPTIONS;
        int i = !d->has_startup ? -1 : d->startup == 0x00 ? 0 : d->startup == 0x01 ? 1 : d->startup == 0x02 ? 2
              : d->startup == 0xFF ? 3 : -1;
        st->missing = i < 0;
        if (i >= 0) strlcpy(st->text, OPT[i], sizeof(st->text));
        break;
    }
    case V_ZB_FW: {
        // Versions as the OTA file version in decimal, as OTA indexes list them; Home
        // Assistant compares them as numbers
        st->missing = !d->has_fw_ver;
        snprintf(st->text, sizeof(st->text), "%lu", (unsigned long)d->fw_ver);
        uint32_t latest = d->has_offer && d->offer_ver > d->fw_ver ? d->offer_ver : d->fw_ver;
        snprintf(st->text2, sizeof(st->text2), "%lu", (unsigned long)latest);
        float p;
        st->b = zb_ota_device(d->ieee, &p);  // in progress
        st->f = p;                           // %, -1 = not known
        break;
    }
    case V_ZB_ONLINE:   st->missing = false; st->b = d->available; break;
    case V_ZB_RSSI:     st->missing = d->rssi == 0; st->f = d->rssi; break;
    case V_ZB_SEEN:
        // Wall-clock time of the last frame; unknown before the clock is set or before
        // the device was heard since boot. Made once per frame (recomputing it would
        // wobble by a second)
        if (!prev->missing && prev->src_us == d->last_seen_us) {
            *st = *prev;
        } else if (d->last_seen_us && hublog_time_valid()) {
            int64_t age_s = (esp_timer_get_time() - d->last_seen_us) / 1000000;
            time_t t = time(NULL) - (time_t)age_s;
            struct tm tm;
            gmtime_r(&t, &tm);
            strftime(st->text, sizeof(st->text), "%Y-%m-%dT%H:%M:%S+00:00", &tm);
            st->missing = false;
            st->src_us = d->last_seen_us;
        }
        break;
    default: break;
    }
}

void ha_model_state(const ha_ent_t *e, const ha_inputs_t *in, const ha_state_t *prev, ha_state_t *st)
{
    memset(st, 0, sizeof(*st));
    switch (e->val) {
    case V_HUB_PERMIT: st->missing = true; break;  // buttons have no state
    case V_HUB_PAIRING: st->b = zb_permit_left() > 0; break;
    case V_HUB_MOTION: {
        int r = motion_moving_now();
        st->missing = r < 0;
        st->b = r > 0;
        break;
    }
    case V_HUB_DEVICES: st->f = in->n_zb; break;
    case V_HUB_CHANNEL: {
        uint8_t ch = zb_channel();
        st->missing = ch == 0;
        st->f = ch;
        break;
    }
    case V_HUB_RSSI: {
        wifi5g_stats_t w;
        wifi5g_get_stats(&w);
        st->missing = !w.connected;
        st->f = w.rssi;
        break;
    }
    case V_HUB_UPTIME: st->f = (float)(esp_timer_get_time() / 1000000); break;
    case V_SCRIPT_ENABLED:
        st->missing = true;
        for (int i = 0; i < in->n_scripts; i++) {
            if (strcmp(in->scripts[i].name, e->ref) == 0) {
                st->missing = false;
                st->b = in->scripts[i].enabled;
            }
        }
        break;
    case V_SCRIPT_VALUE:
        st->missing = true;
        for (int i = 0; i < in->n_sent; i++) {
            const auto_ha_entity_t *p = &in->sent[i];
            if (strcmp(p->script, e->ref) == 0 && strcmp(p->name, e->ref2) == 0) {
                st->missing = !p->has_value;
                st->f = p->value;
                st->b = p->on;
            }
        }
        break;
    default: zb_state(e, zb_of(in, e->ref), prev, st); break;
    }
}

int ha_select_options(uint8_t val, const char *const **opts)
{
    (void)val;  // the only select so far: a plug's power-on behaviour
    static const char *const POWER_ON[] = HA_POWER_ON_OPTIONS;
    *opts = POWER_ON;
    return sizeof(POWER_ON) / sizeof(POWER_ON[0]);
}
