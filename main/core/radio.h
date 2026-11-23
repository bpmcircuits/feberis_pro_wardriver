#pragma once
#include "nmea.h"
#include <stddef.h>

typedef struct {
    uint8_t bssid[6], ssid[32], ssid_len, channel;
    int8_t rssi;
    uint32_t generation;
    int64_t received_ms;
} wd_beacon_t;
typedef struct {
    uint32_t generation;
    wd_beacon_t beacon;
    gps_fix_t fix;
} wd_radio_entry_t;
typedef struct {
    wd_radio_entry_t *entries;
    size_t capacity, count;
    uint32_t generation;
    int64_t started_ms;
    uint64_t overflows;
} wd_radio_cache_t;
/* Input is a complete management frame without its four-byte FCS. */
bool wd_beacon_parse(const uint8_t *frame, size_t length, uint8_t channel, int8_t rssi,
                     int64_t received_ms, wd_beacon_t *out);
bool wd_radio_cache_init(wd_radio_cache_t *c, wd_radio_entry_t *entries, size_t capacity);
uint32_t wd_radio_cache_begin(wd_radio_cache_t *c, int64_t started_ms);
bool wd_radio_cache_store(wd_radio_cache_t *c, const wd_beacon_t *b, gps_fix_t fix);
const wd_radio_entry_t *wd_radio_cache_find(const wd_radio_cache_t *c, const uint8_t bssid[6],
                                            uint8_t channel, int64_t completed_ms);
