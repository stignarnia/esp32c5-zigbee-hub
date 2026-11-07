// Tailscale node (microlink port). Never on the path of Zigbee or automations: it
// starts after Wi-Fi has an address, on its own tasks, and if it fails nothing else
// notices.
//
// Settings live in the app NVS (namespace "ts"), set from the web UI:
//   - enabled (default on)
//   - the auth key, kept only until the first successful registration — after that
//     the node key (NVS namespace "microlink") identifies the hub, and no reusable
//     secret stays on the board
// The tailnet name is the hub's hostname.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define TS_MAX_PEERS 16

typedef struct {
    bool enabled;
    bool suspended;         // stopped by the crash guard until turned on again
    bool registered;        // has a node key the tailnet accepted
    bool key_pending;       // an auth key is stored, waiting to be used
    const char *state;      // "off", "suspended", "needs an auth key", "waiting for Wi-Fi", "connecting",
                            // "registering", "connected", "reconnecting", "error"
    char error[96];         // why registration was refused, "" = no problem
    char name[96];          // MagicDNS name, "" until known
    uint32_t ip;            // tailnet IPv4, host byte order, 0 = none yet
    int64_t key_expiry;     // Unix seconds, 0 = never / unknown
    bool key_expired;
    int64_t connected_ms;   // uptime (ms) when first connected, -1 = not yet
    int peer_count;
    char home_derp[8];      // our home DERP region code, "" = not connected
} ts_status_t;

typedef struct {
    char name[64];
    uint32_t ip;            // host byte order
    int8_t online;          // control server's view: 1 online, 0 offline, -1 unknown
    bool direct;            // we have a direct UDP path to it (else traffic goes via DERP)
    char derp[8];           // DERP region code of its home ("fra"), "" = unknown
} ts_peer_t;

// Loads the settings and, if enabled and there is a key or a node key, starts the
// node in the background (it waits for Wi-Fi itself). Call once at boot.
esp_err_t ts_init(const char *hostname);

void ts_get_status(ts_status_t *out);
int ts_get_peers(ts_peer_t *out, int max);  // returns the number filled

// Settings. *reboot_needed is set when the change can only take effect after a
// restart (stopping a running node); the caller decides when to reboot.
// ESP_ERR_INVALID_ARG: not a "tskey-…" key. ESP_ERR_INVALID_STATE: already
// registered (log out first).
esp_err_t ts_set_auth_key(const char *key);
esp_err_t ts_set_enabled(bool on, bool *reboot_needed);
// Forgets the node key and any stored auth key: the next key registers a new node.
// The old node stays in the admin console (as offline) until deleted there.
esp_err_t ts_logout(bool *reboot_needed);
// Diagnostics: microlink DEBUG logs on the console for `seconds`, then back to INFO.
void ts_debug_logs(int seconds);
// A UI client is connected (WebSocket open): peer paths are probed for it even when
// idle. Without one, only peers with recent traffic are probed.
void ts_set_ui_watching(bool watching);

// Our tailnet IPv4 (host byte order), 0 until Tailscale has connected once.
uint32_t ts_tailnet_ip(void);
// Packets for the tailnet IP dropped because they arrived on Wi-Fi (lwIP hook).
uint32_t ts_guard_drops(void);
