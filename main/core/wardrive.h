#pragma once
#include "nmea.h"
#include <stddef.h>

#define WD_MIN_DISTANCE_M 15.0f
#define WD_MIN_RSSI_DELTA_DB 6
#define WD_MAX_OBSERVATION_GAP_MS 30000
#define WD_MIN_OBSERVATION_GAP_MS 2000
#define WD_GPS_MAX_AGE_MS 3000
#define WD_GPS_MAX_HDOP 5.0f

typedef enum { WD_SMART, WD_FULL, WD_UNIQUE } wd_mode_t;
typedef enum {
    WD_FIRST = 1,
    WD_DISTANCE = 2,
    WD_RSSI = 4,
    WD_INTERVAL = 8,
    WD_METADATA = 16,
    WD_GPS_RECOVERED = 32,
    WD_SAMPLE = 64,
    WD_RX_UNVERIFIED = 128
} wd_reason_t;
typedef struct {
    uint32_t version, mode, band, active, dwell_ms, min_distance_m, rssi_delta_db;
    uint32_t max_gap_ms, min_gap_ms, gps_max_age_ms, gps_max_hdop_x10, full_gap_ms, baud;
} wd_config_t;
typedef struct {
    uint8_t bssid[6], ssid[32], ssid_len, band, channel, auth;
    uint16_t frequency_mhz, phy;
    int16_t rssi;
} wd_detection_t;
typedef struct {
    bool used, saved;
    wd_detection_t latest, baseline;
    uint64_t first_seen_ms, last_seen_ms, saved_ms, detections, observations;
    double mean_rssi;
    int16_t strongest_rssi;
    int8_t rssi_direction;
    uint32_t changes;
    gps_fix_t best_location, last_location, saved_location;
    char first_utc[20], last_utc[20];
} wd_ap_t;
typedef struct {
    uint64_t total_detections, unique_aps, observations_saved, observations_filtered;
    uint64_t gps_valid_observations, gps_invalid_observations, wifi_2g_aps, wifi_5g_aps;
    uint64_t open_aps, secured_aps, scan_cycles, scan_errors, uart_retransmissions;
    uint64_t uart_dropped_frames, storage_errors, queue_overflows, table_overflows;
} wd_stats_t;
typedef struct {
    wd_detection_t ap;
    gps_fix_t fix;
    uint64_t detected_ms;
    uint32_t session, sequence, reason;
} wd_observation_t;
typedef struct {
    wd_config_t config;
    /* Caller-owned, bounded storage: PSRAM on ESP, ordinary buffers on the host. */
    wd_ap_t *aps;
    uint32_t *ap_index; /* open addressing; 0 empty, record index + 1 occupied */
    wd_observation_t *queue;
    size_t ap_capacity, index_capacity, ap_count, queue_capacity;
    wd_stats_t stats, lifetime;
    uint32_t session, next_sequence, committed;
    size_t head, queued;
} wd_engine_t;
typedef enum {
    WD_FILTERED,
    WD_ACCEPTED,
    WD_QUEUE_FULL,
    WD_TABLE_FULL,
    WD_SEQUENCE_FULL,
    WD_NOT_INITIALIZED
} wd_result_t;

wd_config_t wd_defaults(void);
bool wd_config_valid(const wd_config_t *c);
/* Storage is allocated once by the caller. No allocations occur during scanning.
 * Index capacity must be >= 2 * AP capacity (load factor <= 0.5 even when full).
 * Invalid arguments leave the engine unchanged. Do not rebind an active engine. */
bool wd_engine_init(wd_engine_t *e, wd_ap_t *aps, size_t ap_capacity, uint32_t *index,
                    size_t index_capacity, wd_observation_t *queue, size_t queue_capacity);
/* Rejects reset while any record still awaits durable ACK. Lifetime survives. */
bool wd_session_begin(wd_engine_t *e, uint32_t session);
wd_result_t wd_detect(wd_engine_t *e, const wd_detection_t *d, gps_fix_t fix, uint64_t now);
const wd_observation_t *wd_pending(const wd_engine_t *e);
bool wd_ack(wd_engine_t *e, uint32_t session, uint32_t sequence);
bool wd_fix_valid(gps_fix_t fix, const wd_config_t *config);
double wd_distance(double lat1, double lon1, double lat2, double lon2);

/* Reconnect may confirm only a durable checkpoint or the last actually transmitted head. */
bool wd_resume_ack(wd_engine_t *e, uint32_t session, uint32_t sequence, uint32_t last_sent);

/* Tags describe provenance; they never force an otherwise filtered observation. */
wd_result_t wd_detect_tagged(wd_engine_t *e, const wd_detection_t *d, gps_fix_t fix, uint64_t now,
                             uint32_t tags);
