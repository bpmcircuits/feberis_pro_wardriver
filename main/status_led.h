#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t status_led_init(void);
esp_err_t status_led_update(bool running, bool gps_valid, bool error);
