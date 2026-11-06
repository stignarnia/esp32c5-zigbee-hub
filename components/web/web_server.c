// HTTP server: static UI, WebSocket status push, shared request helpers.
//
// Everything runs on the httpd task, including WebSocket sends (queued with
// httpd_queue_work), so no locking is needed and no socket is ever touched by a
// task other than the one that owns it (lesson 8 in CLAUDE.md).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hubsys.h"
#include "web.h"
#include "web_internal.h"
#include "zb.h"

static const char *TAG = "web";

static httpd_handle_t s_server;
static char s_hostname[33];
static char s_etag[24];

#define MAX_WS 4
static int s_ws_fds[MAX_WS];  // authenticated WebSocket clients; -1 = free
static volatile bool s_ws_new;  // a client just connected: send it a device snapshot now

// Embedded, gzipped at build time (see CMakeLists.txt).
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const uint8_t app_js_gz_start[] asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[] asm("_binary_app_js_gz_end");
extern const uint8_t style_css_gz_start[] asm("_binary_style_css_gz_start");
extern const uint8_t style_css_gz_end[] asm("_binary_style_css_gz_end");

// ---- helpers ---------------------------------------------------------------------

const char *web_hostname(void)
{
    return s_hostname;
}

esp_err_t web_send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

esp_err_t web_send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    esp_err_t r = web_send_json(req, status, s ? s : "{}");
    free(s);
    return r;
}

bool web_guard(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        // Browsers can't add custom headers to cross-site form posts or simple
        // requests without a CORS preflight, which we never answer: blocks CSRF.
        char v[4];
        if (httpd_req_get_hdr_value_str(req, "X-Hub", v, sizeof(v)) != ESP_OK) {
            web_send_error(req, "403 Forbidden", "missing X-Hub header");
            return false;
        }
    }
    if (!auth_check_request(req)) {
        web_send_error(req, "401 Unauthorized", "login required");
        return false;
    }
    return true;
}

esp_err_t web_read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len >= cap) return ESP_ERR_INVALID_SIZE;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    buf[got] = '\0';
    return ESP_OK;
}

// ---- static files ---------------------------------------------------------------------

typedef struct {
    const char *uri, *type;
    const uint8_t *start, *end;
} asset_t;

