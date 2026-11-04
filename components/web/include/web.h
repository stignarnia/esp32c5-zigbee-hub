// The hub's web UI and REST/WebSocket API (port 80, all interfaces).
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    const char *hostname;
    const char *admin_hash;   // "pbkdf2_sha256$iters$salt_b64$hash_b64" from prov
} web_config_t;

esp_err_t web_start(const web_config_t *cfg);
bool web_is_running(void);
