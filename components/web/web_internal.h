#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

// ---- auth.c ----
esp_err_t auth_init(const char *admin_hash);
bool auth_has_password(void);
// Returns ESP_OK and fills token_hex (65 bytes) on success;
// ESP_ERR_INVALID_STATE when rate-limited (retry_after_s set).
// Password check with the login lockout (3 free attempts, then 2…60 s), no session.
esp_err_t auth_verify(const char *password, int *retry_after_s);
esp_err_t auth_login(const char *password, char token_hex[65], int *retry_after_s);
bool auth_check_request(httpd_req_t *req);  // valid session cookie?
void auth_logout_request(httpd_req_t *req);

// ---- web_api.c ----
esp_err_t api_register(httpd_handle_t server);
char *api_status_json(void);  // malloc'd; caller frees
char *api_zb_json(void);      // devices + network snapshot, {"type":"zb",...}; caller frees (cJSON_free)

// ---- web_auto.c ----
esp_err_t auto_api_register(httpd_handle_t server);

// ---- web_motion.c ----
esp_err_t motion_api_register(httpd_handle_t server);

// ---- web_server.c helpers used by the API ----
esp_err_t web_send_json(httpd_req_t *req, const char *status, const char *json);
esp_err_t web_send_error(httpd_req_t *req, const char *status, const char *msg);
// Requires the session cookie and, for POST, the anti-CSRF header. Sends the
// error response itself and returns false when the request must stop.
bool web_guard(httpd_req_t *req);
// Reads a small request body (<= cap-1 bytes) as a NUL-terminated string.
esp_err_t web_read_body(httpd_req_t *req, char *buf, size_t cap);
const char *web_hostname(void);

#define WEB_SESSION_COOKIE "hub_session"
