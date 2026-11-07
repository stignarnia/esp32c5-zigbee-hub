// HTTP requests for scripts (e.g. an Alexa routine trigger URL).
//
// A script never waits: http.request() queues the request and returns; the result
// comes back later through on_done(status, body) on the automation task (status 0 =
// network error / gave up). One request at a time on this task (one TLS connection,
// ~40 KB of mbedTLS buffers in PSRAM while it runs).
//
//   retry_for = s : offline, timeouts and 5xx/429 are retried with back-off (2 → 60 s)
//                   until s seconds after the request; 0 = one attempt
//   https://      : certificates verified against the ESP-IDF bundle
//   http://       : only to LAN / tailnet addresses (private, link-local, 100.64/10)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "auto_internal.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lwip/netdb.h"
#include "wifi5g.h"

static const char *TAG = "auto.http";

#define SLOTS          16
#define BODY_MAX       (16 * 1024)
#define URL_MAX        512
#define HEADERS_MAX    1024
#define REQ_BODY_MAX   (8 * 1024)
#define RATE_PER_MIN   10
#define BACKOFF_MAX_S  60

typedef struct {
    bool used;
    uint8_t script;
    uint32_t gen;
    int ref;
    char method[8];
    char *url;
    char *headers;          // "Name: value\n" lines
    char *body;
    size_t body_len;
    uint32_t timeout_ms;
    int64_t queued_us, deadline_us, next_us;
    uint32_t attempts;
} req_t;

static req_t *s_req;        // PSRAM
static SemaphoreHandle_t s_mtx;
static TaskHandle_t s_task;
static uint32_t s_seq;

// ---- the task -----------------------------------------------------------------------------------

typedef struct {
    char *buf;
    size_t len;
    bool truncated;
} sink_t;

static esp_err_t on_http_event(esp_http_client_event_t *e)
{
    sink_t *s = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && s->buf) {
        size_t room = BODY_MAX - s->len;
        size_t n = (size_t)e->data_len < room ? (size_t)e->data_len : room;
        memcpy(s->buf + s->len, e->data, n);
        s->len += n;
        if (n < (size_t)e->data_len) s->truncated = true;
    }
    return ESP_OK;
}

static bool private_ip(uint32_t ip)  // host byte order
{
    return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 22) == (100 << 2 | 1) ||
           (ip >> 16) == 0xA9FE || (ip >> 24) == 127;
}

// Plain http:// is allowed only when the host is on the LAN or the tailnet.
static bool lan_target(const char *url, char *err, size_t cap)
{
    const char *h = url + strlen("http://");
    char host[128];
    size_t n = strcspn(h, ":/?#");
    if (n == 0 || n >= sizeof(host)) {
        strlcpy(err, "bad host", cap);
        return false;
    }
    memcpy(host, h, n);
    host[n] = '\0';
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        strlcpy(err, "host not found", cap);
        return false;
    }
    uint32_t ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    if (!private_ip(ip)) {
        strlcpy(err, "plain http:// only to LAN addresses (use https://)", cap);
        return false;
    }
    return true;
}

static void host_of(const char *url, char *out, size_t cap)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t n = strcspn(p, "/?#");
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static int perform(req_t *r, sink_t *sink, char *err, size_t cap)
{
    if (strncasecmp(r->url, "http://", 7) == 0 && !lan_target(r->url, err, cap)) return -1;  // not retried
    esp_http_client_method_t m = HTTP_METHOD_GET;
    if (strcmp(r->method, "POST") == 0) m = HTTP_METHOD_POST;
    else if (strcmp(r->method, "PUT") == 0) m = HTTP_METHOD_PUT;
    else if (strcmp(r->method, "DELETE") == 0) m = HTTP_METHOD_DELETE;
    else if (strcmp(r->method, "PATCH") == 0) m = HTTP_METHOD_PATCH;
    else if (strcmp(r->method, "HEAD") == 0) m = HTTP_METHOD_HEAD;
    esp_http_client_config_t cfg = {
        .url = r->url,
        .method = m,
        .timeout_ms = (int)r->timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http_event,
        .user_data = sink,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .max_redirection_count = 3,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        strlcpy(err, "out of memory", cap);
        return 0;
    }
    esp_http_client_set_header(c, "User-Agent", "zigbee-hub");
    for (char *line = r->headers; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            const char *v = colon + 1;
            while (*v == ' ') v++;
            esp_http_client_set_header(c, line, v);
            *colon = ':';
        }
        if (!nl) break;
        *nl = '\n';
        line = nl + 1;
    }
    if (r->body) esp_http_client_set_post_field(c, r->body, (int)r->body_len);
    esp_err_t e = esp_http_client_perform(c);
    int status = e == ESP_OK ? esp_http_client_get_status_code(c) : 0;
    if (e != ESP_OK) strlcpy(err, esp_err_to_name(e), cap);
    esp_http_client_cleanup(c);
    return status;
}

