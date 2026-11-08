#include "motion.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "ping/ping_sock.h"
#include "wifi5g.h"

static const char *TAG = "motion";

#define INTERVAL_MS      500
#define HOLD_MS          2000     // "moving" stays on this long after the last high score
#define PING_INTERVAL_MS 50
#define MAX_SC           256      // subcarriers kept per frame
#define MIN_SC           8        // fewer non-zero subcarriers: frame ignored
#define CSI_QUEUE_LEN    16
#define LEARN_STILL_N    (30000 / INTERVAL_MS)
#define LEARN_MOVING_N   (15000 / INTERVAL_MS)
#define THR_DEFAULT      800
#define THR_MIN          200
#define THR_MAX          5000
#define UNKNOWN          0xFFFF

typedef struct {
    bool first_invalid;
    uint16_t len;
    int8_t buf[MAX_SC * 2];
} csi_item_t;

// Per-interval statistics and scratch; PSRAM (too big for internal RAM).
typedef struct {
    float amp[MAX_SC];
    float mean[MAX_SC];
    float m2[MAX_SC];
    uint16_t nsc;
    uint32_t n;
    uint16_t learn[LEARN_STILL_N];
} work_t;

static QueueHandle_t s_csi_q;
static csi_item_t *s_cb_item;     // scratch for the callback (Wi-Fi task only)
static csi_item_t *s_item;
static work_t *s_w;

static uint8_t s_own_mac[6];
static volatile bool s_mac_set;
static volatile uint32_t s_fmt_count[8];  // frames addressed to the hub per format, last 10 s
static volatile uint8_t s_use_fmt = 0xFF; // format that goes into the score; 0xFF = any
static bool s_csi_active;
static uint32_t s_connects_seen;
static esp_ping_handle_t s_ping;
static uint32_t s_frames_sec;

static uint16_t s_still = UNKNOWN, s_move = UNKNOWN;  // learned levels
static uint16_t s_thr = THR_DEFAULT;
static volatile uint8_t s_learn_req;      // set by the UI: motion_learn_t
static uint8_t s_learning;
static int s_n_learn;
static int64_t s_last_high_us;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static motion_status_t s_status = {.score = -1, .threshold = THR_DEFAULT};

// ---- CSI ----------------------------------------------------------------------------------
// Wi-Fi task: copy and return. Only frames addressed to the hub (the ping replies), of one
// format: the router sends beacons differently (legacy rate, not beamformed), and two
// channel shapes in one score would read as movement.
static void csi_cb(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf || info->len < 2 * MIN_SC || !s_mac_set) return;
    if (memcmp(info->dmac, s_own_mac, 6) != 0) return;
    uint8_t fmt = info->rx_ctrl.cur_bb_format;
    s_fmt_count[fmt & 7]++;
    if (s_use_fmt != 0xFF && fmt != s_use_fmt) return;
    csi_item_t *it = s_cb_item;
    it->first_invalid = info->first_word_invalid;
    it->len = info->len > sizeof(it->buf) ? sizeof(it->buf) : info->len;
    memcpy(it->buf, info->buf, it->len);
    xQueueSend(s_csi_q, it, 0);  // full: dropped
}

static void reset_interval(work_t *w)
{
    memset(w->mean, 0, sizeof(w->mean));
    memset(w->m2, 0, sizeof(w->m2));
    w->n = 0;
}

// Amplitude per subcarrier, normalised by the frame's mean (removes gain changes), folded
// into a running mean / variance per subcarrier (Welford).
static void process_frame(const csi_item_t *it)
{
    work_t *w = s_w;
    uint16_t nsc = it->len / 2;
    if (nsc > MAX_SC) nsc = MAX_SC;
    uint16_t start = it->first_invalid ? 2 : 0;  // the first 4 bytes are unusable
    if (nsc != w->nsc) {
        w->nsc = nsc;
        reset_interval(w);
    }
    float sum = 0;
    int nz = 0;
    for (uint16_t k = 0; k < nsc; k++) {
        float a = 0;
        if (k >= start) {
            float im = it->buf[2 * k], re = it->buf[2 * k + 1];
            a = sqrtf(im * im + re * re);
        }
        w->amp[k] = a;
        if (a > 0) { sum += a; nz++; }
    }
    if (nz < MIN_SC) return;
    float inv = nz / sum;
    w->n++;
    s_frames_sec++;
    for (uint16_t k = 0; k < nsc; k++) {
        float x = w->amp[k] * inv;
        float d = x - w->mean[k];
        w->mean[k] += d / w->n;
        w->m2[k] += d * (x - w->mean[k]);
    }
}

