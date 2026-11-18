// Home Assistant integration: the ESPHome native API (TCP 6053, Noise encryption only).
//
// Off by default. When on, Home Assistant sees the hub as an ESPHome device:
//   - every Zigbee device as a sub-device with its own entities (contact, battery, …)
//   - the hub's own entities (permit join, pairing, Zigbee channel, devices, Wi-Fi signal,
//     uptime), and its Wi-Fi motion sensing as a sub-device "Motion sensor"
//   - each script as a sub-device: an "Enabled" switch plus what it publishes (ha.*)
// A consumer of the registry like the web UI: it never blocks Zigbee (reads snapshots,
// the Zigbee callback only wakes it), and with the setting off it costs no RAM (the task,
// sockets and entity table exist only while it is on).
//
// Outgoing connection: a client that asks for it (Home Assistant does) is dialed back on
// TCP 6054 whenever none is connected, so it returns seconds after a hub restart.
//
// Settings: app NVS namespace "ha" (on/off, the 32-byte Noise key, the dial-back address). `flash.sh --ha`
// writes a nonce + key into the prov partition; a nonce not seen before turns the
// integration on with that key once, after which the UI's choice holds again.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"
#include "zb.h"

#define HA_API_PORT 6053

typedef struct {
    bool enabled;            // the setting
    bool running;            // server listening
    int clients;             // connected and through the handshake
    int entities;
    int devices;             // sub-devices (Zigbee devices + scripts)
    char key[45];            // base64 Noise PSK, "" = none yet
    char client[48];         // what the last client called itself ("Home Assistant 2026.11.1")
    uint32_t connects;       // handshakes completed since boot
    uint32_t rejected;       // handshakes refused (wrong key, plaintext client, filter)
    char dial_target[16];    // where the hub dials Home Assistant back (TCP 6054), "" = nobody asked
} ha_status_t;

// Loads the settings, applies a new prov nonce (prov_nonce "" = none), and starts the
// server if on. Call after zb_start(), automation_start() and motion_init().
esp_err_t ha_init(const char *hostname, const char *prov_nonce, const char *prov_key);

void ha_get_status(ha_status_t *out);

// Turning on the first time generates a key. Off closes every client and frees it all.
esp_err_t ha_set_enabled(bool on);
// New random key; connected clients are closed (Home Assistant needs the new key).
esp_err_t ha_new_key(void);

// Connections the server may accept (tailnet-only access mode: tailnet only). Called
// right after accept(); NULL = all.
void ha_set_conn_filter(bool (*allowed)(int fd));

// mDNS was just started (or restarted): advertise _esphomelib._tcp if the server runs.
void ha_mdns_announce(void);

// The hub's own Home Assistant entities for one Zigbee device, whether hidden or not, as
// [{"key", "name", "kind"}]: "key" is what zb_set_ha_config's hide list holds. Works with the
// integration off (the per-device settings are kept for when it's on). Caller frees.
cJSON *ha_device_builtin_json(const zb_dev_info_t *d);
