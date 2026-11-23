#ifdef NDEBUG
#undef NDEBUG
#endif
#include "delivery.h"
#include "monotonic.h"
#include "nmea.h"
#include "wardrive.h"
#include "wire.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void sentence(nmea_parser_t *p, const char *body, int64_t now) {
    unsigned crc = 0;
    for (const char *c = body; *c; c++)
        crc ^= (unsigned char)*c;
    char line[256];
    snprintf(line, sizeof(line), "$%s*%02X\r\n", body, crc);
    for (const char *c = line; *c; c++)
        nmea_feed(p, *c, now);
}
static void pair(nmea_parser_t *p, int64_t now) {
    sentence(p, "GNRMC,235959,A,4807.038,N,01131.000,E,10,90,311226,,,A", now);
    sentence(p, "GNGGA,235959,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,", now);
}
static void test_nmea_edges(void) {
    nmea_parser_t p = {0};
    pair(&p, 1000);
    gps_fix_t f = nmea_snapshot(&p, 1000);
    assert(f.valid && fabs(f.speed_kmh - 18.52) < 1e-6 && f.course_deg == 90);
    sentence(&p, "GNGGA,235959,4907.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,", 1000);
    assert(!nmea_snapshot(&p, 1000).valid); /* individually valid, geographically inconsistent */
    pair(&p, 1100);
    sentence(&p, "GNRMC,000000,A,4807.038,N,01131.000,E,10,90,010127,,,A", 1200);
    assert(nmea_snapshot(&p, 1200).valid); /* adjacent epochs across midnight */
    assert(!strcmp(nmea_snapshot(&p, 1200).utc, "2027-01-01 00:00:00"));
    sentence(&p, "GNRMC,000000,A,4807.038,N,01131.000,E,,,010127,,,A", 1200);
    f = nmea_snapshot(&p, 1200);
    assert(f.valid && isnan(f.speed_kmh) && isnan(f.course_deg));
    sentence(&p, "GNGGA,000000,4807.038,N,01131.000,E,1,08,0.9,,M,46.9,M,,", 1200);
    assert(nmea_snapshot(&p, 1200).valid && isnan(nmea_snapshot(&p, 1200).altitude_m));
    pair(&p, 2000);
    sentence(&p, "GNRMC,235959,A,4807.038,N,01131.000,E,1e2,90,311226,,,A", 2000);
    assert(!nmea_snapshot(&p, 2000).valid);
    pair(&p, 2000);
    sentence(&p, "GNGGA,235959,4807.038,N,01131.000,E,0x1,08,0.9,545.4,M,46.9,M,,", 2000);
    assert(!nmea_snapshot(&p, 2000).valid);
    pair(&p, 2000);
    sentence(&p, "GNGGA,235959,4807.038,N,01131.000,E,1,08, 0.9,545.4,M,46.9,M,,", 2000);
    assert(!nmea_snapshot(&p, 2000).valid);
    pair(&p, 2000);
    sentence(&p, "GNRMC,235959,A,48.1173,N,01131.000,E,10,90,311226,,,A", 2000);
    assert(!nmea_snapshot(&p, 2000).valid); /* decimal degrees are not NMEA ddmm */
    pair(&p, INT64_MIN);
    assert(nmea_snapshot(&p, INT64_MIN).valid);
    assert(!nmea_snapshot(&p, INT64_MAX).valid &&
           nmea_snapshot(&p, INT64_MAX).age_ms == UINT32_MAX);
    pair(&p, INT64_MAX);
    assert(!nmea_snapshot_max_age(&p, INT64_MIN, UINT32_MAX).valid);
    pair(&p, UINT32_MAX - 1000LL);
    assert(nmea_snapshot(&p, UINT32_MAX + 1000LL).valid);
    assert(!nmea_snapshot(&p, UINT32_MAX + 2001LL).valid);
    /* Date leap validation and coordinates at the poles/date line. */
    memset(&p, 0, sizeof(p));
    sentence(&p, "GNRMC,120000,A,9000.000,N,18000.000,E,0,0,290228,,,A", 0);
    sentence(&p, "GNGGA,120000,9000.000,N,18000.000,E,1,08,1,-20,M,0,M,,", 0);
    assert(nmea_snapshot(&p, 0).valid);
    sentence(&p, "GNRMC,120000,A,9000.000,N,18000.000,E,0,0,290227,,,A", 0);
    assert(!nmea_snapshot(&p, 0).valid);
}
static void test_transport_edges(void) {
    delivery_t d;
    delivery_begin(&d, 1, INT64_MIN);
    assert(delivery_poll(&d, INT64_MAX) == DELIVERY_RETRY);
    delivery_begin(&d, 2, INT64_MAX);
    assert(delivery_poll(&d, INT64_MIN) == DELIVERY_WAIT);
    assert(!wd_elapsed_ms(99, 100, 0));
    assert(wd_elapsed_ms(120000, 0, 120000)); /* scan deadline independent of pause */
    assert(!wd_elapsed_ms(119999, 0, 120000));
    char payload[WIRE_MAX], frame[WIRE_MAX];
    memset(payload, 'A', sizeof(payload));
    payload[1529] = 0;
    size_t n = wire_encode(payload, frame, sizeof(frame));
    assert(n == 1535);
    wire_reader_t r = {0};
    for (size_t i = 0; i < n; i++)
        assert(wire_feed(&r, frame[i]) == (i + 1 == n));
    payload[1529] = 'A';
    payload[1530] = 0;
    assert(!wire_encode(payload, frame, sizeof(frame)));
    strcpy(payload, "FBR1\t1\tPING");
    for (unsigned i = 3; i < 48; i++)
        strcat(payload, "\tx");
    n = wire_encode(payload, frame, sizeof(frame));
    frame[n - 1] = 0;
    char *fields[48];
    assert(wire_decode(frame, fields, 48) == 48);
    strcat(payload, "\tx");
    n = wire_encode(payload, frame, sizeof(frame));
    frame[n - 1] = 0;
    assert(wire_decode(frame, fields, 48) == 0);
}
static wd_engine_t e;
static wd_ap_t aps[3];
static uint32_t index_table[6];
static wd_observation_t queue[2];
static void test_resume_and_wrap(void) {
    assert(wd_engine_init(&e, aps, 3, index_table, 6, queue, 2));
    assert(wd_session_begin(&e, 7));
    wd_detection_t ap = {.bssid = {2, 0, 0, 0, 0, 1}, .band = 2, .channel = 1, .rssi = -60};
    assert(wd_detect(&e, &ap, (gps_fix_t){0}, 0) == WD_ACCEPTED);
    uint32_t head = wd_pending(&e)->sequence;
    assert(!wd_resume_ack(&e, 7, head, 0)); /* queued but not yet sent */
    assert(!wd_resume_ack(&e, 8, head, head));
    assert(e.queued == 1 && e.committed == 0);
    assert(wd_resume_ack(&e, 7, 0, head)); /* already committed checkpoint is idempotent */
    assert(e.queued == 1);
    assert(wd_resume_ack(&e, 7, head, head));
    assert(!e.queued && e.committed == head);
    assert(wd_resume_ack(&e, 7, head, head));
    e.config.mode = WD_FULL;
    e.config.full_gap_ms = 0;
    /* Wrap the circular queue many times; sequences and records remain ordered. */
    for (unsigned i = 0; i < 10000; i++) {
        ap.rssi = -60 - (int)(i % 10);
        assert(wd_detect(&e, &ap, (gps_fix_t){0}, i + 1) == WD_ACCEPTED);
        assert(wd_pending(&e)->sequence == head + i + 1);
        assert(wd_pending(&e)->ap.rssi == ap.rssi);
        assert(wd_ack(&e, 7, head + i + 1));
    }
    assert(e.stats.total_detections == 10001 && e.stats.unique_aps == 1);
    assert(e.aps[0].mean_rssi <= -60 && e.aps[0].mean_rssi >= -69);
    assert(wd_session_begin(&e, 8));
    e.next_sequence = UINT32_MAX - 1;
    assert(wd_detect(&e, &ap, (gps_fix_t){0}, 0) == WD_ACCEPTED);
    assert(wd_pending(&e)->sequence == UINT32_MAX);
    assert(wd_ack(&e, 8, UINT32_MAX));
    assert(wd_detect(&e, &ap, (gps_fix_t){0}, 30001) == WD_SEQUENCE_FULL);
    assert(!e.queued && e.next_sequence == UINT32_MAX);
}
static void test_binary_and_hidden_ssid(void) {
    assert(wd_engine_init(&e, aps, 3, index_table, 6, queue, 2));
    assert(wd_session_begin(&e, 1));
    wd_detection_t ap = {
        .band = 2, .channel = 1, .rssi = -60, .ssid = {'A', 0, 255}, .ssid_len = 33};
    assert(wd_detect(&e, &ap, (gps_fix_t){0}, 0) == WD_FILTERED);
    assert(!e.ap_count && !e.stats.total_detections && !e.queued);
    ap.ssid_len = 3;
    assert(wd_detect(&e, &ap, (gps_fix_t){0}, 0) == WD_ACCEPTED);
    assert(!memcmp(wd_pending(&e)->ap.ssid, ap.ssid, 3));
    assert(wd_ack(&e, 1, wd_pending(&e)->sequence));
    ap.ssid_len = 0;
    assert(wd_detect(&e, &ap, (gps_fix_t){0}, 1) == WD_ACCEPTED);
    assert(wd_pending(&e)->reason & WD_METADATA);
    assert(wd_ack(&e, 1, wd_pending(&e)->sequence));
    ap.ssid[0] = 'B'; /* bytes beyond the declared length have no meaning */
    assert(wd_detect(&e, &ap, (gps_fix_t){0}, 2) == WD_FILTERED);
}
static void test_config_boundaries(void) {
    struct invalid {
        size_t offset;
        uint32_t value;
    } cases[] = {{offsetof(wd_config_t, version), 0},
                 {offsetof(wd_config_t, version), UINT32_MAX},
                 {offsetof(wd_config_t, mode), 3},
                 {offsetof(wd_config_t, band), 3},
                 {offsetof(wd_config_t, active), 2},
                 {offsetof(wd_config_t, dwell_ms), 19},
                 {offsetof(wd_config_t, dwell_ms), 1501},
                 {offsetof(wd_config_t, min_distance_m), 0},
                 {offsetof(wd_config_t, min_distance_m), 10001},
                 {offsetof(wd_config_t, rssi_delta_db), 0},
                 {offsetof(wd_config_t, rssi_delta_db), 101},
                 {offsetof(wd_config_t, max_gap_ms), 999},
                 {offsetof(wd_config_t, max_gap_ms), 3600001},
                 {offsetof(wd_config_t, min_gap_ms), 60001},
                 {offsetof(wd_config_t, gps_max_age_ms), 99},
                 {offsetof(wd_config_t, gps_max_age_ms), 10001},
                 {offsetof(wd_config_t, gps_max_hdop_x10), 0},
                 {offsetof(wd_config_t, gps_max_hdop_x10), 501},
                 {offsetof(wd_config_t, full_gap_ms), 60001},
                 {offsetof(wd_config_t, baud), 9600},
                 {offsetof(wd_config_t, baud), UINT32_MAX}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        wd_config_t c = wd_defaults();
        memcpy((char *)&c + cases[i].offset, &cases[i].value, sizeof(uint32_t));
        assert(!wd_config_valid(&c));
    }
    wd_config_t c = wd_defaults();
    c.min_gap_ms = 0;
    c.full_gap_ms = 0;
    c.gps_max_hdop_x10 = 1;
    c.dwell_ms = 20;
    c.max_gap_ms = 1000;
    c.gps_max_age_ms = 100;
    assert(wd_config_valid(&c));
    c.min_gap_ms = 60000;
    c.full_gap_ms = 60000;
    c.gps_max_hdop_x10 = 500;
    c.dwell_ms = 1500;
    c.max_gap_ms = 3600000;
    c.gps_max_age_ms = 10000;
    c.min_distance_m = 10000;
    c.rssi_delta_db = 100;
    c.baud = 921600;
    assert(wd_config_valid(&c));
}
int main(void) {
    test_binary_and_hidden_ssid();
    test_config_boundaries();
    test_nmea_edges();
    test_transport_edges();
    test_resume_and_wrap();
    puts("NMEA consistency/grammar/time extremes, framing bounds, reconnect guards and queue wrap "
         "passed");
}
