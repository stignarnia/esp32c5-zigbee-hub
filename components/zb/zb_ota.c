// Zigbee device firmware updates: the hub as OTA Upgrade server.
//
// One image at a time, held in PSRAM (the stack reads blocks on the Zigbee task,
// where file I/O is not allowed), lost on restart.
//
//   stage   a task fetches the image (https://, or http:// to a LAN/tailnet host),
//           checks its sha512, its OTA header, and the header against the device
//   start   the image is handed to the stack's OTA server and the device is told
//           (Image Notify; a sleepy device when it's next heard from)
//   the stack answers the device's Query Next Image and Image Block requests;
//   every step asks here first (zb_ota_on_server_progress), and only the target
//   device asking for the same manufacturer code and image type, while running
//   another file version, is admitted
//   Upgrade End: the device restarts into the new firmware; its version is read
//   again once it's back
//
// Every Query Next Image is also taken from the raw frame (zb_ota_on_raw): it says
// what a device runs, image type included, whether or not an image is offered.
//
// Locking: s_omtx guards s_ota. Taken by the HTTP task, the fetch task and the Zigbee
// task, never across a zb_dev_* call (those take the registry lock, which may be
// held when zb_ota_heard is called: order registry -> s_omtx).

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "psa/crypto.h"
#include "zb.h"
#include "zb_internal.h"

static const char *TAG = "zb.ota";

#define IMG_MAX          (1024 * 1024)
#define NOTIFY_EVERY_US  (5LL * 60 * 1000000)   // always-on target: Image Notify again while unanswered
#define NOTIFY_MAX       6
#define STALL_US         (10LL * 60 * 1000000)  // no block request for this long: failed
#define HDR_MIN          56                     // mandatory OTA header fields

typedef enum { ST_IDLE, ST_FETCHING, ST_STAGED, ST_OFFERED, ST_SENDING, ST_DONE, ST_FAILED } st_t;
static const char *const ST_NAME[] = {"idle", "fetching", "staged", "offered", "sending", "done", "failed"};

static struct {
    st_t st;
    uint64_t ieee;
    char url[256];
    uint8_t sha512[64];
    uint8_t *img;
    uint32_t len;
    uint32_t got;                // bytes fetched so far
    // OTA header of the staged image
    uint16_t manuf, image_type;
    uint32_t file_ver;
    char hdr_str[33];
    // device, as known when staged/started
    uint16_t short_addr;
    uint8_t ota_ep;
    bool sleepy;
    // transfer
    ezb_zcl_ota_file_handle_t fh;
    bool notify_pending;         // sleepy target: tell it when it's heard from
    uint8_t notifies;
    int64_t notify_us, started_us, block_us;
    uint32_t offset;
    uint32_t from_ver;           // what the device ran when it was admitted
    uint8_t end_status;
    bool auto_start;             // installed from an offer: offer as soon as it's staged
    char err[112];
} s_ota;

static SemaphoreHandle_t s_omtx;

#define OLOCK()   xSemaphoreTake(s_omtx, portMAX_DELAY)
#define OUNLOCK() xSemaphoreGive(s_omtx)

static void fail_locked(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail_locked(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_ota.err, sizeof(s_ota.err), fmt, ap);
    va_end(ap);
    s_ota.st = ST_FAILED;
    ESP_LOGW(TAG, "%s", s_ota.err);
    zb_dev_notify();
}

// ---- fetch (own task) ------------------------------------------------------------------

static bool private_ip(uint32_t ip)  // host byte order
{
    return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 22) == (100 << 2 | 1) ||
           (ip >> 16) == 0xA9FE || (ip >> 24) == 127;
}

// Plain http:// only to a host on the LAN or the tailnet.
static bool lan_host(const char *url)
{
    const char *h = url + strlen("http://");
    char host[128];
    size_t n = strcspn(h, ":/?#");
    if (n == 0 || n >= sizeof(host)) return false;
    memcpy(host, h, n);
    host[n] = '\0';
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;
    uint32_t ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return private_ip(ip);
}

