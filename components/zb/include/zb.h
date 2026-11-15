// Zigbee 3.0 coordinator. Starts before Wi-Fi and never depends on it.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#define ZB_MAX_DEVICES   32
#define ZB_MAX_EPS       4
#define ZB_MAX_CLUSTERS  16
#define ZB_NAME_LEN      32

// Starts the stack on its own task. Forms a network on first boot (channel 15 or
// 25), restores it from the zb_storage partition after that.
esp_err_t zb_start(void);
bool zb_is_up(void);  // network formed/restored and the coordinator is running
uint8_t zb_channel(void);  // 0 until the network is up. Any task

// Pairing: open the network for `seconds` (1..254; 0 closes it).
esp_err_t zb_permit_join(uint8_t seconds);
uint32_t zb_permit_left(void);  // seconds left, 0 = closed

// Snapshots for the UI (caller frees with cJSON_Delete). Any task.
cJSON *zb_devices_json(void);
cJSON *zb_network_json(void);
uint32_t zb_revision(void);     // changes whenever devices/state change

esp_err_t zb_rename(const char *ieee_hex, const char *name);
esp_err_t zb_remove(const char *ieee_hex);  // asks the device to leave, then forgets it
esp_err_t zb_reinterview(const char *ieee_hex);  // sleepy devices: at their next check-in
bool zb_interviewing(void);  // a device is being interviewed right now (awake, retrying)
void zb_save_now(void);      // write the device registry to flash now (before a reboot)

// ---- state changes for consumers (automation engine, Home Assistant API) -------------
// Attribute names: "open" / "motion" / "water" / "smoke" / "co" / "vibration" / "alarm"
// (IAS zone alarm bit; `alias` is always "alarm" for those), "tamper", "battery_low",
// "battery" (%), "battery_v" (V), "temperature" (°C), "humidity" (%), "occupied", "on",
// "available".
typedef struct {
    char ieee[17];
    char device[ZB_NAME_LEN];    // the user's name, else the model, else the IEEE
    const char *attr;            // static string
    const char *alias;           // static string or NULL
    bool is_bool;
    double value;                // 0/1 for booleans
} zb_event_t;

// Called on the Zigbee task for every value that changed, while the registry is
// locked: it must only copy the event and return (never block, never call zb_*).
// Up to ZB_MAX_EVENT_CBS listeners, added once at boot.
#define ZB_MAX_EVENT_CBS 2
typedef void (*zb_event_cb_t)(const zb_event_t *e);
esp_err_t zb_add_event_cb(zb_event_cb_t cb);

// Compact copy of one device for consumers that mirror the registry (Home Assistant API).
// What it can report comes from its server clusters (stable once interviewed); the
// values are the last known ones, `has_*` false until the first report.
typedef struct {
    char ieee[17];
    char name[ZB_NAME_LEN];      // the user's name, "" = none
    char manuf[33], model[33];
    bool ias, power_cfg, temp_meas, hum_meas, occ_sensing, on_off;
    uint16_t zone_type;          // 0 = not known yet
    bool available;
    bool sleepy;
    int8_t rssi;                 // 0 = not heard since boot
    int64_t last_seen_us;        // esp_timer time of the last frame, 0 = not since boot
    bool has_zone; uint16_t zone_status;
    bool has_batt; float batt;   // %
    bool has_batt_v; float batt_v;
    bool has_temp; float temp;   // °C
    bool has_hum; float hum;     // %
    bool has_on; bool on;
    bool has_occ; bool occ;
} zb_dev_info_t;

// Fills up to `max` devices; returns how many. Any task (takes the registry lock briefly).
int zb_snapshot(zb_dev_info_t *out, int max);

// Current value of `attr` on the device named `device` (name, case-insensitive, or
// IEEE hex). Last known value, kept across restarts (zb_dev.c persists it).
// ESP_ERR_NOT_FOUND: no such device; ESP_ERR_INVALID_STATE: never reported.
esp_err_t zb_get_attr(const char *device, const char *attr, double *value, bool *is_bool);
