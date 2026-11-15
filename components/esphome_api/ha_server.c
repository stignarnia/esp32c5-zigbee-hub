// ESPHome native API server: TCP 6053, Noise only. One task does everything: accepts,
// reads, writes and closes every socket (lwIP: a socket is closed by the task that reads
// it), refreshes the entity model and pushes state changes.
//
// Frames (both directions): 0x01, be16 length, payload. Handshake payloads are bounded
// to 128 bytes and data frames to RX_CAP before anything is buffered (the reference
// project had a pre-auth overflow from an unchecked hello length). A data payload is
// ChaCha20-Poly1305 of: be16 message type, be16 length, protobuf.
//
// Never in Zigbee's way: it reads snapshots (zb_snapshot takes the registry lock for a
// copy), is woken by polling revisions every 100 ms, and blocks on nothing but its own
// sockets (send timeout 2 s, then that client is dropped).
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esphome_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ha_internal.h"
#include "hub_crypto.h"
#include "hubsys.h"
#include "mbedtls/base64.h"
#include "mdns.h"
#include "nvs.h"

static const char *TAG = "ha";

#define MAX_CLIENTS        3
#define RX_CAP             4096            // largest frame accepted from a client
#define HANDSHAKE_MAX      128
#define TX_CAP             8192            // frames batched per send()
#define MSG_CAP            3072            // one message (DeviceInfo with 48 sub-devices ~2 KB)
#define POLL_US            (1000 * 1000)   // inputs re-read at least this often
#define SLOW_US            (60LL * 1000000)
#define SETTLE_US          (3LL * 1000000) // entity set must be stable this long before clients reload
#define HANDSHAKE_TMO_US   (10LL * 1000000)
#define KEEPALIVE_US       (60LL * 1000000)
#define DEAD_US            (150LL * 1000000)
#define PERMIT_S           120
#define NVS_NS             "ha"
#define TASK_STACK         5120            // internal: a SwitchCommand saves the script list to flash (used 2.5 KB)

#define API_MAJOR          1
#define API_MINOR          19              // what ESPHome 2026.10 speaks
#define ESPHOME_COMPAT     "2026.10.2"
// ESPHome's "author.project" format: Home Assistant splits it at the dot into manufacturer and
// model (and crashes on connect, silently, if there is no dot) and then shows project_version
// as the firmware version
#define PROJECT_NAME       "Espressif.ESP32-C5 Zigbee hub"

enum { MSG_HELLO_REQ = 1, MSG_HELLO_RSP = 2, MSG_AUTH_REQ = 3, MSG_DISC_REQ = 5, MSG_DISC_RSP = 6,
       MSG_PING_REQ = 7, MSG_PING_RSP = 8, MSG_DEVINFO_REQ = 9, MSG_DEVINFO_RSP = 10, MSG_LIST_REQ = 11,
       MSG_LIST_DONE = 19, MSG_SUBSCRIBE_STATES = 20, MSG_LIST_BINARY = 12, MSG_LIST_SENSOR = 16,
       MSG_LIST_SWITCH = 17, MSG_LIST_TEXT = 18, MSG_LIST_BUTTON = 61, MSG_LIST_EVENT = 107,
       MSG_ST_BINARY = 21, MSG_ST_SENSOR = 25, MSG_ST_SWITCH = 26, MSG_ST_TEXT = 27, MSG_EVENT = 108,
       MSG_SWITCH_CMD = 33, MSG_BUTTON_CMD = 62, MSG_CAPS_REQ = 149, MSG_CAPS_RSP = 150 };

typedef enum { CS_FREE = 0, CS_HELLO, CS_HANDSHAKE, CS_DATA } cstate_t;

typedef struct {
    int fd;
    uint8_t state;
    bool subscribed;
    size_t rx_len;
    int64_t opened_us, last_rx_us, ping_us;
    char peer[16];
    char info[48];
    ha_noise_t nz;
    uint8_t rx[3 + RX_CAP];
    uint32_t sent_hash[HA_MAX_ENT];
    bool sent_ok[HA_MAX_ENT];
} client_t;

typedef struct {
    int listen_fd;
    client_t cl[MAX_CLIENTS];
    ha_inputs_t in;
    ha_model_t *model, *next;     // listed to clients / just built
    ha_model_t models[2];
    ha_state_t cur[HA_MAX_ENT];
    uint32_t zrev, arev;
    int64_t next_poll_us, next_slow_us, pending_us;
    bool slow_now;
    uint8_t dec[RX_CAP];          // decrypted incoming payload
    uint8_t msg[4 + MSG_CAP];     // header room + protobuf
    uint8_t tx[TX_CAP];
    size_t tx_len;
} srv_t;

typedef struct {
    char script[AUTO_NAME_MAX + 1];
    char name[AUTO_HA_NAME + 1];
    char type[AUTO_HA_TYPE_LEN];
} ev_item_t;

