// REST API. All handlers run on the httpd task.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hubsys.h"
#include "web_internal.h"
#include "wifi5g.h"

static const char *TAG = "web.api";

#define WIFI_TRIAL_S 60

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "power-on";
    case ESP_RST_EXT:        return "external pin";
    case ESP_RST_SW:         return "software restart";
    case ESP_RST_PANIC:      return "crash (panic)";
    case ESP_RST_INT_WDT:    return "interrupt watchdog";
    case ESP_RST_TASK_WDT:   return "task watchdog";
    case ESP_RST_WDT:        return "other watchdog";
    case ESP_RST_DEEPSLEEP:  return "deep sleep wake";
    case ESP_RST_BROWNOUT:   return "brownout (power supply)";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    default:                 return "unknown";
    }
}

static const char *trial_str(uint8_t t)
{
    switch (t) {
    case WIFI5G_TRIAL_RUNNING:  return "running";
    case WIFI5G_TRIAL_OK:       return "ok";
    case WIFI5G_TRIAL_REVERTED: return "reverted";
    default:                    return "none";
    }
}

// ---- status (also pushed over the WebSocket every 2 s) ------------------------------

char *api_status_json(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "hostname", web_hostname());
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));

    cJSON *fw = cJSON_AddObjectToObject(o, "firmware");
    cJSON_AddStringToObject(fw, "version", app->version);
    cJSON_AddStringToObject(fw, "build_id", hubsys_build_id());
    cJSON_AddStringToObject(fw, "idf", app->idf_ver);
    char built[40];
    snprintf(built, sizeof(built), "%s %s", app->date, app->time);
    cJSON_AddStringToObject(fw, "built", built);
    cJSON_AddStringToObject(fw, "partition", esp_ota_get_running_partition()->label);
    cJSON_AddBoolToObject(fw, "on_trial", hubsys_ota_pending_verify());
    hubsys_ota_result_t r;
    hubsys_ota_last_result(&r);
    cJSON *ota = cJSON_AddObjectToObject(fw, "last_update");
    cJSON_AddStringToObject(ota, "result", r.kind);
    cJSON_AddStringToObject(ota, "from", r.from_id);
    cJSON_AddStringToObject(ota, "to", r.to_id);
    cJSON_AddStringToObject(ota, "to_version", r.to_version);

    cJSON *boot = cJSON_AddObjectToObject(o, "boot");
    cJSON_AddStringToObject(boot, "reset_reason", reset_reason_str(esp_reset_reason()));
    cJSON_AddStringToObject(boot, "planned_reason", hubsys_last_planned_reason());

    cJSON *mem = cJSON_AddObjectToObject(o, "memory");
    cJSON_AddNumberToObject(mem, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(mem, "internal_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(mem, "internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(mem, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(mem, "psram_min", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));

    wifi5g_stats_t w;
    wifi5g_get_stats(&w);
    cJSON *wi = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddBoolToObject(wi, "connected", w.connected);
    cJSON_AddStringToObject(wi, "network", w.net == 0 ? "primary" : "backup");
    cJSON_AddStringToObject(wi, "ssid", w.ssid);
    cJSON_AddNumberToObject(wi, "channel", w.channel);
    cJSON_AddBoolToObject(wi, "dfs", w.channel >= 52 && w.channel <= 144);
    cJSON_AddNumberToObject(wi, "rssi", w.rssi);
    char ip[16];
    esp_ip4_addr_t a = {.addr = w.ip};
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a));
    cJSON_AddStringToObject(wi, "ip", ip);
    cJSON_AddNumberToObject(wi, "first_ip_ms", (double)w.first_ip_ms);
    cJSON_AddNumberToObject(wi, "connects", w.connects);
    cJSON_AddNumberToObject(wi, "disconnects", w.disconnects);
    cJSON_AddNumberToObject(wi, "no_ap_found", w.no_ap_found);
    cJSON_AddNumberToObject(wi, "last_reason", w.last_reason);
    cJSON_AddNumberToObject(wi, "failovers", w.failovers);
    cJSON_AddNumberToObject(wi, "driver_restarts", w.driver_restarts);
    cJSON_AddNumberToObject(wi, "watchdog_reboots", w.watchdog_reboots);
    cJSON_AddStringToObject(wi, "trial", trial_str(w.trial));
    cJSON_AddBoolToObject(wi, "custom", w.custom);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macs[18];
    snprintf(macs, sizeof(macs), MACSTR, MAC2STR(mac));
    cJSON_AddStringToObject(wi, "mac", macs);

    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

static esp_err_t h_status(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    char *s = api_status_json();
    esp_err_t r = web_send_json(req, "200 OK", s ? s : "{}");
    free(s);
    return r;
}

// ---- session ---------------------------------------------------------------------------

static esp_err_t h_session(httpd_req_t *req)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"authed\":%s,\"hostname\":\"%s\"}",
             auth_check_request(req) ? "true" : "false", web_hostname());
    return web_send_json(req, "200 OK", buf);
}