static void req_free(req_t *r)
{
    free(r->url);
    free(r->headers);
    free(r->body);
    memset(r, 0, sizeof(*r));
}

static void finish(req_t *r, int status, char *body, size_t len, const char *err)
{
    ev_t ev = {.type = EV_HTTP};
    ev.http.script = r->script;
    ev.http.gen = r->gen;
    ev.http.ref = r->ref;
    ev.http.status = status < 0 ? 0 : status;
    ev.http.body = body;
    ev.http.len = len;
    strlcpy(ev.http.err, err ? err : "", sizeof(ev.http.err));
    // The result must arrive: wait for room in the queue (bounded).
    bool sent = false;
    for (int i = 0; i < 100 && !sent; i++) {
        sent = auto_post(&ev);
        if (!sent) vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!sent) {
        free(body);
        ESP_LOGW(TAG, "result dropped: the automation queue stayed full");
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    req_free(r);
    xSemaphoreGive(s_mtx);
}

static void http_task(void *arg)
{
    for (;;) {
        // Next request due (oldest first).
        int64_t now = esp_timer_get_time();
        int pick = -1;
        int64_t next = INT64_MAX;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        for (int i = 0; i < SLOTS; i++) {
            if (!s_req[i].used) continue;
            if (s_req[i].next_us <= now && (pick < 0 || s_req[i].queued_us < s_req[pick].queued_us)) pick = i;
            if (s_req[i].next_us < next) next = s_req[i].next_us;
        }
        xSemaphoreGive(s_mtx);
        if (pick < 0) {
            int64_t wait_ms = next == INT64_MAX ? 60000 : (next - now) / 1000 + 1;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
            continue;
        }
        req_t *r = &s_req[pick];
        const char *name = g_scripts[r->script].name;
        char host[64];
        host_of(r->url, host, sizeof(host));
        bool retry_ok = now < r->deadline_us;
        if (!wifi5g_is_connected()) {
            if (retry_ok) {
                r->next_us = now + 2000000;  // offline: check again in 2 s, no attempt
                continue;
            }
            auto_log(name, 'E', "http %s %s: offline, not sent", r->method, host);
            finish(r, 0, NULL, 0, "offline");
            continue;
        }
        sink_t sink = {.buf = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)};
        char err[64] = "";
        int64_t t0 = esp_timer_get_time();
        int status = perform(r, &sink, err, sizeof(err));
        int ms = (int)((esp_timer_get_time() - t0) / 1000);
        r->attempts++;
        bool transient = status == 0 || status >= 500 || status == 429;
        if (transient && esp_timer_get_time() < r->deadline_us) {
            free(sink.buf);
            int back = 2 << (r->attempts < 5 ? r->attempts - 1 : 5);
            if (back > BACKOFF_MAX_S) back = BACKOFF_MAX_S;
            r->next_us = esp_timer_get_time() + (int64_t)back * 1000000;
            auto_log(name, 'E', "http %s %s: %s (try %lu), again in %d s", r->method, host,
                     status ? "server error" : err, (unsigned long)r->attempts, back);
            if (status) ESP_LOGD(TAG, "status %d", status);
            continue;
        }
        if (status > 0) {
            auto_log(name, status < 400 ? 'I' : 'E', "http %s %s -> %d in %d ms%s%s", r->method, host, status, ms,
                     r->attempts > 1 ? " (after retries)" : "", sink.truncated ? ", body truncated to 16 KB" : "");
        } else {
            auto_log(name, 'E', "http %s %s: %s after %lu attempt(s)", r->method, host, err[0] ? err : "failed",
                     (unsigned long)r->attempts);
        }
        if (status <= 0) {
            free(sink.buf);
            sink.buf = NULL;
            sink.len = 0;
        }
        finish(r, status, sink.buf, sink.len, err);
    }
}

