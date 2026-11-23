#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPS_MAX_AGE_MS 3000

typedef struct {
    bool valid;
    double latitude, longitude, altitude_m, hdop;
    double speed_kmh, course_deg, horizontal_accuracy_m;
    unsigned satellites;
    uint32_t age_ms;
    char utc[20];
} gps_fix_t;

typedef struct {
    char line[160];
    size_t used;
    bool dropping;
    bool rmc_valid, gga_valid;
    int rmc_second, gga_second;
    int64_t rmc_ms, gga_ms;
    double gga_latitude, gga_longitude;
    gps_fix_t fix;
} nmea_parser_t;

void nmea_feed(nmea_parser_t *parser, char byte, int64_t now_ms);
gps_fix_t nmea_snapshot(const nmea_parser_t *parser, int64_t now_ms);

gps_fix_t nmea_snapshot_max_age(const nmea_parser_t *p, int64_t now_ms, uint32_t max_age_ms);