static esp_err_t h_login(httpd_req_t *req)
{
    char v[4];
    if (httpd_req_get_hdr_value_str(req, "X-Hub", v, sizeof(v)) != ESP_OK) {
        return web_send_error(req, "403 Forbidden", "missing X-Hub header");
    }
    char body[256];
    if (web_read_body(req, body, sizeof(body)) != ESP_OK) {
        return web_send_error(req, "400 Bad Request", "bad body");
    }
    cJSON *j = cJSON_Parse(body);
    const cJSON *pw = cJSON_GetObjectItem(j, "password");
    if (!cJSON_IsString(pw)) {
        cJSON_Delete(j);
        return web_send_error(req, "400 Bad Request", "password required");
    }
    char token[65];
    int retry = 0;
    esp_err_t err = auth_login(pw->valuestring, token, &retry);
    cJSON_Delete(j);
    if (err == ESP_ERR_INVALID_STATE) {
        char ra[8];
        snprintf(ra, sizeof(ra), "%d", retry);
        httpd_resp_set_hdr(req, "Retry-After", ra);
        return web_send_error(req, "429 Too Many Requests", "too many attempts, wait a bit");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return web_send_error(req, "503 Service Unavailable", "no admin password provisioned");
    }
    if (err != ESP_OK) return web_send_error(req, "401 Unauthorized", "wrong password");

    char cookie[160];
    snprintf(cookie, sizeof(cookie),
             WEB_SESSION_COOKIE "=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=31536000", token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return web_send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_logout(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    auth_logout_request(req);
    httpd_resp_set_hdr(req, "Set-Cookie", WEB_SESSION_COOKIE "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return web_send_json(req, "200 OK", "{\"ok\":true}");
}

// ---- Wi-Fi networks ------------------------------------------------------------------------

static esp_err_t h_wifi_get(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    wifi5g_net_t nets[WIFI5G_MAX_NETS];
    wifi5g_get_nets(nets);
    wifi5g_stats_t w;
    wifi5g_get_stats(&w);
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "networks");
    for (int i = 0; i < WIFI5G_MAX_NETS; i++) {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "role", i == 0 ? "primary" : "backup");
        cJSON_AddStringToObject(n, "ssid", nets[i].ssid);
        cJSON_AddBoolToObject(n, "has_password", nets[i].psk[0] != '\0');  // never the password
        cJSON_AddItemToArray(arr, n);
    }
    cJSON_AddNumberToObject(o, "current", w.net);
    cJSON_AddBoolToObject(o, "connected", w.connected);
    cJSON_AddStringToObject(o, "trial", trial_str(w.trial));
    cJSON_AddBoolToObject(o, "custom", w.custom);
    cJSON_AddNumberToObject(o, "trial_seconds", WIFI_TRIAL_S);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    esp_err_t r = web_send_json(req, "200 OK", s ? s : "{}");
    free(s);
    return r;
}

static esp_err_t trial_response(httpd_req_t *req, esp_err_t err)
{
    if (err == ESP_ERR_INVALID_STATE) return web_send_error(req, "409 Conflict", "a switch is already on trial");
    if (err == ESP_ERR_INVALID_ARG) {
        return web_send_error(req, "400 Bad Request",
                              "network needs an SSID and a WPA2/WPA3 password (8-64 chars)");
    }
    if (err != ESP_OK) return web_send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"trial_seconds\":%d}", WIFI_TRIAL_S);
    return web_send_json(req, "202 Accepted", buf);
}

// {"networks":[{"ssid":"..","password":".."},{..}], "target":0|1}
// An empty/missing password keeps the stored one if the SSID didn't change.
static esp_err_t h_wifi_set(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    char body[512];
    if (web_read_body(req, body, sizeof(body)) != ESP_OK) return web_send_error(req, "400 Bad Request", "bad body");
    cJSON *j = cJSON_Parse(body);
    const cJSON *arr = cJSON_GetObjectItem(j, "networks");
    const cJSON *tgt = cJSON_GetObjectItem(j, "target");
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != WIFI5G_MAX_NETS || !cJSON_IsNumber(tgt)) {
        cJSON_Delete(j);
        return web_send_error(req, "400 Bad Request", "expected networks[2] and target");
    }
    wifi5g_net_t cur[WIFI5G_MAX_NETS], nets[WIFI5G_MAX_NETS] = {0};
    wifi5g_get_nets(cur);
    for (int i = 0; i < WIFI5G_MAX_NETS; i++) {
        const cJSON *n = cJSON_GetArrayItem(arr, i);
        const cJSON *ss = cJSON_GetObjectItem(n, "ssid"), *pw = cJSON_GetObjectItem(n, "password");
        if (cJSON_IsString(ss)) strlcpy(nets[i].ssid, ss->valuestring, sizeof(nets[i].ssid));
        if (cJSON_IsString(pw) && pw->valuestring[0]) {
            strlcpy(nets[i].psk, pw->valuestring, sizeof(nets[i].psk));
        } else if (strcmp(nets[i].ssid, cur[i].ssid) == 0) {
            strlcpy(nets[i].psk, cur[i].psk, sizeof(nets[i].psk));
        }
    }
    int target = tgt->valueint;
    cJSON_Delete(j);
    if (target < 0 || target >= WIFI5G_MAX_NETS) return web_send_error(req, "400 Bad Request", "bad target");
    if (nets[0].ssid[0] == '\0') return web_send_error(req, "400 Bad Request", "primary network required");
    ESP_LOGI(TAG, "wifi: new networks '%s' / '%s', switching to %s on trial", nets[0].ssid,
             nets[1].ssid, target ? "backup" : "primary");
    esp_err_t err = wifi5g_apply(nets, (uint8_t)target, WIFI_TRIAL_S);
    memset(nets, 0, sizeof(nets));
    memset(cur, 0, sizeof(cur));
    return trial_response(req, err);
}