typedef struct {
    uint8_t *buf;
    uint32_t len, cap;
    bool too_big;
} sink_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    sink_t *s = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(e->header_key, "Content-Length") == 0 &&
        esp_http_client_get_status_code(e->client) == 200) {
        long n = strtol(e->header_value, NULL, 10);
        if (n > IMG_MAX) s->too_big = true;
    } else if (e->event_id == HTTP_EVENT_ON_DATA && esp_http_client_get_status_code(e->client) == 200 && !s->too_big) {
        if (s->len + e->data_len > IMG_MAX) {
            s->too_big = true;
            return ESP_OK;
        }
        if (s->len + e->data_len > s->cap) {
            uint32_t cap = s->cap ? s->cap : 64 * 1024;
            while (cap < s->len + e->data_len) cap *= 2;
            if (cap > IMG_MAX) cap = IMG_MAX;
            uint8_t *nb = heap_caps_realloc(s->buf, cap, MALLOC_CAP_SPIRAM);
            if (!nb) {
                s->too_big = true;
                return ESP_OK;
            }
            s->buf = nb;
            s->cap = cap;
        }
        memcpy(s->buf + s->len, e->data, e->data_len);
        s->len += e->data_len;
        OLOCK();
        s_ota.got = s->len;
        OUNLOCK();
    }
    return ESP_OK;
}

static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// The image against its OTA header and the device. Fills the header fields of s_ota.
static bool check_image(const uint8_t *b, uint32_t len, char *err, size_t cap)
{
    if (len < HDR_MIN) return snprintf(err, cap, "too short for an OTA file (%" PRIu32 " bytes)", len), false;
    if (le32(b) != EZB_ZCL_OTA_FILE_IDENTIFIER) return snprintf(err, cap, "not a Zigbee OTA file (bad magic)"), false;
    uint16_t hdr_len = le16(b + 6), fc = le16(b + 8);
    uint32_t total = le32(b + 52);
    if (hdr_len < HDR_MIN || hdr_len > len) return snprintf(err, cap, "bad OTA header length %u", hdr_len), false;
    if (total != len) {
        return snprintf(err, cap, "header says %" PRIu32 " bytes, the file has %" PRIu32, total, len), false;
    }
    uint16_t manuf = le16(b + 10), type = le16(b + 12);
    uint32_t ver = le32(b + 14);
    uint16_t short_addr, dev_manuf, dev_type;
    uint8_t ep;
    bool sleepy;
    uint32_t dev_ver;
    if (!zb_dev_ota_info(s_ota.ieee, &short_addr, &ep, &sleepy, &dev_manuf, &dev_type, &dev_ver)) {
        return snprintf(err, cap, "the device is gone or has no OTA client"), false;
    }
    if (dev_manuf != 0xFFFF && manuf != dev_manuf) {
        return snprintf(err, cap, "image is for manufacturer 0x%04x, the device is 0x%04x", manuf, dev_manuf), false;
    }
    if (dev_type != 0xFFFF && type != dev_type) {
        return snprintf(err, cap, "image type 0x%04x, the device's is 0x%04x", type, dev_type), false;
    }
    if (dev_ver != 0xFFFFFFFF && ver == dev_ver) {
        return snprintf(err, cap, "the device already runs file version 0x%08" PRIx32, ver), false;
    }
    if (fc & EZB_ZCL_OTA_FILE_HEADER_FC_DEVICE_SPECIFIC) {
        uint32_t off = HDR_MIN + ((fc & EZB_ZCL_OTA_FILE_HEADER_FC_SECURITY_CREDENTIAL_VERSION_PRESENT) ? 1 : 0);
        if (off + 8 > hdr_len) return snprintf(err, cap, "bad OTA header (destination field)"), false;
        uint64_t dest = 0;
        for (int i = 0; i < 8; i++) dest |= (uint64_t)b[off + i] << (8 * i);
        if (dest != s_ota.ieee) return snprintf(err, cap, "image is for device %016llx", (unsigned long long)dest), false;
    }
    OLOCK();
    s_ota.manuf = manuf;
    s_ota.image_type = type;
    s_ota.file_ver = ver;
    memcpy(s_ota.hdr_str, b + 20, 32);
    s_ota.hdr_str[32] = '\0';
    for (int i = 0; i < 32; i++) {
        if (s_ota.hdr_str[i] && (s_ota.hdr_str[i] < 0x20 || s_ota.hdr_str[i] > 0x7E)) s_ota.hdr_str[i] = '?';
    }
    s_ota.short_addr = short_addr;
    s_ota.ota_ep = ep;
    s_ota.sleepy = sleepy;
    s_ota.from_ver = dev_ver;
    OUNLOCK();
    return true;
}

