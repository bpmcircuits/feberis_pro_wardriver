#include "radio.h"
#include <string.h>

bool wd_beacon_parse(const uint8_t *p, size_t n, uint8_t channel, int8_t rssi, int64_t received_ms,
                     wd_beacon_t *out) {
    if (!p || !out || n < 38 || n > 4095 || (p[0] != 0x80 && p[0] != 0x50) || (p[1] & 0xc7) ||
        (p[22] & 15) || channel == 0 || rssi > 0 || rssi < -127 || (p[16] & 1))
        return false;
    const uint8_t zero[6] = {0};
    if (!memcmp(p + 16, zero, 6))
        return false;
    wd_beacon_t b = {.channel = channel, .rssi = rssi, .received_ms = received_ms};
    memcpy(b.bssid, p + 16, 6);
    bool found = false;
    for (size_t offset = 36; offset < n;) {
        if (n - offset < 2)
            return false;
        unsigned id = p[offset++], length = p[offset++];
        if (length > n - offset)
            return false;
        if (id == 0) {
            if (found || length > 32)
                return false;
            b.ssid_len = (uint8_t)length;
            memcpy(b.ssid, p + offset, length);
            found = true;
        }
        offset += length;
    }
    if (!found)
        return false;
    *out = b;
    return true;
}
static size_t bucket_for(const wd_radio_cache_t *c, const uint8_t bssid[6], uint8_t channel) {
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < 6; i++)
        h = (h ^ bssid[i]) * 16777619u;
    return ((h ^ channel) * 16777619u) % c->capacity;
}
bool wd_radio_cache_init(wd_radio_cache_t *c, wd_radio_entry_t *entries, size_t capacity) {
    if (!c || !entries || capacity < 2 || capacity > SIZE_MAX / sizeof(*entries))
        return false;
    memset(entries, 0, capacity * sizeof(*entries));
    *c = (wd_radio_cache_t){.entries = entries, .capacity = capacity};
    return true;
}
uint32_t wd_radio_cache_begin(wd_radio_cache_t *c, int64_t started_ms) {
    if (!c->entries || c->capacity < 2)
        return 0;
    if (++c->generation == 0) {
        memset(c->entries, 0, c->capacity * sizeof(*c->entries));
        c->generation = 1;
    }
    c->started_ms = started_ms;
    c->count = 0;
    return c->generation;
}
bool wd_radio_cache_store(wd_radio_cache_t *c, const wd_beacon_t *b, gps_fix_t fix) {
    if (!c->generation || b->ssid_len > 32 || b->generation != c->generation ||
        b->received_ms < c->started_ms)
        return false;
    size_t bucket = bucket_for(c, b->bssid, b->channel);
    for (size_t i = 0; i < c->capacity; i++) {
        wd_radio_entry_t *e = &c->entries[bucket];
        if (e->generation != c->generation) {
            if (c->count >= c->capacity / 2) {
                ++c->overflows;
                return false;
            }
            *e = (wd_radio_entry_t){.generation = c->generation, .beacon = *b, .fix = fix};
            ++c->count;
            return true;
        }
        if (e->beacon.channel == b->channel && !memcmp(e->beacon.bssid, b->bssid, 6)) {
            uint8_t ssid[32];
            size_t length = e->beacon.ssid_len;
            memcpy(ssid, e->beacon.ssid, length);
            if (b->rssi > e->beacon.rssi ||
                (b->rssi == e->beacon.rssi && (!e->fix.valid || fix.valid))) {
                e->beacon = *b;
                e->fix = fix;
            }
            /* Preserve a passively learned nonempty SSID within this scan. */
            if (b->ssid_len) {
                e->beacon.ssid_len = b->ssid_len;
                memcpy(e->beacon.ssid, b->ssid, b->ssid_len);
            } else if (length) {
                e->beacon.ssid_len = (uint8_t)length;
                memcpy(e->beacon.ssid, ssid, length);
            }
            return true;
        }
        if (++bucket == c->capacity)
            bucket = 0;
    }
    ++c->overflows;
    return false;
}
const wd_radio_entry_t *wd_radio_cache_find(const wd_radio_cache_t *c, const uint8_t bssid[6],
                                            uint8_t channel, int64_t completed_ms) {
    if (!c->generation)
        return NULL;
    size_t bucket = bucket_for(c, bssid, channel);
    for (size_t i = 0; i < c->capacity; i++) {
        const wd_radio_entry_t *e = &c->entries[bucket];
        if (e->generation != c->generation)
            return NULL;
        if (e->beacon.channel == channel && !memcmp(e->beacon.bssid, bssid, 6))
            return e->beacon.received_ms <= completed_ms ? e : NULL;
        if (++bucket == c->capacity)
            bucket = 0;
    }
    return NULL;
}
