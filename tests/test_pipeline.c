/* Host integration of the actual acquisition, CRC, retry and commit modules.
 * The storage sink models sync success/failure; it is not the Flipper SDK. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "delivery.h"
#include "engine_fixture.h"
#include "wardrive.h"
#include "wire.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
static wd_engine_t engine;
static wd_ap_t aps[TEST_AP_CAPACITY];
static uint32_t index_table[TEST_AP_CAPACITY * 2];
static wd_observation_t observations[TEST_QUEUE_CAPACITY];
static uint32_t persisted;
static unsigned writes;
static bool receive(char *frame, bool storage_full) {
    char *fields[WIRE_FIELDS];
    wire_reader_t reader = {0};
    bool ready = false;
    for (const char *c = frame; *c; c++)
        ready = wire_feed(&reader, *c);
    if (!ready)
        return false;
    size_t count = wire_decode(reader.data, fields, WIRE_FIELDS);
    uint32_t session, seq;
    if (count != 5 || !wire_u32(fields[3], &session) || !wire_u32(fields[4], &seq))
        return false;
    if (seq == persisted)
        return true; /* retransmission after durable sync but lost ACK */
    if (storage_full)
        return false; /* short write or failed sync: no commit */
    assert(session == engine.session);
    ++writes;
    persisted = seq;
    return true;
}
int main(void) {
    assert(wd_engine_init(&engine, aps, TEST_AP_CAPACITY, index_table, TEST_AP_CAPACITY * 2,
                          observations, TEST_QUEUE_CAPACITY));
    wd_session_begin(&engine, 3);
    wd_detection_t d = {.bssid = {1, 2, 3, 4, 5, 6}, .rssi = -60, .channel = 1, .band = 2};
    gps_fix_t gps = {0};
    assert(wd_detect(&engine, &d, gps, 1000) == WD_ACCEPTED);
    const wd_observation_t *o = wd_pending(&engine);
    char payload[128], frame[WIRE_MAX], copy[WIRE_MAX];
    snprintf(payload, sizeof(payload), "FBR1\t0\tOBSERVATION\t%" PRIu32 "\t%" PRIu32, o->session,
             o->sequence);
    size_t n = wire_encode(payload, frame, sizeof(frame));
    assert(n);
    delivery_t link;
    delivery_begin(&link, o->sequence, 0);
    /* Receiver absent; retries never remove the acquired record. */
    assert(delivery_poll(&link, 2000) == DELIVERY_RETRY && engine.queued == 1);
    strcpy(copy, frame);
    copy[6] ^= 1;
    assert(!receive(copy, false) && !writes && engine.queued == 1);
    strcpy(copy, frame);
    assert(!receive(copy, true) && !persisted && engine.queued == 1);
    strcpy(copy, frame);
    assert(receive(copy, false) && writes == 1);
    /* Power loss on the receiver after journal sync but before ACK: same checkpoint. */
    assert(delivery_poll(&link, 4000) == DELIVERY_RETRY);
    strcpy(copy, frame);
    assert(receive(copy, false) && writes == 1);
    assert(delivery_poll(&link, 6000) == DELIVERY_RETRY);
    assert(delivery_poll(&link, 8000) == DELIVERY_TIMEOUT && engine.queued == 1);
    /* Reconnect resumes from durable checkpoint even after bounded retry exhaustion. */
    assert(wd_ack(&engine, 3, persisted) && !engine.queued);
    assert(!wd_ack(&engine, 3, persisted));
    puts("CRC corruption, disconnect, full storage, lost ACK, retry timeout and checkpoint resume "
         "passed");
}
