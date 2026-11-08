/**
 * @file ml_derp.c
 * @brief Unified DERP I/O Task + Connection Management
 *
 * Single task handles BOTH reading and writing to DERP TLS connection.
 * This eliminates the need for a TLS mutex since only one task touches
 * the SSL context. Matches v1's single-threaded DERP model.
 *
 * Architecture:
 * - Poll for incoming DERP frames (TLS read) every iteration
 * - Drain TX queue between reads (TLS write)
 * - No mutex needed — single task owns the SSL context exclusively
 *
 * Backpressure strategy (from tailscaled):
 * When queue is full, dequeue oldest packet and retry up to 3 times.
 * If still full, drop the new packet.
 *
 * Reference: tailscale/wgengine/magicsock/derp.go (runDerpWriter)
 *            tailscale/derp/derphttp/derphttp_client.go
 */

#include "microlink_internal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/error.h"
#include "esp_crt_bundle.h"
#include "nacl_box.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>

static const char *TAG = "ml_derp";

/* Timeout for DERP connection handshake operations */
#define DERP_CONNECT_TIMEOUT_MS  10000

/* ============================================================================
 * Custom BIO callbacks for non-blocking TLS I/O
 *
 * These wrap lwIP recv/send with guaranteed timeout behavior.
 * We don't trust mbedtls_net_recv_timeout on lwIP because lwIP's select()
 * can sometimes block indefinitely on ESP32.
 * ========================================================================== */

/**
 * Custom recv with timeout for mbedtls BIO.
 * Uses SO_RCVTIMEO on the socket as the timeout mechanism (simpler than select).
 * Returns bytes read, or MBEDTLS_ERR_SSL_TIMEOUT, MBEDTLS_ERR_SSL_WANT_READ.
 */
static int ml_derp_bio_recv_timeout(void *ctx, unsigned char *buf, size_t len,
                                      uint32_t timeout) {
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    /* Set SO_RCVTIMEO to the requested timeout.
     * If timeout is 0 (mbedTLS default = "no timeout"), use 10s as a sane
     * default to avoid indefinite blocking on AT sockets. */
    uint32_t effective_timeout = (timeout > 0) ? timeout : DERP_CONNECT_TIMEOUT_MS;
    struct timeval tv;
    tv.tv_sec = effective_timeout / 1000;
    tv.tv_usec = (effective_timeout % 1000) * 1000;
    ml_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int ret = (int)ml_read_sock(fd, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_TIMEOUT;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        if (errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return ret;
}

/**
 * Custom send for mbedtls BIO.
 */
static int ml_derp_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    int ret = (int)ml_write_sock(fd, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        if (errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return ret;
}

/* DERP frame types */
#define DERP_FRAME_SERVER_KEY   0x01
#define DERP_FRAME_CLIENT_INFO  0x02
#define DERP_FRAME_SERVER_INFO  0x03
#define DERP_FRAME_SEND_PACKET  0x04
#define DERP_FRAME_RECV_PACKET  0x05
#define DERP_FRAME_KEEP_ALIVE   0x06
#define DERP_FRAME_NOTE_PREFERRED 0x07
#define DERP_FRAME_PEER_GONE    0x08
#define DERP_FRAME_PING         0x12
#define DERP_FRAME_PONG         0x13

/* DISCO magic bytes: "TS" + sparkles emoji UTF-8 */
static const uint8_t DISCO_MAGIC[6] = { 'T', 'S', 0xf0, 0x9f, 0x92, 0xac };

/* ============================================================================
 * TLS Read/Write Helpers
 * ========================================================================== */
/* Hub port: per-region connections (defined with connection management below) */
static ml_derp_conn_t *derp_conn_for(microlink_t *ml, uint16_t region, uint64_t now_ms);
static void derp_close_conn(ml_derp_conn_t *c);

/* Hub port: latency trace. Time inside the hub from reading a DISCO ping off a DERP
 * connection to finishing the TLS write of its pong. One INFO line per 12 samples
 * (per-packet logging would itself add ~7 ms per line). */
static void derp_trace_record(uint32_t rx_up_ms, uint32_t tx_up_ms) {
    /* Logged as wall-clock ms within the minute (SNTP), to line up with timestamps
     * taken on the peer. rx = DERP task read the ping, then ms until the pong's TLS
     * write was done. One line per 12 pings (per-packet logs would add ~7 ms each). */
    static uint32_t rx[12], tx[12];
    static int n;
    rx[n] = rx_up_ms;
    tx[n] = tx_up_ms;
    if (++n < 12) return;
    n = 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t wall_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    int64_t off = wall_ms - (int64_t)ml_get_time_ms();
    char line[12 * 16 + 1];
    int pos = 0;
    for (int i = 0; i < 12 && pos < (int)sizeof(line) - 16; i++) {
        pos += snprintf(line + pos, sizeof(line) - pos, " %lu/%lu",
                        (unsigned long)(((int64_t)rx[i] + off) % 60000), (unsigned long)(tx[i] - rx[i]));
    }
    ESP_LOGD(TAG, "DERP ping trace (wall ms in minute / ms inside hub):%s", line);
}


/**
 * Read exactly `len` bytes via TLS with timeout and WANT_READ retry.
 * Returns number of bytes read on success, -1 on error, -2 on timeout.
 */
static int derp_tls_read_all(ml_derp_conn_t *c, uint8_t *data, size_t len, int timeout_ms) {
    size_t received = 0;
    uint64_t start_ms = ml_get_time_ms();

    while (received < len) {
        if (timeout_ms > 0 && (ml_get_time_ms() - start_ms) > (uint64_t)timeout_ms) {
            ESP_LOGW(TAG, "derp_tls_read_all timeout (%d/%d bytes in %dms)",
                     (int)received, (int)len, timeout_ms);
            return -2;
        }

        int ret = mbedtls_ssl_read(&c->ssl, data + received, len - received);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                ESP_LOGW(TAG, "DERP server closed connection");
                return -1;
            }
            ESP_LOGE(TAG, "TLS read failed: -0x%04x", -ret);
            return -1;
        }
        if (ret == 0) {
            ESP_LOGW(TAG, "TLS connection closed by peer (%d/%d bytes)",
                     (int)received, (int)len);
            return -1;
        }
        received += ret;
    }
    return (int)received;
}

/**
 * Read a DERP frame header (5 bytes: type + 4-byte BE length) with timeout.
 */
static esp_err_t derp_recv_frame_header(ml_derp_conn_t *c, uint8_t *type,
                                          uint32_t *len, int timeout_ms) {
    uint8_t header[5];
    int ret = derp_tls_read_all(c, header, 5, timeout_ms);
    if (ret < 0) {
        return (ret == -2) ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    *type = header[0];
    *len = ((uint32_t)header[1] << 24) |
           ((uint32_t)header[2] << 16) |
           ((uint32_t)header[3] << 8) |
           (uint32_t)header[4];

    return ESP_OK;
}

/**
 * Write exactly `len` bytes via TLS with WANT_WRITE retry.
 * Returns bytes written on success, -1 on error.
 * No mutex needed — called only from the DERP I/O task.
 */
static int derp_tls_write_all(ml_derp_conn_t *c, const uint8_t *data, size_t len) {
    size_t written = 0;
    int retries = 0;
    const int max_retries = 50;  /* 50 * 10ms = 500ms max */

    while (written < len) {
        int ret = mbedtls_ssl_write(&c->ssl, data + written, len - written);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(10));
                if (++retries > max_retries) {
                    ESP_LOGW(TAG, "TLS write timeout after %d retries", retries);
                    return -1;
                }
                continue;
            }
            ESP_LOGE(TAG, "TLS write failed: -0x%04x", -ret);
            return -1;
        }
        written += ret;
        retries = 0;
    }
    return (int)written;
}