static SemaphoreHandle_t s_api_mtx;         // settings calls (HTTP task)
static portMUX_TYPE s_key_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_key[32];
static bool s_have_key, s_enabled;
static char s_host[33];
static char s_mac_hex[13], s_mac_pretty[18];
static srv_t *s_srv;                        // PSRAM, only while running
static TaskHandle_t s_task;
static volatile bool s_running, s_stop, s_kick;
static volatile int s_clients, s_entities, s_devices;
static volatile uint32_t s_connects, s_rejected;
static char s_last_client[48];
static bool (*s_filter)(int fd);
static QueueHandle_t s_evq;                 // ha.event() from the automation task

// ---- settings ------------------------------------------------------------------------------------

static esp_err_t save_settings(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "on", s_enabled);
    if (err == ESP_OK && s_have_key) err = nvs_set_blob(h, "key", s_key, 32);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void new_key(void)
{
    uint8_t k[32];
    esp_fill_random(k, sizeof(k));
    taskENTER_CRITICAL(&s_key_lock);
    memcpy(s_key, k, 32);
    s_have_key = true;
    taskEXIT_CRITICAL(&s_key_lock);
    memset(k, 0, sizeof(k));
}

static void key_copy(uint8_t out[32])
{
    taskENTER_CRITICAL(&s_key_lock);
    memcpy(out, s_key, 32);
    taskEXIT_CRITICAL(&s_key_lock);
}

// ---- mDNS ---------------------------------------------------------------------------------------

static void mdns_add(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    mdns_txt_item_t txt[] = {
        {"friendly_name", "Zigbee Hub"},
        {"version", ESPHOME_COMPAT},
        {"mac", s_mac_hex},
        {"platform", "ESP32"},
        {"board", "esp32-c5"},
        {"network", "wifi"},
        {"api_encryption", "Noise_NNpsk0_25519_ChaChaPoly_SHA256"},
        {"project_name", PROJECT_NAME},
        {"project_version", app->version},
    };
    esp_err_t err = mdns_service_add(s_host, "_esphomelib", "_tcp", HA_API_PORT, txt, sizeof(txt) / sizeof(txt[0]));
    if (err == ESP_OK) ESP_LOGI(TAG, "mdns: _esphomelib._tcp on %d", HA_API_PORT);
}

static void mdns_del(void)
{
    mdns_service_remove("_esphomelib", "_tcp");
}

void ha_mdns_announce(void)
{
    if (s_running) mdns_add();
}

// ---- sockets ------------------------------------------------------------------------------------

static bool send_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static void close_client(client_t *c, const char *why)
{
    if (c->state == CS_FREE) return;
    if (c->state == CS_DATA) {
        s_clients--;
        ESP_LOGI(TAG, "%s (%s) disconnected: %s", c->info[0] ? c->info : "client", c->peer, why);
    } else {
        ESP_LOGD(TAG, "%s: closed during the handshake: %s", c->peer, why);
    }
    shutdown(c->fd, SHUT_RDWR);
    close(c->fd);
    ha_noise_wipe(&c->nz);
    c->state = CS_FREE;
    c->fd = -1;
}

// An unencrypted handshake frame (status byte first).
static bool send_raw_frame(client_t *c, const uint8_t *p, size_t n)
{
    uint8_t hdr[3] = {0x01, (uint8_t)(n >> 8), (uint8_t)n};
    return send_all(c->fd, hdr, 3) && send_all(c->fd, p, n);
}

// Tells the client why (aioesphomeapi shows "Handshake MAC failure" as a wrong key),
// then closes.
static void reject(client_t *c, const char *reason)
{
    uint8_t buf[40];
    size_t n = strlen(reason);
    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    buf[0] = 0x01;
    memcpy(buf + 1, reason, n);
    send_raw_frame(c, buf, n + 1);
    s_rejected++;
    ESP_LOGW(TAG, "%s: handshake refused: %s", c->peer, reason);
    close_client(c, reason);
}

// ---- outgoing messages: encode into S->msg + 4, encrypt into S->tx --------------------------------

static pb_w_t msg_begin(srv_t *S)
{
    return (pb_w_t){.buf = S->msg + 4, .cap = MSG_CAP};
}

static bool out_flush(srv_t *S, client_t *c)
{
    bool ok = S->tx_len == 0 || send_all(c->fd, S->tx, S->tx_len);
    S->tx_len = 0;
    if (!ok) close_client(c, "send failed");
    return ok;
}

