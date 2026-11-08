// "Tailnet only, no password" access mode.
//
// When on, the web server accepts connections only over Tailscale and doesn't ask for
// the password there: being on the tailnet is the login. Two layers enforce it:
//  1. open_fn below: a new connection whose local address isn't our tailnet IP, or
//     whose peer isn't in 100.64.0.0/10, is closed before a byte is read
//  2. the lwIP input hook in the tailscale component drops packets that arrive on
//     Wi-Fi addressed to the tailnet IP (lwIP would otherwise accept them: weak host)
// Recovery when the tailnet is unreachable (expired key, account change):
//  - hold BOOT for 10 s: LAN access with the password re-opens for 15 minutes
//  - flash.sh --prov-only --lan-access: writes a new nonce into prov; a nonce we
//    haven't seen turns the mode off at boot
#include <string.h>
#include "access.h"
#include "tailscale.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web.access";

#define NVS_NS          "web"
#define BOOT_GPIO       GPIO_NUM_28     // ESP32-C5 download-mode strap = the BOOT button
#define HOLD_MS         10000
#define LAN_WINDOW_US   (15LL * 60 * 1000000)

static volatile bool s_tsonly;          // the setting
static volatile int64_t s_lan_until;    // esp_timer µs; LAN re-opened until then
static void (*s_lan_cb)(bool lan_open);
static bool s_lan_open_last = true;

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "tsonly", s_tsonly);
    nvs_commit(h);
    nvs_close(h);
}

bool access_lan_open(void)
{
    return !s_tsonly || esp_timer_get_time() < s_lan_until;
}

bool access_tailnet_only_setting(void)
{
    return s_tsonly;
}

int access_lan_window_s(void)
{
    int64_t left = s_lan_until - esp_timer_get_time();
    return left > 0 ? (int)(left / 1000000) : 0;
}

// Tell main (mDNS) when the LAN side opens or closes.
static void notify(void)
{
    bool open = access_lan_open();
    if (open != s_lan_open_last) {
        s_lan_open_last = open;
        if (s_lan_cb) s_lan_cb(open);
    }
}

bool access_fd_via_tailnet(int fd)
{
    uint32_t ts = ts_tailnet_ip();
    if (!ts) return false;
    struct sockaddr_in local = {0}, peer = {0};
    socklen_t ll = sizeof(local), pl = sizeof(peer);
    if (getsockname(fd, (struct sockaddr *)&local, &ll) != 0) return false;
    if (getpeername(fd, (struct sockaddr *)&peer, &pl) != 0) return false;
    if (local.sin_family != AF_INET || peer.sin_family != AF_INET) return false;
    uint32_t l = ntohl(local.sin_addr.s_addr), p = ntohl(peer.sin_addr.s_addr);
    return l == ts && (p & 0xFFC00000u) == 0x64400000u;  // peer in 100.64.0.0/10
}

bool access_passwordless(httpd_req_t *req)
{
    return s_tsonly && access_fd_via_tailnet(httpd_req_to_sockfd(req));
}

esp_err_t access_open_fn(httpd_handle_t hd, int fd)
{
    if (access_lan_open() || access_fd_via_tailnet(fd)) return ESP_OK;
    return ESP_FAIL;  // httpd closes it before reading anything
}

esp_err_t access_set_tailnet_only(bool on)
{
    s_tsonly = on;
    if (!on) s_lan_until = 0;
    save();
    ESP_LOGW(TAG, "tailnet-only access %s", on ? "ON: LAN access closed" : "off");
    notify();
    return ESP_OK;
}

static void button_task(void *arg)
{
    int64_t down_since = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        int64_t now = esp_timer_get_time();
        if (gpio_get_level(BOOT_GPIO) == 0) {
            if (!down_since) down_since = now;
            if (now - down_since >= (int64_t)HOLD_MS * 1000 && down_since != -1) {
                down_since = -1;  // once per press
                if (s_tsonly) {
                    s_lan_until = now + LAN_WINDOW_US;
                    ESP_LOGW(TAG, "BOOT held 10 s: LAN access with password open for 15 min");
                } else {
                    ESP_LOGI(TAG, "BOOT held 10 s: LAN access is already open");
                }
            }
        } else {
            down_since = 0;
        }
        notify();  // also closes the LAN again when the window ends
    }
}

void access_init(const char *prov_lan_reset, void (*lan_cb)(bool lan_open))
{
    s_lan_cb = lan_cb;
    nvs_handle_t h;
    char seen[33] = "";
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "tsonly", &v) == ESP_OK) s_tsonly = v != 0;
        size_t len = sizeof(seen);
        if (nvs_get_str(h, "lanreset", seen, &len) != ESP_OK) seen[0] = '\0';
        nvs_close(h);
    }
    // flash.sh --prov-only --lan-access writes a fresh nonce: apply it once.
    if (prov_lan_reset && prov_lan_reset[0] && strcmp(prov_lan_reset, seen) != 0) {
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "lanreset", prov_lan_reset);
            nvs_set_u8(h, "tsonly", 0);
            nvs_commit(h);
            nvs_close(h);
        }
        if (s_tsonly) ESP_LOGW(TAG, "flash.sh --lan-access: tailnet-only access turned off");
        s_tsonly = false;
    }
    s_lan_open_last = access_lan_open();

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&io) == ESP_OK) {
        ESP_LOGI(TAG, "access: %s; BOOT (GPIO%d) reads %d (1 = released)",
                 s_tsonly ? "tailnet only" : "LAN + tailnet with password",
                 BOOT_GPIO, gpio_get_level(BOOT_GPIO));
        // PSRAM stack: never writes flash (the setting is saved by the web server task)
        xTaskCreateWithCaps(button_task, "bootbtn", 2560, NULL, 2, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    } else {
        ESP_LOGE(TAG, "BOOT button not available: the 10 s recovery won't work");
    }
}
