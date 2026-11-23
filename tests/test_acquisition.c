#ifdef NDEBUG
#undef NDEBUG
#endif
#include "gps_history.h"
#include "radio.h"
#include "wardrive.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static gps_fix_t fix(double latitude) {
    return (gps_fix_t){.valid = true,
                       .latitude = latitude,
                       .longitude = 11,
                       .hdop = 1,
                       .age_ms = 20,
                       .utc = "2026-11-23 12:00:00"};
}
static void sentence(wd_gps_history_t *h, nmea_parser_t *p, const char *body, int64_t now) {
    unsigned crc = 0;
    for (const char *c = body; *c; c++)
        crc ^= (unsigned char)*c;
    char line[180];
    snprintf(line, sizeof(line), "$%s*%02X\r\n", body, crc);
    for (const char *c = line; *c; c++)
        wd_gps_history_feed(h, p, *c, now);
}
static void test_nmea_history(void) {
    nmea_parser_t parser = {0};
    wd_gps_history_t h = {0};
    const char *rmc = "GNRMC,120000,A,4807.038,N,01131.000,E,10,90,231126,,,A";
    const char *gga = "GNGGA,120000,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,";
    sentence(&h, &parser, rmc, 1000);
    assert(!wd_gps_history_at(&h, 1000).valid);
    sentence(&h, &parser, gga, 1100);
    assert(wd_gps_history_at(&h, 1100).valid);
    assert(!wd_gps_history_at(&h, 1099).valid);
    assert(wd_gps_history_at(&h, 1500).age_ms == 500);
    const char *bad = "$GNRMC,broken*00\r\n";
    for (const char *c = bad; *c; c++)
        wd_gps_history_feed(&h, &parser, *c, 1600);
    assert(wd_gps_history_at(&h, 1700).age_ms == 700); /* Bad CRC never refreshes fix. */
    sentence(&h, &parser, "GNRMC,120001,V,,,,,,,231126,,,N", 2000);
    assert(!wd_gps_history_at(&h, 2000).valid);
    assert(wd_gps_history_at(&h, 1900).valid);
    sentence(&h, &parser, rmc, 2100);
    sentence(&h, &parser, gga, 2200);
    assert(wd_gps_history_at(&h, 2300).valid);
    assert(!wd_gps_history_at(&h, 13000).valid);
}
static void test_history(void) {
    wd_gps_history_t h = {0};
    assert(!wd_gps_history_at(&h, 100).valid);
    wd_gps_history_push(&h, fix(48), 1000);
    wd_gps_history_push(&h, fix(49), 2000);
    gps_fix_t f = wd_gps_history_at(&h, 1500);
    assert(f.valid && f.latitude == 48 && f.age_ms == 520);
    assert(!wd_gps_history_at(&h, 999).valid); /* Never borrow a future fix. */
    wd_gps_history_push(&h, (gps_fix_t){.age_ms = UINT32_MAX}, 2100);
    assert(!wd_gps_history_at(&h, 2200).valid);
    assert(wd_gps_history_at(&h, 2050).valid); /* A later loss cannot rewrite history. */
    wd_gps_history_push(&h, fix(50), 3000);
    f = wd_gps_history_at(&h, 14000);
    assert(!f.valid && isnan(f.latitude) && !f.utc[0]);
    assert(wd_gps_history_at(&h, INT64_MAX).age_ms == UINT32_MAX);
    h = (wd_gps_history_t){0};
    wd_gps_history_push(&h, fix(48), INT64_MIN);
    assert(wd_gps_history_at(&h, INT64_MAX).age_ms == UINT32_MAX);
    h = (wd_gps_history_t){0};
    for (unsigned i = 0; i < 10000; i++)
        wd_gps_history_push(&h, fix(48), UINT32_MAX + (int64_t)i);
    assert(h.count == WD_GPS_HISTORY_CAPACITY && h.next < WD_GPS_HISTORY_CAPACITY);
    assert(!wd_gps_history_at(&h, UINT32_MAX).valid);
    assert(wd_gps_history_at(&h, UINT32_MAX + 9999LL).valid);
}
static size_t frame(uint8_t *p, size_t length) {
    memset(p, 0, 256);
    p[0] = 0x80;
    p[16] = 2;
    p[21] = 1;
    p[36] = 0;
    p[37] = (uint8_t)length;
    for (size_t i = 0; i < length; i++)
        p[38 + i] = (uint8_t)i; /* Embedded NUL and nontext bytes stay length-delimited. */
    return 38 + length;
}
static void test_parser(void) {
    uint8_t p[256];
    wd_beacon_t b;
    for (size_t length = 0; length <= 32; length++) {
        size_t n = frame(p, length);
        assert(wd_beacon_parse(p, n, 36, -60, 500, &b));
        assert(b.ssid_len == length && !memcmp(b.ssid, p + 38, length));
        assert(b.channel == 36 && b.rssi == -60 && b.received_ms == 500);
        p[0] = 0x50;
        assert(wd_beacon_parse(p, n, 36, -60, 500, &b));
        for (size_t truncated = 0; truncated < n; truncated++)
            assert(!wd_beacon_parse(p, truncated, 36, -60, 500, &b));
    }
    size_t n = frame(p, 33);
    assert(!wd_beacon_parse(p, n, 1, -60, 0, &b));
    n = frame(p, 3);
    p[n++] = 0;
    p[n++] = 0; /* Duplicate SSID IE. */
    assert(!wd_beacon_parse(p, n, 1, -60, 0, &b));
    n = frame(p, 3);
    p[36] = 1;
    assert(!wd_beacon_parse(p, n, 1, -60, 0, &b));
    n = frame(p, 3);
    p[16] = 3;
    assert(!wd_beacon_parse(p, n, 1, -60, 0, &b));
    n = frame(p, 3);
    p[1] = 0x40;
    assert(!wd_beacon_parse(p, n, 1, -60, 0, &b));
    n = frame(p, 3);
    p[22] = 1;
    assert(!wd_beacon_parse(p, n, 1, -60, 0, &b));
    n = frame(p, 3);
    assert(!wd_beacon_parse(p, n, 0, -60, 0, &b));
    assert(!wd_beacon_parse(p, n, 1, 1, 0, &b));
    assert(!wd_beacon_parse(p, n, 1, -128, 0, &b));
    assert(!wd_beacon_parse(p, 4096, 1, -60, 0, &b));
    /* Deterministic malformed IE fuzzing under ASan/UBSan. */
    uint32_t random = 1;
    for (unsigned trial = 0; trial < 30000; trial++) {
        n = frame(p, 0);
        for (size_t i = 36; i < sizeof(p); i++) {
            random = random * 1664525u + 1013904223u;
            p[i] = (uint8_t)(random >> 24);
        }
        n = 36 + random % (sizeof(p) - 36);
        if (wd_beacon_parse(p, n, 1, -60, 0, &b))
            assert(b.ssid_len <= 32);
    }
}
static void test_cache(void) {
    wd_radio_entry_t entries[8];
    wd_radio_cache_t c;
    assert(wd_radio_cache_init(&c, entries, 8));
    uint32_t generation = wd_radio_cache_begin(&c, 1000);
    wd_beacon_t b = {.bssid = {2, 0, 0, 0, 0, 1},
                     .channel = 1,
                     .rssi = -60,
                     .received_ms = 1100,
                     .generation = generation};
    assert(wd_radio_cache_store(&c, &b, fix(48)));
    b.rssi = -80;
    b.received_ms = 1200;
    b.ssid_len = 3;
    memcpy(b.ssid, "x\0y", 3);
    assert(wd_radio_cache_store(&c, &b, fix(49)));
    const wd_radio_entry_t *e = wd_radio_cache_find(&c, b.bssid, 1, 1300);
    assert(e && e->beacon.rssi == -60 && e->beacon.received_ms == 1100 && e->fix.latitude == 48);
    assert(e->beacon.ssid_len == 3 && !memcmp(e->beacon.ssid, "x\0y", 3));
    b.rssi = -50;
    b.ssid_len = 0;
    assert(wd_radio_cache_store(&c, &b, fix(50)));
    e = wd_radio_cache_find(&c, b.bssid, 1, 1300);
    assert(e->beacon.rssi == -50 && e->fix.latitude == 50 && e->beacon.ssid_len == 3);
    assert(!wd_radio_cache_find(&c, b.bssid, 1, 1199)); /* Reject later evidence. */
    b.channel = 36;
    assert(wd_radio_cache_store(&c, &b, fix(51)));
    assert(c.count == 2 && wd_radio_cache_find(&c, b.bssid, 36, 1300)->fix.latitude == 51);
    b.channel = 1;
    for (unsigned i = 2; i <= 3; i++) {
        b.bssid[5] = i;
        assert(wd_radio_cache_store(&c, &b, fix(48)));
    }
    assert(c.count == 4);
    b.bssid[5] = 4;
    assert(!wd_radio_cache_store(&c, &b, fix(48)) && c.overflows == 1);
    b.bssid[5] = 1;
    assert(wd_radio_cache_store(&c, &b, fix(48))); /* Existing entry survives pressure. */
    wd_radio_cache_begin(&c, 2000);
    assert(!wd_radio_cache_store(&c, &b, fix(48))); /* Previous scan generation. */
    assert(!wd_radio_cache_find(&c, b.bssid, 1, 3000));
    b.generation = c.generation;
    assert(!wd_radio_cache_store(&c, &b, fix(48))); /* Timestamp before scan. */
    c.generation = UINT32_MAX;
    assert(wd_radio_cache_begin(&c, 3000) == 1 && c.count == 0);
    assert(!wd_radio_cache_find(&c, b.bssid, 1, 4000));
}
static void test_unverified_detection(void) {
    wd_engine_t engine;
    wd_ap_t aps[2];
    uint32_t index[4];
    wd_observation_t queue[2];
    assert(wd_engine_init(&engine, aps, 2, index, 4, queue, 2));
    assert(wd_session_begin(&engine, 1));
    wd_detection_t d = {.bssid = {2, 0, 0, 0, 0, 1}, .channel = 1, .band = 2, .rssi = -60};
    assert(wd_detect_tagged(&engine, &d, (gps_fix_t){0}, 1000, WD_RX_UNVERIFIED) == WD_ACCEPTED);
    const wd_observation_t *o = wd_pending(&engine);
    assert((o->reason & WD_RX_UNVERIFIED) && !o->fix.valid && isnan(o->fix.latitude));
    assert(wd_ack(&engine, 1, o->sequence));
    assert(wd_detect_tagged(&engine, &d, (gps_fix_t){0}, 1001, WD_RX_UNVERIFIED) == WD_FILTERED);
    assert(wd_detect(&engine, &d, fix(48), 1002) == WD_ACCEPTED);
    assert(wd_pending(&engine)->reason & WD_GPS_RECOVERED);
}
int main(void) {
    test_nmea_history();
    test_history();
    test_parser();
    test_cache();
    test_unverified_detection();
    puts("acquisition: history, binary beacons, malformed IE fuzzing, bounded cache, provenance "
         "passed");
}
