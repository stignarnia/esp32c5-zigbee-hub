/**
 * @file microlink.c
 * @brief MicroLink v2 - Public API and Task Orchestration
 *
 * Creates all FreeRTOS tasks, queues, and event groups.
 * Provides the public API that the application calls.
 */

#include "microlink_internal.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include "lwip/sockets.h"

static const char *TAG = "microlink";

/* NVS keys */
#define NVS_NAMESPACE       "microlink"
#define NVS_KEY_MACHINE_PRI "machine_pri"
#define NVS_KEY_MACHINE_PUB "machine_pub"
#define NVS_KEY_WG_PRI      "wg_private"
#define NVS_KEY_WG_PUB      "wg_public"
#define NVS_KEY_DISCO_PRI   "disco_pri"
#define NVS_KEY_DISCO_PUB   "disco_pub"

/* X25519 from x25519.h */
#include "x25519.h"

/* ============================================================================
 * Key Management (loaded once at init, read-only after)
 * ========================================================================== */

static void generate_keypair(uint8_t *private_key, uint8_t *public_key) {
    esp_fill_random(private_key, 32);
    private_key[0] &= 248;
    private_key[31] &= 127;
    private_key[31] |= 64;
    x25519_base(public_key, private_key, 1);
}

static esp_err_t load_or_generate_keys(microlink_t *ml) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed (%d), generating ephemeral keys", err);
        generate_keypair(ml->machine_private_key, ml->machine_public_key);
        generate_keypair(ml->wg_private_key, ml->wg_public_key);
        generate_keypair(ml->disco_private_key, ml->disco_public_key);
        return ESP_OK;
    }

    size_t key_len = 32;
    bool need_save = false;

    /* Machine key */
    if (nvs_get_blob(nvs, NVS_KEY_MACHINE_PRI, ml->machine_private_key, &key_len) != ESP_OK) {
        generate_keypair(ml->machine_private_key, ml->machine_public_key);
        need_save = true;
        ESP_LOGI(TAG, "Generated new machine key");
    } else {
        x25519_base(ml->machine_public_key, ml->machine_private_key, 1);
    }

    /* WireGuard key */
    key_len = 32;
    if (nvs_get_blob(nvs, NVS_KEY_WG_PRI, ml->wg_private_key, &key_len) != ESP_OK) {
        generate_keypair(ml->wg_private_key, ml->wg_public_key);
        need_save = true;
        ESP_LOGI(TAG, "Generated new WireGuard key");
    } else {
        x25519_base(ml->wg_public_key, ml->wg_private_key, 1);
    }

    /* DISCO key */
    key_len = 32;
    if (nvs_get_blob(nvs, NVS_KEY_DISCO_PRI, ml->disco_private_key, &key_len) != ESP_OK) {
        generate_keypair(ml->disco_private_key, ml->disco_public_key);
        need_save = true;
        ESP_LOGI(TAG, "Generated new DISCO key");
    } else {
        x25519_base(ml->disco_public_key, ml->disco_private_key, 1);
    }

    if (need_save) {
        nvs_set_blob(nvs, NVS_KEY_MACHINE_PRI, ml->machine_private_key, 32);
        nvs_set_blob(nvs, NVS_KEY_MACHINE_PUB, ml->machine_public_key, 32);
        nvs_set_blob(nvs, NVS_KEY_WG_PRI, ml->wg_private_key, 32);
        nvs_set_blob(nvs, NVS_KEY_WG_PUB, ml->wg_public_key, 32);
        nvs_set_blob(nvs, NVS_KEY_DISCO_PRI, ml->disco_private_key, 32);
        nvs_set_blob(nvs, NVS_KEY_DISCO_PUB, ml->disco_public_key, 32);
        nvs_commit(nvs);
        ESP_LOGI(TAG, "Keys saved to NVS");
    } else {
        ESP_LOGI(TAG, "Keys loaded from NVS");
    }

    nvs_close(nvs);
    return ESP_OK;
}

/* ============================================================================
 * Factory Reset
 * ========================================================================== */

