// The hub's web UI and REST/WebSocket API (port 80, all interfaces).
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    const char *hostname;
    const char *admin_hash;   // "pbkdf2_sha256$iters$salt_b64$hash_b64" from prov
    const char *lan_reset;    // prov nonce from flash.sh --lan-access ("" or NULL = none)
    void (*lan_cb)(bool lan_open);  // LAN side opened/closed (tailnet-only mode); may be NULL
} web_config_t;

esp_err_t web_start(const web_config_t *cfg);
bool web_is_running(void);
// False while tailnet-only access keeps LAN clients out (see web/access.c).
bool web_lan_open(void);
