#include "wardrive.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

wd_config_t wd_defaults(void) {
    return (wd_config_t){.version = 1,
                         .mode = WD_SMART,
                         .dwell_ms = 120,
                         .min_distance_m = WD_MIN_DISTANCE_M,
                         .rssi_delta_db = WD_MIN_RSSI_DELTA_DB,
                         .max_gap_ms = WD_MAX_OBSERVATION_GAP_MS,
                         .min_gap_ms = WD_MIN_OBSERVATION_GAP_MS,
                         .gps_max_age_ms = WD_GPS_MAX_AGE_MS,
                         .gps_max_hdop_x10 = 50,
                         .full_gap_ms = 2000,
                         .baud = 115200};
}
bool wd_config_valid(const wd_config_t *c) {
    return c->version == 1 && c->mode <= WD_UNIQUE && c->band <= 2 && c->active <= 1 &&
           c->dwell_ms >= 20 && c->dwell_ms <= 1500 && c->min_distance_m >= 1 &&
           c->min_distance_m <= 10000 && c->rssi_delta_db >= 1 && c->rssi_delta_db <= 100 &&
           c->min_gap_ms <= 60000 && c->max_gap_ms >= c->min_gap_ms && c->max_gap_ms >= 1000 &&
           c->max_gap_ms <= 3600000 && c->gps_max_age_ms >= 100 && c->gps_max_age_ms <= 10000 &&
           c->gps_max_hdop_x10 >= 1 && c->gps_max_hdop_x10 <= 500 && c->full_gap_ms <= 60000 &&
           (c->baud == 115200 || c->baud == 460800 || c->baud == 921600);
}
bool wd_engine_init(wd_engine_t *e, wd_ap_t *aps, size_t ap_capacity, uint32_t *index,
                    size_t index_capacity, wd_observation_t *queue, size_t queue_capacity) {
    if (!e || !aps || !index || !queue || !ap_capacity || !queue_capacity ||
        ap_capacity > UINT32_MAX - 1 || ap_capacity > SIZE_MAX / sizeof(*aps) ||
        ap_capacity > SIZE_MAX / 2 || index_capacity < ap_capacity * 2 ||
        index_capacity > SIZE_MAX / sizeof(*index) || queue_capacity > SIZE_MAX / sizeof(*queue))
        return false;
    *e = (wd_engine_t){.config = wd_defaults(),
                       .aps = aps,
                       .ap_capacity = ap_capacity,
                       .ap_index = index,
                       .index_capacity = index_capacity,
                       .queue = queue,
                       .queue_capacity = queue_capacity};
    return wd_session_begin(e, 0);
}
bool wd_session_begin(wd_engine_t *e, uint32_t session) {
    if (!e || !e->aps || !e->ap_index || !e->queue || e->queued)
        return false;
    memset(e->aps, 0, e->ap_capacity * sizeof(*e->aps));
    memset(e->ap_index, 0, e->index_capacity * sizeof(*e->ap_index));
    memset(&e->stats, 0, sizeof(e->stats));
    e->ap_count = 0;
    e->session = session;
    e->committed = e->next_sequence;
    e->head = 0;
    return true;
}
bool wd_fix_valid(gps_fix_t f, const wd_config_t *c) {
    return f.valid && f.age_ms <= c->gps_max_age_ms && isfinite(f.hdop) && f.hdop > 0 &&
           f.hdop <= c->gps_max_hdop_x10 / 10.0 && isfinite(f.latitude) && isfinite(f.longitude) &&
           fabs(f.latitude) <= 90 && fabs(f.longitude) <= 180;
}
double wd_distance(double a, double b, double c, double d) {
    const double r = 0.017453292519943295;
    double x = sin((c - a) * r / 2), y = sin((d - b) * r / 2);
    double h = x * x + cos(a * r) * cos(c * r) * y * y;
    return 12742000.0 * asin(sqrt(fmin(1.0, fmax(0.0, h))));
}
#define INC(field)                                                                                 \
    do {                                                                                           \
        ++e->stats.field;                                                                          \
        ++e->lifetime.field;                                                                       \
    } while (0)
