// Shared between zb.c (stack, signals) and zb_dev.c (registry, interview).
// Everything named zb_dev_on_* runs on the Zigbee task.
#pragma once

#include <stdint.h>
#include "esp_zigbee.h"

#define ZB_HUB_EP 1

esp_err_t zb_dev_init(void);                  // load the registry (before the stack starts)
void zb_dev_on_announce(uint16_t short_addr, uint64_t ieee, uint8_t capability);
void zb_dev_on_joined(uint16_t short_addr, uint64_t ieee, uint8_t capability);
void zb_dev_on_neighbor(uint16_t short_addr, uint64_t ieee, uint8_t capability);
void zb_dev_on_leave(uint64_t ieee);
void zb_dev_on_zcl(ezb_zcl_core_action_callback_id_t id, void *message);
void zb_dev_tick(void *arg);                  // 1 s, posted to the Zigbee task
void zb_dev_notify(void);                     // something changed: tell the UI
void zb_dev_set_my_ieee(uint64_t ieee);
bool zb_dev_lookup_short(const char *ieee_hex, uint16_t *short_addr, uint64_t *ieee);
void zb_dev_forget(uint64_t ieee);
void zb_dev_reinterview(uint64_t ieee);
void zb_dev_onoff_sent(uint16_t short_addr, uint8_t ep);  // Zigbee task
esp_err_t zb_dev_target(const char *device, uint16_t cluster, uint16_t *short_addr, uint8_t *ep);  // any task
