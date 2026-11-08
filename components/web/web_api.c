// REST API. All handlers run on the httpd task.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "zb.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hublog.h"
#include "hubsys.h"
#include "tailscale.h"
#include "web_internal.h"
#include "access.h"
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

static void add_ts_status(cJSON *o, const ts_status_t *t)
{
    char ip[16] = "";
    if (t->ip) {
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", (unsigned)(t->ip >> 24), (unsigned)(t->ip >> 16 & 0xff),
                 (unsigned)(t->ip >> 8 & 0xff), (unsigned)(t->ip & 0xff));
    }
    cJSON_AddBoolToObject(o, "enabled", t->enabled);
    cJSON_AddBoolToObject(o, "suspended", t->suspended);
    cJSON_AddBoolToObject(o, "registered", t->registered);
    cJSON_AddBoolToObject(o, "key_pending", t->key_pending);
    cJSON_AddStringToObject(o, "state", t->state);
    cJSON_AddStringToObject(o, "error", t->error);
    cJSON_AddStringToObject(o, "name", t->name);
    cJSON_AddStringToObject(o, "ip", ip);
    cJSON_AddNumberToObject(o, "key_expiry", (double)t->key_expiry);
    cJSON_AddBoolToObject(o, "key_expired", t->key_expired);
    cJSON_AddNumberToObject(o, "connected_ms", (double)t->connected_ms);
    cJSON_AddNumberToObject(o, "peers", t->peer_count);
    cJSON_AddStringToObject(o, "home_derp", t->home_derp);
}

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
    cJSON_AddStringToObject(wi, "last_reason_name", wifi5g_reason_str(w.last_reason));
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

    ts_status_t t;
    ts_get_status(&t);
    cJSON *ts = cJSON_AddObjectToObject(o, "tailscale");
    add_ts_status(ts, &t);

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
             (access_passwordless(req) || auth_check_request(req)) ? "true" : "false", web_hostname());
    return web_send_json(req, "200 OK", buf);
}

static esp_err_t h_login(httpd_req_t *req)
{
    char v[4];
    if (httpd_req_get_hdr_value_str(req, "X-Hub", v, sizeof(v)) != ESP_OK) {
        return web_send_error(req, "403 Forbidden", "missing X-Hub header");
    }
    // Tailnet-only mode: being on the tailnet is the login (flash.sh --ota still posts one).
    if (access_passwordless(req)) return web_send_json(req, "200 OK", "{\"ok\":true}");
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

// ---- Tailscale ----------------------------------------------------------------------------------

static esp_err_t h_ts_get(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    ts_status_t t;
    ts_get_status(&t);
    cJSON *o = cJSON_CreateObject();
    add_ts_status(o, &t);
    static ts_peer_t peers[TS_MAX_PEERS];  // httpd task only
    int n = ts_get_peers(peers, TS_MAX_PEERS);
    cJSON *arr = cJSON_AddArrayToObject(o, "peer_list");
    for (int i = 0; i < n; i++) {
        cJSON *p = cJSON_CreateObject();
        char ip[16];
        uint32_t a = peers[i].ip;
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)(a >> 16 & 0xff),
                 (unsigned)(a >> 8 & 0xff), (unsigned)(a & 0xff));
        cJSON_AddStringToObject(p, "name", peers[i].name);
        cJSON_AddStringToObject(p, "ip", ip);
        if (peers[i].online < 0) cJSON_AddNullToObject(p, "online");
        else cJSON_AddBoolToObject(p, "online", peers[i].online);
        cJSON_AddBoolToObject(p, "direct", peers[i].direct);
        cJSON_AddStringToObject(p, "derp", peers[i].derp);
        cJSON_AddItemToArray(arr, p);
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    esp_err_t r = web_send_json(req, "200 OK", s ? s : "{}");
    free(s);
    return r;
}