static bool out_msg(srv_t *S, client_t *c, uint16_t type, const pb_w_t *w)
{
    if (c->state != CS_DATA) return false;
    if (w->ovf) {
        ESP_LOGE(TAG, "message %u too large, not sent", type);
        return true;
    }
    size_t plen = 4 + w->len, flen = 3 + plen + HUB_AEAD_TAG;
    if (S->tx_len + flen > TX_CAP && !out_flush(S, c)) return false;
    uint8_t *f = S->tx + S->tx_len;
    S->msg[0] = type >> 8;
    S->msg[1] = type & 0xFF;
    S->msg[2] = w->len >> 8;
    S->msg[3] = w->len & 0xFF;
    f[0] = 0x01;
    f[1] = (plen + HUB_AEAD_TAG) >> 8;
    f[2] = (plen + HUB_AEAD_TAG) & 0xFF;
    if (!ha_noise_encrypt(&c->nz, S->msg, plen, f + 3)) {
        close_client(c, "encrypt failed");
        return false;
    }
    S->tx_len += flen;
    return true;
}

static bool out_empty(srv_t *S, client_t *c, uint16_t type)
{
    pb_w_t w = msg_begin(S);
    return out_msg(S, c, type, &w);
}

// ---- entities -----------------------------------------------------------------------------------

static bool list_entity(srv_t *S, client_t *c, const ha_ent_t *e)
{
    pb_w_t w = msg_begin(S);
    uint16_t type = 0;
    pb_fixed32(&w, 2, e->key);
    pb_str(&w, 3, e->name);
    switch (e->type) {
    case ET_BINARY:
        type = MSG_LIST_BINARY;
        pb_str(&w, 5, e->device_class);
        pb_bool(&w, 7, e->disabled);
        pb_str(&w, 8, e->icon);
        pb_varint(&w, 9, e->category);
        pb_varint(&w, 10, e->device_id);
        break;
    case ET_SENSOR:
        type = MSG_LIST_SENSOR;
        pb_str(&w, 5, e->icon);
        pb_str(&w, 6, e->unit);
        pb_varint(&w, 7, (uint32_t)e->decimals);
        pb_str(&w, 9, e->device_class);
        pb_varint(&w, 10, e->state_class);
        pb_bool(&w, 12, e->disabled);
        pb_varint(&w, 13, e->category);
        pb_varint(&w, 14, e->device_id);
        break;
    case ET_SWITCH:
        type = MSG_LIST_SWITCH;
        pb_str(&w, 5, e->icon);
        pb_bool(&w, 7, e->disabled);
        pb_varint(&w, 8, e->category);
        pb_str(&w, 9, e->device_class);
        pb_varint(&w, 10, e->device_id);
        break;
    case ET_TEXT:
        type = MSG_LIST_TEXT;
        pb_str(&w, 5, e->icon);
        pb_bool(&w, 6, e->disabled);
        pb_varint(&w, 7, e->category);
        pb_str(&w, 8, e->device_class);
        pb_varint(&w, 9, e->device_id);
        break;
    case ET_BUTTON:
        type = MSG_LIST_BUTTON;
        pb_str(&w, 5, e->icon);
        pb_bool(&w, 6, e->disabled);
        pb_varint(&w, 7, e->category);
        pb_str(&w, 8, e->device_class);
        pb_varint(&w, 9, e->device_id);
        break;
    case ET_EVENT:
        type = MSG_LIST_EVENT;
        pb_str(&w, 5, e->icon);
        pb_bool(&w, 6, e->disabled);
        pb_varint(&w, 7, e->category);
        pb_str(&w, 8, e->device_class);
        for (int t = 0; t < e->n_types; t++) pb_str(&w, 9, e->types[t]);
        pb_varint(&w, 10, e->device_id);
        break;
    default:
        return true;
    }
    return out_msg(S, c, type, &w);
}

