#pragma once
#include "core/nmea.h"
#include "esp_err.h"

esp_err_t gps_service_start(void);
gps_fix_t gps_service_snapshot(void);
