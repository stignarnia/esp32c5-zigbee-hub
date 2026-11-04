// On-board diagnostic log: warnings, errors and selected info lines (Wi-Fi,
// OTA, reboots, 5-minute samples) appended to LittleFS on the `storage`
// partition, two rotating files of HUBLOG_FILE_MAX bytes. Downloadable from the
// web UI, so long tests need no serial cable.
//
// Lines are captured by an esp_log hook into a PSRAM ring buffer (the hook can run
// inside the Wi-Fi driver, so it never touches flash) and written by a background
// task every 10 s, and on every esp_restart() via a shutdown handler.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define HUBLOG_FILE_MAX (256 * 1024)
#define HUBLOG_DIR "/storage/logs"
#define HUBLOG_CUR HUBLOG_DIR "/events.log"
#define HUBLOG_OLD HUBLOG_DIR "/events.1.log"

// Mounts /storage and installs the log hook. Call as early as possible in app_main.
esp_err_t hublog_init(void);

// Writes buffered lines to flash now.
void hublog_flush(void);

// Starts SNTP (UTC timestamps in the log once synced). Call after networking is up.
void hublog_start_time_sync(void);

bool hublog_time_valid(void);