static uint32_t state_hash(const ha_state_t *st)
{
    uint32_t h = 2166136261u;
    uint8_t hdr[2] = {st->missing, st->b};
    const uint8_t *p = hdr;
    for (int i = 0; i < 2; i++) h = (h ^ p[i]) * 16777619u;
    p = (const uint8_t *)&st->f;
    for (int i = 0; i < 4; i++) h = (h ^ p[i]) * 16777619u;
    for (const char *s = st->text; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static bool send_state(srv_t *S, client_t *c, const ha_ent_t *e, const ha_state_t *st)
{
    pb_w_t w = msg_begin(S);
    pb_fixed32(&w, 1, e->key);
    switch (e->type) {
    case ET_BINARY:
        pb_bool(&w, 2, st->b);
        pb_bool(&w, 3, st->missing);
        pb_varint(&w, 4, e->device_id);
        return out_msg(S, c, MSG_ST_BINARY, &w);
    case ET_SENSOR:
        pb_float(&w, 2, st->f);
        pb_bool(&w, 3, st->missing);
        pb_varint(&w, 4, e->device_id);
        return out_msg(S, c, MSG_ST_SENSOR, &w);
    case ET_SWITCH:
        pb_bool(&w, 2, st->b);
        pb_varint(&w, 3, e->device_id);
        pb_bool(&w, 4, st->missing);
        return out_msg(S, c, MSG_ST_SWITCH, &w);
    case ET_TEXT:
        pb_str(&w, 2, st->text);
        pb_bool(&w, 3, st->missing);
        pb_varint(&w, 4, e->device_id);
        return out_msg(S, c, MSG_ST_TEXT, &w);
    default:
        return true;  // buttons and events have no state
    }
}

// Every entity whose state differs from what this client was last sent.
static void push_states(srv_t *S, client_t *c)
{
    const ha_model_t *m = S->model;
    for (int i = 0; i < m->n_ent && c->state == CS_DATA; i++) {
        const ha_ent_t *e = &m->ent[i];
        if (e->type == ET_BUTTON || e->type == ET_EVENT) continue;
        uint32_t h = state_hash(&S->cur[i]);
        if (c->sent_ok[i] && c->sent_hash[i] == h) continue;
        if (!send_state(S, c, e, &S->cur[i])) return;
        c->sent_ok[i] = true;
        c->sent_hash[i] = h;
    }
    out_flush(S, c);
}

static void send_device_info(srv_t *S, client_t *c)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char built[40];
    snprintf(built, sizeof(built), "%s, %s", app->date, app->time);
    pb_w_t w = msg_begin(S);
    pb_str(&w, 2, s_host);
    pb_str(&w, 3, s_mac_pretty);
    pb_str(&w, 4, ESPHOME_COMPAT);
    pb_str(&w, 5, built);
    pb_str(&w, 6, "ESP32-C5 Zigbee hub");
    pb_str(&w, 8, PROJECT_NAME);
    pb_str(&w, 9, app->version);
    pb_varint(&w, 10, 80);
    pb_str(&w, 12, "Espressif");
    pb_str(&w, 13, "Zigbee Hub");
    pb_bool(&w, 19, true);
    const ha_model_t *m = S->model;
    for (int i = 0; i < m->n_dev; i++) {
        uint8_t sub[64];
        pb_w_t d = {.buf = sub, .cap = sizeof(sub)};
        pb_varint(&d, 1, m->dev[i].id);
        pb_str(&d, 2, m->dev[i].name);
        if (!d.ovf) pb_bytes(&w, 20, sub, d.len);
    }
    out_msg(S, c, MSG_DEVINFO_RSP, &w);
}

static const ha_ent_t *ent_by_key(const srv_t *S, uint32_t key)
{
    for (int i = 0; i < S->model->n_ent; i++) {
        if (S->model->ent[i].key == key) return &S->model->ent[i];
    }
    return NULL;
}

static uint32_t cmd_key(const uint8_t *p, size_t n, bool *state)
{
    pb_r_t r = {p, p + n};
    pb_field_t f;
    uint32_t key = 0;
    *state = false;
    while (pb_next(&r, &f)) {
        if (f.field == 1 && f.wire == 5) key = (uint32_t)f.v;
        if (f.field == 2 && f.wire == 0) *state = f.v != 0;
    }
    return key;
}

static void handle_message(srv_t *S, client_t *c, uint16_t type, const uint8_t *p, size_t n)
{
    switch (type) {
    case MSG_HELLO_REQ: {
        pb_r_t r = {p, p + n};
        pb_field_t f;
        while (pb_next(&r, &f)) {
            if (f.field == 1 && f.wire == 2) {
                size_t k = 0;
                for (size_t i = 0; i < f.len && k < sizeof(c->info) - 1; i++) {
                    if (f.data[i] >= 0x20 && f.data[i] < 0x7f) c->info[k++] = (char)f.data[i];
                }
                c->info[k] = '\0';
            }
        }
        const esp_app_desc_t *app = esp_app_get_description();
        char server[48];
        snprintf(server, sizeof(server), "zigbee-hub %s", app->version);
        server[32] = '\0';  // the field's limit
        pb_w_t w = msg_begin(S);
        pb_varint(&w, 1, API_MAJOR);
        pb_varint(&w, 2, API_MINOR);
        pb_str(&w, 3, server);
        pb_str(&w, 4, s_host);
        out_msg(S, c, MSG_HELLO_RSP, &w);
        strlcpy(s_last_client, c->info, sizeof(s_last_client));
        ESP_LOGI(TAG, "%s connected from %s", c->info[0] ? c->info : "client", c->peer);
        break;
    }
    case MSG_AUTH_REQ:      // no passwords: the Noise key is the authentication
        break;
    case MSG_DISC_REQ:
        out_empty(S, c, MSG_DISC_RSP);
        out_flush(S, c);
        close_client(c, "client said goodbye");
        break;
    case MSG_DISC_RSP:
        close_client(c, "disconnect answered");
        break;
    case MSG_PING_REQ:
        out_empty(S, c, MSG_PING_RSP);
        break;
    case MSG_DEVINFO_REQ:
        send_device_info(S, c);
        break;
    case MSG_CAPS_REQ:      // no Bluetooth/voice/Z-Wave/serial proxy, no wizard: all defaults
        out_empty(S, c, MSG_CAPS_RSP);
        break;
    case MSG_LIST_REQ:
        for (int i = 0; i < S->model->n_ent; i++) {
            if (!list_entity(S, c, &S->model->ent[i])) return;
        }
        out_empty(S, c, MSG_LIST_DONE);
        break;
    case MSG_SUBSCRIBE_STATES:
        c->subscribed = true;
        memset(c->sent_ok, 0, sizeof(c->sent_ok));
        out_flush(S, c);
        push_states(S, c);
        break;
    case MSG_SWITCH_CMD: {
        bool on;
        const ha_ent_t *e = ent_by_key(S, cmd_key(p, n, &on));
        if (e && e->val == V_SCRIPT_ENABLED) {
            ESP_LOGI(TAG, "%s: script %s %s", c->info, e->ref, on ? "enabled" : "disabled");
            esp_err_t err = automation_set_enabled(e->ref, on);
            if (err != ESP_OK) ESP_LOGW(TAG, "script %s: %s", e->ref, esp_err_to_name(err));
            S->next_poll_us = 0;  // show the new state at once
        }
        break;
    }
    case MSG_BUTTON_CMD: {
        bool unused;
        const ha_ent_t *e = ent_by_key(S, cmd_key(p, n, &unused));
        if (e && e->val == V_HUB_PERMIT) {
            ESP_LOGI(TAG, "%s: permit join for %d s", c->info, PERMIT_S);
            zb_permit_join(PERMIT_S);
            S->next_poll_us = 0;
        }
        break;
    }
    default:                // subscriptions we don't serve (logs, HA states/services), unknown types
        ESP_LOGD(TAG, "message %u ignored", type);
        break;
    }
}

// ---- incoming frames ----------------------------------------------------------------------------

static void handle_frame(srv_t *S, client_t *c, const uint8_t *p, size_t n, int64_t now)
{
    switch (c->state) {
    case CS_HELLO: {
        // Prologue = "NoiseAPIInit" + be16 length + the client hello (its content is
        // reserved for flags; mixing it in makes a tampered hello fail the handshake)
        uint8_t pro[12 + 2 + HANDSHAKE_MAX];
        memcpy(pro, "NoiseAPIInit", 12);
        pro[12] = n >> 8;
        pro[13] = n & 0xFF;
        memcpy(pro + 14, p, n);
        // Server hello: chosen protocol 0x01, our name and MAC (Home Assistant checks both)
        uint8_t hello[1 + sizeof(s_host) + sizeof(s_mac_hex)];
        size_t hn = strlen(s_host) + 1, mn = strlen(s_mac_hex) + 1;
        hello[0] = 0x01;
        memcpy(hello + 1, s_host, hn);
        memcpy(hello + 1 + hn, s_mac_hex, mn);
        if (!send_raw_frame(c, hello, 1 + hn + mn)) {
            close_client(c, "send failed");
            return;
        }
        uint8_t key[32];
        key_copy(key);
        ha_noise_init(&c->nz, pro, 14 + n, key);
        memset(key, 0, sizeof(key));
        c->state = CS_HANDSHAKE;
        break;
    }
    case CS_HANDSHAKE: {
        if (n < 1 || p[0] != 0x00) {
            reject(c, "Bad handshake error byte");
            return;
        }
        if (!ha_noise_read_msg1(&c->nz, p + 1, n - 1)) {
            reject(c, "Handshake MAC failure");  // the wording clients match for "wrong key"
            return;
        }
        uint8_t out[1 + HA_NOISE_MSG2];
        out[0] = 0x00;
        if (!ha_noise_write_msg2(&c->nz, out + 1) || !send_raw_frame(c, out, sizeof(out))) {
            close_client(c, "handshake failed");
            return;
        }
        c->state = CS_DATA;
        c->subscribed = false;
        memset(c->sent_ok, 0, sizeof(c->sent_ok));
        s_clients++;
        s_connects++;
        break;
    }
    case CS_DATA: {
        if (n < 4 + HUB_AEAD_TAG || !ha_noise_decrypt(&c->nz, p, n, S->dec)) {
            close_client(c, "bad data frame");
            return;
        }
        size_t mlen = n - HUB_AEAD_TAG;
        uint16_t type = (uint16_t)(S->dec[0] << 8 | S->dec[1]);
        size_t dlen = (size_t)(S->dec[2] << 8 | S->dec[3]);
        if (dlen > mlen - 4) {
            close_client(c, "bad message length");
            return;
        }
        handle_message(S, c, type, S->dec + 4, dlen);
        break;
    }
    default:
        break;
    }
    (void)now;
}

static void read_client(srv_t *S, client_t *c, int64_t now)
{
    ssize_t r = recv(c->fd, c->rx + c->rx_len, sizeof(c->rx) - c->rx_len, MSG_DONTWAIT);
    if (r == 0) {
        close_client(c, "closed by the client");
        return;
    }
    if (r < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) close_client(c, "receive error");
        return;
    }
    c->rx_len += (size_t)r;
    c->last_rx_us = now;
    c->ping_us = 0;
    size_t off = 0;
    while (c->state != CS_FREE) {
        size_t avail = c->rx_len - off;
        if (avail < 1) break;
        if (c->rx[off] != 0x01) {
            if (c->state == CS_DATA) close_client(c, "bad frame indicator");
            else reject(c, "Bad indicator byte");  // a plaintext client: encryption is required
            return;
        }
        if (avail < 3) break;
        size_t len = (size_t)c->rx[off + 1] << 8 | c->rx[off + 2];
        if (len > (c->state == CS_DATA ? RX_CAP : HANDSHAKE_MAX)) {
            if (c->state == CS_DATA) close_client(c, "frame too large");
            else reject(c, "Bad handshake packet len");
            return;
        }
        if (avail < 3 + len) break;
        handle_frame(S, c, c->rx + off + 3, len, now);
        off += 3 + len;
    }
    if (c->state == CS_FREE) return;
    out_flush(S, c);
    if (c->state == CS_FREE) return;
    memmove(c->rx, c->rx + off, c->rx_len - off);
    c->rx_len -= off;
}

