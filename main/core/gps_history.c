#include "gps_history.h"
#include <math.h>
#include <string.h>

void wd_gps_history_feed(wd_gps_history_t *h, nmea_parser_t *p, char byte, int64_t now_ms) {
    bool solution = byte == '\n' && p->used >= 6 && p->line[0] == '$' &&
                    (!memcmp(p->line + 3, "RMC", 3) || !memcmp(p->line + 3, "GGA", 3));
    nmea_feed(p, byte, now_ms);
    if (solution)
        wd_gps_history_push(h, nmea_snapshot_max_age(p, now_ms, 10000), now_ms);
}

void wd_gps_history_push(wd_gps_history_t *h, gps_fix_t fix, int64_t sampled_ms) {
    h->samples[h->next] = (wd_gps_sample_t){.fix = fix, .sampled_ms = sampled_ms};
    h->next = (h->next + 1) % WD_GPS_HISTORY_CAPACITY;
    if (h->count < WD_GPS_HISTORY_CAPACITY)
        ++h->count;
}
gps_fix_t wd_gps_history_at(const wd_gps_history_t *h, int64_t at_ms) {
    gps_fix_t f = {.latitude = NAN,
                   .longitude = NAN,
                   .altitude_m = NAN,
                   .hdop = NAN,
                   .horizontal_accuracy_m = NAN,
                   .speed_kmh = NAN,
                   .course_deg = NAN,
                   .age_ms = UINT32_MAX};
    for (size_t i = 0; i < h->count; i++) {
        size_t slot = (h->next + WD_GPS_HISTORY_CAPACITY - 1 - i) % WD_GPS_HISTORY_CAPACITY;
        const wd_gps_sample_t *s = &h->samples[slot];
        if (s->sampled_ms > at_ms)
            continue;
        f = s->fix;
        uint64_t elapsed = (uint64_t)at_ms - (uint64_t)s->sampled_ms;
        f.age_ms = elapsed > UINT32_MAX - f.age_ms ? UINT32_MAX : f.age_ms + (uint32_t)elapsed;
        if (f.age_ms > 10000) {
            f.valid = false;
            f.utc[0] = 0;
        }
        if (!f.valid)
            f.latitude = f.longitude = f.altitude_m = f.horizontal_accuracy_m = NAN;
        return f;
    }
    return f;
}
