// REST API for the automation engine (Automations page, Automations → Secrets / GPIO, Settings → time
// zone). All handlers run on the httpd task and need a session (web_guard).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "automation.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "web_internal.h"

static esp_err_t send_cjson(httpd_req_t *req, const char *status, cJSON *j)
{
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!txt) return web_send_error(req, "500 Internal Server Error", "out of memory");
    esp_err_t err = web_send_json(req, status, txt);
    cJSON_free(txt);
    return err;
}

static esp_err_t send_err(httpd_req_t *req, esp_err_t r, const char *msg)
{
    const char *st = r == ESP_ERR_NOT_FOUND       ? "404 Not Found"
                   : r == ESP_ERR_INVALID_ARG     ? "400 Bad Request"
                   : r == ESP_ERR_INVALID_SIZE    ? "413 Payload Too Large"
                   : r == ESP_ERR_INVALID_STATE   ? "409 Conflict"
                   : r == ESP_ERR_TIMEOUT         ? "503 Service Unavailable"
                   : "500 Internal Server Error";
    return web_send_error(req, st, msg && msg[0] ? msg : esp_err_to_name(r));
}

// Body (up to cap-1 bytes) into a PSRAM buffer; caller frees.
static char *read_body(httpd_req_t *req, size_t cap, size_t *len)
{
    if (req->content_len >= cap) return NULL;
    char *buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return NULL;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = '\0';
    if (len) *len = got;
    return buf;
}

static cJSON *read_json_body(httpd_req_t *req)
{
    char *b = read_body(req, 4096, NULL);
    cJSON *j = b ? cJSON_Parse(b) : NULL;
    if (b) {
        memset(b, 0, strlen(b));  // may hold a secret
        free(b);
    }
    return j;
}

static bool query(httpd_req_t *req, const char *key, char *out, size_t cap)
{
    char q[96];
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, key, out, cap) == ESP_OK;
}

static esp_err_t h_auto_get(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    return send_cjson(req, "200 OK", automation_json());
}

static esp_err_t h_script_get(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    char name[AUTO_NAME_MAX + 1];
    if (!query(req, "name", name, sizeof(name))) return web_send_error(req, "400 Bad Request", "name required");
    char *src = automation_source(name);
    if (!src) return web_send_error(req, "404 Not Found", "no such script");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, src, HTTPD_RESP_USE_STRLEN);
    free(src);
    return err;
}

// POST /api/auto/script?name=x&enabled=1, body = the Lua source.
static esp_err_t h_script_save(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    char name[AUTO_NAME_MAX + 1], en[4] = "1";
    if (!query(req, "name", name, sizeof(name))) return web_send_error(req, "400 Bad Request", "name required");
    query(req, "enabled", en, sizeof(en));
    if (req->content_len > AUTO_SCRIPT_MAX) return web_send_error(req, "413 Payload Too Large", "script larger than 32 KB");
    size_t len = 0;
    char *src = read_body(req, AUTO_SCRIPT_MAX + 1, &len);
    if (!src) return web_send_error(req, "400 Bad Request", "could not read the script");
    char err[200] = "";
    esp_err_t r = automation_save(name, src, len, strcmp(en, "0") != 0, err, sizeof(err));
    free(src);
    if (r == ESP_ERR_INVALID_ARG && err[0]) {
        // Syntax error: "name:LINE: message" → {"error": ..., "line": LINE}
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "error", err);
        const char *p = strchr(err, ':');
        if (p && atoi(p + 1) > 0) cJSON_AddNumberToObject(o, "line", atoi(p + 1));
        return send_cjson(req, "400 Bad Request", o);
    }
    if (r != ESP_OK) return send_err(req, r, err);
    if (err[0]) {
        // Stored, but it stopped while loading (e.g. a GPIO pin another script owns)
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", true);
        cJSON_AddBoolToObject(o, "started", false);
        cJSON_AddStringToObject(o, "error", err);
        const char *p = strchr(err, ':');
        if (p && atoi(p + 1) > 0) cJSON_AddNumberToObject(o, "line", atoi(p + 1));
        return send_cjson(req, "200 OK", o);
    }
    return web_send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t name_action(httpd_req_t *req, esp_err_t (*fn)(const char *))
{
    if (!web_guard(req)) return ESP_OK;
    cJSON *j = read_json_body(req);
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
    esp_err_t r = name ? fn(name) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j);
    if (r != ESP_OK) return send_err(req, r, r == ESP_ERR_INVALID_ARG ? "name required" : NULL);
    return web_send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_restart(httpd_req_t *req) { return name_action(req, automation_restart); }
static esp_err_t h_delete(httpd_req_t *req) { return name_action(req, automation_delete); }