static void accept_client(srv_t *S, int64_t now)
{
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    int fd = accept(S->listen_fd, (struct sockaddr *)&a, &al);
    if (fd < 0) return;
    char peer[16];
    inet_ntoa_r(a.sin_addr, peer, sizeof(peer));
    if (s_filter && !s_filter(fd)) {
        ESP_LOGW(TAG, "%s refused: tailnet-only access", peer);
        s_rejected++;
        close(fd);
        return;
    }
    client_t *c = NULL;
    for (int i = 0; i < MAX_CLIENTS && !c; i++) {
        if (S->cl[i].state == CS_FREE) c = &S->cl[i];
    }
    if (!c) {
        ESP_LOGW(TAG, "%s refused: %d clients connected already", peer, MAX_CLIENTS);
        close(fd);
        return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval tv = {.tv_sec = 2};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    c->fd = fd;
    c->state = CS_HELLO;
    c->rx_len = 0;
    c->subscribed = false;
    c->opened_us = c->last_rx_us = now;
    c->ping_us = 0;
    c->info[0] = '\0';
    strlcpy(c->peer, peer, sizeof(c->peer));
}

// ---- model and states ---------------------------------------------------------------------------

static void close_all(srv_t *S, const char *why)
{
    for (int i = 0; i < MAX_CLIENTS; i++) close_client(&S->cl[i], why);
}

static void refresh_inputs(srv_t *S)
{
    ha_inputs_t *in = &S->in;
    in->n_zb = zb_snapshot(in->zb, ZB_MAX_DEVICES);
    in->n_scripts = automation_list(in->scripts, AUTO_MAX_SCRIPTS);
    in->n_sent = automation_ha_entities(in->sent, AUTO_HA_MAX);
}

static void apply_model(srv_t *S)
{
    ha_model_t *t = S->model;
    S->model = S->next;
    S->next = t;
    memset(S->cur, 0, sizeof(S->cur));
    for (int i = 0; i < HA_MAX_ENT; i++) S->cur[i].missing = true;
    S->slow_now = true;
    s_entities = S->model->n_ent;
    s_devices = S->model->n_dev;
}

static void update(srv_t *S, int64_t now)
{
    uint32_t zrev = zb_revision(), arev = automation_ha_revision();
    if (zrev == S->zrev && arev == S->arev && now < S->next_poll_us && !S->pending_us) return;
    S->zrev = zrev;
    S->arev = arev;
    S->next_poll_us = now + POLL_US;
    refresh_inputs(S);
    ha_model_build(S->next, &S->in);

    int data_clients = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) data_clients += S->cl[i].state == CS_DATA;
    if (S->next->sig != S->model->sig) {
        // Home Assistant learns entities only when it connects: reconnecting it is how it
        // picks up a new set. Wait until the set stops changing (an interview adds
        // clusters one after the other) so it reloads once.
        if (!S->pending_us) S->pending_us = now;
        if (!data_clients || now - S->pending_us >= SETTLE_US) {
            S->pending_us = 0;
            apply_model(S);
            if (data_clients) {
                ESP_LOGI(TAG, "entities changed (%d on %d sub-devices): Home Assistant reconnects to reload them",
                         S->model->n_ent, S->model->n_dev);
                close_all(S, "entities changed");
            }
        }
    } else {
        S->pending_us = 0;
    }

    bool slow = S->slow_now || now >= S->next_slow_us;
    if (slow) {
        S->slow_now = false;
        S->next_slow_us = now + SLOW_US;
    }
    for (int i = 0; i < S->model->n_ent; i++) {
        const ha_ent_t *e = &S->model->ent[i];
        if (ha_model_is_slow(e) && !slow) continue;
        ha_state_t st;
        ha_model_state(e, &S->in, &S->cur[i], &st);
        S->cur[i] = st;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &S->cl[i];
        if (c->state == CS_DATA && c->subscribed) push_states(S, c);
    }
}

