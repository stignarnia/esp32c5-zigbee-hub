// Zigbee 3.0 coordinator. Starts before Wi-Fi and never depends on it.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Starts the stack on its own task. Forms a network on first boot (channel 15 or
// 25, picked by energy scan), restores it from the zb_storage partition after that.
esp_err_t zb_start(void);

bool zb_is_up(void);  // network formed/restored and the coordinator is running
