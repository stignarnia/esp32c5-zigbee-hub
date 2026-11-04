#include "prov.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define PROV_PARTITION "prov"
#define PROV_NAMESPACE "prov"

static const char *TAG = "prov";

static void read_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err != ESP_OK) {
        dst[0] = '\0';
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "key '%s': %s", key, esp_err_to_name(err));
        }
    }
}

esp_err_t prov_load(prov_t *out)
{
    memset(out, 0, sizeof(*out));

    esp_err_t err = nvs_flash_init_partition(PROV_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "partition '%s' init failed: %s", PROV_PARTITION, esp_err_to_name(err));
        return err;
    }

    nvs_handle_t h;
    err = nvs_open_from_partition(PROV_PARTITION, PROV_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "not provisioned (run ./flash.sh)");
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        return err;
    }

    read_str(h, "ssid", out->ssid, sizeof(out->ssid));
    read_str(h, "psk", out->psk, sizeof(out->psk));
    read_str(h, "ssid2", out->ssid2, sizeof(out->ssid2));
    read_str(h, "psk2", out->psk2, sizeof(out->psk2));
    read_str(h, "hostname", out->hostname, sizeof(out->hostname));
    read_str(h, "country", out->country, sizeof(out->country));
    read_str(h, "admin_hash", out->admin_hash, sizeof(out->admin_hash));
    read_str(h, "ts_authkey", out->ts_authkey, sizeof(out->ts_authkey));
    nvs_close(h);

    out->has_wifi = out->ssid[0] != '\0';
    if (out->hostname[0] == '\0') {
        strcpy(out->hostname, "zigbee-hub");
    }

    ESP_LOGI(TAG, "ssid='%s' psk=%s backup='%s' hostname='%s' country='%s' admin=%s tailscale_key=%s",
             out->ssid, out->psk[0] ? "set" : "empty", out->ssid2, out->hostname,
             out->country[0] ? out->country : "default",
             out->admin_hash[0] ? "set" : "missing", out->ts_authkey[0] ? "set" : "none");
    return ESP_OK;
}
