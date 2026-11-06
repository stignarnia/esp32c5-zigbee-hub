#include "hubsys.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_core_dump.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "wifi5g.h"

static const char *TAG = "hubsys";

#define RTC_MAGIC 0x4B0B5EEDu
RTC_NOINIT_ATTR static uint32_t s_rtc_magic;
RTC_NOINIT_ATTR static char s_rtc_reason[48];

static char s_last_reason[48];
static char s_build_id[17];

// ---- planned reboot -----------------------------------------------------------------

static void reboot_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    wifi5g_prepare_reboot();
    esp_restart();
}

void hubsys_reboot(const char *reason, uint32_t delay_ms)
{
    ESP_LOGW(TAG, "reboot in %" PRIu32 " ms: %s", delay_ms, reason);
    strlcpy(s_rtc_reason, reason, sizeof(s_rtc_reason));
    s_rtc_magic = RTC_MAGIC;
    xTaskCreate(reboot_task, "reboot", 3072, (void *)(uintptr_t)delay_ms, 10, NULL);
}

const char *hubsys_last_planned_reason(void)
{
    return s_last_reason;
}

static void planned_reason_init(void)
{
    if (s_rtc_magic == RTC_MAGIC && esp_reset_reason() == ESP_RST_SW) {
        s_rtc_reason[sizeof(s_rtc_reason) - 1] = '\0';
        strlcpy(s_last_reason, s_rtc_reason, sizeof(s_last_reason));
        ESP_LOGI(TAG, "previous boot ended by a planned reboot: %s", s_last_reason);
    }
    s_rtc_magic = 0;
}

// ---- build id -------------------------------------------------------------------

static void hex8(const uint8_t *sha, char out[17])
{
    for (int i = 0; i < 8; i++) sprintf(out + 2 * i, "%02x", sha[i]);
    out[16] = '\0';
}

const char *hubsys_build_id(void)
{
    if (!s_build_id[0]) hex8(esp_app_get_description()->app_elf_sha256, s_build_id);
    return s_build_id;
}

// ---- OTA --------------------------------------------------------------------------
// NVS namespace "ota": pend_id / pend_ver = what we just installed, from_id = what
// was running. Checked (and erased) on the next boot.

static hubsys_ota_result_t s_result = {.kind = "none"};
static bool s_pending_verify;
static esp_ota_handle_t s_ota;
static const esp_partition_t *s_ota_part;
static size_t s_ota_size, s_ota_written;
static bool s_ota_header_ok;
static esp_app_desc_t s_ota_desc;

void hubsys_ota_boot_check(void)
{
    planned_reason_init();

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify = true;
        ESP_LOGW(TAG, "running a new image on trial (rollback armed until confirmed)");
    }

    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    char pend[17] = "", from[17] = "", ver[33] = "";
    size_t l1 = sizeof(pend), l2 = sizeof(from), l3 = sizeof(ver);
    bool have = nvs_get_str(h, "pend_id", pend, &l1) == ESP_OK;
    nvs_get_str(h, "from_id", from, &l2);
    nvs_get_str(h, "pend_ver", ver, &l3);

    if (have && s_pending_verify && strcmp(pend, hubsys_build_id()) == 0) {
        // The new image on its first boot: not "updated" until it confirms. If it
        // crashes first, the bootloader rolls back and that boot finds pend_id
        // still here, pointing at a build that isn't running -> rolled_back.
        strlcpy(s_result.from_id, from, sizeof(s_result.from_id));
        strlcpy(s_result.to_id, pend, sizeof(s_result.to_id));
        strlcpy(s_result.to_version, ver, sizeof(s_result.to_version));
        s_result.kind = "trial";
        ESP_LOGI(TAG, "update installed: %s -> %s (%s), on trial", from, pend, ver);
    } else if (have) {
        strlcpy(s_result.from_id, from, sizeof(s_result.from_id));
        strlcpy(s_result.to_id, pend, sizeof(s_result.to_id));
        strlcpy(s_result.to_version, ver, sizeof(s_result.to_version));
        if (strcmp(pend, hubsys_build_id()) == 0) {
            s_result.kind = "updated";
            ESP_LOGI(TAG, "update installed: %s -> %s (%s)", from, pend, ver);
        } else {
            s_result.kind = "rolled_back";
            ESP_LOGE(TAG, "update %s ROLLED BACK, running %s again", pend, hubsys_build_id());
        }
        // Keep the result visible for this boot; forget it on the next one.
        nvs_erase_key(h, "pend_id");
        nvs_set_str(h, "last_kind", s_result.kind);
        nvs_set_str(h, "last_from", from);
        nvs_set_str(h, "last_to", pend);
        nvs_set_str(h, "last_ver", ver);
        nvs_commit(h);
    } else {
        char kind[16] = "";
        size_t lk = sizeof(kind);
        if (nvs_get_str(h, "last_kind", kind, &lk) == ESP_OK) {
            // An earlier boot's result: still worth showing.
            s_result.kind = strcmp(kind, "updated") == 0 ? "updated" : "rolled_back";
            l1 = sizeof(s_result.from_id);
            nvs_get_str(h, "last_from", s_result.from_id, &l1);
            l2 = sizeof(s_result.to_id);
            nvs_get_str(h, "last_to", s_result.to_id, &l2);
            l3 = sizeof(s_result.to_version);
            nvs_get_str(h, "last_ver", s_result.to_version, &l3);
        }
    }
    nvs_close(h);
}

bool hubsys_ota_pending_verify(void)
{
    return s_pending_verify;
}

