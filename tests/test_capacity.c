#ifdef NDEBUG
#undef NDEBUG
#endif
#include "wardrive.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wd_engine_t e;
static wd_detection_t detection(unsigned i) {
    wd_detection_t d = {.bssid = {2, 0, 0, 0, 0, 0}, .band = 2, .channel = 1, .rssi = -65};
    d.bssid[4] = (uint8_t)(i >> 8);
    d.bssid[5] = (uint8_t)i;
    return d;
}
int main(void) {
    const size_t capacity = 10000, buckets = capacity * 2, queue_capacity = 2048;
    wd_ap_t *aps = calloc(capacity, sizeof(*aps));
    uint32_t *index = calloc(buckets, sizeof(*index));
    wd_observation_t *queue = calloc(queue_capacity, sizeof(*queue));
    assert(aps && index && queue);
    assert(!wd_engine_init(&e, NULL, capacity, index, buckets, queue, queue_capacity));
    assert(!wd_engine_init(&e, aps, capacity, index, buckets - 1, queue, queue_capacity));
    assert(!wd_engine_init(&e, aps, capacity, index, buckets, queue, 0));
    assert(!wd_engine_init(&e, aps, SIZE_MAX, index, buckets, queue, queue_capacity));
    assert(wd_engine_init(&e, aps, capacity, index, buckets, queue, queue_capacity));
    assert(wd_session_begin(&e, 7));
    e.config.mode = WD_UNIQUE;
    const size_t bytes =
        capacity * sizeof(*aps) + buckets * sizeof(*index) + queue_capacity * sizeof(*queue);
    gps_fix_t gps = {0};
    for (unsigned i = 0; i < capacity; i++) {
        wd_detection_t d = detection(i);
        assert(wd_detect(&e, &d, gps, 1000) == WD_ACCEPTED);
        assert(!memcmp(aps[i].latest.bssid, d.bssid, 6)); /* stable dense GET_AP slots */
        assert(wd_ack(&e, 7, wd_pending(&e)->sequence));
    }
    assert(e.ap_count == capacity && e.stats.unique_aps == capacity);
    /* Successful lookups still work when every record is occupied. */
    for (unsigned i = 0; i < capacity; i++) {
        wd_detection_t d = detection(capacity - i - 1);
        assert(wd_detect(&e, &d, gps, 2000) == WD_FILTERED);
    }
    wd_detection_t extra = detection((unsigned)capacity);
    assert(wd_detect(&e, &extra, gps, 2001) == WD_TABLE_FULL);
    assert(e.ap_count == capacity && e.stats.unique_aps == capacity);
    assert(wd_session_begin(&e, 8));
    assert(!e.ap_count && e.stats.unique_aps == 0 && e.lifetime.unique_aps == capacity);
    /* Full disconnect queue retains every byte and baseline; rejected reset is non-destructive. */
    for (unsigned i = 0; i < queue_capacity; i++) {
        wd_detection_t d = detection(i);
        assert(wd_detect(&e, &d, gps, 3000) == WD_ACCEPTED);
    }
    const wd_observation_t first = *wd_pending(&e);
    assert(!wd_session_begin(&e, 9) && e.session == 8 && e.queued == queue_capacity);
    extra = detection((unsigned)queue_capacity);
    assert(wd_detect(&e, &extra, gps, 3001) == WD_QUEUE_FULL);
    assert(e.aps[queue_capacity].saved == false);
    assert(!memcmp(&first, wd_pending(&e), sizeof(first)));
    while (e.queued)
        assert(wd_ack(&e, 8, wd_pending(&e)->sequence));
    assert(wd_detect(&e, &extra, gps, 3002) == WD_ACCEPTED);
    assert(wd_pending(&e)->reason == WD_FIRST);
    assert(wd_ack(&e, 8, wd_pending(&e)->sequence));
    /* Initialization failure leaves an already initialized engine unchanged. */
    assert(!wd_engine_init(&e, aps, capacity, index, 0, queue, queue_capacity));
    assert(e.session == 8 && e.ap_capacity == capacity && e.aps == aps);
    wd_engine_t unavailable = {0};
    assert(!wd_session_begin(&unavailable, 1));
    assert(wd_detect(&unavailable, &extra, gps, 0) == WD_NOT_INITIALIZED);
    assert(wd_pending(&unavailable) == NULL && !wd_ack(&unavailable, 1, 1));
    printf("10000 AP inserted and revisited; 2048-record disconnect queue; %zu bounded storage "
           "bytes\n",
           bytes);
    free(aps);
    free(index);
    free(queue);
}
