// System services shared by the web UI and later phases: planned reboots and
// over-the-air firmware updates with rollback bookkeeping.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// ---- planned reboot -------------------------------------------------------------
// Reboots after `delay_ms` (lets an HTTP response flush), disconnecting Wi-Fi
// cleanly first. `reason` is logged and shown as the last reboot reason.
void hubsys_reboot(const char *reason, uint32_t delay_ms);

// Reason the previous boot ended, if it was a planned reboot ("" otherwise).
const char *hubsys_last_planned_reason(void);

// ---- OTA ----------------------------------------------------------------------------
// Call once early in app_main: works out what happened to the last update.
void hubsys_ota_boot_check(void);

// True while a freshly updated image runs on trial (bootloader rollback armed).
bool hubsys_ota_pending_verify(void);

// Marks the running image good. Called once the firmware has proven healthy.
void hubsys_ota_confirm(void);

// Streaming upload into the inactive slot. begin() checks the size; the first
// write() must contain the image header + app description (>= 288 bytes) and
// rejects images for another project or chip.
esp_err_t hubsys_ota_begin(size_t image_size);
esp_err_t hubsys_ota_write(const void *data, size_t len);
esp_err_t hubsys_ota_finish(void);  // validates, sets boot partition, records it
void hubsys_ota_abort(void);
bool hubsys_ota_in_progress(void);  // an upload is being written

typedef struct {
    const char *kind;      // "none" | "trial" | "updated" | "rolled_back"
    char from_id[17];      // build ids (first 8 bytes of the ELF SHA-256, hex)
    char to_id[17];
    char to_version[33];
} hubsys_ota_result_t;

void hubsys_ota_last_result(hubsys_ota_result_t *out);

// Build id of the running image (16 hex chars).
const char *hubsys_build_id(void);

// Logs the previous boot's crash (task, PC, RA, trap cause) from the core dump, if
// the last reset was a panic or watchdog. Call once at boot, after hublog_init().
void hubsys_crash_report(void);

// Flash address and size of a valid core dump; ESP_ERR_NOT_FOUND if there is none.
esp_err_t hubsys_coredump_get(size_t *addr, size_t *size);
