// Wi-Fi station locked to 5 GHz. 2.4 GHz belongs to Zigbee: no fallback, ever.
//
// Two saved networks, primary and backup (e.g. a phone hotspot). If the current
// one has been unreachable for 2 min, the other is tried, alternating. That alone
// makes "move the hub to a new router remotely" work: switch to the hotspot, fix
// the primary, turn the hotspot off, and it returns to the primary by itself.
//
// Recovery when no network is reachable:
//   reconnect with backoff (1 s .. 30 s)
//   -> driver restart every 5 min of continuous downtime
//   -> after 3 driver restarts, a reboot once the downtime passes 20 / 30 / 60 min
//      (consecutive watchdog reboots in one outage), and only when the reboot gate
//      says it is safe. The count survives the soft reset (RTC memory).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define WIFI5G_MAX_NETS 2   // [0] primary, [1] backup

typedef struct {
    char ssid[33];
    char psk[65];
} wifi5g_net_t;

typedef enum {
    WIFI5G_TRIAL_NONE = 0,      // no switch requested since boot
    WIFI5G_TRIAL_RUNNING,       // switched, waiting for an IP
    WIFI5G_TRIAL_OK,            // got an IP: new networks saved
    WIFI5G_TRIAL_REVERTED,      // no IP in time: previous networks restored
} wifi5g_trial_t;

typedef struct {
    bool connected;
    uint8_t net;                // index of the network in use / being tried
    char ssid[33];              // of that network
    uint8_t trial;              // wifi5g_trial_t of the last requested switch
    uint8_t trial_target;
    bool custom;                // networks come from the UI (NVS), not from flash.sh
    uint32_t failovers;         // network switches since boot
    uint8_t channel;            // primary channel of the current AP, 0 if not connected
    int8_t rssi;
    uint32_t ip;                // network byte order, 0 if none
    int64_t first_ip_ms;        // ms since boot of the first IP, -1 if never
    uint32_t connects;          // successful GOT_IP count since boot
    uint32_t disconnects;       // DISCONNECTED events since boot
    uint32_t no_ap_found;       // of which reason 201 (the C5's known failure mode)
    uint8_t last_reason;
    uint32_t driver_restarts;   // since boot
    uint32_t watchdog_reboots;  // consecutive watchdog reboots in the current outage
} wifi5g_stats_t;

// Returns true when a reboot would not interrupt anything important.
typedef bool (*wifi5g_reboot_gate_t)(void);

// Starts the station. `defaults` are the flash.sh networks (prov); networks saved
// from the UI (app NVS) take precedence when present. defaults[0] is required;
// [1] (backup) is used if its ssid is non-empty. country "" keeps the default.
// The app NVS partition must be initialised first.
esp_err_t wifi5g_start(const wifi5g_net_t defaults[WIFI5G_MAX_NETS], const char *hostname,
                       const char *country);

// Current networks (passwords included: callers must not expose them).
void wifi5g_get_nets(wifi5g_net_t out[WIFI5G_MAX_NETS]);

// Replaces the networks and connects to `target`, on trial: with no IP within
// trial_s seconds the previous networks and network are restored. Only a
// successful trial saves to NVS. ESP_ERR_INVALID_STATE while a trial is running.
esp_err_t wifi5g_apply(const wifi5g_net_t nets[WIFI5G_MAX_NETS], uint8_t target,
                       uint32_t trial_s);

// Same trial, back to the flash.sh networks; success erases the UI override.
esp_err_t wifi5g_reset_to_defaults(uint32_t trial_s);

// Disconnects cleanly so the AP drops our association before a planned reboot
// (avoids the stale-association login delay on the next boot).
void wifi5g_prepare_reboot(void);

// Later phases install a gate (automation timers, pairing, OTA). NULL = always safe.
void wifi5g_set_reboot_gate(wifi5g_reboot_gate_t gate);

void wifi5g_get_stats(wifi5g_stats_t *out);
bool wifi5g_is_connected(void);

// Name of a Wi-Fi disconnect reason code ("AUTH_EXPIRE", "NO_AP_FOUND", ... or "other").
const char *wifi5g_reason_str(uint8_t reason);
