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
esp_err_t zb_reinterview(const char *ieee_hex);
// Switch a device with an On/Off server cluster (name, case-insensitive, or IEEE hex):
// ZB_OFF, ZB_ON or ZB_TOGGLE. Queued to the Zigbee task, never blocks; the new state
// arrives as the device's report. ESP_ERR_NOT_FOUND: no such device;
// ESP_ERR_NOT_SUPPORTED: it has no On/Off cluster. Any task.
enum { ZB_OFF = 0, ZB_ON = 1, ZB_TOGGLE = 2 };
esp_err_t zb_set_on(const char *device, int mode);  // sleepy devices: at their next check-in
// ---- device firmware updates (OTA Upgrade server) ----------------------------------------
// One image at a time, in PSRAM, lost on restart. Any task.
// Stage: fetch `url` (https://, or http:// to a LAN/tailnet host) for the device with IEEE
// `ieee_hex`, check the sha512 (128 hex digits) and the image's OTA header against the
// device. Nothing is offered yet. ESP_ERR_NOT_FOUND: no such device or no OTA client;
// ESP_ERR_INVALID_STATE: another image is being fetched, offered or sent.
esp_err_t zb_ota_stage(const char *ieee_hex, const char *url, const char *sha512_hex);
// Start: offer the staged image to its device. Only that device, asking for the same
// manufacturer code and image type while running another file version, gets it.
// ESP_ERR_INVALID_STATE: nothing staged (or Zigbee down); ESP_ERR_INVALID_RESPONSE: the
// device has since reported another image type.
esp_err_t zb_ota_start(void);
esp_err_t zb_ota_cancel(void);  // stops offering/sending, drops the image
// Ask an always-on device what it runs (its answer updates its "fw"). Offers nothing.
// ESP_ERR_NOT_SUPPORTED: a sleepy device (it asks by itself, now and then).
esp_err_t zb_ota_ask(const char *ieee_hex);
// Offer: newer firmware for a device, left by whoever checks an OTA index (Home Assistant).
// Persisted with the device, shown to Home Assistant as the latest version, dropped once the
// device runs it. url NULL clears it. ESP_ERR_NOT_SUPPORTED: no OTA client.
esp_err_t zb_ota_offer(const char *ieee_hex, uint32_t file_ver, const char *url, const char *sha512_hex,
                       const char *notes);
// Install the offered firmware: stage, then start by itself once staged.
// ESP_ERR_NOT_FOUND: nothing offered; otherwise as zb_ota_stage.
esp_err_t zb_ota_install(const char *ieee_hex);
// An update for this device is under way; progress in % while blocks are sent, else -1.
bool zb_ota_device(const char *ieee_hex, float *progress);
cJSON *zb_ota_json(void);       // state, progress, last error (caller frees)
bool zb_ota_busy(void);         // fetching, or blocks being sent (the reboot gate waits)

bool zb_interviewing(void);  // a device is being interviewed right now (awake, retrying)
void zb_save_now(void);      // write the device registry to flash now (before a reboot)

// ---- state changes for consumers (automation engine, Home Assistant API) -------------
// Attribute names: "open" / "motion" / "water" / "smoke" / "co" / "vibration" / "alarm"
// (IAS zone alarm bit; `alias` is always "alarm" for those), "tamper", "battery_low",
// "battery" (%), "battery_v" (V), "temperature" (°C), "humidity" (%), "occupied", "on",
// "power" (W), "voltage" (V), "current" (A), "energy" (kWh), "available".
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
    bool ias, power_cfg, temp_meas, hum_meas, occ_sensing, on_off, elec_meas, metering;
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
    bool has_power; float power; // W
    bool has_volt; float volt;   // V
    bool has_curr; float curr;   // A
    bool has_energy; float energy;  // kWh
    bool ota;                    // has an OTA Upgrade client (can be updated)
    bool has_fw_ver; uint32_t fw_ver;        // OTA file version it runs
    bool has_offer; uint32_t offer_ver;      // newer file version offered (zb_ota_offer)
    char offer_notes[128];                   // its release notes, "" = none
} zb_dev_info_t;

// Fills up to `max` devices; returns how many. Any task (takes the registry lock briefly).
int zb_snapshot(zb_dev_info_t *out, int max);

// Current value of `attr` on the device named `device` (name, case-insensitive, or
// IEEE hex). Last known value, kept across restarts (zb_dev.c persists it).
// ESP_ERR_NOT_FOUND: no such device; ESP_ERR_INVALID_STATE: never reported.
esp_err_t zb_get_attr(const char *device, const char *attr, double *value, bool *is_bool);
