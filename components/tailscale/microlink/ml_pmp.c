/**
 * @file ml_pmp.c
 * @brief NAT-PMP port mapping for the DISCO/WireGuard UDP port (hub port, RFC 6886)
 *
 * Why: behind the ISP's CGNAT, packets to some peers (another customer of the
 * same ISP) don't leave through the CGNAT at all: they go out on the ISP's internal
 * network from the router's WAN address (10.x) and a port the router picks per flow.
 * That peer's router only accepts replies from the exact address:port it sent to, and
 * the hub never advertised one, so no direct path could form. Measured 2026-11-07:
 * a NAT-PMP-mapped port goes out as <router WAN>:<mapped port>, unchanged, and arrives.
 * tailscaled on the dev machine does the same (advertises <router WAN>:41641).
 *
 * So: map disco_local_port on the gateway, advertise <WAN>:<port> as a "portmapped"
 * endpoint (tailcfg type 3), renew at half the lifetime. A router without NAT-PMP
 * just doesn't answer: retried every 10 min, nothing else changes.
 *
 * Runs on the coord task. Every exchange is bounded: at most 2 tries x 250 ms each,
 * two exchanges per refresh.
 */

#include "microlink_internal.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "ml_pmp";

#define PMP_PORT            5351
#define PMP_LIFETIME_S      7200            /* asked for; the router may grant less */
#define PMP_RETRY_MS        (10 * 60 * 1000)
#define PMP_TRY_TIMEOUT_MS  250
#define PMP_TRIES           2

/* One request/response with the gateway. Returns the response length, or -1. */
static int pmp_exchange(uint32_t gw_nbo, const uint8_t *req, size_t req_len,
                        uint8_t *resp, size_t resp_size, uint8_t expect_op) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return -1;
    struct timeval tv = { .tv_sec = 0, .tv_usec = PMP_TRY_TIMEOUT_MS * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in gw = { .sin_family = AF_INET, .sin_port = htons(PMP_PORT),
                              .sin_addr.s_addr = gw_nbo };
    int got = -1;
    for (int t = 0; t < PMP_TRIES && got < 0; t++) {
        if (sendto(s, req, req_len, 0, (struct sockaddr *)&gw, sizeof(gw)) < 0) break;
        for (;;) {  /* ignore stray packets; the receive timeout bounds this */
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int n = recvfrom(s, resp, resp_size, 0, (struct sockaddr *)&from, &fl);
            if (n < 0) break;  /* timeout: next try */
            if (from.sin_addr.s_addr != gw_nbo || from.sin_port != htons(PMP_PORT)) continue;
            if (n < 8 || resp[0] != 0 || resp[1] != expect_op) continue;
            got = n;
            break;
        }
    }
    close(s);
    return got;
}

bool ml_pmp_refresh(microlink_t *ml) {
    uint64_t now = ml_get_time_ms();
    if (now < ml->pmp_next_ms || ml->disco_local_port == 0) return false;

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.gw.addr == 0) {
        ml->pmp_next_ms = now + PMP_RETRY_MS;
        return false;
    }

    uint32_t old_ip = ml->pmp_ip;
    uint16_t old_port = ml->pmp_port;
    uint8_t resp[16];

    /* Opcode 0: the gateway's external address. */
    static const uint8_t req_addr[2] = {0, 0};
    int n = pmp_exchange(ip.gw.addr, req_addr, sizeof(req_addr), resp, sizeof(resp), 128);
    uint16_t result = n >= 12 ? (uint16_t)((resp[2] << 8) | resp[3]) : 0xFFFF;
    uint32_t wan = n >= 12 ? ((uint32_t)resp[8] << 24) | ((uint32_t)resp[9] << 16) |
                             ((uint32_t)resp[10] << 8) | resp[11] : 0;

    /* Opcode 1: map UDP disco_local_port (same external port requested). */
    uint16_t ext = 0;
    uint32_t life = 0;
    if (result == 0 && wan != 0) {
        uint8_t req_map[12] = {0, 1, 0, 0,
                               (uint8_t)(ml->disco_local_port >> 8), (uint8_t)ml->disco_local_port,
                               (uint8_t)(ml->disco_local_port >> 8), (uint8_t)ml->disco_local_port,
                               (uint8_t)(PMP_LIFETIME_S >> 24), (uint8_t)(PMP_LIFETIME_S >> 16),
                               (uint8_t)(PMP_LIFETIME_S >> 8), (uint8_t)PMP_LIFETIME_S};
        n = pmp_exchange(ip.gw.addr, req_map, sizeof(req_map), resp, sizeof(resp), 129);
        result = n >= 16 ? (uint16_t)((resp[2] << 8) | resp[3]) : 0xFFFF;
        if (result == 0) {
            ext = (uint16_t)((resp[10] << 8) | resp[11]);
            life = ((uint32_t)resp[12] << 24) | ((uint32_t)resp[13] << 16) |
                   ((uint32_t)resp[14] << 8) | resp[15];
        }
    }

    if (result != 0 || ext == 0 || life == 0) {
        if (old_ip) ESP_LOGW(TAG, "NAT-PMP mapping lost (result %u)", result);
        else if (!ml->pmp_tried) ESP_LOGI(TAG, "no NAT-PMP on the gateway (result %u)", result);
        ml->pmp_tried = true;
        ml->pmp_ip = 0;
        ml->pmp_port = 0;
        ml->pmp_next_ms = now + PMP_RETRY_MS;
        return old_ip != 0;
    }

    ml->pmp_tried = true;
    ml->pmp_ip = wan;
    ml->pmp_port = ext;
    ml->pmp_next_ms = now + (uint64_t)life * 1000 / 2;  /* renew at half-life */
    bool changed = (wan != old_ip || ext != old_port);
    if (changed) {
        ESP_LOGI(TAG, "NAT-PMP: %lu.%lu.%lu.%lu:%u -> local :%u for %lu s",
                 (unsigned long)(wan >> 24), (unsigned long)(wan >> 16 & 0xff),
                 (unsigned long)(wan >> 8 & 0xff), (unsigned long)(wan & 0xff),
                 ext, ml->disco_local_port, (unsigned long)life);
    }
    return changed;
}
