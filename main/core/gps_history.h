#pragma once
#include "nmea.h"
#define WD_GPS_HISTORY_CAPACITY 64

typedef struct {
    gps_fix_t fix;
    int64_t sampled_ms;
} wd_gps_sample_t;
typedef struct {
    wd_gps_sample_t samples[WD_GPS_HISTORY_CAPACITY];
    size_t next, count;
} wd_gps_history_t;
void wd_gps_history_push(wd_gps_history_t *h, gps_fix_t fix, int64_t sampled_ms);
void wd_gps_history_feed(wd_gps_history_t *h, nmea_parser_t *parser, char byte, int64_t now_ms);
/* Never uses a fix published after the requested radio timestamp. */
gps_fix_t wd_gps_history_at(const wd_gps_history_t *h, int64_t at_ms);