void ahttp_start(void)
{
    s_req = heap_caps_calloc(SLOTS, sizeof(req_t), MALLOC_CAP_SPIRAM);
    s_mtx = xSemaphoreCreateMutex();
    if (xTaskCreateWithCaps(http_task, "auto_http", 10 * 1024, NULL, 2, &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "task not created");
    }
}

// ---- Lua API (automation task) ----------------------------------------------------------------

static char *psram_copy(const char *s, size_t n)
{
    char *c = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c) {
        memcpy(c, s, n);
        c[n] = '\0';
    }
    return c;
}

// The options table is at index `t`. Returns true, or nil + reason.
static int request_from(lua_State *L, int t)
{
    script_t *s = auto_script_of(L);
    t = lua_absindex(L, t);
    lua_getfield(L, t, "url");
    size_t url_len;
    const char *url = luaL_checklstring(L, -1, &url_len);
    luaL_argcheck(L, url_len < URL_MAX, 1, "url too long");
    luaL_argcheck(L, strncasecmp(url, "https://", 8) == 0 || strncasecmp(url, "http://", 7) == 0, 1,
                  "url must start with https:// or http://");
    int urli = lua_gettop(L);

    lua_getfield(L, t, "method");
    const char *method = luaL_optstring(L, -1, "GET");
    static const char *const methods[] = {"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD"};
    bool mok = false;
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) mok |= strcmp(method, methods[i]) == 0;
    luaL_argcheck(L, mok, 1, "method: GET POST PUT DELETE PATCH HEAD");

    // Body: `body` (string) or `json` (table/value, encoded with json.encode).
    const char *body = NULL;
    size_t body_len = 0;
    bool is_json = false;
    if (lua_getfield(L, t, "json") != LUA_TNIL) {
        lua_getglobal(L, "json");
        lua_getfield(L, -1, "encode");
        lua_pushvalue(L, -3);
        lua_call(L, 1, 1);
        body = lua_tolstring(L, -1, &body_len);
        is_json = true;
    } else if (lua_getfield(L, t, "body") != LUA_TNIL) {
        body = luaL_checklstring(L, -1, &body_len);
    }
    luaL_argcheck(L, body_len <= REQ_BODY_MAX, 1, "body larger than 8 KB");
    int bodyi = lua_gettop(L);

    // Headers → "Name: value\n" lines (plain C buffer: nothing else may sit on the Lua stack
    // between luaL_Buffer calls).
    char headers[HEADERS_MAX + 1];
    size_t hlen = 0;
    headers[0] = '\0';
    if (is_json) hlen = strlcpy(headers, "Content-Type: application/json\n", sizeof(headers));
    if (lua_getfield(L, t, "headers") == LUA_TTABLE) {
        int hi = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, hi)) {
            if (lua_type(L, -2) != LUA_TSTRING || !lua_isstring(L, -1)) luaL_error(L, "http: headers are name = value strings");
            const char *k = lua_tostring(L, -2), *v = lua_tostring(L, -1);
            if (strpbrk(k, ":\r\n") || strpbrk(v, "\r\n")) luaL_error(L, "http: bad header");
            int w = snprintf(headers + hlen, sizeof(headers) - hlen, "%s: %s\n", k, v);
            if (w < 0 || hlen + (size_t)w >= sizeof(headers)) luaL_error(L, "http: headers larger than 1 KB");
            hlen += (size_t)w;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, t, "timeout");
    lua_Number timeout = luaL_optnumber(L, -1, 10);
    lua_getfield(L, t, "retry_for");
    lua_Number retry_for = luaL_optnumber(L, -1, 0);
    luaL_argcheck(L, timeout >= 1 && timeout <= 60, 1, "timeout: 1..60 s");
    luaL_argcheck(L, retry_for >= 0 && retry_for <= 86400, 1, "retry_for: 0..86400 s");
    lua_getfield(L, t, "on_done");
    int has_cb = lua_type(L, -1);
    luaL_argexpected(L, has_cb == LUA_TNIL || has_cb == LUA_TFUNCTION, 1, "on_done: function");

    // Rate limit (per script) and a free slot.
    int64_t now = esp_timer_get_time();
    if (now - s->http_win_us > 60LL * 1000000) {
        s->http_win_us = now;
        s->http_n = 0;
    }
    if (s->http_n >= RATE_PER_MIN) {
        lua_pushnil(L);
        lua_pushfstring(L, "rate limited (%d requests a minute)", RATE_PER_MIN);
        return 2;
    }
    char *c_url = psram_copy(lua_tostring(L, urli), url_len);
    char *c_hdr = hlen ? psram_copy(headers, hlen) : NULL;
    char *c_body = body ? psram_copy(lua_tolstring(L, bodyi, NULL), body_len) : NULL;
    if (!c_url || (hlen && !c_hdr) || (body && !c_body)) {
        free(c_url);
        free(c_hdr);
        free(c_body);
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }
    int ref = LUA_NOREF;
    if (has_cb == LUA_TFUNCTION) {
        lua_pushvalue(L, -1);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    req_t *r = NULL;
    for (int i = 0; i < SLOTS && !r; i++) if (!s_req[i].used) r = &s_req[i];
    if (r) {
        *r = (req_t){.used = true, .script = (uint8_t)auto_script_index(s), .gen = s->gen, .ref = ref,
                     .url = c_url, .headers = c_hdr, .body = c_body, .body_len = body_len,
                     .timeout_ms = (uint32_t)(timeout * 1000), .queued_us = now + (s_seq++ & 0xFF),
                     .deadline_us = now + (int64_t)(retry_for * 1e6), .next_us = now};
        strlcpy(r->method, method, sizeof(r->method));
    }
    xSemaphoreGive(s_mtx);
    if (!r) {
        free(c_url);
        free(c_hdr);
        free(c_body);
        if (ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ref);
        lua_pushnil(L);
        lua_pushfstring(L, "queue full (%d requests waiting)", SLOTS);
        return 2;
    }
    s->http_n++;
    xTaskNotifyGive(s_task);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_request(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    return request_from(L, 1);
}

// Builds an options table from (url, [body,] opts) and sends it.
static int shorthand(lua_State *L, const char *method, bool has_body)
{
    luaL_checkstring(L, 1);
    int opts = has_body ? 3 : 2;
    lua_newtable(L);
    int t = lua_gettop(L);
    if (lua_istable(L, opts)) {
        lua_pushnil(L);
        while (lua_next(L, opts)) {
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_rawset(L, t);
        }
    }
    lua_pushvalue(L, 1);
    lua_setfield(L, t, "url");
    lua_pushstring(L, method);
    lua_setfield(L, t, "method");
    if (has_body && !lua_isnoneornil(L, 2)) {
        lua_pushvalue(L, 2);
        lua_setfield(L, t, lua_istable(L, 2) ? "json" : "body");
    }
    return request_from(L, t);
}

static int l_get(lua_State *L) { return shorthand(L, "GET", false); }
static int l_post(lua_State *L) { return shorthand(L, "POST", true); }

void ahttp_open_lib(lua_State *L)
{
    static const luaL_Reg fns[] = {{"request", l_request}, {"get", l_get}, {"post", l_post}, {NULL, NULL}};
    luaL_newlib(L, fns);
    lua_setglobal(L, "http");
}