static void deliver_events(srv_t *S)
{
    ev_item_t ev;
    while (xQueueReceive(s_evq, &ev, 0) == pdTRUE) {
        const ha_ent_t *e = NULL;
        for (int i = 0; i < S->model->n_ent && !e; i++) {
            const ha_ent_t *x = &S->model->ent[i];
            if (x->type == ET_EVENT && strcmp(x->ref, ev.script) == 0 && strcmp(x->ref2, ev.name) == 0) e = x;
        }
        bool known = false;
        for (int t = 0; e && t < e->n_types; t++) known |= strcmp(e->types[t], ev.type) == 0;
        if (!known) continue;  // not listed to Home Assistant yet: it will be after the reload
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &S->cl[i];
            if (c->state != CS_DATA || !c->subscribed) continue;
            pb_w_t w = msg_begin(S);
            pb_fixed32(&w, 1, e->key);
            pb_str(&w, 2, ev.type);
            pb_varint(&w, 3, e->device_id);
            if (out_msg(S, c, MSG_EVENT, &w)) out_flush(S, c);
        }
    }
}

static void on_script_event(const char *script, const char *name, const char *type)
{
    if (!s_running || !s_evq) return;
    ev_item_t ev;
    strlcpy(ev.script, script, sizeof(ev.script));
    strlcpy(ev.name, name, sizeof(ev.name));
    strlcpy(ev.type, type, sizeof(ev.type));
    xQueueSend(s_evq, &ev, 0);  // full: dropped (never block the automation task)
}