static esp_err_t h_wifi_switch(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    char body[64];
    if (web_read_body(req, body, sizeof(body)) != ESP_OK) return web_send_error(req, "400 Bad Request", "bad body");
    cJSON *j = cJSON_Parse(body);
    const cJSON *tgt = cJSON_GetObjectItem(j, "target");
    int target = cJSON_IsNumber(tgt) ? tgt->valueint : -1;
    cJSON_Delete(j);
    if (target < 0 || target >= WIFI5G_MAX_NETS) return web_send_error(req, "400 Bad Request", "bad target");
    wifi5g_net_t nets[WIFI5G_MAX_NETS];
    wifi5g_get_nets(nets);
    esp_err_t err = wifi5g_apply(nets, (uint8_t)target, WIFI_TRIAL_S);
    memset(nets, 0, sizeof(nets));
    return trial_response(req, err);
}

static esp_err_t h_wifi_reset(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    return trial_response(req, wifi5g_reset_to_defaults(WIFI_TRIAL_S));
}

// ---- firmware ---------------------------------------------------------------------------------

static esp_err_t h_ota(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    esp_err_t err = hubsys_ota_begin(req->content_len);
    if (err == ESP_ERR_INVALID_SIZE) return web_send_error(req, "413 Content Too Large", "image size out of range");
    if (err == ESP_ERR_INVALID_STATE) {
        return web_send_error(req, "409 Conflict", "an update is running, or the current firmware is still on trial");
    }
    if (err != ESP_OK) return web_send_error(req, "500 Internal Server Error", esp_err_to_name(err));

    const size_t chunk = 4096;
    char *buf = heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        hubsys_ota_abort();
        return web_send_error(req, "500 Internal Server Error", "out of memory");
    }
    size_t left = req->content_len;
    int64_t t0 = esp_timer_get_time();
    const char *fail = NULL;
    while (left > 0) {
        int r = httpd_req_recv(req, buf, left < chunk ? left : chunk);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            fail = "upload interrupted";
            break;
        }
        // The first write needs the image header + app description in one piece.
        if (left == req->content_len && r < 512 && left >= 512) {
            int more = httpd_req_recv(req, buf + r, 512 - r);
            if (more > 0) r += more;
        }
        err = hubsys_ota_write(buf, r);
        if (err == ESP_ERR_INVALID_ARG) {
            fail = "not a firmware image for this hub (wrong chip or project)";
            break;
        }
        if (err != ESP_OK) {
            fail = "flash write failed";
            break;
        }
        left -= r;
    }
    free(buf);
    if (fail) {
        hubsys_ota_abort();
        return web_send_error(req, "400 Bad Request", fail);
    }
    err = hubsys_ota_finish();
    if (err != ESP_OK) return web_send_error(req, "400 Bad Request", "image failed validation");

    ESP_LOGI(TAG, "ota: %u bytes in %lld ms", (unsigned)req->content_len,
             (esp_timer_get_time() - t0) / 1000);
    web_send_json(req, "200 OK", "{\"ok\":true,\"rebooting\":true}");
    hubsys_reboot("firmware update", 800);
    return ESP_OK;
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    web_send_json(req, "200 OK", "{\"ok\":true}");
    hubsys_reboot("requested from the web UI", 500);
    return ESP_OK;
}

// ---- registration -------------------------------------------------------------------------

esp_err_t api_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/session", .method = HTTP_GET, .handler = h_session},
        {.uri = "/api/login", .method = HTTP_POST, .handler = h_login},
        {.uri = "/api/logout", .method = HTTP_POST, .handler = h_logout},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = h_wifi_get},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi_set},
        {.uri = "/api/wifi/switch", .method = HTTP_POST, .handler = h_wifi_switch},
        {.uri = "/api/wifi/reset", .method = HTTP_POST, .handler = h_wifi_reset},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = h_ota},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