/* Write a complete DERP frame via TLS.
 * Header and payload go out in ONE write = one TLS record = one TCP segment (two
 * records would make Nagle hold the second for the server's delayed ACK of the
 * first). `prefix` (optional) goes between header and
 * payload, so SendPacket needs no extra copy for the destination key. */
static int derp_write_frame2(ml_derp_conn_t *c, uint8_t type, const uint8_t *prefix,
                             uint32_t prefix_len, const uint8_t *payload, uint32_t len) {
    uint32_t body = prefix_len + (payload ? len : 0);
    uint8_t stackbuf[256];
    uint8_t *buf = (5 + body <= sizeof(stackbuf)) ? stackbuf : malloc(5 + body);
    if (!buf) return -1;
    buf[0] = type;
    buf[1] = (body >> 24) & 0xFF;
    buf[2] = (body >> 16) & 0xFF;
    buf[3] = (body >> 8) & 0xFF;
    buf[4] = body & 0xFF;
    if (prefix_len) memcpy(buf + 5, prefix, prefix_len);
    if (payload && len) memcpy(buf + 5 + prefix_len, payload, len);
    int ret = derp_tls_write_all(c, buf, 5 + body) < 0 ? -1 : 0;
    if (buf != stackbuf) free(buf);
    return ret;
}

static int derp_write_frame(ml_derp_conn_t *c, uint8_t type,
                             const uint8_t *payload, uint32_t len) {
    return derp_write_frame2(c, type, NULL, 0, payload, len);
}

/* Send a packet to a peer via DERP (SendPacket frame: 32-byte dest key + payload) */
static int derp_send_packet(ml_derp_conn_t *c, const uint8_t *dest_key,
                              const uint8_t *data, size_t len) {
    int ret = derp_write_frame2(c, DERP_FRAME_SEND_PACKET, dest_key, 32, data, len);
    if (ret < 0) {
        ESP_LOGW(TAG, "derp_send_packet FAILED: dest=%02x%02x%02x%02x len=%d",
                 dest_key[0], dest_key[1], dest_key[2], dest_key[3], (int)len);
    }
    return ret;
}

/* ============================================================================
 * DERP Frame Reading and Dispatch (runs on DERP I/O task)
 * ========================================================================== */

/* Packet classification */
typedef enum {
    PKT_DISCO,
    PKT_STUN,
    PKT_WIREGUARD,
    PKT_UNKNOWN,
} pkt_type_t;

static pkt_type_t classify_packet(const uint8_t *data, size_t len) {
    if (len >= 20 && (data[0] == 0x00 || data[0] == 0x01) && data[1] == 0x01) {
        return PKT_STUN;
    }
    if (len >= 62 && memcmp(data, DISCO_MAGIC, 6) == 0) {
        return PKT_DISCO;
    }
    if (len >= 4) {
        return PKT_WIREGUARD;
    }
    return PKT_UNKNOWN;
}

static void route_derp_packet(microlink_t *ml, uint8_t *data, size_t len,
                               const uint8_t *src_pubkey) {
    pkt_type_t type = classify_packet(data, len);

    ml_rx_packet_t pkt = {
        .data = data,
        .len = len,
        .via_derp = true,
        .t_rx_ms = (uint32_t)ml_get_time_ms(),
    };
    memcpy(pkt.src_pubkey, src_pubkey, 32);

    QueueHandle_t target = (type == PKT_DISCO) ? ml->disco_rx_queue : ml->wg_rx_queue;
    if (xQueueSend(target, &pkt, 0) != pdTRUE) {
        free(data);
    } else if (ml->wg_mgr_task) {
        xTaskNotifyGive(ml->wg_mgr_task);  /* hub port: wake it now */
    }
}

