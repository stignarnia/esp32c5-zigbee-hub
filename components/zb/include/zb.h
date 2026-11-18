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
// What an On/Off device does when power comes back (ZCL StartUpOnOff): off, on, toggle or
// its previous state. Written, then read back: the device's answer shows as "power_on" in
// zb_devices_json. Same errors as zb_set_on. Any task.
enum { ZB_POWER_ON_OFF = 0, ZB_POWER_ON_ON, ZB_POWER_ON_TOGGLE, ZB_POWER_ON_PREVIOUS };
esp_err_t zb_set_power_on(const char *device, int mode);
// SONOFF eWeLink devices (cluster 0xFC11), "inching": after each turn-on the device switches
// itself off `seconds` later (ZB_INCHING_OFF, mode byte 0x80), or after each turn-off back on
// (ZB_INCHING_ON, 0x81); ZB_INCHING_NONE = never. 1..3599 s. Sent as SONOFF's command; the device
// can't be asked for it, so the hub keeps what was last set (persisted). ESP_ERR_NOT_SUPPORTED:
// not such a device. Any task.
enum { ZB_INCHING_NONE = 0, ZB_INCHING_OFF, ZB_INCHING_ON };
#define ZB_INCHING_MAX_S 3599
esp_err_t zb_set_inching(const char *device, int mode, uint16_t seconds);
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

// ---- arbitrary requests (zb_raw.c) ---------------------------------------------------------
// Read or write any attribute of any cluster, or send a command on a manufacturer cluster
// (>= 0xFC00; standard clusters have their dedicated calls), and get the device's answer.
#define ZB_RAW_MAX_ATTRS   8
#define ZB_RAW_VALUE_MAX   32
#define ZB_RAW_PAYLOAD_MAX 64
typedef enum { ZB_RAW_READ = 0, ZB_RAW_WRITE, ZB_RAW_COMMAND } zb_raw_op_t;

typedef struct {
    esp_err_t err;               // ESP_OK: answered; ESP_ERR_TIMEOUT: no answer in 4 s; ESP_ERR_NOT_FOUND:
                                 // no such device; ESP_ERR_NOT_SUPPORTED: refused (see zcl_status); other:
                                 // not sent
    uint8_t zcl_status;          // write / command / refusal: the device's ZCL status (0 = success)
    uint8_t n;                   // read: one entry per attribute in the answer
    struct {
        uint16_t attr;
        uint8_t status;          // 0 = success, else the ZCL status (0x86 = unsupported attribute)
        uint8_t type;            // ZCL data type (0x20 uint8, 0x42 string, ...)
        uint8_t len;
        uint8_t data[ZB_RAW_VALUE_MAX];  // as on the wire (little-endian; strings length-prefixed)
    } v[ZB_RAW_MAX_ATTRS];
} zb_raw_result_t;

// Called once per request, on the Zigbee task (or the caller's, when it can't be sent): copy and return.
typedef void (*zb_raw_cb_t)(const zb_raw_result_t *res, void *ctx);

typedef struct {
    zb_raw_op_t op;
    char device[ZB_NAME_LEN];    // name (case-insensitive) or IEEE hex
    uint8_t ep;                  // 0 = the endpoint with that cluster, else the first
    uint16_t cluster;
    uint16_t manuf;              // manufacturer code for manufacturer-specific attributes, 0 = none
    bool to_client;              // the device's client side of the cluster
    uint8_t n_attrs;             // read
    uint16_t attrs[ZB_RAW_MAX_ATTRS];
    uint16_t attr;               // write: attribute, ZCL type, value as on the wire (zb_raw_encode)
    uint8_t type, len;
    uint8_t value[ZB_RAW_VALUE_MAX];
    uint8_t cmd, payload_len;    // command
    uint8_t payload[ZB_RAW_PAYLOAD_MAX];
    zb_raw_cb_t cb;              // set by zb_raw_request
    void *ctx;
} zb_raw_req_t;