static void keepalive(srv_t *S, int64_t now)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &S->cl[i];
        if (c->state == CS_FREE) continue;
        if (c->state != CS_DATA) {
            if (now - c->opened_us > HANDSHAKE_TMO_US) close_client(c, "handshake timeout");
            continue;
        }
        if (now - c->last_rx_us > DEAD_US) {
            close_client(c, "no answer to keepalive pings");
        } else if (now - c->last_rx_us > KEEPALIVE_US && !c->ping_us) {
            c->ping_us = now;
            if (out_empty(S, c, MSG_PING_REQ)) out_flush(S, c);
        }
    }
}

// ---- the task -----------------------------------------------------------------------------------

static int open_listener(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(HA_API_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 2) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void server_task(void *arg)
{
    srv_t *S = s_srv;
    for (int i = 0; i < MAX_CLIENTS; i++) S->cl[i].fd = -1;
    S->model = &S->models[0];
    S->next = &S->models[1];
    refresh_inputs(S);
    ha_model_build(S->next, &S->in);
    apply_model(S);
    S->zrev = zb_revision();
    S->arev = automation_ha_revision();
    S->listen_fd = -1;
    int64_t retry_us = 0;
    bool goodbye = false;
    ESP_LOGI(TAG, "Home Assistant API on, %d entities on %d sub-devices", S->model->n_ent, S->model->n_dev);

    while (!s_stop) {
        int64_t now = esp_timer_get_time();
        // Planned reboot: tell the clients (as ESPHome does on shutdown) and take no new ones,
        // or Home Assistant would show stale states until its keepalive times out
        if (!goodbye && hubsys_reboot_pending()) {
            goodbye = true;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                client_t *c = &S->cl[i];
                if (c->state == CS_DATA && out_empty(S, c, MSG_DISC_REQ)) out_flush(S, c);
                close_client(c, "hub restarting");
            }
            if (S->listen_fd >= 0) close(S->listen_fd);
            S->listen_fd = -1;
        }
        if (S->listen_fd < 0 && now >= retry_us && !goodbye) {
            S->listen_fd = open_listener();
            if (S->listen_fd < 0) {
                ESP_LOGW(TAG, "can't listen on port %d (errno %d), retrying in 10 s", HA_API_PORT, errno);
                retry_us = now + 10 * 1000000LL;
            } else {
                ESP_LOGI(TAG, "listening on port %d", HA_API_PORT);
                mdns_add();
            }
        }
        fd_set rs;
        FD_ZERO(&rs);
        int maxfd = -1;
        if (S->listen_fd >= 0) {
            FD_SET(S->listen_fd, &rs);
            maxfd = S->listen_fd;
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (S->cl[i].state == CS_FREE) continue;
            FD_SET(S->cl[i].fd, &rs);
            if (S->cl[i].fd > maxfd) maxfd = S->cl[i].fd;
        }
        struct timeval tv = {.tv_usec = 100 * 1000};
        int r = maxfd >= 0 ? select(maxfd + 1, &rs, NULL, NULL, &tv) : (vTaskDelay(pdMS_TO_TICKS(100)), 0);
        now = esp_timer_get_time();
        if (r > 0) {
            if (S->listen_fd >= 0 && FD_ISSET(S->listen_fd, &rs)) accept_client(S, now);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                client_t *c = &S->cl[i];
                if (c->state != CS_FREE && FD_ISSET(c->fd, &rs)) read_client(S, c, now);
            }
        }
        if (s_kick) {
            s_kick = false;
            close_all(S, "new encryption key");
        }
        update(S, now);
        deliver_events(S);
        keepalive(S, now);
    }

    close_all(S, "Home Assistant integration turned off");
    if (S->listen_fd >= 0) close(S->listen_fd);
    mdns_del();
    ESP_LOGI(TAG, "Home Assistant API off (stack free min %u B)", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_entities = s_devices = s_clients = 0;
    s_running = false;
    vTaskDelete(NULL);
}