/* Dispatch a received DERP frame */
static void dispatch_derp_frame(microlink_t *ml, ml_derp_conn_t *c, uint8_t frame_type,
                                 uint8_t *src_key, uint8_t *payload, size_t payload_len) {
    switch (frame_type) {
    case DERP_FRAME_RECV_PACKET:
        if (payload) {
            ESP_LOGD(TAG, "DERP RecvPacket: %d bytes from %02x%02x%02x%02x, hdr=%02x",
                     (int)payload_len,
                     src_key[0], src_key[1], src_key[2], src_key[3],
                     payload_len > 0 ? payload[0] : 0xFF);
            route_derp_packet(ml, payload, payload_len, src_key);
            return;  /* payload ownership transferred */
        }
        break;

    case DERP_FRAME_KEEP_ALIVE:
        ESP_LOGD(TAG, "DERP KeepAlive received");
        break;

    case DERP_FRAME_PING:
        /* Echo ping data back as PONG directly (single-threaded, safe to write) */
        if (payload && payload_len > 0) {
            ESP_LOGD(TAG, "DERP PING received, sending PONG");
            derp_write_frame(c, DERP_FRAME_PONG, payload, payload_len);
        }
        break;

    case DERP_FRAME_PONG:
        ESP_LOGD(TAG, "DERP PONG received");
        break;

    case DERP_FRAME_PEER_GONE:
        if (payload && payload_len >= 32) {
            ESP_LOGD(TAG, "DERP PeerGone: %02x%02x%02x%02x (len=%d)",
                     payload[0], payload[1], payload[2], payload[3],
                     (int)payload_len);
        }
        break;

    default:
        ESP_LOGD(TAG, "DERP frame type 0x%02x ignored (%d bytes)",
                 frame_type, (int)payload_len);
        break;
    }

    if (payload) free(payload);
}

/**
 * Try to read one DERP frame.
 * Uses mbedtls recv_timeout (100ms) so ssl_read never blocks indefinitely.
 * Returns: 1 = frame read and dispatched, 0 = timeout (no data), <0 = error
 */
static int poll_derp_read(microlink_t *ml, ml_derp_conn_t *c) {
    if (!c->connected || c->sockfd < 0) return -1;

    /* Read 5-byte frame header.
     * SO_RCVTIMEO=100ms ensures read() returns within 100ms if no data. */
    uint8_t header[5];
    int n = mbedtls_ssl_read(&c->ssl, header, 5);
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
        n == MBEDTLS_ERR_SSL_TIMEOUT) {
        return 0;  /* No data available / timeout */
    }
    if (n <= 0) {
        ESP_LOGW(TAG, "DERP header read returned %d (0x%04x)", n, n < 0 ? -n : 0);
        return n;
    }
    if (n < 5) {
        ESP_LOGW(TAG, "DERP partial header: got %d of 5 bytes", n);
        return -1;
    }

    uint8_t frame_type = header[0];
    uint32_t len = (header[1] << 24) | (header[2] << 16) | (header[3] << 8) | header[4];

    uint8_t src_key[32] = {0};
    uint8_t *payload = NULL;
    size_t payload_len = 0;

    if (len == 0) {
        dispatch_derp_frame(ml, c, frame_type, src_key, NULL, 0);
        return 1;
    }

    if (len > 65536) {
        ESP_LOGW(TAG, "DERP frame too large: %lu", (unsigned long)len);
        return -1;
    }

    /* Read frame payload - we already got the header so payload should follow.
     * Use longer timeout (2s) since we KNOW data is coming. */
    uint8_t *buf = ml_psram_malloc(len);
    if (!buf) return -1;

    size_t total_read = 0;
    uint64_t payload_start = ml_get_time_ms();
    while (total_read < len) {
        /* Safety timeout: 5 seconds for payload */
        if (ml_get_time_ms() - payload_start > 5000) {
            ESP_LOGW(TAG, "DERP payload timeout at %d/%lu bytes",
                     (int)total_read, (unsigned long)len);
            free(buf);
            return -1;
        }
        n = mbedtls_ssl_read(&c->ssl, buf + total_read, len - total_read);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
            n == MBEDTLS_ERR_SSL_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (n <= 0) {
            ESP_LOGW(TAG, "DERP payload read error: %d (0x%04x) at %d/%lu bytes",
                     n, n < 0 ? -n : 0, (int)total_read, (unsigned long)len);
            free(buf);
            return n;
        }
        total_read += n;
    }

    /* For RecvPacket (0x05): first 32 bytes are sender's public key */
    if (frame_type == DERP_FRAME_RECV_PACKET && len > 32) {
        memcpy(src_key, buf, 32);
        payload = malloc(len - 32);
        if (payload) {
            memcpy(payload, buf + 32, len - 32);
            payload_len = len - 32;
        }
        free(buf);
    } else {
        payload = buf;
        payload_len = len;
    }

    dispatch_derp_frame(ml, c, frame_type, src_key, payload, payload_len);
    return 1;
}

/* ============================================================================
 * DERP TX Queue Processing
 * ========================================================================== */

/* Queue a packet for DERP TX with backpressure */
esp_err_t ml_derp_queue_send(microlink_t *ml, const uint8_t *dest_key,
                              const uint8_t *data, size_t len) {
    return ml_derp_queue_send_traced(ml, dest_key, data, len, 0);
}

