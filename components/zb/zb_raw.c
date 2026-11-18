// Arbitrary Zigbee requests: read or write any attribute of any cluster, or send a command on a
// manufacturer cluster, and hand the device's answer back. For the API (/api/zb/raw) and Lua
// (zb.read / zb.write / zb.command): everything the hub has no dedicated call or UI for.
//
// A request is sent from the Zigbee task. Its answer comes back through the ZCL callbacks
// (zb_dev_on_zcl calls the zb_raw_on_* hooks) and is matched by source address, cluster and
// kind (read response, write response, default response for a command). No answer within
// RAW_TIMEOUT_US: the result says ESP_ERR_TIMEOUT (a sleepy device only answers while awake).
// The callback runs on the Zigbee task, outside the lock here: it must only copy.

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "zb.h"
#include "zb_internal.h"

static const char *TAG = "zb.raw";

#define RAW_SLOTS      8
#define RAW_TIMEOUT_US (4LL * 1000000)

typedef struct {
    bool used;
    zb_raw_op_t op;
    uint16_t short_addr, cluster;
    uint16_t attrs[ZB_RAW_MAX_ATTRS];  // read: what was asked
    uint8_t n_attrs, cmd;
    int64_t deadline_us;
    zb_raw_cb_t cb;
    void *ctx;
} pending_t;

static pending_t s_pend[RAW_SLOTS];
static SemaphoreHandle_t s_rmtx;

#define RLOCK()   xSemaphoreTake(s_rmtx, portMAX_DELAY)
#define RUNLOCK() xSemaphoreGive(s_rmtx)

esp_err_t zb_raw_init(void)
{
    s_rmtx = xSemaphoreCreateMutex();
    return s_rmtx ? ESP_OK : ESP_ERR_NO_MEM;
}

// ---- values --------------------------------------------------------------------------------

// Size of a fixed-length integer-like type, 0 for anything else
static uint8_t int_size(uint8_t type)
{
    if (type == EZB_ZCL_ATTR_TYPE_BOOL || type == EZB_ZCL_ATTR_TYPE_ENUM8) return 1;
    if (type == EZB_ZCL_ATTR_TYPE_ENUM16) return 2;
    if (type >= 0x08 && type <= 0x0F) return type - 0x07;  // data8..data64
    if (type >= 0x18 && type <= 0x1F) return type - 0x17;  // bitmap8..bitmap64
    if (type >= 0x20 && type <= 0x27) return type - 0x1F;  // uint8..uint64
    if (type >= 0x28 && type <= 0x2F) return type - 0x27;  // int8..int64
    return 0;
}

static bool is_signed(uint8_t type)
{
    return type >= 0x28 && type <= 0x2F;
}

bool zb_raw_value_number(uint8_t type, const uint8_t *data, uint8_t len, double *out)
{
    uint8_t n = int_size(type);
    if (!n || n > 6 || len < n) return false;  // up to 48 bits fit a double exactly
    uint64_t u = 0;
    for (int i = 0; i < n; i++) u |= (uint64_t)data[i] << (8 * i);
    if (is_signed(type) && (u >> (8 * n - 1)) & 1) {
        *out = (double)(int64_t)(u | (~0ULL << (8 * n)));
    } else {
        *out = (double)u;
    }
    return true;
}

static void hex_of(const uint8_t *data, uint8_t len, char *out, size_t cap)
{
    size_t o = 0;
    for (uint8_t i = 0; i < len && o + 2 < cap; i++) o += snprintf(out + o, cap - o, "%02x", data[i]);
    out[o] = '\0';
}

// Character strings as text (printable ASCII only), integers as numbers, the rest as hex
void zb_raw_value_text(uint8_t type, const uint8_t *data, uint8_t len, char *out, size_t cap, bool *is_number,
                       double *num)
{
    *is_number = zb_raw_value_number(type, data, len, num);
    if (*is_number) return;
    if (type == EZB_ZCL_ATTR_TYPE_STRING && len >= 1) {
        uint8_t n = data[0] == 0xFF ? 0 : data[0];
        if (n > len - 1) n = len - 1;
        size_t o = 0;
        for (uint8_t i = 0; i < n && o + 1 < cap; i++) {
            char c = (char)data[1 + i];
            out[o++] = (c >= 0x20 && c < 0x7F) ? c : '?';
        }
        out[o] = '\0';
        return;
    }
    if (type == EZB_ZCL_ATTR_TYPE_OCTSTR && len >= 1) {
        uint8_t n = data[0] == 0xFF ? 0 : data[0];
        if (n > len - 1) n = len - 1;
        hex_of(data + 1, n, out, cap);
        return;
    }
    hex_of(data, len, out, cap);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int zb_raw_hex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (int)(n / 2);
}