wd_result_t wd_detect(wd_engine_t *e, const wd_detection_t *d, gps_fix_t fix, uint64_t now) {
    if (!e->aps || !e->ap_index || !e->queue)
        return WD_NOT_INITIALIZED;
    if (d->ssid_len > 32)
        return WD_FILTERED;
    INC(total_detections);
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < 6; i++)
        hash = (hash ^ d->bssid[i]) * 16777619u;
    wd_ap_t *a = NULL;
    size_t bucket = hash % e->index_capacity;
    for (size_t n = 0; n < e->index_capacity; n++) {
        uint32_t record = e->ap_index[bucket];
        if (!record) {
            if (e->ap_count < e->ap_capacity) {
                a = &e->aps[e->ap_count];
                e->ap_index[bucket] = (uint32_t)(++e->ap_count);
            }
            break;
        }
        if (!memcmp(e->aps[record - 1].latest.bssid, d->bssid, 6)) {
            a = &e->aps[record - 1];
            break;
        }
        if (++bucket == e->index_capacity)
            bucket = 0;
    }
    if (!a) {
        INC(table_overflows);
        return WD_TABLE_FULL;
    }
    fix.valid = wd_fix_valid(fix, &e->config);
    if (!fix.valid) {
        if (fix.age_ms > e->config.gps_max_age_ms)
            fix.utc[0] = 0;
        fix.latitude = fix.longitude = fix.altitude_m = NAN;
        fix.horizontal_accuracy_m = NAN;
    }
    bool first = !a->used;
    if (first) {
        a->used = true;
        a->first_seen_ms = now;
        a->strongest_rssi = INT16_MIN;
        memcpy(a->first_utc, fix.utc, sizeof(a->first_utc));
        INC(unique_aps);
        if (d->band == 5) {
            INC(wifi_5g_aps);
        } else {
            INC(wifi_2g_aps);
        }
        if (d->auth == 0) {
            INC(open_aps);
        } else {
            INC(secured_aps);
        }
    }
    bool changed = !first && (a->latest.channel != d->channel || a->latest.band != d->band ||
                              a->latest.auth != d->auth || a->latest.phy != d->phy ||
                              a->latest.ssid_len != d->ssid_len ||
                              memcmp(a->latest.ssid, d->ssid, d->ssid_len));
    if (changed)
        ++a->changes;
    a->last_seen_ms = now;
    a->last_location = fix;
    memcpy(a->last_utc, fix.utc, sizeof(a->last_utc));
    ++a->detections;
    a->mean_rssi += (d->rssi - a->mean_rssi) / (double)a->detections;
    if (d->rssi > a->strongest_rssi ||
        (d->rssi == a->strongest_rssi && fix.valid && !a->best_location.valid)) {
        a->strongest_rssi = d->rssi;
        a->best_location = fix;
    }
    a->latest = *d;
    uint32_t reason = 0;
    uint64_t elapsed = now >= a->saved_ms ? now - a->saved_ms : 0;
    if (!a->saved)
        reason = WD_FIRST;
    else if (e->config.mode == WD_FULL) {
        if (elapsed >= e->config.full_gap_ms)
            reason = WD_SAMPLE;
    } else if (e->config.mode == WD_SMART) {
        /* Metadata compares to saved baseline, including after queue overflow. */
        if (a->baseline.channel != d->channel || a->baseline.band != d->band ||
            a->baseline.auth != d->auth || a->baseline.phy != d->phy ||
            a->baseline.ssid_len != d->ssid_len || memcmp(a->baseline.ssid, d->ssid, d->ssid_len))
            reason |= WD_METADATA;
        if (fix.valid && !a->saved_location.valid)
            reason |= WD_GPS_RECOVERED;
        if (elapsed >= e->config.min_gap_ms) {
            if (elapsed >= e->config.max_gap_ms)
                reason |= WD_INTERVAL;
            /* Deadband relative to saved value, not consecutive raw detections. */
            int delta = d->rssi - a->baseline.rssi;
            int direction = (delta > 0) - (delta < 0);
            int threshold = (int)e->config.rssi_delta_db;
            /* Schmitt hysteresis: a reversal needs an extra 2 dB. Large changes remain immediate.
             */
            if (a->rssi_direction && direction != a->rssi_direction)
                threshold += 2;
            if (abs(delta) >= threshold)
                reason |= WD_RSSI;
            if (fix.valid && a->saved_location.valid &&
                wd_distance(fix.latitude, fix.longitude, a->saved_location.latitude,
                            a->saved_location.longitude) >= e->config.min_distance_m)
                reason |= WD_DISTANCE;
        }
    }
    if (!reason) {
        INC(observations_filtered);
        return WD_FILTERED;
    }
    if (e->queued == e->queue_capacity) {
        INC(queue_overflows);
        return WD_QUEUE_FULL;
    }
    if (e->next_sequence == UINT32_MAX)
        return WD_SEQUENCE_FULL;
    wd_observation_t *o = &e->queue[(e->head + e->queued) % e->queue_capacity];
    *o = (wd_observation_t){.ap = *d,
                            .fix = fix,
                            .session = e->session,
                            .sequence = ++e->next_sequence,
                            .reason = reason,
                            .detected_ms = now};
    ++e->queued;
    a->saved = true;
    if (reason & WD_RSSI) {
        int delta = d->rssi - a->baseline.rssi;
        a->rssi_direction = (delta > 0) - (delta < 0);
    }
    a->baseline = *d;
    a->saved_ms = now;
    a->saved_location = fix;
    ++a->observations;
    INC(observations_saved);
    if (fix.valid) {
        INC(gps_valid_observations);
    } else {
        INC(gps_invalid_observations);
    }
    return WD_ACCEPTED;
}
const wd_observation_t *wd_pending(const wd_engine_t *e) {
    return e->queued ? &e->queue[e->head] : NULL;
}
bool wd_ack(wd_engine_t *e, uint32_t session, uint32_t sequence) {
    const wd_observation_t *o = wd_pending(e);
    if (!o || o->session != session || o->sequence != sequence)
        return false;
    e->committed = sequence;
    e->head = (e->head + 1) % e->queue_capacity;
    --e->queued;
    return true;
}

bool wd_resume_ack(wd_engine_t *e, uint32_t session, uint32_t sequence, uint32_t last_sent) {
    if (session != e->session)
        return false;
    if (sequence == e->committed)
        return true;
    const wd_observation_t *o = wd_pending(e);
    if (!o || sequence != o->sequence || sequence != last_sent)
        return false;
    return wd_ack(e, session, sequence);
}