esp_err_t ml_derp_queue_send_traced(microlink_t *ml, const uint8_t *dest_key,
                              const uint8_t *data, size_t len, uint32_t trace_ms) {
    if (!ml || !dest_key || !data || len == 0) return ESP_ERR_INVALID_ARG;

    uint8_t *pkt_data = malloc(len);
    if (!pkt_data) return ESP_ERR_NO_MEM;
    memcpy(pkt_data, data, len);

    ml_derp_tx_item_t item = {
        .data = pkt_data,
        .len = len,
        .frame_type = DERP_FRAME_SEND_PACKET,
        .trace_ms = trace_ms,
    };
    memcpy(item.dest_pubkey, dest_key, 32);

    /* Hub port: route via the destination's home region. The peer table belongs to the
     * wg_mgr task; reading one region number unlocked can at worst pick a stale region
     * for one packet while a peer moves. */
    for (int i = 0; i < ml->peer_count; i++) {
        if (ml->peers[i].active && memcmp(ml->peers[i].public_key, dest_key, 32) == 0) {
            item.region = ml->peers[i].derp_region;
            break;
        }
    }

    /* WG handshake packets (type 1=init, 2=response) get priority — front of queue.
     * This ensures handshake responses aren't delayed behind DISCO pings. */
    bool is_wg_handshake = (len >= 4 && (data[0] == 0x01 || data[0] == 0x02));

    /* Try to send to queue */
    if ((is_wg_handshake ? xQueueSendToFront(ml->derp_tx_queue, &item, 0)
                         : xQueueSend(ml->derp_tx_queue, &item, 0)) == pdTRUE) {
        return ESP_OK;
    }

    /* Queue full - backpressure: drop oldest, retry up to 3 times */
    for (int i = 0; i < 3; i++) {
        ml_derp_tx_item_t dropped;
        if (xQueueReceive(ml->derp_tx_queue, &dropped, 0) == pdTRUE) {
            free(dropped.data);  /* Drop oldest */
        }
        if (xQueueSend(ml->derp_tx_queue, &item, 0) == pdTRUE) {
            return ESP_OK;
        }
    }

    /* Still full after 3 attempts, drop new packet */
    free(pkt_data);
    return ESP_ERR_TIMEOUT;
}

/* ============================================================================
 * Unified DERP I/O Task
 * ========================================================================== */