// Queues the request (any task). The callback always runs once when ESP_OK is returned.
esp_err_t zb_raw_request(const zb_raw_req_t *req, zb_raw_cb_t cb, void *ctx);
// Value helpers. number: integer types up to 48 bits. text: strings as text, octet strings and
// other types as hex. encode: a number (integer types) or a string (0x42 text, 0x41 hex) for a write.
bool zb_raw_value_number(uint8_t type, const uint8_t *data, uint8_t len, double *out);
void zb_raw_value_text(uint8_t type, const uint8_t *data, uint8_t len, char *out, size_t cap, bool *is_number,
                       double *num);
esp_err_t zb_raw_encode(uint8_t type, double num, const char *str, uint8_t *out, uint8_t *len);
int zb_raw_hex(const char *hex, uint8_t *out, size_t cap);  // bytes written, -1 on bad hex

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

// ---- per-device Home Assistant settings ---------------------------------------------------
// Entities the user adds to a device: any attribute, read by the hub every `interval_s` (sleepy
// devices: when heard from) and published to Home Assistant under that device. Persisted.
#define ZB_CUSTOM_MAX     8
#define ZB_CUSTOM_NAME    32
#define ZB_HA_HIDE_LEN    192
enum { ZB_CUSTOM_SENSOR = 0, ZB_CUSTOM_BINARY, ZB_CUSTOM_SWITCH };
enum { ZB_STATE_CLASS_NONE = 0, ZB_STATE_CLASS_MEASUREMENT, ZB_STATE_CLASS_TOTAL_INCREASING };
// Published as 0 while the device's own On/Off reports off: some plugs keep reporting the last
// reading after the relay opens
#define ZB_CUSTOM_ZERO_WHEN_OFF 0x01
typedef struct {
    char name[ZB_CUSTOM_NAME];
    uint8_t kind;                // ZB_CUSTOM_*
    uint8_t ep;                  // 0 = the endpoint with that cluster
    uint8_t type;                // ZCL type of the attribute (writes use it)
    uint16_t cluster, attr, manuf;
    float scale;                 // sensor: value = raw / scale (0 = 1)
    char unit[12];
    char device_class[16];       // Home Assistant device class ("power", "voltage", ...), "" = none
    uint8_t state_class;         // ZB_STATE_CLASS_*: sensors' statistics in Home Assistant
    uint8_t flags;               // ZB_CUSTOM_ZERO_WHEN_OFF
    uint16_t interval_s;         // 10..65535 (clamped)
    // live, not persisted
    bool has_value;
    double value;                // scaled (sensor) or 0/1 (binary, switch)
} zb_custom_t;

// Replaces a device's settings: `hide` = comma-separated keys of the hub's own entities not to
// publish (opaque here: the Home Assistant API defines them), `custom` = n entries. Any task.
esp_err_t zb_set_ha_config(const char *ieee_hex, const char *hide, const zb_custom_t *custom, int n);
// Switch entity: writes 0/1 to the attribute and reads it back. Any task.
esp_err_t zb_custom_write(const char *ieee_hex, int index, bool on);

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
    bool has_startup; uint8_t startup;       // StartUpOnOff: 0 off, 1 on, 2 toggle, 0xFF previous
    bool ewelink;                            // SONOFF eWeLink cluster: inching (zb_set_inching)
    bool has_inching; uint8_t inching; uint16_t inching_s;  // last set: ZB_INCHING_*, seconds
    char ha_hide[ZB_HA_HIDE_LEN];            // keys of the hub's own entities not to publish
    uint8_t n_custom;
    zb_custom_t custom[ZB_CUSTOM_MAX];       // the user's own entities
} zb_dev_info_t;

// Fills up to `max` devices; returns how many. Any task (takes the registry lock briefly).
int zb_snapshot(zb_dev_info_t *out, int max);

// Current value of `attr` on the device named `device` (name, case-insensitive, or
// IEEE hex). Last known value, kept across restarts (zb_dev.c persists it).
// ESP_ERR_NOT_FOUND: no such device; ESP_ERR_INVALID_STATE: never reported.
esp_err_t zb_get_attr(const char *device, const char *attr, double *value, bool *is_bool);