// {"key":"tskey-auth-..."} — write-only: the key is never sent back.
static esp_err_t h_ts_key(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    char body[256];
    if (web_read_body(req, body, sizeof(body)) != ESP_OK) return web_send_error(req, "400 Bad Request", "bad body");
    cJSON *j = cJSON_Parse(body);
    memset(body, 0, sizeof(body));
    const cJSON *k = cJSON_GetObjectItem(j, "key");
    esp_err_t err = cJSON_IsString(k) ? ts_set_auth_key(k->valuestring) : ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(k)) memset(k->valuestring, 0, strlen(k->valuestring));
    cJSON_Delete(j);
    if (err == ESP_ERR_INVALID_ARG) return web_send_error(req, "400 Bad Request", "not a Tailscale auth key (tskey-auth-…)");
    if (err == ESP_ERR_INVALID_STATE) return web_send_error(req, "409 Conflict", "already logged in: log out first");
    if (err != ESP_OK) return web_send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    return web_send_json(req, "200 OK", "{\"ok\":true}");
}

// Turning off or logging out a running node is applied by a restart (see ts.c).
static esp_err_t ts_reply(httpd_req_t *req, esp_err_t err, bool reboot, const char *why)
{
    if (err != ESP_OK) return web_send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    web_send_json(req, "200 OK", reboot ? "{\"ok\":true,\"restarting\":true}" : "{\"ok\":true}");
    if (reboot) hubsys_reboot(why, 800);  // >= 800 ms: let the reply reach the browser
    return ESP_OK;
}

// {"on":true|false}
static esp_err_t h_ts_enabled(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    char body[64];
    if (web_read_body(req, body, sizeof(body)) != ESP_OK) return web_send_error(req, "400 Bad Request", "bad body");
    cJSON *j = cJSON_Parse(body);
    const cJSON *on = cJSON_GetObjectItem(j, "on");
    if (!cJSON_IsBool(on)) {
        cJSON_Delete(j);
        return web_send_error(req, "400 Bad Request", "expected {\"on\":true|false}");
    }
    bool reboot = false;
    esp_err_t err = ts_set_enabled(cJSON_IsTrue(on), &reboot);
    cJSON_Delete(j);
    return ts_reply(req, err, reboot, "Tailscale turned off from the web UI");
}

static esp_err_t h_ts_logout(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    bool reboot = false;
    esp_err_t err = ts_logout(&reboot);
    return ts_reply(req, err, reboot, "Tailscale logged out from the web UI");
}

// Diagnostics: microlink's per-packet DEBUG lines on the serial console for 10 minutes.
static esp_err_t h_ts_debug(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    ts_debug_logs(10 * 60);
    return web_send_json(req, "200 OK", "{\"debug_s\":600}");
}

// ---- access mode ("tailnet only, no password") -------------------------------------------

static esp_err_t access_reply(httpd_req_t *req)
{
    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"tailnet_only\":%s,\"lan_open\":%s,\"lan_window_s\":%d,\"via_tailnet\":%s,"
             "\"tailscale_up\":%s,\"forged_dropped\":%lu}",
             access_tailnet_only_setting() ? "true" : "false",
             access_lan_open() ? "true" : "false", access_lan_window_s(),
             access_fd_via_tailnet(httpd_req_to_sockfd(req)) ? "true" : "false",
             ts_tailnet_ip() ? "true" : "false", (unsigned long)ts_guard_drops());
    return web_send_json(req, "200 OK", buf);
}

static esp_err_t h_access_get(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    return access_reply(req);
}

