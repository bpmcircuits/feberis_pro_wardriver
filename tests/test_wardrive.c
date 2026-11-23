#ifdef NDEBUG
#undef NDEBUG
#endif
#include "engine_fixture.h"
#include "wardrive.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static wd_engine_t e;
static wd_ap_t aps[TEST_AP_CAPACITY];
static uint32_t index_table[TEST_AP_CAPACITY * 2];
static wd_observation_t observations[TEST_QUEUE_CAPACITY];
static wd_detection_t d;
static gps_fix_t f;
static void setup(void) {
    memset(&e, 0, sizeof(e));
    assert(wd_engine_init(&e, aps, TEST_AP_CAPACITY, index_table, TEST_AP_CAPACITY * 2,
                          observations, TEST_QUEUE_CAPACITY));
    assert(wd_session_begin(&e, 1));
    d = (wd_detection_t){.bssid = {1, 2, 3, 4, 5, 6},
                         .ssid = {'A', 0, 255},
                         .ssid_len = 3,
                         .band = 2,
                         .channel = 1,
                         .rssi = -60};
    f = (gps_fix_t){.valid = true,
                    .latitude = 52,
                    .longitude = 21,
                    .hdop = 1,
                    .satellites = 8,
                    .horizontal_accuracy_m = NAN};
    memcpy(f.utc, "2026-11-23 12:00:00", 20);
}
static void ack(void) {
    const wd_observation_t *o = wd_pending(&e);
    assert(o);
    uint32_t seq = o->sequence;
    assert(!wd_ack(&e, 2, seq));
    assert(wd_ack(&e, 1, seq));
    assert(!wd_ack(&e, 1, seq));
}
static wd_ap_t *ap(void) {
    for (unsigned i = 0; i < TEST_AP_CAPACITY; i++)
        if (e.aps[i].used)
            return &e.aps[i];
    assert(0);
    return NULL;
}
static void test_stationary(void) {
    setup();
    for (unsigned i = 0; i < 100; i++) {
        assert(wd_detect(&e, &d, f, i * 100) == (i ? WD_FILTERED : WD_ACCEPTED));
        if (!i)
            ack();
    }
    assert(e.stats.unique_aps == 1 && e.stats.total_detections == 100);
    assert(e.stats.observations_saved == 1 && e.stats.observations_filtered == 99);
    assert(ap()->detections == 100 && ap()->mean_rssi == -60);
    assert(wd_detect(&e, &d, f, 30000) == WD_ACCEPTED);
    assert(wd_pending(&e)->reason & WD_INTERVAL);
}
static void test_smart(void) {
    setup();
    assert(wd_detect(&e, &d, f, 0) == WD_ACCEPTED);
    ack();
    f.latitude += 20.0 / 111195.0;
    assert(wd_detect(&e, &d, f, 2000) == WD_ACCEPTED);
    assert(wd_pending(&e)->reason & WD_DISTANCE);
    ack();
    d.rssi = -50;
    assert(wd_detect(&e, &d, f, 4000) == WD_ACCEPTED);
    assert(wd_pending(&e)->reason & WD_RSSI);
    ack();
    d.rssi = -47;
    assert(wd_detect(&e, &d, f, 6000) == WD_FILTERED);
    d.rssi = -44;
    assert(wd_detect(&e, &d, f, 8000) == WD_ACCEPTED);
    ack();
    d.channel = 6;
    assert(wd_detect(&e, &d, f, 8001) == WD_ACCEPTED);
    assert(wd_pending(&e)->reason & WD_METADATA);
    ack();
    d.bssid[5]++;
    assert(wd_detect(&e, &d, f, 8002) == WD_ACCEPTED);
    assert(e.stats.unique_aps == 2);
    ack();
}
static void test_gps_modes(void) {
    setup();
    f.valid = false;
    assert(wd_detect(&e, &d, f, 0) == WD_ACCEPTED);
    assert(!wd_pending(&e)->fix.valid && isnan(wd_pending(&e)->fix.latitude));
    ack();
    f.valid = true;
    assert(wd_detect(&e, &d, f, 1) == WD_ACCEPTED);
    assert(wd_pending(&e)->reason & WD_GPS_RECOVERED);
    assert(ap()->best_location.valid); /* equal strongest RSSI can complete missing GPS */
    ack();
    f.age_ms = 3001;
    assert(wd_detect(&e, &d, f, 30001) == WD_ACCEPTED);
    assert(!wd_pending(&e)->fix.valid);
    ack();
    f.age_ms = 0;
    f.hdop = 6;
    assert(wd_detect(&e, &d, f, 60001) == WD_ACCEPTED);
    assert(!wd_pending(&e)->fix.valid);
    ack();
    e.config.mode = WD_UNIQUE;
    d.channel++;
    assert(wd_detect(&e, &d, f, 90001) == WD_FILTERED);
    e.config.mode = WD_FULL;
    e.config.full_gap_ms = 100;
    assert(wd_detect(&e, &d, f, 90002) == WD_ACCEPTED);
    ack();
    assert(wd_detect(&e, &d, f, 90050) == WD_FILTERED);
    assert(wd_detect(&e, &d, f, 90102) == WD_ACCEPTED);
    ack();
}
static void test_queue_restart(void) {
    setup();
    e.config.mode = WD_FULL;
    e.config.full_gap_ms = 0;
    for (unsigned i = 0; i < TEST_QUEUE_CAPACITY; i++)
        assert(wd_detect(&e, &d, f, i) == WD_ACCEPTED);
    wd_observation_t first = *wd_pending(&e);
    assert(wd_detect(&e, &d, f, 40) == WD_QUEUE_FULL);
    assert(!memcmp(&first, wd_pending(&e), sizeof(first)));
    assert(e.stats.queue_overflows == 1 && ap()->detections == TEST_QUEUE_CAPACITY + 1);
    while (e.queued)
        ack();
    assert(wd_detect(&e, &d, f, 41) == WD_ACCEPTED);
    ack();
    uint64_t lifetime = e.lifetime.total_detections;
    uint32_t previous_sequence = e.next_sequence;
    assert(wd_session_begin(&e, 2));
    assert(!e.stats.total_detections && e.lifetime.total_detections == lifetime);
    assert(wd_detect(&e, &d, f, 50) == WD_ACCEPTED);
    assert(wd_pending(&e)->session == 2 && wd_pending(&e)->sequence == previous_sequence + 1);
    /* RAM is deliberately volatile after an ESP reset; old-session ACK cannot remove a new record.
     */
    assert(!wd_ack(&e, 1, 1));
}
static void test_capacity_and_clock(void) {
    setup();
    size_t bytes = sizeof(e);
    for (unsigned i = 0; i < 10000; i++) {
        d.bssid[0] = (uint8_t)i;
        d.bssid[1] = (uint8_t)(i >> 8);
        wd_result_t result = wd_detect(&e, &d, f, (uint64_t)UINT32_MAX + i);
        if (i < TEST_AP_CAPACITY) {
            assert(result == WD_ACCEPTED);
            ack();
        } else
            assert(result == WD_TABLE_FULL);
    }
    assert(e.stats.unique_aps == TEST_AP_CAPACITY && sizeof(e) == bytes);
    assert(e.stats.table_overflows == 10000 - TEST_AP_CAPACITY);
    setup();
    assert(wd_detect(&e, &d, f, UINT32_MAX - 1000ULL) == WD_ACCEPTED);
    ack();
    assert(wd_detect(&e, &d, f, UINT32_MAX + 1000ULL) == WD_FILTERED);
    d.rssi = -50;
    assert(wd_detect(&e, &d, f, UINT32_MAX + 1001ULL) == WD_ACCEPTED);
    ack();
    d.rssi = -56;
    assert(wd_detect(&e, &d, f, UINT32_MAX + 4001ULL) == WD_FILTERED); /* reversal deadband */
    d.rssi = -58;
    assert(wd_detect(&e, &d, f, UINT32_MAX + 4002ULL) == WD_ACCEPTED);
    ack();
    e.next_sequence = UINT32_MAX;
    d.channel++;
    assert(wd_detect(&e, &d, f, UINT32_MAX + 1002ULL) == WD_SEQUENCE_FULL);
    assert(wd_config_valid(&e.config));
    e.config.max_gap_ms = 1;
    assert(!wd_config_valid(&e.config));
}
int main(void) {
    test_stationary();
    test_smart();
    test_gps_modes();
    test_queue_restart();
    test_capacity_and_clock();
    puts("SMART, GPS, modes, queue, identity, bounded memory, restart and clock tests passed");
}