static void fetch_task(void *arg)
{
    char url[sizeof(s_ota.url)];
    uint8_t want[64];
    OLOCK();
    strlcpy(url, s_ota.url, sizeof(url));
    memcpy(want, s_ota.sha512, 64);
    OUNLOCK();
    sink_t sink = {0};
    char err[sizeof(s_ota.err)] = "";
    int status = 0;
    if (strncasecmp(url, "http://", 7) == 0 && !lan_host(url)) {
        strlcpy(err, "plain http:// only to LAN or tailnet hosts (use https://)", sizeof(err));
    } else {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 20000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = on_http,
            .user_data = &sink,
            .buffer_size = 2048,
            .buffer_size_tx = 1024,
            .max_redirection_count = 5,
            .keep_alive_enable = false,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) {
            strlcpy(err, "out of memory", sizeof(err));
        } else {
            esp_http_client_set_header(c, "User-Agent", "zigbee-hub");
            esp_err_t e = esp_http_client_perform(c);
            status = e == ESP_OK ? esp_http_client_get_status_code(c) : 0;
            if (e != ESP_OK) snprintf(err, sizeof(err), "download failed: %s", esp_err_to_name(e));
            esp_http_client_cleanup(c);
        }
    }
    if (!err[0] && status != 200) snprintf(err, sizeof(err), "download failed: HTTP %d", status);
    if (!err[0] && sink.too_big) snprintf(err, sizeof(err), "larger than %d KB, or out of memory", IMG_MAX / 1024);
    if (!err[0]) {
        uint8_t got[64];
        size_t olen = 0;
        psa_crypto_init();
        if (psa_hash_compute(PSA_ALG_SHA_512, sink.buf, sink.len, got, sizeof(got), &olen) != PSA_SUCCESS ||
            olen != 64) {
            strlcpy(err, "sha512 failed", sizeof(err));
        } else if (memcmp(got, want, 64) != 0) {
            strlcpy(err, "sha512 does not match", sizeof(err));
        }
    }
    if (!err[0]) check_image(sink.buf, sink.len, err, sizeof(err));
    OLOCK();
    if (s_ota.st != ST_FETCHING) {
        free(sink.buf);  // cancelled meanwhile
    } else if (err[0]) {
        free(sink.buf);
        fail_locked("%s", err);
    } else {
        s_ota.img = sink.buf;
        s_ota.len = sink.len;
        s_ota.st = ST_STAGED;
        ESP_LOGI(TAG, "staged for %016llx: %" PRIu32 " bytes, manufacturer 0x%04x, image type 0x%04x, file 0x%08" PRIx32
                 " ('%s')", (unsigned long long)s_ota.ieee, s_ota.len, s_ota.manuf, s_ota.image_type, s_ota.file_ver,
                 s_ota.hdr_str);
    }
    bool go = s_ota.st == ST_STAGED && s_ota.auto_start;
    OUNLOCK();
    zb_dev_notify();
    if (go) {
        esp_err_t e = zb_ota_start();
        if (e != ESP_OK) {
            OLOCK();
            if (s_ota.st == ST_STAGED) fail_locked("staged, but could not be offered: %s", esp_err_to_name(e));
            OUNLOCK();
        }
    }
    vTaskDeleteWithCaps(NULL);
}

// ---- public API (any task) ----------------------------------------------------------------

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Drops the image (Zigbee task, or no image handed to the stack)
static void release_locked(void)
{
    if (s_ota.fh) {
        ezb_zcl_ota_upgrade_remove_ota_file(ZB_HUB_EP, s_ota.fh);
        s_ota.fh = NULL;
    }
    free(s_ota.img);
    s_ota.img = NULL;
    s_ota.len = 0;
}