void ml_derp_tx_task(void *arg) {
    microlink_t *ml = (microlink_t *)arg;
    ml_derp_conn_t *c = &ml->derp;  /* home connection; extra ones via derp_conn_for() */
    ESP_LOGI(TAG, "DERP I/O task started (Core %d)", xPortGetCoreID());

    uint32_t frames_rx = 0;
    uint32_t frames_tx = 0;
    uint64_t last_status_ms = 0;
    uint32_t loop_count = 0;
    uint64_t last_heartbeat_ms = 0;
    uint64_t connected_since_ms = 0;
    bool verbose_phase = false;  /* verbose logging for first 15s after connect */

    while (!(xEventGroupGetBits(ml->events) & ML_EVT_SHUTDOWN_REQUEST)) {
        loop_count++;
        uint64_t loop_start = ml_get_time_ms();

        /* Unconditional heartbeat - proves task is alive */
        if (loop_start - last_heartbeat_ms > 5000) {
            ESP_LOGD(TAG, "HEARTBEAT: loop=%lu conn=%d rx=%lu tx=%lu stack_free=%lu",
                     (unsigned long)loop_count, c->connected,
                     (unsigned long)frames_rx, (unsigned long)frames_tx,
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            last_heartbeat_ms = loop_start;
        }

        /* ---- Periodic status logging (always, even when disconnected) ---- */
        {
            uint64_t now_ms = loop_start;
            if (now_ms - last_status_ms > 10000) {
                ESP_LOGD(TAG, "DERP status: connected=%d fd=%d rx=%lu tx=%lu loops=%lu",
                         c->connected, c->sockfd,
                         (unsigned long)frames_rx, (unsigned long)frames_tx,
                         (unsigned long)loop_count);
                last_status_ms = now_ms;
            }
        }

        /* ---- Handle DERP connect request from coord task ---- */
        {
            EventBits_t bits = xEventGroupGetBits(ml->events);
            if ((bits & ML_EVT_DERP_CONNECT_REQ) && !c->connected) {
                xEventGroupClearBits(ml->events, ML_EVT_DERP_CONNECT_REQ);
                /* Retry up to 3 times with 2s backoff */
                for (int attempt = 0; attempt < 3 && !c->connected; attempt++) {
                    if (attempt > 0) {
                        ESP_LOGW(TAG, "DERP connect retry %d/3 in 2s...", attempt + 1);
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    } else {
                        ESP_LOGI(TAG, "DERP connect requested, connecting from I/O task");
                    }
                    if (ml_derp_connect(ml) == ESP_OK) {
                        connected_since_ms = ml_get_time_ms();
                        verbose_phase = true;
                        break;
                    }
                    ESP_LOGW(TAG, "DERP connect attempt %d failed", attempt + 1);
                }
            }
            if (bits & ML_EVT_DERP_RECONNECT) {
                xEventGroupClearBits(ml->events, ML_EVT_DERP_RECONNECT);
                ESP_LOGW(TAG, "DERP reconnect requested (was %s)",
                         c->connected ? "connected" : "disconnected");
                ml_derp_disconnect(ml);
                verbose_phase = false;
                /* Auto-reconnect after disconnect */
                vTaskDelay(pdMS_TO_TICKS(1000));
                for (int attempt = 0; attempt < 3 && !c->connected; attempt++) {
                    if (attempt > 0) {
                        ESP_LOGW(TAG, "DERP reconnect retry %d/3 in 2s...", attempt + 1);
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    }
                    if (ml_derp_connect(ml) == ESP_OK) {
                        connected_since_ms = ml_get_time_ms();
                        verbose_phase = true;
                        break;
                    }
                    ESP_LOGW(TAG, "DERP reconnect attempt %d failed", attempt + 1);
                }
            }
        }

        /* Disable verbose logging after 15s */
        if (verbose_phase && ml_get_time_ms() - connected_since_ms > 15000) {
            verbose_phase = false;
        }

        if (!c->connected) {
            /* Not connected - just wait and check again */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* ---- Phase 1: Drain TX queue FIRST (prioritize outgoing) ---- */
        {
            for (int tx_count = 0; tx_count < 8; tx_count++) {
                ml_derp_tx_item_t item;
                if (xQueueReceive(ml->derp_tx_queue, &item, 0) != pdTRUE) {
                    break;
                }
                /* Hub port: the destination's home region (may open a connection) */
                ml_derp_conn_t *tc = c->connected ?
                                     derp_conn_for(ml, item.region, ml_get_time_ms()) : NULL;
                if (!tc) {
                    free(item.data);
                    continue;
                }
                int ret;
                if (item.frame_type == DERP_FRAME_SEND_PACKET) {
                    ESP_LOGD(TAG, "DERP TX: SendPacket %d bytes, dest=%02x%02x%02x%02x, hdr=%02x, region=%u",
                             (int)item.len, item.dest_pubkey[0], item.dest_pubkey[1],
                             item.dest_pubkey[2], item.dest_pubkey[3],
                             item.data[0], tc->region);
                    ret = derp_send_packet(tc, item.dest_pubkey, item.data, item.len);
                } else {
                    ret = derp_write_frame(tc, item.frame_type, item.data, item.len);
                }
                if (ret < 0) {
                    ESP_LOGW(TAG, "DERP write failed (region %u)", tc->region);
                    if (tc == c) {
                        c->connected = false;
                        xEventGroupSetBits(ml->events, ML_EVT_DERP_RECONNECT);
                    } else {
                        derp_close_conn(tc);
                    }
                } else {
                    frames_tx++;
                    tc->last_tx_ms = ml_get_time_ms();
                    if (item.trace_ms) derp_trace_record(item.trace_ms, (uint32_t)tc->last_tx_ms);
                }
                free(item.data);
            }
        }

        if (!c->connected) continue;

        /* ---- Phase 2: Poll for incoming DERP frames (home, then the others) ---- */
        for (int ci = -1; ci < ML_DERP_EXTRA_CONNS; ci++) {
            ml_derp_conn_t *rc = ci < 0 ? c : &ml->derp_extra[ci];
            if (!rc->connected) continue;
            for (int burst = 0; burst < 4; burst++) {
                int ret = poll_derp_read(ml, rc);
                if (ret > 0) {
                    frames_rx++;
                    rc->last_recv_ms = ml_get_time_ms();
                } else if (ret == 0) {
                    break;  /* No more data / timeout */
                } else {
                    ESP_LOGW(TAG, "DERP read error: %d (region %u)", ret, rc->region);
                    if (rc == c) {
                        c->connected = false;
                        xEventGroupSetBits(ml->events, ML_EVT_DERP_RECONNECT);
                    } else {
                        derp_close_conn(rc);
                    }
                    break;
                }
            }
        }

        /* Hub port: close other-region connections we haven't sent on for a while.
         * Only our sends count: every DERP server sends keepalives. */
        {
            uint64_t now_ms = ml_get_time_ms();
            for (int ci = 0; ci < ML_DERP_EXTRA_CONNS; ci++) {
                ml_derp_conn_t *xc = &ml->derp_extra[ci];
                if (xc->connected && now_ms - xc->last_tx_ms > ML_DERP_EXTRA_IDLE_MS) {
                    ESP_LOGI(TAG, "DERP region %u idle, closing", xc->region);
                    derp_close_conn(xc);
                }
            }
        }

        /* Yield briefly */
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGI(TAG, "DERP I/O task exiting");
    vTaskDeleteWithCaps(NULL);  /* hub port: PSRAM stack (WithCaps) */
}

/* ============================================================================
 * DERP Connection Management (called from coord task)
 * ========================================================================== */

/* Free a TLS context that never became a live connection, and close its socket. */
static void derp_tls_abort(ml_derp_conn_t *c) {
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->ssl_conf);
    if (c->sockfd >= 0) ml_close_sock(c->sockfd);
    c->sockfd = -1;
}

/* Hub port: one function for the home connection and the other-region ones. The host
 * is copied: the coord task may re-parse the DERPMap while this runs. */
static esp_err_t derp_connect_conn(microlink_t *ml, ml_derp_conn_t *c, uint16_t region) {
    /* Determine DERP host/port from DERPMap.
     * Always use the first DERP node of the region (the one most peers use). */
    char derp_host[64] = "";
    int derp_port = ML_DERP_PORT;
    bool is_home = (c == &ml->derp);

    for (int i = 0; i < ml->derp_region_count; i++) {
        const ml_derp_region_t *r = &ml->derp_regions[i];
        if (r->region_id != region) continue;
        for (int n = 0; n < r->node_count && n < ML_MAX_DERP_NODES; n++) {
            if (!r->nodes[n].stun_only && r->nodes[n].hostname[0]) {
                strlcpy(derp_host, r->nodes[n].hostname, sizeof(derp_host));
                if (r->nodes[n].derp_port > 0) derp_port = r->nodes[n].derp_port;
                break;
            }
        }
        break;
    }
    if (!derp_host[0]) {
        if (!is_home) {
            ESP_LOGW(TAG, "DERP region %u not in the DERPMap", region);
            return ESP_FAIL;
        }
        strlcpy(derp_host, ML_DERP_HOST, sizeof(derp_host));  /* no DERPMap yet */
        region = ML_DERP_REGION;
    }

    int64_t t_derp_start = esp_timer_get_time();

    ESP_LOGI(TAG, "Connecting to DERP %s:%d (region %u%s)",
             derp_host, derp_port, region, is_home ? ", home" : "");

    /* DNS resolve — accept IPv4 or IPv6 (carrier may be IPv6-only) */
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", derp_port);

    if (ml_getaddrinfo(derp_host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "DNS resolve failed for %s", derp_host);
        return ESP_FAIL;
    }

    int64_t t_derp_dns = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP DNS: %lld ms", (t_derp_dns - t_derp_start) / 1000);

    /* TCP connect — use address family from DNS result */
    int sock = ml_socket(res->ai_family, SOCK_STREAM, 0);
    if (sock < 0) {
        ml_freeaddrinfo(res);
        return ESP_FAIL;
    }

    /* Set connect timeout */
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    ml_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ml_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (ml_connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        ESP_LOGE(TAG, "TCP connect failed: %d", errno);
        ml_close_sock(sock);
        ml_freeaddrinfo(res);
        return ESP_FAIL;
    }
    ml_freeaddrinfo(res);

    int64_t t_derp_tcp = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP TCP connect: %lld ms", (t_derp_tcp - t_derp_dns) / 1000);

    /* Hub port: DERP frames are small and latency-bound (DISCO, relayed WireGuard):
     * send each one now instead of letting Nagle hold it for the previous ACK. */
    {
        int one = 1;
        ml_setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    /* TLS setup.
     * mbedTLS 4 takes randomness from PSA (no entropy/CTR-DRBG contexts), and the
     * server certificate is verified against the IDF CA bundle. Every failure from
     * here on frees the context via derp_tls_abort(): ~20 KB per failed attempt
     * would turn a long uplink outage into heap exhaustion. */
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->ssl_conf);
    c->sockfd = sock;

    int ret = mbedtls_ssl_config_defaults(&c->ssl_conf,
                                 MBEDTLS_SSL_IS_CLIENT,
                                 MBEDTLS_SSL_TRANSPORT_STREAM,
                                 MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret == 0) {
        mbedtls_ssl_conf_authmode(&c->ssl_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        if (esp_crt_bundle_attach(&c->ssl_conf) != ESP_OK) ret = -1;
    }
    if (ret == 0) {
        mbedtls_ssl_conf_read_timeout(&c->ssl_conf, DERP_CONNECT_TIMEOUT_MS);
        ret = mbedtls_ssl_setup(&c->ssl, &c->ssl_conf);
    }
    if (ret == 0) ret = mbedtls_ssl_set_hostname(&c->ssl, derp_host);
    if (ret != 0) {
        ESP_LOGE(TAG, "TLS setup failed: -0x%04x", -ret);
        derp_tls_abort(c);
        return ESP_FAIL;
    }
    /* Store socket fd BEFORE setting bio.
     * Use custom BIO callbacks that route through ml_read_sock/ml_write_sock,
     * which transparently support both lwIP and AT socket backends.
     * Timeout is handled via SO_RCVTIMEO. */
    mbedtls_ssl_set_bio(&c->ssl, &c->sockfd,
                         ml_derp_bio_send, NULL, ml_derp_bio_recv_timeout);

    /* TLS handshake - socket has 10s SO_RCVTIMEO from connect phase. */
    while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        char err_buf[128];
        mbedtls_strerror(ret, err_buf, sizeof(err_buf));
        ESP_LOGE(TAG, "TLS handshake failed: %s", err_buf);
        derp_tls_abort(c);
        return ESP_FAIL;
    }

    int64_t t_derp_tls = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP TLS handshake: %lld ms", (t_derp_tls - t_derp_tcp) / 1000);
    ESP_LOGI(TAG, "TLS connected to DERP");

    /* HTTP Upgrade: GET /derp with Upgrade: DERP header */
    char upgrade_req[256];
    snprintf(upgrade_req, sizeof(upgrade_req),
             "GET /derp HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: Upgrade\r\n"
             "Upgrade: DERP\r\n"
             "\r\n",
             derp_host);

    ret = mbedtls_ssl_write(&c->ssl, (const uint8_t *)upgrade_req, strlen(upgrade_req));
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send HTTP upgrade");
        derp_tls_abort(c);
        return ESP_FAIL;
    }

    /* Read HTTP response byte-by-byte until \r\n\r\n to avoid over-reading
     * into the DERP binary frame stream (matching v1 approach) */
    {
        uint8_t resp_buf[512];
        int resp_len = 0;
        bool found_end = false;
        uint64_t http_start = ml_get_time_ms();

        while (resp_len < (int)sizeof(resp_buf) - 1) {
            if (ml_get_time_ms() - http_start > DERP_CONNECT_TIMEOUT_MS) {
                ESP_LOGE(TAG, "HTTP upgrade response timeout");
                derp_tls_abort(c);
                return ESP_FAIL;
            }

            ret = mbedtls_ssl_read(&c->ssl, resp_buf + resp_len, 1);
            if (ret < 0) {
                if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                    ret == MBEDTLS_ERR_SSL_TIMEOUT) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                ESP_LOGE(TAG, "HTTP upgrade read failed: -0x%04x", -ret);
                derp_tls_abort(c);
                return ESP_FAIL;
            }
            if (ret == 0) {
                ESP_LOGE(TAG, "Connection closed during HTTP upgrade");
                derp_tls_abort(c);
                return ESP_FAIL;
            }
            resp_len++;

            /* Check for \r\n\r\n */
            if (resp_len >= 4 &&
                resp_buf[resp_len - 4] == '\r' && resp_buf[resp_len - 3] == '\n' &&
                resp_buf[resp_len - 2] == '\r' && resp_buf[resp_len - 1] == '\n') {
                found_end = true;
                break;
            }
        }

        resp_buf[resp_len] = '\0';

        if (!found_end || strstr((char *)resp_buf, "101") == NULL) {
            ESP_LOGE(TAG, "DERP upgrade rejected: %.100s", resp_buf);
            derp_tls_abort(c);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "HTTP 101 Switching Protocols received");
    }

    /* Match v1 exactly: O_NONBLOCK + short SO_RCVTIMEO + SO_SNDTIMEO.
     * O_NONBLOCK ensures read()/write() never block indefinitely.
     * SO_RCVTIMEO provides 100ms polling rhythm for reads.
     * SO_SNDTIMEO prevents writes from blocking too long. */
    {
        int flags = ml_fcntl(sock, F_GETFL, 0);
        if (flags >= 0) {
            ml_fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        }
        struct timeval io_tv = { .tv_sec = 0, .tv_usec = 100000 };  /* 100ms */
        ml_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &io_tv, sizeof(io_tv));
        ml_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &io_tv, sizeof(io_tv));
    }

    /* ========================================================
     * DERP Handshake: ServerKey -> ClientInfo -> ServerInfo
     * ======================================================== */

    /* Step 1: Read ServerKey frame header using reliable read helper */
    uint8_t frame_type;
    uint32_t frame_len;
    esp_err_t err = derp_recv_frame_header(c, &frame_type, &frame_len, DERP_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read ServerKey frame header (err=%d)", err);
        derp_tls_abort(c);
        return ESP_FAIL;
    }

    if (frame_type != DERP_FRAME_SERVER_KEY || frame_len < 40) {
        ESP_LOGE(TAG, "Expected ServerKey frame (0x01), got 0x%02x len=%lu",
                 frame_type, (unsigned long)frame_len);
        derp_tls_abort(c);
        return ESP_FAIL;
    }

    /* Read and verify 8-byte magic */
    uint8_t magic[8];
    static const uint8_t DERP_MAGIC[8] = {0x44, 0x45, 0x52, 0x50, 0xf0, 0x9f, 0x94, 0x91};
    if (derp_tls_read_all(c, magic, 8, DERP_CONNECT_TIMEOUT_MS) < 0) {
        ESP_LOGE(TAG, "Failed to read ServerKey magic");
        derp_tls_abort(c);
        return ESP_FAIL;
    }

    if (memcmp(magic, DERP_MAGIC, 8) != 0) {
        ESP_LOGE(TAG, "Invalid DERP magic: %02x%02x%02x%02x%02x%02x%02x%02x",
                 magic[0], magic[1], magic[2], magic[3],
                 magic[4], magic[5], magic[6], magic[7]);
        derp_tls_abort(c);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "DERP magic verified");

    /* Read 32-byte server public key */
    uint8_t derp_server_key[32];
    if (derp_tls_read_all(c, derp_server_key, 32, DERP_CONNECT_TIMEOUT_MS) < 0) {
        ESP_LOGE(TAG, "Failed to read server key");
        derp_tls_abort(c);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "DERP server key received (first 8): %02x%02x%02x%02x%02x%02x%02x%02x",
             derp_server_key[0], derp_server_key[1], derp_server_key[2], derp_server_key[3],
             derp_server_key[4], derp_server_key[5], derp_server_key[6], derp_server_key[7]);

    /* Skip remaining bytes if frame_len > 40 */
    if (frame_len > 40) {
        uint8_t skip_buf[64];
        size_t remaining = frame_len - 40;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(skip_buf) ? sizeof(skip_buf) : remaining;
            if (derp_tls_read_all(c, skip_buf, chunk, DERP_CONNECT_TIMEOUT_MS) < 0) break;
            remaining -= chunk;
        }
    }

    /* Step 2: Send ClientInfo frame (type 0x02)
     * Payload: [our_nodekey(32)][nonce(24)][nacl_box(JSON)] */
    {
        const char *client_info_json = "{\"Version\":2,\"CanAckPings\":true,\"IsProber\":false}";
        size_t json_len = strlen(client_info_json);

        /* Generate random nonce */
        uint8_t nonce[NACL_BOX_NONCEBYTES];
        esp_fill_random(nonce, NACL_BOX_NONCEBYTES);

        /* Encrypt JSON with NaCl box: our WG private key -> DERP server public key */
        size_t ciphertext_len = json_len + NACL_BOX_MACBYTES;
        uint8_t *ciphertext = malloc(ciphertext_len);
        if (!ciphertext) {
            derp_tls_abort(c);
            return ESP_FAIL;
        }

        if (nacl_box(ciphertext,
                     (const uint8_t *)client_info_json, json_len,
                     nonce,
                     derp_server_key,       /* recipient: DERP server */
                     ml->wg_private_key     /* sender: our WG node key */
                     ) != 0) {
            ESP_LOGE(TAG, "NaCl box encrypt failed");
            free(ciphertext);
            derp_tls_abort(c);
            return ESP_FAIL;
        }

        /* Build ClientInfo frame payload: nodekey(32) + nonce(24) + ciphertext */
        size_t ci_payload_len = 32 + NACL_BOX_NONCEBYTES + ciphertext_len;
        uint8_t *ci_payload = malloc(ci_payload_len);
        if (!ci_payload) {
            free(ciphertext);
            derp_tls_abort(c);
            return ESP_FAIL;
        }

        memcpy(ci_payload, ml->wg_public_key, 32);
        memcpy(ci_payload + 32, nonce, NACL_BOX_NONCEBYTES);
        memcpy(ci_payload + 32 + NACL_BOX_NONCEBYTES, ciphertext, ciphertext_len);
        free(ciphertext);

        ESP_LOGI(TAG, "DERP ClientInfo node_key=%02x%02x%02x%02x%02x%02x%02x%02x",
                 ml->wg_public_key[0], ml->wg_public_key[1],
                 ml->wg_public_key[2], ml->wg_public_key[3],
                 ml->wg_public_key[4], ml->wg_public_key[5],
                 ml->wg_public_key[6], ml->wg_public_key[7]);

        /* Send ClientInfo frame */
        if (derp_write_frame(c, DERP_FRAME_CLIENT_INFO, ci_payload, ci_payload_len) < 0) {
            ESP_LOGE(TAG, "Failed to send ClientInfo");
            free(ci_payload);
            derp_tls_abort(c);
            return ESP_FAIL;
        }
        free(ci_payload);

        ESP_LOGI(TAG, "ClientInfo sent");
    }

    /* Step 3: Read ServerInfo frame (type 0x03) */
    {
        uint8_t si_type;
        uint32_t si_len;
        err = derp_recv_frame_header(c, &si_type, &si_len, DERP_CONNECT_TIMEOUT_MS);
        if (err == ESP_OK && si_type == DERP_FRAME_SERVER_INFO && si_len > 0) {
            /* Read and discard ServerInfo payload */
            uint8_t *si_buf = malloc(si_len);
            if (si_buf) {
                derp_tls_read_all(c, si_buf, si_len, DERP_CONNECT_TIMEOUT_MS);
                free(si_buf);
            }
            ESP_LOGI(TAG, "ServerInfo received (discarded)");
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "No ServerInfo frame (continuing anyway)");
        }
    }

    /* Send NotePreferred (type 0x07): only the home connection is our preferred DERP
     * (the server counts clients by home region) */
    {
        uint8_t preferred = is_home ? 0x01 : 0x00;
        derp_write_frame(c, DERP_FRAME_NOTE_PREFERRED, &preferred, 1);
    }

    /* Switch socket to short timeout for data phase.
     * Long timeout was needed for TLS handshake, but polling must be fast. */
    {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };  /* 200ms */
        ml_setsockopt(c->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        mbedtls_ssl_conf_read_timeout(&c->ssl_conf, 200);
    }

    c->region = region;
    c->connected = true;
    c->last_recv_ms = ml_get_time_ms();
    c->last_tx_ms = c->last_recv_ms;
    if (is_home) xEventGroupSetBits(ml->events, ML_EVT_DERP_CONNECTED);

    int64_t t_derp_done = esp_timer_get_time();
    ESP_LOGI(TAG, "[TIMING] DERP total: %lld ms (DNS=%lld, TCP=%lld, TLS=%lld, proto=%lld)",
             (t_derp_done - t_derp_start) / 1000,
             (t_derp_dns - t_derp_start) / 1000,
             (t_derp_tcp - t_derp_dns) / 1000,
             (t_derp_tls - t_derp_tcp) / 1000,
             (t_derp_done - t_derp_tls) / 1000);
    ESP_LOGI(TAG, "DERP handshake complete, connected (region %u%s)", region, is_home ? ", home" : "");
    return ESP_OK;
}