esp_err_t microlink_factory_reset(void) {
    esp_err_t err;

    /* Erase key namespace */
    nvs_handle_t nvs;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Factory reset: keys erased");
    }

    ESP_LOGI(TAG, "Factory reset complete");
    return ESP_OK;
}

/* ============================================================================
 * Public API
 * ========================================================================== */

microlink_t *microlink_init(const microlink_config_t *config) {
    if (!config || !config->auth_key) {
        ESP_LOGE(TAG, "Invalid config: auth_key required");
        return NULL;
    }

    /* Allocate context from PSRAM */
    microlink_t *ml = ml_psram_calloc(1, sizeof(microlink_t));
    if (!ml) {
        ESP_LOGE(TAG, "Failed to allocate context");
        return NULL;
    }

    /* Copy config */
    ml->config = *config;
    if (ml->config.max_peers == 0) ml->config.max_peers = ML_MAX_PEERS;
    if (ml->config.max_peers > ML_MAX_PEERS) ml->config.max_peers = ML_MAX_PEERS;
    ml->config.enable_derp = true;  /* Always need DERP for relay */

    ml->state = ML_STATE_IDLE;
    ml->coord_sock = -1;
    ml->disco_sock4 = -1;
    ml->disco_sock6 = -1;
    ml->stun_sock = -1;
    ml->stun_sock6 = -1;
    ml->derp.sockfd = -1;
    for (int i = 0; i < ML_DERP_EXTRA_CONNS; i++) ml->derp_extra[i].sockfd = -1;

    /* Resolve timing (0 = use defaults from #defines) */
    ml->t_disco_heartbeat_ms = ml->config.disco_heartbeat_ms ? ml->config.disco_heartbeat_ms : ML_DISCO_HEARTBEAT_MS;
    ml->t_stun_interval_ms = ml->config.stun_interval_ms ? ml->config.stun_interval_ms : ML_STUN_RESTUN_INTERVAL_MS;
    ml->t_ctrl_watchdog_ms = ml->config.ctrl_watchdog_ms ? ml->config.ctrl_watchdog_ms : ML_CTRL_WATCHDOG_MS;

    /* Load or generate persistent keys */
    if (load_or_generate_keys(ml) != ESP_OK) {
        free(ml);
        return NULL;
    }

    /* Create event group */
    ml->events = xEventGroupCreate();
    if (!ml->events) {
        ESP_LOGE(TAG, "Failed to create event group");
        free(ml);
        return NULL;
    }

    /* Create queues */
    ml->derp_tx_queue = xQueueCreate(ML_DERP_TX_QUEUE_DEPTH, sizeof(ml_derp_tx_item_t));
    ml->disco_rx_queue = xQueueCreate(ML_DISCO_RX_QUEUE_DEPTH, sizeof(ml_rx_packet_t));
    ml->wg_rx_queue = xQueueCreate(ML_WG_RX_QUEUE_DEPTH, sizeof(ml_rx_packet_t));
    ml->stun_rx_queue = xQueueCreate(ML_STUN_RX_QUEUE_DEPTH, sizeof(ml_rx_packet_t));
    ml->coord_cmd_queue = xQueueCreate(ML_COORD_CMD_QUEUE_DEPTH, sizeof(ml_coord_cmd_t));
    ml->peer_update_queue = xQueueCreate(ML_PEER_UPDATE_QUEUE_DEPTH, sizeof(ml_peer_update_t *));

    if (!ml->derp_tx_queue || !ml->disco_rx_queue || !ml->wg_rx_queue ||
        !ml->stun_rx_queue || !ml->coord_cmd_queue || !ml->peer_update_queue) {
        ESP_LOGE(TAG, "Failed to create queues");
        microlink_destroy(ml);
        return NULL;
    }

    ESP_LOGI(TAG, "MicroLink v2 initialized (max_peers=%d)", ml->config.max_peers);
    return ml;
}