void hubsys_ota_confirm(void)
{
    if (!s_pending_verify) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_pending_verify = false;
        ESP_LOGI(TAG, "new image confirmed healthy; rollback disarmed");
        if (strcmp(s_result.kind, "trial") == 0) {
            s_result.kind = "updated";
            nvs_handle_t h;
            if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_key(h, "pend_id");
                nvs_set_str(h, "last_kind", s_result.kind);
                nvs_set_str(h, "last_from", s_result.from_id);
                nvs_set_str(h, "last_to", s_result.to_id);
                nvs_set_str(h, "last_ver", s_result.to_version);
                nvs_commit(h);
                nvs_close(h);
            }
        }
    }
}

esp_err_t hubsys_ota_begin(size_t image_size)
{
    if (s_ota_part) return ESP_ERR_INVALID_STATE;
    if (s_pending_verify) {
        ESP_LOGW(TAG, "refusing an update while the current image is still on trial");
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return ESP_ERR_NOT_FOUND;
    if (image_size < 1024 || image_size > part->size) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = esp_ota_begin(part, image_size, &s_ota);
    if (err != ESP_OK) return err;
    s_ota_part = part;
    s_ota_size = image_size;
    s_ota_written = 0;
    s_ota_header_ok = false;
    ESP_LOGI(TAG, "ota: receiving %u bytes into %s", (unsigned)image_size, part->label);
    return ESP_OK;
}

#define DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))

esp_err_t hubsys_ota_write(const void *data, size_t len)
{
    if (!s_ota_part) return ESP_ERR_INVALID_STATE;
    if (s_ota_written + len > s_ota_size) return ESP_ERR_INVALID_SIZE;

    if (!s_ota_header_ok) {
        // Check before writing anything: right chip, right project, valid descriptor.
        if (s_ota_written != 0 || len < DESC_OFFSET + sizeof(esp_app_desc_t)) {
            return ESP_ERR_INVALID_ARG;
        }
        const esp_image_header_t *hdr = data;
        memcpy(&s_ota_desc, (const uint8_t *)data + DESC_OFFSET, sizeof(s_ota_desc));
        if (hdr->magic != ESP_IMAGE_HEADER_MAGIC || hdr->chip_id != ESP_CHIP_ID_ESP32C5) {
            ESP_LOGE(TAG, "ota: not an ESP32-C5 image");
            return ESP_ERR_INVALID_ARG;
        }
        if (s_ota_desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
            strncmp(s_ota_desc.project_name, esp_app_get_description()->project_name,
                    sizeof(s_ota_desc.project_name)) != 0) {
            ESP_LOGE(TAG, "ota: image is for project '%.32s'", s_ota_desc.project_name);
            return ESP_ERR_INVALID_ARG;
        }
        s_ota_header_ok = true;
    }
    esp_err_t err = esp_ota_write(s_ota, data, len);
    if (err == ESP_OK) s_ota_written += len;
    return err;
}

esp_err_t hubsys_ota_finish(void)
{
    if (!s_ota_part) return ESP_ERR_INVALID_STATE;
    if (s_ota_written != s_ota_size) {
        hubsys_ota_abort();
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = esp_ota_end(s_ota);  // full image validation (segments, checksum, hash)
    const esp_partition_t *part = s_ota_part;
    s_ota_part = NULL;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota: image invalid: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) return err;

    char to[17];
    hex8(s_ota_desc.app_elf_sha256, to);
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "pend_id", to);
        nvs_set_str(h, "from_id", hubsys_build_id());
        nvs_set_str(h, "pend_ver", s_ota_desc.version);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "ota: %s (%s) written to %s, boots on trial next", to, s_ota_desc.version,
             part->label);
    return ESP_OK;
}

bool hubsys_ota_in_progress(void)
{
    return s_ota_part != NULL;
}

void hubsys_ota_abort(void)
{
    if (s_ota_part) {
        esp_ota_abort(s_ota);
        s_ota_part = NULL;
        ESP_LOGW(TAG, "ota: aborted after %u bytes", (unsigned)s_ota_written);
    }
}

void hubsys_ota_last_result(hubsys_ota_result_t *out)
{
    *out = s_result;
}

// ---- crash report -------------------------------------------------------------------
// A panic writes a core dump to the `coredump` partition. On the next boot, one log
// line with the crashing task, PC, RA and trap cause goes into the on-board log;
// decode with `riscv32-esp-elf-addr2line -pfiaC -e build/<elf> <pc> <ra>` (same
// build id). The full dump is downloadable from /api/coredump for esp-coredump.

void hubsys_crash_report(void)
{
    if (esp_reset_reason() != ESP_RST_PANIC && esp_reset_reason() != ESP_RST_TASK_WDT &&
        esp_reset_reason() != ESP_RST_INT_WDT) {
        return;
    }
    if (esp_core_dump_image_check() != ESP_OK) {
        ESP_LOGE(TAG, "crash on the previous boot, but no valid core dump");
        return;
    }
    esp_core_dump_summary_t *sum = malloc(sizeof(*sum));
    if (!sum) return;
    if (esp_core_dump_get_summary(sum) == ESP_OK) {
        char build[17];
        hex8(sum->app_elf_sha256, build);
        ESP_LOGE(TAG, "CRASH on the previous boot: task '%s' pc 0x%08" PRIx32 " ra 0x%08" PRIx32
                 " mcause 0x%" PRIx32 " mtval 0x%08" PRIx32 " build %s (dump at /api/coredump)",
                 sum->exc_task, sum->exc_pc, sum->ex_info.ra, sum->ex_info.mcause, sum->ex_info.mtval,
                 build);
    }
    free(sum);
}

esp_err_t hubsys_coredump_get(size_t *addr, size_t *size)
{
    if (esp_core_dump_image_check() != ESP_OK) return ESP_ERR_NOT_FOUND;
    return esp_core_dump_image_get(addr, size);
}
