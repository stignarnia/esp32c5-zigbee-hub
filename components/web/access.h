// "Tailnet only, no password" access mode. See access.c.
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

// prov_lan_reset: nonce from the prov partition ("" = none). lan_cb: called when the
// LAN side opens or closes (main uses it for mDNS); may be NULL.
void access_init(const char *prov_lan_reset, void (*lan_cb)(bool lan_open));

bool access_tailnet_only_setting(void);  // the saved setting
bool access_lan_open(void);              // LAN connections accepted right now
int access_lan_window_s(void);           // seconds left of the BOOT-button window, 0 = none
bool access_fd_via_tailnet(int fd);      // connection from 100.64/10 to our tailnet IP
bool access_passwordless(httpd_req_t *req);  // mode on and this request came over Tailscale
esp_err_t access_open_fn(httpd_handle_t hd, int fd);  // httpd open_fn
esp_err_t access_set_tailnet_only(bool on);
