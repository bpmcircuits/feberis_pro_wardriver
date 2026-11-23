#pragma once
#include "core/radio.h"
#include "core/wardrive.h"
#include "esp_err.h"

typedef struct {
    uint64_t received, dropped, malformed;
    size_t queue_high_water;
} radio_capture_stats_t;
/* Cache is allocated once in PSRAM; callback queue lives in internal SRAM. */
esp_err_t radio_capture_init(void);
size_t radio_capture_storage_bytes(void);
void radio_capture_begin(int64_t started_ms);
void radio_capture_end(void);
void radio_capture_drain(const wd_config_t *config);
void radio_capture_reset_stats(void);
radio_capture_stats_t radio_capture_stats(void);
const wd_radio_entry_t *radio_capture_find(const uint8_t bssid[6], uint8_t channel,
                                           int64_t completed_ms);
uint64_t radio_capture_overflows(void);