esp_err_t zb_raw_encode(uint8_t type, double num, const char *str, uint8_t *out, uint8_t *len)
{
    uint8_t n = int_size(type);
    if (n) {
        if (str) return ESP_ERR_INVALID_ARG;
        int64_t v = (int64_t)num;
        if ((double)v != num) return ESP_ERR_INVALID_ARG;  // not an integer
        if (n < 8) {
            int64_t lo = is_signed(type) ? -(1LL << (8 * n - 1)) : 0;
            int64_t hi = is_signed(type) ? (1LL << (8 * n - 1)) - 1 : (int64_t)((1ULL << (8 * n)) - 1);
            if (v < lo || v > hi) return ESP_ERR_INVALID_SIZE;
        }
        for (int i = 0; i < n; i++) out[i] = (uint8_t)((uint64_t)v >> (8 * i));
        *len = n;
        return ESP_OK;
    }
    if (type == EZB_ZCL_ATTR_TYPE_STRING && str) {
        size_t s = strlen(str);
        if (s + 1 > ZB_RAW_VALUE_MAX) return ESP_ERR_INVALID_SIZE;
        out[0] = (uint8_t)s;
        memcpy(out + 1, str, s);
        *len = (uint8_t)(s + 1);
        return ESP_OK;
    }
    if (type == EZB_ZCL_ATTR_TYPE_OCTSTR && str) {
        int b = zb_raw_hex(str, out + 1, ZB_RAW_VALUE_MAX - 1);
        if (b < 0) return ESP_ERR_INVALID_ARG;
        out[0] = (uint8_t)b;
        *len = (uint8_t)(b + 1);
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

// ---- sending (Zigbee task) ------------------------------------------------------------------

static void finish(zb_raw_cb_t cb, void *ctx, zb_raw_result_t *res)
{
    if (cb) cb(res, ctx);
}

static void send_on_zb_task(void *arg)
{
    zb_raw_req_t *q = arg;
    zb_raw_cb_t cb = q->cb;
    void *ctx = q->ctx;
    static zb_raw_result_t res;
    memset(&res, 0, sizeof(res));
    uint16_t short_addr;
    uint8_t ep = q->ep;
    if (!zb_dev_resolve(q->device, q->cluster, q->to_client, &short_addr, ep ? NULL : &ep)) {
        free(q);
        res.err = ESP_ERR_NOT_FOUND;
        finish(cb, ctx, &res);
        return;
    }
    RLOCK();
    pending_t *p = NULL;
    for (int i = 0; i < RAW_SLOTS && !p; i++) if (!s_pend[i].used) p = &s_pend[i];
    if (p) {
        *p = (pending_t){.used = true, .op = q->op, .short_addr = short_addr, .cluster = q->cluster,
                         .n_attrs = q->n_attrs, .cmd = q->cmd, .deadline_us = esp_timer_get_time() + RAW_TIMEOUT_US,
                         .cb = cb, .ctx = ctx};
        memcpy(p->attrs, q->attrs, sizeof(p->attrs));
    }
    RUNLOCK();
    if (!p) {
        free(q);
        res.err = ESP_ERR_NO_MEM;  // too many requests in flight
        finish(cb, ctx, &res);
        return;
    }
    ezb_zcl_cmd_ctrl_t c = {0};
    c.dst_addr.addr_mode = EZB_ADDR_MODE_SHORT;
    c.dst_addr.u.short_addr = short_addr;
    c.dst_ep = ep;
    c.src_ep = ZB_HUB_EP;
    c.cluster_id = q->cluster;
    c.fc.direction = q->to_client ? 1 : 0;
    if (q->manuf) {
        c.fc.manuf_specific = 1;
        c.manuf_code = q->manuf;
    }
    ezb_err_t e = EZB_ERR_NONE;
    if (q->op == ZB_RAW_READ) {
        static uint16_t attrs[ZB_RAW_MAX_ATTRS];
        memcpy(attrs, q->attrs, sizeof(attrs));
        ezb_zcl_read_attr_cmd_t r = {.cmd_ctrl = c, .payload = {.attr_number = q->n_attrs, .attr_field = attrs}};
        e = ezb_zcl_read_attr_cmd_req(&r);
    } else if (q->op == ZB_RAW_WRITE) {
        static uint8_t value[ZB_RAW_VALUE_MAX];
        memcpy(value, q->value, q->len);
        ezb_zcl_attribute_t a = {.id = q->attr, .data = {.type = q->type, .size = q->len, .value = value}};
        ezb_zcl_write_attr_cmd_t w = {.cmd_ctrl = c, .payload = {.attr_number = 1, .attr_field = &a}};
        e = ezb_zcl_write_attr_cmd_req(&w);
    } else {
        static uint8_t payload[ZB_RAW_PAYLOAD_MAX];
        memcpy(payload, q->payload, q->payload_len);
        ezb_zcl_custom_cmd_t m = {.cmd_ctrl = c, .cmd_id = q->cmd, .data_length = q->payload_len, .data = payload};
        e = ezb_zcl_custom_cmd_req(&m);
    }
    ESP_LOGI(TAG, "0x%04x ep %u cluster 0x%04x: %s sent (0x%x)", short_addr, ep, q->cluster,
             q->op == ZB_RAW_READ ? "read" : q->op == ZB_RAW_WRITE ? "write" : "command", e);
    free(q);
    if (e != EZB_ERR_NONE) {
        RLOCK();
        p->used = false;
        RUNLOCK();
        res.err = ESP_FAIL;
        finish(cb, ctx, &res);
    }
}

esp_err_t zb_raw_request(const zb_raw_req_t *req, zb_raw_cb_t cb, void *ctx)
{
    if (!req || !zb_is_up()) return ESP_ERR_INVALID_STATE;
    if (req->op == ZB_RAW_READ && (req->n_attrs == 0 || req->n_attrs > ZB_RAW_MAX_ATTRS)) return ESP_ERR_INVALID_ARG;
    if (req->op == ZB_RAW_WRITE && (req->len == 0 || req->len > ZB_RAW_VALUE_MAX)) return ESP_ERR_INVALID_ARG;
    if (req->op == ZB_RAW_COMMAND && (req->cluster < EZB_ZCL_CLUSTER_ID_MIN_CUSTOM || req->payload_len > ZB_RAW_PAYLOAD_MAX)) {
        return ESP_ERR_NOT_SUPPORTED;  // standard clusters have their own commands
    }
    zb_raw_req_t *q = heap_caps_malloc(sizeof(*q), MALLOC_CAP_SPIRAM);
    if (!q) return ESP_ERR_NO_MEM;
    *q = *req;
    q->cb = cb;
    q->ctx = ctx;
    esp_err_t err = esp_zigbee_task_queue_post(send_on_zb_task, q);
    if (err != ESP_OK) free(q);
    return err;
}

// ---- answers (Zigbee task) ------------------------------------------------------------------

static pending_t *take(uint16_t short_addr, uint16_t cluster, zb_raw_op_t op, pending_t *copy)
{
    for (int i = 0; i < RAW_SLOTS; i++) {
        pending_t *p = &s_pend[i];
        if (p->used && p->short_addr == short_addr && p->cluster == cluster && p->op == op) {
            *copy = *p;
            p->used = false;
            return copy;
        }
    }
    return NULL;
}

static uint16_t src_of(const ezb_zcl_cmd_hdr_t *h)
{
    return h && h->src_addr.addr_mode == EZB_ADDR_MODE_SHORT ? h->src_addr.u.short_addr : 0xFFFF;
}

void zb_raw_on_read_rsp(const ezb_zcl_cmd_read_attr_rsp_message_t *m)
{
    uint16_t from = src_of(m->in.header);
    pending_t p;
    RLOCK();
    pending_t *got = NULL;
    // only a read that asked for one of these attributes: the hub's own reads of the cluster stay its own
    for (int i = 0; i < RAW_SLOTS && !got; i++) {
        pending_t *s = &s_pend[i];
        if (!s->used || s->op != ZB_RAW_READ || s->short_addr != from || s->cluster != m->info.cluster_id) continue;
        for (const ezb_zcl_read_attr_rsp_variable_t *v = m->in.variables; v && !got; v = v->next) {
            for (int k = 0; k < s->n_attrs; k++) {
                if (s->attrs[k] == v->attr_id) {
                    p = *s;
                    s->used = false;
                    got = &p;
                    break;
                }
            }
        }
    }
    RUNLOCK();
    if (!got) return;
    static zb_raw_result_t res;
    memset(&res, 0, sizeof(res));
    for (const ezb_zcl_read_attr_rsp_variable_t *v = m->in.variables; v && res.n < ZB_RAW_MAX_ATTRS; v = v->next) {
        typeof(res.v[0]) *o = &res.v[res.n++];
        o->attr = v->attr_id;
        o->status = v->status;
        if (v->status == EZB_ZCL_STATUS_SUCCESS && v->attr_value) {
            o->type = v->attr_type;
            uint16_t sz = ezb_zcl_get_attr_value_size(v->attr_type, v->attr_value);
            o->len = sz > ZB_RAW_VALUE_MAX ? ZB_RAW_VALUE_MAX : (uint8_t)sz;
            memcpy(o->data, v->attr_value, o->len);
        }
    }
    finish(p.cb, p.ctx, &res);
}

void zb_raw_on_write_rsp(const ezb_zcl_cmd_write_attr_rsp_message_t *m)
{
    pending_t p;
    RLOCK();
    pending_t *got = take(src_of(m->in.header), m->info.cluster_id, ZB_RAW_WRITE, &p);
    RUNLOCK();
    if (!got) return;
    static zb_raw_result_t res;
    memset(&res, 0, sizeof(res));
    res.zcl_status = m->in.variables ? m->in.variables->status : EZB_ZCL_STATUS_SUCCESS;
    finish(p.cb, p.ctx, &res);
}

void zb_raw_on_default_rsp(const ezb_zcl_cmd_default_rsp_message_t *m)
{
    uint16_t from = src_of(m->in.header);
    pending_t p;
    RLOCK();
    pending_t *got = NULL;
    for (int i = 0; i < RAW_SLOTS && !got; i++) {
        pending_t *s = &s_pend[i];
        if (!s->used || s->short_addr != from || s->cluster != m->info.cluster_id) continue;
        // a command's answer, or a refused read / write (unsupported cluster, manufacturer code)
        bool mine = s->op == ZB_RAW_COMMAND ? m->in.rsp_to_cmd == s->cmd
                  : m->in.status_code != EZB_ZCL_STATUS_SUCCESS &&
                        m->in.rsp_to_cmd == (s->op == ZB_RAW_READ ? 0x00 : 0x02);
        if (mine) {
            p = *s;
            s->used = false;
            got = &p;
        }
    }
    RUNLOCK();
    if (!got) return;
    static zb_raw_result_t res;
    memset(&res, 0, sizeof(res));
    res.zcl_status = m->in.status_code;
    res.err = p.op == ZB_RAW_COMMAND ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    finish(p.cb, p.ctx, &res);
}

void zb_raw_tick(void)
{
    int64_t now = esp_timer_get_time();
    pending_t gone[RAW_SLOTS];
    int n = 0;
    RLOCK();
    for (int i = 0; i < RAW_SLOTS; i++) {
        if (s_pend[i].used && now >= s_pend[i].deadline_us) {
            gone[n++] = s_pend[i];
            s_pend[i].used = false;
        }
    }
    RUNLOCK();
    for (int i = 0; i < n; i++) {
        static zb_raw_result_t res;
        memset(&res, 0, sizeof(res));
        res.err = ESP_ERR_TIMEOUT;
        ESP_LOGI(TAG, "0x%04x cluster 0x%04x: no answer", gone[i].short_addr, gone[i].cluster);
        finish(gone[i].cb, gone[i].ctx, &res);
    }
}