esp_err_t ml_derp_connect(microlink_t *ml) {
    return derp_connect_conn(ml, &ml->derp, ml->derp_home_region);
}

/* Hub port: close one connection (DERP I/O task only). */
static void derp_close_conn(ml_derp_conn_t *c) {
    c->connected = false;
    if (c->sockfd >= 0) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->ssl_conf);
        ml_close_sock(c->sockfd);
        c->sockfd = -1;
    }
}

/* Hub port: regions whose connection failed recently. Until the back-off ends, packets
 * for them are dropped instead of stalling this task in another connect attempt
 * (a connect blocks the I/O task, up to 10 s against an unreachable server). */
#define DERP_REGION_BACKOFF_MS  60000
static struct { uint16_t region; uint64_t until_ms; } s_region_backoff[4];

/* The connection that reaches peers homed in `region`, opening one if needed.
 * Runs on the DERP I/O task. NULL = drop the packet. */
static ml_derp_conn_t *derp_conn_for(microlink_t *ml, uint16_t region, uint64_t now_ms) {
    ml_derp_conn_t *home = &ml->derp;
    if (region == 0 || region == home->region) return home;

    ml_derp_conn_t *slot = NULL;
    for (int i = 0; i < ML_DERP_EXTRA_CONNS; i++) {
        ml_derp_conn_t *x = &ml->derp_extra[i];
        if (x->connected && x->region == region) return x;
        if (!x->connected && !slot) slot = x;
    }

    int bo = 0;
    for (int i = 0; i < 4; i++) {
        if (s_region_backoff[i].region == region) {
            if (now_ms < s_region_backoff[i].until_ms) return NULL;
            bo = i;
            break;
        }
        if (s_region_backoff[i].until_ms < s_region_backoff[bo].until_ms) bo = i;
    }

    if (!slot) {  /* all in use: replace the least recently used */
        slot = &ml->derp_extra[0];
        for (int i = 1; i < ML_DERP_EXTRA_CONNS; i++) {
            if (ml->derp_extra[i].last_tx_ms < slot->last_tx_ms) slot = &ml->derp_extra[i];
        }
        ESP_LOGI(TAG, "DERP region %u replaced by %u", slot->region, region);
        derp_close_conn(slot);
    }

    if (derp_connect_conn(ml, slot, region) != ESP_OK) {
        ESP_LOGW(TAG, "DERP region %u unreachable, retry in %d s", region,
                 DERP_REGION_BACKOFF_MS / 1000);
        s_region_backoff[bo].region = region;
        s_region_backoff[bo].until_ms = ml_get_time_ms() + DERP_REGION_BACKOFF_MS;
        return NULL;
    }
    return slot;
}

void ml_derp_disconnect(microlink_t *ml) {
    ml->derp.connected = false;
    xEventGroupClearBits(ml->events, ML_EVT_DERP_CONNECTED);
    derp_close_conn(&ml->derp);
    for (int i = 0; i < ML_DERP_EXTRA_CONNS; i++) derp_close_conn(&ml->derp_extra[i]);

    /* Drain TX queue */
    ml_derp_tx_item_t item;
    while (xQueueReceive(ml->derp_tx_queue, &item, 0) == pdTRUE) {
        free(item.data);
    }

    ESP_LOGI(TAG, "DERP disconnected");
}