esp_err_t zb_ota_stage(const char *ieee_hex, const char *url, const char *sha512_hex)
{
    if (!ieee_hex || !url || !sha512_hex || strlen(sha512_hex) != 128 || strlen(url) >= sizeof(s_ota.url) ||
        (strncasecmp(url, "https://", 8) != 0 && strncasecmp(url, "http://", 7) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t sha[64];
    for (int i = 0; i < 64; i++) {
        int hi = hexval(sha512_hex[2 * i]), lo = hexval(sha512_hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return ESP_ERR_INVALID_ARG;
        sha[i] = (uint8_t)(hi << 4 | lo);
    }
    uint64_t ieee = strtoull(ieee_hex, NULL, 16);
    uint16_t sa, m, t;
    uint8_t ep;
    bool sleepy;
    uint32_t v;
    if (!zb_dev_ota_info(ieee, &sa, &ep, &sleepy, &m, &t, &v)) return ESP_ERR_NOT_FOUND;
    OLOCK();
    if (s_ota.st == ST_FETCHING || s_ota.st == ST_OFFERED || s_ota.st == ST_SENDING) {
        OUNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ota.fh) {
        OUNLOCK();
        return ESP_ERR_INVALID_STATE;  // still with the stack: zb_ota_cancel first
    }
    free(s_ota.img);
    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.st = ST_FETCHING;
    s_ota.ieee = ieee;
    strlcpy(s_ota.url, url, sizeof(s_ota.url));
    memcpy(s_ota.sha512, sha, 64);
    OUNLOCK();
    // PSRAM stack: the task never touches flash
    if (xTaskCreateWithCaps(fetch_task, "zb_ota_get", 10 * 1024, NULL, 2, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
        pdPASS) {
        OLOCK();
        fail_locked("no memory for the download task");
        OUNLOCK();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "fetching for %016llx: %s", (unsigned long long)ieee, url);
    return ESP_OK;
}

static void notify_locked(void)
{
    ezb_zcl_ota_upgrade_image_notify_cmd_t n = {
        .cmd_ctrl = {
            .dst_addr = {.addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = s_ota.short_addr},
            .dst_ep = s_ota.ota_ep,
            .src_ep = ZB_HUB_EP,
        },
        // Jitter only: some devices don't answer a notify that names manufacturer, image type and
        // version. Which device gets the image is decided when it asks
        // (zb_ota_on_server_progress), not by the notify
        .payload = {
            .type = EZB_ZCL_OTA_UPGRADE_IMAGE_NOTIFY_PAYLOAD_TYPE_JITTER,
            .query_jitter = 100,  // every device that gets it asks at once
        },
    };
    ezb_err_t e = ezb_zcl_ota_upgrade_image_notify_cmd_req(&n);
    s_ota.notifies++;
    s_ota.notify_us = esp_timer_get_time();
    s_ota.notify_pending = false;
    ESP_LOGI(TAG, "0x%04x: image notify %d sent (0x%x)", s_ota.short_addr, s_ota.notifies, e);
}

static void start_on_zb_task(void *arg)
{
    OLOCK();
    if (s_ota.st == ST_STAGED && s_ota.img && !s_ota.fh) {
        s_ota.fh = ezb_zcl_ota_upgrade_add_ota_file(ZB_HUB_EP, s_ota.img, s_ota.len);
        if (!s_ota.fh) {
            fail_locked("the stack's OTA server refused the image");
        } else {
            s_ota.st = ST_OFFERED;
            s_ota.started_us = esp_timer_get_time();
            s_ota.notifies = 0;
            ESP_LOGI(TAG, "offered to 0x%04x (%s)", s_ota.short_addr, s_ota.sleepy ? "sleepy: told when next heard" : "told now");
            if (s_ota.sleepy) s_ota.notify_pending = true;
            else notify_locked();
        }
    }
    OUNLOCK();
    zb_dev_notify();
}

esp_err_t zb_ota_start(void)
{
    if (!zb_is_up()) return ESP_ERR_INVALID_STATE;
    OLOCK();
    bool ok = s_ota.st == ST_STAGED;
    uint64_t ieee = s_ota.ieee;
    OUNLOCK();
    if (!ok) return ESP_ERR_INVALID_STATE;
    // The device's short address or OTA endpoint may have changed since staging
    uint16_t sa, m, t;
    uint8_t ep;
    bool sleepy;
    uint32_t v;
    if (!zb_dev_ota_info(ieee, &sa, &ep, &sleepy, &m, &t, &v)) return ESP_ERR_NOT_FOUND;
    OLOCK();
    s_ota.short_addr = sa;
    s_ota.ota_ep = ep;
    s_ota.sleepy = sleepy;
    s_ota.from_ver = v;
    bool type_ok = t == 0xFFFF || t == s_ota.image_type;
    OUNLOCK();
    if (!type_ok) return ESP_ERR_INVALID_RESPONSE;
    return esp_zigbee_task_queue_post(start_on_zb_task, NULL);
}

static void cancel_on_zb_task(void *arg)
{
    OLOCK();
    if (s_ota.st != ST_IDLE) ESP_LOGI(TAG, "cancelled (%s)", ST_NAME[s_ota.st]);
    release_locked();
    memset(&s_ota, 0, sizeof(s_ota));
    OUNLOCK();
    zb_dev_notify();
}

esp_err_t zb_ota_cancel(void)
{
    OLOCK();
    bool with_stack = s_ota.fh != NULL;
    if (!with_stack) {
        if (s_ota.st != ST_IDLE) ESP_LOGI(TAG, "cancelled (%s)", ST_NAME[s_ota.st]);
        free(s_ota.img);
        memset(&s_ota, 0, sizeof(s_ota));  // a running fetch sees ST_IDLE and drops its buffer
    }
    OUNLOCK();
    if (!with_stack) zb_dev_notify();
    return with_stack ? esp_zigbee_task_queue_post(cancel_on_zb_task, NULL) : ESP_OK;
}

static bool sha_hex_ok(const char *s)
{
    if (!s || strlen(s) != 128) return false;
    for (int i = 0; i < 128; i++) {
        if (hexval(s[i]) < 0) return false;
    }
    return true;
}

esp_err_t zb_ota_offer(const char *ieee_hex, uint32_t file_ver, const char *url, const char *sha512_hex,
                       const char *notes)
{
    if (!ieee_hex) return ESP_ERR_INVALID_ARG;
    uint64_t ieee = strtoull(ieee_hex, NULL, 16);
    if (!url) return zb_dev_set_offer(ieee, 0, NULL, NULL, NULL);
    if (strlen(url) >= 200 || (strncasecmp(url, "https://", 8) != 0 && strncasecmp(url, "http://", 7) != 0) ||
        !sha_hex_ok(sha512_hex)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = zb_dev_set_offer(ieee, file_ver, url, sha512_hex, notes);
    if (err == ESP_OK) ESP_LOGI(TAG, "%s: offer of file %" PRIu32 ": %s", ieee_hex, file_ver, url);
    return err;
}

esp_err_t zb_ota_install(const char *ieee_hex)
{
    char url[200], sha[129];
    if (!ieee_hex || !zb_dev_get_offer(strtoull(ieee_hex, NULL, 16), url, sizeof(url), sha, sizeof(sha))) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = zb_ota_stage(ieee_hex, url, sha);
    if (err == ESP_OK) {
        OLOCK();
        if (s_ota.st == ST_FETCHING || s_ota.st == ST_STAGED) s_ota.auto_start = true;
        bool staged = s_ota.st == ST_STAGED;  // a fetch faster than this line
        OUNLOCK();
        if (staged) err = zb_ota_start();
    }
    return err;
}

bool zb_ota_device(const char *ieee_hex, float *progress)
{
    uint64_t ieee = strtoull(ieee_hex, NULL, 16);
    OLOCK();
    bool busy = s_ota.ieee == ieee && (s_ota.st == ST_FETCHING || s_ota.st == ST_OFFERED || s_ota.st == ST_SENDING ||
                                       (s_ota.st == ST_STAGED && s_ota.auto_start));
    *progress = s_ota.st == ST_SENDING && s_ota.len ? (float)s_ota.offset * 100.0f / s_ota.len : -1;
    OUNLOCK();
    return busy;
}

bool zb_ota_busy(void)
{
    OLOCK();
    bool busy = s_ota.st == ST_FETCHING || s_ota.st == ST_SENDING;
    OUNLOCK();
    return busy;
}

cJSON *zb_ota_json(void)
{
    cJSON *o = cJSON_CreateObject();
    char hex[17];
    OLOCK();
    cJSON_AddStringToObject(o, "state", ST_NAME[s_ota.st]);
    if (s_ota.st != ST_IDLE) {
        snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)s_ota.ieee);
        cJSON_AddStringToObject(o, "ieee", hex);
        cJSON_AddStringToObject(o, "url", s_ota.url);
    }
    if (s_ota.st == ST_FETCHING) cJSON_AddNumberToObject(o, "fetched", s_ota.got);
    if (s_ota.img) {
        cJSON_AddNumberToObject(o, "size", s_ota.len);
        cJSON_AddNumberToObject(o, "manuf_code", s_ota.manuf);
        cJSON_AddNumberToObject(o, "image_type", s_ota.image_type);
        cJSON_AddNumberToObject(o, "file_ver", s_ota.file_ver);
        cJSON_AddStringToObject(o, "header", s_ota.hdr_str);
        if (s_ota.from_ver != 0xFFFFFFFF) cJSON_AddNumberToObject(o, "from_ver", s_ota.from_ver);
    }
    if (s_ota.st == ST_OFFERED) cJSON_AddNumberToObject(o, "notifies", s_ota.notifies);
    if (s_ota.st == ST_SENDING || s_ota.st == ST_DONE) {
        cJSON_AddNumberToObject(o, "offset", s_ota.offset);
        if (s_ota.len) cJSON_AddNumberToObject(o, "progress", (double)s_ota.offset * 100.0 / s_ota.len);
        cJSON_AddNumberToObject(o, "elapsed_s", (double)((esp_timer_get_time() - s_ota.started_us) / 1000000));
    }
    if (s_ota.err[0]) cJSON_AddStringToObject(o, "error", s_ota.err);
    OUNLOCK();
    return o;
}

// ---- Zigbee task ------------------------------------------------------------------------------

bool zb_ota_on_raw(const ezb_zcl_raw_frame_t *f)
{
    const ezb_zcl_cmd_hdr_t *h = f ? f->header : NULL;
    if (!h || h->cluster_id != EZB_ZCL_CLUSTER_ID_OTA_UPGRADE || h->src_addr.addr_mode != EZB_ADDR_MODE_SHORT ||
        EZB_ZCL_CMD_FC_GET_FRAME_TYPE(h->fc) != EZB_ZCL_FRAME_TYPE_CLUSTER_SPECIFIC ||
        h->cmd_id != EZB_ZCL_CMD_OTA_UPGRADE_QUERY_NEXT_IMAGE_REQUEST_ID) {
        return false;
    }
    // field control (1), manufacturer code (2), image type (2), file version (4), [hardware version (2)]
    if (!f->payload || f->payload_length < 9) return false;
    const uint8_t *p = f->payload;
    uint16_t manuf = le16(p + 1), type = le16(p + 3);
    uint32_t ver = le32(p + 5);
    int hw = (p[0] & EZB_ZCL_OTA_UPGRADE_QUERY_NEXT_IMAGE_FC_HW_VERSION_PRESENT) && f->payload_length >= 11 ? le16(p + 9) : -1;
    ESP_LOGI(TAG, "0x%04x: query next image: manufacturer 0x%04x, image type 0x%04x, file 0x%08" PRIx32 ", hw %d",
             h->src_addr.u.short_addr, manuf, type, ver, hw);
    zb_dev_ota_query(h->src_addr.u.short_addr, h->src_ep, manuf, type, ver);
    return false;  // the stack answers it
}

static void finish_on_zb_task(void *arg)
{
    OLOCK();
    if (s_ota.fh) {
        ezb_zcl_ota_upgrade_remove_ota_file(ZB_HUB_EP, s_ota.fh);
        s_ota.fh = NULL;
    }
    free(s_ota.img);
    s_ota.img = NULL;
    OUNLOCK();
}

void zb_ota_on_server_progress(ezb_zcl_ota_upgrade_server_progress_message_t *m)
{
    uint16_t from = m->in.header && m->in.header->src_addr.addr_mode == EZB_ADDR_MODE_SHORT
                        ? m->in.header->src_addr.u.short_addr : 0xFFFF;
    uint16_t done_addr = 0xFFFF;
    OLOCK();
    bool active = s_ota.st == ST_OFFERED || s_ota.st == ST_SENDING;
    st_t st0 = s_ota.st;
    uint32_t pct0 = s_ota.len ? s_ota.offset * 100 / s_ota.len : 0;
    switch (m->in.progress) {
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_START: {
        const typeof(m->in.start.query) *q = &m->in.start.query;
        const char *why = !active                                 ? "nothing offered"
                        : q->short_address != s_ota.short_addr    ? "not the target device"
                        : q->manuf_code != s_ota.manuf            ? "other manufacturer code"
                        : q->image_type != s_ota.image_type       ? "other image type"
                        : q->file_version == s_ota.file_ver       ? "already runs this version"
                        : NULL;
        m->out.result = why ? EZB_ZCL_STATUS_NO_IMAGE_AVAILABLE : EZB_ZCL_STATUS_SUCCESS;
        if (why) {
            if (active) {
                ESP_LOGI(TAG, "0x%04x: query for 0x%04x/0x%04x/0x%08" PRIx32 " refused: %s", q->short_address,
                         q->manuf_code, q->image_type, q->file_version, why);
            }
            if (active && q->short_address == s_ota.short_addr && q->image_type != s_ota.image_type) {
                fail_locked("the device asked for image type 0x%04x, the staged image is 0x%04x: not sent",
                            q->image_type, s_ota.image_type);
                esp_zigbee_task_queue_post(finish_on_zb_task, NULL);
            } else if (active && q->short_address == s_ota.short_addr && q->file_version == s_ota.file_ver) {
                // back after an update, or already up to date
                s_ota.st = ST_DONE;
                ESP_LOGI(TAG, "0x%04x runs file 0x%08" PRIx32 " now", q->short_address, q->file_version);
                esp_zigbee_task_queue_post(finish_on_zb_task, NULL);
            }
        } else {
            s_ota.st = ST_SENDING;
            s_ota.from_ver = q->file_version;
            s_ota.offset = 0;
            s_ota.started_us = s_ota.block_us = esp_timer_get_time();
            ESP_LOGI(TAG, "0x%04x: query admitted, file 0x%08" PRIx32 " -> 0x%08" PRIx32 ", %" PRIu32 " bytes",
                     q->short_address, q->file_version, s_ota.file_ver, s_ota.len);
        }
        break;
    }
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_SENDING:
        if (active && from == s_ota.short_addr) {
            m->out.result = EZB_ZCL_STATUS_SUCCESS;
            s_ota.st = ST_SENDING;
            uint32_t pct_was = s_ota.len ? s_ota.offset * 10 / s_ota.len : 0;
            s_ota.offset = m->in.sending.file_offset;
            s_ota.block_us = esp_timer_get_time();
            uint32_t pct = s_ota.len ? s_ota.offset * 10 / s_ota.len : 0;
            if (pct != pct_was) {
                ESP_LOGI(TAG, "0x%04x: %" PRIu32 "0 %% (%" PRIu32 " of %" PRIu32 " bytes, %lld s)", from, pct,
                         s_ota.offset, s_ota.len, (s_ota.block_us - s_ota.started_us) / 1000000);
            }
        } else {
            m->out.result = EZB_ZCL_STATUS_NO_IMAGE_AVAILABLE;
        }
        break;
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_FINISH:
        m->out.result = EZB_ZCL_STATUS_SUCCESS;
        // Upgrade now: a current time of 0 makes the upgrade time an offset from now
        m->out.finish.current_time = 0;
        m->out.finish.upgrade_time = 0;
        if (active && from == s_ota.short_addr) {
            s_ota.end_status = m->in.finish.status;
            if (m->in.finish.status == EZB_ZCL_STATUS_SUCCESS) {
                s_ota.st = ST_DONE;
                s_ota.offset = s_ota.len;
                done_addr = from;
                ESP_LOGI(TAG, "0x%04x: upgrade end, success after %lld s: it restarts into file 0x%08" PRIx32, from,
                         (esp_timer_get_time() - s_ota.started_us) / 1000000, s_ota.file_ver);
            } else {
                fail_locked("the device ended the upgrade with status 0x%02x", m->in.finish.status);
            }
            esp_zigbee_task_queue_post(finish_on_zb_task, NULL);
        }
        break;
    case EZB_ZCL_OTA_UPGRADE_PROGRESS_ABORT:
        m->out.result = EZB_ZCL_STATUS_SUCCESS;
        if (active && (from == s_ota.short_addr || from == 0xFFFF)) {
            fail_locked("transfer aborted");
            esp_zigbee_task_queue_post(finish_on_zb_task, NULL);
        }
        break;
    default:
        m->out.result = EZB_ZCL_STATUS_SUCCESS;
        break;
    }
    bool moved = s_ota.st != st0 || (s_ota.len && s_ota.offset * 100 / s_ota.len != pct0);
    OUNLOCK();
    if (done_addr != 0xFFFF) zb_dev_ota_done(done_addr);
    if (moved) zb_dev_notify();  // the UI and Home Assistant follow by the percent, not by the block
}

void zb_ota_heard(uint16_t short_addr)
{
    OLOCK();
    if (s_ota.st == ST_OFFERED && s_ota.notify_pending && short_addr == s_ota.short_addr) notify_locked();
    OUNLOCK();
}

void zb_ota_tick(void)
{
    int64_t now = esp_timer_get_time();
    OLOCK();
    if (s_ota.st == ST_OFFERED && s_ota.notifies >= NOTIFY_MAX && now - s_ota.notify_us >= NOTIFY_EVERY_US) {
        fail_locked("the device didn't ask for the image (%d notifies)", s_ota.notifies);
        release_locked();
    } else if (s_ota.st == ST_OFFERED && !s_ota.sleepy && now - s_ota.notify_us >= NOTIFY_EVERY_US) {
        notify_locked();
    } else if (s_ota.st == ST_OFFERED && s_ota.sleepy && !s_ota.notify_pending &&
               now - s_ota.notify_us >= NOTIFY_EVERY_US && s_ota.notifies < NOTIFY_MAX) {
        s_ota.notify_pending = true;  // tell it again the next time it's heard from
    } else if (s_ota.st == ST_SENDING && now - s_ota.block_us > STALL_US) {
        fail_locked("transfer stalled at %" PRIu32 " of %" PRIu32 " bytes", s_ota.offset, s_ota.len);
        release_locked();
    }
    OUNLOCK();
}

esp_err_t zb_ota_init(void)
{
    s_omtx = xSemaphoreCreateMutex();
    return s_omtx ? ESP_OK : ESP_ERR_NO_MEM;
}

// Image Notify with only a jitter: the device answers with a Query Next Image, which says
// what it runs. Nothing is offered unless an image was started for it.
static void ask_on_zb_task(void *arg)
{
    uint32_t v = (uint32_t)(uintptr_t)arg;
    ezb_zcl_ota_upgrade_image_notify_cmd_t n = {
        .cmd_ctrl = {
            .dst_addr = {.addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = (uint16_t)(v & 0xFFFF)},
            .dst_ep = (uint8_t)(v >> 16),
            .src_ep = ZB_HUB_EP,
        },
        .payload = {.type = EZB_ZCL_OTA_UPGRADE_IMAGE_NOTIFY_PAYLOAD_TYPE_JITTER, .query_jitter = 100},
    };
    ezb_err_t e = ezb_zcl_ota_upgrade_image_notify_cmd_req(&n);
    ESP_LOGI(TAG, "0x%04x: asked what it runs (image notify, 0x%x)", (unsigned)(v & 0xFFFF), e);
}

esp_err_t zb_ota_ask(const char *ieee_hex)
{
    uint16_t sa, m, t;
    uint8_t ep;
    bool sleepy;
    uint32_t ver;
    if (!ieee_hex || !zb_dev_ota_info(strtoull(ieee_hex, NULL, 16), &sa, &ep, &sleepy, &m, &t, &ver)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (sleepy) return ESP_ERR_NOT_SUPPORTED;  // asleep: it asks by itself, now and then
    return esp_zigbee_task_queue_post(ask_on_zb_task, (void *)(uintptr_t)((uint32_t)ep << 16 | sa));
}