esp_err_t microlink_start(microlink_t *ml) {
    if (!ml) return ESP_ERR_INVALID_ARG;
    if (ml->state != ML_STATE_IDLE) {
        ESP_LOGW(TAG, "Already started (state=%d)", ml->state);
        return ESP_ERR_INVALID_STATE;
    }

    ml->state = ML_STATE_WIFI_WAIT;

    /* microlink never touches the Wi-Fi driver (wifi5g owns it: 5 GHz only, its own
     * watchdog). */

    /* Create DISCO/magicsock UDP socket (port 51820 = WireGuard standard)
     * This is the single socket for ALL direct UDP traffic: DISCO pings/pongs,
     * CallMeMaybe probes, and WireGuard encrypted data.
     * Matches v1 microlink_disco_init() and tailscale's pconn4. */
    ml->disco_sock4 = ml_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ml->disco_sock4 >= 0) {
        struct sockaddr_in bind_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(51820),
            .sin_addr.s_addr = INADDR_ANY,
        };
        if (ml_bind(ml->disco_sock4, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            ESP_LOGW(TAG, "Failed to bind port 51820 (errno=%d), trying ephemeral", errno);
            bind_addr.sin_port = 0;
            ml_bind(ml->disco_sock4, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
        }

        /* Mark packets as DSCP 46 (Expedited Forwarding) → WMM AC_VO.
         * WiFi APs with WMM use shorter contention windows for voice-priority
         * traffic, reducing jitter on WireGuard/DISCO UDP packets. */
        int tos = 0xB8;  /* DSCP 46 = EF, TOS byte = 46 << 2 = 184 */
        setsockopt(ml->disco_sock4, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

        /* Set non-blocking for select() in net_io */
        int flags = ml_fcntl(ml->disco_sock4, F_GETFL, 0);
        ml_fcntl(ml->disco_sock4, F_SETFL, flags | O_NONBLOCK);

        /* Record the actual bound port (getsockname not wrapped — AT sockets use stored port) */
        struct sockaddr_in local_addr;
        socklen_t addr_len = sizeof(local_addr);
        getsockname(ml->disco_sock4, (struct sockaddr *)&local_addr, &addr_len);
        ml->disco_local_port = ntohs(local_addr.sin_port);
        ESP_LOGI(TAG, "DISCO/magicsock UDP socket bound to port %d", ml->disco_local_port);
    } else {
        ESP_LOGE(TAG, "Failed to create DISCO socket: errno=%d", errno);
    }

    /* Create tasks */
    BaseType_t ret;

    ret = xTaskCreatePinnedToCoreWithCaps(ml_net_io_task, "ml_net_io", ML_TASK_NET_IO_STACK,
                                   ml, ML_TASK_NET_IO_PRIO, &ml->net_io_task, ML_TASK_NET_IO_CORE,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create net_io task");
        return ESP_FAIL;
    }

    ret = xTaskCreatePinnedToCoreWithCaps(ml_derp_tx_task, "ml_derp_tx", ML_TASK_DERP_TX_STACK,
                                   ml, ML_TASK_DERP_TX_PRIO, &ml->derp_tx_task, ML_TASK_DERP_TX_CORE,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create derp_tx task");
        return ESP_FAIL;
    }

    ret = xTaskCreatePinnedToCoreWithCaps(ml_coord_task, "ml_coord", ML_TASK_COORD_STACK,
                                   ml, ML_TASK_COORD_PRIO, &ml->coord_task, ML_TASK_COORD_CORE,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create coord task");
        return ESP_FAIL;
    }

    ret = xTaskCreatePinnedToCoreWithCaps(ml_wg_mgr_task, "ml_wg_mgr", ML_TASK_WG_MGR_STACK,
                                   ml, ML_TASK_WG_MGR_PRIO, &ml->wg_mgr_task, ML_TASK_WG_MGR_CORE,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create wg_mgr task");
        return ESP_FAIL;
    }

    /* WiFi is expected to be connected before microlink_start() is called.
     * Signal the event so coord/wg_mgr tasks proceed immediately. */
    xEventGroupSetBits(ml->events, ML_EVT_WIFI_CONNECTED);

    /* Signal coord task to start connecting */
    ml_coord_cmd_t cmd = ML_CMD_CONNECT;
    xQueueSend(ml->coord_cmd_queue, &cmd, 0);

    ESP_LOGI(TAG, "All tasks started");
    return ESP_OK;
}

esp_err_t microlink_rebind(microlink_t *ml) {
    if (!ml) return ESP_ERR_INVALID_ARG;
    if (ml->state == ML_STATE_IDLE) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "=== Rebinding to new network interface ===");

    /* Step 1: Invalidate socket FDs FIRST, then delay to let net_io_task's
     * select() cycle complete (50ms timeout). Only THEN close the old FDs.
     * Closing a socket while another thread has it in select() deadlocks
     * on lwIP's global socket lock. */
    int old_disco = ml->disco_sock4;
    int old_stun = ml->stun_sock;
    int old_stun6 = ml->stun_sock6;

    /* Invalidate — net_io_task will skip these on next iteration */
    ml->disco_sock4 = -1;
    ml->stun_sock = -1;
    ml->stun_sock6 = -1;

    /* Wait for net_io_task select() to cycle out (50ms timeout + margin) */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Now safe to close the old FDs */
    if (old_disco >= 0) ml_close_sock(old_disco);
    if (old_stun >= 0) ml_close_sock(old_stun);
    if (old_stun6 >= 0) ml_close_sock(old_stun6);
    ESP_LOGI(TAG, "Rebind: closed DISCO + STUN sockets");

    /* Step 2: Signal coord to reconnect. ML_CMD_FORCE_RECONNECT closes
     * the coord socket, resets Noise state, and re-enters the
     * STUN → DNS → TCP → Noise → Register → MapRequest flow.
     * Peers and WG state are preserved. */
    xEventGroupClearBits(ml->events, ML_EVT_COORD_REGISTERED);
    ml_coord_cmd_t cmd = ML_CMD_FORCE_RECONNECT;
    xQueueSend(ml->coord_cmd_queue, &cmd, pdMS_TO_TICKS(100));

    /* Step 3: Signal DERP to reconnect. ML_EVT_DERP_RECONNECT triggers
     * derp_tx_task to close TLS, then reconnect with full handshake.
     * Pending TX packets are drained but WG state is preserved. */
    xEventGroupClearBits(ml->events, ML_EVT_DERP_CONNECTED);
    xEventGroupSetBits(ml->events, ML_EVT_DERP_RECONNECT);
    ESP_LOGI(TAG, "Rebind: signaled coord + DERP to reconnect");

    /* Step 4: Brief delay for coord/DERP to process reconnect signals */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Step 5: Recreate DISCO UDP socket on the new interface */
    ml->disco_sock4 = ml_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ml->disco_sock4 >= 0) {
        struct sockaddr_in bind_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(51820),
            .sin_addr.s_addr = INADDR_ANY,
        };
        if (ml_bind(ml->disco_sock4, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            bind_addr.sin_port = 0;
            ml_bind(ml->disco_sock4, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
        }
        int tos = 0xB8;
        setsockopt(ml->disco_sock4, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
        int flags = ml_fcntl(ml->disco_sock4, F_GETFL, 0);
        ml_fcntl(ml->disco_sock4, F_SETFL, flags | O_NONBLOCK);

        struct sockaddr_in local_addr;
        socklen_t addr_len = sizeof(local_addr);
        getsockname(ml->disco_sock4, (struct sockaddr *)&local_addr, &addr_len);
        ml->disco_local_port = ntohs(local_addr.sin_port);
        ESP_LOGI(TAG, "Rebind: DISCO socket rebound to port %d", ml->disco_local_port);
    } else {
        ESP_LOGE(TAG, "Rebind: failed to create DISCO socket: errno=%d", errno);
    }

    /* Step 6: Update WG output mode for new transport */
    ml_wg_mgr_update_transport(ml);

    ESP_LOGI(TAG, "=== Rebind complete — waiting for coord+DERP reconnect ===");
    return ESP_OK;
}

esp_err_t microlink_stop(microlink_t *ml) {
    if (!ml) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Stopping...");
    xEventGroupSetBits(ml->events, ML_EVT_SHUTDOWN_REQUEST);

    /* Wait for tasks to exit (they check ML_EVT_SHUTDOWN_REQUEST).
     * Tasks call vTaskDelete(NULL) to self-delete, so we must NOT call
     * vTaskDelete() on them again — that causes a crash in uxListRemove
     * because the task's list node is already invalid. Just wait and
     * NULL the handles. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    ml->net_io_task = NULL;
    ml->derp_tx_task = NULL;
    ml->coord_task = NULL;
    ml->wg_mgr_task = NULL;

    /* Close sockets */
    if (ml->disco_sock4 >= 0) { ml_close_sock(ml->disco_sock4); ml->disco_sock4 = -1; }
    if (ml->stun_sock >= 0) { ml_close_sock(ml->stun_sock); ml->stun_sock = -1; }
    if (ml->stun_sock6 >= 0) { ml_close_sock(ml->stun_sock6); ml->stun_sock6 = -1; }

    ml->state = ML_STATE_IDLE;
    ESP_LOGI(TAG, "Stopped");
    return ESP_OK;
}

void microlink_destroy(microlink_t *ml) {
    if (!ml) return;

    microlink_stop(ml);

    /* Delete queues */
    if (ml->derp_tx_queue) vQueueDelete(ml->derp_tx_queue);
    if (ml->disco_rx_queue) vQueueDelete(ml->disco_rx_queue);
    if (ml->wg_rx_queue) vQueueDelete(ml->wg_rx_queue);
    if (ml->stun_rx_queue) vQueueDelete(ml->stun_rx_queue);
    if (ml->coord_cmd_queue) vQueueDelete(ml->coord_cmd_queue);
    if (ml->peer_update_queue) vQueueDelete(ml->peer_update_queue);

    /* Delete event group */
    if (ml->events) vEventGroupDelete(ml->events);

    /* Clear keys from memory */
    memset(ml->machine_private_key, 0, 32);
    memset(ml->wg_private_key, 0, 32);
    memset(ml->disco_private_key, 0, 32);

    free(ml);
    ESP_LOGI(TAG, "Destroyed");
}

/* ============================================================================
 * State Queries
 * ========================================================================== */

microlink_state_t microlink_get_state(const microlink_t *ml) {
    return ml ? ml->state : ML_STATE_IDLE;
}

bool microlink_is_connected(const microlink_t *ml) {
    return ml && ml->state == ML_STATE_CONNECTED;
}

const char *microlink_get_last_error(const microlink_t *ml) {
    return ml ? ml->reg_error : "";
}

const char *microlink_get_self_name(const microlink_t *ml) {
    return ml ? ml->self_name : "";
}

bool microlink_key_expired(const microlink_t *ml) {
    return ml && ml->key_expired;
}

int64_t microlink_get_key_expiry(const microlink_t *ml) {
    return ml ? ml->key_expiry_epoch : 0;
}

uint32_t microlink_get_vpn_ip(const microlink_t *ml) {
    return ml ? ml->vpn_ip : 0;
}

/* hub port: DERPMap code for a region id ("" if unknown) */
static const char *region_code(const microlink_t *ml, uint16_t id) {
    for (int i = 0; id && i < ml->derp_region_count; i++) {
        if (ml->derp_regions[i].region_id == id) return ml->derp_regions[i].code;
    }
    return "";
}

void microlink_set_ui_watching(microlink_t *ml, bool watching) {
    if (ml) ml->ui_watching = watching;
}

const char *microlink_get_home_derp(const microlink_t *ml) {
    return (ml && ml->derp.connected) ? region_code(ml, ml->derp.region) : "";
}

int microlink_get_peer_count(const microlink_t *ml) {
    return ml ? ml->peer_count : 0;
}

esp_err_t microlink_get_peer_info(const microlink_t *ml, int index, microlink_peer_info_t *info) {
    if (!ml || !info || index < 0 || index >= ml->peer_count) {
        return ESP_ERR_INVALID_ARG;
    }
    const ml_peer_t *p = &ml->peers[index];
    if (!p->active) return ESP_ERR_NOT_FOUND;  /* hub port: removed slot */
    info->vpn_ip = p->vpn_ip;
    strncpy(info->hostname, p->hostname, sizeof(info->hostname) - 1);
    memcpy(info->public_key, p->public_key, 32);
    info->online_state = p->ts_online;
    info->online = p->ts_online == 1;
    info->direct_path = p->has_direct_path;
    strlcpy(info->derp, region_code(ml, p->derp_region), sizeof(info->derp));
    return ESP_OK;
}

/* ============================================================================
 * Send API
 * ========================================================================== */

esp_err_t microlink_send(microlink_t *ml, uint32_t dest_vpn_ip,
                          const uint8_t *data, size_t len) {
    if (!ml || !data || len == 0 || len > 1400) return ESP_ERR_INVALID_ARG;
    if (ml->state != ML_STATE_CONNECTED) return ESP_ERR_INVALID_STATE;

    /* Find peer by VPN IP */
    for (int i = 0; i < ml->peer_count; i++) {
        if (ml->peers[i].vpn_ip == dest_vpn_ip && ml->peers[i].active) {
            /* TODO: Route through WireGuard tunnel */
            /* For now, queue via DERP as fallback */
            return ml_derp_queue_send(ml, ml->peers[i].public_key, data, len);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* ============================================================================
 * Callbacks
 * ========================================================================== */

void microlink_set_state_callback(microlink_t *ml, microlink_state_cb_t cb, void *user_data) {
    if (ml) { ml->state_cb = cb; ml->state_cb_data = user_data; }
}

void microlink_set_peer_callback(microlink_t *ml, microlink_peer_cb_t cb, void *user_data) {
    if (ml) { ml->peer_cb = cb; ml->peer_cb_data = user_data; }
}

void microlink_set_data_callback(microlink_t *ml, microlink_data_cb_t cb, void *user_data) {
    if (ml) { ml->data_cb = cb; ml->data_cb_data = user_data; }
}

/* ============================================================================
 * Utilities
 * ========================================================================== */

void microlink_ip_to_str(uint32_t ip, char *buf) {
    snprintf(buf, 16, "%lu.%lu.%lu.%lu",
             (unsigned long)((ip >> 24) & 0xFF),
             (unsigned long)((ip >> 16) & 0xFF),
             (unsigned long)((ip >> 8) & 0xFF),
             (unsigned long)(ip & 0xFF));
}

uint32_t microlink_parse_ip(const char *ip_str) {
    if (!ip_str) return 0;
    unsigned int a, b, c, d;
    if (sscanf(ip_str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

const char *microlink_default_device_name(void) {
    static char name[48] = {0};
    if (name[0] == 0) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);

        snprintf(name, sizeof(name), "esp32-%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
    return name;
}

uint64_t ml_get_time_ms(void) {
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

/* ============================================================================
 * MagicDNS — Resolve tailnet hostnames against peer list
 * ========================================================================== */

/* Case-insensitive string compare (limited to len bytes) */
static int strncasecmp_local(const char *a, const char *b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

uint32_t microlink_resolve(const microlink_t *ml, const char *hostname) {
    if (!ml || !hostname || hostname[0] == '\0') return 0;

    size_t query_len = strlen(hostname);

    for (int i = 0; i < ml->peer_count; i++) {
        const ml_peer_t *p = &ml->peers[i];
        if (!p->active || p->hostname[0] == '\0') continue;

        /* 1. Exact match (case-insensitive) */
        if (strncasecmp_local(p->hostname, hostname, sizeof(p->hostname)) == 0) {
            return p->vpn_ip;
        }

        /* 2. Prefix match: query "npc1" matches peer "npc1.tail12345.ts.net"
         * The query must match up to the first '.' in the peer hostname. */
        const char *dot = strchr(p->hostname, '.');
        if (dot) {
            size_t short_len = (size_t)(dot - p->hostname);
            if (query_len == short_len &&
                strncasecmp_local(p->hostname, hostname, short_len) == 0) {
                return p->vpn_ip;
            }
        }
    }

    return 0;  /* Not found */
}
