#include "wigle.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void wigle_mac(const uint8_t *b, char *out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
}

size_t wigle_row(const ap_observation_t *ap, const gps_fix_t *fix, char *out, size_t capacity) {
    if (!ap->auth || strstr(ap->auth, "UNKNOWN") || !fix->valid || !fix->utc[0] ||
        ap->ssid_length > 32 || !isfinite(fix->latitude) || !isfinite(fix->longitude) ||
        fabs(fix->latitude) > 90 || fabs(fix->longitude) > 180)
        return 0;
    char ssid[32 * 4 + 1];
    size_t used = 0;
    for (size_t i = 0; i < ap->ssid_length; ++i) {
        uint8_t c = ap->ssid[i];
        if (c == '"') {
            ssid[used++] = '"';
            ssid[used++] = '"';
        } else if (c >= 32 && c < 127 && c != '\\')
            ssid[used++] = (char)c;
        else {
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