// {"tailnet_only":true,"password":"…"} — turning it on needs the password once more,
// a working Tailscale connection, and must itself come over Tailscale: proof that the
// tailnet path works before the LAN is cut off. Turning it off needs only the usual
// login (or the tailnet, in this mode).
static esp_err_t h_access_set(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    char body[256];
    if (web_read_body(req, body, sizeof(body)) != ESP_OK) return web_send_error(req, "400 Bad Request", "bad body");
    cJSON *j = cJSON_Parse(body);
    memset(body, 0, sizeof(body));
    const cJSON *on = cJSON_GetObjectItem(j, "tailnet_only");
    const cJSON *pw = cJSON_GetObjectItem(j, "password");
    if (!cJSON_IsBool(on)) {
        cJSON_Delete(j);
        return web_send_error(req, "400 Bad Request", "tailnet_only must be true or false");
    }
    if (cJSON_IsTrue(on)) {
        if (!ts_tailnet_ip()) {
            cJSON_Delete(j);
            return web_send_error(req, "409 Conflict", "Tailscale isn't connected");
        }
        if (!access_fd_via_tailnet(httpd_req_to_sockfd(req))) {
            cJSON_Delete(j);
            return web_send_error(req, "409 Conflict",
                                  "open the UI over Tailscale (http://<hub>/ on the tailnet) to switch this on");
        }
        int retry = 0;
        esp_err_t err = cJSON_IsString(pw) ? auth_verify(pw->valuestring, &retry) : ESP_FAIL;
        if (cJSON_IsString(pw)) memset(pw->valuestring, 0, strlen(pw->valuestring));
        cJSON_Delete(j);
        if (err == ESP_ERR_INVALID_STATE) return web_send_error(req, "429 Too Many Requests", "too many attempts, wait a bit");
        if (err != ESP_OK) return web_send_error(req, "401 Unauthorized", "wrong password");
        access_set_tailnet_only(true);
    } else {
        cJSON_Delete(j);
        access_set_tailnet_only(false);
    }
    return access_reply(req);
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

// ---- diagnostic log ------------------------------------------------------------------

static esp_err_t send_file(httpd_req_t *req, const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return ESP_OK;  // not there yet: nothing to send
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(buf, 1, cap, f)) > 0) {
        err = httpd_resp_send_chunk(req, buf, n);
        if (err != ESP_OK) break;
    }
    fclose(f);
    return err;
}

static esp_err_t h_logs(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    hublog_flush();  // include what's still buffered
    char disp[96];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s-log.txt\"", web_hostname());
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const size_t cap = 4096;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return web_send_error(req, "500 Internal Server Error", "out of memory");
    esp_err_t err = send_file(req, HUBLOG_OLD, buf, cap);
    if (err == ESP_OK) err = send_file(req, HUBLOG_CUR, buf, cap);
    free(buf);
    if (err != ESP_OK) return err;
    return httpd_resp_send_chunk(req, NULL, 0);
}

// ---- Zigbee ---------------------------------------------------------------------------------

static esp_err_t send_cjson(httpd_req_t *req, cJSON *j)
{
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) return web_send_error(req, "500 Internal Server Error", "out of memory");
    esp_err_t err = web_send_json(req, "200 OK", txt);
    cJSON_free(txt);
    return err;
}

static cJSON *zb_snapshot(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "zb");
    cJSON_AddItemToObject(o, "network", zb_network_json());
    cJSON_AddNumberToObject(o, "rev", zb_revision());
    cJSON_AddItemToObject(o, "devices", zb_devices_json());
    return o;
}

char *api_zb_json(void)
{
    cJSON *o = zb_snapshot();
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return txt;
}

static esp_err_t h_zb_get(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    return send_cjson(req, zb_snapshot());
}

// Small JSON body -> cJSON (NULL on error; the caller replies 400).
static cJSON *read_json(httpd_req_t *req)
{
    char body[160];
    if (web_read_body(req, body, sizeof(body)) != ESP_OK) return NULL;
    return cJSON_Parse(body);
}