static esp_err_t asset_handler(httpd_req_t *req)
{
    const asset_t *a = req->user_ctx;
    char inm[sizeof(s_etag) + 4];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
        strcmp(inm, s_etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", s_etag);
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, a->type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");  // revalidate: ETag = build id
    httpd_resp_set_hdr(req, "ETag", s_etag);
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    if (strcmp(a->type, "text/html") == 0) {
        httpd_resp_set_hdr(req, "Content-Security-Policy",
                           "default-src 'self'; connect-src 'self' ws: wss:; "
                           "style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
                           "frame-ancestors 'none'");
    }
    return httpd_resp_send(req, (const char *)a->start, a->end - a->start);
}

static asset_t s_assets[3];

// ---- WebSocket ------------------------------------------------------------------------

static void ws_forget(int fd)
{
    for (int i = 0; i < MAX_WS; i++) {
        if (s_ws_fds[i] == fd) s_ws_fds[i] = -1;
    }
}

// IDF 6.1 never calls the URI handler for the handshake itself: auth has to run
// in the pre-handshake hook, *before* the server agrees to upgrade.
static esp_err_t ws_pre_handshake(httpd_req_t *req)
{
    if (!auth_check_request(req)) {
        ESP_LOGW(TAG, "ws: rejected unauthenticated client");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ws_post_handshake(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    for (int i = 0; i < MAX_WS; i++) {
        if (s_ws_fds[i] == -1) {
            s_ws_fds[i] = fd;
            s_ws_new = true;
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG, "ws: too many clients");
    return ESP_FAIL;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    // Incoming frames: only pings/pongs/close matter; payloads are ignored.
    httpd_ws_frame_t f = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) return err;
    if (f.len > 0 && f.len <= 128) {
        uint8_t buf[129];
        f.payload = buf;
        httpd_ws_recv_frame(req, &f, f.len);
    } else if (f.len > 128) {
        return ESP_FAIL;  // nothing legitimate is that long
    }
    if (f.type == HTTPD_WS_TYPE_CLOSE) ws_forget(httpd_req_to_sockfd(req));
    return ESP_OK;
}

// arg: NULL = status, non-NULL = Zigbee snapshot.
static void ws_push_work(void *arg)
{
    char *json = arg ? api_zb_json() : api_status_json();
    if (!json) return;
    httpd_ws_frame_t f = {
        .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)json, .len = strlen(json),
    };
    for (int i = 0; i < MAX_WS; i++) {
        int fd = s_ws_fds[i];
        if (fd < 0) continue;
        if (httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET ||
            httpd_ws_send_frame_async(s_server, fd, &f) != ESP_OK) {
            s_ws_fds[i] = -1;
        }
    }
    free(json);
}

// Status every 2 s. Zigbee devices: within 100 ms of any change (a contact opening,
// a new device announcing), and every 5 s anyway so "last seen" and the permit-join
// countdown stay current.
static void ws_push_task(void *arg)
{
    uint32_t last_rev = UINT32_MAX, last_permit = 0;
    for (int tick = 1;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        bool any = false;
        for (int i = 0; i < MAX_WS; i++) any |= s_ws_fds[i] >= 0;
        if (!any || !s_server) {
            last_rev = UINT32_MAX;  // a new client gets a snapshot at once
            continue;
        }
        if (tick % 20 == 0) httpd_queue_work(s_server, ws_push_work, NULL);
        uint32_t rev = zb_revision(), permit = zb_permit_left();
        if (rev != last_rev || permit != last_permit || tick % 50 == 0 || s_ws_new) {
            s_ws_new = false;
            last_rev = rev;
            last_permit = permit;
            httpd_queue_work(s_server, ws_push_work, (void *)1);
        }
    }
}

static void on_close(httpd_handle_t hd, int fd)
{
    ws_forget(fd);
    close(fd);
}

// ---- cJSON in PSRAM (lesson 6: internal RAM is the bottleneck) ---------------------

static void *psram_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

// ---- start -------------------------------------------------------------------------------

esp_err_t web_start(const web_config_t *cfg)
{
    strlcpy(s_hostname, cfg->hostname, sizeof(s_hostname));
    snprintf(s_etag, sizeof(s_etag), "\"%s\"", hubsys_build_id());
    for (int i = 0; i < MAX_WS; i++) s_ws_fds[i] = -1;

    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);

    ESP_ERROR_CHECK(auth_init(cfg->admin_hash));

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 8192;           // PBKDF2 + JSON building
    hc.max_uri_handlers = 24;
    // Measured: with 7, a phone's Chrome failed with ERR_TOO_MANY_RETRIES while a
    // laptop had the UI open — the LRU purge kept closing the phone's own freshly
    // opened parallel connections. Must stay <= LWIP_MAX_SOCKETS - 3.
    hc.max_open_sockets = 13;
    hc.lru_purge_enable = true;     // a stuck client can't lock others out
    hc.recv_wait_timeout = 10;      // OTA uploads stream for seconds
    hc.send_wait_timeout = 10;
    hc.close_fn = on_close;
    hc.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&s_server, &hc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    s_assets[0] = (asset_t){"/", "text/html", index_html_gz_start, index_html_gz_end};
    s_assets[1] = (asset_t){"/app.js", "text/javascript", app_js_gz_start, app_js_gz_end};
    s_assets[2] = (asset_t){"/style.css", "text/css", style_css_gz_start, style_css_gz_end};
    for (int i = 0; i < 3; i++) {
        httpd_uri_t u = {.uri = s_assets[i].uri, .method = HTTP_GET, .handler = asset_handler,
                         .user_ctx = &s_assets[i]};
        httpd_register_uri_handler(s_server, &u);
    }
    httpd_uri_t ws = {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler,
                      .is_websocket = true, .handle_ws_control_frames = false,
                      .ws_pre_handshake_cb = ws_pre_handshake,
                      .ws_post_handshake_cb = ws_post_handshake};
    httpd_register_uri_handler(s_server, &ws);
    ESP_ERROR_CHECK(api_register(s_server));

    xTaskCreate(ws_push_task, "ws_push", 2560, NULL, 4, NULL);  // only queues work: small stack
    ESP_LOGI(TAG, "web UI on port %d (UI %u B gzipped)", hc.server_port,
             (unsigned)((index_html_gz_end - index_html_gz_start) + (app_js_gz_end - app_js_gz_start) +
                        (style_css_gz_end - style_css_gz_start)));
    return ESP_OK;
}

bool web_is_running(void)
{
    return s_server != NULL;
}