static esp_err_t start_server(void)
{
    if (s_running) return ESP_OK;
    if (!s_evq) s_evq = xQueueCreateWithCaps(8, sizeof(ev_item_t), MALLOC_CAP_SPIRAM);
    s_srv = heap_caps_calloc(1, sizeof(srv_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_srv || !s_evq) {
        free(s_srv);
        s_srv = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_stop = false;
    s_running = true;
    if (xTaskCreate(server_task, "ha_api", TASK_STACK, NULL, 2, &s_task) != pdPASS) {
        s_running = false;
        free(s_srv);
        s_srv = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void stop_server(void)
{
    if (!s_running) return;
    s_stop = true;
    for (int i = 0; i < 50 && s_running; i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (!s_running) {
        free(s_srv);
        s_srv = NULL;
    }
}

// ---- public -------------------------------------------------------------------------------------

esp_err_t ha_init(const char *hostname, const char *prov_nonce, const char *prov_key)
{
    s_api_mtx = xSemaphoreCreateMutex();
    strlcpy(s_host, hostname, sizeof(s_host));
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_mac_hex, sizeof(s_mac_hex), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_mac_pretty, sizeof(s_mac_pretty), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    automation_ha_set_event_cb(on_script_event);

    nvs_handle_t h;
    char seen[33] = "";
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t on = 0;
        size_t kl = 32, sl = sizeof(seen);
        nvs_get_u8(h, "on", &on);
        s_enabled = on;
        s_have_key = nvs_get_blob(h, "key", s_key, &kl) == ESP_OK && kl == 32;
        if (nvs_get_str(h, "nonce", seen, &sl) != ESP_OK) seen[0] = '\0';
        nvs_close(h);
    }
    // flash.sh --ha: a nonce not seen before turns the integration on with its key
    if (prov_nonce && prov_nonce[0] && strcmp(prov_nonce, seen) != 0) {
        uint8_t k[48];
        size_t kl = 0;
        if (prov_key && mbedtls_base64_decode(k, sizeof(k), &kl, (const uint8_t *)prov_key, strlen(prov_key)) == 0 &&
            kl == 32) {
            memcpy(s_key, k, 32);
            s_have_key = true;
        } else {
            ESP_LOGW(TAG, "prov: bad key from flash.sh --ha, generating one");
            new_key();
        }
        memset(k, 0, sizeof(k));
        s_enabled = true;
        esp_err_t err = save_settings();
        if (err == ESP_OK && nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "nonce", prov_nonce);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGI(TAG, "turned on by flash.sh --ha");
    }
    if (!s_enabled) {
        ESP_LOGI(TAG, "Home Assistant integration off");
        return ESP_OK;
    }
    if (!s_have_key) {
        new_key();
        save_settings();
    }
    return start_server();
}

void ha_get_status(ha_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = s_enabled;
    out->running = s_running;
    out->clients = s_clients;
    out->entities = s_entities;
    out->devices = s_devices;
    out->connects = s_connects;
    out->rejected = s_rejected;
    strlcpy(out->client, s_last_client, sizeof(out->client));
    if (s_have_key) {
        uint8_t k[32];
        size_t n = 0;
        key_copy(k);
        mbedtls_base64_encode((uint8_t *)out->key, sizeof(out->key), &n, k, 32);
        memset(k, 0, sizeof(k));
    }
}

esp_err_t ha_set_enabled(bool on)
{
    xSemaphoreTake(s_api_mtx, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (on && !s_have_key) new_key();
    if (s_enabled != on) {
        s_enabled = on;
        err = save_settings();
    }
    if (err == ESP_OK) {
        if (on) err = start_server();
        else stop_server();
    }
    xSemaphoreGive(s_api_mtx);
    ESP_LOGI(TAG, "Home Assistant integration %s from the web UI", on ? "on" : "off");
    return err;
}

esp_err_t ha_new_key(void)
{
    xSemaphoreTake(s_api_mtx, portMAX_DELAY);
    new_key();
    esp_err_t err = save_settings();
    s_kick = true;
    xSemaphoreGive(s_api_mtx);
    ESP_LOGI(TAG, "new encryption key");
    return err;
}

void ha_set_conn_filter(bool (*allowed)(int fd))
{
    s_filter = allowed;
}