// Score of the interval: mean per-subcarrier standard deviation, x10000. UNKNOWN when fewer
// than 3 frames arrived.
static uint16_t interval_score(void)
{
    work_t *w = s_w;
    uint16_t score = UNKNOWN;
    if (w->n >= 3) {
        double acc = 0;
        int used = 0;
        for (uint16_t k = 0; k < w->nsc; k++) {
            if (w->mean[k] < 0.05f) continue;  // null and guard subcarriers
            acc += sqrt(w->m2[k] / (w->n - 1));
            used++;
        }
        if (used) {
            double v = acc / used * 10000;
            score = v > 65534 ? 65534 : (uint16_t)(v + 0.5);
        }
    }
    reset_interval(w);
    return score;
}

// ---- calibration --------------------------------------------------------------------------

static int cmp_u16(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

static void update_thr(void)
{
    uint32_t t = THR_DEFAULT;
    if (s_still != UNKNOWN && s_move != UNKNOWN && s_move > s_still) {
        t = (uint32_t)sqrt((double)s_still * s_move);  // between the two, on a ratio scale
    } else if (s_still != UNKNOWN) {
        t = s_still * 5u / 2;
    }
    s_thr = t < THR_MIN ? THR_MIN : t > THR_MAX ? THR_MAX : t;
}

static void save_levels(void)
{
    nvs_handle_t h;
    if (nvs_open("motion", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "still", s_still);
    nvs_set_u16(h, "move", s_move);
    nvs_commit(h);
    nvs_close(h);
}

static void learn_step(uint16_t score)
{
    uint8_t req = s_learn_req;
    if (req) {
        s_learn_req = 0;
        s_learning = req;
        s_n_learn = 0;
    }
    if (!s_learning || score == UNKNOWN) return;
    int need = s_learning == MOTION_LEARN_STILL ? LEARN_STILL_N : LEARN_MOVING_N;
    s_w->learn[s_n_learn++] = score;
    if (s_n_learn < need) return;
    qsort(s_w->learn, need, sizeof(uint16_t), cmp_u16);
    if (s_learning == MOTION_LEARN_STILL) {
        s_still = s_w->learn[need * 99 / 100];  // the loudest a still room gets
    } else {
        s_move = s_w->learn[need / 4];          // the quieter end of real movement
    }
    update_thr();
    save_levels();
    ESP_LOGI(TAG, "learned %s level: still %u, moving %u -> threshold %u",
             s_learning == MOTION_LEARN_STILL ? "still" : "moving", s_still, s_move, s_thr);
    s_learning = MOTION_LEARN_NONE;
}

void motion_learn(bool moving)
{
    s_learn_req = moving ? MOTION_LEARN_MOVING : MOTION_LEARN_STILL;
}

// ---- traffic and driver setup ------------------------------------------------------------

static void ping_stop(void)
{
    if (!s_ping) return;
    esp_ping_stop(s_ping);
    esp_ping_delete_session(s_ping);
    s_ping = NULL;
}

static void ping_start(void)
{
    ping_stop();
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (!n || esp_netif_get_ip_info(n, &ip) != ESP_OK || ip.gw.addr == 0) return;
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = PING_INTERVAL_MS;
    cfg.timeout_ms = 1000;
    cfg.data_size = 8;
    cfg.task_stack_size = 2560;
    cfg.task_prio = 2;
    ip_addr_set_ip4_u32(&cfg.target_addr, ip.gw.addr);
    esp_ping_callbacks_t cbs = {0};
    if (esp_ping_new_session(&cfg, &cbs, &s_ping) != ESP_OK) {
        s_ping = NULL;
        ESP_LOGW(TAG, "ping session not created");
        return;
    }
    esp_ping_start(s_ping);
}

static void csi_start(void)
{
    s_mac_set = false;
    esp_wifi_get_mac(WIFI_IF_STA, s_own_mac);
    s_mac_set = true;
    // The ping replies come as HE SU frames: without the HE/HT options the driver gives CSI
    // for legacy frames (beacons) only. force_lltf makes every format yield the same L-LTF
    // layout (53 subcarriers here).
    wifi_csi_config_t c = {0};
    c.enable = 1;
    c.acquire_csi_legacy = 1;
    c.acquire_csi_ht20 = 1;
    c.acquire_csi_ht40 = 1;
    c.acquire_csi_vht = 1;
    c.acquire_csi_su = 1;
    c.acquire_csi_mu = 1;
    c.acquire_csi_dcm = 1;
    c.acquire_csi_beamformed = 1;
    c.acquire_csi_he_stbc_mode = 2;
    c.acquire_csi_force_lltf = 1;
    c.lltf_bit_mode = 1;  // 8-bit I/Q
    esp_err_t err = esp_wifi_set_csi_config(&c);
    if (err == ESP_OK) err = esp_wifi_set_csi_rx_cb(csi_cb, NULL);
    if (err == ESP_OK) err = esp_wifi_set_csi(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CSI not enabled: %s", esp_err_to_name(err));
        return;
    }
    s_csi_active = true;
    ping_start();
    ESP_LOGI(TAG, "sensing started");
}

static void csi_stop(void)
{
    ping_stop();
    if (s_csi_active) esp_wifi_set_csi(false);  // fails harmlessly if the driver is down
    s_csi_active = false;
}

// Once a second: follow Wi-Fi up/down, and every 10 s pick the frame format the router sends
// the hub most (the ping replies).
static void housekeeping(void)
{
    bool up = wifi5g_is_connected();
    if (up) {
        wifi5g_stats_t w;
        wifi5g_get_stats(&w);
        if (!s_csi_active || w.connects != s_connects_seen) {
            csi_stop();
            s_connects_seen = w.connects;
            csi_start();
        }
    } else if (s_csi_active || s_ping) {
        csi_stop();
    }

    static int ticks;
    if (++ticks >= 10) {
        ticks = 0;
        uint8_t best = 0xFF;
        uint32_t most = 0;
        for (int i = 0; i < 8; i++) {
            if (s_fmt_count[i] > most) { most = s_fmt_count[i]; best = i; }
            s_fmt_count[i] = 0;
        }
        if (best != 0xFF) s_use_fmt = best;
    }

    taskENTER_CRITICAL(&s_lock);
    s_status.fps = s_frames_sec;
    s_status.sensing = up && s_frames_sec > 0;
    taskEXIT_CRITICAL(&s_lock);
    s_frames_sec = 0;
}

static void motion_task(void *arg)
{
    int64_t next_interval = esp_timer_get_time() + INTERVAL_MS * 1000LL;
    int64_t next_sec = esp_timer_get_time() + 1000000;
    for (;;) {
        int64_t now = esp_timer_get_time();
        int64_t wait_us = next_interval - now;
        TickType_t wait = wait_us > 0 ? pdMS_TO_TICKS(wait_us / 1000) : 0;
        if (xQueueReceive(s_csi_q, s_item, wait) == pdTRUE) process_frame(s_item);

        now = esp_timer_get_time();
        if (now >= next_interval) {
            next_interval += INTERVAL_MS * 1000LL;
            if (next_interval < now) next_interval = now + INTERVAL_MS * 1000LL;
            uint16_t score = interval_score();
            learn_step(score);
            if (score != UNKNOWN && score > s_thr) s_last_high_us = now;
            taskENTER_CRITICAL(&s_lock);
            s_status.score = score == UNKNOWN ? -1 : score;
            s_status.moving = s_last_high_us && now - s_last_high_us < HOLD_MS * 1000LL;
            s_status.learning = s_learning;
            s_status.threshold = s_thr;
            taskEXIT_CRITICAL(&s_lock);
        }
        if (now >= next_sec) {
            next_sec = now + 1000000;
            housekeeping();
        }
    }
}

void motion_get_status(motion_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
}

esp_err_t motion_init(void)
{
    s_csi_q = xQueueCreateWithCaps(CSI_QUEUE_LEN, sizeof(csi_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_cb_item = heap_caps_calloc(1, sizeof(csi_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_item = heap_caps_calloc(1, sizeof(csi_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_w = heap_caps_calloc(1, sizeof(work_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_csi_q || !s_cb_item || !s_item || !s_w) return ESP_ERR_NO_MEM;

    nvs_handle_t h;
    if (nvs_open("motion", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, "still", &s_still);
        nvs_get_u16(h, "move", &s_move);
        nvs_close(h);
    }
    update_thr();
    // Internal stack: it writes the learned levels to NVS (flash).
    if (xTaskCreate(motion_task, "motion", 4096, NULL, 2, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "ready, threshold %u", s_thr);
    return ESP_OK;
}