static esp_err_t h_zb_permit(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    cJSON *j = read_json(req);
    const cJSON *s = cJSON_GetObjectItem(j, "seconds");
    int secs = cJSON_IsNumber(s) ? s->valueint : -1;
    cJSON_Delete(j);
    if (secs < 0 || secs > 254) return web_send_error(req, "400 Bad Request", "seconds: 0..254");
    esp_err_t err = zb_permit_join((uint8_t)secs);
    if (err != ESP_OK) return web_send_error(req, "503 Service Unavailable", esp_err_to_name(err));
    return web_send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_zb_rename(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    cJSON *j = read_json(req);
    const cJSON *ieee = cJSON_GetObjectItem(j, "ieee"), *name = cJSON_GetObjectItem(j, "name");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(ieee) && cJSON_IsString(name)) err = zb_rename(ieee->valuestring, name->valuestring);
    cJSON_Delete(j);
    if (err == ESP_ERR_INVALID_ARG) return web_send_error(req, "400 Bad Request", "ieee and name required");
    if (err != ESP_OK) return web_send_error(req, "404 Not Found", "no such device");
    return web_send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_zb_remove(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    cJSON *j = read_json(req);
    const cJSON *ieee = cJSON_GetObjectItem(j, "ieee");
    esp_err_t err = cJSON_IsString(ieee) ? zb_remove(ieee->valuestring) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j);
    if (err == ESP_ERR_INVALID_ARG) return web_send_error(req, "400 Bad Request", "ieee required");
    if (err != ESP_OK) return web_send_error(req, "404 Not Found", "no such device");
    return web_send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_zb_reinterview(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    cJSON *j = read_json(req);
    const cJSON *ieee = cJSON_GetObjectItem(j, "ieee");
    esp_err_t err = cJSON_IsString(ieee) ? zb_reinterview(ieee->valuestring) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j);
    if (err == ESP_ERR_INVALID_ARG) return web_send_error(req, "400 Bad Request", "ieee required");
    if (err == ESP_ERR_NOT_FOUND) return web_send_error(req, "404 Not Found", "no such device");
    if (err != ESP_OK) return web_send_error(req, "503 Service Unavailable", esp_err_to_name(err));
    return web_send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_coredump(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    size_t addr = 0, size = 0;
    if (hubsys_coredump_get(&addr, &size) != ESP_OK) {
        return web_send_error(req, "404 Not Found", "no core dump");
    }
    char disp[96];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s-coredump.bin\"", web_hostname());
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const size_t cap = 4096;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return web_send_error(req, "500 Internal Server Error", "out of memory");
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < size && err == ESP_OK; off += cap) {
        size_t n = size - off < cap ? size - off : cap;
        err = esp_flash_read(NULL, buf, addr + off, n);
        if (err == ESP_OK) err = httpd_resp_send_chunk(req, buf, n);
    }
    free(buf);
    if (err != ESP_OK) return err;
    return httpd_resp_send_chunk(req, NULL, 0);
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
        {.uri = "/api/ts", .method = HTTP_GET, .handler = h_ts_get},
        {.uri = "/api/ts/key", .method = HTTP_POST, .handler = h_ts_key},
        {.uri = "/api/ts/enabled", .method = HTTP_POST, .handler = h_ts_enabled},
        {.uri = "/api/ts/logout", .method = HTTP_POST, .handler = h_ts_logout},
        {.uri = "/api/ts/debug", .method = HTTP_POST, .handler = h_ts_debug},
        {.uri = "/api/access", .method = HTTP_GET, .handler = h_access_get},
        {.uri = "/api/access", .method = HTTP_POST, .handler = h_access_set},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = h_logs},
        {.uri = "/api/coredump", .method = HTTP_GET, .handler = h_coredump},
        {.uri = "/api/zb", .method = HTTP_GET, .handler = h_zb_get},
        {.uri = "/api/zb/permit", .method = HTTP_POST, .handler = h_zb_permit},
        {.uri = "/api/zb/rename", .method = HTTP_POST, .handler = h_zb_rename},
        {.uri = "/api/zb/remove", .method = HTTP_POST, .handler = h_zb_remove},
        {.uri = "/api/zb/reinterview", .method = HTTP_POST, .handler = h_zb_reinterview},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    esp_err_t err = auto_api_register(server);
    if (err != ESP_OK) return err;
    return motion_api_register(server);
}
