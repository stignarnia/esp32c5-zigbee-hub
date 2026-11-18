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

// zb_dev.c for zb_ota.c. Unknown manufacturer code / image type = 0xFFFF, file version
// 0xFFFFFFFF. False: no such device, or it has no OTA client cluster. Any task.
bool zb_dev_ota_info(uint64_t ieee, uint16_t *short_addr, uint8_t *ota_ep, bool *sleepy, uint16_t *manuf,
                     uint16_t *image_type, uint32_t *file_ver);
// What a device said it runs in its Query Next Image. Zigbee task.
void zb_dev_ota_query(uint16_t short_addr, uint8_t ep, uint16_t manuf, uint16_t image_type, uint32_t file_ver);
void zb_dev_ota_done(uint16_t short_addr);  // updated: read its version again once it's back. Zigbee task
// Offers (persisted with the device). Any task.
esp_err_t zb_dev_set_offer(uint64_t ieee, uint32_t file_ver, const char *url, const char *sha512, const char *notes);
bool zb_dev_get_offer(uint64_t ieee, char *url, size_t url_cap, char *sha512, size_t sha_cap);

// Address and endpoint for a request on `cluster` (server side, or client side): the endpoint
// with that cluster, else the first. ep NULL = only the address. Any task.
bool zb_dev_resolve(const char *device, uint16_t cluster, bool client, uint16_t *short_addr, uint8_t *ep);

// zb_raw.c (Zigbee task unless noted)
esp_err_t zb_raw_init(void);                // any task, before the stack starts
void zb_raw_on_read_rsp(const ezb_zcl_cmd_read_attr_rsp_message_t *m);
void zb_raw_on_write_rsp(const ezb_zcl_cmd_write_attr_rsp_message_t *m);
void zb_raw_on_default_rsp(const ezb_zcl_cmd_default_rsp_message_t *m);
void zb_raw_tick(void);                     // 1 s: timeouts

// zb_ota.c (Zigbee task unless noted)
esp_err_t zb_ota_init(void);                // before the stack starts. Any task
bool zb_ota_on_raw(const ezb_zcl_raw_frame_t *f);
void zb_ota_on_server_progress(ezb_zcl_ota_upgrade_server_progress_message_t *m);
void zb_ota_heard(uint16_t short_addr);     // a device was heard from (registry lock may be held)
void zb_ota_tick(void);                     // 1 s
