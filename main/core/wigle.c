#include "wigle.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void wigle_mac(const uint8_t *b, char *out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
}

/* Return the length of a canonical UTF-8 sequence, excluding surrogates and
 * code points above U+10FFFF. Invalid bytes remain visible as hexadecimal. */
static size_t utf8_length(const uint8_t *s, size_t remaining) {
    uint8_t c = s[0];
    size_t length = c >= 0xc2 && c <= 0xdf   ? 2
                    : c >= 0xe0 && c <= 0xef ? 3
                    : c >= 0xf0 && c <= 0xf4 ? 4
                                             : 0;
    if (!length || remaining < length)
        return 0;
    for (size_t i = 1; i < length; ++i)
        if (s[i] < 0x80 || s[i] > 0xbf)
            return 0;
    if ((c == 0xe0 && s[1] < 0xa0) || (c == 0xed && s[1] >= 0xa0) || (c == 0xf0 && s[1] < 0x90) ||
        (c == 0xf4 && s[1] >= 0x90))
        return 0;
    return length;
}

size_t wigle_row(const ap_observation_t *ap, const gps_fix_t *fix, char *out, size_t capacity) {
    if (!ap->auth || strstr(ap->auth, "UNKNOWN") || !fix->valid || !fix->utc[0] ||
        ap->ssid_length > 32 || !isfinite(fix->latitude) || !isfinite(fix->longitude) ||
        fabs(fix->latitude) > 90 || fabs(fix->longitude) > 180)
        return 0;
    char ssid[32 * 4 + 1];
    size_t used = 0;
    bool hidden = true;
    for (size_t i = 0; i < ap->ssid_length; ++i)
        if (ap->ssid[i])
            hidden = false;
    for (size_t i = 0; !hidden && i < ap->ssid_length; ++i) {
        uint8_t c = ap->ssid[i];
        size_t length = utf8_length(ap->ssid + i, ap->ssid_length - i);
        if (c == '"') {
            ssid[used++] = '"';
            ssid[used++] = '"';
        } else if (c >= 32 && c < 127 && c != '\\')
            ssid[used++] = (char)c;
        else if (length) {
            memcpy(ssid + used, ap->ssid + i, length);
            used += length;
            i += length - 1;
        } else {
            snprintf(ssid + used, sizeof(ssid) - used, "\\x%02X", c);
            used += 4;
        }
    }
    ssid[used] = '\0';
    char mac[18];
    wigle_mac(ap->bssid, mac);
    char accuracy[32] = "", altitude[32] = "";
    if (isfinite(fix->horizontal_accuracy_m) && fix->horizontal_accuracy_m > 0)
        snprintf(accuracy, sizeof(accuracy), "%.1f", fix->horizontal_accuracy_m);
    if (isfinite(fix->altitude_m))
        snprintf(altitude, sizeof(altitude), "%.1f", fix->altitude_m);
    int written = snprintf(out, capacity, "%s,\"%s\",%s,%s,%u,%d,%.7f,%.7f,%s,%s,WIFI\n", mac, ssid,
                           ap->auth, fix->utc, ap->channel, ap->rssi, fix->latitude, fix->longitude,
                           altitude, accuracy);
    return written > 0 && (size_t)written < capacity ? (size_t)written : 0;
}
