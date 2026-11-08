// Wi-Fi motion sensing: "moving" / "not moving" from how much the router's signal changes.
//
// The hub pings its gateway 20 times a second so the router keeps sending it frames, reads
// the channel state information (CSI) of each one, and every 500 ms scores how much the
// signal changed. People moving anywhere along the router-hub path (ends included) raise it.
// Calibration is two buttons in the UI: "Not moving" (stay still 30 s) and "Moving" (move
// for 15 s); the threshold sits between the two levels.
//
// Never in Zigbee's way: the CSI callback only copies into a queue (dropped when full), the
// work runs on a priority-2 task.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    MOTION_LEARN_NONE = 0,
    MOTION_LEARN_STILL,      // collecting the "not moving" level
    MOTION_LEARN_MOVING,     // collecting the "moving" level
} motion_learn_t;

typedef struct {
    bool sensing;            // Wi-Fi up and frames from the router arriving
    bool moving;             // score above the threshold within the last 2 s
    uint8_t learning;        // motion_learn_t
    int32_t score;           // last 500 ms, -1 = not enough frames
    uint16_t threshold;
    uint16_t fps;            // frames scored in the last second
} motion_status_t;

// Starts the task; sensing begins once Wi-Fi has an IP. Call after the NVS init.
esp_err_t motion_init(void);

void motion_get_status(motion_status_t *out);

// Calibration: learn the "moving" (true) or "not moving" (false) level from now on.
void motion_learn(bool moving);

// Moving right now: 1 / 0, -1 when not sensing.
int motion_moving_now(void);

// Was there movement between from_s and to_s seconds ago (0 <= to_s < from_s <= 600)?
// 1 = yes (in any half second of the window); 0 = still the whole time; -1 = the hub
// couldn't see part of it (no Wi-Fi signal, or not running that long). Any task.
int motion_moved(double from_s, double to_s);
#define MOTION_HISTORY_S 600
