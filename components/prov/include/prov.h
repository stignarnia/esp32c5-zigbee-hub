// Provisioning data written by flash.sh into the dedicated "prov" NVS partition.
// Read-only on the board: re-provisioning never touches Zigbee or app settings.
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    char ssid[33];
    char psk[65];          // never log this
    char ssid2[33];        // backup network (optional), e.g. a phone hotspot
    char psk2[65];
    char hostname[33];
    char country[3];       // ISO 3166 alpha-2 from the flashing machine, "" = keep default
    char admin_hash[128];  // "pbkdf2_sha256$<iters>$<salt_b64>$<hash_b64>", checked in Phase 2
    char lan_reset[33];    // nonce from flash.sh --lan-access: turns tailnet-only access off once
    bool has_wifi;         // ssid present
} prov_t;

// Loads the prov partition. Missing optional keys are left empty.
// Returns ESP_ERR_NOT_FOUND if the partition was never written.
esp_err_t prov_load(prov_t *out);
