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