static esp_err_t h_enable(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    cJSON *j = read_json_body(req);
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
    const cJSON *en = cJSON_GetObjectItem(j, "enabled");
    esp_err_t r = name && cJSON_IsBool(en) ? automation_set_enabled(name, cJSON_IsTrue(en)) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j);
    if (r != ESP_OK) return send_err(req, r, r == ESP_ERR_INVALID_ARG ? "name and enabled required" : NULL);
    return web_send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_log(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    char a[12] = "0";
    query(req, "after", a, sizeof(a));
    unsigned last = 0;
    cJSON *lines = automation_log_json((unsigned)strtoul(a, NULL, 10), 200, &last);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "last", last);
    cJSON_AddItemToObject(o, "lines", lines);
    return send_cjson(req, "200 OK", o);
}

static esp_err_t h_secret(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    cJSON *j = read_json_body(req);
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
    const cJSON *v = cJSON_GetObjectItem(j, "value");
    esp_err_t r = ESP_ERR_INVALID_ARG;
    if (name && (cJSON_IsNull(v) || cJSON_IsString(v))) r = automation_secret_set(name, cJSON_IsString(v) ? v->valuestring : NULL);
    if (cJSON_IsString(v)) memset(v->valuestring, 0, strlen(v->valuestring));
    cJSON_Delete(j);
    if (r == ESP_ERR_INVALID_ARG) return web_send_error(req, "400 Bad Request", "name: 1-15 of a-z 0-9 _; value: string or null");
    if (r == ESP_ERR_INVALID_SIZE) return web_send_error(req, "400 Bad Request", "value: 1..512 characters");
    if (r != ESP_OK) return send_err(req, r, NULL);
    return web_send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_gpio(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    cJSON *j = read_json_body(req);
    char err[120] = "";
    esp_err_t r = j ? automation_set_gpio(cJSON_GetObjectItem(j, "pins"), err, sizeof(err)) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j);
    if (r != ESP_OK) return send_err(req, r, err[0] ? err : "pins required");
    return web_send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_tz(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    cJSON *j = read_json_body(req);
    const char *tz = cJSON_GetStringValue(cJSON_GetObjectItem(j, "tz"));
    esp_err_t r = tz ? automation_set_tz(tz) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j);
    if (r != ESP_OK) return send_err(req, r, r == ESP_ERR_INVALID_ARG ? "tz: a POSIX TZ string, e.g. CET-1CEST,M3.5.0,M10.5.0/3" : NULL);
    return web_send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_safe(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    esp_err_t r = automation_leave_safe_mode();
    if (r != ESP_OK) return send_err(req, r, NULL);
    return web_send_json(req, "200 OK", "{\"ok\":true}");
}

#ifdef HUB_TEST_HOOKS
// Test builds only (idf.py -DHUB_TEST_HOOKS=1): crash on purpose to test the boot-loop
// guard. {"after_ms": N} aborts N ms from now; {"boots": N} also aborts 5 s after each of
// the next N boots (auto_engine.c).
#include "esp_timer.h"
#include "nvs.h"
static void crash_cb(void *arg) { abort(); }
static esp_err_t h_crash(httpd_req_t *req)
{
    if (!web_guard(req)) return ESP_OK;
    cJSON *j = read_json_body(req);
    int after = cJSON_IsNumber(cJSON_GetObjectItem(j, "after_ms")) ? cJSON_GetObjectItem(j, "after_ms")->valueint : -1;
    int boots = cJSON_IsNumber(cJSON_GetObjectItem(j, "boots")) ? cJSON_GetObjectItem(j, "boots")->valueint : 0;
    cJSON_Delete(j);
    if (boots > 0) {
        nvs_handle_t h;
        if (nvs_open("hubtest", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "crashn", (uint8_t)boots);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    if (after >= 0) {
        static esp_timer_handle_t t;
        esp_timer_create_args_t a = {.callback = crash_cb, .name = "testcrash"};
        if (!t) esp_timer_create(&a, &t);
        esp_timer_start_once(t, (uint64_t)(after > 0 ? after : 1) * 1000);
    }
    return web_send_json(req, "200 OK", "{\"ok\":true}");
}
#endif

esp_err_t auto_api_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/auto", .method = HTTP_GET, .handler = h_auto_get},
        {.uri = "/api/auto/script", .method = HTTP_GET, .handler = h_script_get},
        {.uri = "/api/auto/script", .method = HTTP_POST, .handler = h_script_save},
        {.uri = "/api/auto/enable", .method = HTTP_POST, .handler = h_enable},
        {.uri = "/api/auto/restart", .method = HTTP_POST, .handler = h_restart},
        {.uri = "/api/auto/delete", .method = HTTP_POST, .handler = h_delete},
        {.uri = "/api/auto/log", .method = HTTP_GET, .handler = h_log},
        {.uri = "/api/auto/secret", .method = HTTP_POST, .handler = h_secret},
        {.uri = "/api/auto/gpio", .method = HTTP_POST, .handler = h_gpio},
        {.uri = "/api/auto/tz", .method = HTTP_POST, .handler = h_tz},
        {.uri = "/api/auto/safe", .method = HTTP_POST, .handler = h_safe},
#ifdef HUB_TEST_HOOKS
        {.uri = "/api/debug/crash", .method = HTTP_POST, .handler = h_crash},
#endif
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
